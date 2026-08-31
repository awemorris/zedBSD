/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD strings userland command.
 */

#include "userland/base/common/command.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int scan(int fd, const char *name, unsigned minimum);

/*
 * Runs the strings command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	unsigned long long v;
	int fd;
	unsigned minimum;
	int i, failed;

	minimum = 4;
	i = 1;
	failed = 0;

	/* Handles the selected command-line operation. */
	if (i + 1 < argc && !strcmp(argv[i], "-n")) {
		/* Validates the command-line arguments. */
		if (command_parse_ull(argv[i + 1], &v) || v > 4096)
			goto usage;
		minimum = (unsigned)v;
		i += 2;
	}

	/* Validates the command-line arguments. */
	if (i == argc) {
		/* Obtains the scan result. */
		function_result = scan(0, NULL, minimum);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining command-line operand. */
	for (; i < argc; i++) {
				fd = open(argv[i], O_RDONLY);

		/* Checks the file descriptor. */
		if (fd < 0) {
			command_error("strings", argv[i]);
			failed = 1;
			continue;
		}
		failed |= scan(fd, argv[i], minimum);
		close(fd);
	}

	/* Returns the computed result. */
	return failed;
usage:
	fprintf(stderr, "usage: strings [-n length] [file...]\n");

	/* Reports operation failure. */
	return 1;
}

/* Supports the scan operation. */
static int
scan(
	int fd,
	const char *name,
	unsigned minimum)
{
	unsigned char c;
	ssize_t n;
	ssize_t i_index_for;
	unsigned char in[4096], out[4096];
	size_t used;

	/* Continue until the operation reaches a terminal state. */
	used = 0;
	for (;;) {

		n = read(fd, in, sizeof(in));

		/* Checks the current item count. */
		if (n < 0) {
			command_error("strings", name);

			/* Reports operation failure. */
			return 1;
		}

		/* Process each remaining element. */
		for (i_index_for = 0; i_index_for < n; i_index_for++) {
						c = in[i_index_for];

			/* Classifies the current input character. */
			if (c >= 0x20 && c <= 0x7e) {
				/* Checks the current capacity usage. */
				if (used < sizeof(out))
					out[used++] = c;
			} else {
				/* Checks the current capacity usage. */
				if (used >= minimum) {
					command_write_all(1, out, used);
					command_write_all(1, "\n", 1);
				}
				used = 0;
			}
		}

		/* Checks the current item count. */
		if (!n)
			break;
	}

	/* Checks the current capacity usage. */
	if (used >= minimum) {
		command_write_all(1, out, used);
		command_write_all(1, "\n", 1);
	}

	/* Reports successful completion. */
	return 0;
}
