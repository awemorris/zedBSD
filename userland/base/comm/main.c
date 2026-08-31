/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD comm userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void out(int col, int s1, int s2, int s3, const char *p, long n);

/*
 * Runs the comm command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	const char *p;
	size_t n;
	int c;
	int s1, s2, s3, i;
	FILE *a, *b;
	char *x, *y;
	size_t cx, cy;
	long nx, ny;

	/* Process each remaining command-line operand. */
	s1 = 0;
	s2 = 0;
	s3 = 0;
	i = 1;
	x = NULL;
	y = NULL;
	cx = 0;
	cy = 0;
	for (; i < argc && argv[i][0] == '-'; ++i) {
		/* Continue while the operation condition remains true. */
		p = argv[i] + 1;
		while (*p) {
			/* Checks the current pointer. */
			if (*p == '1')
				s1 = 1;
			else if (*p == '2')
				s2 = 1;
			else if (*p == '3') {
				s3 = 1;
			} else {
				fprintf(stderr, "comm: invalid option\n");

				/* Reports operation failure. */
				return 2;
			}
			++p;
		}
	}

	/* Validates the command-line arguments. */
	if (argc - i != 2) {
		fprintf(stderr, "usage: comm [-123] file1 file2\n");

		/* Reports operation failure. */
		return 2;
	}
	a = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
	b = !strcmp(argv[i + 1], "-") ? stdin : fopen(argv[i + 1], "r");

	/* Handles the a condition. */
	if (!a || !b || a == b) {
		fprintf(stderr, "comm: cannot open inputs\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Continue while the operation condition remains true. */
	nx = command_read_line(a, &x, &cx);
	ny = command_read_line(b, &y, &cy);
	while (nx > 0 || ny > 0) {
		/* Handles the nx condition. */
		if (nx <= 0)
			c = 1;
		else if (ny <= 0) {
			c = -1;
		} else {
			n = (size_t)(nx < ny ? nx : ny);
			c = memcmp(x, y, n);

			/* Classifies the current input character. */
			if (!c)
				c = (nx > ny) - (nx < ny);
		}

		/* Classifies the current input character. */
		if (c <= 0) {
			out(c ? 1 : 3, s1, s2, s3, x, nx);
			nx = command_read_line(a, &x, &cx);
		}

		/* Classifies the current input character. */
		if (c >= 0) {
			/* Classifies the current input character. */
			if (c)
				out(2, s1, s2, s3, y, ny);
			ny = command_read_line(b, &y, &cy);
		}
	}
	free(x);
	free(y);

	/* Handles the a condition. */
	if (a != stdin)
		fclose(a);

	/* Handles the b condition. */
	if (b != stdin)
		fclose(b);

	/* Computes the function result. */
	function_result = nx < 0 || ny < 0 || ferror(stdout);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the out operation. */
static void
out(
	int col,
	int s1,
	int s2,
	int s3,
	const char *p,
	long n)
{
	int tabs;

	tabs = 0;

	/* Handles the col condition. */
	if ((col == 1 && s1) || (col == 2 && s2) || (col == 3 && s3))
		return;

	/* Handles the col condition. */
	if (col > 1 && !s1)
		tabs++;

	/* Handles the col condition. */
	if (col > 2 && !s2)
		tabs++;

	/* Continue while the operation condition remains true. */
	while (tabs--)
		putchar('\t');
	fwrite(p, 1, (size_t)n, stdout);
}
