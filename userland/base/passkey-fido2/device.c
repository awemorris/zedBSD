/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * passkey-fido2's side of the device helper (fido2.h; ws172-p003): the
 * security keys' nodes are opened and claimed here, as root, and the
 * smart card slots attached (ws199-p001: a key held to an NFC reader; a
 * slot's card is powered and claimed by the helper), so that the helper
 * needs to open nothing; the helper is started with them and its
 * messages are read until its answer.  A "touch" is passed on at once as
 * passkey's "status touch".  A helper that does not answer within the
 * touch's time is killed and the attempt is a timeout.
 *
 * sessiond ends the work with SIGTERM to the process group (ws199-p001
 * section 11, R1): passkey-fido2 notes it (fido2_catch_end), passes it on
 * to the helper, which cancels the key's command, and waits a short time
 * for the helper's answer (a reset already sent is waited for to its own
 * end, so its lines are removed).
 */

#include "fido2.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How much longer than the touch the answer may take (the PIN's agreement and the key's work). */
#define DEVICE_SLACK_MS		3000U

/* How long the helper may take to answer once the work was ended. */
#define DEVICE_END_MS		1500U

/* Whether sessiond ended the work (SIGTERM). */
volatile sig_atomic_t fido2_ended;

static uint64_t device_now_ms(void);
static void device_ended(int signal_number);

/* Notes sessiond's SIGTERM instead of dying of it (the waits see EINTR). */
void
fido2_catch_end(void)
{
	struct sigaction ending;

	/* No restart: a wait ends at once. */
	memset(&ending, 0, sizeof(ending));
	ending.sa_handler = device_ended;
	sigemptyset(&ending.sa_mask);
	(void)sigaction(SIGTERM, &ending, NULL);
}

/* Notes the end (async-signal-safe). */
static void
device_ended(
	int signal_number)
{
	/* Only noted. */
	(void)signal_number;
	fido2_ended = 1;
}

/*
 * Opens and claims every security key's node, and attaches every smart
 * card slot.  Returns 0 (none open is not an error: the helper says
 * no-key), or an errno value when the nodes cannot be listed.
 */
int
fido2_devices_open(
	struct fido2_devices *devices)
{
	struct pk_os_device found[PK_OS_DEVICES_MAX];
	struct pk_hid_io io;
	size_t count;
	size_t index;
	int error;

	/* The keys there are. */
	memset(devices, 0, sizeof(*devices));
	error = pk_os_list(found, PK_OS_DEVICES_MAX, &count);
	if (error != 0)
		return error;

	/* Each one opened and claimed; one another program holds is left alone. */
	for (index = 0U; index < count; index++) {
		error = pk_os_open(&devices->handles[devices->count], found[index].path, 1, &io);
		if (error != 0)
			continue;
		(void)snprintf(devices->names[devices->count], sizeof(devices->names[0]), "%s", found[index].name);
		devices->count++;
	}

	/* The smart card slots, with a card or without (none is not an error). */
	error = pk_os_list_slots(found, PK_OS_DEVICES_MAX, &count);
	if (error != 0)
		count = 0U;
	for (index = 0U; index < count; index++) {
		error = pk_os_card_attach(&devices->cards[devices->card_count], found[index].path);
		if (error != 0)
			continue;
		(void)snprintf(devices->card_names[devices->card_count], sizeof(devices->card_names[0]), "%s", found[index].name);
		devices->card_count++;
	}

	/* Succeeded: the keys that could be claimed, and the slots. */
	return 0;
}

/* Closes the keys' nodes and the slots (their claims go with them). */
void
fido2_devices_close(
	struct fido2_devices *devices)
{
	size_t index;

	/* Each one, and each slot (its card powered off). */
	for (index = 0U; index < devices->count; index++)
		pk_os_close(&devices->handles[index]);
	devices->count = 0U;
	for (index = 0U; index < devices->card_count; index++)
		pk_os_card_close(&devices->cards[index]);
	devices->card_count = 0U;
}

