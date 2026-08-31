/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD csplit userland command.
 */

#include "userland/base/common/command.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int openpart(unsigned n, char *name);

/*
 * Runs the csplit command.
 */
int
main(
	int argc,
	char **argv)
{
	FILE *f;
	char *l, name[16];
	size_t cap;
	long n;
	unsigned long long target, line, part, count;
	int out;

	l = NULL;
	cap = 0;
	line = 1;
	part = 0;
	count = 0;

	/* Validates the command-line arguments. */
	if (argc != 3 || command_parse_ull(argv[2], &target) || target < 1) {
		fprintf(stderr, "usage: csplit file line-number\n");

		/* Reports operation failure. */
		return 2;
	}
	f = !strcmp(argv[1], "-") ? stdin : fopen(argv[1], "r");

	/* Checks the current file state. */
	if (!f) {
		command_error("csplit", argv[1]);

		/* Reports operation failure. */
		return 1;
	}
	out = openpart(part++, name);

	/* Handles the out condition. */
	if (out < 0)
		return 1;

	/* Continue while the operation condition remains true. */
	while ((n = command_read_line(f, &l, &cap)) > 0) {
		/* Handles the line condition. */
		if (line == target) {
			printf("%llu\n", count);
			close(out);
			out = openpart(part++, name);
			count = 0;

			/* Handles the out condition. */
			if (out < 0)
				return 1;
		}

		/* Handles the command write all condition. */
		if (command_write_all(out, l, (size_t)n))
			return 1;
		count += (unsigned long long)n;
		line++;
	}
	printf("%llu\n", count);
	close(out);
	free(l);

	/* Checks the current file state. */
	if (f != stdin)
		fclose(f);

	/* Returns the computed result. */
	return n < 0;
}

/* Supports the openpart operation. */
static int
openpart(
	unsigned n,
	char *name)
{
	int function_result;

	snprintf(name, 16, "xx%02u", n);

	/* Obtains the open result. */
	function_result = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);

	/* Returns the computed result. */
	return function_result;
}
