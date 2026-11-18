/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Moves files (POSIX XCU mv).
 *
 *	mv [-if] source_file target_file
 *	mv [-if] source_file... target_dir
 *
 * Each source is renamed to the target pathname, or into the target
 * directory under its own last component.  An existing destination is
 * asked about when -i is given, or when it cannot be written and standard
 * input is a terminal, unless -f is given; the last of -i and -f wins.
 * When the destination is on another file system the source is copied
 * with its owner, permission bits, times and symbolic links, as by
 * cp -pRP, and then removed.
 *
 * zedBSD also accepts -n, --no-clobber and --update=none (keep existing
 * destinations, atomically), --update=none-fail (refuse them), --force,
 * and -T and --no-target-directory (the target is always a pathname).
 *
 * GNU's extensions are taken too (ws045): -v (--verbose) writes each move,
 * -t directory (--target-directory) names the directory before the
 * sources, -u (--update, --update=older) keeps a destination that is not
 * older than the source, --update=all replaces as usual,
 * --strip-trailing-slashes is taken, and the long forms of the letters;
 * options may follow operands unless POSIXLY_CORRECT is set.
 */

#include "userland/base/common/command.h"
#include "userland/base/cp/copy.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* The codes of the long options that have no letter. */
#define OPTION_HELP 256
#define OPTION_IGNORED 257
#define OPTION_UPDATE 258
#define OPTION_VERSION 259

/*
 * The options written in full, read by the scan of the command line; a
 * long option shares its code with its letter.
 */
static const struct command_long_option mv_long_options[] = {
	{"force", COMMAND_VALUE_NONE, 'f'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"interactive", COMMAND_VALUE_NONE, 'i'},
	{"no-clobber", COMMAND_VALUE_NONE, 'n'},
	{"no-target-directory", COMMAND_VALUE_NONE, 'T'},
	{"strip-trailing-slashes", COMMAND_VALUE_NONE, OPTION_IGNORED},
	{"target-directory", COMMAND_VALUE_REQUIRED, 't'},
	{"update", COMMAND_VALUE_OPTIONAL, OPTION_UPDATE},
	{"verbose", COMMAND_VALUE_NONE, 'v'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{NULL, 0, 0}
};

/*
 * What the command line asks for.
 *
 * One instance lives for the run.  prompt is 1 for -i, 0 for -f and -1
 * when neither was given; terminal says whether standard input is one.
 * keep_newer is -u; directory is -t's.
 */
struct mv_options {
	int prompt;
	int no_replace;
	int conflict_fails;
	int keep_newer;
	int literal;
	int verbose;
	int terminal;
	const char *directory;
};

static int read_options(int argc, char **argv, struct mv_options *options);
static int read_letter(int letter, const char *value, struct mv_options *options);
static void apply_update(struct mv_options *options, const char *word);
static int destination_newer(const struct stat *from, const struct stat *existing);
static int move_into(const struct mv_options *options, const char *source, const char *directory);
static int move_operand(const struct mv_options *options, const char *source, const char *destination);
static int confirm_replace(const struct mv_options *options, const char *destination);
static int rename_source(const struct mv_options *options, const char *source, const char *destination);
static int move_across(const char *source, const char *destination, const struct stat *from);
static int clear_destination(const char *source, const char *destination, const struct stat *from);
static int remove_tree(const char *path);
static int remove_children(const char *path);
static void usage(void);

/*
 * Runs mv.
 */
int
main(
	int argc,
	char **argv)
{
	struct mv_options options;
	struct stat status_of_target;
	const char *target;
	int count;
	int last;
	int index;
	int directory;
	int failed;
	int status;

	/*
	 * Reads the options; the operands are left from argv[1] on: the
	 * sources, then the target unless -t named it.
	 */
	count = read_options(argc, argv, &options);
	last = count;
	target = options.directory;
	if (target == NULL) {
		target = argv[count];
		last = count - 1;
	}

	/* The number of sources. */
	count = last;
	if (count < 1)
		usage();
	options.terminal = isatty(STDIN_FILENO);

	/* Learns whether the target is an existing directory. */
	directory = 0;
	if (!options.literal) {
		status = stat(target, &status_of_target);
		if (status == 0)
			directory = S_ISDIR(status_of_target.st_mode);
	}

	/* Several sources, or -t, need a directory to go into. */
	if ((count > 1 || options.directory != NULL) && !directory) {
		fprintf(stderr, "mv: target '%s' is not a directory\n", target);
		return 1;
	}

	/* Moves each source; a failure is remembered and the rest go on. */
	failed = 0;
	for (index = 1; index <= last; index++) {
		if (directory)
			status = move_into(&options, argv[index], target);
		else
			status = move_operand(&options, argv[index], target);
		if (status != 0)
			failed = 1;
	}

	/* Reports whether any source could not be moved. */
	if (failed)
		return 1;

	/* Succeeded: every source was moved. */
	return 0;
}

/*
 * Reads the options and returns the number of operands, which are left in
 * argv from argv[1] on.  An invalid option ends mv with a usage message.
 */
static int
read_options(
	int argc,
	char **argv,
	struct mv_options *options)
{
	struct command_options scan;
	int option;
	int status;

	/* Nothing asked for yet. */
	memset(options, 0, sizeof(*options));
	options->prompt = -1;

	/* Reads each option, and the long forms. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "mv";
	scan.letters = "finuvTt:";
	scan.names = mv_long_options;
	command_options_start(&scan);
	for (;;) {
		option = command_options_next(&scan);
		if (option == COMMAND_OPTION_END)
			break;

		/* Records what the option asks for. */
		status = read_letter(option, scan.value, options);
		if (status != 0)
			usage();
	}

	/* Reports how many operands there are. */
	return scan.operand_count;
}

