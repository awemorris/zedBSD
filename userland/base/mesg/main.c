/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Permits or denies messages to the terminal (POSIX XCU mesg).
 *
 *	mesg [y|n]
 *
 * The terminal is the first of standard input, standard output and
 * standard error that is one.  y lets other users write to it (write and
 * talk), by giving its group write permission; n takes the write
 * permission of the group and of others away.  The other permission bits
 * stay.  Without an operand the state is written: "is y" or "is n".
 *
 * mesg exits with 0 when messages are permitted, 1 when they are not, and
 * 2 on an error.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The status on an error. */
#define MESG_STATUS_ERROR 2

static int find_terminal(void);
static int usage(void);

/*
 * Runs mesg.
 */
int
main(
	int argc,
	char **argv)
{
	struct stat status_of_terminal;
	mode_t mode;
	int descriptor;
	int permitted;
	int result;
	int compare;
	int first;

	/* At most one operand, after an optional --. */
	first = 1;
	if (argc > 1) {
		compare = strcmp(argv[1], "--");
		if (compare == 0)
			first = 2;
	}

	/* Only y or n. */
	if (argc - first > 1)
		return usage();

	/* The terminal. */
	descriptor = find_terminal();
	if (descriptor < 0) {
		fprintf(stderr, "mesg: not a terminal\n");
		return MESG_STATUS_ERROR;
	}

	/* Its mode. */
	result = fstat(descriptor, &status_of_terminal);
	if (result != 0) {
		fprintf(stderr, "mesg: %s\n", strerror(errno));
		return MESG_STATUS_ERROR;
	}

	/* Without an operand: the state. */
	permitted = (status_of_terminal.st_mode & S_IWGRP) != 0;
	if (argc == first) {
		if (permitted)
			printf("is y\n");
		else
			printf("is n\n");
		result = fflush(stdout);
		if (result != 0)
			return MESG_STATUS_ERROR;
		return !permitted;
	}

	/* y gives the group write permission; n takes it from the group and others. */
	mode = status_of_terminal.st_mode & 07777;
	compare = strcmp(argv[first], "y");
	if (compare == 0) {
		mode |= S_IWGRP;
		permitted = 1;
	} else {
		compare = strcmp(argv[first], "n");
		if (compare != 0)
			return usage();
		mode &= ~(mode_t)(S_IWGRP | S_IWOTH);
		permitted = 0;
	}

	/* Sets it. */
	result = fchmod(descriptor, mode);
	if (result != 0) {
		fprintf(stderr, "mesg: %s\n", strerror(errno));
		return MESG_STATUS_ERROR;
	}

	/* Succeeded: the status tells the new state. */
	return !permitted;
}

/*
 * Finds the first of standard input, output and error that is a
 * terminal; returns -1 when none is.
 */
static int
find_terminal(void)
{
	int descriptor;
	int terminal;

	/* Descriptors 0, 1 and 2 in turn. */
	for (descriptor = 0; descriptor <= 2; descriptor++) {
		terminal = isatty(descriptor);
		if (terminal)
			return descriptor;
	}

	/* None. */
	return -1;
}

/* Writes the usage message and returns the error status. */
static int
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: mesg [y|n]\n");
	return MESG_STATUS_ERROR;
}
