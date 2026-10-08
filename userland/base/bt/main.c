/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Bluetooth command (ws143-p003 and p004): asks bluetoothd on its
 * socket.
 *
 *   bt show                     the state and the controller
 *   bt scan [SECONDS]           a scan (8 seconds unless given), and the
 *                               devices found
 *   bt devices                  the devices of the last scan
 *   bt pair ADDRESS [TYPE]      pairs a device (TYPE bredr, the default,
 *                               le-public or le-random); the daemon's
 *                               questions are asked on the terminal
 *   bt forget ADDRESS [TYPE]    forgets a bond
 *   bt bonds                    the bonds
 *   bt agent                    answers the pairings' questions on the
 *                               terminal until it is ended
 *   bt power on|off             Bluetooth on or off for the user (ws143-p006)
 *   bt connect ADDRESS [TYPE]   connects a bonded HID device (ws143-p005)
 *   bt disconnect ADDRESS [TYPE]
 *                               disconnects it (it does not come back by
 *                               itself until connected again)
 *   bt status                   the HID devices and their state
 *
 * Changing things (scan, pair, forget, agent, connect, disconnect) is for root, the seat's user
 * and wheel.  It prints the daemon's lines as they are, then one line the
 * tests read: "BT SHOW state=WORD", "BT SCAN devices=N", "BT PAIR
 * result=paired|error", "BT FORGET result=ok|error", "BT BONDS bonds=N" or
 * "BT POWER result=on|off|error", "BT CONNECT result=connected|error
 * input=/dev/input/eventN|-", "BT DISCONNECT result=ok|error" or "BT STATUS
 * devices=N open=M".
 * A question ("CONFIRM NUMBER", "CONSENT") is answered from standard input
 * (y for yes, anything else or its end for no).  It exits with 0, 1 when
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
static void bt_question(int descriptor, const char *line);
static int bt_device_request(const char *verb, int argc, char **argv, char *request, size_t size);
static void bt_usage(void);

/*
 * Runs one request.
 */
int
main(
	int argc,
	char **argv)
{
	char request[96];
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

	/* bt bonds. */
	same = strcmp(argv[1], "bonds");
	if (same == 0 && argc == 2) {
		status = bt_ask("BONDS", "BONDS");
		return status;
	}

	/* bt agent: answers until it is ended. */
	same = strcmp(argv[1], "agent");
	if (same == 0 && argc == 2) {
		status = bt_ask("AGENT", "AGENT");
		return status;
	}

	/* bt pair ADDRESS [TYPE]. */
	same = strcmp(argv[1], "pair");
	if (same == 0) {
		status = bt_device_request("PAIR", argc, argv, request, sizeof(request));
		if (status != 0) {
			bt_usage();
			return BT_EXIT_USAGE;
		}

		/* The pairing, its questions asked here. */
		status = bt_ask(request, "PAIR");
		return status;
	}

	/* bt forget ADDRESS [TYPE]. */
	same = strcmp(argv[1], "forget");
	if (same == 0) {
		status = bt_device_request("FORGET", argc, argv, request, sizeof(request));
		if (status != 0) {
			bt_usage();
			return BT_EXIT_USAGE;
		}

		/* The bond's removal. */
		status = bt_ask(request, "FORGET");
		return status;
	}

	/* bt connect ADDRESS [TYPE] (ws143-p005). */
	same = strcmp(argv[1], "connect");
	if (same == 0) {
		status = bt_device_request("CONNECT", argc, argv, request, sizeof(request));
		if (status != 0) {
			bt_usage();
			return BT_EXIT_USAGE;
		}

		/* The connection, up to the input device. */
		status = bt_ask(request, "CONNECT");
		return status;
	}

	/* bt disconnect ADDRESS [TYPE] (ws143-p005). */
	same = strcmp(argv[1], "disconnect");
	if (same == 0) {
		status = bt_device_request("DISCONNECT", argc, argv, request, sizeof(request));
		if (status != 0) {
			bt_usage();
			return BT_EXIT_USAGE;
		}

		/* The disconnection. */
		status = bt_ask(request, "DISCONNECT");
		return status;
	}

	/* bt status (ws143-p005). */
	same = strcmp(argv[1], "status");
	if (same == 0 && argc == 2) {
		status = bt_ask("STATUS", "STATUS");
		return status;
	}

	/* bt power on|off (ws143-p006). */
	same = strcmp(argv[1], "power");
	if (same == 0 && argc == 3) {
		(void)snprintf(request, sizeof(request), "POWER %s", argv[2]);
		status = bt_ask(request, "POWER");
		return status;
	}

	/* Anything else. */
	bt_usage();
	return BT_EXIT_USAGE;
}

/* Writes "VERB ADDRESS TYPE" for bt pair, forget, connect and disconnect (TYPE bredr unless given); returns 0, or -1 for a misuse. */
static int
bt_device_request(
	const char *verb,
	int argc,
	char **argv,
	char *request,
	size_t size)
{
	const char *type;
	size_t length;

	/* An address, and maybe a type. */
	if (argc != 3 && argc != 4)
		return -1;
	length = strlen(argv[2]);
	if (length != 17U)
		return -1;
	type = "bredr";
	if (argc == 4)
		type = argv[3];

	/* The request (the daemon checks the address and the type). */
	(void)snprintf(request, size, "%s %s %s", verb, argv[2], type);
	return 0;
}

/*
 * Asks the user a pairing's question on the terminal and sends the answer:
 * y is YES, anything else (or no input) NO.
 */
