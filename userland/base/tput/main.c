/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD tput userland command.
 */

#include "userland/base/common/command.h"
#include "userland/base/common/terminfo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_parameter(const char *text, long *result);

/*
 * Runs the tput command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	struct terminfo terminal;
	const struct terminfo_capability *capability;
	const char *type;
	const char *directory;
	long parameters[9] = {0};
	char expanded[1024];
	int index;
	int parameter;

	type = NULL;
	directory = getenv("TERMINFO");
	index = 1;

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "-T") == 0) {
		/* Validates the command-line arguments. */
		if (++index >= argc)
			goto usage;
		type = argv[index++];
	}

	/* Validates the command-line arguments. */
	if (index >= argc)
		goto usage;

	/* Handles the type availability. */
	if (type == NULL)
		type = getenv("TERM");

	/* Handles the type availability. */
	if (type == NULL || *type == '\0') {
		fprintf(stderr, "tput: TERM is not set\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Handles a failed terminfo load operation. */
	if (terminfo_load(&terminal, type, directory) != 0) {
		fprintf(stderr, "tput: %s: unknown or invalid terminal\n",
			type);

		/* Returns the computed result. */
		return 3;
	}

	/* Handles the selected command-line operation. */
	if (strcmp(argv[index], "longname") == 0) {
		/* Validates the command-line arguments. */
		if (index + 1 != argc)
			goto usage;
		puts(terminal.name);

		/* Reports successful completion. */
		return 0;
	}
	capability = terminfo_find(&terminal, argv[index++]);

	/* Handles the capability availability. */
	if (capability == NULL) {
		fprintf(stderr, "tput: unknown capability\n");

		/* Returns the computed result. */
		return 4;
	}

	/* Process each remaining command-line operand. */
	for (parameter = 0; index < argc && parameter < 9; parameter++, index++)

		/* Validates the command-line arguments. */
		if (!parse_parameter(argv[index], &parameters[parameter])) {
			fprintf(stderr, "tput: %s: invalid numeric parameter\n",
				argv[index]);

			/* Reports operation failure. */
			return 2;
		}

	/* Validates the command-line arguments. */
	if (index != argc)
		goto usage;

	/* Handles the capability condition. */
	if (capability->kind == TERMINFO_BOOLEAN)
		return capability->number ? 0 : 1;

	/* Handles the capability condition. */
	if (capability->kind == TERMINFO_NUMBER) {
		printf("%ld\n", capability->number);

		/* Computes the function result. */
		function_result = ferror(stdout) ? 1 : 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed terminfo expand operation. */
	if (terminfo_expand(capability->string, parameters, expanded,
			    sizeof(expanded)) < 0) {
		fprintf(stderr, "tput: malformed capability expansion\n");

		/* Returns the computed result. */
		return 4;
	}

	/* Handles a failed command write all operation. */
	if (command_write_all(STDOUT_FILENO, expanded, strlen(expanded)) != 0) {
		fprintf(stderr, "tput: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;

usage:
	fprintf(stderr,
		"usage: tput [-T terminal] capability [parameter ...]\n");

	/* Reports operation failure. */
	return 2;
}

/* Supports the parse parameter operation. */
static int
parse_parameter(
	const char *text,
	long *result)
{
	char *end;
	long value;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0')
		return 0;
	errno = 0;
	value = strtol(text, &end, 10);

	/* Handles the reported system error. */
	if (errno != 0 || *end != '\0')
		return 0;
	*result = value;
	/* Reports operation failure. */
	return 1;
}
