/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD uudecode userland command.
 */

#include "userland/base/common/uucodec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the uudecode command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *output;
	const char *input_path;
	int input;
	int index;

	output = NULL;
	input_path = NULL;
	input = STDIN_FILENO;
	index = 1;

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "-o") == 0) {
		/* Validates the command-line arguments. */
		if (++index >= argc)
			goto usage;
		output = argv[index++];
	}

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "--") == 0)
		index++;

	/* Validates the command-line arguments. */
	if (index < argc)
		input_path = argv[index++];

	/* Validates the command-line arguments. */
	if (index != argc)
		goto usage;

	/* Handles the input path availability. */
	if (input_path != NULL && strcmp(input_path, "-") != 0) {
		input = open(input_path, O_RDONLY | O_CLOEXEC);

		/* Validates the current input. */
		if (input < 0) {
			fprintf(stderr, "uudecode: %s: %s\n", input_path,
				strerror(errno));

			/* Reports operation failure. */
			return 1;
		}
	}

	/* Handles a failed uu decode fd operation. */
	if (uu_decode_fd(input, output) != 0) {
		fprintf(stderr, "uudecode: %s\n", strerror(errno));

		/* Validates the current input. */
		if (input != STDIN_FILENO)
			(void)close(input);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed close operation. */
	if (input != STDIN_FILENO && close(input) != 0) {
		fprintf(stderr, "uudecode: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;

usage:
	fprintf(stderr, "usage: uudecode [-o output] [file]\n");

	/* Reports operation failure. */
	return 2;
}
