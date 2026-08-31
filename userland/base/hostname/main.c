/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD hostname userland command.
 */

#include "userland/base/common/command.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the hostname command.
 */
int
main(
	int argc,
	char **argv)
{
	char *dot;
	size_t length;
	char name[65];
	int short_name;

	short_name = 0;

	/* Handles the selected command-line operation. */
	if (argc > 1 && strcmp(argv[1], "-s") == 0) {
		short_name = 1;
		argc--;
		argv++;
	}

	/* Validates the command-line arguments. */
	if (argc == 1) {
		/* Handles a failed gethostname operation. */
		if (gethostname(name, sizeof(name)) != 0) {
			command_error("hostname", NULL);

			/* Reports operation failure. */
			return 1;
		}

		/* Handles a failed strchr operation. */
		if (short_name && (dot = strchr(name, '.')) != NULL)
			*dot = '\0';
		puts(name);

		/* Reports successful completion. */
		return 0;
	}

	/* Validates the command-line arguments. */
	if (argc == 2 && !short_name) {
				length = strlen(argv[1]);

		/* Validates the command-line arguments. */
		if (sethostname(argv[1], length) == 0)
			return 0;
		command_error("hostname", argv[1]);

		/* Reports operation failure. */
		return 1;
	}
	fprintf(stderr, "usage: hostname [-s] [name]\n");

	/* Reports operation failure. */
	return 2;
}
