/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Makes a file or directory with a unique name (GNU mktemp).
 *
 *	mktemp [-dqtu] [-p dir] [--tmpdir[=dir]] [--suffix=suffix] [template]
 *
 * The template ends in at least three X's (before the suffix), which are
 * replaced by random letters and digits until the name is new; the file
 * is made with mode 0600, or with -d a directory with mode 0700, and its
 * name written.  -u only writes a name that did not exist (unsafe: it may
 * exist by the time it is used), and -q keeps failures to make the file
 * quiet (a template that cannot be used is still reported).
 *
 * Without a template the name is tmp.XXXXXXXXXX in the temporary
 * directory.  -p dir (--tmpdir=dir) puts the template in dir, or with
 * --tmpdir alone in $TMPDIR or /tmp; -t puts it in $TMPDIR if set, else
 * -p's dir, else /tmp.  With either, the template is one name component.
 * --suffix puts a string after the X's; a template that does not end in
 * X has its end after the last X as the suffix.
 *
 * mktemp is not in POSIX; this is the utility of the GNU core utilities,
 * which scripts and configure scripts use (the user's decision for WS045,
 * 2026-09-27).
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* The fewest X's a template may have. */
#define MKTEMP_X_MIN 3

/* The names tried before giving up. */
#define MKTEMP_TRIES 10000

/* The codes of the long options that have no letter. */
#define OPTION_SUFFIX 256
#define OPTION_TMPDIR 257
#define OPTION_HELP 258
#define OPTION_VERSION 259

/*
 * The options written in full, read by the scan of the command line; a
 * long option shares its code with its letter.
 */
static const struct command_long_option mktemp_long_options[] = {
	{"directory", COMMAND_VALUE_NONE, 'd'},
	{"dry-run", COMMAND_VALUE_NONE, 'u'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"quiet", COMMAND_VALUE_NONE, 'q'},
	{"suffix", COMMAND_VALUE_REQUIRED, OPTION_SUFFIX},
	{"tmpdir", COMMAND_VALUE_OPTIONAL, OPTION_TMPDIR},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{NULL, 0, 0}
};

/*
 * What the command line asks for.
 *
 * directory is -p's (or --tmpdir's) directory; in_directory says that
 * the template goes in a directory, and tmpdir_default that it is
 * $TMPDIR or /tmp.
 */
struct mktemp_options {
	int make_directory;
	int dry_run;
	int quiet;
	int legacy;
	int in_directory;
	int tmpdir_default;
	const char *directory;
	const char *suffix;
};

static void read_options(int argc, char **argv, struct mktemp_options *options, const char **template);
static int build_name(const struct mktemp_options *options, const char *template, char *name, size_t size, size_t *x_start, size_t *x_count);
static const char *temporary_directory(const struct mktemp_options *options);
static int make_unique(const struct mktemp_options *options, char *name, size_t x_start, size_t x_count);
static void fill_random(char *where, size_t count);
static void fail(const struct mktemp_options *options, const char *message, const char *subject);
static void complain(const char *message, const char *subject);
static void usage(void);

/*
 * Runs mktemp.
 */
int
main(
	int argc,
	char **argv)
{
	struct mktemp_options options;
	char name[PATH_MAX + 1];
	const char *template;
	size_t x_start;
	size_t x_count;
	int status;

	/* Reads the options and the template. */
	read_options(argc, argv, &options, &template);

	/* The name with its X's, in the directory asked for. */
	status = build_name(&options, template, name, sizeof(name), &x_start, &x_count);
	if (status != 0)
		return 1;

	/* Makes the file or directory under a new name. */
	status = make_unique(&options, name, x_start, x_count);
	if (status != 0)
		return 1;

	/* Writes the name. */
	printf("%s\n", name);
	status = fflush(stdout);
	if (status != 0) {
		fail(&options, "write error", "standard output");
		return 1;
	}

	/* Succeeded: the name is new. */
	return 0;
}

/* Reads the options, and the template or NULL. */
static void
read_options(
	int argc,
	char **argv,
	struct mktemp_options *options,
	const char **template)
{
	struct command_options scan;
	int option;

	/* Nothing asked for yet. */
	memset(options, 0, sizeof(*options));
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "mktemp";
	scan.letters = "dqtup:";
	scan.names = mktemp_long_options;
	command_options_start(&scan);
	for (;;) {
		option = command_options_next(&scan);
		if (option == COMMAND_OPTION_END)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'd':
			options->make_directory = 1;
			break;
		case 'q':
			options->quiet = 1;
			break;
		case 't':
			options->legacy = 1;
			break;
		case 'u':
			options->dry_run = 1;
			break;
		case 'p':
			options->in_directory = 1;
			options->directory = scan.value;
			break;
		case OPTION_TMPDIR:
			options->in_directory = 1;
			options->directory = scan.value;
			break;
		case OPTION_SUFFIX:
			options->suffix = scan.value;
			break;
		case OPTION_VERSION:
			printf("mktemp (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
			break;
		default:
			usage();
			break;
		}
	}

	/* At most one template; none is tmp.XXXXXXXXXX in the directory. */
	if (scan.operand_count > 1)
		usage();
	*template = NULL;
	if (scan.operand_count == 1)
		*template = argv[1];
	if (*template == NULL) {
		*template = "tmp.XXXXXXXXXX";
		options->in_directory = 1;
	}
}

/*
 * Makes the name: the directory, the template and the suffix.  Stores
 * where the X's are.  Returns -1 after a diagnostic for a template that
 * cannot be used.
 */
