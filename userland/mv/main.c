/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *leaf(const char *path) { const char *p = strrchr(path, '/'); return p ? p + 1 : path; }
int main(int argc, char **argv)
{
	struct stat st; int i, failed = 0, isdir;
	if (argc > 1 && !strcmp(argv[1], "--")) { argv++; argc--; }
	if (argc < 3) { fprintf(stderr, "usage: mv source... destination\n"); return 1; }
	isdir = stat(argv[argc - 1], &st) == 0 && S_ISDIR(st.st_mode);
	if (argc > 3 && !isdir) { fprintf(stderr, "mv: destination is not a directory\n"); return 1; }
	for (i = 1; i < argc - 1; i++) {
		char target[1024]; const char *to = argv[argc - 1];
		if (isdir) { if (snprintf(target, sizeof(target), "%s/%s", to, leaf(argv[i])) >= (int)sizeof(target)) { errno = ENAMETOOLONG; command_error("mv", argv[i]); failed = 1; continue; } to = target; }
		if (rename(argv[i], to) != 0) { command_error("mv", argv[i]); failed = 1; }
	}
	return failed;
}
