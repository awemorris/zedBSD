/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD uname userland command.
 */

#include "userland/base/common/command.h"

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

/*
 * Runs the uname command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	const char *option;
	int printed;
	struct utsname value;
	int all, system, node, release, version, machine;
	int index;

	/* Process each remaining command-line operand. */
	all = 0;
	system = 0;
	node = 0;
	release = 0;
	version = 0;
	machine = 0;
	for (index = 1; index < argc; index++) {
				option = argv[index];

		/* Handles the option condition. */
		if (option[0] != '-' || option[1] == '\0')
			goto usage;

		/* Continue while the operation condition remains true. */
		while (*++option != '\0') {
			/* Handles the option condition. */
			if (*option == 'a')
				all = 1;
			else if (*option == 's')
				system = 1;
			else if (*option == 'n')
				node = 1;
			else if (*option == 'r')
				release = 1;
			else if (*option == 'v')
				version = 1;
			else if (*option == 'm')
				machine = 1;
			else
				goto usage;
		}
	}

	/* Handles a failed uname operation. */
	if (uname(&value) != 0) {
		command_error("uname", NULL);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the all condition. */
	if (!all && !system && !node && !release && !version && !machine)
		system = 1;
#define FIELD(selected, member)                                                \
	do {                                                                   \
		if ((selected) || all) {                                       \
			printf("%s%s", printed++ ? " " : "", value.member);    \
		}                                                              \
	} while (0)

	printed = 0;
	FIELD(system, sysname);
	FIELD(node, nodename);
	FIELD(release, release);
	FIELD(version, version);
	FIELD(machine, machine);
	putchar('\n');

	/* Computes the function result. */
	function_result = ferror(stdout) ? 1 : 0;

	/* Returns the computed result. */
	return function_result;
usage:
	fprintf(stderr, "usage: uname [-amnrsv]\n");

	/* Reports operation failure. */
	return 1;
}
