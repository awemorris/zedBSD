/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Cuts out selected parts of each line (POSIX XCU cut).
 *
 *	cut -b list [-n] [file...]
 *	cut -c list [file...]
 *	cut -f list [-d delim] [-s] [file...]
 *
 * A list is positions and ranges (N, N-M, -M, N-) separated by commas or
 * blanks, counted from 1.  What is selected is written in the order of the
 * line, each part once, whatever order the list gives.  Characters are
 * bytes in the C locale, so -c is -b and -n changes nothing.  Fields are
 * separated by the delimiter (a tab by default) and written with it; a line
 * without the delimiter is written whole, unless -s.
 *
 * GNU's extensions: --complement selects what the list does not,
 * --output-delimiter joins the fields (and the byte ranges) with a string
 * of its own, -z reads and writes lines that end with a NUL byte, the long
 * options, and options after operands (unless POSIXLY_CORRECT is set).
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/kern/os-name.h"

/* What the list counts. */
#define MODE_NONE	0
#define MODE_BYTES	1	/* -b and -c */
#define MODE_FIELDS	2	/* -f */

/* The codes of the long options that have no letter. */
#define OPTION_COMPLEMENT	256
#define OPTION_OUTPUT_DELIMITER	257
#define OPTION_HELP		258
#define OPTION_VERSION		259

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option cut_long_options[] = {
	{"bytes", COMMAND_VALUE_REQUIRED, 'b'},
	{"characters", COMMAND_VALUE_REQUIRED, 'c'},
	{"complement", COMMAND_VALUE_NONE, OPTION_COMPLEMENT},
	{"delimiter", COMMAND_VALUE_REQUIRED, 'd'},
	{"fields", COMMAND_VALUE_REQUIRED, 'f'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"only-delimited", COMMAND_VALUE_NONE, 's'},
	{"output-delimiter", COMMAND_VALUE_REQUIRED, OPTION_OUTPUT_DELIMITER},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{"zero-terminated", COMMAND_VALUE_NONE, 'z'},
	{NULL, 0, 0}
};

/* One range of the list; last is 0 for "to the end". */
struct range {
	unsigned long first;
	unsigned long last;
};

/* The options and the list. */
struct options {
	int mode;
	char delimiter;
	int suppress;
	struct range *ranges;
	size_t count;

	/* GNU's: the selection turned round, the string between parts, and the end of a line. */
	int complement;
	const char *output_delimiter;
	int end;
};

/* A line being read. */
struct line {
	char *data;
	size_t length;
	size_t capacity;
};

static int read_options(int argc, char **argv, struct options *options);
static void apply_list(struct options *options, int code, const char *list);
static void apply_delimiter(struct options *options, const char *text);
static void write_delimiter(const struct options *options);
static void parse_list(struct options *options, const char *list);
static const char *parse_position(const char *cursor, unsigned long *value);
static int selected(const struct options *options, unsigned long position);
static void cut_stream(const struct options *options, FILE *stream);
static void cut_bytes(const struct options *options, const struct line *line);
static void cut_fields(const struct options *options, const struct line *line);
static int read_line(FILE *stream, struct line *line, int end);
static void list_error(const char *message);
static void usage(void);

