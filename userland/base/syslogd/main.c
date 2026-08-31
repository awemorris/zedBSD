/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD syslogd userland command.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_SOCKET "/run/log"
#define LOG_PATH "/var/log/messages"
#define BOOT_LOG_PATH "/run/dmesg.boot"

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t reopening;

static void save_boot_log(void);
static int write_all(int descriptor, const void *buffer, size_t length);
static int open_output(void);
static void handle_signal(int number);

/*
 * Runs the syslogd command.
 */
int
main(
	int argc,
	char **argv)
{
	int replacement;
	ssize_t length;
	struct sockaddr_un address;
	char message[2048];
	int socket_descriptor, output;

	(void)argv;

	/* Validates the command-line arguments. */
	if (argc != 1) {
		fprintf(stderr, "usage: syslogd\n");

		/* Reports operation failure. */
		return 2;
	}
	(void)signal(SIGHUP, handle_signal);
	(void)signal(SIGTERM, handle_signal);
	(void)signal(SIGINT, handle_signal);
	save_boot_log();
	output = open_output();

	/* Handles the output condition. */
	if (output < 0) {
		fprintf(stderr, "syslogd: %s: %s\n", LOG_PATH, strerror(errno));

		/* Reports operation failure. */
		return 1;
	}
	socket_descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);

	/* Handles the socket descriptor condition. */
	if (socket_descriptor < 0)
		return 1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, LOG_SOCKET);
	(void)unlink(LOG_SOCKET);

	/* Handles a failed bind operation. */
	if (bind(socket_descriptor, (struct sockaddr *)&address,
		 sizeof(address)) != 0 ||
	    chmod(LOG_SOCKET, 0666) != 0) {
		fprintf(stderr, "syslogd: %s: %s\n", LOG_SOCKET,
			strerror(errno));

		/* Reports operation failure. */
		return 1;
	}
	while (!stopping) {
		/* Handles the reopening condition. */
		if (reopening) {
			replacement = open_output();
			reopening = 0;

			/* Handles the replacement condition. */
			if (replacement >= 0) {
				close(output);
				output = replacement;
			}
		}
		length =
		    recv(socket_descriptor, message, sizeof(message) - 1, 0);

		/* Handles the reported system error. */
		if (length < 0 && errno == EINTR)
			continue;

		/* Checks the current data length. */
		if (length < 0)
			break;
		message[length] = '\0';

		/* Handles a failed memchr operation. */
		if (memchr(message, '\0', (size_t)length) != NULL)
			continue;

		/* Handles a failed write all operation. */
		if (write_all(output, message, (size_t)length) != 0 ||
		    write_all(output, "\n", 1) != 0) {
			fprintf(stderr, "syslogd: log write failed: %s\n",
				strerror(errno));
		}
	}
	(void)fsync(output);
	close(output);
	close(socket_descriptor);
	unlink(LOG_SOCKET);

	/* Reports successful completion. */
	return 0;
}

/* Supports the save boot log operation. */
static void
save_boot_log(
	void)
{
	char buffer[65536];
	size_t length;
	int descriptor;

	length = sizeof(buffer);

	/* Handles a failed sysctlbyname operation. */
	if (sysctlbyname("kern.msgbuf", buffer, &length, NULL, 0) != 0)
		return;
	descriptor = open(BOOT_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	/* Checks the file descriptor. */
	if (descriptor >= 0) {
		(void)write_all(descriptor, buffer, length);
		(void)fsync(descriptor);
		(void)close(descriptor);
	}
}

/* Supports the write all operation. */
static int
write_all(
	int descriptor,
	const void *buffer,
	size_t length)
{
	ssize_t written;
	const char *cursor;

	/* Process each remaining element. */
	cursor = buffer;
	while (length != 0) {
		written = write(descriptor, cursor, length);

		/* Handles the reported system error. */
		if (written < 0 && errno == EINTR)
			continue;

		/* Handles the written condition. */
		if (written <= 0)
			return -1;
		cursor += written;
		length -= (size_t)written;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the open output operation. */
static int
open_output(
	void)
{
	int function_result;

	/* Obtains the open result. */
	function_result = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0640);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the handle signal operation. */
static void
handle_signal(
	int number)
{
	/* Handles the number condition. */
	if (number == SIGHUP)
		reopening = 1;
	else
		stopping = 1;
}
