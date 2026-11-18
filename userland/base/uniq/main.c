/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reports or filters out repeated lines (POSIX XCU uniq).
 *
 *	uniq [-c|-d|-u] [-f fields] [-s chars] [input [output]]
 *
 * Adjacent lines that compare equal are one group.  -f skips that many
 * fields (a field is blanks then non-blanks) and -s that many characters
 * before comparing.  Each group is written once; -d writes only groups of
 * more than one line, -u only groups of one, and -c puts the size of the
 * group before the line (padded to 7, as GNU uniq does).
 *
 * GNU's extensions: -i compares ignoring case, -w N compares at most N
 * characters, -z reads and writes lines that end with a NUL byte, the long
 * options, and options after operands (unless POSIXLY_CORRECT is set).
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static const struct command_long_option uniq_long_options[] = {
	{"check-chars", COMMAND_VALUE_REQUIRED, 'w'},
	{"count", COMMAND_VALUE_NONE, 'c'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"ignore-case", COMMAND_VALUE_NONE, 'i'},
	{"repeated", COMMAND_VALUE_NONE, 'd'},
	{"skip-chars", COMMAND_VALUE_REQUIRED, 's'},
	{"skip-fields", COMMAND_VALUE_REQUIRED, 'f'},
	{"unique", COMMAND_VALUE_NONE, 'u'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{"zero-terminated", COMMAND_VALUE_NONE, 'z'},
	{NULL, 0, 0}
};

/* The options. */
struct options {
	int count;
	int repeated;
	int unique;
	unsigned long fields;
	unsigned long characters;

	/* GNU's: case ignored, the characters compared (-w), the end of a line. */
	int ignore_case;
	int limited;
	unsigned long check;
	int end;
};

/* A line being read. */
struct line {
	char *data;
	size_t length;
	size_t capacity;
};

static int read_options(int argc, char **argv, struct options *options);
static unsigned long parse_number(const char *text);
static int read_line(FILE *stream, struct line *line, int end);
static int same_bytes(const char *left, const char *right, size_t length, int ignore_case);
static size_t compared_part(const struct options *options, const struct line *line);
static int same_lines(const struct options *options, const struct line *left, const struct line *right);
static void write_group(const struct options *options, FILE *output, const struct line *line, unsigned long size);
static void swap_lines(struct line *left, struct line *right);
static int is_blank(char value);
static void usage(void);

/*
 * Runs uniq.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	struct line previous;
	struct line current;
	unsigned long size;
	FILE *input;
	FILE *output;
	int count;
	int first;
	int read;
	int same;
	int compare;

	/* The options and at most two operands, which follow argv[0]. */
	memset(&options, 0, sizeof(options));
	options.end = '\n';
	count = read_options(argc, argv, &options);
	first = 1;
	argc = first + count;
	if (count > 2)
		usage();

	/* The input: standard input, - or a file. */
	input = stdin;
	if (first < argc) {
		compare = strcmp(argv[first], "-");
		if (compare != 0)
			input = fopen(argv[first], "r");
		if (input == NULL) {
			fprintf(stderr, "uniq: %s: %s\n", argv[first],
				strerror(errno));
			return 1;
		}
	}

	/* The output: standard output, or a file. */
	output = stdout;
	if (first + 1 < argc) {
		output = fopen(argv[first + 1], "w");
		if (output == NULL) {
			fprintf(stderr, "uniq: %s: %s\n", argv[first + 1],
				strerror(errno));
			return 1;
		}
	}

	/* The first line starts the first group. */
	memset(&previous, 0, sizeof(previous));
	memset(&current, 0, sizeof(current));
	read = read_line(input, &previous, options.end);
	size = 1;
	while (read) {
		/* The next line joins the group, or ends it. */
		read = read_line(input, &current, options.end);
		if (read) {
			same = same_lines(&options, &previous, &current);
			if (same) {
				size++;
				continue;
			}
		}

		/* The group ended: it is written, and the line starts the next. */
		write_group(&options, output, &previous, size);
		swap_lines(&previous, &current);
		size = 1;
	}

	/* Succeeded. */
	free(previous.data);
	free(current.data);
	if (output != stdout)
		fclose(output);
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
	scan.program = "uniq";
	scan.letters = "cdiuzf:s:w:";
	scan.names = uniq_long_options;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;
		switch (code) {
		case 'c':
			options->count = 1;
			break;
		case 'd':
			options->repeated = 1;
			break;
		case 'u':
			options->unique = 1;
			break;
		case 'i':
			options->ignore_case = 1;
			break;
		case 'z':
			options->end = '\0';
			break;
		case 'f':
			options->fields = parse_number(scan.value);
			break;
		case 's':
			options->characters = parse_number(scan.value);
			break;
		case 'w':
			options->check = parse_number(scan.value);
			options->limited = 1;
			break;
		case OPTION_VERSION:
			printf("uniq (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
		default:
			usage();
		}
	}

	/* Succeeded: the operands follow argv[0]. */
	return scan.operand_count;
}

