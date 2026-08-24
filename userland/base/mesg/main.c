/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int
main(int argc, char **argv)
{
	struct stat st;
	if (argc > 2 ||
	    (argc == 2 && strcmp(argv[1], "y") && strcmp(argv[1], "n"))) {
		fprintf(stderr, "usage: mesg [y|n]\n");
		return 2;
	}
	if (fstat(0, &st)) {
		command_error("mesg", NULL);
		return 1;
	}
	if (argc == 1) {
		puts(st.st_mode & S_IWGRP ? "is y" : "is n");
		return st.st_mode & S_IWGRP ? 0 : 1;
	}
	if (fchmod(0, !strcmp(argv[1], "y") ? st.st_mode | S_IWGRP
					    : st.st_mode & ~S_IWGRP)) {
		command_error("mesg", NULL);
		return 1;
	}
	return 0;
}
