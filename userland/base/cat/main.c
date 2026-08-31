/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD cat userland command.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the cat command.
 */
int
main(
	int argc,
	char **argv)
{
	int descriptor;
	int index, failed;

	index = 1;
	failed = 0;

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "-u"))
		index++;

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "--"))
		index++;

	/* Validates the command-line arguments. */
	if (index == argc) {
		/* Handles a failed command copy fd operation. */
		if (command_copy_fd(STDIN_FILENO, STDOUT_FILENO) != 0) {
			command_error("cat", NULL);

			/* Reports operation failure. */
			return 1;
		}

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining command-line operand. */
	for (; index < argc; index++) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[index], "-"))
			descriptor = STDIN_FILENO;
		else {
			descriptor = open(argv[index], O_RDONLY);

			/* Checks the file descriptor. */
			if (descriptor < 0) {
				command_error("cat", argv[index]);
				failed = 1;
				continue;
			}
		}

		/* Handles a failed command copy fd operation. */
		if (command_copy_fd(descriptor, STDOUT_FILENO) != 0) {
			command_error("cat", argv[index]);
			failed = 1;
		}

		/* Handles a failed close operation. */
		if (descriptor != STDIN_FILENO && close(descriptor) != 0) {
			command_error("cat", argv[index]);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
}
