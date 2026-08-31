/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD nice userland command.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_increment(const char *text, int *result);

/*
 * Runs the nice command.
 */
int
main(
	int argc,
	char **argv)
{
	int increment;
	int index;

	increment = 10;
	index = 1;

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "-n") == 0) {
		/* Validates the command-line arguments. */
		if (++index >= argc ||
		    !parse_increment(argv[index++], &increment))
			goto usage;
	}

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "--") == 0)
		index++;

	/* Validates the command-line arguments. */
	if (index >= argc)
		goto usage;
	errno = 0;

	/* Handles the reported system error. */
	if (nice(increment) == -1 && errno != 0) {
		command_error("nice", NULL);

		/* Reports operation failure. */
		return 1;
	}
	command_exec(argv[index], &argv[index]);
	command_error("nice", argv[index]);

	/* Returns the computed result. */
	return errno == ENOENT ? 127 : 126;

usage:
	fprintf(stderr, "usage: nice [-n increment] utility [argument ...]\n");

	/* Reports operation failure. */
	return 2;
}

/* Supports the parse increment operation. */
static int
parse_increment(
	const char *text,
	int *result)
{
	char *end;
	long value;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0')
		return 0;
	errno = 0;
	value = strtol(text, &end, 10);

	/* Handles the reported system error. */
	if (errno == ERANGE || *end != '\0' || value < INT_MIN ||
	    value > INT_MAX)

		/* Reports successful completion. */
		return 0;
	*result = (int)value;
	/* Reports operation failure. */
	return 1;
}
