/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Counts the lines, words and bytes of files (POSIX XCU wc).
 *
 *	wc [-c|-m] [-lw] [file...]
 *
 * With no option, -l, -w and -c.  The counts are written in the order lines,
 * words, characters (-m), bytes (-c), then the name, and a total when there
 * are several files.  A word is a run of non-white-space bytes; characters
 * are bytes in the C locale.
 *
 * The counts are padded to one width, as GNU wc does: the digits of the
 * total size of the regular files counted, and at least 7 when one of the
 * inputs is not a regular file (a pipe); one count of one input is not
 * padded.
 *
 * GNU's extensions: -L (--max-line-length) writes the width of the longest
 * line (a tab moves to the next multiple of 8, a printable byte is one
 * column), after the other counts; the long options; and options after
 * operands (unless POSIXLY_CORRECT is set).
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
static const struct command_long_option wc_long_options[] = {
	{"bytes", COMMAND_VALUE_NONE, 'c'},
	{"chars", COMMAND_VALUE_NONE, 'm'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"lines", COMMAND_VALUE_NONE, 'l'},
	{"max-line-length", COMMAND_VALUE_NONE, 'L'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{"words", COMMAND_VALUE_NONE, 'w'},
	{NULL, 0, 0}
};

/* The counts of one input, and its longest line (-L). */
struct counts {
	unsigned long long lines;
	unsigned long long words;
	unsigned long long bytes;
	unsigned long long longest;
};

/* What to write. */
struct options {
	int lines;
	int words;
	int characters;
	int bytes;
	int longest;
	int width;
};

static int read_options(int argc, char **argv, struct options *options);
static int count_stream(FILE *stream, struct counts *counts);
static int compute_width(int argc, char **argv, int first, const struct options *options);
static int is_space(int value);
static unsigned long long advance_column(unsigned long long column, int value, unsigned long long *longest);
static void print_counts(const struct options *options, const struct counts *counts, const char *name);
static void print_number(int *first, int width, unsigned long long value);

/*
 * Runs wc.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	struct counts counts;
	struct counts total;
	FILE *stream;
	const char *name;
	int count;
	int first;
	int index;
	int status;
	int compare;
	int ok;

	/* The options; none means -l -w -c.  The operands follow argv[0]. */
	memset(&options, 0, sizeof(options));
	count = read_options(argc, argv, &options);
	first = 1;
	argc = first + count;
	if (!options.lines && !options.words && !options.characters &&
	    !options.bytes && !options.longest) {
		options.lines = 1;
		options.words = 1;
		options.bytes = 1;
	}

	/* The width of the columns, from the sizes of the files. */
	options.width = compute_width(argc, argv, first, &options);

	/* Standard input when there is no file. */
	status = 0;
	if (first >= argc) {
		ok = count_stream(stdin, &counts);
		if (!ok)
			status = 1;
		print_counts(&options, &counts, NULL);
		return status;
	}

	/* Each file, and the totals. */
	memset(&total, 0, sizeof(total));
	for (index = first; index < argc; index++) {
		/* - is standard input; a file that cannot be opened is an error. */
		name = argv[index];
		compare = strcmp(name, "-");
		stream = stdin;
		if (compare != 0)
			stream = fopen(name, "r");
		if (stream == NULL) {
			fprintf(stderr, "wc: %s: %s\n", name, strerror(errno));
			status = 1;
			continue;
		}

		/* The counts of the file, and the total. */
		ok = count_stream(stream, &counts);
		if (!ok) {
			fprintf(stderr, "wc: %s: %s\n", name, strerror(errno));
			status = 1;
		}

		/* The file is done with; its counts are written and added to the totals. */
		if (stream != stdin)
			fclose(stream);
		print_counts(&options, &counts, name);
		total.lines += counts.lines;
		total.words += counts.words;
		total.bytes += counts.bytes;
		if (counts.longest > total.longest)
			total.longest = counts.longest;
	}

	/* Succeeded: the total of several files. */
	if (argc - first > 1)
		print_counts(&options, &total, "total");
	return status;
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
	scan.program = "wc";
	scan.letters = "lwmcL";
	scan.names = wc_long_options;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;
		switch (code) {
		case 'l':
			options->lines = 1;
			break;
		case 'w':
			options->words = 1;
			break;
		case 'm':
			options->characters = 1;
			break;
		case 'c':
			options->bytes = 1;
			break;
		case 'L':
			options->longest = 1;
			break;
		case OPTION_VERSION:
			printf("wc (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
		default:
			fprintf(stderr, "usage: wc [-c|-m] [-lwL] [file...]\n");
			exit(1);
		}
	}

	/* Succeeded: the operands follow argv[0]. */
	return scan.operand_count;
}

