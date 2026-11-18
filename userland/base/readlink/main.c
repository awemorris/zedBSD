/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes the target of a symbolic link (POSIX XCU readlink), or with GNU's
 * options the canonical name of a file.
 *
 *	readlink [-n] file
 *	readlink [-f | -e | -m] [-nqsvz] file...
 *
 * -f follows every link in every part of the name, all of which but the
 * last must exist; -e wants the last to exist too, and -m none of them.
 * The name that comes out is absolute, without . , .. or links.  -n leaves
 * out the newline, -z ends each name with a NUL byte, -q and -s silence
 * the messages about files, -v writes them.  Options may follow operands
 * (unless POSIXLY_CORRECT is set).
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* How much of a name must exist when it is made canonical. */
#define CANONICAL_NONE		0	/* only the link itself is read */
#define CANONICAL_PARENTS	1	/* -f: all but the last part */
#define CANONICAL_ALL		2	/* -e: every part */
#define CANONICAL_MISSING	3	/* -m: no part */

/* How many links a name may go through. */
#define LINK_LIMIT 40

/* The codes of the long options that have no letter. */
#define OPTION_HELP	256
#define OPTION_VERSION	257

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option readlink_long_options[] = {
	{"canonicalize", COMMAND_VALUE_NONE, 'f'},
	{"canonicalize-existing", COMMAND_VALUE_NONE, 'e'},
	{"canonicalize-missing", COMMAND_VALUE_NONE, 'm'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"no-newline", COMMAND_VALUE_NONE, 'n'},
	{"quiet", COMMAND_VALUE_NONE, 'q'},
	{"silent", COMMAND_VALUE_NONE, 's'},
	{"verbose", COMMAND_VALUE_NONE, 'v'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{"zero", COMMAND_VALUE_NONE, 'z'},
	{NULL, 0, 0}
};

/* What the command line asks for. */
struct options {
	int canonical;
	int newline;
	int verbose;
	int end;
};

/* A name being built, and the parts still to add to it. */
struct name {
	char resolved[PATH_MAX + 1];
	char pending[PATH_MAX * 2 + 2];
};

static int read_options(int argc, char **argv, struct options *options);
static int write_link(const struct options *options, const char *path, int last);
static int canonicalize(const char *path, int mode, char *out);
static int take_part(char *pending, char *part, size_t size);
static int add_part(struct name *name, const char *part, int last, int mode, int *links);
static int follow_link(struct name *name, const char *target);
static void drop_part(char *resolved);
static void usage(void);

/*
 * Runs the readlink command.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	int count;
	int index;
	int ok;
	int failed;

	/* The options; the files follow argv[0]. */
	memset(&options, 0, sizeof(options));
	count = read_options(argc, argv, &options);
	if (count == 0)
		usage();

	/* Without canonicalizing, a single file (POSIX). */
	if (options.canonical == CANONICAL_NONE && count != 1)
		usage();

	/* Each file; the newline stays between several with -n. */
	failed = 0;
	for (index = 1; index <= count; index++) {
		ok = write_link(&options, argv[index], index == count);
		if (!ok)
			failed = 1;
	}

	/* A file whose name could not be written. */
	if (failed)
		return 1;

	/* Succeeded. */
	return 0;
}

/*
 * Reads the options; returns the number of files, which are left in argv
 * from argv[1] on.
 */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	struct command_options scan;
	const char *posix;
	int code;

	/* A newline after each name by default; messages only for POSIX. */
	options->newline = 1;
	options->end = '\n';
	posix = getenv("POSIXLY_CORRECT");
	if (posix != NULL)
		options->verbose = 1;

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "readlink";
	scan.letters = "femnqsvz";
	scan.names = readlink_long_options;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;

		/* The option of its code. */
		switch (code) {
		case 'f':
			options->canonical = CANONICAL_PARENTS;
			break;
		case 'e':
			options->canonical = CANONICAL_ALL;
			break;
		case 'm':
			options->canonical = CANONICAL_MISSING;
			break;
		case 'n':
			options->newline = 0;
			break;
		case 'q':
		case 's':
			options->verbose = 0;
			break;
		case 'v':
			options->verbose = 1;
			break;
		case 'z':
			options->end = '\0';
			break;
		case OPTION_VERSION:
			printf("readlink (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
		default:
			usage();
		}
	}

	/* Succeeded: the files follow argv[0]. */
	return scan.operand_count;
}

