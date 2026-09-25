/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Guest test for select() with FD_SETSIZE 1024 (ws034-p048).
 *
 * Prints one "SELECT ok|FAIL <case>" line per case and "SELECT DONE n/m".
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static int passed;
static int total;

static void
check(int ok, const char *name, int value)
{
	total++;
	if (ok)
		passed++;
	printf("SELECT %s %s (%d, errno=%d)\n", ok ? "ok" : "FAIL", name,
	    value, errno);
}

int
main(void)
{
	struct timeval zero;
	fd_set read_set;
	fd_set write_set;
	uint32_t small[1];
	uint32_t guard[3];
	int pipe_fd[2];
	int result;

	memset(&zero, 0, sizeof(zero));
	check(FD_SETSIZE == 1024, "FD_SETSIZE", FD_SETSIZE);
	check(sizeof(fd_set) == 128, "sizeof(fd_set)", (int)sizeof(fd_set));

	/* The macros reach the last word. */
	FD_ZERO(&read_set);
	FD_SET(1023, &read_set);
	FD_SET(40, &read_set);
	check(FD_ISSET(1023, &read_set) && FD_ISSET(40, &read_set) &&
	    !FD_ISSET(39, &read_set) && read_set.fds_bits[31] == 0x80000000U,
	    "macros", (int)read_set.fds_bits[1]);
	FD_CLR(1023, &read_set);
	check(!FD_ISSET(1023, &read_set), "FD_CLR", 0);

	if (pipe(pipe_fd) != 0) {
		check(0, "pipe", -1);
		return 1;
	}

	/* An empty pipe: readable no, writable yes, with nfds up to 1024. */
	FD_ZERO(&read_set);
	FD_ZERO(&write_set);
	FD_SET(pipe_fd[0], &read_set);
	FD_SET(pipe_fd[1], &write_set);
	result = select(FD_SETSIZE, &read_set, &write_set, NULL, &zero);
	check(result == 1 && !FD_ISSET(pipe_fd[0], &read_set) &&
	    FD_ISSET(pipe_fd[1], &write_set), "nfds=1024 empty pipe", result);

	/* With data: both, and nfds between 33 and 1023. */
	(void)write(pipe_fd[1], "x", 1);
	FD_ZERO(&read_set);
	FD_ZERO(&write_set);
	FD_SET(pipe_fd[0], &read_set);
	FD_SET(pipe_fd[1], &write_set);
	result = select(100, &read_set, &write_set, NULL, &zero);
	check(result == 2 && FD_ISSET(pipe_fd[0], &read_set) &&
	    FD_ISSET(pipe_fd[1], &write_set), "nfds=100 data", result);

	/* A set bit that names no open descriptor. */
	FD_ZERO(&read_set);
	FD_SET(pipe_fd[0], &read_set);
	FD_SET(700, &read_set);
	errno = 0;
	result = select(701, &read_set, NULL, NULL, &zero);
	check(result == -1 && errno == EBADF, "fd 700 EBADF", result);

	/* nfds above FD_SETSIZE. */
	errno = 0;
	result = select(FD_SETSIZE + 1, NULL, NULL, NULL, &zero);
	check(result == -1 && errno == EINVAL, "nfds=1025 EINVAL", result);

	/*
	 * A program built when fd_set was one word passes 4 bytes; the words
	 * after it must be left alone.
	 */
	guard[0] = 0;
	guard[1] = 0xdeadbeefU;
	guard[2] = 0xdeadbeefU;
	guard[0] |= 1U << pipe_fd[0];
	result = select(pipe_fd[0] + 1, (fd_set *)(void *)guard, NULL, NULL,
	    &zero);
	check(result == 1 && guard[0] == (1U << pipe_fd[0]) &&
	    guard[1] == 0xdeadbeefU && guard[2] == 0xdeadbeefU,
	    "4-byte set untouched past nfds", (int)guard[1]);

	small[0] = 1U << pipe_fd[1];
	result = select(32, NULL, (fd_set *)(void *)small, NULL, &zero);
	check(result == 1 && small[0] == (1U << pipe_fd[1]), "nfds=32 one word",
	    result);

	printf("SELECT DONE %d/%d\n", passed, total);
	return passed == total ? 0 : 1;
}
