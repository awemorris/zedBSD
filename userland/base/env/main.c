/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Sets the environment for a utility (POSIX XCU env).
 *
 *	env [-i] [name=value]... [utility [argument...]]
 *
 * -i (or - alone) starts from an empty environment.  Without a utility the
 * resulting environment is written, one name=value to a line.  It is
 * installed as /usr/bin/env, the path that #! lines name; the shell has an
 * env builtin of its own.
 *
 * The new environment is an array of env's own, given to the utility with
 * execve, so that the C library's environment is never rebuilt under it.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The environment of the process. */
extern char **environ;

static void set_entry(char **entries, size_t *count, char *assignment);
static int run_utility(char **arguments, char **entries);
static int path_search(const char *name, char *path, size_t size);

/*
 * Runs env.
 */
int
main(
	int argc,
	char **argv)
{
	char **entries;
	char **entry;
	char *equals;
	size_t count;
	size_t size;
	int index;
	int empty;
	int compare;
	int status;

	/* -i, or - alone, starts from nothing; -- ends the options. */
	empty = 0;
	for (index = 1; index < argc; index++) {
		compare = strcmp(argv[index], "-i");
		if (compare == 0 || (argv[index][0] == '-' && argv[index][1] == '\0')) {
			empty = 1;
			continue;
		}

		/* -- ends the options; anything else is the first operand. */
		compare = strcmp(argv[index], "--");
		if (compare == 0)
			index++;
		break;
	}

	/* Room for the inherited entries and every assignment. */
	size = (size_t)argc + 1U;
	for (entry = environ; !empty && entry != NULL && *entry != NULL; entry++)
		size++;
	entries = calloc(size, sizeof(*entries));
	if (entries == NULL) {
		fprintf(stderr, "env: out of memory\n");
		return 125;
	}

	/* The inherited environment, unless -i. */
	count = 0;
	for (entry = environ; !empty && entry != NULL && *entry != NULL; entry++) {
		entries[count] = *entry;
		count++;
	}

	/* Each name=value before the utility, replacing an inherited one. */
	for (; index < argc; index++) {
		equals = strchr(argv[index], '=');
		if (equals == NULL)
			break;
		set_entry(entries, &count, argv[index]);
	}

	/* No utility: the environment. */
	if (index >= argc) {
		for (count = 0; entries[count] != NULL; count++)
			printf("%s\n", entries[count]);
		return 0;
	}

	/* Succeeded or not: the utility, in env's place. */
	status = run_utility(argv + index, entries);
	return status;
}

/* Sets name=value in the entries, replacing an entry of the same name. */
static void
set_entry(
	char **entries,
	size_t *count,
	char *assignment)
{
	size_t name_length;
	size_t index;
	int compare;

	/* The name is everything before the first =. */
	name_length = (size_t)(strchr(assignment, '=') - assignment);

	/* An entry of the same name is replaced. */
	for (index = 0; index < *count; index++) {
		compare = strncmp(entries[index], assignment, name_length + 1U);
		if (compare == 0) {
			entries[index] = assignment;
			return;
		}
	}

	/* Succeeded: a new entry at the end. */
	entries[*count] = assignment;
	(*count)++;
	entries[*count] = NULL;
}

/*
 * Runs a utility with the entries as its environment, found through PATH
 * when its name has no slash.  Returns only on failure: 127 when it cannot
 * be found, 126 when it cannot be run.
 */
static int
run_utility(
	char **arguments,
	char **entries)
{
	char path[4096];
	const char *slash;
	int found;

	/* A name with a slash is run as it is. */
	slash = strchr(arguments[0], '/');
	if (slash != NULL) {
		execve(arguments[0], arguments, entries);
	} else {
		found = path_search(arguments[0], path, sizeof(path));
		if (found)
			execve(path, arguments, entries);
		else
			errno = ENOENT;
	}

	/* It could not be run. */
	fprintf(stderr, "env: %s: %s\n", arguments[0], strerror(errno));
	if (errno == ENOENT)
		return 127;

	/* Found but not runnable. */
	return 126;
}

/* Finds an executable file of a name in PATH; returns whether it did. */
static int
path_search(
	const char *name,
	char *path,
	size_t size)
{
	const char *directories;
	const char *start;
	const char *end;
	size_t length;
	int written;
	int usable;

	/* PATH, or the default search path. */
	directories = getenv("PATH");
	if (directories == NULL)
		directories = "/bin:/usr/bin";

	/* Each directory; an empty one is the current directory. */
	start = directories;
	for (;;) {
		end = strchr(start, ':');
		if (end == NULL)
			end = start + strlen(start);
		length = (size_t)(end - start);
		if (length == 0)
			written = snprintf(path, size, "%s", name);
		else
			written = snprintf(path, size, "%.*s/%s", (int)length, start, name);
		if (written > 0 && (size_t)written < size) {
			usable = access(path, X_OK);
			if (usable == 0)
				return 1;
		}

		/* The last directory has been looked at. */
		if (*end == '\0')
			break;
		start = end + 1;
	}

	/* None. */
	return 0;
}
