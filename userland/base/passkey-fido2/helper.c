/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The device helper (fido2.h; ws172-p003; docs/architecture/security.md,
 * "The parts", "The security key").  A child of passkey-fido2: it gives up
 * root for _passkey inside the empty root /var/empty, may start no
 * process, and holds only the keys' descriptors (opened and claimed by
 * passkey-fido2) and its pipe.  It never reads /etc/passkey, does not make
 * the challenge and does not judge the answer.
 *
 * To log in, it first asks every key, without the user and without the
 * PIN, whether it holds one of the account's credentials; the PIN goes to
 * that key only, then the assertion is asked with the user's touch.  To
 * register, exactly one key must be there; it makes a new credential with
 * the PIN, and the authenticator data goes back as the key gave it.
 *
 * A key may also be held to an NFC reader (ws199-p001 section 5, BUG-286):
 * passkey-fido2 attached the readers' slots, and the helper powers a card
 * and selects its FIDO applet (pk_os_card_select), a slot whose card does
 * not answer (a reader's SAM slot, a card that is not a key) being let go.
 * A card on the reader is the user's presence for the key (the user's
 * decision of 2026-10-10: a card left on the reader counts as a touch), so
 * to log in the cards already there are asked after the USB keys; when no
 * key answers at all, the helper says "touch" (touch the key, or hold it
 * to the reader) and waits for a card to come until the touch's time is
 * nearly out.  To register, a card already there counts as the one key,
 * and with no key at all the helper waits for one the same way.
 */

#include "fido2.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

/* How long the channel's opening (or a card's applet's selection) may take on one key. */
#define HELPER_OPEN_MS		2000U

/* How much of the touch's time is kept, after the wait for a card held to the reader, for the PIN and the answer. */
#define HELPER_TAP_SPARE_MS	3000U

/* A key the helper talks to: its channel or applet, transport, device, what it is, and its slot (-1: a USB key). */
struct helper_key {
	struct pk_hid hid;
	struct pk_nfc nfc;
	struct pk_transport transport;
	struct pk_device device;
	struct pk_info info;
	int card;
	int cancelled;
};

/* The helper's pipe to passkey-fido2, whether the touch was asked for (told once), when the helper started, and whether sessiond ended the work (SIGTERM). */
static int helper_pipe = -1;
static int helper_touch_told;
static uint64_t helper_started_ms;
static volatile sig_atomic_t helper_ended;

/* The longest the reset's look for the credentials the key holds may take (the key takes the reset only a few seconds after it is powered). */
#define HELPER_RESET_LOOK_MS	1500U

static int helper_sandbox(uid_t uid, gid_t gid);
static int helper_open(struct fido2_devices *devices, size_t index, struct helper_key *key);
static int helper_open_card(struct fido2_devices *devices, size_t index, struct helper_key *key);
static int helper_ready(struct helper_key *key);
static void helper_release(struct fido2_devices *devices, struct helper_key *key);
static int helper_wait_card(struct fido2_devices *devices, size_t *index);
static void helper_touch(void);
static int helper_one(struct fido2_devices *devices, struct helper_key *key, int wait);
static void helper_info(struct fido2_devices *devices);
static void helper_pin(struct fido2_devices *devices, const struct fido2_job *job);
static void helper_reset(struct fido2_devices *devices, const struct fido2_job *job);
static void helper_owner(struct fido2_devices *devices, const struct fido2_job *job);
static int helper_owner_group(struct helper_key *key, const struct fido2_job *job, unsigned group, struct pk_assertion_reply *reply);
static void helper_terminated(int signal_number);
static uint64_t helper_now_ms(void);
static void helper_assert(struct fido2_devices *devices, const struct fido2_job *job);
static void helper_make(struct fido2_devices *devices, const struct fido2_job *job);
static int helper_token(struct helper_key *key, const struct fido2_job *job, unsigned permissions, uint8_t *token, size_t *token_size);
static const char *helper_reason(const struct helper_key *key, int error);
static void helper_keepalive(void *context, uint8_t status);
static void helper_send(const char *line);
static void helper_fail(const char *reason);

/*
 * Runs the helper's job on the keys and writes its answer on pipe_out;
 * it never returns.
 */
