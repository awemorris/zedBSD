/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD uuencode userland command.
 */

#include "userland/base/common/uucodec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Runs the uuencode command.
 */
int
main(
	int argc,
	char **argv)
{
	int base64;
	int index;
	int input;
	unsigned mode;
	struct stat status;

	base64 = 0;
	index = 1;
	input = STDIN_FILENO;
	mode = 0666;

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "-m") == 0) {
		base64 = 1;
		index++;
	}

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "--") == 0)
		index++;

	/* Validates the command-line arguments. */
	if (argc - index == 2) {
		input = open(argv[index], O_RDONLY | O_CLOEXEC);

		/* Validates the current input. */
		if (input < 0) {
			fprintf(stderr, "uuencode: %s: %s\n", argv[index],
				strerror(errno));

			/* Reports operation failure. */
			return 1;
		}

		/* Handles a failed fstat operation. */
		if (fstat(input, &status) == 0)
			mode = (unsigned)status.st_mode;
		index++;
	} else if (argc - index != 1) {
		fprintf(stderr, "usage: uuencode [-m] [file] decode_path\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Validates the command-line arguments. */
	if (uu_encode_fd(input, base64, mode, argv[index]) != 0) {
		fprintf(stderr, "uuencode: %s\n", strerror(errno));

		/* Validates the current input. */
		if (input != STDIN_FILENO)
			(void)close(input);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed close operation. */
	if (input != STDIN_FILENO && close(input) != 0) {
		fprintf(stderr, "uuencode: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}
