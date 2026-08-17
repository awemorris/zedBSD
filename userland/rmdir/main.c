/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int parents = 0, i = 1, failed = 0;
	if (i < argc && !strcmp(argv[i], "-p")) { parents = 1; i++; }
	if (i < argc && !strcmp(argv[i], "--")) i++;
	if (i == argc) { fprintf(stderr, "usage: rmdir [-p] directory...\n"); return 1; }
	for (; i < argc; i++) {
		char path[1024]; size_t n = strlen(argv[i]);
		if (n >= sizeof(path)) { command_error("rmdir", argv[i]); failed = 1; continue; }
		memcpy(path, argv[i], n + 1);
		for (;;) {
			char *slash;
			while (n > 1 && path[n - 1] == '/') path[--n] = '\0';
			if (rmdir(path) != 0) { command_error("rmdir", path); failed = 1; break; }
			if (!parents || !(slash = strrchr(path, '/'))) break;
			while (slash > path && slash[-1] == '/') slash--;
			if (slash == path)
				break;
			*slash = '\0';
			n = (size_t)(slash - path);
		}
	}
	return failed;
}
