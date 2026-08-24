/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>

static int
parse_increment(const char *text, int *result)
{
	char *end;
	long value;

	if (text == NULL || *text == '\0')
		return 0;
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno == ERANGE || *end != '\0' || value < INT_MIN ||
	    value > INT_MAX)
		return 0;
	*result = (int)value;
	return 1;
}

static int
parse_id(const char *text, int which, id_t *result)
{
	char *end;
	unsigned long long value;

	if (which == PRIO_USER && text[0] != '\0') {
		struct passwd *account = getpwnam(text);

		if (account != NULL) {
			*result = (id_t)account->pw_uid;
			return 1;
		}
	}
	if (text[0] == '\0' || text[0] == '-')
		return 0;
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno == ERANGE || *end != '\0' || (id_t)value != value)
		return 0;
	*result = (id_t)value;
	return 1;
}

int
main(int argc, char **argv)
{
	int which = PRIO_PROCESS;
	int increment = 0;
	int have_increment = 0;
	int failed = 0;
	int index = 1;

	while (index < argc) {
		if (strcmp(argv[index], "-g") == 0)
			which = PRIO_PGRP;
		else if (strcmp(argv[index], "-p") == 0)
			which = PRIO_PROCESS;
		else if (strcmp(argv[index], "-u") == 0)
			which = PRIO_USER;
		else if (strcmp(argv[index], "-n") == 0) {
			if (++index >= argc ||
			    !parse_increment(argv[index], &increment))
				goto usage;
			have_increment = 1;
		} else if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		} else
			break;
		index++;
	}
	if (!have_increment || index >= argc)
		goto usage;
	for (; index < argc; index++) {
		id_t identifier;
		int old_priority;
		int new_priority;

		if (!parse_id(argv[index], which, &identifier)) {
			fprintf(stderr, "renice: %s: invalid identifier\n",
				argv[index]);
			failed = 1;
			continue;
		}
		errno = 0;
		old_priority = getpriority(which, identifier);
		if (old_priority == -1 && errno != 0) {
			fprintf(stderr, "renice: %s: %s\n", argv[index],
				strerror(errno));
			failed = 1;
			continue;
		}
		if (increment > 0 && old_priority > 20 - increment)
			new_priority = 20;
		else if (increment < 0 && old_priority < -20 - increment)
			new_priority = -20;
		else
			new_priority = old_priority + increment;
		if (setpriority(which, identifier, new_priority) != 0) {
			fprintf(stderr, "renice: %s: %s\n", argv[index],
				strerror(errno));
			failed = 1;
			continue;
		}
		printf("%s: old priority %d, new priority %d\n", argv[index],
		       old_priority, new_priority);
	}
	return failed;

usage:
	fprintf(stderr, "usage: renice [-g|-p|-u] -n increment ID...\n");
	return 2;
}
