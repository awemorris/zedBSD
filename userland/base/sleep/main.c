/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Suspends execution for an interval (POSIX XCU sleep).
 *
 *	sleep time
 *
 * time is a non-negative number of seconds.  A fraction after a point
 * (sleep 0.1) is taken too, as GNU and the BSDs do.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int parse_interval(const char *text, struct timespec *interval);
static void usage(void);

/*
 * Runs sleep.
 */
int
main(
	int argc,
	char **argv)
{
	struct timespec request;
	struct timespec remaining;
	int valid;
	int error;

	/* One interval. */
	if (argc != 2)
		usage();
	valid = parse_interval(argv[1], &request);
	if (!valid)
		usage();

	/* The wait, resumed after a signal that does not end the process. */
	for (;;) {
		error = nanosleep(&request, &remaining);
		if (error == 0)
			break;
		if (errno != EINTR) {
			command_error("sleep", NULL);
			return 1;
		}

		/* The rest of the interval. */
		request = remaining;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Reads digits, and a point and digits after them, into seconds and
 * nanoseconds.  Returns 0 when the text is not such a number.
 */
static int
parse_interval(
	const char *text,
	struct timespec *interval)
{
	const char *cursor;
	long long seconds;
	long nanoseconds;
	long scale;
	int digits;

	/* The whole seconds. */
	seconds = 0;
	digits = 0;
	for (cursor = text; *cursor >= '0' && *cursor <= '9'; cursor++) {
		if (seconds > 100000000000LL)
			return 0;
		seconds = seconds * 10 + (*cursor - '0');
		digits++;
	}

	/* The fraction, to nanoseconds; digits past them are dropped. */
	nanoseconds = 0;
	if (*cursor == '.') {
		scale = 100000000L;
		for (cursor++; *cursor >= '0' && *cursor <= '9'; cursor++) {
			nanoseconds += (long)(*cursor - '0') * scale;
			scale /= 10;
			digits++;
		}
	}

	/* Nothing else may follow, and a digit must be there. */
	if (*cursor != '\0' || digits == 0)
		return 0;

	/* Succeeded. */
	interval->tv_sec = (time_t)seconds;
	interval->tv_nsec = nanoseconds;
	return 1;
}

/* Reports the usage and ends sleep. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: sleep seconds\n");
	exit(1);
}