void
fido2_helper(
	struct fido2_devices *devices,
	const struct fido2_job *job,
	int pipe_out,
	uid_t uid,
	gid_t gid)
{
	struct sigaction ending;
	int error;

	/* The pipe, the time, and the sandbox. */
	helper_pipe = pipe_out;
	helper_started_ms = helper_now_ms();
	error = helper_sandbox(uid, gid);
	if (error != 0) {
		helper_fail("internal");
		_exit(2);
	}

	/* sessiond's end of the work: the key's command under way is cancelled, and the job ends with the key's answer (ws199-p001). */
	memset(&ending, 0, sizeof(ending));
	ending.sa_handler = helper_terminated;
	sigemptyset(&ending.sa_mask);
	(void)sigaction(SIGTERM, &ending, NULL);

	/* The job. */
	switch (job->kind) {
	case FIDO2_JOB_ASSERT:
		helper_assert(devices, job);
		break;
	case FIDO2_JOB_MAKE:
		helper_make(devices, job);
		break;
	case FIDO2_JOB_INFO:
		helper_info(devices);
		break;
	case FIDO2_JOB_SET_PIN:
	case FIDO2_JOB_CHANGE_PIN:
		helper_pin(devices, job);
		break;
	case FIDO2_JOB_OWNER:
		helper_owner(devices, job);
		break;
	default:
		helper_reset(devices, job);
		break;
	}

	/* Done: the answer was sent. */
	_exit(0);
}

/*
 * Gives up everything but the keys and the pipe: the standard descriptors
 * on /dev/null, no core file, an alarm, the empty root (no program to
 * start in it), and _passkey's group and user.  Returns 0 or an errno value.
 */
static int
helper_sandbox(
	uid_t uid,
	gid_t gid)
{
	struct rlimit none;
	int descriptor;
	int result;

	/* Nothing read from or written to passkey's own descriptors. */
	descriptor = open("/dev/null", O_RDWR);
	if (descriptor < 0)
		return errno;
	(void)dup2(descriptor, 0);
	(void)dup2(descriptor, 1);
	(void)dup2(descriptor, 2);
	if (descriptor > 2)
		(void)close(descriptor);

	/* No core file, and an end of its own (the empty root has no program to start). */
	none.rlim_cur = 0;
	none.rlim_max = 0;
	(void)setrlimit(RLIMIT_CORE, &none);
	(void)alarm(FIDO2_HELPER_SECONDS);

	/* The empty root. */
	result = chroot("/var/empty");
	if (result != 0)
		return errno;
	result = chdir("/");
	if (result != 0)
		return errno;

	/* _passkey's group alone, then its user (root cannot come back). */
	result = setgroups(1U, &gid);
	if (result != 0)
		return errno;
	result = setgid(gid);
	if (result != 0)
		return errno;
	result = setuid(uid);
	if (result != 0)
		return errno;

	/* Succeeded: the helper holds nothing else. */
	return 0;
}

/*
 * Opens a CTAP2 channel on a key and asks what it is.  Returns 0 or an
 * errno value.
 */
static int
helper_open(
	struct fido2_devices *devices,
	size_t index,
	struct helper_key *key)
{
	struct pk_hid_io io;
	int error;

	/* The node's reports, the channel, CTAP2 over it. */
	memset(key, 0, sizeof(*key));
	key->card = -1;
	pk_os_posix_io(&devices->handles[index], &io);
	error = pk_hid_open(&key->hid, &io, HELPER_OPEN_MS);
	if (error != 0)
		return error;
	(void)pk_hid_transport(&key->transport, &key->hid);

	/* Succeeded when the key says what it is. */
	return helper_ready(key);
}

/*
 * Opens a card held to a reader's slot: powered, its FIDO applet
 * selected, and asked what it is (a card that does not answer is powered
 * off again).  Returns 0 or an errno value.
 */
static int
helper_open_card(
	struct fido2_devices *devices,
	size_t index,
	struct helper_key *key)
{
	int error;

	/* The card and its applet, CTAP2 over it. */
	memset(key, 0, sizeof(*key));
	key->card = -1;
	error = pk_os_card_select(&devices->cards[index], &key->nfc, &key->transport, HELPER_OPEN_MS);
	if (error != 0)
		return error;
	key->card = (int)index;

	/* What the key is; a key that does not say lets the slot go. */
	error = helper_ready(key);
	if (error != 0)
		helper_release(devices, key);
	return error;
}

/* Starts CTAP2 on an open key's transport and asks what the key is.  Returns 0 or an errno value. */
static int
helper_ready(
	struct helper_key *key)
{
	int error;

	/* The device, the touch's wait told. */
	pk_device_init(&key->device, &key->transport, FIDO2_TOUCH_MS);
	key->device.keepalive = helper_keepalive;
	key->device.keepalive_context = key;

	/* What the key is. */
	error = pk_ctap2_get_info(&key->device, &key->info);
	if (error != 0)
		return error;

	/* Succeeded: the key answers. */
	return 0;
}

