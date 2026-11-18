/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD truncate userland command.
 *
 * truncate [-c] -s size file... sets the size of each file, creating a file
 * that does not exist unless -c is given, as the GNU and BSD commands do.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* The kinds of command-line argument that come before the files. */
enum truncate_argument {
	TRUNCATE_ARGUMENT_FILE,
	TRUNCATE_ARGUMENT_END,
	TRUNCATE_ARGUMENT_NO_CREATE,
	TRUNCATE_ARGUMENT_SIZE_NEXT,
	TRUNCATE_ARGUMENT_SIZE_JOINED,
	TRUNCATE_ARGUMENT_UNKNOWN
};

static enum truncate_argument classify_argument(const char *argument);
static int parse_size(const char *text, off_t *size);
static int truncate_file(const char *path, off_t size, int no_create);
static void print_usage(void);

/*
 * Runs the truncate command.
 */
int
main(
	int argc,
	char **argv)
{
	enum truncate_argument kind;
	const char *size_text;
	off_t size;
	int index, failed, no_create, error, options_done;

	/* Starts with no size, creation allowed and no failure. */
	failed = 0;
	no_create = 0;
	size_text = NULL;
	options_done = 0;

	/* Reads the options that come before the files. */
	index = 1;
	while (!options_done && index < argc) {
		/* Tells an option from the first file. */
		kind = classify_argument(argv[index]);

		/* Acts on the kind of the argument. */
		switch (kind) {
		case TRUNCATE_ARGUMENT_END:
			/* Everything after -- is a file. */
			index++;
			options_done = 1;
			break;
		case TRUNCATE_ARGUMENT_NO_CREATE:
			/* -c leaves a file that does not exist alone. */
			no_create = 1;
			index++;
			break;
		case TRUNCATE_ARGUMENT_SIZE_NEXT:
			/* -s takes the size from the next argument. */
			if (index + 1 >= argc) {
				print_usage();
				return 1;
			}

			/* Takes the size and steps past both arguments. */
			size_text = argv[index + 1];
			index += 2;
			break;
		case TRUNCATE_ARGUMENT_SIZE_JOINED:
			/* -sSIZE carries the size in the same argument. */
			size_text = argv[index] + 2;
			index++;
			break;
		case TRUNCATE_ARGUMENT_UNKNOWN:
			/* Refuses an option the command does not know. */
			print_usage();
			return 1;
		case TRUNCATE_ARGUMENT_FILE:
		default:
			/* The first file ends the options. */
			options_done = 1;
			break;
		}
	}

	/* Refuses a command line without a size or without a file. */
	if (size_text == NULL || index >= argc) {
		print_usage();
		return 1;
	}

	/* Converts the size into a file offset. */
	error = parse_size(size_text, &size);
	if (error != 0) {
		print_usage();
		return 1;
	}

	/* Sets the size of each file, reporting each one that fails. */
	for (; index < argc; index++) {
		error = truncate_file(argv[index], size, no_create);
		if (error != 0) {
			errno = error;
			command_error("truncate", argv[index]);
			failed = 1;
		}
	}

	/* Reports whether any file failed. */
	return failed;
}

/* Tells which kind of argument before the files one argument is. */
static enum truncate_argument
classify_argument(
	const char *argument)
{
	int differs;

	/* A lone "-" or a word without a leading "-" is a file. */
	if (argument[0] != '-')
		return TRUNCATE_ARGUMENT_FILE;
	if (argument[1] == '\0')
		return TRUNCATE_ARGUMENT_FILE;

	/* "--" ends the options. */
	differs = strcmp(argument, "--");
	if (differs == 0)
		return TRUNCATE_ARGUMENT_END;

	/* "-c" asks not to create a missing file. */
	differs = strcmp(argument, "-c");
	if (differs == 0)
		return TRUNCATE_ARGUMENT_NO_CREATE;

	/* "-s" alone takes the size from the next argument. */
	differs = strcmp(argument, "-s");
	if (differs == 0)
		return TRUNCATE_ARGUMENT_SIZE_NEXT;

	/* "-sSIZE" carries the size itself. */
	if (argument[1] == 's')
		return TRUNCATE_ARGUMENT_SIZE_JOINED;

	/* Succeeded: the argument is an option the command does not know. */
	return TRUNCATE_ARGUMENT_UNKNOWN;
}

/* Converts a decimal size into a file offset that the kernel accepts. */
static int
parse_size(
	const char *text,
	off_t *size)
{
	unsigned long long value;
	int error;

	/* Reads the decimal number. */
	error = command_parse_ull(text, &value);
	if (error != 0)
		return EINVAL;

	/* Refuses a size that a file offset cannot hold. */
	if ((off_t)value < 0)
		return EINVAL;
	if ((unsigned long long)(off_t)value != value)
		return EINVAL;

	/* Succeeded: the size fits a file offset. */
	*size = (off_t)value;
	return 0;
}

/*
 * Sets the size of one file, creating it first unless no_create is set.
 * Reports 0 or the errno value of the failed step.  A missing file with
 * no_create set is not an error.
 */
static int
truncate_file(
	const char *path,
	off_t size,
	int no_create)
{
	int flags, fd, status, error;

	/*
	 * Opens the file for writing.  O_NONBLOCK keeps the open of a FIFO
	 * from waiting for a reader; ftruncate then refuses it.
	 */
	flags = O_WRONLY | O_NONBLOCK;

	/* Creates a missing file unless -c was given. */
	if (!no_create)
		flags |= O_CREAT;

	/* Opens, and perhaps creates, the file. */
	fd = open(path, flags, 0666);
	if (fd < 0) {
		error = errno;

		/* -c asks to leave a file that does not exist alone. */
		if (no_create && error == ENOENT)
			return 0;

		/* Reports why the file could not be opened. */
		return error;
	}

	/* Sets the size, keeping the error across the close. */
	status = ftruncate(fd, size);
	error = 0;
	if (status != 0)
		error = errno;

	/* Closes the file; a failed close loses nothing already set. */
	close(fd);

	/* Reports why the size could not be set. */
	if (error != 0)
		return error;

	/* Succeeded: the file now has the requested size. */
	return 0;
}

/* Prints how the command is used. */
static void
print_usage(void)
{
	/* Writes the usage line to the error stream. */
	fprintf(stderr, "usage: truncate [-c] -s size file...\n");
}
