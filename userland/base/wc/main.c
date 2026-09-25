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
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The counts of one input. */
struct counts {
	unsigned long long lines;
	unsigned long long words;
	unsigned long long bytes;
};

/* What to write. */
struct options {
	int lines;
	int words;
	int characters;
	int bytes;
	int width;
};

static int read_options(int argc, char **argv, struct options *options);
static int count_stream(FILE *stream, struct counts *counts);
static int compute_width(int argc, char **argv, int first, const struct options *options);
static int is_space(int value);
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
	int first;
	int index;
	int status;
	int compare;
	int ok;

	/* The options; none means -l -w -c. */
	memset(&options, 0, sizeof(options));
	first = read_options(argc, argv, &options);
	if (!options.lines && !options.words && !options.characters &&
	    !options.bytes) {
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
	}

	/* Succeeded: the total of several files. */
	if (argc - first > 1)
		print_counts(&options, &total, "total");
	return status;
}

/* Reads the options; returns the index of the first operand. */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	const char *letter;
	const char *word;
	int index;

	/* Each option word. */
	for (index = 1; index < argc; index++) {
		/* An operand, or - alone, ends the options; so does --. */
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* Each letter. */
		for (letter = word + 1; *letter != '\0'; letter++) {
			switch (*letter) {
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
			default:
				fprintf(stderr, "wc: invalid option -- '%c'\n",
					*letter);
				fprintf(stderr, "usage: wc [-c|-m] [-lw] "
					"[file...]\n");
				exit(1);
			}
		}
	}

	/* Succeeded: the first operand. */
	return index;
}

/* Counts one input.  Returns 0 when reading failed. */
static int
count_stream(
	FILE *stream,
	struct counts *counts)
{
	int value;
	int in_word;
	int space;
	int failed;

	/* No counts yet. */
	memset(counts, 0, sizeof(*counts));
	in_word = 0;
	for (;;) {
		/* The next byte. */
		value = getc(stream);
		if (value == EOF)
			break;
		counts->bytes++;
		if (value == '\n')
			counts->lines++;

		/* A word starts at a non-space after a space. */
		space = is_space(value);
		if (space) {
			in_word = 0;
		} else if (!in_word) {
			in_word = 1;
			counts->words++;
		}
	}

	/* Succeeded unless the stream failed. */
	failed = ferror(stream);
	if (failed)
		return 0;
	return 1;
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
	    options->bytes;
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
