/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD expand userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>

/*
 * Runs the expand command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	FILE *f;
	unsigned long long n;
	unsigned long long tab, col;
	int i, c;

	tab = 8;
	col = 0;
	i = 1;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-t") && ++i < argc) {
		/* Validates the command-line arguments. */
		if (command_parse_ull(argv[i++], &tab) || !tab) {
			fprintf(stderr, "expand: invalid tab stop\n");

			/* Reports operation failure. */
			return 2;
		}
	}
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");

		/* Checks the current file state. */
		if (!f) {
			command_error("expand", argv[i]);

			/* Reports operation failure. */
			return 1;
		}
		while ((c = fgetc(f)) != EOF) {
			/* Classifies the current input character. */
			if (c == '\t') {
				/* Continue while the operation condition remains true. */
				n = tab - col % tab;
				while (n--)
					putchar(' ');
				col += tab - col % tab;
			} else {
				putchar(c);

				/* Classifies the current input character. */
				if (c == '\n' || c == '\r')
					col = 0;
				else if (c == '\b' && col)
					col--;
				else
					col++;
			}
		}

		/* Checks the current file state. */
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);

	/* Obtains the ferror result. */
	function_result = ferror(stdout);

	/* Returns the computed result. */
	return function_result;
}
