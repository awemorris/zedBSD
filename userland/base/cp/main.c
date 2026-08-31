/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD cp userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *leaf(const char *path);
static int copy_file(const char *source, const char *destination, const char **failed_operand);

/*
 * Runs the cp command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *failed_operand;
	char target[1024];
	const char *destination;
	struct stat destination_status;
	int i, failed, destination_is_dir;

	failed = 0;

	/* Handles the selected command-line operation. */
	if (argc > 1 && !strcmp(argv[1], "--")) {
		argv++;
		argc--;
	}

	/* Validates the command-line arguments. */
	if (argc < 3) {
		fprintf(stderr, "usage: cp source... destination\n");

		/* Reports operation failure. */
		return 1;
	}
	destination_is_dir = stat(argv[argc - 1], &destination_status) == 0 &&
			     S_ISDIR(destination_status.st_mode);

	/* Validates the command-line arguments. */
	if (argc > 3 && !destination_is_dir) {
		fprintf(stderr, "cp: destination is not a directory\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (i = 1; i < argc - 1; i++) {

				destination = argv[argc - 1];

		/* Handles the destination is dir condition. */
		if (destination_is_dir) {
			/* Validates the command-line arguments. */
			if (snprintf(target, sizeof(target), "%s/%s",
				     destination,
				     leaf(argv[i])) >= (int)sizeof(target)) {
				errno = ENAMETOOLONG;
				command_error("cp", argv[i]);
				failed = 1;
				continue;
			}
			destination = target;
		}

		/* Validates the command-line arguments. */
		if (copy_file(argv[i], destination, &failed_operand)) {
			command_error("cp", failed_operand);
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

/* Supports the copy file operation. */
static int
copy_file(
	const char *source,
	const char *destination,
	const char **failed_operand)
{
	struct stat from, to;
	int input, output, result;

	input = -1;
	output = -1;
	result = -1;
	*failed_operand = source;
	/* Handles the stat condition. */
	if (stat(source, &from))
		goto done;

	/* Handles a failed S ISREG operation. */
	if (!S_ISREG(from.st_mode)) {
		errno = EINVAL;
		goto done;
	}

	/* Handles a failed stat operation. */
	if (stat(destination, &to) == 0 && from.st_dev == to.st_dev &&
	    from.st_ino == to.st_ino) {
		errno = EINVAL;
		goto done;
	}
	input = open(source, O_RDONLY);

	/* Validates the current input. */
	if (input < 0)
		goto done;
	*failed_operand = destination;
	output = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0666);

	/* Handles the output condition. */
	if (output < 0)
		goto done;

	/* Handles the command copy fd condition. */
	if (command_copy_fd(input, output) || close(output)) {
		output = -1;
		goto done;
	}
	output = -1;
	result = 0;
done:

	/* Handles the output condition. */
	if (output >= 0)
		(void)close(output);

	/* Validates the current input. */
	if (input >= 0)
		(void)close(input);

	/* Returns the computed result. */
	return result;
}
