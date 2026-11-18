/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Copies files (POSIX XCU cp).
 *
 *	cp [-Pfip] source_file target_file
 *	cp [-Pfip] source_file... target
 *	cp -R [-H|-L|-P] [-fip] source_file... target
 *
 * With one source and a target that is not a directory, the source is
 * copied to the target pathname; otherwise each source is copied into the
 * target directory under its own last component.  -R copies directories
 * and everything below them; -H, -L and -P choose which symbolic links are
 * followed (the last one given wins; -R alone copies links as links, and
 * without -R links named as operands are followed).  -f replaces a
 * destination that cannot be opened, -i asks before overwriting, and -p
 * keeps the owner, the permission bits and the times.
 *
 * zedBSD also accepts -r (as -R), -a (-R -P -p and shared hard links), -n
 * and --no-clobber (keep existing files), --update=none-fail (refuse
 * them), -T and --no-target-directory (the target is always a pathname),
 * --attributes-only, --preserve=mode, and --report-file=PATH, which the
 * installer uses to account for a tree copy.
 *
 * GNU's extensions are taken too (ws045): -v (--verbose) writes each copy,
 * -t directory (--target-directory) names the directory before the
 * sources, --preserve[=list] keeps mode, ownership, timestamps, links or
 * all, --no-preserve is taken, --update[=none|none-fail|all|older], and
 * the long forms of the letters; options may follow operands unless
 * POSIXLY_CORRECT is set.
 */

#include "userland/base/common/command.h"
#include "userland/base/cp/copy.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "include/kern/os-name.h"

/* The follow policy is the default of the -R choice. */
#define CP_FOLLOW_DEFAULT (-1)

/* The codes of the long options that have no letter. */
#define CP_OPTION_ATTRIBUTES 256
#define CP_OPTION_HELP 257
#define CP_OPTION_NO_PRESERVE 258
#define CP_OPTION_PRESERVE 259
#define CP_OPTION_REPORT 260
#define CP_OPTION_UPDATE 261
#define CP_OPTION_VERSION 262

/*
 * The options written in full, read by the scan of the command line; a
 * long option shares its code with its letter.
 */
static const struct command_long_option cp_long_options[] = {
	{"archive", COMMAND_VALUE_NONE, 'a'},
	{"attributes-only", COMMAND_VALUE_NONE, CP_OPTION_ATTRIBUTES},
	{"dereference", COMMAND_VALUE_NONE, 'L'},
	{"force", COMMAND_VALUE_NONE, 'f'},
	{"help", COMMAND_VALUE_NONE, CP_OPTION_HELP},
	{"interactive", COMMAND_VALUE_NONE, 'i'},
	{"no-clobber", COMMAND_VALUE_NONE, 'n'},
	{"no-dereference", COMMAND_VALUE_NONE, 'P'},
	{"no-preserve", COMMAND_VALUE_REQUIRED, CP_OPTION_NO_PRESERVE},
	{"no-target-directory", COMMAND_VALUE_NONE, 'T'},
	{"preserve", COMMAND_VALUE_OPTIONAL, CP_OPTION_PRESERVE},
	{"recursive", COMMAND_VALUE_NONE, 'R'},
	{"report-file", COMMAND_VALUE_REQUIRED, CP_OPTION_REPORT},
	{"target-directory", COMMAND_VALUE_REQUIRED, 't'},
	{"update", COMMAND_VALUE_OPTIONAL, CP_OPTION_UPDATE},
	{"verbose", COMMAND_VALUE_NONE, 'v'},
	{"version", COMMAND_VALUE_NONE, CP_OPTION_VERSION},
	{NULL, 0, 0}
};

/*
 * What the command line asks for beyond the copy policy.
 *
 * One instance lives for the run.  follow stays CP_FOLLOW_DEFAULT until
 * -H, -L or -P is given; directory is -t's.
 */
struct cp_request {
	int follow;
	int literal;
	const char *report_path;
	const char *directory;
};

static int read_options(int argc, char **argv, struct copy_options *options, struct cp_request *request);
static int read_letter(int letter, const char *value, struct copy_options *options, struct cp_request *request);
static void apply_preserve(struct copy_options *options, const char *list);
static void apply_update(struct copy_options *options, const char *word);
static int word_is(const char *word, size_t length, const char *name);
static int copy_into(struct copy_options *options, const char *source, const char *directory);
static void usage(void);

