/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD time userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * Runs the time command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	struct timespec a, b;
	pid_t p, w;
	int st;

	/* Validates the command-line arguments. */
	if (argc < 2) {
		fprintf(stderr, "usage: time command [argument ...]\n");

		/* Reports operation failure. */
		return 2;
	}
	clock_gettime(CLOCK_MONOTONIC, &a);
	p = fork();

	/* Checks the current pointer. */
	if (p < 0) {
		command_error("time", NULL);

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the current pointer. */
	if (!p) {
		command_exec(argv[1], &argv[1]);
		_exit(errno == ENOENT ? 127 : 126);
	}
	do

	/* Continue while the operation condition remains true. */
		w = waitpid(p, &st, 0);
	while (w < 0 && errno == EINTR);
	clock_gettime(CLOCK_MONOTONIC, &b);
	fprintf(stderr, "real %lld.%03ld\n", (long long)(b.tv_sec - a.tv_sec),
		(b.tv_nsec - a.tv_nsec) / 1000000);

	/* Handles the w condition. */
	if (w < 0)
		return 1;

	/* Computes the function result. */
	function_result = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);

	/* Returns the computed result. */
	return function_result;
}
