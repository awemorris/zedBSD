/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland command support.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

/*
 * Writes an entire buffer to a descriptor.
 */
int
command_write_all(
	int descriptor,
	const void *data,
	size_t length)
{
	const unsigned char *bytes;
	ssize_t count;

	bytes = data;

	/* Write every byte, retrying interrupted system calls. */
	while (length != 0) {
		count = write(descriptor, bytes, length);

		/* Checks the remaining item count. */
		if (count < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;

			/* Reports operation failure. */
			return -1;
		}

		/* Checks the remaining item count. */
		if (count == 0) {
			errno = EIO;

			/* Reports operation failure. */
			return -1;
		}

		bytes += count;
		length -= (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Copies all available bytes between two descriptors.
 */
int
command_copy_fd(
	int input,
	int output)
{
	unsigned char buffer[4096];
	ssize_t count;
	int result;

	/* Copy chunks until the input reaches end of file. */
	for (;;) {
		count = read(input, buffer, sizeof(buffer));

		/* Checks the remaining item count. */
		if (count == 0)
			return 0;

		/* Checks the remaining item count. */
		if (count < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;

			/* Reports operation failure. */
			return -1;
		}

		result = command_write_all(output, buffer, (size_t)count);

		/* Checks the operation result. */
		if (result != 0)
			return -1;
	}
}

/*
 * Parses an unsigned decimal integer.
 */
int
command_parse_ull(
	const char *text,
	unsigned long long *result)
{
	char *end;
	unsigned long long value;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0' || *text == '-') {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	errno = 0;
	value = strtoull(text, &end, 10);

	/* Handles the reported system error. */
	if (errno != 0 || *end != '\0') {
		/* Handles the reported system error. */
		if (errno == 0)
			errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	*result = value;

	/* Reports successful completion. */
	return 0;
}

/*
 * Parses an unsigned octal file mode.
 */
int
command_parse_mode(
	const char *text,
	unsigned *result)
{
	char *end;
	unsigned long value;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0' || *text == '-') {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	errno = 0;
	value = strtoul(text, &end, 8);

	/* Handles the reported system error. */
	if (errno != 0 || *end != '\0' || value > 07777UL) {
		/* Handles the reported system error. */
		if (errno == 0)
			errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	*result = (unsigned)value;

	/* Reports successful completion. */
	return 0;
}

/*
 * Reports a command error using the saved errno value.
 */
void
command_error(
	const char *command,
	const char *operand)
{
	int saved;

	saved = errno;

	/* Handles the operand availability. */
	if (operand != NULL) {
		fprintf(
			stderr,
			"%s: %s: %s\n",
			command,
			operand,
			strerror(saved));
	} else {
		fprintf(stderr, "%s: %s\n", command, strerror(saved));
	}
}

/*
 * Reads one arbitrarily sized line from a stream.
 */
long
command_read_line(
	FILE *stream,
	char **line,
	size_t *capacity)
{
	size_t next;
	char *grown;
	size_t length;
	int c;

	length = 0;

	/* Handles the line availability. */
	if (*line == NULL || *capacity < 2) {
		*capacity = 128;
		*line = malloc(*capacity);
		/* Handles the line availability. */
		if (*line == NULL)
			return -1;
	}

	/* Collect characters through a newline or end of file. */
	for (;;) {
		c = fgetc(stream);

		/* Handles the end-of-file condition. */
		if (c == EOF)
			break;

		/* Checks the current data length. */
		if (length + 1 >= *capacity) {
			/* Handles the capacity condition. */
			if (*capacity > SIZE_MAX / 2)
				next = SIZE_MAX;
			else
				next = *capacity * 2;

			/* Handles the next condition. */
			if (next <= *capacity) {
				errno = EOVERFLOW;

				/* Reports operation failure. */
				return -1;
			}

			grown = realloc(*line, next);

			/* Handles the grown availability. */
			if (grown == NULL)
				return -1;

			*line = grown;
			*capacity = next;
		}

		(*line)[length++] = (char)c;

		/* Classifies the current input character. */
		if (c == '\n')
			break;
	}

	/* Handles the end-of-file condition. */
	if (length == 0 && c == EOF) {
		/* Handles an operation failure. */
		if (ferror(stream))
			return -1;

		/* Reports successful completion. */
		return 0;
	}

	(*line)[length] = '\0';

	/* Returns the computed result. */
	return (long)length;
}

/*
 * Executes a command using the current search path.
 */
int
command_exec(
	const char *name,
	char *const argv[])
{
	int function_result;
	char path[512];
	const char *search;
	const char *position;
	const char *end;
	size_t name_length;
	size_t directory_length;

	/* Handles a failed strchr operation. */
	if (strchr(name, '/') != NULL) {
		/* Obtains the execve result. */
		function_result = execve(name, argv, environ);

		/* Returns the computed result. */
		return function_result;
	}

	search = getenv("PATH");

	/* Handles the search availability. */
	if (search == NULL || *search == '\0')
		search = "/bin:/usr/bin";

	name_length = strlen(name);
	position = search;

	/* Try every search-path component in order. */
	for (;;) {
		end = strchr(position, ':');

		/* Handles the end availability. */
		if (end == NULL)
			directory_length = strlen(position);
		else
			directory_length = (size_t)(end - position);

		/* Handles the directory length condition. */
		if (directory_length + name_length + 2 < sizeof(path)) {
			/* Handles the directory length condition. */
			if (directory_length != 0) {
				memcpy(path, position, directory_length);
			} else {
				path[0] = '.';
				directory_length = 1;
			}

			path[directory_length] = '/';
			strcpy(path + directory_length + 1, name);
			execve(path, argv, environ);

			/* Handles the reported system error. */
			if (errno != ENOENT && errno != ENOTDIR)
				return -1;
		}

		/* Handles the end availability. */
		if (end == NULL)
			break;

		position = end + 1;
	}

	errno = ENOENT;

	/* Reports operation failure. */
	return -1;
}
