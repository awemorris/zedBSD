/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD grep userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int match(const char *p, const char *s);
static int here(const char *p, const char *s);
static int atom(const char *p, char c);

/*
 * Runs the grep command.
 */
int
main(
	int argc,
	char **argv)
{
	FILE *f;
	const char *p;
	int yes;
	char *l;
	size_t cap;
	long n, ln, hits;
	int inv, num, count, list, quiet, fixed, i, status, multi;
	const char *pat;

	/* Process each remaining command-line operand. */
	inv = 0;
	num = 0;
	count = 0;
	list = 0;
	quiet = 0;
	fixed = 0;
	i = 1;
	status = 1;
	for (; i < argc && argv[i][0] == '-'; i++) {
				p = argv[i] + 1;

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}
		while (*p) {
			/* Checks the current pointer. */
			if (*p == 'v')
				inv = 1;
			else if (*p == 'n')
				num = 1;
			else if (*p == 'c')
				count = 1;
			else if (*p == 'l')
				list = 1;
			else if (*p == 'q')
				quiet = 1;
			else if (*p == 'F')
				fixed = 1;
			else if (*p == 'E') {
			} else {
				fprintf(stderr, "grep: invalid option\n");

				/* Reports operation failure. */
				return 2;
			}
			p++;
		}
	}

	/* Validates the command-line arguments. */
	if (i >= argc) {
		fprintf(stderr, "usage: grep [-vnlcqFE] pattern [file ...]\n");

		/* Reports operation failure. */
		return 2;
	}
	pat = argv[i++];
	multi = argc - i > 1;
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");
				l = NULL;
				cap = 0;
				ln = 0;
		hits = 0;
		const char *name = i == argc ? "(standard input)" : argv[i];

		/* Checks the current file state. */
		if (!f) {
			command_error("grep", name);
			status = 2;
			++i;
			continue;
		}
		while ((n = command_read_line(f, &l, &cap)) > 0) {

			ln++;

			/* Checks the current item count. */
			if (n && l[n - 1] == '\n')
				l[n - 1] = 0;
			yes = fixed ? strstr(l, pat) != NULL : match(pat, l);

			/* Handles the inv condition. */
			if (inv)
				yes = !yes;

			/* Handles the yes condition. */
			if (yes) {
				hits++;
				status = 0;

				/* Handles the quiet condition. */
				if (quiet)
					break;

				/* Handles the list condition. */
				if (list) {
					puts(name);
					break;
				}

				/* Checks the remaining item count. */
				if (!count) {
					/* Handles the multi condition. */
					if (multi)
						printf("%s:", name);

					/* Handles the num condition. */
					if (num)
						printf("%ld:", ln);
					fwrite(l, 1, strlen(l), stdout);
					putchar('\n');
				}
			}
		}

		/* Checks the remaining item count. */
		if (count) {
			/* Handles the multi condition. */
			if (multi)
				printf("%s:", name);
			printf("%ld\n", hits);
		}
		free(l);

		/* Checks the current file state. */
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);

	/* Returns the computed result. */
	return status;
}

/* Supports the match operation. */
static int
match(
	const char *p,
	const char *s)
{
	int function_result;

	/* Checks the current pointer. */
	if (*p == '^') {
		/* Obtains the here result. */
		function_result = here(p + 1, s);

		/* Returns the computed result. */
		return function_result;
	}
	do {
		/* Handles the here condition. */
		if (here(p, s))
			return 1;
	} while (*s++);

	/* Reports successful completion. */
	return 0;
}

/* Supports the here operation. */
static int
here(
	const char *p,
	const char *s)
{
	int function_result;

	/* Checks the current pointer. */
	if (!*p)
		return 1;

	/* Checks the current pointer. */
	if (p[1] == '*') {
		do {
			/* Handles the here condition. */
			if (here(p + 2, s))
				return 1;
		} while (*s && atom(p, *s++));

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the current pointer. */
	if (*p == '$' && !p[1])
		return !*s;

	/* Checks the current string state. */
	if (*s && atom(p, *s)) {
		/* Obtains the here result. */
		function_result = here(p + 1, s + 1);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the atom operation. */
static int
atom(
	const char *p,
	char c)
{
	/* Returns the computed result. */
	return *p == '.' || *p == c;
}
