/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD renice userland command.
 */

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>

static int parse_increment(const char *text, int *result);
static int parse_id(const char *text, int which, id_t *result);

/*
 * Runs the renice command.
 */
int
main(
	int argc,
	char **argv)
{
	id_t identifier;
	int old_priority;
	int new_priority;
	int which;
	int increment;
	int have_increment;
	int failed;
	int index;

	which = PRIO_PROCESS;
	increment = 0;
	have_increment = 0;
	failed = 0;
	index = 1;

	/* Process each remaining command-line operand. */
	while (index < argc) {
		/* Handles the selected command-line operation. */
		if (strcmp(argv[index], "-g") == 0)
			which = PRIO_PGRP;
		else if (strcmp(argv[index], "-p") == 0)
			which = PRIO_PROCESS;
		else if (strcmp(argv[index], "-u") == 0)
			which = PRIO_USER;
		else if (strcmp(argv[index], "-n") == 0) {
			/* Validates the command-line arguments. */
			if (++index >= argc ||
			    !parse_increment(argv[index], &increment))
				goto usage;
			have_increment = 1;
		} else if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		} else {
			break;
		}
		index++;
	}

	/* Validates the command-line arguments. */
	if (!have_increment || index >= argc)
		goto usage;

	/* Process each remaining command-line operand. */
	for (; index < argc; index++) {
		/* Validates the command-line arguments. */
		if (!parse_id(argv[index], which, &identifier)) {
			fprintf(stderr, "renice: %s: invalid identifier\n",
				argv[index]);
			failed = 1;
			continue;
		}
		errno = 0;
		old_priority = getpriority(which, identifier);

		/* Handles the reported system error. */
		if (old_priority == -1 && errno != 0) {
			fprintf(stderr, "renice: %s: %s\n", argv[index],
				strerror(errno));
			failed = 1;
			continue;
		}

		/* Handles the increment condition. */
		if (increment > 0 && old_priority > 20 - increment)
			new_priority = 20;
		else if (increment < 0 && old_priority < -20 - increment)
			new_priority = -20;
		else
			new_priority = old_priority + increment;

		/* Handles a failed setpriority operation. */
		if (setpriority(which, identifier, new_priority) != 0) {
			fprintf(stderr, "renice: %s: %s\n", argv[index],
				strerror(errno));
			failed = 1;
			continue;
		}
		printf("%s: old priority %d, new priority %d\n", argv[index],
		       old_priority, new_priority);
	}

	/* Returns the computed result. */
	return failed;

usage:
	fprintf(stderr, "usage: renice [-g|-p|-u] -n increment ID...\n");

	/* Reports operation failure. */
	return 2;
}

/* Supports the parse increment operation. */
static int
parse_increment(
	const char *text,
	int *result)
{
	char *end;
	long value;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0')
		return 0;
	errno = 0;
	value = strtol(text, &end, 10);

	/* Handles the reported system error. */
	if (errno == ERANGE || *end != '\0' || value < INT_MIN ||
	    value > INT_MAX)

		/* Reports successful completion. */
		return 0;
	*result = (int)value;
	/* Reports operation failure. */
	return 1;
}

/* Supports the parse id operation. */
static int
parse_id(
	const char *text,
	int which,
	id_t *result)
{
	struct passwd *account;
	char *end;
	unsigned long long value;

	/* Handles the which condition. */
	if (which == PRIO_USER && text[0] != '\0') {
		account = getpwnam(text);

		/* Handles the account availability. */
		if (account != NULL) {
			*result = (id_t)account->pw_uid;
			/* Reports operation failure. */
			return 1;
		}
	}

	/* Validates the current text. */
	if (text[0] == '\0' || text[0] == '-')
		return 0;
	errno = 0;
	value = strtoull(text, &end, 10);

	/* Handles the reported system error. */
	if (errno == ERANGE || *end != '\0' || (id_t)value != value)
		return 0;
	*result = (id_t)value;
	/* Reports operation failure. */
	return 1;
}
