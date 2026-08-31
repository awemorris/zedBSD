/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD link userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the link command.
 */
int
main(
	int argc,
	char **argv)
{
	/* Handles the selected command-line operation. */
	if (argc > 1 && strcmp(argv[1], "--") == 0) {
		argc--;
		argv++;
	}

	/* Validates the command-line arguments. */
	if (argc != 3) {
		fprintf(stderr, "usage: link source target\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (link(argv[1], argv[2])) {
		command_error("link", argv[2]);

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}
