/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	unsigned long long value;
	int index, failed = 0;
	if (argc < 4 || strcmp(argv[1], "-s") != 0 ||
	    command_parse_ull(argv[2], &value) != 0 ||
	    (off_t)value < 0 || (unsigned long long)(off_t)value != value) {
		fprintf(stderr, "usage: truncate -s size file...\n");
		return 1;
	}
	for (index = 3; index < argc; index++)
		if (truncate(argv[index], (off_t)value) != 0) {
			command_error("truncate", argv[index]);
			failed = 1;
		}
	return failed;
}
