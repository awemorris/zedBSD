/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD chown userland command.
 */

#include "userland/base/common/command.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the chown command.
 */
int
main(
	int argc,
	char **argv)
{
	char *separator;
	unsigned long long user, group;
	int i, failed;

	group = (unsigned long long)(gid_t)-1;
	failed = 0;

	/* Validates the command-line arguments. */
	if (argc < 3)
		goto usage;
	separator = strchr(argv[1], ':');

	/* Handles the separator condition. */
	if (separator)
		*separator = '\0';
	/* Validates the command-line arguments. */
	if (command_parse_ull(argv[1], &user) ||
	    user > (unsigned long long)(uid_t)-1)
		goto usage;

	/* Handles a failed command parse ull operation. */
	if (separator && (command_parse_ull(separator + 1, &group) ||
			  group > (unsigned long long)(gid_t)-1))
		goto usage;

	/* Process each remaining command-line operand. */
	for (i = 2; i < argc; i++)

		/* Validates the command-line arguments. */
		if (chown(argv[i], (uid_t)user, (gid_t)group)) {
			command_error("chown", argv[i]);
			failed = 1;
		}

	/* Returns the computed result. */
	return failed;
usage:
	fprintf(stderr, "usage: chown uid[:gid] file...\n");

	/* Reports operation failure. */
	return 1;
}
