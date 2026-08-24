/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

int
command_write_all(int descriptor, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	while (length != 0) {
		ssize_t count = write(descriptor, bytes, length);
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

int
command_copy_fd(int input, int output)
{
	unsigned char buffer[4096];
	for (;;) {
		ssize_t count = read(input, buffer, sizeof(buffer));
		if (count == 0)
			return 0;
		if (count < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (command_write_all(output, buffer, (size_t)count) != 0)
			return -1;
	}
}

int
command_parse_ull(const char *text, unsigned long long *result)
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

int
command_parse_mode(const char *text, unsigned *result)
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

void
command_error(const char *command, const char *operand)
{
	int saved = errno;
	if (operand != NULL)
		fprintf(stderr, "%s: %s: %s\n", command, operand,
			strerror(saved));
	else
		fprintf(stderr, "%s: %s\n", command, strerror(saved));
}

long
command_read_line(FILE *stream, char **line, size_t *capacity)
{
	size_t length = 0;
	int c;
	if (*line == NULL || *capacity < 2) {
		*capacity = 128;
		*line = malloc(*capacity);
		if (*line == NULL)
			return -1;
	}
	while ((c = fgetc(stream)) != EOF) {
		if (length + 1 >= *capacity) {
			size_t next =
			    *capacity > SIZE_MAX / 2 ? SIZE_MAX : *capacity * 2;
			char *grown;
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
	if (length == 0 && c == EOF)
		return ferror(stream) ? -1 : 0;
	(*line)[length] = '\0';
	return (long)length;
}

int
command_exec(const char *name, char *const argv[])
{
	char path[512];
	const char *search, *p;
	if (strchr(name, '/') != NULL)
		return execve(name, argv, environ);
	search = getenv("PATH");
	if (search == NULL || *search == '\0')
		search = "/bin:/usr/bin";
	p = search;
	for (;;) {
		const char *end = strchr(p, ':');
		size_t n = end == NULL ? strlen(p) : (size_t)(end - p);
		if (n + strlen(name) + 2 < sizeof(path)) {
			if (n != 0)
				memcpy(path, p, n);
			else {
				path[0] = '.';
				n = 1;
			}
			path[n] = '/';
			strcpy(path + n + 1, name);
			execve(path, argv, environ);
			if (errno != ENOENT && errno != ENOTDIR)
				return -1;
		}
		if (end == NULL)
			break;
		p = end + 1;
	}
	errno = ENOENT;
	return -1;
}
