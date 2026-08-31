/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD diff userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct lines {
	char **v;
	size_t n, c;
};

static int load(const char *p, struct lines *l);

/*
 * Runs the diff command.
 */
int
main(
	int argc,
	char **argv)
{
	struct lines a = {0}, b = {0};
	size_t i, n;
	int different;

	different = 0;

	/* Handles the selected command-line operation. */
	if (argc == 4 && !strcmp(argv[1], "-u")) {
		argv++;
		argc--;
	}

	/* Validates the command-line arguments. */
	if (argc != 3) {
		fprintf(stderr, "usage: diff [-u] file1 file2\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Validates the command-line arguments. */
	if (load(argv[1], &a) || load(argv[2], &b))
		return 2;

	/* Process each element required by the operation. */
	n = a.n > b.n ? a.n : b.n;
	for (i = 0; i < n; i++) {
		/* Checks the current index. */
		if (i >= a.n || i >= b.n || strcmp(a.v[i], b.v[i])) {
			/* Handles the different condition. */
			if (!different) {
				printf("--- %s\n+++ %s\n@@ -1,%lu +1,%lu @@\n",
				       argv[1], argv[2], (unsigned long)a.n,
				       (unsigned long)b.n);
			}

			/* Checks the current index. */
			if (i < a.n) {
				putchar('-');
				fwrite(a.v[i], 1, strlen(a.v[i]), stdout);

				/* Handles a failed strlen operation. */
				if (a.v[i][strlen(a.v[i]) - 1] != '\n')
					putchar('\n');
			}

			/* Checks the current index. */
			if (i < b.n) {
				putchar('+');
				fwrite(b.v[i], 1, strlen(b.v[i]), stdout);

				/* Handles a failed strlen operation. */
				if (b.v[i][strlen(b.v[i]) - 1] != '\n')
					putchar('\n');
			}
			different = 1;
		}
	}

	/* Process each element required by the operation. */
	for (i = 0; i < a.n; i++)
		free(a.v[i]);

	/* Process each element required by the operation. */
	for (i = 0; i < b.n; i++)
		free(b.v[i]);
	free(a.v);
	free(b.v);

	/* Returns the computed result. */
	return different;
}

/* Supports the load operation. */
static int
load(
	const char *p,
	struct lines *l)
{
	size_t c;
	char **v;
	char *s;
	FILE *f;
	char *b;
	size_t cap;
	long n;

	f = fopen(p, "r");
	b = NULL;
	cap = 0;

	/* Checks the current file state. */
	if (!f) {
		command_error("diff", p);

		/* Reports operation failure. */
		return -1;
	}
	while ((n = command_read_line(f, &b, &cap)) > 0) {
		s = malloc((size_t)n + 1);

		/* Checks the current string state. */
		if (!s)
			return -1;
		memcpy(s, b, (size_t)n + 1);

		/* Handles the l condition. */
		if (l->n == l->c) {
			c = l->c ? l->c * 2 : 32;
			v = realloc(l->v, c * sizeof(*v));

			/* Handles the v condition. */
			if (!v)
				return -1;
			l->v = v;
			l->c = c;
		}
		l->v[l->n++] = s;
	}
	free(b);
	fclose(f);

	/* Returns the computed result. */
	return n < 0 ? -1 : 0;
}
