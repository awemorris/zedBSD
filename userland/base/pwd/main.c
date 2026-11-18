/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes the working directory pathname (POSIX XCU pwd).
 *
 *	pwd [-L|-P]
 *
 * -L, the default, writes $PWD when it is an absolute pathname of the
 * working directory without . or .. components; otherwise, and with -P,
 * the physical pathname without symbolic links is written.  The last of
 * -L and -P given wins.  The shell has a pwd builtin of its own; this is
 * the command that exec and find -exec reach.  Operands are ignored, as
 * the shells' builtins ignore them.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_options(int argc, char **argv, int *physical);
static int logical_is_valid(const char *path);
static int has_dot_component(const char *path);
static int write_line(const char *path);
static void usage(void);

/*
 * Runs pwd.
 */
int
main(
	int argc,
	char **argv)
{
	char physical_path[PATH_MAX];
	const char *logical_path;
	char *found;
	int physical;
	int first;
	int valid;
	int status;

	/* Reads -L and -P; pwd takes no operand, and one given is ignored. */
	first = read_options(argc, argv, &physical);
	if (first < argc)
		fprintf(stderr, "pwd: ignoring operands\n");

	/* With -L, a valid $PWD is the answer. */
	if (!physical) {
		logical_path = getenv("PWD");
		valid = logical_is_valid(logical_path);
		if (valid) {
			status = write_line(logical_path);
			return status;
		}
	}

	/* Otherwise the physical pathname of the working directory. */
	found = getcwd(physical_path, sizeof(physical_path));
	if (found == NULL) {
		fprintf(stderr, "pwd: %s\n", strerror(errno));
		return 1;
	}

	/* Writes it. */
	status = write_line(physical_path);
	return status;
}

/*
 * Reads the options and returns the index of the first operand.  Stores
 * whether the physical pathname was asked for.
 */
static int
read_options(
	int argc,
	char **argv,
	int *physical)
{
	int option;

	/* -L is the default; the last of -L and -P given wins. */
	*physical = 0;
	for (;;) {
		option = getopt(argc, argv, "LP");
		if (option == -1)
			break;

		/* Records the choice, or refuses an unknown option. */
		switch (option) {
		case 'L':
			*physical = 0;
			break;
		case 'P':
			*physical = 1;
			break;
		default:
			usage();
			break;
		}
	}

	/* Reports where the operands start. */
	return optind;
}

/*
 * Tells whether $PWD may be written for -L: an absolute pathname without
 * . or .. components that names the working directory.
 */
static int
logical_is_valid(
	const char *path)
{
	struct stat named;
	struct stat current;
	int dotted;
	int error;

	/* An unset or relative $PWD is not used. */
	if (path == NULL)
		return 0;
	if (path[0] != '/')
		return 0;

	/* A pathname with . or .. components is not used. */
	dotted = has_dot_component(path);
	if (dotted)
		return 0;

	/* It must name the working directory itself. */
	error = stat(path, &named);
	if (error != 0)
		return 0;
	error = stat(".", &current);
	if (error != 0)
		return 0;
	if (named.st_dev != current.st_dev)
		return 0;
	if (named.st_ino != current.st_ino)
		return 0;

	/* Succeeded: $PWD names the working directory. */
	return 1;
}

/* Tells whether a pathname has a . or .. component. */
static int
has_dot_component(
	const char *path)
{
	const char *component;
	size_t length;

	/* Looks at each component between slashes. */
	component = path;
	while (*component != '\0') {
		/* Skips the slashes before the component. */
		if (*component == '/') {
			component++;
			continue;
		}

		/* Measures the component. */
		length = strcspn(component, "/");

		/* A component of one or two dots is refused. */
		if (length == 1 && component[0] == '.')
			return 1;
		if (length == 2 && component[0] == '.' && component[1] == '.')
			return 1;

		/* Goes on after the component. */
		component += length;
	}

	/* No dot component. */
	return 0;
}

/*
 * Writes a pathname and a newline.  Returns 0, or 1 when standard output
 * could not be written.
 */
static int
write_line(
	const char *path)
{
	int failed;

	/* Writes the line and pushes it out. */
	printf("%s\n", path);
	failed = fflush(stdout);
	if (failed == 0)
		failed = ferror(stdout);

	/* A write that failed is an error. */
	if (failed != 0) {
		fprintf(stderr, "pwd: write error: %s\n", strerror(errno));
		return 1;
	}

	/* Succeeded: the pathname was written. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the two options. */
	fprintf(stderr, "usage: pwd [-L|-P]\n");
	exit(1);
}
