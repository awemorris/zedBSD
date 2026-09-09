/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD df userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

static int units(unsigned long long blocks, unsigned long long fragment, unsigned long long unit, unsigned long long *converted);
static unsigned capacity(unsigned long long used, unsigned long long available);

/*
 * Runs the df command.
 */
int
main(
	int argc,
	char **argv)
{
	struct statvfs s;
	unsigned long long total, free, available, used;
	unsigned long long unit;
	const char *path;
	unsigned percent;
	int i, j, count, failed, error;

	unit = 512;
	i = 1;
	failed = 0;

	/* Selects portable output units before examining filesystem operands. */
	while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}

		/* Both forms use one complete portable record per filesystem. */
		for (j = 1; argv[i][j] != '\0'; j++) {
			if (argv[i][j] == 'k') {
				unit = 1024;
			} else if (argv[i][j] != 'P') {
				fprintf(stderr, "usage: df [-kP] [--] [file ...]\n");

				/* Rejects an unknown option before producing capacity data. */
				return 2;
			}
		}
		i++;
	}
	printf("Filesystem %llu-blocks Used Available Capacity Mounted on\n",
	       unit);

	/* The default root operand does not replace argv's terminating pointer. */
	count = argc - i;
	if (count == 0)
		count = 1;

	/* Process each remaining command-line operand. */
	for (j = 0; j < count; j++) {
		path = "/";
		if (i < argc)
			path = argv[i + j];

		/* A failed observation cannot become a zero-capacity success. */
		if (statvfs(path, &s) != 0) {
			command_error("df", path);
			failed = 1;
			continue;
		}

		/* Inconsistent counters cannot support subtraction or admission. */
		error = 0;
		if (s.f_frsize == 0)
			error = EIO;
		if (s.f_bfree > s.f_blocks)
			error = EIO;
		if (s.f_bavail > s.f_bfree)
			error = EIO;
		if (error == 0)
			error = units(s.f_blocks, s.f_frsize, unit, &total);
		if (error == 0)
			error = units(s.f_bfree, s.f_frsize, unit, &free);
		if (error == 0)
			error = units(s.f_bavail, s.f_frsize, unit, &available);
		if (error != 0) {
			errno = error;
			command_error("df", path);
			failed = 1;
			continue;
		}

		/* Validated free counters keep used plus available within total. */
		used = total - free;
		percent = capacity(used, available);
		printf("%-10s %10llu %10llu %10llu %3u%% %s\n", path,
		       total, used, available, percent, path);
	}

	/* A buffered output failure invalidates an otherwise complete report. */
	if (fflush(stdout) == EOF || ferror(stdout)) {
		if (errno == 0)
			errno = EIO;
		command_error("df", "stdout");
		failed = 1;
	}
	if (failed)
		return 1;

	/* Succeeded: every requested capacity record reached stdout. */
	return 0;
}

/* Converts without overflowing a byte product whose quotient would fit. */
static int
units(
	unsigned long long blocks,
	unsigned long long fragment,
	unsigned long long unit,
	unsigned long long *converted)
{
	unsigned long long whole, remainder, high, low, tail;

	/* Both callers select 512 or 1024, bounding the remainder product. */
	whole = fragment / unit;
	remainder = fragment % unit;
	if (whole != 0 && blocks > ULLONG_MAX / whole)
		return EOVERFLOW;
	high = blocks * whole;
	low = (blocks / unit) * remainder;
	tail = ((blocks % unit) * remainder) / unit;
	if (low > ULLONG_MAX - high)
		return EOVERFLOW;
	high += low;
	if (tail > ULLONG_MAX - high)
		return EOVERFLOW;
	*converted = high + tail;

	/* Succeeded: publishes the representable number of complete units. */
	return 0;
}

/* Finds the rounded-up percentage without multiplying a large count by 100. */
static unsigned
capacity(
	unsigned long long used,
	unsigned long long available)
{
	unsigned long long denominator, threshold;
	unsigned percent;

	/* The caller has established that this sum is representable. */
	denominator = used + available;

	/* Each threshold is floor(denominator * percent / 100), without wrap. */
	for (percent = 0; percent < 100; percent++) {
		threshold = (denominator / 100) * percent;
		threshold += ((denominator % 100) * percent) / 100;
		if (used <= threshold)
			return percent;
	}

	/* Succeeded: the remaining utilization rounds upward to 100 percent. */
	return 100;
}
