/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD tty userland command.
 */

#include <stdio.h>
#include <unistd.h>

/*
 * Runs the tty command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	char *name;

	(void)argv;

	/* Validates the command-line arguments. */
	if (argc != 1) {
		fprintf(stderr, "usage: tty\n");

		/* Reports operation failure. */
		return 2;
	}
	name = ttyname(STDIN_FILENO);

	/* Handles the name availability. */
	if (name == NULL) {
		puts("not a tty");

		/* Reports operation failure. */
		return 1;
	}
	puts(name);

	/* Computes the function result. */
	function_result = ferror(stdout) ? 2 : 0;

	/* Returns the computed result. */
	return function_result;
}