/* Counts one input.  Returns 0 when reading failed. */
static int
count_stream(
	FILE *stream,
	struct counts *counts)
{
	unsigned long long column;
	int value;
	int in_word;
	int space;
	int failed;

	/* No counts yet. */
	memset(counts, 0, sizeof(*counts));
	in_word = 0;
	column = 0;
	for (;;) {
		/* The next byte. */
		value = getc(stream);
		if (value == EOF)
			break;
		counts->bytes++;
		if (value == '\n')
			counts->lines++;

		/* The column the byte leaves the line at, for -L. */
		column = advance_column(column, value, &counts->longest);

		/* A word starts at a non-space after a space. */
		space = is_space(value);
		if (space) {
			in_word = 0;
		} else if (!in_word) {
			in_word = 1;
			counts->words++;
		}
	}

	/* A last line without a newline counts for -L too. */
	if (column > counts->longest)
		counts->longest = column;

	/* Succeeded unless the stream failed. */
	failed = ferror(stream);
	if (failed)
		return 0;
	return 1;
}

/*
 * Returns the column after a byte, as GNU's -L counts it: a line end (a
 * newline, a return or a form feed) goes back to 0, remembering the widest
 * line; a tab goes to the next multiple of 8; a printable byte is one
 * column, and any other none.
 */
static unsigned long long
advance_column(
	unsigned long long column,
	int value,
	unsigned long long *longest)
{
	/* The end of a line. */
	if (value == '\n' || value == '\r' || value == '\f') {
		if (column > *longest)
			*longest = column;
		return 0;
	}

	/* A tab. */
	if (value == '\t')
		return column + 8U - column % 8U;

	/* A printable byte. */
	if (value >= 0x20 && value < 0x7f)
		return column + 1U;

	/* Succeeded: anything else takes no room. */
	return column;
}

/*
 * Computes the width of the counts: the digits of the total size of the
 * regular files, at least 7 when an input is not one, and 1 for a single
 * count of a single input.
 */
static int
compute_width(
	int argc,
	char **argv,
	int first,
	const struct options *options)
{
	struct stat status;
	unsigned long long total;
	int counts;
	int inputs;
	int minimum;
	int width;
	int index;
	int error;
	int compare;
	int regular;

	/* One count of one input is written as it is. */
	counts = options->lines + options->words + options->characters +
	    options->bytes + options->longest;
	inputs = argc - first;
	if (counts == 1 && inputs <= 1)
		return 1;

	/* Standard input is not a regular file (as a pipe, at least). */
	minimum = 1;
	total = 0;
	if (inputs == 0)
		minimum = 7;
	for (index = first; index < argc; index++) {
		compare = strcmp(argv[index], "-");
		if (compare == 0) {
			minimum = 7;
			continue;
		}

		/* The size of a regular file; anything else may be of any size. */
		error = stat(argv[index], &status);
		if (error != 0)
			continue;
		regular = S_ISREG(status.st_mode);
		if (regular)
			total += (unsigned long long)status.st_size;
		else
			minimum = 7;
	}

	/* The digits of the total. */
	for (width = 1; total >= 10U; total /= 10U)
		width++;
	if (width < minimum)
		width = minimum;

	/* Succeeded. */
	return width;
}

/* Reports whether a byte is white space in the C locale. */
static int
is_space(
	int value)
{
	/* Space, tab, newline, vertical tab, form feed, carriage return. */
	if (value == ' ' || (value >= '\t' && value <= '\r'))
		return 1;
	return 0;
}

/* Writes the counts asked for, and the name when there is one. */
static void
print_counts(
	const struct options *options,
	const struct counts *counts,
	const char *name)
{
	int first;

	/* The counts, in the POSIX order. */
	first = 1;
	if (options->lines)
		print_number(&first, options->width, counts->lines);
	if (options->words)
		print_number(&first, options->width, counts->words);
	if (options->characters)
		print_number(&first, options->width, counts->bytes);
	if (options->bytes)
		print_number(&first, options->width, counts->bytes);
	if (options->longest)
		print_number(&first, options->width, counts->longest);

	/* The name. */
	if (name != NULL)
		printf(" %s", name);
	putchar('\n');
}

/* Writes one count, after a space when it is not the first. */
static void
print_number(
	int *first,
	int width,
	unsigned long long value)
{
	/* The separator. */
	if (!*first)
		putchar(' ');
	*first = 0;

	/* The count, padded. */
	printf("%*llu", width, value);
}