/*
 * Writes the target of a link, or the canonical name of a file.  last is
 * set for the last file (-n leaves out only its newline).  Returns 0 when
 * there is none.
 */
static int
write_link(
	const struct options *options,
	const char *path,
	int last)
{
	char target[PATH_MAX + 1];
	ssize_t length;
	int ok;

	/* The canonical name, or the link's target. */
	if (options->canonical != CANONICAL_NONE) {
		ok = canonicalize(path, options->canonical, target);
		if (!ok) {
			if (options->verbose)
				command_error("readlink", path);
			return 0;
		}

		/* The name's length. */
		length = (ssize_t)strlen(target);
	} else {
		length = readlink(path, target, sizeof(target) - 1U);
		if (length < 0) {
			if (options->verbose)
				command_error("readlink", path);
			return 0;
		}
	}

	/* The name, and its end unless -n on the last. */
	fwrite(target, 1, (size_t)length, stdout);
	if (options->newline || !last || options->end == '\0')
		putchar(options->end);

	/* Succeeded. */
	return 1;
}

/*
 * Makes a name canonical: absolute, without ., .. or links, with as much of
 * it existing as the mode asks.  Returns 0 with errno set when it cannot.
 */
static int
canonicalize(
	const char *path,
	int mode,
	char *out)
{
	static struct name name;
	char part[NAME_MAX + 1];
	char *cwd;
	size_t length;
	int links;
	int last;
	int taken;
	int ok;

	/* An empty name is no file. */
	if (path[0] == '\0') {
		errno = ENOENT;
		return 0;
	}

	/* A relative name starts at the working directory. */
	name.resolved[0] = '/';
	name.resolved[1] = '\0';
	if (path[0] != '/') {
		cwd = getcwd(name.resolved, sizeof(name.resolved));
		if (cwd == NULL)
			return 0;
	}

	/* The parts still to add: the whole name. */
	length = strlen(path);
	if (length >= sizeof(name.pending)) {
		errno = ENAMETOOLONG;
		return 0;
	}

	/* The copy that the parts are taken from. */
	memcpy(name.pending, path, length + 1U);

	/* Each part in turn; a link puts its target's parts in front. */
	links = 0;
	for (;;) {
		taken = take_part(name.pending, part, sizeof(part));
		if (taken < 0) {
			errno = ENAMETOOLONG;
			return 0;
		}

		/* No part left: the name is done. */
		if (taken == 0)
			break;

		/* The part, whether it is the last, and what it is. */
		last = 0;
		if (name.pending[0] == '\0')
			last = 1;
		ok = add_part(&name, part, last, mode, &links);
		if (!ok)
			return 0;
	}

	/* Succeeded: the name. */
	memcpy(out, name.resolved, strlen(name.resolved) + 1U);
	return 1;
}

/*
 * Takes the first part of a name that is left (skipping slashes), leaving
 * the rest.  Returns 1 for a part, 0 when there is none and -1 when it is
 * too long.
 */
static int
take_part(
	char *pending,
	char *part,
	size_t size)
{
	size_t start;
	size_t end;
	size_t length;

	/* The part between slashes. */
	start = 0;
	while (pending[start] == '/')
		start++;
	end = start;
	while (pending[end] != '\0' && pending[end] != '/')
		end++;

	/* No part left. */
	if (end == start) {
		pending[0] = '\0';
		return 0;
	}

	/* A part longer than a name can be. */
	length = end - start;
	if (length >= size)
		return -1;

	/* Succeeded: the part, and the rest moved to the front. */
	memcpy(part, pending + start, length);
	part[length] = '\0';
	while (pending[end] == '/')
		end++;
	memmove(pending, pending + end, strlen(pending + end) + 1U);
	return 1;
}

