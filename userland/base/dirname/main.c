/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD dirname userland command.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Runs the dirname command.
 */
int
main(
	int argc,
	char **argv)
{
	char *copy, *end;
	size_t length;
	int status;

	status = 0;

	/* Handles the selected command-line operation. */
	if (argc > 1 && strcmp(argv[1], "--") == 0) {
		argc--;
		argv++;
	}

	/* Validates the command-line arguments. */
	if (argc != 2) {
		fprintf(stderr, "usage: dirname string\n");

		/* Reports operation failure. */
		return 1;
	}
	copy = strdup(argv[1]);

	/* Handles the copy availability. */
	if (copy == NULL) {
		fprintf(stderr, "dirname: out of memory\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining element. */
	length = strlen(copy);
	while (length != 0 && copy[length - 1U] == '/')
		copy[--length] = '\0';

	/* Checks the current data length. */
	if (length == 0) {
		strcpy(copy, argv[1][0] == '/' ? "/" : ".");
	} else if ((end = strrchr(copy, '/')) == NULL) {
		strcpy(copy, ".");
	} else {
		/* Continue while the operation condition remains true. */
		while (end > copy && end[-1] == '/')
			end--;

		/* Checks the current endpoint. */
		if (end == copy)
			end++;
		*end = '\0';
	}

	/* Handles the end-of-file condition. */
	if (printf("%s\n", copy) < 0 || fflush(stdout) == EOF)
		status = 1;
	free(copy);

	/* Returns the computed result. */
	return status;
}