/*
 * Runs cut.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	FILE *stream;
	int count;
	int first;
	int index;
	int status;
	int compare;

	/* The options; one of -b, -c and -f is required. */
	memset(&options, 0, sizeof(options));
	options.delimiter = '\t';
	options.end = '\n';
	count = read_options(argc, argv, &options);
	first = 1;
	argc = first + count;
	if (options.mode == MODE_NONE)
		usage();

	/* Standard input when there is no file. */
	if (count == 0) {
		cut_stream(&options, stdin);
		return 0;
	}

	/* Each file in turn. */
	status = 0;
	for (index = first; index < argc; index++) {
		/* - is standard input. */
		stream = stdin;
		compare = strcmp(argv[index], "-");
		if (compare != 0)
			stream = fopen(argv[index], "r");
		if (stream == NULL) {
			fprintf(stderr, "cut: %s: %s\n", argv[index],
				strerror(errno));
			status = 1;
			continue;
		}

		/* The selected parts of its lines. */
		cut_stream(&options, stream);
		if (stream != stdin)
			fclose(stream);
	}

	/* Succeeded. */
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
	scan.program = "cut";
	scan.letters = "nszb:c:f:d:";
	scan.names = cut_long_options;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;
		switch (code) {
		case 'n':
			/* Bytes are characters here. */
			break;
		case 's':
			options->suppress = 1;
			break;
		case 'z':
			options->end = '\0';
			break;
		case 'b':
		case 'c':
		case 'f':
			apply_list(options, code, scan.value);
			break;
		case 'd':
			apply_delimiter(options, scan.value);
			break;
		case OPTION_COMPLEMENT:
			options->complement = 1;
			break;
		case OPTION_OUTPUT_DELIMITER:
			options->output_delimiter = scan.value;
			break;
		case OPTION_VERSION:
			printf("cut (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
		default:
			usage();
		}
	}

	/* Succeeded: the operands follow argv[0]. */
	return scan.operand_count;
}

/* Applies -b, -c or -f: the kind of list, which may be given once. */
static void
apply_list(
	struct options *options,
	int code,
	const char *list)
{
	/* Only one of -b, -c and -f. */
	if (options->mode != MODE_NONE) {
		fprintf(stderr, "cut: only one type of list may be specified\n");
		exit(1);
	}

	/* Succeeded: -b and -c select bytes, -f fields, from the list. */
	options->mode = MODE_BYTES;
	if (code == 'f')
		options->mode = MODE_FIELDS;
	parse_list(options, list);
}

/* Applies -d: the delimiter, one character. */
static void
apply_delimiter(
	struct options *options,
	const char *text)
{
	/* One character. */
	if (text[0] == '\0' || text[1] != '\0') {
		fprintf(stderr, "cut: the delimiter must be a single character\n");
		exit(1);
	}

	/* Succeeded. */
	options->delimiter = text[0];
}

/* Writes what goes between two parts: --output-delimiter, or the delimiter. */
static void
write_delimiter(
	const struct options *options)
{
	/* GNU's string of its own. */
	if (options->output_delimiter != NULL) {
		fputs(options->output_delimiter, stdout);
		return;
	}

	/* The delimiter of the fields. */
	putchar(options->delimiter);
}

/* Parses a list of positions and ranges. */
static void
parse_list(
	struct options *options,
	const char *list)
{
	struct range range;
	const char *cursor;
	size_t capacity;

	/* Each range of the list. */
	capacity = 0;
	cursor = list;
	for (;;) {
		/* A position or range: N, N-M, -M or N-. */
		range.first = 1;
		range.last = 0;
		if (*cursor != '-')
			cursor = parse_position(cursor, &range.first);
		if (*cursor == '-') {
			cursor++;
			if (*cursor >= '0' && *cursor <= '9')
				cursor = parse_position(cursor, &range.last);
		} else {
			range.last = range.first;
		}

		/* Ranges count from 1 and do not decrease. */
		if (range.first == 0)
			list_error("fields and positions are numbered from 1");
		if (range.last != 0 && range.last < range.first)
			list_error("invalid decreasing range");

		/* Kept. */
		if (options->count == capacity) {
			capacity = capacity * 2U + 8U;
			options->ranges = realloc(options->ranges,
			    capacity * sizeof(*options->ranges));
			if (options->ranges == NULL)
				list_error("out of memory");
		}

		/* The range, added. */
		options->ranges[options->count] = range;
		options->count++;

		/* A comma or a blank separates the next. */
		if (*cursor == '\0')
			return;
		if (*cursor != ',' && *cursor != ' ' && *cursor != '\t')
			list_error("invalid byte, character or field list");
		cursor++;
	}
}

/* Reads decimal digits; an absent number is an error. */
static const char *
parse_position(
	const char *cursor,
	unsigned long *value)
{
	/* At least one digit. */
	if (*cursor < '0' || *cursor > '9')
		list_error("invalid byte, character or field list");
	*value = 0;
	while (*cursor >= '0' && *cursor <= '9') {
		*value = *value * 10UL + (unsigned long)(*cursor - '0');
		cursor++;
	}

	/* Succeeded: after the digits. */
	return cursor;
}