/*
 * Adds a part to the name built so far: . is nothing, .. goes up, a link is
 * followed.  A part that does not exist is allowed as the mode says.
 * Returns 0 with errno set when the name cannot be made.
 */
static int
add_part(
	struct name *name,
	const char *part,
	int last,
	int mode,
	int *links)
{
	char target[PATH_MAX + 1];
	struct stat status;
	size_t length;
	size_t part_length;
	ssize_t read;
	int same;
	int result;

	/* . is nothing, and .. the directory above. */
	same = strcmp(part, ".");
	if (same == 0)
		return 1;
	same = strcmp(part, "..");
	if (same == 0) {
		drop_part(name->resolved);
		return 1;
	}

	/* The part after a slash. */
	length = strlen(name->resolved);
	part_length = strlen(part);
	if (length + part_length + 2U > sizeof(name->resolved)) {
		errno = ENAMETOOLONG;
		return 0;
	}

	/* A slash, unless the name is the root. */
	if (length > 1U || name->resolved[0] != '/') {
		name->resolved[length] = '/';
		length++;
	}

	/* The part itself. */
	memcpy(name->resolved + length, part, part_length + 1U);

	/* What it is: -m does not care whether it exists. */
	result = lstat(name->resolved, &status);
	if (result != 0) {
		if (mode == CANONICAL_MISSING)
			return 1;
		if (mode == CANONICAL_PARENTS && last && errno == ENOENT)
			return 1;
		return 0;
	}

	/* A link: its target's parts come next. */
	if ((status.st_mode & S_IFMT) == S_IFLNK) {
		(*links)++;
		if (*links > LINK_LIMIT) {
			errno = ELOOP;
			return 0;
		}

		/* The target, in place of the link. */
		read = readlink(name->resolved, target, sizeof(target) - 1U);
		if (read < 0)
			return 0;
		target[read] = '\0';
		drop_part(name->resolved);
		result = follow_link(name, target);
		return result;
	}

	/* A part before others must be a directory (-m lets it be). */
	if (!last && (status.st_mode & S_IFMT) != S_IFDIR &&
	    mode != CANONICAL_MISSING) {
		errno = ENOTDIR;
		return 0;
	}

	/* Succeeded. */
	return 1;
}

/*
 * Puts a link's target in front of the parts still to add; an absolute
 * target starts again at the root.  Returns 0 when it is too long.
 */
static int
follow_link(
	struct name *name,
	const char *target)
{
	size_t target_length;
	size_t pending_length;

	/* Room for the target, a slash and the rest. */
	target_length = strlen(target);
	pending_length = strlen(name->pending);
	if (target_length + pending_length + 2U > sizeof(name->pending)) {
		errno = ENAMETOOLONG;
		return 0;
	}

	/* An absolute target starts at the root. */
	if (target[0] == '/') {
		name->resolved[0] = '/';
		name->resolved[1] = '\0';
	}

	/* Succeeded: the target, a slash, then the rest. */
	memmove(name->pending + target_length + 1U, name->pending,
		pending_length + 1U);
	memcpy(name->pending, target, target_length);
	name->pending[target_length] = '/';
	return 1;
}

/* Drops the last part of a name, stopping at the root. */
static void
drop_part(
	char *resolved)
{
	char *slash;

	/* The last slash; the root keeps its own. */
	slash = strrchr(resolved, '/');
	if (slash == NULL || slash == resolved) {
		resolved[0] = '/';
		resolved[1] = '\0';
		return;
	}

	/* Succeeded: the name ends before it. */
	*slash = '\0';
}

/* Reports the usage and ends readlink. */
static void
usage(
	void)
{
	/* The forms. */
	fprintf(stderr, "usage: readlink [-n] file\n"
		"       readlink [-f | -e | -m] [-nqsvz] file...\n");
	exit(1);
}
