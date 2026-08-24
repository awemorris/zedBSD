/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
parse_increment(const char *text, int *result)
{
	char *end;
	long value;

	if (text == NULL || *text == '\0')
		return 0;
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno == ERANGE || *end != '\0' || value < INT_MIN ||
	    value > INT_MAX)
		return 0;
	*result = (int)value;
	return 1;
}

int
main(int argc, char **argv)
{
	int increment = 10;
	int index = 1;

	if (index < argc && strcmp(argv[index], "-n") == 0) {
		if (++index >= argc ||
		    !parse_increment(argv[index++], &increment))
			goto usage;
	}
	if (index < argc && strcmp(argv[index], "--") == 0)
		index++;
	if (index >= argc)
		goto usage;
	errno = 0;
	if (nice(increment) == -1 && errno != 0) {
		command_error("nice", NULL);
		return 1;
	}
	command_exec(argv[index], &argv[index]);
	command_error("nice", argv[index]);
	return errno == ENOENT ? 127 : 126;

usage:
	fprintf(stderr, "usage: nice [-n increment] utility [argument ...]\n");
	return 2;
}
