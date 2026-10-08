/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD dmesg userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>
#define DMESG_LIMIT (1024U * 1024U)

/*
 * Room beyond the size the kernel last reported: the log may grow between
 * the size's query and the copy (a driver logging while dmesg runs), and a
 * copy that does not fit fails with ENOMEM (T1-426, ws143-p002).
 */
#define DMESG_SLACK (64U * 1024U)

/* How many times a copy that found the log grown is tried again. */
#define DMESG_TRIES 4

/*
 * Runs the dmesg command.
 */
int
main(
	int argc,
	char **argv)
{
	char *buffer;
	size_t size, capacity;
	int tries;

	(void)argv;

	size = 0;
	tries = 0;

	/* Validates the command-line arguments. */
	if (argc != 1) {
		fprintf(stderr, "usage: dmesg\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the sysctlbyname condition. */
	if (sysctlbyname("kern.msgbuf", NULL, &size, NULL, 0)) {
		command_error("dmesg", NULL);

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the current data size. */
	if (size > DMESG_LIMIT) {
		errno = EOVERFLOW;
		command_error("dmesg", NULL);

		/* Reports operation failure. */
		return 1;
	}
	do {
		/* Room for the log as it was and what it may gain meanwhile. */
		capacity = size + DMESG_SLACK;
		if (capacity > DMESG_LIMIT)
			capacity = DMESG_LIMIT;
		buffer = malloc(capacity ? capacity : 1U);

		/* Handles the buffer condition. */
		if (!buffer) {
			command_error("dmesg", NULL);

			/* Reports operation failure. */
			return 1;
		}
		size = capacity;

		/* Handles a failed sysctlbyname operation. */
		if (sysctlbyname("kern.msgbuf", buffer, &size, NULL, 0) == 0)
			break;
		free(buffer);

		/* Handles the reported system error. */
		if (errno != ENOMEM || ++tries >= DMESG_TRIES) {
			command_error("dmesg", NULL);

			/* Reports operation failure. */
			return 1;
		}

		/* Handles the sysctlbyname condition. */
		if (sysctlbyname("kern.msgbuf", NULL, &size, NULL, 0)) {
			command_error("dmesg", NULL);

			/* Reports operation failure. */
			return 1;
		}

		/* Checks the current data size. */
		if (size > DMESG_LIMIT) {
			errno = EOVERFLOW;
			command_error("dmesg", NULL);

			/* Reports operation failure. */
			return 1;
		}
	} while (1);

	/* Checks the current data size. */
	if (size && command_write_all(STDOUT_FILENO, buffer, size)) {
		command_error("dmesg", NULL);
		free(buffer);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed command write all operation. */
	if (size && buffer[size - 1] != '\n' &&
	    command_write_all(STDOUT_FILENO, "\n", 1)) {
		command_error("dmesg", NULL);
		free(buffer);

		/* Reports operation failure. */
		return 1;
	}
	free(buffer);

	/* Reports successful completion. */
	return 0;
}
