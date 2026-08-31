/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD pr userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Runs the pr command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	FILE *f;
	char *l;
	size_t cap;
	long n;
	unsigned long line;
	const char *header;
	int number, i;

	/* Process each remaining command-line operand. */
	header = NULL;
	number = 0;
	i = 1;
	for (; i < argc; i++) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "-h") && ++i < argc)
			header = argv[i];
		else if (!strcmp(argv[i], "-n"))
			number = 1;
		else
			break;
	}
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");
				l = NULL;
				cap = 0;

				line = 1;

		/* Checks the current file state. */
		if (!f) {
			command_error("pr", argv[i]);

			/* Reports operation failure. */
			return 1;
		}

		/* Handles the header condition. */
		if (header)
			printf("\n\n%s\n\n", header);

		/* Continue while the operation condition remains true. */
		while ((n = command_read_line(f, &l, &cap)) > 0) {
			/* Handles the number condition. */
			if (number)
				printf("%5lu ", line++);
			fwrite(l, 1, (size_t)n, stdout);
		}
		free(l);

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
