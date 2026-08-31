/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD cut userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int selected(const char *list, unsigned long pos);

/*
 * Runs the cut command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	FILE *f;
	int yes;
	int has;
	long k;
	unsigned long pos;
	int fields, suppress, i;
	const char *list;
	int delim;
	char *line;
	size_t cap;
	long n;

	/* Process each remaining command-line operand. */
	fields = 0;
	suppress = 0;
	list = NULL;
	delim = '\t';
	line = NULL;
	cap = 0;
	for (i = 1; i < argc; ++i) {
		/* Handles the selected command-line operation. */
		if ((!strcmp(argv[i], "-b") || !strcmp(argv[i], "-c")) &&
		    ++i < argc)
			list = argv[i];
		else if (!strcmp(argv[i], "-f") && ++i < argc) {
			fields = 1;
			list = argv[i];
		} else if (!strcmp(argv[i], "-d") && ++i < argc && argv[i][0] &&
			   !argv[i][1])
			delim = argv[i][0];
		else if (!strcmp(argv[i], "-s"))
			suppress = 1;
		else
			break;
	}

	/* Handles the list condition. */
	if (!list) {
		fprintf(stderr, "usage: cut -b list | -c list | -f list [-d "
				"char] [-s] [file ...]\n");

		/* Reports operation failure. */
		return 2;
	}
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");

		/* Checks the current file state. */
		if (!f) {
			command_error("cut", argv[i]);

			/* Reports operation failure. */
			return 1;
		}
		while ((n = command_read_line(f, &line, &cap)) > 0) {
			pos = 1;
			has = memchr(line, delim, (size_t)n) != NULL;

			/* Handles the fields condition. */
			if (fields && !has) {
				/* Handles the suppress condition. */
				if (!suppress)
					fwrite(line, 1, (size_t)n, stdout);
				continue;
			}

			/* Process each element required by the operation. */
			for (k = 0; k < n; ++k) {
				/* Handles the fields condition. */
				if (!fields && line[k] == '\n') {
					fputc('\n', stdout);
					continue;
				}
				yes = selected(list, pos);

				/* Handles the yes condition. */
				if (yes < 0) {
					fprintf(stderr, "cut: invalid list\n");

					/* Reports operation failure. */
					return 2;
				}

				/* Handles the yes condition. */
				if (yes)
					fputc(line[k], stdout);

				/* Handles the fields condition. */
				if (fields && line[k] == delim)
					++pos;
				else if (!fields)
					++pos;
			}
		}

		/* Checks the current item count. */
		if (n < 0) {
			command_error("cut", i == argc ? NULL : argv[i]);

			/* Reports operation failure. */
			return 1;
		}

		/* Checks the current file state. */
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);
	free(line);

	/* Computes the function result. */
	function_result = ferror(stdout) ? 1 : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the selected operation. */
static int
selected(
	const char *list,
	unsigned long pos)
{
	char *e;
	unsigned long a, b;
	const char *p;

	/* Continue while the operation condition remains true. */
	p = list;
	while (*p) {
		a = strtoul(p, &e, 10);
		b = a;

		/* Handles the e condition. */
		if (e == p || !a)
			return -1;
		p = e;

		/* Checks the current pointer. */
		if (*p == '-') {
			++p;

			/* Checks the current pointer. */
			if (*p == ',' || !*p) {
				b = (unsigned long)-1;
			} else {
				b = strtoul(p, &e, 10);

				/* Handles the e condition. */
				if (e == p || b < a)
					return -1;
				p = e;
			}
		}

		/* Handles the pos condition. */
		if (pos >= a && pos <= b)
			return 1;

		/* Checks the current pointer. */
		if (*p == ',')
			++p;
		else if (*p)

			/* Reports operation failure. */
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}
