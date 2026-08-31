/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD pathchk userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int check_path(const char *path, int portable);
static int portable_character(unsigned char value);

/*
 * Runs the pathchk command.
 */
int
main(
	int argc,
	char **argv)
{
	int portable, index, failed;

	portable = 0;
	index = 1;
	failed = 0;

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "-p")) {
		portable = 1;
		index++;
	}

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "--"))
		index++;

	/* Validates the command-line arguments. */
	if (index == argc) {
		fprintf(stderr, "usage: pathchk [-p] pathname...\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (; index < argc; index++) {
		/* Validates the command-line arguments. */
		if (!check_path(argv[index], portable)) {
			command_error("pathchk", argv[index]);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
}

/* Supports the check path operation. */
static int
check_path(
	const char *path,
	int portable)
{
	const char *end;
	const char *component;
	long path_limit;
	long name_limit;
	size_t length;
	const unsigned char *byte;

	component = path;
	path_limit = portable ? 256 : pathconf("/", _PC_PATH_MAX);
	name_limit = portable ? 14 : pathconf("/", _PC_NAME_MAX);

	/* Handles the path limit condition. */
	if (path_limit <= 0)
		path_limit = 256;

	/* Handles the name limit condition. */
	if (name_limit <= 0)
		name_limit = 255;

	/* Handles a failed strlen operation. */
	if (strlen(path) >= (size_t)path_limit) {
		errno = ENAMETOOLONG;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the portable condition. */
	if (portable) {
		/* Process each element required by the operation. */
		for (byte = (const unsigned char *)path; *byte != '\0'; byte++) {
			/* Handles a failed portable character operation. */
			if (!portable_character(*byte)) {
				errno = EINVAL;

				/* Reports successful completion. */
				return 0;
			}
		}
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		end = strchr(component, '/');
		length = end == NULL ? strlen(component)
				     : (size_t)(end - component);

		/* Checks the current data length. */
		if (length > (size_t)name_limit) {
			errno = ENAMETOOLONG;

			/* Reports successful completion. */
			return 0;
		}

		/* Handles the end availability. */
		if (end == NULL)
			break;
		component = end + 1;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the portable character operation. */
static int
portable_character(
	unsigned char value)
{
	/* Returns the computed result. */
	return (value >= 'A' && value <= 'Z') ||
	       (value >= 'a' && value <= 'z') ||
	       (value >= '0' && value <= '9') || value == '.' || value == '_' ||
	       value == '-' || value == '/';
}
