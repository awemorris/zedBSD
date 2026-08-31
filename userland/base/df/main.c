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
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

static unsigned long long units(unsigned long long blocks, unsigned long long fragment, unsigned long long unit);

/*
 * Runs the df command.
 */
int
main(
	int argc,
	char **argv)
{
	struct statvfs s;
	unsigned long long total, free, available, used, percent;
	unsigned long long unit;
	int i, failed;

	unit = 512;
	i = 1;
	failed = 0;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-k")) {
		unit = 1024;
		i++;
	}
	printf("Filesystem %llu-blocks Used Available Capacity Mounted on\n",
	       unit);

	/* Validates the command-line arguments. */
	if (i == argc) {
		argv[argc++] = "/";
		i = argc - 1;
	}

	/* Process each remaining command-line operand. */
	for (; i < argc; i++) {
		/* Validates the command-line arguments. */
		if (statvfs(argv[i], &s)) {
			command_error("df", argv[i]);
			failed = 1;
			continue;
		}
		total = units(s.f_blocks, s.f_frsize, unit);
		free = units(s.f_bfree, s.f_frsize, unit);
		available = units(s.f_bavail, s.f_frsize, unit);
		used = total - free;
		percent = used + available
			      ? (used * 100 + (used + available) - 1) /
				    (used + available)
			      : 0;
		printf("%-10s %10llu %10llu %10llu %3llu%% %s\n", argv[i],
		       total, used, available, percent, argv[i]);
	}

	/* Returns the computed result. */
	return failed;
}

/* Supports the units operation. */
static unsigned long long
units(
	unsigned long long blocks,
	unsigned long long fragment,
	unsigned long long unit)
{
	/* Handles the fragment condition. */
	if (!fragment)
		return 0;

	/* Returns the computed result. */
	return blocks * (fragment / unit) + blocks * (fragment % unit) / unit;
}