static int
build_name(
	const struct mktemp_options *options,
	const char *template,
	char *name,
	size_t size,
	size_t *x_start,
	size_t *x_count)
{
	const char *directory;
	const char *suffix;
	const char *slash;
	size_t length;
	size_t end;
	size_t count;
	int written;

	/* The suffix: --suffix, or what follows the last X. */
	length = strlen(template);
	end = length;
	suffix = "";
	if (options->suffix != NULL) {
		suffix = options->suffix;
		slash = strchr(suffix, '/');
		if (slash != NULL) {
			complain("invalid suffix, contains directory separator", suffix);
			return -1;
		}
	} else {
		while (end > 0 && template[end - 1] != 'X')
			end--;
		if (end == 0)
			end = length;
		suffix = template + end;
	}

	/* The X's before the suffix. */
	count = 0;
	while (count < end && template[end - 1 - count] == 'X')
		count++;
	if (count < MKTEMP_X_MIN) {
		complain("too few X's in template", template);
		return -1;
	}

	/* In a directory, the template is one component. */
	directory = NULL;
	if (options->in_directory || options->legacy) {
		slash = strchr(template, '/');
		if (slash != NULL) {
			complain("invalid template, contains directory separator", template);
			return -1;
		}

		/* The directory it goes in. */
		directory = temporary_directory(options);
	}

	/* The whole name. */
	if (directory != NULL)
		written = snprintf(name, size, "%s/%.*s%s", directory, (int)end, template, suffix);
	else
		written = snprintf(name, size, "%.*s%s", (int)end, template, suffix);
	if (written < 0 || (size_t)written >= size) {
		complain(strerror(ENAMETOOLONG), template);
		return -1;
	}

	/* Succeeded: the X's are just before the suffix. */
	*x_count = count;
	*x_start = (size_t)written - strlen(suffix) - count;
	return 0;
}

/*
 * Chooses the directory of the template: -t's $TMPDIR, -p's directory,
 * or $TMPDIR and then /tmp.
 */
static const char *
temporary_directory(
	const struct mktemp_options *options)
{
	const char *environment;

	/* -t prefers $TMPDIR to -p's directory. */
	environment = getenv("TMPDIR");
	if (environment != NULL && environment[0] == '\0')
		environment = NULL;
	if (options->legacy && environment != NULL)
		return environment;

	/* -p's directory, then $TMPDIR, then /tmp. */
	if (options->directory != NULL && options->directory[0] != '\0')
		return options->directory;
	if (environment != NULL)
		return environment;
	return "/tmp";
}

/*
 * Replaces the X's until the name is new, and makes the file or the
 * directory (or with -u only checks that nothing has the name).
 */
static int
make_unique(
	const struct mktemp_options *options,
	char *name,
	size_t x_start,
	size_t x_count)
{
	struct stat status;
	int tries;
	int descriptor;
	int result;

	/* A new name each time the last one was taken. */
	for (tries = 0; tries < MKTEMP_TRIES; tries++) {
		fill_random(name + x_start, x_count);

		/* -u: only a name that nothing has. */
		if (options->dry_run) {
			result = lstat(name, &status);
			if (result != 0 && errno == ENOENT)
				return 0;
			if (result != 0)
				break;
			continue;
		}

		/* A directory, or a file that is made only if it is new. */
		if (options->make_directory) {
			result = mkdir(name, 0700);
			if (result == 0)
				return 0;
		} else {
			descriptor = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
			if (descriptor >= 0) {
				close(descriptor);
				return 0;
			}
		}

		/* A name taken is tried again; anything else is a failure. */
		if (errno != EEXIST)
			break;
	}

	/* No name could be made. */
	if (options->make_directory)
		fail(options, "failed to create directory via template", name);
	else
		fail(options, "failed to create file via template", name);
	return -1;
}

/* Puts random letters and digits in place of the X's. */
static void
fill_random(
	char *where,
	size_t count)
{
	static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	static unsigned long fallback;
	unsigned char bytes[256];
	size_t index;
	int status;

	/* Random bytes, or a counter mixed with the time and the process. */
	if (count > sizeof(bytes))
		count = sizeof(bytes);
	status = getentropy(bytes, count);
	if (status != 0) {
		fallback = fallback * 6364136223846793005UL + (unsigned long)getpid() + (unsigned long)time(NULL);
		for (index = 0; index < count; index++)
			bytes[index] = (unsigned char)(fallback >> ((index % 8) * 8));
	}

	/* One letter or digit for each byte. */
	for (index = 0; index < count; index++)
		where[index] = letters[bytes[index] % (sizeof(letters) - 1)];
}

/* Writes a diagnostic of a failure to make the name, unless -q. */
static void
fail(
	const struct mktemp_options *options,
	const char *message,
	const char *subject)
{
	/* -q keeps it quiet. */
	if (options->quiet)
		return;
	complain(message, subject);
}

/* Writes a diagnostic, which -q does not silence (a template not usable). */
static void
complain(
	const char *message,
	const char *subject)
{
	/* The message and what it is about. */
	fprintf(stderr, "mktemp: %s: '%s'\n", message, subject);
}

/* Writes the usage message and exits. */
static void
usage(void)
{
	/* Names the form. */
	fprintf(stderr, "usage: mktemp [-dqtu] [-p dir] [--tmpdir[=dir]] [--suffix=suffix] [template]\n");
	exit(1);
}
