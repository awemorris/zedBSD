/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Guest test for 1024 descriptors a process (ws034-p051).
 *
 * Prints one "OPENMAX ok|FAIL <case>" line per case and "OPENMAX DONE n/m".
 * Run with an argument "child" it is the exec'd half of the close-on-exec
 * case: it reports how many of the marked descriptors survived.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LIMIT 1024

static int passed;
static int total;

static void
check(int ok, const char *name, long value)
{
	total++;
	if (ok)
		passed++;
	printf("OPENMAX %s %s (%ld, errno=%d)\n", ok ? "ok" : "FAIL", name,
	    value, errno);
	fflush(stdout);
}

/* The exec'd half: counts descriptors 100..599 still open. */
static int
child_main(void)
{
	int open_count;
	int fd;

	open_count = 0;
	for (fd = 100; fd < 600; fd++) {
		if (fcntl(fd, F_GETFD) != -1)
			open_count++;
	}
	printf("CHILD open=%d fd700=%d\n", open_count,
	    fcntl(700, F_GETFD) != -1);
	return 0;
}

int
main(int argc, char **argv)
{
	struct rlimit limit;
	struct pollfd *fds;
	struct stat info;
	fd_set read_set;
	struct timeval zero;
	char line[128];
	FILE *output;
	int pipe_fd[2];
	int status;
	int count;
	int base;
	int fd;
	int last;
	pid_t child;

	if (argc > 1 && strcmp(argv[1], "child") == 0)
		return child_main();

	check(getrlimit(RLIMIT_NOFILE, &limit) == 0 &&
	    limit.rlim_cur == LIMIT && limit.rlim_max == LIMIT,
	    "RLIMIT_NOFILE", (long)limit.rlim_cur);
	check(sysconf(_SC_OPEN_MAX) == LIMIT, "sysconf(_SC_OPEN_MAX)",
	    sysconf(_SC_OPEN_MAX));
	check(getdtablesize() == LIMIT, "getdtablesize", getdtablesize());

	/* Opens descriptors until the table is full: 3..1023. */
	base = open("/dev/null", O_RDONLY);
	check(base >= 3, "first open", base);
	last = base;
	for (;;) {
		fd = dup(base);
		if (fd < 0)
			break;
		if (fd != last + 1)
			break;
		last = fd;
	}
	check(fd < 0 && errno == EMFILE && last == LIMIT - 1,
	    "fill to EMFILE", last);

	/* A freed low slot is reused first. */
	close(50);
	fd = dup(base);
	check(fd == 50, "lowest free reused", fd);

	/* dup2 onto the last slot and past it. */
	check(dup2(base, LIMIT - 1) == LIMIT - 1, "dup2 1023", LIMIT - 1);
	errno = 0;
	check(dup2(base, LIMIT) == -1 && errno == EBADF, "dup2 1024 EBADF",
	    LIMIT);
	check(fcntl(base, F_DUPFD, 1000) == -1 && errno == EMFILE,
	    "F_DUPFD full EMFILE", 0);

	/* /dev/fd names a four-digit descriptor. */
	check(stat("/dev/fd/1000", &info) == 0, "/dev/fd/1000", 0);

	/* Frees everything above 99 but a few, then checks select and poll. */
	for (fd = 100; fd < LIMIT; fd++)
		close(fd);
	check(pipe(pipe_fd) == 0 && pipe_fd[0] == 100, "pipe after close",
	    pipe_fd[0]);
	check(dup2(pipe_fd[0], 1000) == 1000, "dup2 pipe to 1000", 1000);
	check(write(pipe_fd[1], "x", 1) == 1, "write pipe", 1);
	FD_ZERO(&read_set);
	FD_SET(1000, &read_set);
	memset(&zero, 0, sizeof(zero));
	check(select(1001, &read_set, NULL, NULL, &zero) == 1 &&
	    FD_ISSET(1000, &read_set), "select fd 1000", 1000);

	fds = calloc(1001, sizeof(*fds));
	for (fd = 0; fd < 1001; fd++) {
		fds[fd].fd = fd < 100 ? fd : -1;
		fds[fd].events = POLLIN;
	}
	fds[1000].fd = 1000;
	count = poll(fds, 1001, 0);
	check(count >= 1 && (fds[1000].revents & POLLIN) != 0,
	    "poll 1001 entries", count);
	free(fds);
	close(1000);
	close(pipe_fd[0]);
	close(pipe_fd[1]);

	/* fork copies a table grown past its first size. */
	check(dup2(base, 900) == 900, "dup2 900", 900);
	child = fork();
	if (child == 0)
		_exit(fcntl(900, F_GETFD) != -1 ? 0 : 1);
	check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0, "fork keeps fd 900", status);
	close(900);

	/* close-on-exec on 500 descriptors, more than one batch. */
	for (fd = 100; fd < 600; fd++) {
		if (dup2(base, fd) != fd || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
			break;
	}
	check(fd == 600, "mark 500 close-on-exec", fd);
	check(dup2(base, 700) == 700, "dup2 700 kept", 700);
	if (pipe(pipe_fd) != 0) {
		check(0, "pipe for child", -1);
		return 1;
	}
	child = fork();
	if (child == 0) {
		dup2(pipe_fd[1], 1);
		execl(argv[0], argv[0], "child", (char *)NULL);
		_exit(127);
	}
	close(pipe_fd[1]);
	output = fdopen(pipe_fd[0], "r");
	line[0] = '\0';
	if (output != NULL && fgets(line, sizeof(line), output) == NULL)
		line[0] = '\0';
	(void)waitpid(child, &status, 0);
	check(strcmp(line, "CHILD open=0 fd700=1\n") == 0,
	    "exec closes 500 marked, keeps 700", 0);
	printf("%s", line);

	printf("OPENMAX DONE %d/%d\n", passed, total);
	return passed == total ? 0 : 1;
}
