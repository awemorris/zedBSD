/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD nohup userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

/*
 * Runs the nohup command.
 */
int
main(
	int argc,
	char **argv)
{
	int fd;

	/* Validates the command-line arguments. */
	if (argc < 2) {
		fprintf(stderr, "usage: nohup command [argument ...]\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Handles a failed signal operation. */
	if (signal(SIGHUP, (void (*)(int))SIG_IGN) == SIG_ERR) {
		command_error("nohup", NULL);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the isatty condition. */
	if (isatty(1)) {
		fd = open("nohup.out", O_WRONLY | O_CREAT | O_APPEND, 0600);

		/* Handles a failed dup2 operation. */
		if (fd < 0 || dup2(fd, 1) < 0) {
			command_error("nohup", "nohup.out");

			/* Reports operation failure. */
			return 1;
		}

		/* Handles the isatty condition. */
		if (isatty(2))
			dup2(fd, 2);

		/* Checks the file descriptor. */
		if (fd > 2)
			close(fd);
	}
	command_exec(argv[1], &argv[1]);
	command_error("nohup", argv[1]);

	/* Returns the computed result. */
	return errno == ENOENT ? 127 : 126;
}
