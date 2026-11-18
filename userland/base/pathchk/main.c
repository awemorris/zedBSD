/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks pathnames (POSIX XCU pathchk).
 *
 *	pathchk [-p] [-P] pathname...
 *
 * Without -p each pathname is checked against the system: its length
 * against PATH_MAX, each component against the NAME_MAX of the directory
 * it would be in, and each existing prefix for being a searchable
 * directory.  With -p it is checked for portability instead: at most
 * _POSIX_PATH_MAX - 1 bytes, components of at most _POSIX_NAME_MAX bytes
 * from the portable filename character set, and not empty.  -P also
 * refuses an empty pathname and a component that starts with -.  A
 * pathname need not exist.  Each problem is reported and the status is 1
 * when any pathname has one.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The POSIX minimums -p checks against. */
#define PATHCHK_PORTABLE_PATH 256
#define PATHCHK_PORTABLE_NAME 14

/*
 * What the command line asks for.
 */
struct pathchk_options {
	int portable;
	int leading_dash;
};

static int check_pathname(const struct pathchk_options *options, const char *path);
static int check_portable(const char *path, const char *component, size_t length);
static int check_system(const char *path, const char *component, size_t length, char *prefix, size_t prefix_length);
static int is_portable_character(int byte);
static void usage(void);

/*
 * Runs pathchk.
 */
int
main(
	int argc,
	char **argv)
{
	struct pathchk_options options;
	int option;
	int index;
	int failed;
	int status;

	/* Reads -p and -P. */
	options.portable = 0;
	options.leading_dash = 0;
	for (;;) {
		option = getopt(argc, argv, "pP");
		if (option == -1)
			break;

		/* Records what the option asks for. */
		if (option == 'p')
			options.portable = 1;
		else if (option == 'P')
			options.leading_dash = 1;
		else
			usage();
	}

	/* At least one pathname. */
	if (optind >= argc)
		usage();

	/* Checks each; a problem is remembered and the rest go on. */
	failed = 0;
	for (index = optind; index < argc; index++) {
		status = check_pathname(&options, argv[index]);
		if (status != 0)
			failed = 1;
	}

	/* Reports whether any pathname has a problem. */
	if (failed)
		return 1;

	/* Succeeded: every pathname is fine. */
	return 0;
}

/* Checks one pathname; returns -1 after reporting its first problem. */
static int
check_pathname(
	const struct pathchk_options *options,
	const char *path)
{
	char prefix[PATH_MAX + 1];
	const char *component;
	size_t path_length;
	size_t length;
	size_t prefix_length;
	int status;

	/* An empty pathname is refused by -p and -P. */
	path_length = strlen(path);
	if (path_length == 0 && (options->portable || options->leading_dash)) {
		fprintf(stderr, "pathchk: empty file name\n");
		return -1;
	}

	/* The whole length: the portable limit, or the system's. */
	if (options->portable && path_length >= PATHCHK_PORTABLE_PATH) {
		fprintf(stderr, "pathchk: limit %d exceeded by length %lu of file name '%s'\n", PATHCHK_PORTABLE_PATH - 1, (unsigned long)path_length, path);
		return -1;
	}

	/* The system's limit without -p. */
	if (!options->portable && path_length >= PATH_MAX) {
		fprintf(stderr, "pathchk: limit %d exceeded by length %lu of file name '%s'\n", PATH_MAX - 1, (unsigned long)path_length, path);
		return -1;
	}

	/* Checks each component, with the prefix before it. */
	prefix_length = 0;
	component = path;
	if (*component == '/') {
		prefix[0] = '/';
		prefix_length = 1;
	}

	/* Takes each component in turn. */
	for (;;) {
		/* Skips the slashes before the component. */
		while (*component == '/')
			component++;
		if (*component == '\0')
			break;
		length = strcspn(component, "/");

		/* -P refuses a component that starts with -. */
		if (options->leading_dash && component[0] == '-') {
			fprintf(stderr, "pathchk: leading '-' in a component of file name '%s'\n", path);
			return -1;
		}

		/* The portable checks, or the system's. */
		if (options->portable)
			status = check_portable(path, component, length);
		else
			status = check_system(path, component, length, prefix, prefix_length);
		if (status != 0)
			return -1;

		/* The component joins the prefix. */
		if (prefix_length > 0 && prefix[prefix_length - 1] != '/') {
			prefix[prefix_length] = '/';
			prefix_length++;
		}

		/* Appends the component. */
		memcpy(prefix + prefix_length, component, length);
		prefix_length += length;
		prefix[prefix_length] = '\0';
		component += length;
	}

	/* Succeeded: no problem. */
	return 0;
}

