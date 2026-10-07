/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A greeter without a picture, for sessiond's tests (ws035-p094).
 *
 * sessiond starts it in place of the compositor --greeter, as _greeter, with the
 * socket on descriptor 3.  It reads its steps from /tmp/greeter-probe, one a
 * line, and logs each with the answer (the password is not logged):
 *
 *   AUTH name password     sends the request and logs the answer
 *   POWER what             the same
 *   SLEEP seconds          waits
 *   EXIT status            ends with that status
 *
 * Without the file it ends with status 1, as a greeter that cannot start.
 *
 * Run by root as greeter-probe --open-as=UID PATH it opens PATH read-write
 * as that user instead, for the GPU's admission test (ws035-p095).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The steps' file, which the test writes. */
#define PROBE_SCRIPT	"/tmp/greeter-probe"

/* The socket sessiond answers on. */
#define PROBE_FD	3

static int probe_send(const char *line);
static int probe_open_as(uid_t uid, const char *path);

/*
 * Runs the steps.
 */
int
main(
	int count,
	char **arguments)
{
	char line[512];
	FILE *script;
	char *got;
	size_t length;
	int match;
	int index;
	int opened;

	/* Run by root as greeter-probe --open-as=UID PATH: opens PATH as that user (the GPU's admission test). */
	match = 1;
	if (count == 3)
		match = strncmp(arguments[1], "--open-as=", 10);
	if (match == 0) {
		opened = probe_open_as((uid_t)atoi(arguments[1] + 10), arguments[2]);
		return opened;
	}

	/* The options sessiond passes (--greeter, --auth-fd, --wallpaper) are logged, not used. */
	for (index = 1; index < count; index++)
		printf("PROBE argument=%s\n", arguments[index]);
	printf("PROBE start uid=%u\n", (unsigned)getuid());
	fflush(stdout);

	/* The steps. */
	script = fopen(PROBE_SCRIPT, "r");
	if (script == NULL) {
		printf("PROBE no script errno=%d\n", errno);
		return 1;
	}

	/* Each step. */
	for (;;) {
		got = fgets(line, sizeof(line), script);
		if (got == NULL)
			break;
		length = strlen(line);
		if (length > 0U && line[length - 1U] == '\n')
			line[length - 1U] = '\0';

		/* A wait. */
		match = strncmp(line, "SLEEP ", 6);
		if (match == 0) {
			sleep((unsigned)atoi(line + 6));
			continue;
		}

		/* The end, with a status. */
		match = strncmp(line, "EXIT ", 5);
		if (match == 0) {
			printf("PROBE exit status=%d\n", atoi(line + 5));
			fclose(script);
			return atoi(line + 5);
		}

		/* A request. */
		(void)probe_send(line);
	}

	/* The steps have run out. */
	fclose(script);
	printf("PROBE done\n");
	return 0;
}

/* Sends one request to sessiond and logs its answer. */
static int
probe_send(
	const char *line)
{
	char request[520];
	char reply[64];
	char shown[80];
	char *space;
	ssize_t count;
	size_t length;
	int match;

	/* The request as it is logged: an AUTH's password is left out. */
	snprintf(shown, sizeof(shown), "%s", line);
	match = strncmp(shown, "AUTH ", 5);
	if (match == 0) {
		space = strchr(shown + 5, ' ');
		if (space != NULL)
			*space = '\0';
	}

	/* The request. */
	snprintf(request, sizeof(request), "%s\n", line);
	length = strlen(request);
	count = write(PROBE_FD, request, length);
	if (count != (ssize_t)length) {
		printf("PROBE send=%s errno=%d\n", shown, errno);
		fflush(stdout);
		return -1;
	}

	/* The answer, one line. */
	count = read(PROBE_FD, reply, sizeof(reply) - 1U);
	if (count <= 0) {
		printf("PROBE send=%s closed\n", shown);
		fflush(stdout);
		return -1;
	}

	/* The answer's first line. */
	reply[count] = '\0';
	space = strchr(reply, '\n');
	if (space != NULL)
		*space = '\0';

	/* Succeeded: the answer is logged. */
	printf("PROBE send=%s reply=%s\n", shown, reply);
	fflush(stdout);
	return 0;
}

/* Opens a path read-write as a user and says whether it opened (exit 0) or why not (exit 1). */
static int
probe_open_as(
	uid_t uid,
	const char *path)
{
	int descriptor;
	int error;

	/* The user's ids, from root (the group of the same number). */
	error = setgid((gid_t)uid);
	if (error == 0)
		error = setuid(uid);
	if (error != 0) {
		printf("PROBE open-as uid=%u setuid errno=%d\n", (unsigned)uid, errno);
		return 2;
	}

	/* The open. */
	descriptor = open(path, O_RDWR);
	if (descriptor < 0) {
		printf("PROBE open-as uid=%u path=%s errno=%d\n", (unsigned)uid, path, errno);
		return 1;
	}

	/* Succeeded: the user may open it. */
	close(descriptor);
	printf("PROBE open-as uid=%u path=%s ok\n", (unsigned)uid, path);
	return 0;
}
