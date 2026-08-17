/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	int index = 1;
	if (index < argc && !strcmp(argv[index], "--"))
		index++;
	if (index + 1 != argc) {
		fprintf(stderr, "usage: unlink file\n");
		return 1;
	}
	if (unlink(argv[index]) != 0) {
		command_error("unlink", argv[index]);
		return 1;
	}
	return 0;
}
