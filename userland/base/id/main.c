/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD id userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the id command.
 */
int
main(
	int argc,
	char **argv)
{
	int i, count;
	gid_t groups[32];

	/* Handles the selected command-line operation. */
	if (argc == 2 && !strcmp(argv[1], "-u")) {
		printf("%u\n", (unsigned)geteuid());

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the selected command-line operation. */
	if (argc == 2 && !strcmp(argv[1], "-g")) {
		printf("%u\n", (unsigned)getegid());

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the selected command-line operation. */
	if (argc == 2 && !strcmp(argv[1], "-G")) {
		count = getgroups(32, groups);

		/* Checks the remaining item count. */
		if (count < 0) {
			command_error("id", NULL);

			/* Reports operation failure. */
			return 1;
		}

		/* Process each remaining element. */
		for (i = 0; i < count; i++)
			printf("%s%u", i ? " " : "", (unsigned)groups[i]);
		putchar('\n');

		/* Reports successful completion. */
		return 0;
	}

	/* Validates the command-line arguments. */
	if (argc != 1) {
		fprintf(stderr, "usage: id [-u|-g|-G]\n");

		/* Reports operation failure. */
		return 1;
	}
	printf("uid=%u euid=%u gid=%u egid=%u", (unsigned)getuid(),
	       (unsigned)geteuid(), (unsigned)getgid(), (unsigned)getegid());
	count = getgroups(32, groups);

	/* Checks the remaining item count. */
	if (count >= 0) {
		printf(" groups=");

		/* Process each remaining element. */
		for (i = 0; i < count; i++)
			printf("%s%u", i ? "," : "", (unsigned)groups[i]);
	}
	putchar('\n');

	/* Reports successful completion. */
	return 0;
}