/* Reports whether the list selects a position. */
static int
selected(
	const struct options *options,
	unsigned long position)
{
	size_t index;

	/* Any range that holds it; --complement turns the answer round. */
	for (index = 0; index < options->count; index++) {
		if (position < options->ranges[index].first)
			continue;
		if (options->ranges[index].last == 0)
			return !options->complement;
		if (position <= options->ranges[index].last)
			return !options->complement;
	}

	/* None. */
	return options->complement;
}

/* Cuts each line of an input. */
static void
cut_stream(
	const struct options *options,
	FILE *stream)
{
	struct line line;
	int read;

	/* Each line of the stream. */
	memset(&line, 0, sizeof(line));
	for (;;) {
		/* The next line. */
		read = read_line(stream, &line, options->end);
		if (!read)
			break;

		/* Its parts. */
		if (options->mode == MODE_FIELDS)
			cut_fields(options, &line);
		else
			cut_bytes(options, &line);
	}

	/* The line's buffer goes. */
	free(line.data);
}

/* Writes the selected bytes of a line. */
static void
cut_bytes(
	const struct options *options,
	const struct line *line)
{
	size_t index;
	size_t previous;
	int chosen;
	int written;

	/* Each byte the list selects; --output-delimiter between ranges. */
	written = 0;
	previous = 0;
	for (index = 0; index < line->length; index++) {
		chosen = selected(options, (unsigned long)index + 1UL);
		if (!chosen)
			continue;
		if (options->output_delimiter != NULL && written &&
		    previous + 1U != index)
			fputs(options->output_delimiter, stdout);
		putchar(line->data[index]);
		written = 1;
		previous = index;
	}

	/* The line ends. */
	putchar(options->end);
}

/*
 * Writes the selected fields of a line, joined by the delimiter.  A line
 * without the delimiter is written whole, unless -s.
 */
static void
cut_fields(
	const struct options *options,
	const struct line *line)
{
	const char *found;
	unsigned long field;
	size_t start;
	size_t end;
	int written;
	int chosen;

	/* A line without the delimiter. */
	found = memchr(line->data, options->delimiter, line->length);
	if (found == NULL) {
		if (options->suppress)
			return;
		fwrite(line->data, 1, line->length, stdout);
		putchar(options->end);
		return;
	}

	/* Each field, from one delimiter to the next. */
	written = 0;
	field = 1;
	start = 0;
	for (;;) {
		end = start;
		while (end < line->length && line->data[end] != options->delimiter)
			end++;
		chosen = selected(options, field);
		if (chosen) {
			if (written)
				write_delimiter(options);
			fwrite(line->data + start, 1, end - start, stdout);
			written = 1;
		}

		/* The field after the delimiter, unless the line ended. */
		if (end >= line->length)
			break;
		start = end + 1U;
		field++;
	}

	/* The line ends. */
	putchar(options->end);
}

/* Reads one line, without its end (a newline, or NUL with -z).  Returns 0 at the end. */
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
			if (line->data == NULL)
				list_error("out of memory");
		}

		/* The byte. */
		line->data[line->length] = (char)value;
		line->length++;
	}

	/* The end of the input. */
	if (line->length == 0 && !newline)
		return 0;

	/* Succeeded: a line (an empty one still has storage). */
	if (line->data == NULL) {
		line->capacity = 128U;
		line->data = malloc(line->capacity);
		if (line->data == NULL)
			list_error("out of memory");
	}

	/* Succeeded: a line. */
	return 1;
}

/* Reports a bad list and ends cut. */
static void
list_error(
	const char *message)
{
	/* The message. */
	fprintf(stderr, "cut: %s\n", message);
	exit(1);
}

/* Reports the usage and ends cut. */
static void
usage(
	void)
{
	/* The forms. */
	fprintf(stderr, "usage: cut -b list [-n] [file...]\n"
		"       cut -c list [file...]\n"
		"       cut -f list [-d delim] [-s] [file...]\n");
	exit(1);
}