/* Records one option; of -i, -f and -n the last wins.  Returns -1 for an unknown one. */
static int
read_letter(
	int letter,
	const char *value,
	struct mv_options *options)
{
	/* Records what the option asks for. */
	switch (letter) {
	case 'f':
		options->prompt = 0;
		options->no_replace = 0;
		options->conflict_fails = 0;
		break;
	case 'i':
		options->prompt = 1;
		break;
	case 'n':
		options->no_replace = 1;
		options->conflict_fails = 0;
		break;
	case 'u':
		options->keep_newer = 1;
		break;
	case 'v':
		options->verbose = 1;
		break;
	case 'T':
		options->literal = 1;
		break;
	case 't':
		options->directory = value;
		break;
	case OPTION_UPDATE:
		apply_update(options, value);
		break;
	case OPTION_IGNORED:
		break;
	case OPTION_VERSION:
		printf("mv (" ZEDBSD_OS_NAME ") 1.0\n");
		exit(0);
		break;
	default:
		return -1;
	}

	/* Succeeded: the option was known. */
	return 0;
}

/*
 * Applies --update[=when]: older (or no word) is -u, all replaces as
 * usual, none keeps existing destinations and none-fail refuses them.
 */
static void
apply_update(
	struct mv_options *options,
	const char *word)
{
	int compare;

	/* No word, or older: -u. */
	options->keep_newer = 1;
	if (word == NULL)
		return;
	compare = strcmp(word, "older");
	if (compare == 0)
		return;

	/* all: replaces as usual. */
	options->keep_newer = 0;
	compare = strcmp(word, "all");
	if (compare == 0)
		return;

	/* none and none-fail keep existing destinations. */
	compare = strcmp(word, "none");
	if (compare == 0) {
		options->no_replace = 1;
		options->conflict_fails = 0;
		return;
	}

	/* none-fail refuses them. */
	compare = strcmp(word, "none-fail");
	if (compare == 0) {
		options->no_replace = 1;
		options->conflict_fails = 1;
	}
}

/* Tells whether a destination is not older than its source (-u keeps it). */
static int
destination_newer(
	const struct stat *from,
	const struct stat *existing)
{
	/* Older by seconds, then by nanoseconds. */
	if (existing->st_mtim.tv_sec < from->st_mtim.tv_sec)
		return 0;
	if (existing->st_mtim.tv_sec == from->st_mtim.tv_sec && existing->st_mtim.tv_nsec < from->st_mtim.tv_nsec)
		return 0;

	/* As new or newer: it stays. */
	return 1;
}

/* Moves one source into a target directory under its last component. */
static int
move_into(
	const struct mv_options *options,
	const char *source,
	const char *directory)
{
	char name[PATH_MAX + 1];
	char destination[PATH_MAX + 1];
	const char *leaf;
	int count;
	int status;

	/* The last component of the source, trailing slashes ignored. */
	leaf = copy_leaf(source, name, sizeof(name));
	if (leaf == NULL) {
		fprintf(stderr, "mv: %s: %s\n", source, strerror(ENAMETOOLONG));
		return -1;
	}

	/* The destination inside the directory. */
	count = snprintf(destination, sizeof(destination), "%s/%s", directory, leaf);
	if (count < 0 || (size_t)count >= sizeof(destination)) {
		fprintf(stderr, "mv: %s: %s\n", source, strerror(ENAMETOOLONG));
		return -1;
	}

	/* Moves the source there. */
	status = move_operand(options, source, destination);
	if (status != 0)
		return -1;

	/* Succeeded: the source was moved. */
	return 0;
}