/* Checks one component for portability: its length and its characters. */
static int
check_portable(
	const char *path,
	const char *component,
	size_t length)
{
	size_t index;
	int portable;

	/* At most _POSIX_NAME_MAX bytes. */
	if (length > PATHCHK_PORTABLE_NAME) {
		fprintf(stderr, "pathchk: limit %d exceeded by length %lu of file name component '%.*s'\n", PATHCHK_PORTABLE_NAME, (unsigned long)length, (int)length, component);
		return -1;
	}

	/* Only characters of the portable filename character set. */
	for (index = 0; index < length; index++) {
		portable = is_portable_character((unsigned char)component[index]);
		if (!portable) {
			fprintf(stderr, "pathchk: nonportable character '%c' in file name '%s'\n", component[index], path);
			return -1;
		}
	}

	/* Succeeded: the component is portable. */
	return 0;
}

/*
 * Checks one component against the system: its length against the
 * NAME_MAX of the directory it is in, when that directory exists, and the
 * directory itself for being a searchable directory.
 */
static int
check_system(
	const char *path,
	const char *component,
	size_t length,
	char *prefix,
	size_t prefix_length)
{
	struct stat status_of_prefix;
	const char *directory;
	long name_max;
	int status;
	int searchable;
	int is_directory;

	/* The directory the component is in: the prefix, or here. */
	directory = ".";
	if (prefix_length > 0) {
		prefix[prefix_length] = '\0';
		directory = prefix;
	}

	/* An existing prefix must be a searchable directory. */
	status = stat(directory, &status_of_prefix);
	if (status == 0) {
		is_directory = S_ISDIR(status_of_prefix.st_mode);
		if (!is_directory) {
			fprintf(stderr, "pathchk: '%s' is not a directory, in file name '%s'\n", directory, path);
			return -1;
		}

		/* It must be searchable. */
		searchable = access(directory, X_OK);
		if (searchable != 0) {
			fprintf(stderr, "pathchk: '%s' is not searchable, in file name '%s'\n", directory, path);
			return -1;
		}
	}

	/* The length limit of names in that directory. */
	name_max = NAME_MAX;
	if (status == 0) {
		errno = 0;
		name_max = pathconf(directory, _PC_NAME_MAX);
		if (name_max < 0)
			name_max = NAME_MAX;
	}

	/* The component must fit in that limit. */
	if ((long)length > name_max) {
		fprintf(stderr, "pathchk: limit %ld exceeded by length %lu of file name component '%.*s'\n", name_max, (unsigned long)length, (int)length, component);
		return -1;
	}

	/* Succeeded: the component fits. */
	return 0;
}

/* Tells whether a byte is in the portable filename character set. */
static int
is_portable_character(
	int byte)
{
	/* Letters, digits, period, underscore and hyphen. */
	if (byte >= 'A' && byte <= 'Z')
		return 1;
	if (byte >= 'a' && byte <= 'z')
		return 1;
	if (byte >= '0' && byte <= '9')
		return 1;
	if (byte == '.' || byte == '_' || byte == '-')
		return 1;

	/* Anything else is not portable. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: pathchk [-p] [-P] pathname...\n");
	exit(1);
}
