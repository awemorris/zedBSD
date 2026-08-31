/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD truncate userland command.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Runs the truncate command.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned long long value;
	int index, failed;

	failed = 0;

	/* Handles the selected command-line operation. */
	if (argc < 4 || strcmp(argv[1], "-s") != 0 ||
	    command_parse_ull(argv[2], &value) != 0 || (off_t)value < 0 ||
	    (unsigned long long)(off_t)value != value) {
		fprintf(stderr, "usage: truncate -s size file...\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (index = 3; index < argc; index++) {
		/* Validates the command-line arguments. */
		if (truncate(argv[index], (off_t)value) != 0) {
			command_error("truncate", argv[index]);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
}
