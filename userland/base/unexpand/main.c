/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD unexpand userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>

/*
 * Runs the unexpand command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	FILE *f;
	unsigned long long tab, col, spaces;
	int all, i, c;

	tab = 8;
	col = 0;
	spaces = 0;
	all = 0;
	i = 1;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-a")) {
		all = 1;
		i++;
	}

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-t") && ++i < argc) {
		/* Validates the command-line arguments. */
		if (command_parse_ull(argv[i++], &tab) || !tab)
			return 2;
	}
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");

		/* Checks the current file state. */
		if (!f) {
			command_error("unexpand", argv[i]);

			/* Reports operation failure. */
			return 1;
		}
		while ((c = fgetc(f)) != EOF) {
			/* Classifies the current input character. */
			if (c == ' ' && (all || col == spaces)) {
				spaces++;

				/* Handles the col condition. */
				if ((col + spaces) % tab == 0) {
					putchar('\t');
					col += spaces;
					spaces = 0;
				}
				continue;
			}
			while (spaces--) {
				putchar(' ');
				col++;
			}
			spaces = 0;
			putchar(c);

			/* Classifies the current input character. */
			if (c == '\n' || c == '\r')
				col = 0;
			else if (c == '\t')
				col += tab - col % tab;
			else
				col++;
		}
		while (spaces--)
			putchar(' ');
		spaces = 0;

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
