/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD mv userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *leaf(const char *path);

/*
 * Runs the mv command.
 */
int
main(
	int argc,
	char **argv)
{
	char target[1024];
	const char *to;
	struct stat st;
	int i, failed, isdir;

	failed = 0;

	/* Handles the selected command-line operation. */
	if (argc > 1 && !strcmp(argv[1], "--")) {
		argv++;
		argc--;
	}

	/* Validates the command-line arguments. */
	if (argc < 3) {
		fprintf(stderr, "usage: mv source... destination\n");

		/* Reports operation failure. */
		return 1;
	}
	isdir = stat(argv[argc - 1], &st) == 0 && S_ISDIR(st.st_mode);

	/* Validates the command-line arguments. */
	if (argc > 3 && !isdir) {
		fprintf(stderr, "mv: destination is not a directory\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (i = 1; i < argc - 1; i++) {

				to = argv[argc - 1];

		/* Handles the isdir condition. */
		if (isdir) {
			/* Validates the command-line arguments. */
			if (snprintf(target, sizeof(target), "%s/%s", to,
				     leaf(argv[i])) >= (int)sizeof(target)) {
				errno = ENAMETOOLONG;
				command_error("mv", argv[i]);
				failed = 1;
				continue;
			}
			to = target;
		}

		/* Validates the command-line arguments. */
		if (rename(argv[i], to) != 0) {
			command_error("mv", argv[i]);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
}

/* Supports the leaf operation. */
static const char *
leaf(
	const char *path)
{
	const char *p;

	p = strrchr(path, '/');

	/* Returns the computed result. */
	return p ? p + 1 : path;
}
