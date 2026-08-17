/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	int symbolic = 0, i = 1;
	if (i < argc && !strcmp(argv[i], "-s")) { symbolic = 1; i++; }
	if (i < argc && !strcmp(argv[i], "--")) i++;
	if (argc - i != 2) { fprintf(stderr, "usage: ln [-s] source target\n"); return 1; }
	if ((symbolic ? symlink(argv[i], argv[i + 1]) : link(argv[i], argv[i + 1])) != 0) { command_error("ln", argv[i + 1]); return 1; }
	return 0;
}
