/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	int index = 1, failed = 0;
	if (index < argc && !strcmp(argv[index], "-u"))
		index++;
	if (index < argc && !strcmp(argv[index], "--"))
		index++;
	if (index == argc) {
		if (command_copy_fd(STDIN_FILENO, STDOUT_FILENO) != 0) {
			command_error("cat", NULL);
			return 1;
		}
		return 0;
	}
	for (; index < argc; index++) {
		int descriptor;
		if (!strcmp(argv[index], "-"))
			descriptor = STDIN_FILENO;
		else {
			descriptor = open(argv[index], O_RDONLY);
			if (descriptor < 0) {
				command_error("cat", argv[index]);
				failed = 1;
				continue;
			}
		}
		if (command_copy_fd(descriptor, STDOUT_FILENO) != 0) {
			command_error("cat", argv[index]);
			failed = 1;
		}
		if (descriptor != STDIN_FILENO && close(descriptor) != 0) {
			command_error("cat", argv[index]);
			failed = 1;
		}
	}
	return failed;
}
