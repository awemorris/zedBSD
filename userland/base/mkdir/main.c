/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int
make_parents(const char *name, mode_t mode)
{
	char path[1024];
	size_t i, length = strlen(name);
	if (length == 0 || length >= sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(path, name, length + 1);
	for (i = 1; i < length; i++)
		if (path[i] == '/') {
			struct stat status;
			path[i] = '\0';
			if (mkdir(path, mode) != 0 &&
			    (errno != EEXIST || stat(path, &status) != 0 ||
			     !S_ISDIR(status.st_mode)))
				return -1;
			path[i] = '/';
		}
	if (mkdir(path, mode) != 0) {
		struct stat status;
		if (errno != EEXIST || stat(path, &status) != 0 ||
		    !S_ISDIR(status.st_mode))
			return -1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int p = 0, i = 1, failed = 0;
	mode_t mode = 0777;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}
		if (!strcmp(argv[i], "-p"))
			p = 1;
		else if (!strcmp(argv[i], "-m") && ++i < argc) {
			unsigned value;
			if (command_parse_mode(argv[i], &value))
				goto usage;
			mode = (mode_t)value;
		} else
			goto usage;
	}
	if (i == argc)
		goto usage;
	for (; i < argc; i++)
		if ((p ? make_parents(argv[i], mode) : mkdir(argv[i], mode)) !=
		    0) {
			command_error("mkdir", argv[i]);
			failed = 1;
		}
	return failed;
usage:
	fprintf(stderr, "usage: mkdir [-p] [-m mode] directory...\n");
	return 1;
}
