/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD which userland command.
 *
 * Reports where the shell would find a command.  The search is the one
 * execvp() performs: the directories of PATH in order, each joined with
 * the name, taking the first that is a regular file the caller may
 * execute.  A name holding a slash is not searched for; it names one
 * file, which is reported only when it is executable.
 */

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void not_found(const char *name, int silent);
static int executable(const char *path);
static int report(const char *directory, size_t length, const char *name,
		  int all, int *found);
static int search(const char *name, const char *path, int all, int silent);

/*
 * Reports one name that is not a command.
 *
 * The found names go to the standard output, which is a block buffer when
 * it is not a terminal, so it is emptied first: otherwise a reader that
 * merges the two streams sees the complaint before the answers it follows.
 */
static void
not_found(
	const char *name,
	int silent)
{
	/* Handles the silent request. */
	if (silent)
		return;
	fflush(stdout);
	fprintf(stderr, "which: %s: not found\n", name);
}

/*
 * Reports whether one path names a regular file this process may execute.
 *
 * access() answers for the real user, which is not who would run the
 * command, so the test is the effective one the kernel makes at exec:
 * faccessat() with AT_EACCESS.  A directory can carry the execute bits
 * without being a command, so the file type is checked as well.
 */
static int
executable(
	const char *path)
{
	struct stat information;

	/* Handles a name that cannot be inspected at all. */
	if (stat(path, &information) != 0)
		return 0;

	/* Handles a name that is not a plain file. */
	if (!S_ISREG(information.st_mode))
		return 0;

	/* Succeeded: the caller may run this file. */
	return faccessat(AT_FDCWD, path, X_OK, AT_EACCESS) == 0;
}

/*
 * Joins one PATH element with the name and reports it when it is a command.
 *
 * An empty element means the current directory, which is what the shell
 * does with a leading, trailing or doubled colon.
 */
static int
report(
	const char *directory,
	size_t length,
	const char *name,
	int all,
	int *found)
{
	char candidate[PATH_MAX];
	size_t name_length;

	name_length = strlen(name);

	/* Handles an element that would not fit with the name. */
	if (length + 1U + name_length + 1U > sizeof(candidate))
		return 0;

	/* Handles the empty element, which names the current directory. */
	if (length == 0) {
		candidate[0] = '.';
		length = 1U;
	} else {
		memcpy(candidate, directory, length);
	}
	candidate[length] = '/';
	memcpy(candidate + length + 1U, name, name_length + 1U);

	/* Handles a candidate that is not a command. */
	if (!executable(candidate))
		return 0;

	/* Handles the output failure. */
	if (printf("%s\n", candidate) < 0)
		return -1;
	*found = 1;

	/* Succeeded: stops unless every match was asked for. */
	return all ? 0 : 1;
}

/*
 * Searches PATH for one name and reports what was found.
 *
 * Returns zero when the name was found, one when it was not, and two when
 * the output could not be written.
 */
static int
search(
	const char *name,
	const char *path,
	int all,
	int silent)
{
	const char *element, *separator;
	size_t length;
	int found, status;

	found = 0;

	/* Handles a name that holds a slash, which is not searched for. */
	if (strchr(name, '/') != NULL) {
		/* Handles a name that is not a command. */
		if (!executable(name)) {
			not_found(name, silent);

			/* Failed: this name is not a command. */
			return 1;
		}

		/* Handles the output failure. */
		if (printf("%s\n", name) < 0)
			return 2;

		/* Succeeded: the name itself is the answer. */
		return 0;
	}
	element = path;

	/* Process each PATH element in order. */
	for (;;) {
		separator = strchr(element, ':');
		length = separator != NULL ?
			(size_t)(separator - element) : strlen(element);
		status = report(element, length, name, all, &found);

		/* Handles the output failure. */
		if (status < 0)
			return 2;

		/* Stops at the first match unless every match was asked for. */
		if (status > 0)
			break;

		/* Stops after the last element. */
		if (separator == NULL)
			break;
		element = separator + 1;
	}

	/* Reports a name that is nowhere on PATH. */
	if (!found)
		not_found(name, silent);

	/* Returns the computed result. */
	return found ? 0 : 1;
}

/*
 * Runs the which command.
 *
 * The status is zero when every name was found, one when any was not, and
 * two when the command could not be run or its output could not be written.
 */
int
main(
	int argc,
	char **argv)
{
	const char *path;
	int all, silent, index, status, result;

	all = 0;
	silent = 0;
	status = 0;

	/* Process each option. */
	for (index = 1; index < argc; index++) {
		/* Stops at the first operand. */
		if (argv[index][0] != '-' || argv[index][1] == '\0')
			break;

		/* Stops after the end-of-options marker. */
		if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		}

		/* Handles the selected option. */
		if (strcmp(argv[index], "-a") == 0) {
			all = 1;
		} else if (strcmp(argv[index], "-s") == 0) {
			silent = 1;
		} else {
			fprintf(stderr, "which: unknown option: %s\n",
				argv[index]);
			fprintf(stderr, "usage: which [-a] [-s] name ...\n");

			/* Failed: the command cannot be run. */
			return 2;
		}
	}

	/* Validates the command-line arguments. */
	if (index >= argc) {
		fprintf(stderr, "usage: which [-a] [-s] name ...\n");

		/* Failed: the command cannot be run. */
		return 2;
	}
	path = getenv("PATH");

	/*
	 * An absent PATH is not an empty PATH: the shell falls back to the
	 * system default, and so must this command, or it would answer
	 * "not found" for every command on a stripped environment.
	 */
	if (path == NULL)
		path = "/bin:/usr/bin";

	/* Process each remaining element. */
	for (; index < argc; index++) {
		result = search(argv[index], path, all, silent);

		/* Handles the output failure, which ends the command. */
		if (result == 2) {
			fprintf(stderr, "which: write error\n");

			/* Failed: the output could not be written. */
			return 2;
		}

		/* Keeps the failure while the remaining names are searched. */
		if (result != 0)
			status = 1;
	}

	/* Handles the output failure. */
	if (fflush(stdout) == EOF) {
		fprintf(stderr, "which: write error\n");

		/* Failed: the output could not be written. */
		return 2;
	}

	/* Returns the computed result. */
	return status;
}