/* Reads a non-negative decimal number. */
static unsigned long
parse_number(
	const char *text)
{
	const char *cursor;
	unsigned long value;

	/* Digits only. */
	if (*text == '\0')
		usage();
	value = 0;
	for (cursor = text; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			usage();
		value = value * 10UL + (unsigned long)(*cursor - '0');
	}

	/* Succeeded. */
	return value;
}

/* Reads one line, without its newline.  Returns 0 at the end. */
static int
read_line(
	FILE *stream,
	struct line *line,
	int end)
{
	int value;
	int newline;

	/* Bytes up to the newline. */
	line->length = 0;
	newline = 0;
	for (;;) {
		/* The next byte. */
		value = getc(stream);
		if (value == EOF)
			break;
		if (value == end) {
			newline = 1;
			break;
		}

		/* Room for it. */
		if (line->length + 1U > line->capacity) {
			line->capacity = line->capacity * 2U + 128U;
			line->data = realloc(line->data, line->capacity);
			if (line->data == NULL) {
				fprintf(stderr, "uniq: out of memory\n");
				exit(1);
			}
		}

		/* The byte. */
		line->data[line->length] = (char)value;
		line->length++;
	}

	/* The end of the input. */
	if (line->length == 0 && !newline)
		return 0;
	return 1;
}

/* Returns where the compared part of a line starts, after -f and -s. */
static size_t
compared_part(
	const struct options *options,
	const struct line *line)
{
	unsigned long field;
	size_t position;
	int blank;

	/* Each skipped field: blanks, then non-blanks. */
	position = 0;
	for (field = 0; field < options->fields; field++) {
		for (; position < line->length; position++) {
			blank = is_blank(line->data[position]);
			if (!blank)
				break;
		}

		/* The non-blanks after them. */
		for (; position < line->length; position++) {
			blank = is_blank(line->data[position]);
			if (blank)
				break;
		}
	}

	/* The skipped characters. */
	if (options->characters > line->length - position)
		return line->length;

	/* Succeeded. */
	return position + options->characters;
}

/* Reports whether two lines compare equal after -f and -s. */
static int
same_lines(
	const struct options *options,
	const struct line *left,
	const struct line *right)
{
	size_t a;
	size_t b;
	size_t left_length;
	size_t right_length;
	int result;

	/* The compared parts, at most -w's characters of each. */
	a = compared_part(options, left);
	b = compared_part(options, right);
	left_length = left->length - a;
	right_length = right->length - b;
	if (options->limited && left_length > options->check)
		left_length = (size_t)options->check;
	if (options->limited && right_length > options->check)
		right_length = (size_t)options->check;

	/* Of the same length and bytes (case aside with -i). */
	if (left_length != right_length)
		return 0;
	result = same_bytes(left->data + a, right->data + b, left_length,
			    options->ignore_case);
	if (!result)
		return 0;

	/* Succeeded: the same. */
	return 1;
}

/* Reports whether two runs of bytes are the same, ignoring case when asked. */
static int
same_bytes(
	const char *left,
	const char *right,
	size_t length,
	int ignore_case)
{
	size_t index;
	int a;
	int b;

	/* Each byte, folded to lower case with -i. */
	for (index = 0; index < length; index++) {
		a = (unsigned char)left[index];
		b = (unsigned char)right[index];
		if (ignore_case && a >= 'A' && a <= 'Z')
			a = a - 'A' + 'a';
		if (ignore_case && b >= 'A' && b <= 'Z')
			b = b - 'A' + 'a';
		if (a != b)
			return 0;
	}

	/* Succeeded: the same. */
	return 1;
}

/* Writes a group, if -d or -u lets it through. */
static void
write_group(
	const struct options *options,
	FILE *output,
	const struct line *line,
	unsigned long size)
{
	/* -d wants repeats only, -u single lines only. */
	if (options->repeated && size < 2U)
		return;
	if (options->unique && size > 1U)
		return;

	/* The count, then the line. */
	if (options->count)
		fprintf(output, "%7lu ", size);
	if (line->length > 0)
		fwrite(line->data, 1, line->length, output);
	putc(options->end, output);
}

/* Swaps two line buffers. */
static void
swap_lines(
	struct line *left,
	struct line *right)
{
	struct line swap;

	/* The whole structures. */
	swap = *left;
	*left = *right;
	*right = swap;
}

/* Reports whether a character is a blank. */
static int
is_blank(
	char value)
{
	/* Space and tab. */
	if (value == ' ' || value == '\t')
		return 1;
	return 0;
}

/* Reports the usage and ends uniq. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: uniq [-c|-d|-u] [-iz] [-f fields] [-s chars] "
		"[-w chars] [input [output]]\n");
	exit(1);
}