/*
 * Moves one source to a destination pathname.  Returns 0 when moved or
 * deliberately left, and -1 after a diagnosed failure.
 */
static int
move_operand(
	const struct mv_options *options,
	const char *source,
	const char *destination)
{
	struct stat from;
	struct stat existing;
	int status;
	int answer;
	int newer;

	/* The source must exist; a link is moved, not followed. */
	status = lstat(source, &from);
	if (status != 0) {
		fprintf(stderr, "mv: %s: %s\n", source, strerror(errno));
		return -1;
	}

	/* Deals with an existing destination before touching anything. */
	status = lstat(destination, &existing);
	if (status == 0) {
		/* Moving a file onto another name of itself is refused. */
		if (existing.st_dev == from.st_dev && existing.st_ino == from.st_ino) {
			fprintf(stderr, "mv: '%s' and '%s' are the same file\n", source, destination);
			return -1;
		}

		/* -u keeps a destination that is not older. */
		newer = 0;
		if (options->keep_newer)
			newer = destination_newer(&from, &existing);
		if (newer)
			return 0;

		/* -n keeps the destination; --update=none-fail refuses it. */
		if (options->no_replace) {
			if (!options->conflict_fails)
				return 0;
			fprintf(stderr, "mv: %s: %s\n", destination, strerror(EEXIST));
			return -1;
		}

		/*
		 * Asks when -i or an unwritable destination calls for it.
		 * Declining leaves both files, and the status reports that not
		 * every file was moved.
		 */
		answer = confirm_replace(options, destination);
		if (!answer)
			return -1;
	}

	/* Renames the source, or copies it across file systems; -v tells. */
	status = rename_source(options, source, destination);
	if (status == 0 && options->verbose)
		printf("renamed '%s' -> '%s'\n", source, destination);
	if (status == 0)
		return 0;
	if (errno != EXDEV) {
		fprintf(stderr, "mv: cannot move '%s' to '%s': %s\n", source, destination, strerror(errno));
		return -1;
	}

	/* The destination is on another file system. */
	status = move_across(source, destination, &from);
	if (status != 0)
		return -1;
	if (options->verbose)
		printf("copied '%s' -> '%s'\n", source, destination);

	/* Succeeded: the source was moved. */
	return 0;
}

/*
 * Decides whether an existing destination may be replaced: -f replaces it,
 * -i asks, and without either an unwritable destination is asked about
 * when standard input is a terminal.
 */
static int
confirm_replace(
	const struct mv_options *options,
	const char *destination)
{
	int writable;
	int answer;

	/* -f never asks. */
	if (options->prompt == 0)
		return 1;

	/* -i always asks. */
	if (options->prompt == 1) {
		answer = copy_ask("mv", "overwrite", destination);
		return answer;
	}

	/* Without either, only an unwritable destination on a terminal asks. */
	if (!options->terminal)
		return 1;
	writable = access(destination, W_OK);
	if (writable == 0)
		return 1;

	/* Asks about replacing a file its owner protected. */
	answer = copy_ask("mv", "replace unwritable", destination);
	return answer;
}

/*
 * Renames the source; with -n or --update=none-fail the rename refuses an
 * existing destination in the same operation.
 */
static int
rename_source(
	const struct mv_options *options,
	const char *source,
	const char *destination)
{
	int status;

	/* Keeps an existing destination atomically. */
	if (options->no_replace) {
		status = renameat2(AT_FDCWD, source, AT_FDCWD, destination, RENAME_NOREPLACE);
		if (status != 0)
			return -1;
		return 0;
	}

	/* Replaces an existing destination. */
	status = rename(source, destination);
	if (status != 0)
		return -1;

	/* Succeeded: the source has its new name. */
	return 0;
}

/*
 * Moves a source to another file system: removes a destination it may
 * replace, copies the source as cp -pRP would, and removes the source
 * only after the whole copy succeeded.
 */
static int
move_across(
	const char *source,
	const char *destination,
	const struct stat *from)
{
	struct copy_options copy;
	int status;

	/* Makes room at the destination. */
	status = clear_destination(source, destination, from);
	if (status != 0)
		return -1;

	/* Copies everything, keeping owners, bits, times, links and hard links. */
	copy_options_init(&copy, "mv");
	copy.follow = COPY_FOLLOW_NONE;
	copy.recursive = 1;
	copy.preserve_mode = 1;
	copy.preserve_owner = 1;
	copy.preserve_times = 1;
	copy.preserve_links = 1;
	status = copy_operand(&copy, source, destination);
	copy_finish(&copy);
	if (status != 0) {
		fprintf(stderr, "mv: '%s' was not removed\n", source);
		return -1;
	}

	/* Removes the source now that the copy is complete. */
	status = remove_tree(source);
	if (status != 0)
		return -1;

	/* Succeeded: the source lives only at the destination. */
	return 0;
}

