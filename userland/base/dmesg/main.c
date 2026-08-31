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
		capacity = size;
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
		if (errno != ENOMEM || ++tries > 1) {
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
