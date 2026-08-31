/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD touch userland command.
 */

#include "userland/base/common/command.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * Runs the touch command.
 */
int
main(
	int argc,
	char **argv)
{
	int fd;
	int i, failed;
	struct timespec times[2];

	i = 1;
	failed = 0;
	times[0].tv_sec = UTIME_NOW;
	times[0].tv_nsec = 0;
	times[1].tv_sec = UTIME_NOW;
	times[1].tv_nsec = 0;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "--"))
		i++;

	/* Validates the command-line arguments. */
	if (i == argc) {
		fprintf(stderr, "usage: touch file...\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (; i < argc; i++) {
				fd = open(argv[i], O_WRONLY | O_CREAT, 0666);

		/* Validates the command-line arguments. */
		if (fd < 0 ||
		    (close(fd), utimensat(AT_FDCWD, argv[i], times, 0)) != 0) {
			command_error("touch", argv[i]);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
}
