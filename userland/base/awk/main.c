/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD awk userland command.
 */

#include "userland/base/common/command.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Runs the awk command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	FILE *f;
	const char *p_local;
	char *p_local1;
	char *e;
	int k;
	char *l;
	size_t cap;
	long n;
	const char *prog;
	int field, whole, i;

	field = 0;
	whole = 0;
	i = 1;

	/* Validates the command-line arguments. */
	if (argc < 2) {
		fprintf(stderr, "usage: awk program [file ...]\n");

		/* Reports operation failure. */
		return 2;
	}
	prog = argv[i++];

	/* Selects the matching value. */
	if (!strcmp(prog, "{print}") || !strcmp(prog, "{ print }"))
		whole = 1;
	else {
				p_local = strstr(prog, "print $");

		/* Handles the p local condition. */
		if (p_local)
			field = atoi(p_local + 7);

		/* Handles the field condition. */
		if (field < 1) {
			fprintf(stderr, "awk: selected implementation supports "
					"{ print $N }\n");

			/* Reports operation failure. */
			return 2;
		}
	}
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");
				l = NULL;
				cap = 0;

		/* Checks the current file state. */
		if (!f) {
			command_error("awk", argv[i]);

			/* Reports operation failure. */
			return 1;
		}
		while ((n = command_read_line(f, &l, &cap)) > 0) {
			/* Handles the whole condition. */
			if (whole)
				fwrite(l, 1, (size_t)n, stdout);
			else {
				/* Continue while the operation condition remains true. */
								p_local1 = l;
								k = 1;
				while (isspace((unsigned char)*p_local1))
					p_local1++;

				/* Continue while the operation condition remains true. */
				while (k < field && *p_local1) {
					/* Continue while the operation condition remains true. */
					while (*p_local1 &&
					       !isspace((unsigned char)*p_local1))
						p_local1++;

					/* Continue while the operation condition remains true. */
					while (isspace((unsigned char)*p_local1))
						p_local1++;
					k++;
				}

				/* Continue while the operation condition remains true. */
				e = p_local1;
				while (*e &&
				       !isspace((unsigned char)*e))
					e++;
				fwrite(p_local1, 1, (size_t)(e - p_local1), stdout);
				putchar('\n');
			}
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