/* Lets a card held to a reader go (powered off, the slot kept); a USB key stays (its node is passkey-fido2's). */
static void
helper_release(
	struct fido2_devices *devices,
	struct helper_key *key)
{
	/* Only a card. */
	if (key->card < 0)
		return;

	/* Off. */
	pk_os_card_power_off(&devices->cards[key->card]);
	key->card = -1;
}

/*
 * Waits, until the touch's time is nearly out, for a card to come to one
 * of the slots.  Returns 0 with its slot in *index, ETIMEDOUT, ENODEV when
 * no slot is left, or another errno value.
 */
static int
helper_wait_card(
	struct fido2_devices *devices,
	size_t *index)
{
	struct pollfd waits[PK_OS_DEVICES_MAX];
	size_t slots[PK_OS_DEVICES_MAX];
	uint64_t deadline;
	uint64_t now;
	size_t count;
	size_t slot;
	size_t at;
	int inserted;
	int ready;
	int error;

	/* The time left for it. */
	deadline = helper_started_ms + FIDO2_TOUCH_MS - HELPER_TAP_SPARE_MS;
	for (;;) {
		/* The slots still there. */
		count = 0U;
		for (slot = 0U; slot < devices->card_count; slot++) {
			if (devices->cards[slot].descriptor < 0)
				continue;
			waits[count].fd = devices->cards[slot].descriptor;
			waits[count].events = POLLIN;
			waits[count].revents = 0;
			slots[count] = slot;
			count++;
		}

		/* None left to wait on. */
		if (count == 0U)
			return ENODEV;

		/* Their events, until the deadline. */
		now = helper_now_ms();
		if (now >= deadline)
			return ETIMEDOUT;
		ready = poll(waits, count, (int)(deadline - now));
		if (helper_ended)
			return ECANCELED;
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			return errno;
		if (ready == 0)
			return ETIMEDOUT;

		/* Each slot's events: a card that came is the one; a reader gone is waited on no more. */
		for (at = 0U; at < count; at++) {
			slot = slots[at];
			for (;;) {
				error = pk_os_card_event(&devices->cards[slot], &inserted);
				if (error != 0)
					break;
				if (inserted) {
					*index = slot;
					return 0;
				}
			}

			/* A reader that went. */
			if (error != EAGAIN || (waits[at].revents & POLLHUP) != 0)
				devices->cards[slot].descriptor = -1;
		}
	}
}

/* Tells passkey-fido2, once, that the user is to touch the key (or hold it to the reader). */
static void
helper_touch(void)
{
	/* Once. */
	if (helper_touch_told)
		return;
	helper_touch_told = 1;
	helper_send("touch");
}

