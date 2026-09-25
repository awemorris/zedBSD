/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs grep -F with the same arguments (fgrep is the historical name that
 * scripts still use; POSIX has only grep -F).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs fgrep.
 */
int
main(
	int argc,
	char **argv)
{
	char **arguments;
	int index;

	/* grep, -F, and every argument after the name. */
	arguments = calloc((size_t)argc + 2U, sizeof(*arguments));
	if (arguments == NULL) {
		fprintf(stderr, "fgrep: out of memory\n");
		return 2;
	}

	/* The arguments grep is run with. */
	arguments[0] = "grep";
	arguments[1] = "-F";
	for (index = 1; index < argc; index++)
		arguments[index + 1] = argv[index];

	/* grep in its place; returning means it could not be run. */
	execvp("grep", arguments);
	fprintf(stderr, "fgrep: grep: %s\n", strerror(errno));
	return 2;
}
