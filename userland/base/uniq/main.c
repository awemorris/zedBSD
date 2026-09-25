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
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The options. */
struct options {
	int count;
	int repeated;
	int unique;
	unsigned long fields;
	unsigned long characters;
};

/* A line being read. */
struct line {
	char *data;
	size_t length;
	size_t capacity;
};

static int read_options(int argc, char **argv, struct options *options);
static unsigned long parse_number(const char *text);
static int read_line(FILE *stream, struct line *line);
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
	int first;
	int read;
	int same;
	int compare;

	/* The options and at most two operands. */
	memset(&options, 0, sizeof(options));
	first = read_options(argc, argv, &options);
	if (argc - first > 2)
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
	read = read_line(input, &previous);
	size = 1;
	while (read) {
		/* The next line joins the group, or ends it. */
		read = read_line(input, &current);
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

/* Reads the options; returns the index of the first operand. */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	const char *word;
	const char *letter;
	const char *argument;
	int index;

	/* Each option word. */
	for (index = 1; index < argc; index++) {
		/* An operand, or - alone, ends the options; so does --. */
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* Each letter of the word. */
		for (letter = word + 1; *letter != '\0'; letter++) {
			/* -c, -d and -u. */
			if (*letter == 'c') {
				options->count = 1;
				continue;
			}

			/* -d. */
			if (*letter == 'd') {
				options->repeated = 1;
				continue;
			}

			/* -u. */
			if (*letter == 'u') {
				options->unique = 1;
				continue;
			}

			/* Any other letter but -f and -s is not an option. */
			if (*letter != 'f' && *letter != 's')
				usage();

			/* -f and -s take the rest of the word or the next. */
			argument = letter + 1;
			if (*argument == '\0') {
				if (index + 1 >= argc)
					usage();
				index++;
				argument = argv[index];
			}

			/* The number of fields or of characters. */
			if (*letter == 'f')
				options->fields = parse_number(argument);
			else
				options->characters = parse_number(argument);
			break;
		}
	}

	/* Succeeded: the first operand. */
	return index;
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
	struct line *line)
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
		if (value == '\n') {
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
	int result;

	/* The compared parts, of the same length and bytes. */
	a = compared_part(options, left);
	b = compared_part(options, right);
	if (left->length - a != right->length - b)
		return 0;
	if (left->length == a)
		return 1;
	result = memcmp(left->data + a, right->data + b, left->length - a);
	if (result != 0)
		return 0;

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
	putc('\n', output);
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
	fprintf(stderr, "usage: uniq [-c|-d|-u] [-f fields] [-s chars] "
		"[input [output]]\n");
	exit(1);
}
