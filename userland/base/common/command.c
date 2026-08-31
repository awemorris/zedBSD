/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

/* Writes an entire buffer to a descriptor. */
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
		if (count < 0) {
			if (errno == EINTR)
				continue;

			return -1;
		}
		if (count == 0) {
			errno = EIO;

			return -1;
		}

		bytes += count;
		length -= (size_t)count;
	}

	return 0;
}

/* Copies all available bytes between two descriptors. */
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
		if (count == 0)
			return 0;
		if (count < 0) {
			if (errno == EINTR)
				continue;

			return -1;
		}

		result = command_write_all(output, buffer, (size_t)count);
		if (result != 0)
			return -1;
	}
}

/* Parses an unsigned decimal integer. */
int
command_parse_ull(
	const char *text,
	unsigned long long *result)
{
	char *end;
	unsigned long long value;

	if (text == NULL || *text == '\0' || *text == '-') {
		errno = EINVAL;

		return -1;
	}

	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno != 0 || *end != '\0') {
		if (errno == 0)
			errno = EINVAL;

		return -1;
	}

	*result = value;

	return 0;
}

/* Parses an unsigned octal file mode. */
int
command_parse_mode(
	const char *text,
	unsigned *result)
{
	char *end;
	unsigned long value;

	if (text == NULL || *text == '\0' || *text == '-') {
		errno = EINVAL;

		return -1;
	}

	errno = 0;
	value = strtoul(text, &end, 8);
	if (errno != 0 || *end != '\0' || value > 07777UL) {
		if (errno == 0)
			errno = EINVAL;

		return -1;
	}

	*result = (unsigned)value;

	return 0;
}

/* Reports a command error using the saved errno value. */
void
command_error(
	const char *command,
	const char *operand)
{
	int saved;

	saved = errno;
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

/* Reads one arbitrarily sized line from a stream. */
long
command_read_line(
	FILE *stream,
	char **line,
	size_t *capacity)
{
	size_t length;
	int c;

	length = 0;
	if (*line == NULL || *capacity < 2) {
		*capacity = 128;
		*line = malloc(*capacity);
		if (*line == NULL)
			return -1;
	}

	/* Collect characters through a newline or end of file. */
	for (;;) {
		c = fgetc(stream);
		if (c == EOF)
			break;

		if (length + 1 >= *capacity) {
			size_t next;
			char *grown;

			if (*capacity > SIZE_MAX / 2)
				next = SIZE_MAX;
			else
				next = *capacity * 2;
			if (next <= *capacity) {
				errno = EOVERFLOW;

				return -1;
			}

			grown = realloc(*line, next);
			if (grown == NULL)
				return -1;

			*line = grown;
			*capacity = next;
		}

		(*line)[length++] = (char)c;
		if (c == '\n')
			break;
	}

	if (length == 0 && c == EOF) {
		if (ferror(stream))
			return -1;

		return 0;
	}

	(*line)[length] = '\0';

	return (long)length;
}

/* Executes a command using the current search path. */
int
command_exec(
	const char *name,
	char *const argv[])
{
	char path[512];
	const char *search;
	const char *position;
	const char *end;
	size_t name_length;
	size_t directory_length;

	if (strchr(name, '/') != NULL)
		return execve(name, argv, environ);

	search = getenv("PATH");
	if (search == NULL || *search == '\0')
		search = "/bin:/usr/bin";

	name_length = strlen(name);
	position = search;

	/* Try every search-path component in order. */
	for (;;) {
		end = strchr(position, ':');
		if (end == NULL)
			directory_length = strlen(position);
		else
			directory_length = (size_t)(end - position);

		if (directory_length + name_length + 2 < sizeof(path)) {
			if (directory_length != 0) {
				memcpy(path, position, directory_length);
			} else {
				path[0] = '.';
				directory_length = 1;
			}

			path[directory_length] = '/';
			strcpy(path + directory_length + 1, name);
			execve(path, argv, environ);
			if (errno != ENOENT && errno != ENOTDIR)
				return -1;
		}

		if (end == NULL)
			break;

		position = end + 1;
	}

	errno = ENOENT;

	return -1;
}
