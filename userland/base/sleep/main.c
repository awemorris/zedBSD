/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD sleep userland command.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>

/*
 * Runs the sleep command.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned long long seconds;
	struct timespec request, remaining;

	/* Validates the command-line arguments. */
	if (argc != 2 || command_parse_ull(argv[1], &seconds) != 0 ||
	    (time_t)seconds < 0 ||
	    (unsigned long long)(time_t)seconds != seconds) {
		fprintf(stderr, "usage: sleep seconds\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Continue while the operation condition remains true. */
	request.tv_sec = (time_t)seconds;
	request.tv_nsec = 0;
	while (nanosleep(&request, &remaining) != 0) {
		/* Handles the reported system error. */
		if (errno != EINTR) {
			command_error("sleep", NULL);

			/* Reports operation failure. */
			return 1;
		}
		request = remaining;
	}

	/* Reports successful completion. */
	return 0;
}
