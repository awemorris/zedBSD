/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Exercises the shared base-command helpers. */
int
main(void)
{
	unsigned long long number;
	unsigned mode;
	char output[8];
	char *line;
	size_t capacity;
	FILE *stream;
	int input[2];
	int copied[2];
	long length;
	ssize_t count;
	int result;

	result = command_parse_ull("184467", &number);
	assert(result == 0);
	assert(number == 184467ULL);

	errno = 0;
	result = command_parse_ull("-1", &number);
	assert(result == -1);
	assert(errno == EINVAL);

	result = command_parse_mode("0755", &mode);
	assert(result == 0);
	assert(mode == 0755U);

	result = pipe(input);
	assert(result == 0);
	result = pipe(copied);
	assert(result == 0);
	result = command_write_all(input[1], "payload", 7);
	assert(result == 0);
	result = close(input[1]);
	assert(result == 0);
	result = command_copy_fd(input[0], copied[1]);
	assert(result == 0);
	result = close(input[0]);
	assert(result == 0);
	result = close(copied[1]);
	assert(result == 0);
	count = read(copied[0], output, sizeof(output));
	assert(count == 7);
	assert(memcmp(output, "payload", 7) == 0);
	result = close(copied[0]);
	assert(result == 0);

	stream = tmpfile();
	assert(stream != NULL);
	result = fputs("first line\nsecond", stream);
	assert(result >= 0);
	result = fseek(stream, 0, SEEK_SET);
	assert(result == 0);
	line = NULL;
	capacity = 0;
	length = command_read_line(stream, &line, &capacity);
	assert(length == 11);
	assert(strcmp(line, "first line\n") == 0);
	length = command_read_line(stream, &line, &capacity);
	assert(length == 6);
	assert(strcmp(line, "second") == 0);
	length = command_read_line(stream, &line, &capacity);
	assert(length == 0);
	free(line);
	result = fclose(stream);
	assert(result == 0);

	return 0;
}
