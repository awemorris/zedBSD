/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int portable_character(unsigned char value)
{
	return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
	    (value >= '0' && value <= '9') || value == '.' || value == '_' ||
	    value == '-' || value == '/';
}

static int check_path(const char *path, int portable)
{
	const char *component = path;
	long path_limit = portable ? 256 : pathconf("/", _PC_PATH_MAX);
	long name_limit = portable ? 14 : pathconf("/", _PC_NAME_MAX);
	const unsigned char *byte;
	if (path_limit <= 0) path_limit = 256;
	if (name_limit <= 0) name_limit = 255;
	if (strlen(path) >= (size_t)path_limit) { errno = ENAMETOOLONG; return 0; }
	if (portable)
		for (byte = (const unsigned char *)path; *byte != '\0'; byte++)
			if (!portable_character(*byte)) { errno = EINVAL; return 0; }
	for (;;) {
		const char *end = strchr(component, '/');
		size_t length = end == NULL ? strlen(component) :
		    (size_t)(end - component);
		if (length > (size_t)name_limit) { errno = ENAMETOOLONG; return 0; }
		if (end == NULL) break;
		component = end + 1;
	}
	return 1;
}

int main(int argc, char **argv)
{
	int portable = 0, index = 1, failed = 0;
	if (index < argc && !strcmp(argv[index], "-p")) { portable = 1; index++; }
	if (index < argc && !strcmp(argv[index], "--")) index++;
	if (index == argc) { fprintf(stderr, "usage: pathchk [-p] pathname...\n"); return 1; }
	for (; index < argc; index++)
		if (!check_path(argv[index], portable)) {
			command_error("pathchk", argv[index]);
			failed = 1;
		}
	return failed;
}
