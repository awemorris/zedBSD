/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Bluetooth command (ws143-p003): asks bluetoothd on its socket.
 *
 *   bt show            the state and the controller
 *   bt scan [SECONDS]  a scan (root only in this Phase; 8 seconds unless
 *                      given), and the devices found
 *   bt devices         the devices of the last scan
 *
 * It prints the daemon's lines as they are, then one line the tests read:
 * "BT SHOW state=WORD" or "BT SCAN devices=N".  It exits with 0, 1 when
 * the daemon answered ERROR, or 2 when there is no daemon.
 */

#include "userland/base/bluetoothd/protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* The scan's length when none is given. */
#define BT_SCAN_SECONDS		8UL

/* How a run ended: the daemon answered, it answered ERROR, there is no daemon (or it went), the command was misused. */
#define BT_EXIT_OK		0
#define BT_EXIT_ERROR		1
#define BT_EXIT_NO_DAEMON	2
#define BT_EXIT_USAGE		64

static int bt_connect(void);
static int bt_ask(const char *request, const char *summary);
static void bt_usage(void);

/*
 * Runs one request.
 */
int
main(
	int argc,
	char **argv)
{
	char request[64];
	unsigned long seconds;
	char *end;
	int status;
	int same;

	/* A command is needed. */
	if (argc < 2) {
		bt_usage();
		return BT_EXIT_USAGE;
	}

	/* bt show. */
	same = strcmp(argv[1], "show");
	if (same == 0 && argc == 2) {
		status = bt_ask("SHOW", "SHOW");
		return status;
	}

	/* bt devices. */
	same = strcmp(argv[1], "devices");
	if (same == 0 && argc == 2) {
		status = bt_ask("DEVICES", "SCAN");
		return status;
	}

	/* bt scan [SECONDS]. */
	same = strcmp(argv[1], "scan");
	if (same == 0 && argc <= 3) {
		seconds = BT_SCAN_SECONDS;
		if (argc == 3) {
			errno = 0;
			seconds = strtoul(argv[2], &end, 10);
			if (errno != 0 || end == argv[2] || *end != '\0') {
				bt_usage();
				return BT_EXIT_USAGE;
			}
		}

		/* The request. */
		(void)snprintf(request, sizeof(request), "SCAN %lu", seconds);
		status = bt_ask(request, "SCAN");
		return status;
	}

	/* Anything else. */
	bt_usage();
	return BT_EXIT_USAGE;
}

/* Connects to bluetoothd's socket; returns the descriptor or -1. */
static int
bt_connect(
	void)
{
	struct sockaddr_un address;
	int descriptor;
	int status;

	/* The socket. */
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return -1;

	/* The daemon's address. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", BTD_SOCKET);
	status = connect(descriptor, (struct sockaddr *)&address, sizeof(address));
	if (status != 0) {
		(void)close(descriptor);
		return -1;
	}

	/* Succeeded: connected. */
	return descriptor;
}

/*
 * Sends one request, prints the answer's lines up to DONE, and the summary
 * line; returns the exit status.
 */
static int
bt_ask(
	const char *request,
	const char *summary)
{
	char line[BTD_LINE_MAX + 1024U];
	char state[64];
	FILE *answer;
	ssize_t written;
	char *got;
	unsigned devices;
	size_t length;
	int descriptor;
	int failed;
	int done;
	int same;

	/* The daemon. */
	descriptor = bt_connect();
	if (descriptor < 0) {
		(void)fprintf(stderr, "bt: bluetoothd is not running (%s)\n", strerror(errno));
		(void)printf("BT %s state=none\n", summary);
		return BT_EXIT_NO_DAEMON;
	}

	/* The request. */
	(void)snprintf(line, sizeof(line), "%s\n", request);
	length = strlen(line);
	written = write(descriptor, line, length);
	if (written != (ssize_t)length) {
		(void)close(descriptor);
		(void)fprintf(stderr, "bt: the request could not be sent\n");
		return BT_EXIT_NO_DAEMON;
	}

	/* The answer's lines, read as a stream. */
	answer = fdopen(descriptor, "r");
	if (answer == NULL) {
		(void)close(descriptor);
		return BT_EXIT_NO_DAEMON;
	}

	/* Each line up to DONE: printed, the state and the devices counted. */
	(void)snprintf(state, sizeof(state), "%s", "unknown");
	devices = 0U;
	failed = 0;
	done = 0;
	for (;;) {
		/* The next line; the stream's end before DONE is the daemon gone. */
		got = fgets(line, (int)sizeof(line), answer);
		if (got == NULL)
			break;

		/* The DONE ends the answer. */
		same = strcmp(line, "DONE\n");
		if (same == 0) {
			done = 1;
			break;
		}

		/* Any other line is the answer's, printed as it is. */
		(void)fputs(line, stdout);

		/* A STATE line names the state. */
		same = strncmp(line, "STATE ", 6U);
		if (same == 0)
			(void)sscanf(line + 6, "%63s", state);

		/* A DEVICE line counts. */
		same = strncmp(line, "DEVICE ", 7U);
		if (same == 0)
			devices++;

		/* An ERROR line fails the run. */
		same = strncmp(line, "ERROR ", 6U);
		if (same == 0)
			failed = 1;
	}

	/* The answer is read. */
	(void)fclose(answer);

	/* A daemon that went before DONE. */
	if (!done) {
		(void)fprintf(stderr, "bt: bluetoothd went before it answered\n");
		return BT_EXIT_NO_DAEMON;
	}

	/* The summary line the tests read. */
	same = strcmp(summary, "SHOW");
	if (same == 0)
		(void)printf("BT SHOW state=%s\n", state);
	else
		(void)printf("BT SCAN devices=%u\n", devices);

	/* An ERROR answer. */
	if (failed)
		return BT_EXIT_ERROR;

	/* Succeeded: answered. */
	return BT_EXIT_OK;
}

/* Prints how the command is used. */
static void
bt_usage(
	void)
{
	/* The three commands. */
	(void)fprintf(stderr, "usage: bt show | bt scan [SECONDS] | bt devices\n");
}
