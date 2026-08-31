/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD rmdir userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the rmdir command.
 */
int
main(
	int argc,
	char **argv)
{
	char *slash;
	char path[1024];
	size_t n;
	int parents, i, failed;

	parents = 0;
	i = 1;
	failed = 0;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-p")) {
		parents = 1;
		i++;
	}

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "--"))
		i++;

	/* Validates the command-line arguments. */
	if (i == argc) {
		fprintf(stderr, "usage: rmdir [-p] directory...\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (; i < argc; i++) {

				n = strlen(argv[i]);

		/* Checks the current item count. */
		if (n >= sizeof(path)) {
			command_error("rmdir", argv[i]);
			failed = 1;
			continue;
		}
		memcpy(path, argv[i], n + 1);

		/* Continue until the operation reaches a terminal state. */
		for (;;) {
			/* Continue while the operation condition remains true. */
			while (n > 1 && path[n - 1] == '/')
				path[--n] = '\0';

			/* Handles a failed rmdir operation. */
			if (rmdir(path) != 0) {
				command_error("rmdir", path);
				failed = 1;
				break;
			}

			/* Handles a failed strrchr operation. */
			if (!parents || !(slash = strrchr(path, '/')))
				break;

			/* Continue while the operation condition remains true. */
			while (slash > path && slash[-1] == '/')
				slash--;

			/* Handles the slash condition. */
			if (slash == path)
				break;
			*slash = '\0';
			n = (size_t)(slash - path);
		}
	}

	/* Returns the computed result. */
	return failed;
}
