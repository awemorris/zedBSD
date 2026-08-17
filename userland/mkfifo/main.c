/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
int main(int argc, char **argv)
{
	mode_t mode = 0666; int i = 1, failed = 0;
	if (i < argc && !strcmp(argv[i], "-m") && ++i < argc) { unsigned value; if (command_parse_mode(argv[i], &value)) goto usage; mode = (mode_t)value; i++; }
	if (i == argc) goto usage;
	for (; i < argc; i++) if (mkfifo(argv[i], mode)) { command_error("mkfifo", argv[i]); failed = 1; }
	return failed;
usage: fprintf(stderr, "usage: mkfifo [-m mode] file...\n"); return 1;
}
