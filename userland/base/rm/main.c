/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	int force = 0, i = 1, failed = 0;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "--")) { i++; break; }
		if (!strcmp(argv[i], "-f")) force = 1;
		else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-R")) { fprintf(stderr, "rm: recursive removal is not yet available\n"); return 2; }
		else { fprintf(stderr, "usage: rm [-f] file...\n"); return 1; }
	}
	if (i == argc) return force ? 0 : 1;
	for (; i < argc; i++) if (unlink(argv[i])) { if (force && errno == ENOENT) continue; command_error("rm", argv[i]); failed = 1; }
	return failed;
}
