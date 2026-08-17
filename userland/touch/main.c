/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	int i = 1, failed = 0; struct timespec times[2] = {{UTIME_NOW, 0}, {UTIME_NOW, 0}};
	if (i < argc && !strcmp(argv[i], "--")) i++;
	if (i == argc) { fprintf(stderr, "usage: touch file...\n"); return 1; }
	for (; i < argc; i++) { int fd = open(argv[i], O_WRONLY | O_CREAT, 0666); if (fd < 0 || (close(fd), utimensat(AT_FDCWD, argv[i], times, 0)) != 0) { command_error("touch", argv[i]); failed = 1; } }
	return failed;
}
