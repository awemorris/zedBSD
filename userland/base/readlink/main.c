/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD readlink userland command.
 */

#include "userland/base/common/command.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the readlink command.
 */
int
main(
	int argc,
	char **argv)
{
	char buffer[PATH_MAX + 1U];
	int newline;
	int index;
	ssize_t length;

	newline = 1;
	index = 1;

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "-n")) {
		newline = 0;
		index++;
	}

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "--"))
		index++;

	/* Validates the command-line arguments. */
	if (argc - index != 1) {
		fprintf(stderr, "usage: readlink [-n] file\n");

		/* Reports operation failure. */
		return 2;
	}
	length = readlink(argv[index], buffer, sizeof(buffer));

	/* Handles a failed command write all operation. */
	if (length < 0 ||
	    command_write_all(STDOUT_FILENO, buffer, (size_t)length) != 0 ||
	    (newline && command_write_all(STDOUT_FILENO, "\n", 1) != 0)) {
		command_error("readlink", argv[index]);

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}
