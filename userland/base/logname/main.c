/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Returns the user's login name (POSIX XCU logname).
 *
 *	logname
 *
 * The login name of the session, as getlogin() gives it, is written.
 * Without one (no login session, as under a service) logname fails.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The longest login name logname reads. */
#define LOGNAME_SIZE 256

/*
 * Runs logname.
 */
int
main(
	int argc,
	char **argv)
{
	char name[LOGNAME_SIZE];
	int status;
	int first;
	int compare;

	/* logname takes no option or operand, but -- may end the options. */
	first = 1;
	if (argc > 1) {
		compare = strcmp(argv[1], "--");
		if (compare == 0)
			first = 2;
	}

	/* Anything else is an error. */
	if (argc > first) {
		fprintf(stderr, "usage: logname\n");
		return 1;
	}

	/* Asks for the login name. */
	status = getlogin_r(name, sizeof(name));
	if (status != 0) {
		fprintf(stderr, "logname: no login name: %s\n", strerror(status));
		return 1;
	}

	/* Writes it. */
	printf("%s\n", name);
	fflush(stdout);
	status = ferror(stdout);
	if (status != 0) {
		fprintf(stderr, "logname: write error\n");
		return 1;
	}

	/* Succeeded: the name was written. */
	return 0;
}
