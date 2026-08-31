/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD nl userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Runs the nl command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	FILE *f;
	int nonempty;
	unsigned long long number, inc;
	int all, i;
	char *l;
	size_t cap;
	long n;

	/* Process each remaining command-line operand. */
	number = 1;
	inc = 1;
	all = 0;
	i = 1;
	l = NULL;
	cap = 0;
	for (; i < argc; i++) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "-ba"))
			all = 1;
		else if (!strcmp(argv[i], "-bt"))
			all = 0;
		else if (!strcmp(argv[i], "-v") && ++i < argc) {
			/* Validates the command-line arguments. */
			if (command_parse_ull(argv[i], &number))
				return 2;
		} else if (!strcmp(argv[i], "-i") && ++i < argc) {
			/* Validates the command-line arguments. */
			if (command_parse_ull(argv[i], &inc))
				return 2;
		} else
			break;
	}
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");

		/* Checks the current file state. */
		if (!f) {
			command_error("nl", argv[i]);

			/* Reports operation failure. */
			return 1;
		}
		while ((n = command_read_line(f, &l, &cap)) > 0) {

			nonempty = n > 1 || (n == 1 && l[0] != '\n');

			/* Handles the all condition. */
			if (all || nonempty) {
				printf("%6llu\t", number);
				number += inc;
			} else
				printf("       ");
			fwrite(l, 1, (size_t)n, stdout);
		}

		/* Checks the current file state. */
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);
	free(l);

	/* Obtains the ferror result. */
	function_result = ferror(stdout);

	/* Returns the computed result. */
	return function_result;
}
