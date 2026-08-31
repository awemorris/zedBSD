/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD sort userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int reverse, numeric;

static int cmp(const char *x, const char *y);

/*
 * Runs the sort command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	FILE *f;
	const char *p;
	size_t nc;
	char **nv;
	char *s;
	char *x;
	int j;
	int unique, i, status;
	char **v, *line;
	size_t n, cap, lc;
	long len;

	/* Process each remaining command-line operand. */
	unique = 0;
	i = 1;
	status = 0;
	v = NULL;
	line = NULL;
	n = 0;
	cap = 0;
	lc = 0;
	for (; i < argc && argv[i][0] == '-'; ++i) {
		/* Continue while the operation condition remains true. */
				p = argv[i] + 1;
		while (*p) {
			/* Checks the current pointer. */
			if (*p == 'r')
				reverse = 1;
			else if (*p == 'n')
				numeric = 1;
			else if (*p == 'u')
				unique = 1;
			else {
				fprintf(stderr, "sort: invalid option\n");

				/* Reports operation failure. */
				return 2;
			}
			++p;
		}
	}
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");

		/* Checks the current file state. */
		if (!f) {
			command_error("sort", argv[i]);
			status = 1;
			++i;
			continue;
		}
		while ((len = command_read_line(f, &line, &lc)) > 0) {

			s = malloc((size_t)len + 1);

			/* Checks the current string state. */
			if (!s)
				return 1;
			memcpy(s, line, (size_t)len + 1);

			/* Checks the current item count. */
			if (n == cap) {
								nc = cap ? cap * 2 : 64;
								nv = realloc(v, nc * sizeof(*v));

				/* Handles the nv condition. */
				if (!nv)
					return 1;
				v = nv;
				cap = nc;
			}
			v[n++] = s;
		}

		/* Checks the current file state. */
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);

	/* Process each element required by the operation. */
	for (i = 1; i < (int)n; ++i) {
		/* Continue while the operation condition remains true. */
				x = v[i];
				j = i;
		while (j && cmp(v[j - 1], x) > 0) {
			v[j] = v[j - 1];
			--j;
		}
		v[j] = x;
	}

	/* Process each element required by the operation. */
	for (i = 0; i < (int)n; ++i) {
		/* Handles the unique condition. */
		if (unique && i && !strcmp(v[i - 1], v[i]))
			continue;
		fwrite(v[i], 1, strlen(v[i]), stdout);
	}

	/* Process each element required by the operation. */
	for (i = 0; i < (int)n; ++i)
		free(v[i]);
	free(v);
	free(line);

	/* Computes the function result. */
	function_result = status || ferror(stdout);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cmp operation. */
static int
cmp(
	const char *x,
	const char *y)
{
	long long p, q;
	int c;

	/* Handles the numeric condition. */
	if (numeric) {
				p = strtoll(x, NULL, 10);
		q = strtoll(y, NULL, 10);
		c = (p > q) - (p < q);

		/* Classifies the current input character. */
		if (!c)
			c = strcmp(x, y);
	} else
		c = strcmp(x, y);

	/* Returns the computed result. */
	return reverse ? -c : c;
}
