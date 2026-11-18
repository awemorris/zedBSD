/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calls the link() function (POSIX XCU link).
 *
 *	link file1 file2
 *
 * One call to link() makes file2 a new name of file1, with no other
 * checks; its error is reported as it is.  -- may come first.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs link.
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

	/* Exactly two operands. */
	if (argc - first != 2) {
		fprintf(stderr, "usage: link file1 file2\n");
		return 1;
	}

	/* Makes the new name. */
	status = link(argv[first], argv[first + 1]);
	if (status != 0) {
		fprintf(stderr, "link: cannot link '%s' to '%s': %s\n", argv[first + 1], argv[first], strerror(errno));
		return 1;
	}

	/* Succeeded: the new name exists. */
	return 0;
}
