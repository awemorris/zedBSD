/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * fidoctl: a security key's information, PIN, credentials and assertions
 * (ws161-p004; plan/ws161/phase001 section 9.4), over libpasskey.
 *
 *   fidoctl list
 *   fidoctl [-d NODE] info
 *   fidoctl [-d NODE] set-pin              the new PIN on standard input
 *   fidoctl [-d NODE] change-pin           the current PIN, then the new one
 *   fidoctl [-d NODE] [-p] register RP USER
 *   fidoctl [-d NODE] [-p] [-s] assert RP CREDENTIAL
 *   fidoctl [-s] verify RP CREDENTIAL KEY CLIENT-DATA-HASH AUTH-DATA SIGNATURE
 *
 * Without -d the first key listed is used, taken for this program alone
 * while it runs (zedBSD's grab); with no USB key, the first key held to an
 * NFC reader (ws161-p005: a smart card slot /dev/smartcardN with a card,
 * listed as "card" lines; -d names one too).  -s asks an assertion without the
 * user's presence (up false: no touch, ws199-p001, to see what a key answers),
 * and verify -s takes one without the user present.  -p reads the PIN from
 * standard input and
 * asks the key to verify the user.  A PIN never comes from the command line.
 *
 * register prints the new credential's ID, its COSE public key and its
 * signature count; assert asks for an assertion over a new random client
 * data hash and prints it; verify checks such an assertion with the public
 * key alone (no key is needed).  Bytes are written and read as hexadecimal.
 * Each answer is a line "NAME VALUE"; an error is a line on standard error
 * and the exit status 1.
 */

#include "userland/base/libpasskey/crypto.h"
#include "userland/base/libpasskey/ctap2.h"
#include "userland/base/libpasskey/hid.h"
#include "userland/base/libpasskey/nfc.h"
#include "userland/base/libpasskey/os.h"
#include "userland/base/libpasskey/verify.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* How long a command may wait (a touch included), and how long the channel's opening may take. */
#define FIDOCTL_COMMAND_MS	30000U
#define FIDOCTL_OPEN_MS		3000U

/* The longest PIN line read, the user ID's size, and the longest hexadecimal argument taken. */
#define FIDOCTL_PIN_LINE	72U
#define FIDOCTL_USER_ID_SIZE	16U
#define FIDOCTL_BYTES_MAX	1024U

/* The start of a smart card slot's node (a key held to an NFC reader). */
#define FIDOCTL_CARD_PREFIX	"/dev/smartcard"

/* An open key: its node (a raw HID one, or a smart card slot), its channel or applet and transport, the device, what it is, and the PIN protocol chosen. */
struct fidoctl_key {
	struct pk_os_hid handle;
	struct pk_os_card card;
	struct pk_hid hid;
	struct pk_nfc nfc;
	struct pk_transport transport;
	struct pk_device device;
	struct pk_info info;
	unsigned protocol;
};

/* The commands. */
enum fidoctl_kind {
	FIDOCTL_LIST,
	FIDOCTL_INFO,
	FIDOCTL_SET_PIN,
	FIDOCTL_CHANGE_PIN,
	FIDOCTL_REGISTER,
	FIDOCTL_ASSERT,
	FIDOCTL_VERIFY
};

/* A command: its name, the number of its arguments, and which it is. */
struct fidoctl_command {
	const char *name;
	int arguments;
	enum fidoctl_kind kind;
};

/* The commands the program knows (read only). */
static const struct fidoctl_command fidoctl_commands[] = {
	{ "list", 0, FIDOCTL_LIST },
	{ "info", 0, FIDOCTL_INFO },
	{ "set-pin", 0, FIDOCTL_SET_PIN },
	{ "change-pin", 0, FIDOCTL_CHANGE_PIN },
	{ "register", 2, FIDOCTL_REGISTER },
	{ "assert", 2, FIDOCTL_ASSERT },
	{ "verify", 6, FIDOCTL_VERIFY }
};

/* Whether the touch has been asked for in this command (the keepalive's request is printed once). */
static int fidoctl_touch_asked;

static int fidoctl_list(void);
static int fidoctl_open(const char *path, struct fidoctl_key *key);
static int fidoctl_open_card(const char *path, struct fidoctl_key *key);
static int fidoctl_card_answers(const char *path);
static int fidoctl_ready(struct fidoctl_key *key);
static void fidoctl_close(struct fidoctl_key *key);
static int fidoctl_info(struct fidoctl_key *key);
static int fidoctl_set_pin(struct fidoctl_key *key);
static int fidoctl_change_pin(struct fidoctl_key *key);
static int fidoctl_register(struct fidoctl_key *key, int use_pin, const char *rp_id, const char *user);
static int fidoctl_assert(struct fidoctl_key *key, int use_pin, int silent, const char *rp_id, const char *credential);
static int fidoctl_verify(int silent, char **arguments);
static int fidoctl_token(struct fidoctl_key *key, unsigned permissions, const char *rp_id, uint8_t *token, size_t *token_size);
static int fidoctl_read_pin(const char *prompt, char *pin, size_t size);
static int fidoctl_pin_valid(const char *pin);
static int fidoctl_hex_read(const char *text, uint8_t *bytes, size_t capacity, size_t *size);
static int fidoctl_hex_digit(char digit, unsigned *value);
static void fidoctl_hex_print(const char *name, const uint8_t *bytes, size_t size);
static void fidoctl_keepalive(void *context, uint8_t status);
static int fidoctl_fail(const char *step, int error, const struct fidoctl_key *key);
static int fidoctl_usage(void);

/*
 * Carries out one command; the exit status says whether it succeeded.
 */
int
main(
	int argc,
	char **argv)
{
	struct fidoctl_key key;
	const struct fidoctl_command *chosen;
	const char *path;
	size_t index;
	int use_pin;
	int silent;
	int option;
	int status;
	int error;
	int compared;

	/* The options: the node, whether a PIN is read, and whether an assertion asks no touch. */
	path = NULL;
	use_pin = 0;
	silent = 0;
	for (;;) {
		option = getopt(argc, argv, "d:ps");
		if (option == -1)
			break;
		if (option == 'd')
			path = optarg;
		else if (option == 'p')
			use_pin = 1;
		else if (option == 's')
			silent = 1;
		else
			return fidoctl_usage();
	}

	/* A command is needed. */
	if (optind >= argc)
		return fidoctl_usage();

	/* The command, by its name and its arguments' number. */
	chosen = NULL;
	for (index = 0U; index < sizeof(fidoctl_commands) / sizeof(fidoctl_commands[0]); index++) {
		compared = strcmp(argv[optind], fidoctl_commands[index].name);
		if (compared == 0 && argc - optind - 1 == fidoctl_commands[index].arguments) {
			chosen = &fidoctl_commands[index];
			break;
		}
	}

	/* None known. */
	if (chosen == NULL)
		return fidoctl_usage();

	/* The commands that need no key. */
	if (chosen->kind == FIDOCTL_LIST) {
		status = fidoctl_list();
		return status;
	}

	/* verify needs only its arguments. */
	if (chosen->kind == FIDOCTL_VERIFY) {
		status = fidoctl_verify(silent, argv + optind + 1);
		return status;
	}

	/* The key. */
	error = fidoctl_open(path, &key);
	if (error != 0)
		return 1;

	/* The command on it. */
	switch (chosen->kind) {
	case FIDOCTL_INFO:
		status = fidoctl_info(&key);
		break;
	case FIDOCTL_SET_PIN:
		status = fidoctl_set_pin(&key);
		break;
	case FIDOCTL_CHANGE_PIN:
		status = fidoctl_change_pin(&key);
		break;
	case FIDOCTL_REGISTER:
		status = fidoctl_register(&key, use_pin, argv[optind + 1], argv[optind + 2]);
		break;
	default:
		status = fidoctl_assert(&key, use_pin, silent, argv[optind + 1], argv[optind + 2]);
		break;
	}

	/* Done: the key given back. */
	fidoctl_close(&key);
	return status;
}

/* Prints each security key: its node, vendor and product, and name. */
static int
fidoctl_list(void)
{
	struct pk_os_device devices[PK_OS_DEVICES_MAX];
	size_t count;
	size_t answered;
	size_t index;
	int error;

	/* The keys. */
	error = pk_os_list(devices, PK_OS_DEVICES_MAX, &count);
	if (error != 0)
		return fidoctl_fail("list", error, NULL);

	/* One line each. */
	for (index = 0U; index < count; index++)
		printf("device %s %04x:%04x %s\n", devices[index].path, devices[index].vendor, devices[index].product, devices[index].name);
	printf("devices %lu\n", (unsigned long)count);

	/*
	 * The smart card slots that say they hold a card: a "card" line for
	 * each whose card answers the FIDO applet (a key held to an NFC
	 * reader), a "slot" line with why for the others, which are not
	 * counted (BUG-286: a reader's SAM slot says present but answers
	 * nothing).
	 */
	error = pk_os_list_cards(devices, PK_OS_DEVICES_MAX, &count);
	if (error != 0)
		count = 0U;
	answered = 0U;
	for (index = 0U; index < count; index++) {
		error = fidoctl_card_answers(devices[index].path);
		if (error != 0) {
			printf("slot %s %04x:%04x %s: %s\n", devices[index].path, devices[index].vendor, devices[index].product, devices[index].name, strerror(error));
			continue;
		}

		/* A key: counted. */
		printf("card %s %04x:%04x %s\n", devices[index].path, devices[index].vendor, devices[index].product, devices[index].name);
		answered++;
	}

	/* The keys counted. */
	printf("cards %lu\n", (unsigned long)answered);

	/* Succeeded. */
	return 0;
}

/*
 * Opens a key (the first listed when path is NULL), takes its channel and
 * asks what it is.  Returns 0, or an errno value after printing why.
 */
static int
fidoctl_open(
	const char *path,
	struct fidoctl_key *key)
{
	struct pk_os_device devices[PK_OS_DEVICES_MAX];
	struct pk_hid_io io;
	size_t count;
	size_t first;
	int error;
	int same;

	/* The first key, when none is named: a USB one, else one held to an NFC reader. */
	memset(key, 0, sizeof(*key));
	key->handle.descriptor = -1;
	key->card.descriptor = -1;
	if (path == NULL) {
		error = pk_os_list(devices, PK_OS_DEVICES_MAX, &count);
		if (error != 0) {
			(void)fidoctl_fail("list", error, NULL);
			return error;
		}

		/* No USB key: the first card held to an NFC reader that answers the FIDO applet. */
		first = 0U;
		if (count == 0U) {
			error = pk_os_list_cards(devices, PK_OS_DEVICES_MAX, &count);
			if (error != 0)
				count = 0U;
			for (first = 0U; first < count; first++) {
				error = fidoctl_card_answers(devices[first].path);
				if (error == 0)
					break;
			}
		}

		/* At least one key, the first taken. */
		if (first == count) {
			(void)fidoctl_fail("no security key", ENODEV, NULL);
			return ENODEV;
		}

		/* Its node. */
		path = devices[first].path;
	}

	/* A smart card slot: the NFC transport. */
	same = strncmp(path, FIDOCTL_CARD_PREFIX, sizeof(FIDOCTL_CARD_PREFIX) - 1U);
	if (same == 0) {
		error = fidoctl_open_card(path, key);
		if (error != 0)
			return error;
		return fidoctl_ready(key);
	}

	/* The node, taken for this program alone. */
	error = pk_os_open(&key->handle, path, 1, &io);
	if (error != 0) {
		(void)fidoctl_fail(path, error, NULL);
		return error;
	}

	/* The channel, and CTAP2 over it. */
	error = pk_hid_open(&key->hid, &io, FIDOCTL_OPEN_MS);
	if (error != 0) {
		(void)fidoctl_fail("CTAPHID INIT", error, NULL);
		pk_os_close(&key->handle);
		return error;
	}

	/* CTAP2 over the channel. */
	(void)pk_hid_transport(&key->transport, &key->hid);

	/* Succeeded when the key says what it is. */
	error = fidoctl_ready(key);
	return error;
}

/*
 * Opens a key held to an NFC reader: the slot's card powered and the FIDO
 * applet selected, CTAP2 over it.  Returns 0, or an errno value after
 * printing why.
 */
static int
fidoctl_open_card(
	const char *path,
	struct fidoctl_key *key)
{
	struct pk_nfc_io io;
	int error;

	/* The slot, its card powered and claimed for this program. */
	error = pk_os_card_open(&key->card, path, &io);
	if (error != 0) {
		(void)fidoctl_fail(path, error, NULL);
		return error;
	}

	/* The FIDO applet. */
	error = pk_nfc_open(&key->nfc, &io, FIDOCTL_OPEN_MS);
	if (error != 0) {
		(void)fidoctl_fail("NFC SELECT", error, NULL);
		pk_os_card_close(&key->card);
		return error;
	}

	/* Succeeded: CTAP2 over the applet. */
	(void)pk_nfc_transport(&key->transport, &key->nfc);
	return 0;
}

/*
 * Whether a smart card slot's card answers the FIDO applet: the card
 * powered, the applet selected, and the slot given back, nothing printed.
 * Returns 0, or an errno value (EBUSY while another program holds the
 * slot, EIO from a slot whose card does not answer).
 */
static int
fidoctl_card_answers(
	const char *path)
{
	struct pk_os_card card;
	struct pk_nfc_io io;
	struct pk_nfc nfc;
	int error;

	/* The slot, its card powered. */
	card.descriptor = -1;
	error = pk_os_card_open(&card, path, &io);
	if (error != 0)
		return error;

	/* The FIDO applet; the card powered off again either way. */
	error = pk_nfc_open(&nfc, &io, FIDOCTL_OPEN_MS);
	pk_os_card_close(&card);
	return error;
}

/*
 * Starts CTAP2 on an open key's transport (a touch's wait told), asks what
 * the key is and chooses its PIN protocol.  Returns 0, or an errno value
 * after printing why (the key closed).
 */
static int
fidoctl_ready(
	struct fidoctl_key *key)
{
	int error;

	/* The device, a touch's wait told. */
	pk_device_init(&key->device, &key->transport, FIDOCTL_COMMAND_MS);
	key->device.keepalive = fidoctl_keepalive;

	/* What the key is. */
	error = pk_ctap2_get_info(&key->device, &key->info);
	if (error != 0) {
		(void)fidoctl_fail("GetInfo", error, key);
		fidoctl_close(key);
		return error;
	}

	/* The newer PIN protocol it has. */
	key->protocol = pk_ctap2_choose_protocol(&key->info);

	/* Succeeded: the key is open. */
	return 0;
}

/* Gives the key back. */
static void
fidoctl_close(
	struct fidoctl_key *key)
{
	/* The node (and the grab), or the slot (its card powered off). */
	pk_os_close(&key->handle);
	pk_os_card_close(&key->card);
}

/* Prints what the key is, and its PIN's retries when it has a PIN. */
static int
fidoctl_info(
	struct fidoctl_key *key)
{
	const char *pin;
	const char *resident;
	unsigned retries;
	int error;

	/* The versions GetInfo gave. */
	printf("versions");
	if ((key->info.versions & PK_INFO_FIDO_2_0) != 0U)
		printf(" FIDO_2_0");
	if ((key->info.versions & PK_INFO_FIDO_2_1) != 0U)
		printf(" FIDO_2_1");
	if ((key->info.versions & PK_INFO_U2F_V2) != 0U)
		printf(" U2F_V2");
	printf("\n");

	/* Whether it has a PIN: set, not yet set, or no PIN function. */
	pin = "none";
	if ((key->info.options & PK_OPTION_CLIENT_PIN) != 0U)
		pin = "unset";
	if ((key->info.options & PK_OPTION_CLIENT_PIN_SET) != 0U)
		pin = "set";

	/* Whether it keeps credentials of its own. */
	resident = "no";
	if ((key->info.options & PK_OPTION_RK) != 0U)
		resident = "yes";

	/* The identity, the PIN, the protocol and the limits. */
	fidoctl_hex_print("aaguid", key->info.aaguid, sizeof(key->info.aaguid));
	printf("client-pin %s\n", pin);
	printf("pin-protocol %u\n", key->protocol);
	printf("resident-keys %s\n", resident);
	printf("min-pin-length %lu\n", (unsigned long)key->info.min_pin_length);
	printf("max-message %lu\n", (unsigned long)key->info.max_message);

	/* The PIN's retries, when a PIN is set. */
	if ((key->info.options & PK_OPTION_CLIENT_PIN_SET) != 0U && key->protocol != 0U) {
		error = pk_ctap2_pin_retries(&key->device, key->protocol, &retries);
		if (error != 0)
			return fidoctl_fail("getPINRetries", error, key);
		printf("pin-retries %u\n", retries);
	}

	/* Succeeded. */
	return 0;
}

/* Sets the key's first PIN, read from standard input. */
static int
fidoctl_set_pin(
	struct fidoctl_key *key)
{
	char pin[FIDOCTL_PIN_LINE];
	int error;

	/* The new PIN. */
	error = fidoctl_read_pin("New PIN: ", pin, sizeof(pin));
	if (error != 0)
		return fidoctl_fail("the PIN", error, NULL);

	/* Set, and the PIN forgotten. */
	error = pk_ctap2_set_pin(&key->device, key->protocol, pin);
	pk_crypto_wipe(pin, sizeof(pin));
	if (error != 0)
		return fidoctl_fail("setPIN", error, key);

	/* Succeeded. */
	printf("pin set\n");
	return 0;
}

/* Changes the key's PIN: the current one, then the new one, from standard input. */
static int
fidoctl_change_pin(
	struct fidoctl_key *key)
{
	char current[FIDOCTL_PIN_LINE];
	char pin[FIDOCTL_PIN_LINE];
	int error;

	/* The current PIN and the new one. */
	error = fidoctl_read_pin("Current PIN: ", current, sizeof(current));
	if (error == 0)
		error = fidoctl_read_pin("New PIN: ", pin, sizeof(pin));
	if (error != 0) {
		pk_crypto_wipe(current, sizeof(current));
		return fidoctl_fail("the PINs", error, NULL);
	}

	/* Changed, and both forgotten. */
	error = pk_ctap2_change_pin(&key->device, key->protocol, current, pin);
	pk_crypto_wipe(current, sizeof(current));
	pk_crypto_wipe(pin, sizeof(pin));
	if (error != 0)
		return fidoctl_fail("changePIN", error, key);

	/* Succeeded. */
	printf("pin changed\n");
	return 0;
}

/* Makes a credential for a relying party and a user, and prints it. */
static int
fidoctl_register(
	struct fidoctl_key *key,
	int use_pin,
	const char *rp_id,
	const char *user)
{
	struct pk_make_request request;
	struct pk_made_credential credential;
	uint8_t user_id[FIDOCTL_USER_ID_SIZE];
	uint8_t token[PK_PIN_TOKEN_MAX];
	size_t token_size;
	int error;

	/* A new user ID and client data hash. */
	memset(&request, 0, sizeof(request));
	error = pk_crypto_random(user_id, sizeof(user_id));
	if (error == 0)
		error = pk_crypto_random(request.client_data_hash, sizeof(request.client_data_hash));
	if (error != 0)
		return fidoctl_fail("random", error, NULL);
	request.rp_id = rp_id;
	request.user_id = user_id;
	request.user_id_size = sizeof(user_id);
	request.user_name = user;

	/* The PIN's token, when the user is to be verified. */
	if (use_pin) {
		error = fidoctl_token(key, PK_PERMISSION_MAKE_CREDENTIAL, rp_id, token, &token_size);
		if (error != 0)
			return 1;
		request.pin_token = token;
		request.pin_token_size = token_size;
		request.pin_protocol = key->protocol;
	}

	/* The credential (the key asks for a touch). */
	fidoctl_touch_asked = 0;
	error = pk_ctap2_make_credential(&key->device, &request, &credential);
	pk_crypto_wipe(token, sizeof(token));
	if (error != 0)
		return fidoctl_fail("MakeCredential", error, key);

	/* Succeeded: what the verifier keeps. */
	fidoctl_hex_print("credential", credential.id, credential.id_size);
	fidoctl_hex_print("public-key", credential.cose_key, credential.cose_key_size);
	fidoctl_hex_print("user-id", user_id, sizeof(user_id));
	printf("sign-count %lu\n", (unsigned long)credential.sign_count);
	printf("flags 0x%02x\n", credential.flags);
	return 0;
}

/* Asks the key for an assertion of a credential over a new client data hash, and prints it. */
static int
fidoctl_assert(
	struct fidoctl_key *key,
	int use_pin,
	int silent,
	const char *rp_id,
	const char *credential)
{
	struct pk_assertion_request request;
	struct pk_assertion_reply reply;
	const uint8_t *allow[1];
	size_t allow_sizes[1];
	uint8_t id[PK_CREDENTIAL_ID_MAX];
	uint8_t token[PK_PIN_TOKEN_MAX];
	size_t id_size;
	size_t token_size;
	int error;

	/* The credential allowed. */
	error = fidoctl_hex_read(credential, id, sizeof(id), &id_size);
	if (error != 0)
		return fidoctl_fail("the credential", error, NULL);
	allow[0] = id;
	allow_sizes[0] = id_size;

	/* The question: the user present unless asked silent, over a new client data hash. */
	memset(&request, 0, sizeof(request));
	request.rp_id = rp_id;
	request.presence = 1;
	if (silent)
		request.presence = 0;
	request.allow_ids = allow;
	request.allow_sizes = allow_sizes;
	request.allow_count = 1U;
	error = pk_crypto_random(request.client_data_hash, sizeof(request.client_data_hash));
	if (error != 0)
		return fidoctl_fail("random", error, NULL);

	/* The PIN's token, when the user is to be verified. */
	if (use_pin) {
		error = fidoctl_token(key, PK_PERMISSION_GET_ASSERTION, rp_id, token, &token_size);
		if (error != 0)
			return 1;
		request.pin_token = token;
		request.pin_token_size = token_size;
		request.pin_protocol = key->protocol;
	}

	/* The assertion (the key asks for a touch). */
	fidoctl_touch_asked = 0;
	error = pk_ctap2_get_assertion(&key->device, &request, &reply);
	pk_crypto_wipe(token, sizeof(token));
	if (error != 0)
		return fidoctl_fail("GetAssertion", error, key);

	/* Succeeded: what verify takes. */
	fidoctl_hex_print("credential", reply.credential_id, reply.credential_id_size);
	fidoctl_hex_print("client-data-hash", request.client_data_hash, sizeof(request.client_data_hash));
	fidoctl_hex_print("auth-data", reply.auth_data, reply.auth_data_size);
	fidoctl_hex_print("signature", reply.signature, reply.signature_size);

	/* The flags the key set (UP 0x01, UV 0x04), after the relying party's hash. */
	if (reply.auth_data_size > PK_SHA256_SIZE)
		printf("flags 0x%02x\n", reply.auth_data[PK_SHA256_SIZE]);
	return 0;
}

/*
 * Checks an assertion with the stored public key alone: the arguments are
 * the relying party, the credential, its COSE key, the client data hash,
 * the authenticator data and the signature.
 */
static int
fidoctl_verify(
	int silent,
	char **arguments)
{
	static uint8_t cose_key[FIDOCTL_BYTES_MAX];
	static uint8_t id[FIDOCTL_BYTES_MAX];
	static uint8_t auth_data[FIDOCTL_BYTES_MAX];
	static uint8_t signature[FIDOCTL_BYTES_MAX];
	struct pk_expectation expectation;
	struct pk_credential allowed;
	struct pk_assertion assertion;
	size_t cose_key_size;
	size_t id_size;
	size_t hash_size;
	size_t auth_data_size;
	size_t signature_size;
	size_t matched;
	uint32_t sign_count;
	int error;

	/* The bytes. */
	memset(&expectation, 0, sizeof(expectation));
	error = fidoctl_hex_read(arguments[1], id, sizeof(id), &id_size);
	if (error == 0)
		error = fidoctl_hex_read(arguments[2], cose_key, sizeof(cose_key), &cose_key_size);
	if (error == 0)
		error = fidoctl_hex_read(arguments[3], expectation.client_data_hash, sizeof(expectation.client_data_hash), &hash_size);
	if (error == 0)
		error = fidoctl_hex_read(arguments[4], auth_data, sizeof(auth_data), &auth_data_size);
	if (error == 0)
		error = fidoctl_hex_read(arguments[5], signature, sizeof(signature), &signature_size);
	if (error == 0 && hash_size != PK_SHA256_SIZE)
		error = EINVAL;
	if (error != 0)
		return fidoctl_fail("the arguments", error, NULL);

	/* What is expected: the relying party, the user present (unless silent), this credential. */
	allowed.id = id;
	allowed.id_size = id_size;
	allowed.cose_key = cose_key;
	allowed.cose_key_size = cose_key_size;
	allowed.sign_count = 0U;
	expectation.rp_id = arguments[0];
	expectation.required_flags = PK_FLAG_UP;
	if (silent)
		expectation.required_flags = 0U;
	expectation.credentials = &allowed;
	expectation.credential_count = 1U;

	/* What the key answered. */
	assertion.credential_id = id;
	assertion.credential_id_size = id_size;
	assertion.auth_data = auth_data;
	assertion.auth_data_size = auth_data_size;
	assertion.signature = signature;
	assertion.signature_size = signature_size;

	/* Checked. */
	error = pk_verify_assertion(&expectation, &assertion, &matched, &sign_count);
	if (error != 0)
		return fidoctl_fail("verify", error, NULL);

	/* Succeeded: the count to store. */
	printf("verified sign-count %lu\n", (unsigned long)sign_count);
	return 0;
}

/*
 * Reads the PIN from standard input and gets a token of the permissions for
 * the relying party.  Returns 0, or an errno value after printing why.
 */
static int
fidoctl_token(
	struct fidoctl_key *key,
	unsigned permissions,
	const char *rp_id,
	uint8_t *token,
	size_t *token_size)
{
	char pin[FIDOCTL_PIN_LINE];
	int error;

	/* The PIN. */
	error = fidoctl_read_pin("PIN: ", pin, sizeof(pin));
	if (error != 0) {
		(void)fidoctl_fail("the PIN", error, NULL);
		return error;
	}

	/* The token, and the PIN forgotten. */
	*token_size = PK_PIN_TOKEN_MAX;
	error = pk_ctap2_pin_token(&key->device, &key->info, key->protocol, pin, permissions, rp_id, token, token_size);
	pk_crypto_wipe(pin, sizeof(pin));
	if (error != 0) {
		(void)fidoctl_fail("the PIN's token", error, key);
		return error;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Reads one line of standard input as a PIN, without its line end.  On a
 * terminal the prompt is shown and the PIN is typed without echo
 * (ws177-p006).  The PIN must keep CTAP2's rule, checked before the key is
 * asked: at least 4 characters and at most 63 bytes.  Returns 0, or EINVAL
 * for no line, a line longer than the buffer (its rest is read and
 * dropped) or a PIN that breaks the rule (said on standard error).
 */
static int
fidoctl_read_pin(
	const char *prompt,
	char *pin,
	size_t size)
{
	struct termios saved;
	struct termios quiet;
	char *line;
	size_t length;
	int terminal;
	int hidden;
	int got;
	int character;
	int valid;

	/* A terminal is asked with the prompt, its echo turned off for the line. */
	terminal = isatty(STDIN_FILENO);
	hidden = 0;
	if (terminal) {
		fputs(prompt, stderr);
		got = tcgetattr(STDIN_FILENO, &saved);
		if (got == 0) {
			quiet = saved;
			quiet.c_lflag &= ~(tcflag_t)ECHO;
			got = tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
			if (got == 0)
				hidden = 1;
		}
	}

	/* The line, and the echo back as it was. */
	line = fgets(pin, (int)size, stdin);
	if (hidden) {
		(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
		fputc('\n', stderr);
	}

	/* No line at all. */
	if (line == NULL)
		return EINVAL;

	/* A line longer than the buffer: its rest is dropped, and it is not a PIN. */
	length = strcspn(pin, "\r\n");
	if (pin[length] == '\0' && length + 1U == size) {
		character = getchar();
		while (character != EOF && character != '\n')
			character = getchar();
		pk_crypto_wipe(pin, size);
		fprintf(stderr, "fidoctl: a PIN is at most 63 bytes\n");
		return EINVAL;
	}

	/* Without its line end. */
	pin[length] = '\0';

	/* CTAP2's rule. */
	valid = fidoctl_pin_valid(pin);
	if (!valid) {
		pk_crypto_wipe(pin, size);
		fprintf(stderr, "fidoctl: a PIN is 4 to 63 bytes, at least 4 characters\n");
		return EINVAL;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Tells whether a PIN keeps CTAP2's rule (authenticatorClientPIN): at most
 * 63 bytes of UTF-8 and at least 4 characters (code points).  Returns 1 or
 * 0.
 */
static int
fidoctl_pin_valid(
	const char *pin)
{
	size_t length;
	size_t index;
	size_t characters;
	unsigned char byte;

	/* At most 63 bytes. */
	length = strlen(pin);
	if (length > 63U)
		return 0;

	/* The characters: each byte that is not a UTF-8 continuation starts one. */
	characters = 0U;
	for (index = 0U; index < length; index++) {
		byte = (unsigned char)pin[index];
		if ((byte & 0xc0U) != 0x80U)
			characters++;
	}

	/* At least 4. */
	if (characters < 4U)
		return 0;

	/* Succeeded: it keeps the rule. */
	return 1;
}

/*
 * Reads hexadecimal into at most capacity bytes.  Returns 0, or EINVAL for
 * an odd length, a character that is not a digit, or too many bytes.
 */
static int
fidoctl_hex_read(
	const char *text,
	uint8_t *bytes,
	size_t capacity,
	size_t *size)
{
	unsigned high;
	unsigned low;
	size_t length;
	size_t index;
	int error;

	/* An even number of digits that fits. */
	length = strlen(text);
	if (length % 2U != 0U || length / 2U > capacity)
		return EINVAL;

	/* Two digits a byte. */
	for (index = 0U; index < length / 2U; index++) {
		error = fidoctl_hex_digit(text[2U * index], &high);
		if (error == 0)
			error = fidoctl_hex_digit(text[2U * index + 1U], &low);
		if (error != 0)
			return EINVAL;
		bytes[index] = (uint8_t)(high << 4 | low);
	}

	/* Succeeded. */
	*size = length / 2U;
	return 0;
}

/* Reads one hexadecimal digit.  Returns 0, or EINVAL for another character. */
static int
fidoctl_hex_digit(
	char digit,
	unsigned *value)
{
	/* 0 to 9, a to f, A to F. */
	if (digit >= '0' && digit <= '9') {
		*value = (unsigned)(digit - '0');
		return 0;
	}

	/* The small letters. */
	if (digit >= 'a' && digit <= 'f') {
		*value = (unsigned)(digit - 'a' + 10);
		return 0;
	}

	/* The capitals. */
	if (digit >= 'A' && digit <= 'F') {
		*value = (unsigned)(digit - 'A' + 10);
		return 0;
	}

	/* Not a digit. */
	return EINVAL;
}

/* Prints a line "NAME HEX". */
static void
fidoctl_hex_print(
	const char *name,
	const uint8_t *bytes,
	size_t size)
{
	size_t index;

	/* The name, then two digits a byte. */
	printf("%s ", name);
	for (index = 0U; index < size; index++)
		printf("%02x", bytes[index]);
	printf("\n");
}

/* Tells the user, once a command, that the key waits for a touch. */
static void
fidoctl_keepalive(
	void *context,
	uint8_t status)
{
	/* Only the wait for the user, once. */
	(void)context;
	if (status != PK_HID_KEEPALIVE_UP_NEEDED || fidoctl_touch_asked)
		return;
	fidoctl_touch_asked = 1;
	fprintf(stderr, "fidoctl: touch the security key\n");
}

/* Prints why a step failed (with the key's CTAP2 status when it answered one); returns the exit status 1. */
static int
fidoctl_fail(
	const char *step,
	int error,
	const struct fidoctl_key *key)
{
	/* The status the key answered, or the error alone. */
	if (error == EPROTO && key != NULL)
		fprintf(stderr, "fidoctl: %s: the key answered 0x%02x\n", step, key->device.last_status);
	else
		fprintf(stderr, "fidoctl: %s: %s\n", step, strerror(error));
	return 1;
}

/* Prints how the program is used; returns the exit status 2. */
static int
fidoctl_usage(void)
{
	/* The commands. */
	fprintf(stderr,
		"usage: fidoctl list\n"
		"       fidoctl [-d NODE] info | set-pin | change-pin\n"
		"       fidoctl [-d NODE] [-p] register RP USER\n"
		"       fidoctl [-d NODE] [-p] [-s] assert RP CREDENTIAL\n"
		"       fidoctl [-s] verify RP CREDENTIAL KEY CLIENT-DATA-HASH AUTH-DATA SIGNATURE\n");
	return 2;
}
