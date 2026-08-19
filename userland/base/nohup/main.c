/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: nohup command [argument ...]\n"); return 2; }
	if (signal(SIGHUP, (void (*)(int))SIG_IGN) == SIG_ERR) { command_error("nohup", NULL); return 1; }
	if (isatty(1)) {
		int fd = open("nohup.out", O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (fd < 0 || dup2(fd, 1) < 0) { command_error("nohup", "nohup.out"); return 1; }
		if (isatty(2)) dup2(fd, 2);
		if (fd > 2) close(fd);
	}
	command_exec(argv[1], &argv[1]); command_error("nohup", argv[1]);
	return errno == ENOENT ? 127 : 126;
}
