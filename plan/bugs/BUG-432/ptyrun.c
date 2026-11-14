/*
 * Runs a program with its standard output on a pseudo-terminal and keeps
 * what it wrote:  ptyrun OUT PROGRAM [ARGUMENT...]  writes the bytes to OUT
 * and "status N" (or "signal N") to OUT.rc.  The terminal's output
 * processing is off, so the bytes are the program's own.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	char buffer[256], name[512];
	struct termios mode;
	ssize_t got;
	pid_t child;
	int master, slave, out, status;

	if (argc < 3) {
		fprintf(stderr, "usage: ptyrun OUT PROGRAM [ARGUMENT...]\n");
		return 2;
	}
	if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
		perror("ptyrun: openpty");
		return 1;
	}
	if (tcgetattr(slave, &mode) == 0) {
		mode.c_oflag &= ~(tcflag_t)OPOST;
		(void)tcsetattr(slave, TCSANOW, &mode);
	}
	out = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		perror("ptyrun: open");
		return 1;
	}
	child = fork();
	if (child < 0) {
		perror("ptyrun: fork");
		return 1;
	}
	if (child == 0) {
		(void)dup2(slave, STDOUT_FILENO);
		(void)close(slave);
		(void)close(master);
		(void)close(out);
		execv(argv[2], argv + 2);
		_exit(127);
	}
	(void)close(slave);
	/* The master reads 0 or EIO once the child's side is closed. */
	for (;;) {
		got = read(master, buffer, sizeof(buffer));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		if (write(out, buffer, (size_t)got) != got) {
			perror("ptyrun: write");
			return 1;
		}
	}
	(void)close(out);
	if (waitpid(child, &status, 0) != child) {
		perror("ptyrun: waitpid");
		return 1;
	}
	snprintf(name, sizeof(name), "%s.rc", argv[1]);
	out = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0)
		return 1;
	if (WIFEXITED(status))
		dprintf(out, "status %d\n", WEXITSTATUS(status));
	else
		dprintf(out, "signal %d\n", WIFSIGNALED(status) ? WTERMSIG(status) : -1);
	(void)close(out);
	return 0;
}
