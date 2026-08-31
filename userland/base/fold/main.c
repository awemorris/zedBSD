/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD fold userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Runs the fold command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	FILE *f;
	unsigned long long next;
	unsigned long long w, col;
	int bytes, i, c;

	/* Process each remaining command-line operand. */
	w = 80;
	col = 0;
	bytes = 0;
	i = 1;
	for (; i < argc; ++i) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "-b"))
			bytes = 1;
		else if (!strcmp(argv[i], "-w") && ++i < argc) {
			/* Validates the command-line arguments. */
			if (command_parse_ull(argv[i], &w) || !w) {
				fprintf(stderr, "fold: invalid width\n");

				/* Reports operation failure. */
				return 2;
			}
		} else {
			break;
		}
	}
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");

		/* Checks the current file state. */
		if (!f) {
			command_error("fold", argv[i]);

			/* Reports operation failure. */
			return 1;
		}
		while ((c = fgetc(f)) != EOF) {
			next = col;

			/* Classifies the current input character. */
			if (c == '\n' || c == '\r')
				next = 0;
			else if (c == '\b' && !bytes)
				next = col ? col - 1 : 0;
			else if (c == '\t' && !bytes)
				next = (col + 8) & ~7ULL;
			else
				next = col + 1;

			/* Classifies the current input character. */
			if (c != '\n' && next > w) {
				putchar('\n');
				col = 0;
				next = (c == '\t' && !bytes) ? 8 : 1;
			}
			putchar(c);
			col = next;
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
