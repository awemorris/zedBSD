/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Changes file mode bits (POSIX XCU chmod).
 *
 *	chmod [-R] mode file...
 *
 * The mode is octal or symbolic (see mode.c).  A file operand that is a
 * symbolic link changes the file it refers to.  With -R a directory and
 * everything below it change; symbolic links met inside are neither
 * followed nor changed, since a link has no mode of its own.  A mode that
 * starts with - (as in chmod -w file) is taken as the mode, not as an
 * option.
 */

#include "userland/base/chmod/mode.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The deepest directory nesting -R follows. */
#define CHMOD_DEPTH_MAX 128

/*
 * What one run of chmod applies.
 *
 * One instance lives for the run; mask is the file mode creation mask,
 * read once at the start.
 */
struct chmod_request {
	const char *mode;
	mode_t mask;
	int recursive;
};

static int read_options(int argc, char **argv, int *recursive);
static int change_operand(const struct chmod_request *request, const char *path);
static int change_file(const struct chmod_request *request, const char *path, const struct stat *status, unsigned depth);
static int change_children(const struct chmod_request *request, const char *path, unsigned depth);
static void usage(void);

/*
 * Runs chmod.
 */
int
main(
	int argc,
	char **argv)
{
	struct chmod_request request;
	int first;
	int index;
	int failed;
	int status;
	int valid;

	/* Reads -R; the mode and the files follow. */
	first = read_options(argc, argv, &request.recursive);
	if (argc - first < 2)
		usage();

	/* Checks the mode before any file changes. */
	request.mode = argv[first];
	valid = mode_valid(request.mode);
	if (!valid) {
		fprintf(stderr, "chmod: invalid mode: '%s'\n", request.mode);
		return 1;
	}

	/* Reads the creation mask, which modes without who letters respect. */
	request.mask = umask(0);
	umask(request.mask);

	/* Changes each file; a failure is remembered and the rest go on. */
	failed = 0;
	for (index = first + 1; index < argc; index++) {
		status = change_operand(&request, argv[index]);
		if (status != 0)
			failed = 1;
	}

	/* Reports whether any file could not be changed. */
	if (failed)
		return 1;

	/* Succeeded: every file has its new mode. */
	return 0;
}

/*
 * Reads the options and returns the index of the mode operand.  An
 * argument starting with - that is a valid mode is the mode operand.
 */
static int
read_options(
	int argc,
	char **argv,
	int *recursive)
{
	const char *letter;
	int index;
	int compare;
	int valid;

	/* Reads options until the mode. */
	*recursive = 0;
	for (index = 1; index < argc; index++) {
		if (argv[index][0] != '-' || argv[index][1] == '\0')
			break;

		/* -- ends the options. */
		compare = strcmp(argv[index], "--");
		if (compare == 0) {
			index++;
			break;
		}

		/* A mode such as -w is the mode operand, not an option. */
		valid = mode_valid(argv[index]);
		if (valid)
			break;

		/* Otherwise every letter must be R. */
		for (letter = argv[index] + 1; *letter != '\0'; letter++) {
			if (*letter != 'R') {
				fprintf(stderr, "chmod: unknown option -%c\n", *letter);
				usage();
			}
		}

		/* The argument was -R. */
		*recursive = 1;
	}

	/* Reports where the mode is. */
	return index;
}

/* Changes one file operand, following a symbolic link it names. */
static int
change_operand(
	const struct chmod_request *request,
	const char *path)
{
	struct stat status_of_file;
	int status;

	/* Reads the file the operand names. */
	status = stat(path, &status_of_file);
	if (status != 0) {
		fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Changes it and, with -R, what is below it. */
	status = change_file(request, path, &status_of_file, 0);
	if (status != 0)
		return -1;

	/* Succeeded: the file has its new mode. */
	return 0;
}

/*
 * Changes one file and, with -R and a directory, everything below it.
 * The directory changes first, so that a mode giving access lets the
 * traversal in.
 */
static int
change_file(
	const struct chmod_request *request,
	const char *path,
	const struct stat *status_of_file,
	unsigned depth)
{
	mode_t mode;
	int directory;
	int failed;
	int status;

	/* Bounds the nesting -R follows. */
	if (depth >= CHMOD_DEPTH_MAX) {
		fprintf(stderr, "chmod: %s: %s\n", path, strerror(ELOOP));
		return -1;
	}

	/* Computes the new bits from the current ones. */
	directory = S_ISDIR(status_of_file->st_mode);
	status = mode_apply(request->mode, status_of_file->st_mode, request->mask, directory, &mode);
	if (status != 0) {
		fprintf(stderr, "chmod: invalid mode: '%s'\n", request->mode);
		return -1;
	}

	/* Changes the file. */
	failed = 0;
	status = chmod(path, mode);
	if (status != 0) {
		fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
		failed = 1;
	}

	/* Goes below a directory with -R. */
	if (request->recursive && directory) {
		status = change_children(request, path, depth);
		if (status != 0)
			failed = 1;
	}

	/* Reports a failure. */
	if (failed)
		return -1;

	/* Succeeded: the file and everything below it changed. */
	return 0;
}

/* Changes every entry of a directory, skipping symbolic links. */
static int
change_children(
	const struct chmod_request *request,
	const char *path,
	unsigned depth)
{
	char child[PATH_MAX + 1];
	struct stat status_of_child;
	struct dirent *entry;
	DIR *stream;
	int count;
	int failed;
	int status;
	int dot;
	int dot_dot;
	int symbolic;

	/* Opens the directory. */
	stream = opendir(path);
	if (stream == NULL) {
		fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Reads each entry, telling a read error from the end. */
	failed = 0;
	for (;;) {
		errno = 0;
		entry = readdir(stream);
		if (entry == NULL) {
			if (errno != 0) {
				fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
				failed = 1;
			}

			/* The end of the directory ends the loop. */
			break;
		}

		/* Skips the directory itself and its parent. */
		dot = strcmp(entry->d_name, ".");
		dot_dot = strcmp(entry->d_name, "..");
		if (dot == 0 || dot_dot == 0)
			continue;

		/* Names the entry. */
		count = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		if (count < 0 || (size_t)count >= sizeof(child)) {
			fprintf(stderr, "chmod: %s: %s\n", path, strerror(ENAMETOOLONG));
			failed = 1;
			continue;
		}

		/* Reads the entry itself, not what a link refers to. */
		status = lstat(child, &status_of_child);
		if (status != 0) {
			fprintf(stderr, "chmod: %s: %s\n", child, strerror(errno));
			failed = 1;
			continue;
		}

		/* A symbolic link inside the tree is left alone. */
		symbolic = S_ISLNK(status_of_child.st_mode);
		if (symbolic)
			continue;

		/* Changes the entry and what is below it. */
		status = change_file(request, child, &status_of_child, depth + 1);
		if (status != 0)
			failed = 1;
	}

	/* Closes the directory. */
	closedir(stream);

	/* Reports whether any entry failed. */
	if (failed)
		return -1;

	/* Succeeded: every entry changed. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: chmod [-R] mode file...\n");
	exit(1);
}
