/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "userland/base/common/command.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	char buffer[PATH_MAX + 1U];
	int newline = 1;
	int index = 1;
	ssize_t length;

	if (index < argc && !strcmp(argv[index], "-n")) {
		newline = 0;
		index++;
	}
	if (index < argc && !strcmp(argv[index], "--"))
		index++;
	if (argc - index != 1) {
		fprintf(stderr, "usage: readlink [-n] file\n");
		return 2;
	}
	length = readlink(argv[index], buffer, sizeof(buffer));
	if (length < 0 ||
	    command_write_all(STDOUT_FILENO, buffer, (size_t)length) != 0 ||
	    (newline && command_write_all(STDOUT_FILENO, "\n", 1) != 0)) {
		command_error("readlink", argv[index]);
		return 1;
	}
	return 0;
}
