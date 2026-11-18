/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Folds long lines for finite-width output (POSIX XCU fold).
 *
 *	fold [-bs] [-w width] [file...]
 *
 * A newline is inserted wherever a line would pass width columns (80 by
 * default), so that no output line is wider.  Columns count as on a
 * terminal: a tab moves to the next multiple of eight, a backspace back by
 * one, a carriage return to the start, and every other byte takes one
 * column (the POSIX locale); with -b every byte takes one column.  With -s
 * the break goes after the last blank within the width when there is one.
 * A character too wide for an empty line is written alone on it.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The width without -w. */
#define FOLD_DEFAULT_WIDTH 80

/* The distance between tab stops. */
#define FOLD_TAB_DISTANCE 8

/*
 * The part of a line not yet written.
 *
 * The bytes wait here until a break decides where the output line ends.
 * column is the column after them; the buffer grows as a line needs.
 */
struct fold_line {
	char *bytes;
	size_t length;
	size_t capacity;
	size_t column;
};

/*
 * What one run of fold does.
 *
 * One instance lives for the run.
 */
struct fold_options {
	size_t width;
	int bytes;
	int spaces;
};

static int read_width(const char *text, size_t *width);
static int fold_operand(const char *path, const struct fold_options *options, struct fold_line *line);
static int fold_stream(FILE *input, const struct fold_options *options, struct fold_line *line);
static int add_byte(struct fold_line *line, const struct fold_options *options, int byte);
static void break_line(struct fold_line *line, const struct fold_options *options);
static size_t advance(size_t column, int byte, const struct fold_options *options);
static void usage(void);

/*
 * Runs fold.
 */
int
main(
	int argc,
	char **argv)
{
	struct fold_options options;
	struct fold_line line;
	int option;
	int index;
	int failed;
	int status;

	/* Reads the options. */
	options.width = FOLD_DEFAULT_WIDTH;
	options.bytes = 0;
	options.spaces = 0;
	for (;;) {
		option = getopt(argc, argv, "bsw:");
		if (option == -1)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'b':
			options.bytes = 1;
			break;
		case 's':
			options.spaces = 1;
			break;
		case 'w':
			status = read_width(optarg, &options.width);
			if (status != 0) {
				fprintf(stderr, "fold: invalid width: '%s'\n", optarg);
				return 1;
			}

			/* The width is set. */
			break;
		default:
			usage();
			break;
		}
	}

	/* Folds each file, or standard input. */
	memset(&line, 0, sizeof(line));
	failed = 0;
	if (optind >= argc) {
		status = fold_operand("-", &options, &line);
		if (status != 0)
			failed = 1;
	}

	/* Each file named, in turn. */
	for (index = optind; index < argc; index++) {
		status = fold_operand(argv[index], &options, &line);
		if (status != 0)
			failed = 1;
	}

	/* A failed write is an error too. */
	status = fflush(stdout);
	if (status != 0) {
		fprintf(stderr, "fold: write error: %s\n", strerror(errno));
		return 1;
	}

	/* Reports whether any file could not be read. */
	if (failed)
		return 1;

	/* Succeeded: every file was folded. */
	return 0;
}

/* Parses a positive decimal width. */
static int
read_width(
	const char *text,
	size_t *width)
{
	char *end;
	unsigned long value;

	/* Converts the whole text. */
	errno = 0;
	value = strtoul(text, &end, 10);
	if (end == text || *end != '\0' || errno != 0)
		return -1;

	/* A width of zero cannot hold a character. */
	if (value == 0)
		return -1;

	/* Succeeded: the width. */
	*width = (size_t)value;
	return 0;
}

/* Opens one operand, - being standard input, and folds it. */
static int
fold_operand(
	const char *path,
	const struct fold_options *options,
	struct fold_line *line)
{
	FILE *input;
	int status;
	int compare;

	/* - is standard input. */
	compare = strcmp(path, "-");
	if (compare == 0) {
		status = fold_stream(stdin, options, line);
		if (status != 0) {
			fprintf(stderr, "fold: standard input: %s\n", strerror(errno));
			return -1;
		}

		/* Succeeded: standard input was folded. */
		return 0;
	}

