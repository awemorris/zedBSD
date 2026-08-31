/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD iconv userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int enc(const char *s);
static int valid(const unsigned char *b, size_t n);

/*
 * Runs the iconv command.
 */
int
main(
	int argc,
	char **argv)
{
	FILE *f;
	unsigned char b[4096];
	size_t n;
	const char *from, *to;
	int i, status;

	/* Process each remaining command-line operand. */
	from = "UTF-8";
	to = "UTF-8";
	i = 1;
	status = 0;
	for (; i < argc; i++) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "-f") && ++i < argc)
			from = argv[i];
		else if (!strcmp(argv[i], "-t") && ++i < argc)
			to = argv[i];
		else
			break;
	}

	/* Handles a failed enc operation. */
	if (!enc(from) || !enc(to)) {
		fprintf(stderr, "iconv: only UTF-8 is available\n");

		/* Reports operation failure. */
		return 2;
	}
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");

		/* Checks the current file state. */
		if (!f) {
			command_error("iconv", argv[i]);
			status = 1;
			++i;
			continue;
		}
		while ((n = fread(b, 1, sizeof(b), f)) > 0) {
			/* Handles a failed valid operation. */
			if (!valid(b, n)) {
				fprintf(stderr,
					"iconv: invalid UTF-8 sequence\n");
				status = 1;
				break;
			}

			/* Handles a failed fwrite operation. */
			if (fwrite(b, 1, n, stdout) != n) {
				status = 1;
				break;
			}
		}

		/* Checks the current file state. */
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);

	/* Returns the computed result. */
	return status;
}

/* Supports the enc operation. */
static int
enc(
	const char *s)
{
	int function_result;

	/* Computes the function result. */
	function_result = !strcmp(s, "UTF-8") || !strcmp(s, "utf-8") ||
	       !strcmp(s, "UTF8") || !strcmp(s, "utf8");

	/* Returns the computed result. */
	return function_result;
}

/* Supports the valid operation. */
static int
valid(
	const unsigned char *b,
	size_t n)
{
	unsigned c, need;
	size_t i;

	/* Continue while the operation condition remains true. */
	i = 0;
	while (i < n) {
		c = b[i++];

		/* Classifies the current input character. */
		if (c < 128)
			continue;

		/* Classifies the current input character. */
		if (c >= 0xc2 && c <= 0xdf)
			need = 1;
		else if (c >= 0xe0 && c <= 0xef)
			need = 2;
		else if (c >= 0xf0 && c <= 0xf4)
			need = 3;
		else

			/* Reports successful completion. */
			return 0;

		/* Checks the current index. */
		if (i + need > n)
			return 0;

		/* Continue while the operation condition remains true. */
		while (need--) {
			/* Handles the b condition. */
			if ((b[i++] & 0xc0) != 0x80)
				return 0;
		}
	}

	/* Reports operation failure. */
	return 1;
}