/*
 * Runs cp.
 */
int
main(
	int argc,
	char **argv)
{
	struct copy_options options;
	struct cp_request request;
	struct stat status_of_target;
	const char *target;
	int first;
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
	copy_options_init(&options, "cp");
	count = read_options(argc, argv, &options, &request);
	first = 1;
	last = count;
	target = request.directory;
	if (target == NULL) {
		target = argv[count];
		last = count - 1;
	}

	/* The number of sources. */
	count = last;
	if (count < 1)
		usage();

	/*
	 * Settles which symbolic links are followed: the option given, or
	 * none for -R and the operands without it.
	 */
	if (request.follow != CP_FOLLOW_DEFAULT)
		options.follow = request.follow;
	else if (options.recursive)
		options.follow = COPY_FOLLOW_NONE;
	else
		options.follow = COPY_FOLLOW_OPERANDS;

	/* Learns whether the target is an existing directory. */
	directory = 0;
	if (!request.literal) {
		status = stat(target, &status_of_target);
		if (status == 0)
			directory = S_ISDIR(status_of_target.st_mode);
	}

	/* Several sources, or -t, need a directory to go into. */
	if ((count > 1 || request.directory != NULL) && !directory) {
		fprintf(stderr, "cp: target '%s' is not a directory\n", target);
		return 1;
	}

	/* Opens the report before anything is copied. */
	if (request.report_path != NULL) {
		status = copy_report_open(&options, request.report_path, count, argv + first);
		if (status != 0)
			return 1;
	}

	/* Copies each source; a failure is remembered and the rest go on. */
	failed = 0;
	for (index = first; index <= last; index++) {
		if (directory)
			status = copy_into(&options, argv[index], target);
		else
			status = copy_operand(&options, argv[index], target);
		if (status != 0)
			failed = 1;

		/* A failed report stops the copies that could not be recorded. */
		if (options.report_failed) {
			failed = 1;
			break;
		}
	}

	/* Completes the report and releases the run. */
	status = copy_report_close(&options, request.report_path, failed);
	if (status != 0)
		failed = 1;
	copy_finish(&options);

	/* Reports whether any source could not be copied. */
	if (failed)
		return 1;

	/* Succeeded: every source was copied. */
	return 0;
}

/*
 * Reads the options and returns the number of operands, which are left in
 * argv from argv[1] on.  An invalid option ends cp with a usage message.
 */
static int
read_options(
	int argc,
	char **argv,
	struct copy_options *options,
	struct cp_request *request)
{
	struct command_options scan;
	int option;
	int status;

	/* Nothing asked for yet. */
	request->follow = CP_FOLLOW_DEFAULT;
	request->literal = 0;
	request->report_path = NULL;
	request->directory = NULL;

	/* Reads each option, and the long forms. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "cp";
	scan.letters = "HLPRrfiapnTvt:";
	scan.names = cp_long_options;
	command_options_start(&scan);
	for (;;) {
		option = command_options_next(&scan);
		if (option == COMMAND_OPTION_END)
			break;

		/* Records what the option asks for. */
		status = read_letter(option, scan.value, options, request);
		if (status != 0)
			usage();
	}

	/* Reports how many operands there are. */
	return scan.operand_count;
}

