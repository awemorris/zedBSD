/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD mesg userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Runs the mesg command.
 */
int
main(
	int argc,
	char **argv)
{
	struct stat st;

	/* Handles the selected command-line operation. */
	if (argc > 2 ||
	    (argc == 2 && strcmp(argv[1], "y") && strcmp(argv[1], "n"))) {
		fprintf(stderr, "usage: mesg [y|n]\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the fstat condition. */
	if (fstat(0, &st)) {
		command_error("mesg", NULL);

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (argc == 1) {
		puts(st.st_mode & S_IWGRP ? "is y" : "is n");

		/* Returns the computed result. */
		return st.st_mode & S_IWGRP ? 0 : 1;
	}

	/* Handles the selected command-line operation. */
	if (fchmod(0, !strcmp(argv[1], "y") ? st.st_mode | S_IWGRP
					    : st.st_mode & ~S_IWGRP)) {
		command_error("mesg", NULL);

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}
