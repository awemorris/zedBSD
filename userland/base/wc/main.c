/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD wc userland command.
 */

#include "userland/base/common/command.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int count_fd(int fd, const char *name, int lines, int words, int bytes);

/*
 * Runs the wc command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	const char *p;
	int fd;
	int lines, words, bytes, i, failed;

	/* Process each remaining command-line operand. */
	lines = 0;
	words = 0;
	bytes = 0;
	i = 1;
	failed = 0;
	for (; i < argc && argv[i][0] == '-'; i++) {
				p = argv[i] + 1;

		/* Checks the current pointer. */
		if (!*p)
			break;

		/* Continue while the operation condition remains true. */
		while (*p) {
			/* Checks the current pointer. */
			if (*p == 'l')
				lines = 1;
			else if (*p == 'w')
				words = 1;
			else if (*p == 'c')
				bytes = 1;
			else
				goto usage;
			p++;
		}
	}

	/* Handles the lines condition. */
	if (!lines && !words && !bytes)
		lines = words = bytes = 1;

	/* Validates the command-line arguments. */
	if (i == argc) {
		/* Computes the function result. */
		function_result = count_fd(STDIN_FILENO, NULL, lines, words, bytes) != 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining command-line operand. */
	for (; i < argc; i++) {
				fd = !strcmp(argv[i], "-") ? STDIN_FILENO
					       : open(argv[i], O_RDONLY);

		/* Checks the file descriptor. */
		if (fd < 0) {
			command_error("wc", argv[i]);
			failed = 1;
			continue;
		}

		/* Validates the command-line arguments. */
		if (count_fd(fd, argv[i], lines, words, bytes))
			failed = 1;

		/* Checks the file descriptor. */
		if (fd != STDIN_FILENO)
			close(fd);
	}

	/* Returns the computed result. */
	return failed;
usage:
	fprintf(stderr, "usage: wc [-clw] [file...]\n");

	/* Reports operation failure. */
	return 1;
}

/* Supports the count fd operation. */
static int
count_fd(
	int fd,
	const char *name,
	int lines,
	int words,
	int bytes)
{
	ssize_t n;
	size_t i;
	unsigned char buffer[4096];
	unsigned long long l, w, b;
	int inword;

	/* Continue until the operation reaches a terminal state. */
	l = 0;
	w = 0;
	b = 0;
	inword = 0;
	for (;;) {

		n = read(fd, buffer, sizeof(buffer));

		/* Checks the current item count. */
		if (n < 0) {
			command_error("wc", name);

			/* Reports operation failure. */
			return -1;
		}

		/* Checks the current item count. */
		if (!n)
			break;
		b += (unsigned long long)n;

		/* Process each remaining element. */
		for (i = 0; i < (size_t)n; i++) {
			/* Handles the buffer condition. */
			if (buffer[i] == '\n')
				l++;

			/* Handles the isspace condition. */
			if (isspace(buffer[i]))
				inword = 0;
			else if (!inword) {
				inword = 1;
				w++;
			}
		}
	}

	/* Handles the lines condition. */
	if (lines)
		printf("%7llu", l);

	/* Handles the words condition. */
	if (words)
		printf("%7llu", w);

	/* Handles the bytes condition. */
	if (bytes)
		printf("%7llu", b);

	/* Validates the current name. */
	if (name)
		printf(" %s", name);
	putchar('\n');

	/* Reports successful completion. */
	return 0;
}
