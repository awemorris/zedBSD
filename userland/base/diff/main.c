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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct lines {
	char **v;
	size_t n, c;
};

static int load(const char *p, struct lines *l);
static void release_lines(struct lines *lines);
static int text_diff(const char *left, const char *right);
extern int diff_tree(const char *left, const char *right, int recursive, int metadata, int brief, int (*text)(const char *, const char *));

/*
 * Runs the diff command.
 */
int
main(
	int argc,
	char **argv)
{
	int first, recursive, metadata, brief, status;
	const char *option;

	first = 1;
	recursive = metadata = brief = 0;
	/* Selects physical tree comparison without changing ordinary text output. */
	while (first < argc && argv[first][0] == '-') {
		option = argv[first++];
		if (!strcmp(option, "--"))
			break;
		if (!strcmp(option, "-r"))
			recursive = 1;
		else if (!strcmp(option, "-q"))
			brief = 1;
		else if (!strcmp(option, "--metadata"))
			metadata = 1;
		else if (strcmp(option, "-u") != 0) {
			fprintf(stderr, "diff: unknown option: %s\n", option);
			return 2;
		}
	}

	if (argc - first != 2) {
		fprintf(stderr, "usage: diff [-u] [-r] [-q] [--metadata] file1 file2\n");
		return 2;
	}

	/* Keep binary data out of the line-oriented text renderer. */
	status = diff_tree(argv[first], argv[first + 1], recursive, metadata, brief, text_diff);
	if (fclose(stdout) != 0)
		return 2;
	return status;
}

/* Prints the existing text difference format after byte comparison. */
static int
text_diff(const char *left, const char *right)
{
	const char *argv[] = {"diff", left, right};
	struct lines a = {0}, b = {0};
	size_t i, n;
	int different;

	different = 0;

	/* Validates the command-line arguments. */
	if (load(argv[1], &a) || load(argv[2], &b)) {
		release_lines(&a);
		release_lines(&b);
		return 2;
	}

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

/* Releases complete and partial text input when a read fails. */
static void
release_lines(struct lines *lines)
{
	size_t index;

	for (index = 0; index < lines->n; index++)
		free(lines->v[index]);
	free(lines->v);
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
		/* A file changed to binary after the initial byte comparison. */
		if (memchr(b, 0, (size_t)n) != NULL) {
			errno = EIO;
			n = -1;
			break;
		}
		s = malloc((size_t)n + 1);

		/* Checks the current string state. */
		if (!s) {
			n = -1;
			break;
		}
		memcpy(s, b, (size_t)n + 1);

		/* Handles the l condition. */
		if (l->n == l->c) {
			c = l->c ? l->c * 2 : 32;
			v = realloc(l->v, c * sizeof(*v));

			/* Handles the v condition. */
			if (!v) {
				free(s);
				n = -1;
				break;
			}
			l->v = v;
			l->c = c;
		}
		l->v[l->n++] = s;
	}
	free(b);
	if (fclose(f) != 0)
		n = -1;

	/* Returns the computed result. */
	return n < 0 ? -1 : 0;
}
