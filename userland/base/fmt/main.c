/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD fmt userland command.
 */

#include "userland/base/common/command.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Runs the fmt command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	FILE *f;
	int d;
	unsigned long long width, col;
	int i, c, inword;

	width = 75;
	col = 0;
	i = 1;
	inword = 0;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-w") && ++i < argc) {
		/* Validates the command-line arguments. */
		if (command_parse_ull(argv[i++], &width) || !width)
			return 2;
	}
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");

		/* Checks the current file state. */
		if (!f) {
			command_error("fmt", argv[i]);

			/* Reports operation failure. */
			return 1;
		}
		while ((c = fgetc(f)) != EOF) {
			/* Classifies the current input character. */
			if (c == '\n') {
				d = fgetc(f);

				/* Checks the current descriptor. */
				if (d == '\n') {
					putchar('\n');
					putchar('\n');
					col = 0;
					inword = 0;
				} else {
					/* Handles the end-of-file condition. */
					if (d != EOF)
						ungetc(d, f);

					/* Handles the inword condition. */
					if (inword) {
						putchar(' ');
						col++;
						inword = 0;
					}
				}
				continue;
			}

			/* Handles the isspace condition. */
			if (isspace((unsigned char)c)) {
				/* Handles the inword condition. */
				if (inword) {
					putchar(' ');
					col++;
					inword = 0;
				}
				continue;
			}

			/* Handles the col condition. */
			if (col >= width && inword == 0) {
				putchar('\n');
				col = 0;
			}
			putchar(c);
			col++;
			inword = 1;
		}

		/* Checks the current file state. */
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);

	/* Handles the col condition. */
	if (col)
		putchar('\n');

	/* Obtains the ferror result. */
	function_result = ferror(stdout);

	/* Returns the computed result. */
	return function_result;
}
