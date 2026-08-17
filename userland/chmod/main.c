/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <stdio.h>
#include <sys/stat.h>
int main(int argc, char **argv)
{
	unsigned mode; int i, failed = 0;
	if (argc < 3 || command_parse_mode(argv[1], &mode)) { fprintf(stderr, "usage: chmod mode file...\n"); return 1; }
	for (i = 2; i < argc; i++) if (chmod(argv[i], (mode_t)mode)) { command_error("chmod", argv[i]); failed = 1; }
	return failed;
}
