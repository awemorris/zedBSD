/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Removes directories (POSIX XCU rmdir).
 *
 *	rmdir [-p] dir...
 *
 * Each directory, which must be empty, is removed.  With -p the pathname
 * is then cut at its last component again and again, and each shorter
 * pathname is removed too, as rmdir a/b/c, rmdir a/b and rmdir a would;
 * the first that cannot be removed stops that operand.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int remove_operand(const char *path, int parents);
static size_t parent_length(const char *path, size_t length);
static void usage(void);

/*
 * Runs rmdir.
 */
int
main(
	int argc,
	char **argv)
{
	int option;
	int parents;
	int index;
	int failed;
	int status;

	/* Reads -p. */
	parents = 0;
	for (;;) {
		option = getopt(argc, argv, "p");
		if (option == -1)
			break;

		/* -p is the only option. */
		if (option != 'p')
			usage();
		parents = 1;
	}

	/* At least one directory is named. */
	if (optind >= argc)
		usage();

	/* Removes each operand; a failure is remembered and the rest go on. */
	failed = 0;
	for (index = optind; index < argc; index++) {
		status = remove_operand(argv[index], parents);
		if (status != 0)
			failed = 1;
	}

	/* Reports whether any directory could not be removed. */
	if (failed)
		return 1;

	/* Succeeded: every directory was removed. */
	return 0;
}

/* Removes one directory, and with -p each of its pathname's prefixes. */
static int
remove_operand(
	const char *path,
	int parents)
{
	char prefix[PATH_MAX + 1];
	size_t length;
	int status;

	/* Removes the directory itself. */
	status = rmdir(path);
	if (status != 0) {
		fprintf(stderr, "rmdir: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Without -p that is all. */
	if (!parents)
		return 0;

	/* Copies the pathname so that it can be cut. */
	length = strlen(path);
	if (length > PATH_MAX) {
		fprintf(stderr, "rmdir: %s: %s\n", path, strerror(ENAMETOOLONG));
		return -1;
	}

	/* Copies it with its terminator. */
	memcpy(prefix, path, length + 1);

	/* Removes each shorter prefix until none is left. */
	for (;;) {
		length = parent_length(prefix, length);
		if (length == 0)
			break;
		prefix[length] = '\0';

		/* The first prefix that cannot be removed stops the operand. */
		status = rmdir(prefix);
		if (status != 0) {
			fprintf(stderr, "rmdir: %s: %s\n", prefix, strerror(errno));
			return -1;
		}
	}

	/* Succeeded: the directory and its prefixes were removed. */
	return 0;
}

/*
 * Gives the length of a pathname cut before its last component, without
 * the slashes between them, or 0 when nothing is left to remove (a single
 * component, or the root).
 */
static size_t
parent_length(
	const char *path,
	size_t length)
{
	/* Drops trailing slashes. */
	while (length > 0 && path[length - 1] == '/')
		length--;

	/* Drops the last component. */
	while (length > 0 && path[length - 1] != '/')
		length--;

	/* Drops the slashes before it. */
	while (length > 0 && path[length - 1] == '/')
		length--;

	/* Reports what is left; the root is never removed. */
	return length;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: rmdir [-p] dir...\n");
	exit(1);
}
