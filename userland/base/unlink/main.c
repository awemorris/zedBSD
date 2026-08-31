/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD unlink userland command.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the unlink command.
 */
int
main(
	int argc,
	char **argv)
{
	int index;

	index = 1;

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "--"))
		index++;

	/* Validates the command-line arguments. */
	if (index + 1 != argc) {
		fprintf(stderr, "usage: unlink file\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (unlink(argv[index]) != 0) {
		command_error("unlink", argv[index]);

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}
