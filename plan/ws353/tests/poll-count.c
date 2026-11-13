/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The poll counter linked into a guest test client (wltest's sources made
 * into wltest-polls): the program's own poll comes first in the dynamic
 * loader's search, so libvulkan's and libwayland-client's polls come here
 * too.  Each poll that may sleep (a timeout other than 0) is counted by
 * its descriptor count, whether its timeout was short (100 ms or less) or
 * long (900 ms or more), and whether the timeout ended it; the wait itself
 * is the C library's ppoll.
 *
 * At the end the counts are one line on the standard output:
 * "POLLS waits=N two_fd_waits=N two_fd_short=N two_fd_short_timed_out=N
 * two_fd_long=N two_fd_long_timed_out=N other_timed_out=N" (two_fd: the
 * WSI's sleep on the connection and its progress pipe, and the swapchain's
 * acquire on its wake pipe and the connection).
 */

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The counts; any thread adds to them. */
static unsigned poll_waits;
static unsigned two_fd_waits;
static unsigned two_fd_short;
static unsigned two_fd_short_timed_out;
static unsigned two_fd_long;
static unsigned two_fd_long_timed_out;
static unsigned other_timed_out;

int poll(struct pollfd *descriptors, nfds_t count, int timeout);
static void poll_count_report(void) __attribute__((destructor));

/*
 * Waits as the C library's poll does, and counts the wait.
 */
int
poll(
	struct pollfd *descriptors,
	nfds_t count,
	int timeout)
{
	struct timespec limit;
	int ready;

	/* The wait: ppoll with the same limit (none for a negative timeout). */
	if (timeout < 0) {
		ready = ppoll(descriptors, count, NULL, NULL);
	} else {
		limit.tv_sec = timeout / 1000;
		limit.tv_nsec = (long)(timeout % 1000) * 1000000L;
		ready = ppoll(descriptors, count, &limit, NULL);
	}

	/* A look (timeout 0) is not a wait. */
	if (timeout == 0)
		return ready;

	/* The wait, by its kind. */
	__atomic_add_fetch(&poll_waits, 1U, __ATOMIC_RELAXED);
	if (count == 2U) {
		__atomic_add_fetch(&two_fd_waits, 1U, __ATOMIC_RELAXED);
		if (timeout > 0 && timeout <= 100) {
			__atomic_add_fetch(&two_fd_short, 1U, __ATOMIC_RELAXED);
			if (ready == 0)
				__atomic_add_fetch(&two_fd_short_timed_out, 1U, __ATOMIC_RELAXED);
		}
		if (timeout >= 900) {
			__atomic_add_fetch(&two_fd_long, 1U, __ATOMIC_RELAXED);
			if (ready == 0)
				__atomic_add_fetch(&two_fd_long_timed_out, 1U, __ATOMIC_RELAXED);
		}
	} else if (ready == 0) {
		__atomic_add_fetch(&other_timed_out, 1U, __ATOMIC_RELAXED);
	}

	/* Succeeded: the poll's own answer. */
	return ready;
}

/* Writes the counts at the program's end. */
static void
poll_count_report(
	void)
{
	char line[256];
	int length;

	/* One line, written at once. */
	length = snprintf(line, sizeof(line),
	    "POLLS waits=%u two_fd_waits=%u two_fd_short=%u two_fd_short_timed_out=%u two_fd_long=%u two_fd_long_timed_out=%u other_timed_out=%u\n",
	    poll_waits, two_fd_waits, two_fd_short, two_fd_short_timed_out, two_fd_long, two_fd_long_timed_out, other_timed_out);
	if (length > 0)
		(void)write(1, line, (size_t)length);
}
