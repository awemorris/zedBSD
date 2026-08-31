/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD od userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int dump(int fd, const char *name);

/*
 * Runs the od command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int fd;
	int status, i;

	status = 0;

	/* Validates the command-line arguments. */
	if (argc == 1) {
		/* Obtains the dump result. */
		function_result = dump(STDIN_FILENO, "standard input");

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining command-line operand. */
	for (i = 1; i < argc; ++i) {
				fd = !strcmp(argv[i], "-") ? STDIN_FILENO
					       : open(argv[i], O_RDONLY);

		/* Checks the file descriptor. */
		if (fd < 0) {
			command_error("od", argv[i]);
			status = 1;
			continue;
		}
		status |= dump(fd, argv[i]);

		/* Handles a failed close operation. */
		if (fd != STDIN_FILENO && close(fd) != 0) {
			command_error("od", argv[i]);
			status = 1;
		}
	}

	/* Returns the computed result. */
	return status;
}

/* Supports the dump operation. */
static int
dump(
	int fd,
	const char *name)
{
	ssize_t n;
	size_t i;
	unsigned char b[16];
	unsigned long long off;
	int failed;

	/* Continue until the operation reaches a terminal state. */
	off = 0;
	failed = 0;
	for (;;) {

		n = read(fd, b, sizeof(b));

		/* Checks the current item count. */
		if (n < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;
			command_error("od", name);

			/* Reports operation failure. */
			return 1;
		}

		/* Checks the current item count. */
		if (n == 0)
			break;
		printf("%07llo", off);

		/* Process each remaining element. */
		for (i = 0; i < (size_t)n; ++i)
			printf(" %03o", b[i]);
		putchar('\n');
		off += (unsigned long long)n;
	}
	printf("%07llo\n", off);

	/* Returns the computed result. */
	return failed;
}