/* Records one option; returns -1 for an unknown one. */
static int
read_letter(
	int letter,
	const char *value,
	struct copy_options *options,
	struct cp_request *request)
{
	/* Records what the option asks for. */
	switch (letter) {
	case 'H':
		request->follow = COPY_FOLLOW_OPERANDS;
		break;
	case 'L':
		request->follow = COPY_FOLLOW_ALL;
		break;
	case 'P':
		request->follow = COPY_FOLLOW_NONE;
		break;
	case 'R':
	case 'r':
		options->recursive = 1;
		break;
	case 'f':
		options->force = 1;
		break;
	case 'i':
		options->interactive = 1;
		break;
	case 'p':
		apply_preserve(options, NULL);
		break;
	case 'a':
		/* -a is -R -P -p, with names of one file sharing one copy. */
		options->recursive = 1;
		request->follow = COPY_FOLLOW_NONE;
		apply_preserve(options, "all");
		break;
	case 'n':
		options->exclusive = 1;
		options->conflict_fails = 0;
		break;
	case 'T':
		request->literal = 1;
		break;
	case 't':
		request->directory = value;
		break;
	case 'v':
		options->verbose = 1;
		break;
	case CP_OPTION_ATTRIBUTES:
		options->attributes_only = 1;
		break;
	case CP_OPTION_PRESERVE:
		apply_preserve(options, value);
		break;
	case CP_OPTION_NO_PRESERVE:
		/* Nothing is kept that was not asked for. */
		break;
	case CP_OPTION_UPDATE:
		apply_update(options, value);
		break;
	case CP_OPTION_REPORT:
		request->report_path = value;
		break;
	case CP_OPTION_VERSION:
		printf("cp (" ZEDBSD_OS_NAME ") 1.0\n");
		exit(0);
		break;
	default:
		return -1;
	}

	/* Succeeded: the option was known. */
	return 0;
}

/*
 * Applies --preserve[=list]: mode, ownership, timestamps, links and all,
 * separated by commas; no list is mode, ownership and timestamps, as -p.
 * Other attributes are not kept.
 */
static void
apply_preserve(
	struct copy_options *options,
	const char *list)
{
	const char *word;
	size_t length;
	int all;
	int mode;
	int ownership;
	int timestamps;
	int links;

	/* No list: -p. */
	if (list == NULL) {
		options->preserve_mode = 1;
		options->preserve_owner = 1;
		options->preserve_times = 1;
		return;
	}

	/* Each word of the list. */
	for (word = list; *word != '\0'; word += length) {
		length = strcspn(word, ",");
		all = word_is(word, length, "all");
		mode = word_is(word, length, "mode");
		ownership = word_is(word, length, "ownership");
		timestamps = word_is(word, length, "timestamps");
		links = word_is(word, length, "links");
		if (all || mode)
			options->preserve_mode = 1;
		if (all || ownership)
			options->preserve_owner = 1;
		if (all || timestamps)
			options->preserve_times = 1;
		if (all || links)
			options->preserve_links = 1;

		/* The comma after the word. */
		if (word[length] == ',')
			length++;
	}
}

/* Tells whether a word of a given length is a name. */
static int
word_is(
	const char *word,
	size_t length,
	const char *name)
{
	size_t name_length;
	int compare;

	/* The same length and the same bytes. */
	name_length = strlen(name);
	if (name_length != length)
		return 0;
	compare = strncmp(word, name, length);
	if (compare != 0)
		return 0;
	return 1;
}

/*
 * Applies --update[=when]: none keeps existing files (-n), none-fail
 * refuses them; all, older and no word copy as usual.
 */
static void
apply_update(
	struct copy_options *options,
	const char *word)
{
	int compare;

	/* No word copies as usual. */
	if (word == NULL)
		return;

	/* none: never replace, quietly. */
	compare = strcmp(word, "none");
	if (compare == 0) {
		options->exclusive = 1;
		options->conflict_fails = 0;
		return;
	}

	/* none-fail: never replace, and fail. */
	compare = strcmp(word, "none-fail");
	if (compare == 0) {
		options->exclusive = 1;
		options->conflict_fails = 1;
	}
}

/* Copies one source into a target directory under its last component. */
static int
copy_into(
	struct copy_options *options,
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
		fprintf(stderr, "cp: %s: %s\n", source, strerror(ENAMETOOLONG));
		return -1;
	}

	/* The destination inside the directory. */
	count = snprintf(destination, sizeof(destination), "%s/%s", directory, leaf);
	if (count < 0 || (size_t)count >= sizeof(destination)) {
		fprintf(stderr, "cp: %s: %s\n", source, strerror(ENAMETOOLONG));
		return -1;
	}

	/* Copies the source there. */
	status = copy_operand(options, source, destination);
	if (status != 0)
		return -1;

	/* Succeeded: the source was copied. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX forms. */
	fprintf(stderr,
		"usage: cp [-Pfip] source_file target_file\n"
		"       cp [-Pfip] source_file... target\n"
		"       cp -R [-H|-L|-P] [-fip] source_file... target\n");
	exit(1);
}
