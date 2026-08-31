/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD join userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int keycmp(char *a, char *b, int fa, int fb);
static char *field(char *s, int n);

/*
 * Runs the join command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	char *k, *e;
	int c;
	int f1, f2, i;
	FILE *a, *b;
	char *x, *y;
	size_t cx, cy;
	long nx, ny;

	/* Process each remaining command-line operand. */
	f1 = 1;
	f2 = 1;
	i = 1;
	x = NULL;
	y = NULL;
	cx = 0;
	cy = 0;
	for (; i < argc; i++) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "-1") && ++i < argc)
			f1 = atoi(argv[i]);
		else if (!strcmp(argv[i], "-2") && ++i < argc)
			f2 = atoi(argv[i]);
		else
			break;
	}

	/* Validates the command-line arguments. */
	if (argc - i != 2) {
		fprintf(stderr,
			"usage: join [-1 field] [-2 field] file1 file2\n");

		/* Reports operation failure. */
		return 2;
	}
	a = fopen(argv[i], "r");
	b = fopen(argv[i + 1], "r");

	/* Handles the a condition. */
	if (!a || !b) {
		command_error("join", !a ? argv[i] : argv[i + 1]);

		/* Reports operation failure. */
		return 1;
	}

	/* Continue while the operation condition remains true. */
	nx = command_read_line(a, &x, &cx);
	ny = command_read_line(b, &y, &cy);
	while (nx > 0 && ny > 0) {

		c = keycmp(x, y, f1, f2);

		/* Classifies the current input character. */
		if (c < 0)
			nx = command_read_line(a, &x, &cx);
		else if (c > 0)
			ny = command_read_line(b, &y, &cy);
		else {
			/* Continue while the operation condition remains true. */
						k = field(x, f1);
			e = k;
			while (*e && *e != ' ' && *e != '\t' && *e != '\n')
				e++;
			fwrite(k, 1, (size_t)(e - k), stdout);
			printf(" %s", x);

			/* Handles the nx condition. */
			if (nx && x[nx - 1] != '\n')
				putchar('\n');
			printf("%s", y);
			nx = command_read_line(a, &x, &cx);
			ny = command_read_line(b, &y, &cy);
		}
	}
	free(x);
	free(y);
	fclose(a);
	fclose(b);

	/* Obtains the ferror result. */
	function_result = ferror(stdout);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the keycmp operation. */
static int
keycmp(
	char *a,
	char *b,
	int fa,
	int fb)
{
	char *x, *y;

	/* Continue while the operation condition remains true. */
	x = field(a, fa);
	y = field(b, fb);
	while (*x && *x != ' ' && *x != '\t' && *x != '\n' && *y && *y != ' ' &&
	       *y != '\t' && *y != '\n') {
		/* Checks the current horizontal value. */
		if (*x != *y)
			return (unsigned char)*x - (unsigned char)*y;
		x++;
		y++;
	}

	/* Returns the computed result. */
	return ((*x != ' ' && *x != '\t' && *x != '\n') -
		(*y != ' ' && *y != '\t' && *y != '\n'));
}

/* Supports the field operation. */
static char *
field(
	char *s,
	int n)
{
	char *p;

	/* Continue while the operation condition remains true. */
	p = s;
	while (--n > 0) {
		/* Continue while the operation condition remains true. */
		while (*p == ' ' || *p == '\t')
			p++;

		/* Continue while the operation condition remains true. */
		while (*p && *p != ' ' && *p != '\t' && *p != '\n')
			p++;
	}
	while (*p == ' ' || *p == '\t')
		p++;

	/* Returns the computed result. */
	return p;
}
