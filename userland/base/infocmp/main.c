/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD infocmp userland command.
 */

#include "userland/base/common/terminfo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the infocmp command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *directory;
	const char *name;
	struct terminfo terminal;
	int option;

	directory = getenv("TERMINFO");
	name = NULL;

	/* Parse each command-line option. */
	while ((option = getopt(argc, argv, "A:1")) != -1) {
		/* Handles the option condition. */
		if (option == 'A')
			directory = optarg;
		else if (option != '1')
			goto usage;
	}

	/* Validates the command-line arguments. */
	if (optind < argc)
		name = argv[optind++];

	/* Validates the command-line arguments. */
	if (optind != argc)
		goto usage;

	/* Handles the name availability. */
	if (name == NULL)
		name = getenv("TERM");

	/* Handles the name availability. */
	if (name == NULL || *name == '\0') {
		fprintf(stderr, "infocmp: TERM is not set\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Handles a failed terminfo load operation. */
	if (terminfo_load(&terminal, name, directory) != 0) {
		fprintf(stderr, "infocmp: %s: %s\n", name, strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed terminfo write source operation. */
	if (terminfo_write_source(stdout, &terminal, name) != 0) {
		fprintf(stderr, "infocmp: stdout: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;

usage:
	fprintf(stderr, "usage: infocmp [-1] [-A directory] [terminal]\n");

	/* Reports operation failure. */
	return 2;
}
