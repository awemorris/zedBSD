/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD rm userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the rm command.
 */
int
main(
	int argc,
	char **argv)
{
	int force, i, failed;

	/* Process each remaining command-line operand. */
	force = 0;
	i = 1;
	failed = 0;
	for (; i < argc && argv[i][0] == '-'; i++) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "-f"))
			force = 1;
		else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-R")) {
			fprintf(stderr,
				"rm: recursive removal is not yet available\n");

			/* Reports operation failure. */
			return 2;
		} else {
			fprintf(stderr, "usage: rm [-f] file...\n");

			/* Reports operation failure. */
			return 1;
		}
	}

	/* Validates the command-line arguments. */
	if (i == argc)
		return force ? 0 : 1;

	/* Process each remaining command-line operand. */
	for (; i < argc; i++)

		/* Validates the command-line arguments. */
		if (unlink(argv[i])) {
			/* Handles the reported system error. */
			if (force && errno == ENOENT)
				continue;
			command_error("rm", argv[i]);
			failed = 1;
		}

	/* Returns the computed result. */
	return failed;
}