	/* Opens the file. */
	input = fopen(path, "r");
	if (input == NULL) {
		fprintf(stderr, "fold: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Folds it and closes it. */
	status = fold_stream(input, options, line);
	if (status != 0)
		fprintf(stderr, "fold: %s: %s\n", path, strerror(errno));
	fclose(input);

	/* Reports a read error. */
	if (status != 0)
		return -1;

	/* Succeeded: the file was folded. */
	return 0;
}

/*
 * Folds a stream to standard output.  The last line of a file without a
 * newline is written without one.  Returns -1 after a read or memory
 * error.
 */
static int
fold_stream(
	FILE *input,
	const struct fold_options *options,
	struct fold_line *line)
{
	int byte;
	int status;
	int failed;

	/* Reads byte by byte. */
	for (;;) {
		byte = getc(input);
		if (byte == EOF)
			break;

		/* A newline writes the line as it is and starts the next. */
		if (byte == '\n') {
			fwrite(line->bytes, 1, line->length, stdout);
			putchar('\n');
			line->length = 0;
			line->column = 0;
			continue;
		}

		/* Anything else joins the line, breaking it first if it would not fit. */
		status = add_byte(line, options, byte);
		if (status != 0)
			return -1;
	}

	/* Writes what is left of the last line. */
	fwrite(line->bytes, 1, line->length, stdout);
	line->length = 0;
	line->column = 0;

	/* Tells a read error from the end of the file. */
	failed = ferror(input);
	if (failed)
		return -1;

	/* Succeeded: the whole stream was folded. */
	return 0;
}

/*
 * Adds one byte to the line.  When it would take the line past the width,
 * the line is broken first, unless the line is empty.
 */
static int
add_byte(
	struct fold_line *line,
	const struct fold_options *options,
	int byte)
{
	char *grown;
	size_t capacity;
	size_t column;

	/* Breaks the line first when the byte would pass the width. */
	column = advance(line->column, byte, options);
	if (column > options->width && line->length > 0) {
		break_line(line, options);
		column = advance(line->column, byte, options);
	}

	/* Grows the buffer when it is full. */
	if (line->length >= line->capacity) {
		capacity = line->capacity * 2;
		if (capacity < 256)
			capacity = 256;
		grown = realloc(line->bytes, capacity);
		if (grown == NULL) {
			errno = ENOMEM;
			return -1;
		}

		/* Uses the larger buffer from now on. */
		line->bytes = grown;
		line->capacity = capacity;
	}

	/* Adds the byte. */
	line->bytes[line->length] = (char)byte;
	line->length++;
	line->column = column;
	return 0;
}

/*
 * Writes the line up to its break and a newline, keeping the rest.  With
 * -s the break is after the last blank, if the line has one; otherwise the
 * whole line is written.
 */
static void
break_line(
	struct fold_line *line,
	const struct fold_options *options)
{
	size_t split;
	size_t index;
	size_t column;
	int found;

	/* The whole line, or with -s up to and including the last blank. */
	split = line->length;
	if (options->spaces) {
		found = 0;
		for (index = line->length; index > 0 && !found; index--) {
			if (line->bytes[index - 1] == ' ' || line->bytes[index - 1] == '\t') {
				split = index;
				found = 1;
			}
		}
	}

	/* Writes that part and the newline. */
	fwrite(line->bytes, 1, split, stdout);
	putchar('\n');

	/* Keeps the rest at the start of the next line. */
	memmove(line->bytes, line->bytes + split, line->length - split);
	line->length -= split;

	/* Counts the columns of what was kept. */
	column = 0;
	for (index = 0; index < line->length; index++)
		column = advance(column, (unsigned char)line->bytes[index], options);
	line->column = column;
}

/* Gives the column after a byte written at a column. */
static size_t
advance(
	size_t column,
	int byte,
	const struct fold_options *options)
{
	/* With -b every byte is one column. */
	if (options->bytes)
		return column + 1;

	/* Terminal motion: tab, backspace and carriage return. */
	if (byte == '\t')
		return (column / FOLD_TAB_DISTANCE + 1) * FOLD_TAB_DISTANCE;
	if (byte == '\b') {
		if (column > 0)
			return column - 1;
		return 0;
	}

	/* A carriage return goes back to the start. */
	if (byte == '\r')
		return 0;

	/* Any other byte takes one column. */
	return column + 1;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: fold [-bs] [-w width] [file...]\n");
	exit(1);
}
