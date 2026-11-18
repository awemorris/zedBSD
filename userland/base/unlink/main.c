/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calls the unlink() function (POSIX XCU unlink).
 *
 *	unlink file
 *
 * One call to unlink() removes the name, with no other checks; its error
 * (a directory, a missing file) is reported as it is.  -- may come first.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs unlink.
 */
int
main(
	int argc,
	char **argv)
{
	int first;
	int status;
	int compare;

	/* -- may end the options; there are none. */
	first = 1;
	if (argc > 1) {
		compare = strcmp(argv[1], "--");
		if (compare == 0)
			first = 2;
	}

	/* Exactly one operand. */
	if (argc - first != 1) {
		fprintf(stderr, "usage: unlink file\n");
		return 1;
	}

	/* Removes the name. */
	status = unlink(argv[first]);
	if (status != 0) {
		fprintf(stderr, "unlink: cannot unlink '%s': %s\n", argv[first], strerror(errno));
		return 1;
	}

	/* Succeeded: the name is gone. */
	return 0;
}
