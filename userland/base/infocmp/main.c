/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/common/terminfo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	const char *directory = getenv("TERMINFO");
	const char *name = NULL;
	struct terminfo terminal;
	int option;

	while ((option = getopt(argc, argv, "A:1")) != -1) {
		if (option == 'A')
			directory = optarg;
		else if (option != '1')
			goto usage;
	}
	if (optind < argc)
		name = argv[optind++];
	if (optind != argc)
		goto usage;
	if (name == NULL)
		name = getenv("TERM");
	if (name == NULL || *name == '\0') {
		fprintf(stderr, "infocmp: TERM is not set\n");
		return 2;
	}
	if (terminfo_load(&terminal, name, directory) != 0) {
		fprintf(stderr, "infocmp: %s: %s\n", name, strerror(errno));
		return 1;
	}
	if (terminfo_write_source(stdout, &terminal, name) != 0) {
		fprintf(stderr, "infocmp: stdout: %s\n", strerror(errno));
		return 1;
	}
	return 0;

usage:
	fprintf(stderr, "usage: infocmp [-1] [-A directory] [terminal]\n");
	return 2;
}
