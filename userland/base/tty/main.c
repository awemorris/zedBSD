/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Returns the user's terminal name (POSIX XCU tty).
 *
 *	tty
 *
 * The name of the terminal on standard input is written, or "not a tty".
 * The status is 0 for a terminal, 1 for none, and 2 for a wrong command
 * line; the historical -s writes nothing and only sets the status.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* The status for a wrong command line. */
#define TTY_STATUS_USAGE 2

/* The status for a failed write. */
#define TTY_STATUS_WRITE 3

static void usage(void);

/*
 * Runs tty.
 */
int
main(
	int argc,
	char **argv)
{
	char *name;
	int option;
	int silent;
	int status;

	/* Reads -s; tty takes no operand. */
	silent = 0;
	for (;;) {
		option = getopt(argc, argv, "s");
		if (option == -1)
			break;

		/* -s is the only option. */
		if (option != 's')
			usage();
		silent = 1;
	}

	/* An operand is an error. */
	if (optind < argc)
		usage();

	/* Asks for the name of the terminal on standard input. */
	name = ttyname(STDIN_FILENO);
	if (silent) {
		if (name == NULL)
			return 1;
		return 0;
	}

	/* Writes the name, or says there is none. */
	if (name == NULL)
		printf("not a tty\n");
	else
		printf("%s\n", name);

	/* A failed write is an error of its own. */
	fflush(stdout);
	status = ferror(stdout);
	if (status != 0)
		return TTY_STATUS_WRITE;

	/* No terminal is status 1. */
	if (name == NULL)
		return 1;

	/* Succeeded: the terminal was named. */
	return 0;
}

/* Writes the usage message and exits with the usage status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: tty\n");
	exit(TTY_STATUS_USAGE);
}
