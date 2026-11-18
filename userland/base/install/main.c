/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Copies files and sets their attributes (GNU and BSD install).
 *
 *	install [-cCDpsv] [-m mode] [-o owner] [-g group] [-T] source dest
 *	install [-cCDpsv] [-m mode] [-o owner] [-g group] source... directory
 *	install [-cCDpsv] [-m mode] [-o owner] [-g group] -t directory source...
 *	install -d [-v] [-m mode] [-o owner] [-g group] directory...
 *
 * Each source is copied to the destination, or into the directory under
 * its last component, as a new file (an existing one is removed first)
 * with the mode (0755 unless -m, octal or symbolic), and the owner and
 * group of -o and -g.  -p keeps the source's access and modification
 * times, -D makes the missing directories of the destination (and -t's
 * directory), -T takes the destination as a file name always, and -v
 * writes each copy.  -d makes the directories and their missing parents
 * instead, giving the last component the mode, owner and group.
 *
 * -c is taken and changes nothing, as on BSD.  -C copies only when the
 * destination differs.  -s is taken and does not strip: the base system
 * has no strip program; the file is copied as it is.
 *
 * install is not in POSIX; this is the utility of the GNU core utilities
 * that makefiles use (the user's decision for WS045, 2026-09-27).
 */

#include "userland/base/chmod/mode.h"
#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* The mode without -m. */
#define INSTALL_MODE_DEFAULT 0755

/* The size of the buffer a file is copied through. */
#define INSTALL_BUFFER_SIZE 65536

/* The codes of the long options that have no letter. */
#define OPTION_HELP 256
#define OPTION_VERSION 257

/*
 * The options written in full, read by the scan of the command line; a
 * long option shares its code with its letter.
 */
static const struct command_long_option install_long_options[] = {
	{"compare", COMMAND_VALUE_NONE, 'C'},
	{"directory", COMMAND_VALUE_NONE, 'd'},
	{"group", COMMAND_VALUE_REQUIRED, 'g'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"mode", COMMAND_VALUE_REQUIRED, 'm'},
	{"no-target-directory", COMMAND_VALUE_NONE, 'T'},
	{"owner", COMMAND_VALUE_REQUIRED, 'o'},
	{"preserve-timestamps", COMMAND_VALUE_NONE, 'p'},
	{"strip", COMMAND_VALUE_NONE, 's'},
	{"target-directory", COMMAND_VALUE_REQUIRED, 't'},
	{"verbose", COMMAND_VALUE_NONE, 'v'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{NULL, 0, 0}
};

/*
 * What the command line asks for.
 *
 * owner and group are -1 unless -o or -g gave them.
 */
struct install_options {
	int directories;
	int compare;
	int make_leading;
	int preserve_times;
	int verbose;
	int literal;
	mode_t mode;
	uid_t owner;
	gid_t group;
	const char *target;
};

static int read_options(int argc, char **argv, struct install_options *options);
static int install_directory(const struct install_options *options, const char *path);
static int make_parents(const struct install_options *options, const char *path, int include_last);
static int install_file(const struct install_options *options, const char *source, const char *destination);
static int install_into(const struct install_options *options, const char *source, const char *directory);
static int copy_contents(int input, int output);
static int same_contents(const char *source, const char *destination, const struct stat *from);
static int set_attributes(const struct install_options *options, int descriptor, const char *path, const struct stat *from);
static uid_t read_owner(const char *text);
static gid_t read_group(const char *text);
static void usage(void);

/*
 * Runs install.
 */
int
main(
	int argc,
	char **argv)
{
	struct install_options options;
	struct stat status_of_target;
	const char *target;
	int count;
	int last;
	int index;
	int directory;
	int status;
	int failed;

	/* Reads the options; the operands are left from argv[1] on. */
	count = read_options(argc, argv, &options);
	failed = 0;

	/* -d: each operand is a directory to make. */
	if (options.directories) {
		if (count < 1)
			usage();
		for (index = 1; index <= count; index++) {
			status = install_directory(&options, argv[index]);
			if (status != 0)
				failed = 1;
		}

		/* Reports whether any directory could not be made. */
		return failed;
	}

	/* The sources, and the destination unless -t named it. */
	last = count;
	target = options.target;
	if (target == NULL) {
		if (count < 2)
			usage();
		target = argv[count];
		last = count - 1;
	}

	/* At least one source. */
	if (last < 1)
		usage();

	/* -D makes -t's directory, or the destination's directory. */
	if (options.make_leading) {
		status = make_parents(&options, target, options.target != NULL);
		if (status != 0)
			return 1;
	}

	/* Whether the destination is a directory the sources go into. */
	directory = 0;
	if (!options.literal) {
		status = stat(target, &status_of_target);
		if (status == 0)
			directory = S_ISDIR(status_of_target.st_mode);
	}

	/* Several sources, or -t, need a directory. */
	if ((last > 1 || options.target != NULL) && !directory) {
		fprintf(stderr, "install: target '%s' is not a directory\n", target);
		return 1;
	}

	/* Installs each source. */
	for (index = 1; index <= last; index++) {
		if (directory)
			status = install_into(&options, argv[index], target);
		else
			status = install_file(&options, argv[index], target);
		if (status != 0)
			failed = 1;
	}

	/* Reports whether any source could not be installed. */
	if (failed)
		return 1;

	/* Succeeded: every source was installed. */
	return 0;
}

/* Reads the options; returns the number of operands. */
static int
read_options(
	int argc,
	char **argv,
	struct install_options *options)
{
	struct command_options scan;
	const char *mode_text;
	int option;
	int status;

	/* Nothing asked for yet. */
	memset(options, 0, sizeof(*options));
	options->mode = INSTALL_MODE_DEFAULT;
	options->owner = (uid_t)-1;
	options->group = (gid_t)-1;
	mode_text = NULL;
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "install";
	scan.letters = "cCdDpsvTg:m:o:t:";
	scan.names = install_long_options;
	command_options_start(&scan);
	for (;;) {
		option = command_options_next(&scan);
		if (option == COMMAND_OPTION_END)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'c':
		case 's':
			break;
		case 'C':
			options->compare = 1;
			break;
		case 'd':
			options->directories = 1;
			break;
		case 'D':
			options->make_leading = 1;
			break;
		case 'p':
			options->preserve_times = 1;
			break;
		case 'v':
			options->verbose = 1;
			break;
		case 'T':
			options->literal = 1;
			break;
		case 'g':
			options->group = read_group(scan.value);
			break;
		case 'm':
			mode_text = scan.value;
			break;
		case 'o':
			options->owner = read_owner(scan.value);
			break;
		case 't':
			options->target = scan.value;
			break;
		case OPTION_VERSION:
			printf("install (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
			break;
		default:
			usage();
			break;
		}
	}

	/* -m: an octal or symbolic mode, counted from no permissions. */
	if (mode_text != NULL) {
		status = mode_apply(mode_text, 0, 0, options->directories, &options->mode);
		if (status != 0) {
			fprintf(stderr, "install: invalid mode '%s'\n", mode_text);
			exit(1);
		}
	}

	/* Reports how many operands there are. */
	return scan.operand_count;
}

/*
 * -d: makes a directory and its missing parents, and gives it the mode,
 * owner and group.
 */
static int
install_directory(
	const struct install_options *options,
	const char *path)
{
	int status;

	/* The directory and its parents. */
	status = make_parents(options, path, 1);
	if (status != 0)
		return -1;

	/* Its owner and group, then its mode. */
	if (options->owner != (uid_t)-1 || options->group != (gid_t)-1) {
		status = chown(path, options->owner, options->group);
		if (status != 0) {
			command_error("install", path);
			return -1;
		}
	}

	/* Then the mode. */
	status = chmod(path, options->mode);
	if (status != 0) {
		command_error("install", path);
		return -1;
	}

	/* Succeeded: the directory is there. */
	return 0;
}

/*
 * Makes the missing directories of a path, each with 0755 less the mask:
 * every component, or all but the last.
 */
static int
make_parents(
	const struct install_options *options,
	const char *path,
	int include_last)
{
	char prefix[PATH_MAX + 1];
	struct stat status_of_prefix;
	size_t length;
	size_t end;
	int status;
	int directory;

	/* A copy to cut at each slash, without trailing slashes. */
	length = strlen(path);
	if (length > PATH_MAX) {
		errno = ENAMETOOLONG;
		command_error("install", path);
		return -1;
	}

	/* Drops the trailing slashes. */
	memcpy(prefix, path, length + 1);
	while (length > 1 && prefix[length - 1] == '/')
		length--;
	prefix[length] = '\0';

	/* Each prefix that ends before a slash, and the whole path if asked. */
	end = 0;
	while (end < length) {
		while (end < length && prefix[end] == '/')
			end++;
		while (end < length && prefix[end] != '/')
			end++;
		if (end >= length && !include_last)
			break;

		/* Makes it unless a directory is there. */
		prefix[end] = '\0';
		status = stat(prefix, &status_of_prefix);
		directory = 0;
		if (status == 0)
			directory = S_ISDIR(status_of_prefix.st_mode);
		if (!directory) {
			status = mkdir(prefix, 0755);
			if (status != 0) {
				command_error("install", prefix);
				return -1;
			}

			/* -v tells. */
			if (options->verbose)
				printf("install: creating directory '%s'\n", prefix);
		}

		/* On to the next component. */
		if (end < length)
			prefix[end] = '/';
	}

	/* Succeeded: the directories are there. */
	return 0;
}

/* Installs a source into a directory under its last component. */
static int
install_into(
	const struct install_options *options,
	const char *source,
	const char *directory)
{
	char destination[PATH_MAX + 1];
	const char *leaf;
	size_t length;
	int count;
	int status;

	/* The last component, trailing slashes ignored. */
	length = strlen(source);
	while (length > 1 && source[length - 1] == '/')
		length--;
	leaf = source + length;
	while (leaf > source && leaf[-1] != '/')
		leaf--;

	/* The destination inside the directory. */
	count = snprintf(destination, sizeof(destination), "%s/%.*s", directory, (int)(source + length - leaf), leaf);
	if (count < 0 || (size_t)count >= sizeof(destination)) {
		errno = ENAMETOOLONG;
		command_error("install", source);
		return -1;
	}

	/* Installs it there. */
	status = install_file(options, source, destination);
	return status;
}

/*
 * Copies a source to a new file at the destination and sets its
 * attributes.  An existing destination is removed first.
 */
static int
install_file(
	const struct install_options *options,
	const char *source,
	const char *destination)
{
	struct stat from;
	struct stat existing;
	int input;
	int output;
	int status;
	int same;
	int closed;
	int directory;

	/* The source: a file, not a directory. */
	input = open(source, O_RDONLY);
	if (input < 0) {
		command_error("install", source);
		return -1;
	}

	/* Its status. */
	status = fstat(input, &from);
	if (status != 0) {
		command_error("install", source);
		close(input);
		return -1;
	}

	/* A directory is not copied. */
	directory = S_ISDIR(from.st_mode);
	if (directory) {
		fprintf(stderr, "install: omitting directory '%s'\n", source);
		close(input);
		return -1;
	}

	/*
	 * The same file is refused (a symbolic link at the destination is
	 * replaced, not followed); -C leaves an identical one alone.
	 */
	status = lstat(destination, &existing);
	if (status == 0 && existing.st_dev == from.st_dev && existing.st_ino == from.st_ino) {
		fprintf(stderr, "install: '%s' and '%s' are the same file\n", source, destination);
		close(input);
		return -1;
	}

	/* -C leaves an identical file with the mode alone. */
	if (status == 0 && options->compare) {
		same = same_contents(source, destination, &from);
		if (same && (existing.st_mode & 07777) == options->mode) {
			close(input);
			return 0;
		}
	}

	/* A new file in place of an existing one. */
	status = unlink(destination);
	if (status != 0 && errno != ENOENT) {
		command_error("install", destination);
		close(input);
		return -1;
	}

	/* Creates the new file. */
	output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (output < 0) {
		command_error("install", destination);
		close(input);
		return -1;
	}

	/* The contents, then the attributes. */
	status = copy_contents(input, output);
	if (status != 0)
		command_error("install", destination);
	if (status == 0)
		status = set_attributes(options, output, destination, &from);
	closed = close(output);
	close(input);
	if (status == 0 && closed != 0) {
		command_error("install", destination);
		status = -1;
	}

	/* A failure of the copy. */
	if (status != 0)
		return -1;

	/* Succeeded: -v tells. */
	if (options->verbose)
		printf("'%s' -> '%s'\n", source, destination);
	return 0;
}

/* Copies a file's contents to another. */
static int
copy_contents(
	int input,
	int output)
{
	static char buffer[INSTALL_BUFFER_SIZE];
	ssize_t got;
	int status;

	/* Reads and writes until the end. */
	for (;;) {
		got = read(input, buffer, sizeof(buffer));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		status = command_write_all(output, buffer, (size_t)got);
		if (status != 0)
			return -1;
	}

	/* A read error fails the copy. */
	if (got < 0)
		return -1;
	return 0;
}

/* Tells whether two files hold the same bytes (-C). */
static int
same_contents(
	const char *source,
	const char *destination,
	const struct stat *from)
{
	static char left[INSTALL_BUFFER_SIZE];
	static char right[INSTALL_BUFFER_SIZE];
	struct stat to;
	ssize_t got_left;
	ssize_t got_right;
	int input;
	int other;
	int status;
	int same;
	int compare;
	int regular;

	/* A regular file of the same size. */
	status = stat(destination, &to);
	if (status != 0)
		return 0;
	regular = S_ISREG(to.st_mode);
	if (!regular || to.st_size != from->st_size)
		return 0;
	input = open(source, O_RDONLY);
	if (input < 0)
		return 0;
	other = open(destination, O_RDONLY);
	if (other < 0) {
		close(input);
		return 0;
	}

	/* Compares block by block. */
	same = 1;
	for (;;) {
		got_left = read(input, left, sizeof(left));
		got_right = read(other, right, sizeof(right));
		if (got_left != got_right || got_left < 0) {
			same = 0;
			break;
		}

		/* Both files ended together. */
		if (got_left == 0)
			break;
		compare = memcmp(left, right, (size_t)got_left);
		if (compare != 0) {
			same = 0;
			break;
		}
	}

	/* Closes both. */
	close(input);
	close(other);
	return same;
}

/* Gives a new file its owner, group, mode and (with -p) times. */
static int
set_attributes(
	const struct install_options *options,
	int descriptor,
	const char *path,
	const struct stat *from)
{
	struct timespec times[2];
	int status;

	/* The owner and group first, as a change of owner clears set-ID bits. */
	if (options->owner != (uid_t)-1 || options->group != (gid_t)-1) {
		status = fchown(descriptor, options->owner, options->group);
		if (status != 0) {
			command_error("install", path);
			return -1;
		}
	}

	/* The mode. */
	status = fchmod(descriptor, options->mode);
	if (status != 0) {
		command_error("install", path);
		return -1;
	}

	/* -p: the source's times. */
	if (options->preserve_times) {
		times[0] = from->st_atim;
		times[1] = from->st_mtim;
		status = futimens(descriptor, times);
		if (status != 0) {
			command_error("install", path);
			return -1;
		}
	}

	/* Succeeded: the attributes are set. */
	return 0;
}

/* Reads -o's owner: a user name, or a number. */
static uid_t
read_owner(
	const char *text)
{
	struct passwd *account;
	unsigned long number;
	char *end;

	/* A name. */
	account = getpwnam(text);
	if (account != NULL)
		return account->pw_uid;

	/* A number. */
	errno = 0;
	number = strtoul(text, &end, 10);
	if (text[0] < '0' || text[0] > '9' || *end != '\0' || errno != 0) {
		fprintf(stderr, "install: invalid user '%s'\n", text);
		exit(1);
	}

	/* The number. */
	return (uid_t)number;
}

/* Reads -g's group: a group name, or a number. */
static gid_t
read_group(
	const char *text)
{
	struct group *entry;
	unsigned long number;
	char *end;

	/* A name. */
	entry = getgrnam(text);
	if (entry != NULL)
		return entry->gr_gid;

	/* A number. */
	errno = 0;
	number = strtoul(text, &end, 10);
	if (text[0] < '0' || text[0] > '9' || *end != '\0' || errno != 0) {
		fprintf(stderr, "install: invalid group '%s'\n", text);
		exit(1);
	}

	/* The number. */
	return (gid_t)number;
}

/* Writes the usage message and exits. */
static void
usage(void)
{
	/* Names the forms. */
	fprintf(stderr,
		"usage: install [-cCDpsv] [-m mode] [-o owner] [-g group] [-T] source dest\n"
		"       install [-cCDpsv] [-m mode] [-o owner] [-g group] source... directory\n"
		"       install [-cCDpsv] [-m mode] [-o owner] [-g group] -t directory source...\n"
		"       install -d [-v] [-m mode] [-o owner] [-g group] directory...\n");
	exit(1);
}
