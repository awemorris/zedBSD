/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD mkfifo userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Runs the mkfifo command.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned value;
	mode_t mode;
	int i, failed;

	mode = 0666;
	i = 1;
	failed = 0;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-m") && ++i < argc) {
		/* Validates the command-line arguments. */
		if (command_parse_mode(argv[i], &value))
			goto usage;
		mode = (mode_t)value;
		i++;
	}

	/* Validates the command-line arguments. */
	if (i == argc)
		goto usage;

	/* Process each remaining command-line operand. */
	for (; i < argc; i++) {
		/* Validates the command-line arguments. */
		if (mkfifo(argv[i], mode)) {
			command_error("mkfifo", argv[i]);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
usage:
	fprintf(stderr, "usage: mkfifo [-m mode] file...\n");

	/* Reports operation failure. */
	return 1;
}
