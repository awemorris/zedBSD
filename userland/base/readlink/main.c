/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	char buffer[4096]; ssize_t size;
	if (argc != 2) { fprintf(stderr, "usage: readlink file\n"); return 1; }
	size = readlink(argv[1], buffer, sizeof(buffer));
	if (size < 0 || command_write_all(STDOUT_FILENO, buffer, (size_t)size) || command_write_all(STDOUT_FILENO, "\n", 1)) { command_error("readlink", argv[1]); return 1; }
	return 0;
}
