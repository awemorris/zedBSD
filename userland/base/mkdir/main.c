/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD mkdir userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int make_parents(const char *name, mode_t mode);

/*
 * Runs the mkdir command.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned value;
	int p, i, failed;
	mode_t mode;

	/* Process each remaining command-line operand. */
	p = 0;
	i = 1;
	failed = 0;
	mode = 0777;
	for (; i < argc && argv[i][0] == '-'; i++) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "-p"))
			p = 1;
		else if (!strcmp(argv[i], "-m") && ++i < argc) {
			/* Validates the command-line arguments. */
			if (command_parse_mode(argv[i], &value))
				goto usage;
			mode = (mode_t)value;
		} else
			goto usage;
	}

	/* Validates the command-line arguments. */
	if (i == argc)
		goto usage;

	/* Process each remaining command-line operand. */
	for (; i < argc; i++)

		/* Validates the command-line arguments. */
		if ((p ? make_parents(argv[i], mode) : mkdir(argv[i], mode)) !=
		    0) {
			command_error("mkdir", argv[i]);
			failed = 1;
		}

	/* Returns the computed result. */
	return failed;
usage:
	fprintf(stderr, "usage: mkdir [-p] [-m mode] directory...\n");

	/* Reports operation failure. */
	return 1;
}

/* Supports the make parents operation. */
static int
make_parents(
	const char *name,
	mode_t mode)
{
	struct stat status_local;
	struct stat status_local1;
	char path[1024];
	size_t i, length;

	length = strlen(name);

	/* Checks the current data length. */
	if (length == 0 || length >= sizeof(path)) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	memcpy(path, name, length + 1);

	/* Process each remaining element. */
	for (i = 1; i < length; i++)

		/* Handles the path condition. */
		if (path[i] == '/') {

			path[i] = '\0';

			/* Handles the reported system error. */
			if (mkdir(path, mode) != 0 &&
			    (errno != EEXIST || stat(path, &status_local) != 0 ||
			     !S_ISDIR(status_local.st_mode)))

				/* Reports operation failure. */
				return -1;
			path[i] = '/';
		}

	/* Handles a failed mkdir operation. */
	if (mkdir(path, mode) != 0) {
		/* Handles the reported system error. */
		if (errno != EEXIST || stat(path, &status_local1) != 0 ||
		    !S_ISDIR(status_local1.st_mode))

			/* Reports operation failure. */
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}
