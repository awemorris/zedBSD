/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	char name[65];
	int short_name = 0;
	if (argc > 1 && strcmp(argv[1], "-s") == 0) {
		short_name = 1;
		argc--;
		argv++;
	}
	if (argc == 1) {
		char *dot;
		if (gethostname(name, sizeof(name)) != 0) {
			command_error("hostname", NULL);
			return 1;
		}
		if (short_name && (dot = strchr(name, '.')) != NULL)
			*dot = '\0';
		puts(name);
		return 0;
	}
	if (argc == 2 && !short_name) {
		size_t length = strlen(argv[1]);
		if (sethostname(argv[1], length) == 0)
			return 0;
		command_error("hostname", argv[1]);
		return 1;
	}
	fprintf(stderr, "usage: hostname [-s] [name]\n");
	return 2;
}
