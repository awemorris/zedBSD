/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD head userland command.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int copy_head(int input, unsigned long long limit, int bytes);
static int copy_head_lines(int input, unsigned long long limit);

/*
 * Runs the head command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int descriptor;
	unsigned long long limit;
	int bytes, index, failed, files;

	limit = 10;
	bytes = 0;
	index = 1;
	failed = 0;

	/* Handles the selected command-line operation. */
	if (index < argc &&
	    (!strcmp(argv[index], "-n") || !strcmp(argv[index], "-c"))) {
		bytes = argv[index][1] == 'c';

		/* Validates the command-line arguments. */
		if (++index == argc ||
		    command_parse_ull(argv[index++], &limit) != 0)
			goto usage;
	}

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "--"))
		index++;
	files = argc - index;

	/* Handles the files condition. */
	if (files == 0) {
		/* Computes the function result. */
		function_result = (bytes ? copy_head(STDIN_FILENO, limit, 1)
			      : copy_head_lines(STDIN_FILENO, limit)) != 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining command-line operand. */
	for (; index < argc; index++) {
		descriptor = !strcmp(argv[index], "-")
		     ? STDIN_FILENO
		     : open(argv[index], O_RDONLY);

		/* Checks the file descriptor. */
		if (descriptor < 0) {
			command_error("head", argv[index]);
			failed = 1;
			continue;
		}

		/* Handles the files condition. */
		if (files > 1) {
			printf("%s==> %s <==\n",
			       index == argc - files ? "" : "\n", argv[index]);
		}

		/* Handles a failed copy head operation. */
		if ((bytes ? copy_head(descriptor, limit, 1)
			   : copy_head_lines(descriptor, limit)) != 0) {
			command_error("head", argv[index]);
			failed = 1;
		}

		/* Handles a failed close operation. */
		if (descriptor != STDIN_FILENO && close(descriptor) != 0) {
			command_error("head", argv[index]);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
usage:
	fprintf(stderr, "usage: head [-n number | -c bytes] [file...]\n");

	/* Reports operation failure. */
	return 1;
}

/* Supports the copy head operation. */
static int
copy_head(
	int input,
	unsigned long long limit,
	int bytes)
{
	size_t end;
	size_t used;
	size_t wanted;
	ssize_t got;
	unsigned char buffer[4096];
	unsigned long long count;

	/* Process each remaining element. */
	count = 0;
	while (count < limit) {
		wanted = sizeof(buffer);

		/* Handles the bytes condition. */
		if (bytes && limit - count < wanted)
			wanted = (size_t)(limit - count);
		got = read(input, buffer, wanted);

		/* Handles the got condition. */
		if (got == 0)
			return 0;

		/* Handles the got condition. */
		if (got < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;

			/* Reports operation failure. */
			return -1;
		}

		/* Handles the bytes condition. */
		if (bytes) {
			/* Handles a failed command write all operation. */
			if (command_write_all(STDOUT_FILENO, buffer,
					      (size_t)got) != 0)

				/* Reports operation failure. */
				return -1;
			count += (unsigned long long)got;
		} else {
			/* Process each remaining element. */
			used = 0;
			while (used < (size_t)got && count < limit) {
				/* Process each remaining element. */
				end = used;
				while (end < (size_t)got && buffer[end] != '\n')
					end++;

				/* Checks the current endpoint. */
				if (end < (size_t)got) {
					end++;
					count++;
				}

				/* Handles a failed command write all operation. */
				if (command_write_all(STDOUT_FILENO,
						      buffer + used,
						      end - used) != 0)

					/* Reports operation failure. */
					return -1;
				used = end;
			}

			/*
 * Bytes after the requested newline were read
			 * speculatively.  This is harmless for named files but
			 * violates pipeline semantics, so use one-byte reads
			 * for line mode below instead. */
			if (used < (size_t)got) {
				errno = ESPIPE;

				/* Reports operation failure. */
				return -1;
			}
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the copy head lines operation. */
static int
copy_head_lines(
	int input,
	unsigned long long limit)
{
	ssize_t got;
	unsigned char byte;
	unsigned long long lines;

	/* Continue while the operation condition remains true. */
	lines = 0;
	while (lines < limit) {
		got = read(input, &byte, 1);

		/* Handles the got condition. */
		if (got == 0)
			return 0;

		/* Handles the got condition. */
		if (got < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;

			/* Reports operation failure. */
			return -1;
		}

		/* Handles a failed command write all operation. */
		if (command_write_all(STDOUT_FILENO, &byte, 1) != 0)
			return -1;

		/* Classifies the current byte. */
		if (byte == '\n')
			lines++;
	}

	/* Reports successful completion. */
	return 0;
}