/* Gives the monotonic time in milliseconds. */
static uint64_t
helper_now_ms(void)
{
	struct timespec now;

	/* The clock that does not go back. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Logs in: the key that holds a credential, its PIN, the assertion with the touch. */
static void
helper_assert(
	struct fido2_devices *devices,
	const struct fido2_job *job)
{
	static struct helper_key key;
	struct pk_assertion_request request;
	struct pk_assertion_reply reply;
	char line[FIDO2_MESSAGE_MAX];
	char id[2U * PK_CREDENTIAL_ID_MAX + 1U];
	char auth_data[2U * PK_AUTH_DATA_MAX + 1U];
	char signature[2U * PK_SIGNATURE_MAX + 1U];
	uint8_t token[PK_PIN_TOKEN_MAX];
	size_t token_size;
	size_t index;
	size_t slot;
	int answered;
	int present;
	int tapped;
	int found;
	int error;

	/* The silent question: which key holds one of the credentials (without the user, without the PIN). */
	memset(&request, 0, sizeof(request));
	request.rp_id = FIDO2_RP;
	memcpy(request.client_data_hash, job->client_data_hash, sizeof(request.client_data_hash));
	request.allow_ids = job->ids;
	request.allow_sizes = job->id_sizes;
	request.allow_count = job->id_count;
	found = 0;
	answered = 0;
	for (index = 0U; index < devices->count && !found; index++) {
		error = helper_open(devices, index, &key);
		if (error != 0)
			continue;
		answered = 1;
		error = pk_ctap2_get_assertion(&key.device, &request, &reply);
		if (error == 0)
			found = 1;
	}

	/* The cards on a reader now, asked the same; one that holds none is let go. */
	for (slot = 0U; slot < devices->card_count && !found; slot++) {
		error = pk_os_card_present(&devices->cards[slot], &present);
		if (error != 0 || !present)
			continue;
		error = helper_open_card(devices, slot, &key);
		if (error != 0)
			continue;
		answered = 1;
		error = pk_ctap2_get_assertion(&key.device, &request, &reply);
		if (error == 0) {
			found = 1;
			break;
		}

		/* Not this account's key: let go. */
		helper_release(devices, &key);
	}

	/* No key answered at all: one held to a reader during the attempt, each that comes asked the same. */
	tapped = 0;
	if (!found && !answered && devices->card_count != 0U) {
		helper_touch();
		for (;;) {
			error = helper_wait_card(devices, &slot);
			if (error != 0)
				break;
			error = helper_open_card(devices, slot, &key);
			if (error != 0)
				continue;
			tapped = 1;
			error = pk_ctap2_get_assertion(&key.device, &request, &reply);
			if (error == 0) {
				found = 1;
				break;
			}

			/* Not this account's key: let go. */
			helper_release(devices, &key);
		}
	}

	/* None holds one: no key held to the reader in time is a timeout. */
	if (!found) {
		if (!answered && !tapped && devices->card_count != 0U) {
			helper_fail("timeout");
			return;
		}

		/* A key that holds none. */
		helper_fail("no-key");
		return;
	}

	/* The PIN to that key alone, when one is asked (ws199-p001: the account's key may sign in without it). */
	token_size = 0U;
	if (job->pin[0] != '\0') {
		error = helper_token(&key, job, PK_PERMISSION_GET_ASSERTION, token, &token_size);
		if (error != 0)
			return;
		request.pin_token = token;
		request.pin_token_size = token_size;
		request.pin_protocol = pk_ctap2_choose_protocol(&key.info);
	}

	/* The assertion, with the user's touch unless the unlock asks none, and the PIN's verification when it was given. */
	request.presence = job->presence;
	error = pk_ctap2_get_assertion(&key.device, &request, &reply);
	pk_crypto_wipe(token, sizeof(token));
	if (error != 0) {
		helper_fail(helper_reason(&key, error));
		return;
	}

	/* The answer's bytes. */
	(void)fido2_hex_encode(reply.credential_id, reply.credential_id_size, id, sizeof(id));
	(void)fido2_hex_encode(reply.auth_data, reply.auth_data_size, auth_data, sizeof(auth_data));
	(void)fido2_hex_encode(reply.signature, reply.signature_size, signature, sizeof(signature));
	(void)snprintf(line, sizeof(line), "assertion %s %s %s", id, auth_data, signature);
	helper_send(line);
}

/* Registers: the one key there, its PIN, a new credential with the touch. */
static void
helper_make(
	struct fido2_devices *devices,
	const struct fido2_job *job)
{
	static struct helper_key key;
	static struct pk_made_credential made;
	struct pk_make_request request;
	char line[FIDO2_MESSAGE_MAX];
	char auth_data[2U * PK_AUTH_DATA_MAX + 1U];
	uint8_t token[PK_PIN_TOKEN_MAX];
	size_t token_size;
	int error;

	/* Exactly one key (a rogue second one could slip its own credential in), held to a reader in time when none is there. */
	error = helper_one(devices, &key, 1);
	if (error != 0)
		return;

	/* The PIN. */
	error = helper_token(&key, job, PK_PERMISSION_MAKE_CREDENTIAL, token, &token_size);
	if (error != 0)
		return;

	/* The credential, with the user's touch, not one of the account's again. */
	memset(&request, 0, sizeof(request));
	request.rp_id = FIDO2_RP;
	request.user_id = job->user_id;
	request.user_id_size = sizeof(job->user_id);
	request.user_name = job->user_name;
	memcpy(request.client_data_hash, job->client_data_hash, sizeof(request.client_data_hash));
	request.pin_token = token;
	request.pin_token_size = token_size;
	request.pin_protocol = pk_ctap2_choose_protocol(&key.info);
	request.exclude_ids = job->ids;
	request.exclude_sizes = job->id_sizes;
	request.exclude_count = job->id_count;
	error = pk_ctap2_make_credential(&key.device, &request, &made);
	pk_crypto_wipe(token, sizeof(token));
	if (error != 0) {
		helper_fail(helper_reason(&key, error));
		return;
	}

	/* The authenticator data as the key gave it (passkey-fido2 reads it again). */
	(void)fido2_hex_encode(made.auth_data, made.auth_data_size, auth_data, sizeof(auth_data));
	(void)snprintf(line, sizeof(line), "made %s", auth_data);
	helper_send(line);
}

/*
 * Gets a PIN token of the permissions from the key, which must have a PIN
 * of its own.  Returns 0, or an errno value after the failure was sent.
 */
static int
helper_token(
	struct helper_key *key,
	const struct fido2_job *job,
	unsigned permissions,
	uint8_t *token,
	size_t *token_size)
{
	unsigned protocol;
	int error;

	/* A key without a PIN (or without a PIN protocol) is not used. */
	protocol = pk_ctap2_choose_protocol(&key->info);
	if ((key->info.options & PK_OPTION_CLIENT_PIN_SET) == 0U) {
		helper_fail("no-pin");
		return EPERM;
	}

	/* A key without a PIN protocol is not used. */
	if (protocol == 0U) {
		helper_fail("device");
		return EPERM;
	}

	/* The token. */
	*token_size = PK_PIN_TOKEN_MAX;
	error = pk_ctap2_pin_token(&key->device, &key->info, protocol, job->pin, permissions, FIDO2_RP, token, token_size);
	if (error != 0) {
		helper_fail(helper_reason(key, error));
		return error;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Takes the one key there: a USB key, or a card on a reader that answers
 * as a key; with none and wait, one held to a reader in the touch's time.
 * Returns 0 with it open, or an errno value after the failure was sent
 * (many-keys, no-key, timeout, canceled, device).
 */
static int
helper_one(
	struct fido2_devices *devices,
	struct helper_key *key,
	int wait)
{
	static struct helper_key other;
	size_t cards;
	size_t slot;
	int present;
	int error;

	/* No card open yet; two USB keys or more are refused. */
	key->card = -1;
	other.card = -1;
	if (devices->count > 1U) {
		helper_fail("many-keys");
		return EEXIST;
	}

	/* The cards on a reader now that answer as keys: the first kept, any other one more key. */
	cards = 0U;
	for (slot = 0U; slot < devices->card_count; slot++) {
		error = pk_os_card_present(&devices->cards[slot], &present);
		if (error != 0 || !present)
			continue;
		if (cards == 0U) {
			error = helper_open_card(devices, slot, key);
		} else {
			error = helper_open_card(devices, slot, &other);
		}

		/* A card that is not a key does not count; one more than the first is let go. */
		if (error != 0)
			continue;
		cards++;
		if (cards > 1U)
			helper_release(devices, &other);
	}

	/* Exactly one key: two or more are refused. */
	if (devices->count + cards > 1U) {
		helper_release(devices, key);
		helper_fail("many-keys");
		return EEXIST;
	}

	/* The USB key, when it is the one: it answers. */
	if (devices->count == 1U) {
		error = helper_open(devices, 0U, key);
		if (error != 0) {
			helper_fail("device");
			return error;
		}

		/* Open. */
		return 0;
	}

	/* A card already there. */
	if (cards == 1U)
		return 0;

	/* No key, and none to wait for. */
	if (!wait || devices->card_count == 0U) {
		helper_fail("no-key");
		return ENODEV;
	}

	/* Held to a reader, in the touch's time. */
	helper_touch();
	for (;;) {
		error = helper_wait_card(devices, &slot);
		if (error != 0)
			break;
		error = helper_open_card(devices, slot, key);
		if (error == 0)
			return 0;
	}

	/* None in time, or the work ended. */
	if (error == ECANCELED) {
		helper_fail("canceled");
		return error;
	}

	/* Out of time. */
	helper_fail("timeout");
	return error;
}

/*
 * Tells what the keys there are (Settings' wizards, ws199-p001 section
 * 4.1): how many, and for one key whether it has a PIN, its retries and
 * its PIN's fewest characters; the PIN is never sent.
 */
static void
helper_info(
	struct fido2_devices *devices)
{
	static struct helper_key key;
	static struct helper_key other;
	char line[96];
	unsigned count;
	unsigned retries;
	unsigned protocol;
	unsigned minimum;
	unsigned pin;
	size_t index;
	size_t slot;
	int present;
	int chosen;
	int card;
	int error;

	/* The USB keys that answer, the first kept. */
	count = 0U;
	chosen = -1;
	card = 0;
	key.card = -1;
	other.card = -1;
	for (index = 0U; index < devices->count; index++) {
		if (count == 0U) {
			error = helper_open(devices, index, &key);
		} else {
			error = helper_open(devices, index, &other);
		}

		/* One that does not answer is not counted. */
		if (error != 0)
			continue;
		if (count == 0U)
			chosen = (int)index;
		count++;
	}

	/* The cards on a reader that answer as keys (none is waited for). */
	for (slot = 0U; slot < devices->card_count; slot++) {
		error = pk_os_card_present(&devices->cards[slot], &present);
		if (error != 0 || !present)
			continue;
		if (count == 0U) {
			error = helper_open_card(devices, slot, &key);
		} else {
			error = helper_open_card(devices, slot, &other);
		}

		/* A card that is not a key is not counted. */
		if (error != 0)
			continue;
		if (count == 0U) {
			chosen = (int)slot;
			card = 1;
		} else {
			helper_release(devices, &other);
		}

		/* Counted. */
		count++;
	}

	/* Not one key: how many, and nothing more. */
	if (count != 1U) {
		helper_release(devices, &key);
		(void)snprintf(line, sizeof(line), "info %u,0,0,0,0,0", count);
		helper_send(line);
		return;
	}

	/* The one key: its PIN, its retries (without the PIN), its PIN's fewest characters. */
	pin = (key.info.options & PK_OPTION_CLIENT_PIN_SET) != 0U;
	retries = 0U;
	protocol = pk_ctap2_choose_protocol(&key.info);
	if (pin && protocol != 0U)
		(void)pk_ctap2_pin_retries(&key.device, protocol, &retries);
	minimum = key.info.min_pin_length;
	if (minimum == 0U)
		minimum = 4U;

	/* Told. */
	(void)snprintf(line, sizeof(line), "info 1,%d,%d,%u,%u,%u", card, chosen, pin, retries, minimum);
	helper_send(line);
}

/* Sets the one key's first PIN, or changes its PIN (the current one, the new one); the key checks both. */
static void
helper_pin(
	struct fido2_devices *devices,
	const struct fido2_job *job)
{
	static struct helper_key key;
	unsigned protocol;
	int has;
	int error;

	/* The one key there (none is waited for). */
	error = helper_one(devices, &key, 0);
	if (error != 0)
		return;

	/* Its PIN protocol, and whether it has a PIN: a set needs none, a change needs one. */
	protocol = pk_ctap2_choose_protocol(&key.info);
	has = (key.info.options & PK_OPTION_CLIENT_PIN_SET) != 0U;
	if (protocol == 0U) {
		helper_fail("device");
		return;
	}

	/* A first PIN only for a key without one. */
	if (job->kind == FIDO2_JOB_SET_PIN && has) {
		helper_fail("pin-set");
		return;
	}

	/* A change only for a key with one. */
	if (job->kind == FIDO2_JOB_CHANGE_PIN && !has) {
		helper_fail("no-pin");
		return;
	}

	/* The PIN. */
	if (job->kind == FIDO2_JOB_SET_PIN) {
		error = pk_ctap2_set_pin(&key.device, protocol, job->new_pin);
	} else {
		error = pk_ctap2_change_pin(&key.device, protocol, job->pin, job->new_pin);
	}

	/* The key refused it. */
	if (error != 0) {
		helper_fail(helper_reason(&key, error));
		return;
	}

	/* Done. */
	helper_send("done");
}

/*
 * Resets the one key (ws199-p001 section 4.5): passkey-fido2 saw it come
 * back a moment ago, so the key still takes the reset.  First, within a
 * short time, which of the job's credentials the key holds (each asked
 * alone, without the user); then authenticatorReset with the touch; the
 * credentials held go back so passkey-fido2 removes their lines.
 */
static void
helper_reset(
	struct fido2_devices *devices,
	const struct fido2_job *job)
{
	static struct helper_key key;
	struct pk_assertion_request request;
	struct pk_assertion_reply reply;
	char line[32];
	uint64_t until;
	uint64_t now;
	unsigned held;
	size_t index;
	int error;

	/* The one key (it came back just now; none is waited for). */
	error = helper_one(devices, &key, 0);
	if (error != 0)
		return;

	/* The credentials it holds, each asked alone and silently, while there is time. */
	held = 0U;
	until = helper_now_ms() + HELPER_RESET_LOOK_MS;
	memset(&request, 0, sizeof(request));
	request.rp_id = FIDO2_RP;
	memcpy(request.client_data_hash, job->client_data_hash, sizeof(request.client_data_hash));
	request.allow_count = 1U;
	for (index = 0U; index < job->id_count && index < 32U; index++) {
		now = helper_now_ms();
		if (now >= until || helper_ended)
			break;
		request.allow_ids = &job->ids[index];
		request.allow_sizes = &job->id_sizes[index];
		error = pk_ctap2_get_assertion(&key.device, &request, &reply);
		if (error == 0)
			held |= 1U << index;
	}

	/* The work ended before the reset: nothing is reset. */
	if (helper_ended) {
		helper_fail("canceled");
		return;
	}

	/* The reset, with the user's touch. */
	error = pk_ctap2_reset(&key.device);
	if (error != 0) {
		helper_fail(helper_reason(&key, error));
		return;
	}

	/* Done: what it held. */
	(void)snprintf(line, sizeof(line), "reset %x", held);
	helper_send(line);
}

/*
 * Tells whose the one key there is (ws199-p001 section 4.2): which of the
 * job's groups (the accounts, in order) it holds a credential of, each
 * asked silently (without the user, without the PIN), the allow list cut
 * to what the key takes at once; and the first group's answer, which
 * passkey-fido2 checks.  It stops at the second group held.  No key is
 * waited for: a card counts only while it lies on a reader.
 */
static void
helper_owner(
	struct fido2_devices *devices,
	const struct fido2_job *job)
{
	static struct helper_key key;
	static struct helper_key other;
	static struct pk_assertion_reply first;
	struct pk_assertion_reply reply;
	char line[FIDO2_MESSAGE_MAX];
	char id[2U * PK_CREDENTIAL_ID_MAX + 1U];
	char auth_data[2U * PK_AUTH_DATA_MAX + 1U];
	char signature[2U * PK_SIGNATURE_MAX + 1U];
	unsigned groups;
	unsigned group;
	unsigned held;
	unsigned count;
	size_t index;
	size_t slot;
	int present;
	int found;
	int card;
	int error;

	/* The USB keys that answer, the first kept. */
	count = 0U;
	key.card = -1;
	other.card = -1;
	for (index = 0U; index < devices->count; index++) {
		if (count == 0U) {
			error = helper_open(devices, index, &key);
		} else {
			error = helper_open(devices, index, &other);
		}

		/* One that does not answer is not counted. */
		if (error == 0)
			count++;
	}

	/* The cards lying on a reader that answer as keys, the first kept when no USB key was. */
	for (slot = 0U; slot < devices->card_count; slot++) {
		error = pk_os_card_present(&devices->cards[slot], &present);
		if (error != 0 || !present)
			continue;
		if (count == 0U) {
			error = helper_open_card(devices, slot, &key);
		} else {
			error = helper_open_card(devices, slot, &other);
		}

		/* A card that is not a key is not counted; one more than the first is let go. */
		if (error != 0)
			continue;
		if (count != 0U)
			helper_release(devices, &other);
		count++;
	}

	/* No key, or more than one: nobody's. */
	if (count == 0U) {
		helper_fail("no-key");
		return;
	}

	/* Two keys or more: not one owner's to tell. */
	if (count > 1U) {
		helper_release(devices, &key);
		helper_fail("many-keys");
		return;
	}

	/* Whether the one key is a card on a reader. */
	card = 0;
	if (key.card >= 0)
		card = 1;

	/* The groups there are (the last one's number and one). */
	groups = 0U;
	for (index = 0U; index < job->id_count; index++) {
		if (job->groups[index] + 1U > groups)
			groups = job->groups[index] + 1U;
	}

	/* Each group, until two are held or the work ends. */
	held = 0U;
	found = 0;
	for (group = 0U; group < groups && group < 32U; group++) {
		if (helper_ended)
			break;
		error = helper_owner_group(&key, job, group, &reply);
		if (error != 0)
			continue;

		/* The first group held keeps its answer; a second ends the look. */
		held |= 1U << group;
		if (!found) {
			first = reply;
			found = 1;
			continue;
		}

		/* The second group held: enough. */
		break;
	}

	/* The work ended: no answer that could be half of it. */
	if (helper_ended) {
		helper_release(devices, &key);
		helper_fail("canceled");
		return;
	}

	/* No group held: the key is not registered here. */
	if (!found) {
		helper_release(devices, &key);
		(void)snprintf(line, sizeof(line), "owner 0,%d", card);
		helper_send(line);
		return;
	}

	/* The groups held, and the first one's answer. */
	(void)fido2_hex_encode(first.credential_id, first.credential_id_size, id, sizeof(id));
	(void)fido2_hex_encode(first.auth_data, first.auth_data_size, auth_data, sizeof(auth_data));
	(void)fido2_hex_encode(first.signature, first.signature_size, signature, sizeof(signature));
	(void)snprintf(line, sizeof(line), "owner %x,%d %s %s %s", held, card, id, auth_data, signature);
	helper_release(devices, &key);
	helper_send(line);
}

/*
 * Asks the key silently whether it holds one of a group's credentials,
 * as many in one question as the key takes (one when it does not say).
 * Returns 0 with its answer, or an errno value.
 */
static int
helper_owner_group(
	struct helper_key *key,
	const struct fido2_job *job,
	unsigned group,
	struct pk_assertion_reply *reply)
{
	struct pk_assertion_request request;
	const uint8_t *ids[FIDO2_IDS_MAX];
	size_t sizes[FIDO2_IDS_MAX];
	size_t most;
	size_t count;
	size_t index;
	int error;

	/* The most credentials the key takes in one allow list. */
	most = key->info.max_credential_count;
	if (most == 0U)
		most = 1U;

	/* The question: the login's relying party and the job's hash, without the user and without the PIN. */
	memset(&request, 0, sizeof(request));
	request.rp_id = FIDO2_RP;
	memcpy(request.client_data_hash, job->client_data_hash, sizeof(request.client_data_hash));
	request.allow_ids = ids;
	request.allow_sizes = sizes;

	/* The group's credentials, asked a list at a time, the last list maybe shorter. */
	count = 0U;
	error = ENOENT;
	for (index = 0U; index <= job->id_count; index++) {
		if (index < job->id_count && job->groups[index] == group) {
			ids[count] = job->ids[index];
			sizes[count] = job->id_sizes[index];
			count++;
		}

		/* A list full, or the last one, is asked. */
		if (count == 0U)
			continue;
		if (count < most && index < job->id_count)
			continue;
		request.allow_count = count;
		error = pk_ctap2_get_assertion(&key->device, &request, reply);
		if (error == 0)
			return 0;
		if (helper_ended)
			return ECANCELED;
		count = 0U;
	}

	/* None of the group's is held. */
	return error;
}

/* Notes that sessiond ended the work (SIGTERM): the key's command is cancelled at its next keepalive. */
static void
helper_terminated(
	int signal_number)
{
	/* Only noted (async-signal-safe). */
	(void)signal_number;
	helper_ended = 1;
}

/* Gives passkey's reason for a key's failure. */
static const char *
helper_reason(
	const struct helper_key *key,
	int error)
{
	uint8_t status;

	/* The work ended by sessiond, a PIN the library refused before the key saw it, and the key's own answers (ws199-p001 section 4.6). */
	if (error == ECANCELED)
		return "canceled";
	if (error == EINVAL)
		return "pin-policy";
	if (error != EPROTO)
		return "device";
	status = key->device.last_status;
	switch (status) {
	case PK_CTAP2_PIN_INVALID:
		return "bad-key-pin";
	case PK_CTAP2_PIN_BLOCKED:
		return "key-locked";
	case PK_CTAP2_PIN_AUTH_BLOCKED:
		return "key-replug";
	case PK_CTAP2_PIN_NOT_SET:
		return "no-pin";
	case PK_CTAP2_PIN_REQUIRED:
		return "pin-required";
	case PK_CTAP2_PIN_POLICY_VIOLATION:
		return "pin-policy";
	case PK_CTAP2_NOT_ALLOWED:
		return "not-allowed";
	case PK_CTAP2_NO_CREDENTIALS:
		return "no-key";
	case PK_CTAP2_KEEPALIVE_CANCEL:
		return "canceled";
	case PK_CTAP2_OPERATION_DENIED:
	case PK_CTAP2_USER_ACTION_TIMEOUT:
		return "timeout";
	case PK_CTAP2_CREDENTIAL_EXCLUDED:
		return "bad-request";
	default:
		break;
	}

	/* Anything else the key said. */
	return "device";
}

/* Tells passkey-fido2, once, that the key waits for the user's touch; cancels the command when sessiond ended the work. */
static void
helper_keepalive(
	void *context,
	uint8_t status)
{
	struct helper_key *key;

	/*
	 * sessiond ended the work: the command under way is cancelled, once
	 * (a USB key answers KEEPALIVE_CANCEL; a card ends when it is taken
	 * away).
	 */
	key = context;
	if (helper_ended && key != NULL && key->card < 0 && !key->cancelled) {
		key->cancelled = 1;
		(void)pk_hid_cancel(&key->hid);
	}

	/* Only the wait for the user, once. */
	if (status != PK_HID_KEEPALIVE_UP_NEEDED)
		return;
	helper_touch();
}

/* Writes one message line on the pipe. */
static void
helper_send(
	const char *line)
{
	size_t length;
	size_t done;
	ssize_t written;

	/* The line and its end, whole. */
	length = strlen(line);
	done = 0U;
	while (done < length) {
		written = write(helper_pipe, line + done, length - done);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return;
		done += (size_t)written;
	}

	/* Its end. */
	(void)write(helper_pipe, "\n", 1U);
}

/* Sends a failure. */
static void
helper_fail(
	const char *reason)
{
	char line[64];

	/* "fail REASON". */
	(void)snprintf(line, sizeof(line), "fail %s", reason);
	helper_send(line);
}
