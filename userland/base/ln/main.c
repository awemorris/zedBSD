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
 *
 * GNU's extensions: -n (a symbolic link to a directory as the last operand
 * is a name to replace, not the directory), -T (the last operand is always
 * the link's name), -t directory, -r (a symbolic link holds the source
 * relative to the link's directory), -v (each link is written), -i (asks
 * before replacing), a single operand (a link of the same name here), the
 * long options, and options after operands (unless POSIXLY_CORRECT).
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* The codes of the long options that have no letter. */
#define OPTION_HELP	256
#define OPTION_VERSION	257

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option ln_long_options[] = {
	{"force", COMMAND_VALUE_NONE, 'f'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"interactive", COMMAND_VALUE_NONE, 'i'},
	{"logical", COMMAND_VALUE_NONE, 'L'},
	{"no-dereference", COMMAND_VALUE_NONE, 'n'},
	{"no-target-directory", COMMAND_VALUE_NONE, 'T'},
	{"physical", COMMAND_VALUE_NONE, 'P'},
	{"relative", COMMAND_VALUE_NONE, 'r'},
	{"symbolic", COMMAND_VALUE_NONE, 's'},
	{"target-directory", COMMAND_VALUE_REQUIRED, 't'},
	{"verbose", COMMAND_VALUE_NONE, 'v'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{NULL, 0, 0}
};

/* The options. */
struct options {
	int force;
	int symbolic;
	int follow;
	int no_dereference;
	int literal;
	int relative;
	int verbose;
	int interactive;
	const char *target_directory;
};

static int read_options(int argc, char **argv, struct options *options);
static int make_link(const struct options *options, const char *source, const char *target);
static int link_target_is_directory(const struct options *options, const char *path);
static char *relative_source(const char *source, const char *target);
static char *link_directory(const char *target);
static size_t common_prefix(const char *left, const char *right);
static size_t count_components(const char *path);
static char *join_relative(size_t ups, const char *rest);
static char *absolute_path(const char *path);
static void normalize_path(char *path);
static int ask(const char *target);
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
	const char *destination;
	char *target;
	int first;
	int count;
	int sources;
	int index;
	int error;
	int directory;
	int failed;

	/* The options; the operands follow argv[0]. */
	memset(&options, 0, sizeof(options));
	count = read_options(argc, argv, &options);
	first = 1;

	/* The destination: -t's directory, the last operand, or here for one. */
	destination = options.target_directory;
	sources = count;
	if (destination == NULL && count == 1 && !options.literal)
		destination = ".";
	if (destination == NULL) {
		if (count < 2)
			usage();
		destination = argv[count];
		sources = count - 1;
	}

	/* At least one source. */
	if (sources < 1)
		usage();

	/* Whether the destination is a directory the links go into. */
	directory = 0;
	if (!options.literal)
		directory = link_target_is_directory(&options, destination);
	if (options.target_directory != NULL)
		directory = 1;

	/* One source and a target that is not a directory. */
	if (sources == 1 && !directory) {
		error = make_link(&options, argv[first], destination);
		if (error != 0)
			return 1;
		return 0;
	}

	/* Several sources need a directory. */
	if (!directory) {
		fprintf(stderr, "ln: %s: not a directory\n", destination);
		return 1;
	}

	/* Each source, linked into the directory under its own name. */
	failed = 0;
	for (index = first; index < first + sources; index++) {
		target = target_in(destination, argv[index]);
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

/*
 * Reads the options; returns the number of operands, which are left in
 * argv from argv[1] on.
 */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	struct command_options scan;
	int code;

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "ln";
	scan.letters = "fisLPnTrvt:";
	scan.names = ln_long_options;
	command_options_start(&scan);

	/* Each option; the last of -L and -P wins, and of -f and -i. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;
		switch (code) {
		case 'f':
			options->force = 1;
			options->interactive = 0;
			break;
		case 'i':
			options->interactive = 1;
			options->force = 0;
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
		case 'n':
			options->no_dereference = 1;
			break;
		case 'T':
			options->literal = 1;
			break;
		case 'r':
			options->relative = 1;
			break;
		case 'v':
			options->verbose = 1;
			break;
		case 't':
			options->target_directory = scan.value;
			break;
		case OPTION_VERSION:
			printf("ln (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
		default:
			usage();
			break;
		}
	}

	/* Succeeded: the operands follow argv[0]. */
	return scan.operand_count;
}

/*
 * Reports whether the last operand is a directory the links go into: a
 * directory, or a symbolic link to one unless -n.
 */
static int
link_target_is_directory(
	const struct options *options,
	const char *path)
{
	struct stat status;
	int result;

	/* -n: a symbolic link is a name, not the directory it points to. */
	if (options->no_dereference) {
		result = lstat(path, &status);
		if (result == 0 && (status.st_mode & S_IFMT) == S_IFLNK)
			return 0;
	}

	/* The directory, following a link to one. */
	result = stat(path, &status);
	if (result != 0)
		return 0;
	if ((status.st_mode & S_IFMT) != S_IFDIR)
		return 0;

	/* Succeeded: a directory. */
	return 1;
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
	char *relative;
	int error;
	int source_known;
	int flags;
	int yes;

	/* -i asks before an existing target is replaced. */
	if (options->interactive) {
		error = lstat(target, &target_status);
		if (error == 0) {
			yes = ask(target);
			if (!yes)
				return 0;
			error = unlink(target);
			if (error != 0) {
				fprintf(stderr, "ln: %s: %s\n", target, strerror(errno));
				return -1;
			}
		}
	}

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

	/* A symbolic link holds the source as written, or relative with -r. */
	if (options->symbolic) {
		relative = NULL;
		if (options->relative) {
			relative = relative_source(source, target);
			if (relative == NULL) {
				fprintf(stderr, "ln: %s: %s\n", target, strerror(errno));
				return -1;
			}

			/* The link holds the relative path. */
			source = relative;
		}

		/* The link. */
		error = symlink(source, target);
		if (error != 0) {
			fprintf(stderr, "ln: %s: %s\n", target, strerror(errno));
			free(relative);
			return -1;
		}

		/* Succeeded: the symbolic link, said so with -v. */
		if (options->verbose)
			printf("'%s' -> '%s'\n", target, source);
		free(relative);
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

	/* Succeeded: said so with -v. */
	if (options->verbose)
		printf("'%s' => '%s'\n", target, source);
	return 0;
}

/*
 * Returns the source as a path relative to the link's directory (-r),
 * allocated: both made absolute and without . and .., the directory's
 * links followed.  Returns NULL with errno set when it cannot.
 */
static char *
relative_source(
	const char *source,
	const char *target)
{
	char *source_path;
	char *resolved;
	char *relative;
	size_t common;
	size_t ups;

	/* The source, absolute. */
	source_path = absolute_path(source);
	if (source_path == NULL)
		return NULL;

	/* The link's directory, absolute and with its links followed. */
	resolved = link_directory(target);
	if (resolved == NULL) {
		free(source_path);
		return NULL;
	}

	/* The components they share, and a .. for each of the directory's after them. */
	common = common_prefix(source_path, resolved);
	ups = count_components(resolved + common);

	/* Succeeded: the ..s, then the rest of the source. */
	relative = join_relative(ups, source_path + common);
	free(source_path);
	free(resolved);
	return relative;
}

/* Returns the directory a link goes into, absolute and with its links followed. */
static char *
link_directory(
	const char *target)
{
	char *directory;
	char *slash;
	char *resolved;

	/* The target, absolute, without its last component. */
	directory = absolute_path(target);
	if (directory == NULL)
		return NULL;
	slash = strrchr(directory, '/');
	if (slash == directory)
		slash[1] = '\0';
	else
		*slash = '\0';

	/* Succeeded: with its links followed. */
	resolved = realpath(directory, NULL);
	free(directory);
	return resolved;
}

/*
 * Returns how much two absolute paths share, as whole components: a
 * position where both have a slash or end.
 */
static size_t
common_prefix(
	const char *left,
	const char *right)
{
	size_t index;
	size_t common;

	/* The bytes they share, remembering the last slash. */
	common = 0;
	for (index = 0; left[index] != '\0'; index++) {
		if (left[index] != right[index])
			break;
		if (left[index] == '/')
			common = index;
	}

	/* Where both end or reach a slash, the whole is shared. */
	if (left[index] != '\0' && left[index] != '/')
		return common;
	if (right[index] != '\0' && right[index] != '/')
		return common;

	/* Succeeded: up to there. */
	return index;
}

/* Returns the number of components of a path (after the shared part). */
static size_t
count_components(
	const char *path)
{
	size_t count;
	size_t index;

	/* Each byte after a slash that is not a slash starts one. */
	count = 0;
	for (index = 0; path[index] != '\0'; index++) {
		if (path[index] == '/')
			continue;
		if (index == 0 || path[index - 1U] == '/')
			count++;
	}

	/* Succeeded. */
	return count;
}

/* Returns ups times ../ and then the rest of a path, allocated (. for nothing). */
static char *
join_relative(
	size_t ups,
	const char *rest)
{
	char *relative;
	size_t length;
	size_t index;

	/* The rest, without its leading slash. */
	while (*rest == '/')
		rest++;

	/* Room for the ..s, the rest and a NUL. */
	length = strlen(rest);
	relative = malloc(ups * 3U + length + 2U);
	if (relative == NULL)
		return NULL;
	relative[0] = '\0';

	/* The ..s, then the rest. */
	for (index = 0; index < ups; index++)
		strcat(relative, "../");
	strcat(relative, rest);

	/* Nothing left is ., and a last ../ has no slash. */
	length = strlen(relative);
	if (length == 0)
		strcpy(relative, ".");
	else if (relative[length - 1U] == '/')
		relative[length - 1U] = '\0';

	/* Succeeded. */
	return relative;
}

/*
 * Returns a path made absolute (from the working directory) and without
 * . and .. components, allocated; nothing is looked up.  Returns NULL with
 * errno set when it cannot.
 */
static char *
absolute_path(
	const char *path)
{
	char directory[4096];
	char *result;
	char *found;
	size_t length;

	/* An absolute path is copied. */
	length = strlen(path);
	if (path[0] == '/') {
		result = malloc(length + 1U);
		if (result == NULL)
			return NULL;
		memcpy(result, path, length + 1U);
		normalize_path(result);
		return result;
	}

	/* A relative one follows the working directory. */
	found = getcwd(directory, sizeof(directory));
	if (found == NULL)
		return NULL;
	result = malloc(strlen(directory) + length + 2U);
	if (result == NULL)
		return NULL;

	/* Succeeded: the two, joined and made plain. */
	strcpy(result, directory);
	strcat(result, "/");
	strcat(result, path);
	normalize_path(result);
	return result;
}

/* Removes the . and .. components and repeated slashes of an absolute path. */
static void
normalize_path(
	char *path)
{
	char *read;
	char *write;
	char *previous;
	size_t length;

	/* Each component, copied over the path. */
	read = path;
	write = path;
	while (*read != '\0') {
		/* The slashes before the component. */
		while (*read == '/')
			read++;
		if (*read == '\0')
			break;

		/* The component's length. */
		length = 0;
		while (read[length] != '\0' && read[length] != '/')
			length++;

		/* . is nothing. */
		if (length == 1U && read[0] == '.') {
			read += length;
			continue;
		}

		/* .. drops the component before it. */
		if (length == 2U && read[0] == '.' && read[1] == '.') {
			previous = write;
			while (previous > path && previous[-1] != '/')
				previous--;
			if (previous > path)
				previous--;
			write = previous;
			read += length;
			continue;
		}

		/* Any other component, after a slash. */
		*write = '/';
		write++;
		memmove(write, read, length);
		write += length;
		read += length;
	}

	/* The root is a slash. */
	if (write == path) {
		*write = '/';
		write++;
	}

	/* Succeeded: the path, ended. */
	*write = '\0';
}

/* Asks whether to replace a target, on standard error; returns whether yes. */
static int
ask(
	const char *target)
{
	int first;
	int character;

	/* The question. */
	fprintf(stderr, "ln: replace '%s'? ", target);
	fflush(stderr);

	/* The first character of the answer, and the rest of the line. */
	first = getchar();
	character = first;
	while (character != '\n' && character != EOF)
		character = getchar();

	/* Succeeded: y or Y is yes. */
	if (first == 'y' || first == 'Y')
		return 1;
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
	fprintf(stderr, "usage: ln [-fins] [-L|-P] [-rTv] source_file target_file\n"
		"       ln [-fins] [-L|-P] [-rv] source_file... target_dir\n"
		"       ln [-fins] [-L|-P] [-rv] -t target_dir source_file...\n");
	exit(1);
}
