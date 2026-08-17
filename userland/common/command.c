/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
