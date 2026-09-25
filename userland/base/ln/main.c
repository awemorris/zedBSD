/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Links files (POSIX XCU ln).
 *
 *	ln [-fs] [-L|-P] source_file target_file
 *	ln [-fs] [-L|-P] source_file... target_dir
 *
 * The second form is taken when the last operand is a directory (or a
 * symbolic link to one), and each link is named after its source there.
 * -s makes symbolic links, -f removes an existing target first, and -L and
 * -P say whether a hard link to a symbolic link links what it points to or
 * the link itself (-P by default, as GNU ln does).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The options. */
struct options {
	int force;
	int symbolic;
	int follow;
};

static int read_options(int argc, char **argv, struct options *options);
static int make_link(const struct options *options, const char *source, const char *target);
static char *target_in(const char *directory, const char *source);
static void usage(void);

/*
 * Runs ln.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	struct stat status;
	char *target;
	int first;
	int count;
	int index;
	int error;
	int directory;
	int failed;

	/* The options and at least two operands. */
	memset(&options, 0, sizeof(options));
	first = read_options(argc, argv, &options);
	count = argc - first;
	if (count < 2)
		usage();

	/* Whether the last operand is a directory, following a link to one. */
	directory = 0;
	error = stat(argv[argc - 1], &status);
	if (error == 0)
		directory = S_ISDIR(status.st_mode);

	/* One source and a target that is not a directory. */
	if (count == 2 && !directory) {
		error = make_link(&options, argv[first], argv[first + 1]);
		if (error != 0)
			return 1;
		return 0;
	}

	/* Several sources need a directory. */
	if (!directory) {
		fprintf(stderr, "ln: %s: not a directory\n", argv[argc - 1]);
		return 1;
	}

	/* Each source, linked into the directory under its own name. */
	failed = 0;
	for (index = first; index < argc - 1; index++) {
		target = target_in(argv[argc - 1], argv[index]);
		error = make_link(&options, argv[index], target);
		free(target);
		if (error != 0)
			failed = 1;
	}

	/* Some link could not be made. */
	if (failed)
		return 1;

	/* Succeeded. */
	return 0;
}

/* Reads the options; returns the index of the first operand. */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	const char *word;
	const char *letter;
	int index;

	/* Each word that starts with - and is not - alone; -- ends them. */
	for (index = 1; index < argc; index++) {
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* Each letter; the last of -L and -P wins. */
		for (letter = word + 1; *letter != '\0'; letter++) {
			switch (*letter) {
			case 'f':
				options->force = 1;
				break;
			case 's':
				options->symbolic = 1;
				break;
			case 'L':
				options->follow = 1;
				break;
			case 'P':
				options->follow = 0;
				break;
			default:
				usage();
				break;
			}
		}
	}

	/* Succeeded: the first operand. */
	return index;
}

/* Makes one link, removing an existing target first with -f. */
static int
make_link(
	const struct options *options,
	const char *source,
	const char *target)
{
	struct stat source_status;
	struct stat target_status;
	int error;
	int source_known;
	int flags;

	/* With -f, an existing target goes, unless it is the source itself. */
	if (options->force) {
		error = lstat(target, &target_status);
		if (error == 0) {
			source_known = stat(source, &source_status);
			if (!options->symbolic &&
			    source_known == 0 &&
			    source_status.st_dev == target_status.st_dev &&
			    source_status.st_ino == target_status.st_ino) {
				fprintf(stderr, "ln: %s and %s are the same file\n", source, target);
				return -1;
			}

			/* The existing target goes. */
			error = unlink(target);
			if (error != 0) {
				fprintf(stderr, "ln: %s: %s\n", target, strerror(errno));
				return -1;
			}
		}
	}

	/* A symbolic link holds the source as it is written. */
	if (options->symbolic) {
		error = symlink(source, target);
		if (error != 0) {
			fprintf(stderr, "ln: %s: %s\n", target, strerror(errno));
			return -1;
		}

		/* Succeeded: the symbolic link. */
		return 0;
	}

	/* A hard link, to a symbolic link's target with -L. */
	flags = 0;
	if (options->follow)
		flags = AT_SYMLINK_FOLLOW;
	error = linkat(AT_FDCWD, source, AT_FDCWD, target, flags);
	if (error != 0) {
		fprintf(stderr, "ln: %s: %s\n", target, strerror(errno));
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Returns the name of a link in a directory: the source's last component. */
static char *
target_in(
	const char *directory,
	const char *source)
{
	const char *name;
	const char *cursor;
	char *target;
	size_t name_length;
	size_t directory_length;

	/* The last component, without the slashes after it. */
	name_length = strlen(source);
	while (name_length > 1 && source[name_length - 1U] == '/')
		name_length--;
	name = source;
	for (cursor = source; cursor < source + name_length; cursor++) {
		if (*cursor == '/' && cursor + 1 < source + name_length)
			name = cursor + 1;
	}

	/* The length of the name alone. */
	name_length -= (size_t)(name - source);

	/* Room for the directory, a slash, the name and the NUL. */
	directory_length = strlen(directory);
	target = malloc(directory_length + name_length + 2U);
	if (target == NULL) {
		fprintf(stderr, "ln: out of memory\n");
		exit(1);
	}

	/* Succeeded: directory/name. */
	memcpy(target, directory, directory_length);
	target[directory_length] = '/';
	memcpy(target + directory_length + 1U, name, name_length);
	target[directory_length + 1U + name_length] = '\0';
	return target;
}

/* Reports the usage and ends ln. */
static void
usage(
	void)
{
	/* The forms. */
	fprintf(stderr, "usage: ln [-fs] [-L|-P] source_file target_file\n"
		"       ln [-fs] [-L|-P] source_file... target_dir\n");
	exit(1);
}