/*
 * Removes an existing destination that a source from another file system
 * replaces: a directory by a directory only when it is empty, anything
 * else by anything but a directory.
 */
static int
clear_destination(
	const char *source,
	const char *destination,
	const struct stat *from)
{
	struct stat existing;
	int status;
	int existing_directory;
	int source_directory;

	/* Nothing to do when the name is free. */
	status = lstat(destination, &existing);
	if (status != 0) {
		if (errno == ENOENT)
			return 0;
		fprintf(stderr, "mv: %s: %s\n", destination, strerror(errno));
		return -1;
	}

	/* A directory is replaced only by a directory, and the reverse. */
	existing_directory = S_ISDIR(existing.st_mode);
	source_directory = S_ISDIR(from->st_mode);
	if (existing_directory && !source_directory) {
		fprintf(stderr, "mv: cannot overwrite directory '%s' with non-directory '%s'\n", destination, source);
		return -1;
	}

	/* A directory cannot replace a file either. */
	if (!existing_directory && source_directory) {
		fprintf(stderr, "mv: cannot overwrite non-directory '%s' with directory '%s'\n", destination, source);
		return -1;
	}

	/* An empty directory, or any other file, is removed. */
	if (existing_directory)
		status = rmdir(destination);
	else
		status = unlink(destination);
	if (status != 0) {
		fprintf(stderr, "mv: cannot move '%s' to '%s': %s\n", source, destination, strerror(errno));
		return -1;
	}

	/* Succeeded: the name is free. */
	return 0;
}

/* Removes a file, or a directory and everything below it. */
static int
remove_tree(
	const char *path)
{
	struct stat status_of_path;
	int status;
	int failed;
	int directory;

	/* Learns what is there without following a link. */
	status = lstat(path, &status_of_path);
	if (status != 0) {
		fprintf(stderr, "mv: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Anything but a directory is one name to remove. */
	directory = S_ISDIR(status_of_path.st_mode);
	if (!directory) {
		status = unlink(path);
		if (status != 0) {
			fprintf(stderr, "mv: cannot remove '%s': %s\n", path, strerror(errno));
			return -1;
		}

		/* Succeeded: the name is gone. */
		return 0;
	}

	/* Empties the directory, then removes it. */
	failed = 0;
	status = remove_children(path);
	if (status != 0)
		failed = 1;
	status = rmdir(path);
	if (status != 0) {
		fprintf(stderr, "mv: cannot remove '%s': %s\n", path, strerror(errno));
		failed = 1;
	}

	/* Reports whether anything was left. */
	if (failed)
		return -1;

	/* Succeeded: the tree is gone. */
	return 0;
}

/* Removes everything inside a directory. */
static int
remove_children(
	const char *path)
{
	char child[PATH_MAX + 1];
	struct dirent *entry;
	DIR *stream;
	int count;
	int failed;
	int status;
	int dot;
	int dot_dot;

	/* Opens the directory. */
	stream = opendir(path);
	if (stream == NULL) {
		fprintf(stderr, "mv: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Removes each entry but the directory itself and its parent. */
	failed = 0;
	for (;;) {
		errno = 0;
		entry = readdir(stream);
		if (entry == NULL) {
			if (errno != 0)
				failed = 1;
			break;
		}

		/* Skips . and ... */
		dot = strcmp(entry->d_name, ".");
		dot_dot = strcmp(entry->d_name, "..");
		if (dot == 0 || dot_dot == 0)
			continue;

		/* Names the entry. */
		count = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		if (count < 0 || (size_t)count >= sizeof(child)) {
			fprintf(stderr, "mv: %s: %s\n", path, strerror(ENAMETOOLONG));
			failed = 1;
			continue;
		}

		/* Removes it and everything below it. */
		status = remove_tree(child);
		if (status != 0)
			failed = 1;
	}

	/* Closes the directory. */
	closedir(stream);

	/* Reports whether anything was left. */
	if (failed)
		return -1;

	/* Succeeded: the directory is empty. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX forms. */
	fprintf(stderr,
		"usage: mv [-if] source_file target_file\n"
		"       mv [-if] source_file... target_dir\n");
	exit(1);
}
