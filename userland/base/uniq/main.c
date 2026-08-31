/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD uniq userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void emit(const char *s, long n, unsigned long count, int c, int d, int u);

/*
 * Runs the uniq command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	char *t;
	size_t z;
	int c, d, u, i;
	FILE *f, *o;
	char *a, *b;
	size_t ca, cb;
	long na, nb;
	unsigned long count;

	/* Process each remaining command-line operand. */
	c = 0;
	d = 0;
	u = 0;
	i = 1;
	f = stdin;
	o = stdout;
	a = NULL;
	b = NULL;
	ca = 0;
	cb = 0;
	count = 1;
	for (; i < argc && argv[i][0] == '-'; ++i) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "-c"))
			c = 1;
		else if (!strcmp(argv[i], "-d"))
			d = 1;
		else if (!strcmp(argv[i], "-u"))
			u = 1;
		else
			break;
	}

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-"))
		++i;
	else if (i < argc && (f = fopen(argv[i++], "r")) == NULL) {
		command_error("uniq", argv[i - 1]);

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (i < argc && (o = fopen(argv[i++], "w")) == NULL) {
		command_error("uniq", argv[i - 1]);

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (i != argc) {
		fprintf(stderr, "usage: uniq [-cdu] [input [output]]\n");

		/* Reports operation failure. */
		return 2;
	}
	stdout = o;
	na = command_read_line(f, &a, &ca);

	/* Handles the na condition. */
	if (na <= 0)
		return na < 0;

	/* Continue while the operation condition remains true. */
	while ((nb = command_read_line(f, &b, &cb)) > 0) {
		/* Handles the na condition. */
		if (na == nb && !memcmp(a, b, (size_t)na))
			++count;
		else {
			emit(a, na, count, c, d, u);

			t = a;
			a = b;
			b = t;
			z = ca;
			ca = cb;
			cb = z;
			na = nb;
			count = 1;
		}
	}
	emit(a, na, count, c, d, u);
	free(a);
	free(b);

	/* Checks the current file state. */
	if (f != stdin)
		fclose(f);

	/* Handles the o condition. */
	if (o != stdout)
		fclose(o);

	/* Computes the function result. */
	function_result = nb < 0 || ferror(stdout);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the emit operation. */
static void
emit(
	const char *s,
	long n,
	unsigned long count,
	int c,
	int d,
	int u)
{
	/* Checks the current descriptor. */
	if ((d && count < 2) || (u && count != 1))
		return;

	/* Classifies the current input character. */
	if (c)
		printf("%7lu ", count);
	fwrite(s, 1, (size_t)n, stdout);
}