static void
bt_question(
	int descriptor,
	const char *line)
{
	char answer[32];
	const char *reply;
	ssize_t written;
	char *got;
	int same;

	/* The question in words. */
	same = strncmp(line, "CONSENT", 7U);
	if (same == 0)
		(void)printf("Pair with the device (it shows no number)? [y/N] ");
	else
		(void)printf("Does the device show %s? [y/N] ", line + 8);
	(void)fflush(stdout);

	/* The answer from standard input. */
	reply = "NO\n";
	got = fgets(answer, (int)sizeof(answer), stdin);
	if (got != NULL && (answer[0] == 'y' || answer[0] == 'Y'))
		reply = "YES\n";
	(void)printf("\n");

	/* Sent (a daemon that went is seen on the next read). */
	written = write(descriptor, reply, strlen(reply));
	(void)written;
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
	char input[64];
	const char *word;
	const char *found;
	FILE *answer;
	ssize_t written;
	char *got;
	unsigned devices;
	unsigned bonds;
	unsigned hids;
	unsigned open;
	size_t length;
	int descriptor;
	int paired;
	int connected;
	int failed;
	int done;
	int same;
	int off;

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
	bonds = 0U;
	hids = 0U;
	open = 0U;
	(void)snprintf(input, sizeof(input), "%s", "-");
	paired = 0;
	connected = 0;
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

		/* A question is asked on the terminal. */
		same = strncmp(line, "CONFIRM ", 8U);
		if (same == 0) {
			bt_question(descriptor, line);
			continue;
		}

		/* An agreement, likewise. */
		same = strncmp(line, "CONSENT", 7U);
		if (same == 0) {
			bt_question(descriptor, line);
			continue;
		}

		/* A passkey to type on the device. */
		same = strncmp(line, "PASSKEY ", 8U);
		if (same == 0) {
			(void)printf("Type %.6s on the device, then Enter there.\n", line + 8);
			(void)fflush(stdout);
			continue;
		}

		/* A PAIRED line ends the pairing well. */
		same = strncmp(line, "PAIRED ", 7U);
		if (same == 0)
			paired = 1;

		/* A CONNECTED line ends a connection well, and names its input device. */
		same = strncmp(line, "CONNECTED ", 10U);
		if (same == 0) {
			connected = 1;
			found = strstr(line, " input=");
			if (found != NULL)
				(void)sscanf(found + 7, "%63s", input);
		}

		/* A HID line counts, and so does an open one. */
		same = strncmp(line, "HID ", 4U);
		if (same == 0) {
			hids++;
			found = strstr(line, " state=open ");
			if (found != NULL)
				open++;
		}

		/* A BOND line counts. */
		same = strncmp(line, "BOND ", 5U);
		if (same == 0)
			bonds++;

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

	/* The agent goes on after its DONE: it answers every question until the daemon or the user ends it. */
	same = strcmp(summary, "AGENT");
	if (same == 0 && done && !failed) {
		(void)fflush(stdout);
		for (;;) {
			/* The next question. */
			got = fgets(line, (int)sizeof(line), answer);
			if (got == NULL)
				break;
			(void)fputs(line, stdout);
			same = strncmp(line, "CONFIRM ", 8U);
			if (same == 0)
				bt_question(descriptor, line);
			same = strncmp(line, "CONSENT", 7U);
			if (same == 0)
				bt_question(descriptor, line);
			(void)fflush(stdout);
		}
	}

	/* The answer is read. */
	(void)fclose(answer);

	/* A daemon that went before DONE. */
	if (!done) {
		(void)fprintf(stderr, "bt: bluetoothd went before it answered\n");
		return BT_EXIT_NO_DAEMON;
	}

	/* The summary line the tests read, for each request. */
	same = strcmp(summary, "SHOW");
	if (same == 0)
		(void)printf("BT SHOW state=%s\n", state);
	same = strcmp(summary, "SCAN");
	if (same == 0)
		(void)printf("BT SCAN devices=%u\n", devices);
	same = strcmp(summary, "PAIR");
	if (same == 0 && paired)
		(void)printf("BT PAIR result=paired\n");
	if (same == 0 && !paired)
		(void)printf("BT PAIR result=error\n");
	same = strcmp(summary, "FORGET");
	if (same == 0 && !failed)
		(void)printf("BT FORGET result=ok\n");
	if (same == 0 && failed)
		(void)printf("BT FORGET result=error\n");
	same = strcmp(summary, "BONDS");
	if (same == 0)
		(void)printf("BT BONDS bonds=%u\n", bonds);
	same = strcmp(summary, "CONNECT");
	if (same == 0 && connected)
		(void)printf("BT CONNECT result=connected input=%s\n", input);
	if (same == 0 && !connected)
		(void)printf("BT CONNECT result=error input=-\n");
	same = strcmp(summary, "DISCONNECT");
	if (same == 0 && !failed)
		(void)printf("BT DISCONNECT result=ok\n");
	if (same == 0 && failed)
		(void)printf("BT DISCONNECT result=error\n");
	same = strcmp(summary, "STATUS");
	if (same == 0)
		(void)printf("BT STATUS devices=%u open=%u\n", hids, open);
	same = strcmp(summary, "POWER");
	if (same == 0 && failed)
		(void)printf("BT POWER result=error\n");
	if (same == 0 && !failed) {
		/* The word the request asked for. */
		word = "on";
		off = strcmp(request, "POWER off");
		if (off == 0)
			word = "off";
		(void)printf("BT POWER result=%s\n", word);
	}

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
	/* The commands. */
	(void)fprintf(stderr,
		      "usage: bt show | bt scan [SECONDS] | bt devices | bt pair ADDRESS [bredr|le-public|le-random] |\n"
		      "       bt forget ADDRESS [TYPE] | bt bonds | bt agent | bt power on|off |\n"
		      "       bt connect ADDRESS [TYPE] | bt disconnect ADDRESS [TYPE] | bt status\n");
}