/*
 * Starts the helper on the keys with its job (it becomes uid and gid) and
 * reads its messages until its answer, passing each touch on.  Returns 0
 * with the answer in *message, ETIMEDOUT when it took too long, or another
 * errno value.
 */
int
fido2_run_helper(
	struct fido2_devices *devices,
	const struct fido2_job *job,
	uid_t uid,
	gid_t gid,
	struct fido2_message *message)
{
	struct pollfd wait;
	char line[FIDO2_MESSAGE_MAX];
	uint64_t deadline;
	uint64_t now;
	size_t used;
	ssize_t got;
	char *end;
	pid_t child;
	int pipes[2];
	pid_t waited;
	int ready;
	int status;
	int result;
	int error;
	int answered;
	int passed_on;

	/* The helper's pipe. */
	result = pipe(pipes);
	if (result != 0)
		return errno;

	/* The helper. */
	(void)fflush(stdout);
	child = fork();
	if (child < 0) {
		error = errno;
		(void)close(pipes[0]);
		(void)close(pipes[1]);
		return error;
	}

	/* The child becomes the helper and never comes back. */
	if (child == 0) {
		(void)close(pipes[0]);
		fido2_helper(devices, job, pipes[1], uid, gid);
		_exit(2);
	}

	/* Only the helper writes on its pipe. */
	(void)close(pipes[1]);

	/* Its lines until its answer, within the touch's time. */
	deadline = device_now_ms() + FIDO2_TOUCH_MS + DEVICE_SLACK_MS;
	used = 0U;
	answered = 0;
	passed_on = 0;
	error = 0;
	while (!answered && error == 0) {
		/* A whole line already read is taken first. */
		end = memchr(line, '\n', used);
		if (end != NULL) {
			*end = '\0';
			error = fido2_message_parse(line, message);
			if (error != 0) {
				error = EPROTO;
				break;
			}

			/* The line taken out of the buffer. */
			used -= (size_t)(end + 1 - line);
			memmove(line, end + 1, used);

			/* A touch is passed on. */
			if (message->kind == FIDO2_MESSAGE_TOUCH) {
				printf("status touch\n");
				(void)fflush(stdout);
				continue;
			}

			/* Anything else is the answer. */
			answered = 1;
			break;
		}

		/* The work ended: passed on to the helper once; its answer is waited for a short time (a reset's to its end). */
		now = device_now_ms();
		if (fido2_ended && !passed_on) {
			passed_on = 1;
			(void)kill(child, SIGTERM);
			if (job->kind != FIDO2_JOB_RESET && deadline > now + DEVICE_END_MS)
				deadline = now + DEVICE_END_MS;
		}

		/* More, before the deadline. */
		if (now >= deadline || used == sizeof(line)) {
			error = ETIMEDOUT;
			break;
		}

		/* The pipe waited on until the deadline. */
		wait.fd = pipes[0];
		wait.events = POLLIN;
		wait.revents = 0;
		ready = poll(&wait, 1, (int)(deadline - now));
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0) {
			error = ETIMEDOUT;
			break;
		}

		/* What it wrote. */
		got = read(pipes[0], line + used, sizeof(line) - used);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			error = EPIPE;
			break;
		}

		/* Kept after what was there. */
		used += (size_t)got;
	}

	/* The helper is done with: stopped when it did not answer, and waited for. */
	(void)close(pipes[0]);
	if (!answered)
		(void)kill(child, SIGKILL);
	for (;;) {
		waited = waitpid(child, &status, 0);
		if (waited >= 0 || errno != EINTR)
			break;
	}

	/* What it said is not kept; no answer is a failure. */
	pk_crypto_wipe(line, sizeof(line));
	if (!answered)
		return error;

	/* Succeeded: the answer. */
	return 0;
}

/* Gives the monotonic time in milliseconds. */
static uint64_t
device_now_ms(void)
{
	struct timespec now;

	/* The clock that does not go back. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
