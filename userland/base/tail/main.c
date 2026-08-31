/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD tail userland command.
 */

#include "userland/base/common/command.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tail_fd(int fd, unsigned long long wanted);

/*
 * Runs the tail command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int fd;
	unsigned long long lines;
	int i, failed;

	lines = 10;
	i = 1;
	failed = 0;

	/* Handles the selected command-line operation. */
	if (i + 1 < argc && !strcmp(argv[i], "-n")) {
		/* Validates the command-line arguments. */
		if (command_parse_ull(argv[i + 1], &lines))
			goto usage;
		i += 2;
	}

	/* Validates the command-line arguments. */
	if (i == argc) {
		/* Computes the function result. */
		function_result = tail_fd(0, lines) != 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining command-line operand. */
	for (; i < argc; i++) {
				fd = !strcmp(argv[i], "-") ? 0 : open(argv[i], O_RDONLY);

		/* Handles a failed tail fd operation. */
		if (fd < 0 || tail_fd(fd, lines)) {
			command_error("tail", argv[i]);
			failed = 1;
		}

		/* Checks the file descriptor. */
		if (fd > 0)
			close(fd);
	}

	/* Returns the computed result. */
	return failed;
usage:
	fprintf(stderr, "usage: tail [-n lines] [file...]\n");

	/* Reports operation failure. */
	return 1;
}

/* Supports the tail fd operation. */
static int
tail_fd(
	int fd,
	unsigned long long wanted)
{
	size_t next;
	unsigned char *larger;
	ssize_t n;
	unsigned char *data, buffer[4096];
	size_t used, capacity, start;

	/* Continue until the operation reaches a terminal state. */
	data = NULL;
	used = 0;
	capacity = 0;
	for (;;) {

		n = read(fd, buffer, sizeof(buffer));

		/* Checks the current item count. */
		if (n < 0) {
			free(data);

			/* Reports operation failure. */
			return -1;
		}

		/* Checks the current item count. */
		if (!n)
			break;

		/* Checks the current capacity usage. */
		if (used + (size_t)n < used) {
			free(data);

			/* Reports operation failure. */
			return -1;
		}

		/* Checks the current capacity usage. */
		if (used + (size_t)n > capacity) {
			/* Process each remaining element. */
			next = capacity ? capacity * 2 : 4096;
			while (next < used + (size_t)n)
				next *= 2;
			larger = realloc(data, next);

			/* Handles the larger condition. */
			if (!larger) {
				free(data);

				/* Reports operation failure. */
				return -1;
			}
			data = larger;
			capacity = next;
		}
		memcpy(data + used, buffer, (size_t)n);
		used += (size_t)n;
	}

	/* Handles the wanted condition. */
	if (!wanted) {
		free(data);

		/* Reports successful completion. */
		return 0;
	}
	start = used;

	/* Handles the start condition. */
	if (start && data[start - 1] == '\n')
		start--;

	/* Continue while the operation condition remains true. */
	while (start && wanted) {
		start--;

		/* Handles the data condition. */
		if (data[start] == '\n')
			wanted--;
	}

	/* Handles the start condition. */
	if (start < used && data[start] == '\n')
		start++;

	/* Handles the command write all condition. */
	if (command_write_all(1, data + start, used - start)) {
		free(data);

		/* Reports operation failure. */
		return -1;
	}
	free(data);

	/* Reports successful completion. */
	return 0;
}
