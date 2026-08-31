/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD paste userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Runs the paste command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	long n;
	FILE *f;
	char *l;
	size_t c;
	size_t di;
	int first;
	int count, j, active;
	FILE **fs;
	char **ls;
	size_t *cs;
	const char *del;
	int serial, i, status;

	del = "\t";
	serial = 0;
	i = 1;
	status = 0;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-s")) {
		serial = 1;
		i++;
	}

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-d") && ++i < argc)
		del = argv[i++];

	/* Validates the command-line arguments. */
	if (i == argc) {
		argv[--i] = (char *)"-";
		argc = i + 1;
	}

	/* Handles the serial condition. */
	if (serial) {
		/* Process each remaining command-line operand. */
		for (; i < argc; i++) {
						f = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
						l = NULL;
						c = 0;
			long n;
						di = 0;
						first = 1;

			/* Checks the current file state. */
			if (!f) {
				command_error("paste", argv[i]);
				status = 1;
				continue;
			}
			while ((n = command_read_line(f, &l, &c)) > 0) {
				/* Checks the current item count. */
				if (n && l[n - 1] == '\n')
					n--;

				/* Handles the first condition. */
				if (!first)
					putchar(del[di++ % strlen(del)]);
				fwrite(l, 1, (size_t)n, stdout);
				first = 0;
			}
			putchar('\n');
			free(l);

			/* Checks the current file state. */
			if (f != stdin)
				fclose(f);
		}

		/* Computes the function result. */
		function_result = status || ferror(stdout);

		/* Returns the computed result. */
		return function_result;
	} else {
				count = argc - i;
				fs = calloc((size_t)count, sizeof(*fs));
				ls = calloc((size_t)count, sizeof(*ls));
				cs = calloc((size_t)count, sizeof(*cs));

		/* Handles the fs condition. */
		if (!fs || !ls || !cs)
			return 1;

		/* Process each remaining element. */
		for (j = 0; j < count; j++) {
			fs[j] = !strcmp(argv[i + j], "-")
				    ? stdin
				    : fopen(argv[i + j], "r");

			/* Handles the fs condition. */
			if (!fs[j]) {
				command_error("paste", argv[i + j]);

				/* Reports operation failure. */
				return 1;
			}
		}
		do {
			/* Process each remaining element. */
			active = 0;
			for (j = 0; j < count; j++) {
								n = command_read_line(fs[j], &ls[j], &cs[j]);

				/* Checks the current item count. */
				if (n > 0) {
					active = 1;

					/* Handles the ls condition. */
					if (ls[j][n - 1] == '\n')
						n--;
					fwrite(ls[j], 1, (size_t)n, stdout);
				}

				/* Handles the j condition. */
				if (j + 1 < count)
					putchar(del[(size_t)j % strlen(del)]);
			}

			/* Handles the active condition. */
			if (active)
				putchar('\n');
		} while (active);

		/* Process each remaining element. */
		for (j = 0; j < count; j++) {
			free(ls[j]);

			/* Handles the fs condition. */
			if (fs[j] != stdin)
				fclose(fs[j]);
		}
		free(fs);
		free(ls);
		free(cs);

		/* Obtains the ferror result. */
		function_result = ferror(stdout);

		/* Returns the computed result. */
		return function_result;
	}
}
