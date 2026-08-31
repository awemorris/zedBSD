/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD chgrp userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <unistd.h>

/*
 * Runs the chgrp command.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned long long group;
	int i, failed;

	failed = 0;

	/* Validates the command-line arguments. */
	if (argc < 3 || command_parse_ull(argv[1], &group) ||
	    group > (unsigned long long)(gid_t)-1) {
		fprintf(stderr, "usage: chgrp gid file...\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (i = 2; i < argc; i++) {
		/* Validates the command-line arguments. */
		if (chown(argv[i], (uid_t)-1, (gid_t)group)) {
			command_error("chgrp", argv[i]);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
}
