/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD ln userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the ln command.
 */
int
main(
	int argc,
	char **argv)
{
	int symbolic, i;

	symbolic = 0;
	i = 1;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-s")) {
		symbolic = 1;
		i++;
	}

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "--"))
		i++;

	/* Validates the command-line arguments. */
	if (argc - i != 2) {
		fprintf(stderr, "usage: ln [-s] source target\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if ((symbolic ? symlink(argv[i], argv[i + 1])
		      : link(argv[i], argv[i + 1])) != 0) {
		command_error("ln", argv[i + 1]);

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}
