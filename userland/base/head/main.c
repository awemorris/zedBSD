/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Copies the first lines of files (POSIX XCU head).
 *
 *	head [-n number | -c number] [file...]
 *	head -number [file...]		(obsolescent)
 *
 * Ten lines by default; -c counts bytes instead (as GNU and the BSDs do).  With several files each is headed by
 * "==> name <==", with a blank line between them.
 *
 * GNU's extensions: a negative number (-n -N, -c -N) copies all but the
 * last N lines or bytes; a number may have a unit (b, kB, K, MB, M, GB,
 * G); -q and -v leave out or force the headers; -z ends lines with a NUL
 * byte; the long options; options after operands (unless POSIXLY_CORRECT).
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/kern/os-name.h"

/* Whether headers are written. */
#define HEADERS_AUTO	0	/* with several files */
#define HEADERS_NEVER	1	/* -q */
#define HEADERS_ALWAYS	2	/* -v */

/* The codes of the long options that have no letter. */
#define OPTION_HELP	256
#define OPTION_VERSION	257

/* What the options ask head to copy. */
struct options {
	unsigned long long count;
	int bytes;		/* -c */
	int all_but;		/* a negative number: all but the last count */
	int headers;
	int delimiter;		/* the end of a line: newline, or NUL (-z) */
};

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option head_long_options[] = {
	{"bytes", COMMAND_VALUE_REQUIRED, 'c'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"lines", COMMAND_VALUE_REQUIRED, 'n'},
	{"quiet", COMMAND_VALUE_NONE, 'q'},
	{"silent", COMMAND_VALUE_NONE, 'q'},
	{"verbose", COMMAND_VALUE_NONE, 'v'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{"zero-terminated", COMMAND_VALUE_NONE, 'z'},
	{NULL, 0, 0}
};

static int read_options(int argc, char **argv, struct options *options);
static void apply_count(struct options *options, const char *text, int bytes);
static int parse_count(const char *text, unsigned long long *count);
static int parse_unit(const char *text, unsigned long long *multiplier);
static void copy_start(FILE *stream, const struct options *options);
static void copy_all_but_bytes(FILE *stream, unsigned long long count);
static void copy_all_but_lines(FILE *stream, unsigned long long count, int delimiter);
static void usage(void);

/*
 * Runs head.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	FILE *stream;
	const char *name;
	int count;
	int first;
	int index;
	int status;
	int headers;
	int printed;
	int compare;

	/* The options; the operands follow argv[0]. */
	count = read_options(argc, argv, &options);
	first = 1;
	argc = first + count;

	/* Standard input when there is no file. */
	if (count == 0) {
		if (options.headers == HEADERS_ALWAYS)
			printf("==> standard input <==\n");
		copy_start(stdin, &options);
		return 0;
	}

	/* Nothing written yet; a header for each file when there are several. */
	status = 0;
	printed = 0;
	headers = 0;
	if (count > 1)
		headers = 1;
	if (options.headers == HEADERS_NEVER)
		headers = 0;
	if (options.headers == HEADERS_ALWAYS)
		headers = 1;
	for (index = first; index < argc; index++) {
		/* - is standard input. */
		name = argv[index];
		stream = stdin;
		compare = strcmp(name, "-");
		if (compare != 0)
			stream = fopen(name, "r");
		else
			name = "standard input";
		if (stream == NULL) {
			fprintf(stderr, "head: cannot open '%s' for reading: "
				"%s\n", name, strerror(errno));
			status = 1;
			continue;
		}

		/* The header, after a blank line when one came before. */
		if (headers) {
			if (printed)
				putchar('\n');
			printf("==> %s <==\n", name);
			printed = 1;
		}

		/* The lines of the file. */
		copy_start(stream, &options);
		if (stream != stdin)
			fclose(stream);
	}

	/* Succeeded. */
	return status;
}

/*
 * Reads the options: -n number, -c number, or the obsolescent -number.
 * Returns the number of operands, which are left in argv from argv[1] on.
 */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	struct command_options scan;
	int code;

	/* Ten lines unless -n or -c says otherwise. */
	memset(options, 0, sizeof(*options));
	options->count = 10;
	options->delimiter = '\n';

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "head";
	scan.letters = "c:n:qvz";
	scan.names = head_long_options;
	scan.numbers = 1;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;

		/* The option of its code. */
		switch (code) {
		case 'n':
		case COMMAND_OPTION_NUMBER:
			apply_count(options, scan.value, 0);
			break;
		case 'c':
			apply_count(options, scan.value, 1);
			break;
		case 'q':
			options->headers = HEADERS_NEVER;
			break;
		case 'v':
			options->headers = HEADERS_ALWAYS;
			break;
		case 'z':
			options->delimiter = '\0';
			break;
		case OPTION_VERSION:
			printf("head (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
		default:
			usage();
		}
	}

	/* Succeeded: the operands follow argv[0]. */
	return scan.operand_count;
}

/*
 * Reads the count of -n or -c (bytes set): a number with a unit, and with
 * a - before it all but the last so many.
 */
static void
apply_count(
	struct options *options,
	const char *text,
	int bytes)
{
	int valid;

	/* A - asks for all but the last so many. */
	options->bytes = bytes;
	options->all_but = 0;
	if (text[0] == '-') {
		options->all_but = 1;
		text++;
	} else if (text[0] == '+') {
		text++;
	}

	/* The count. */
	valid = parse_count(text, &options->count);
	if (!valid) {
		if (bytes)
			fprintf(stderr, "head: invalid number of bytes: '%s'\n", text);
		else
			fprintf(stderr, "head: invalid number of lines: '%s'\n", text);
		exit(1);
	}
}

/*
 * Reads a count: decimal digits, then an optional unit.  Returns 0 when
 * invalid.
 */
static int
parse_count(
	const char *text,
	unsigned long long *count)
{
	unsigned long long multiplier;
	const char *cursor;
	int valid;

	/* At least one digit. */
	if (*text < '0' || *text > '9')
		return 0;
	*count = 0;
	for (cursor = text; *cursor >= '0' && *cursor <= '9'; cursor++)
		*count = *count * 10U + (unsigned long long)(*cursor - '0');

	/* A unit after the digits, or nothing. */
	valid = parse_unit(cursor, &multiplier);
	if (!valid)
		return 0;

	/* Succeeded. */
	*count *= multiplier;
	return 1;
}

/*
 * Reads GNU's unit of a count: none, b (512), kB, MB, GB (powers of 1000),
 * K, M, G, T (powers of 1024, also written KiB and so on).  Returns 0 for
 * anything else.
 */
static int
parse_unit(
	const char *text,
	unsigned long long *multiplier)
{
	static const char letters[] = "KMGTPE";
	const char *found;
	unsigned long long base;
	int power;

	/* No unit, and b. */
	*multiplier = 1;
	if (text[0] == '\0')
		return 1;
	if (text[0] == 'b' && text[1] == '\0') {
		*multiplier = 512;
		return 1;
	}

	/* The letter of the power (k is K). */
	found = strchr(letters, text[0]);
	if (text[0] == 'k')
		found = letters;
	if (found == NULL || text[0] == '\0')
		return 0;
	power = (int)(found - letters) + 1;

	/* K alone or KiB is 1024; KB (kB) is 1000. */
	base = 1024;
	if (text[1] == 'B' && text[2] == '\0') {
		base = 1000;
	} else if (text[1] == 'i' && text[2] == 'B' && text[3] == '\0') {
		base = 1024;
	} else if (text[1] != '\0') {
		return 0;
	}

	/* Succeeded: the power of the base. */
	for (; power > 0; power--)
		*multiplier *= base;
	return 1;
}

/* Copies the start of a stream as the options ask. */
static void
copy_start(
	FILE *stream,
	const struct options *options)
{
	unsigned long long done;
	int value;

	/* All but the last bytes. */
	if (options->all_but && options->bytes) {
		copy_all_but_bytes(stream, options->count);
		return;
	}

	/* All but the last lines. */
	if (options->all_but) {
		copy_all_but_lines(stream, options->count, options->delimiter);
		return;
	}

	/* Byte by byte, counting the line ends or every byte. */
	done = 0;
	while (done < options->count) {
		value = getc(stream);
		if (value == EOF)
			break;
		putchar(value);
		if (options->bytes || value == options->delimiter)
			done++;
	}
}

/* Copies all but the last count bytes, holding them back in a ring. */
static void
copy_all_but_bytes(
	FILE *stream,
	unsigned long long count)
{
	unsigned char *ring;
	size_t size;
	size_t next;
	size_t held;
	int value;

	/* Nothing held back: everything. */
	if (count == 0) {
		for (;;) {
			value = getc(stream);
			if (value == EOF)
				return;
			putchar(value);
		}
	}

	/* The ring of the last bytes read. */
	size = (size_t)count;
	ring = malloc(size);
	if (ring == NULL) {
		fprintf(stderr, "head: out of memory\n");
		exit(1);
	}

	/* Each byte goes into the ring; the one it pushes out is written. */
	next = 0;
	held = 0;
	for (;;) {
		value = getc(stream);
		if (value == EOF)
			break;
		if (held == size)
			putchar(ring[next]);
		else
			held++;
		ring[next] = (unsigned char)value;
		next = (next + 1U) % size;
	}

	/* The last bytes stay unwritten. */
	free(ring);
}

/* Copies all but the last count lines, holding them back in a ring. */
static void
copy_all_but_lines(
	FILE *stream,
	unsigned long long count,
	int delimiter)
{
	char **lines;
	size_t *lengths;
	size_t size;
	size_t next;
	size_t held;
	size_t index;
	char *line;
	size_t length;
	size_t capacity;
	int value;

	/* The ring: count lines and the one being pushed out. */
	size = (size_t)count + 1U;
	lines = calloc(size, sizeof(*lines));
	if (lines == NULL) {
		fprintf(stderr, "head: out of memory\n");
		exit(1);
	}

	/* The length of each line in the ring. */
	lengths = calloc(size, sizeof(*lengths));
	if (lengths == NULL) {
		fprintf(stderr, "head: out of memory\n");
		exit(1);
	}

	/* Each line goes into the ring; the one it pushes out is written. */
	next = 0;
	held = 0;
	line = NULL;
	length = 0;
	capacity = 0;
	for (;;) {
		/* The next byte; the end of a line or of the input ends a line. */
		value = getc(stream);
		if (value != EOF) {
			if (length + 1U > capacity) {
				capacity = capacity * 2U + 128U;
				line = realloc(line, capacity);
				if (line == NULL) {
					fprintf(stderr, "head: out of memory\n");
					exit(1);
				}
			}

			/* The byte, and more unless it ends the line. */
			line[length] = (char)value;
			length++;
			if (value != delimiter)
				continue;
		}

		/* The end of the input with no line begun. */
		if (value == EOF && length == 0)
			break;

		/* The line into the ring, the oldest out when it is full. */
		free(lines[next]);
		lines[next] = line;
		lengths[next] = length;
		next = (next + 1U) % size;
		held++;
		if (held == size) {
			fwrite(lines[next], 1, lengths[next], stdout);
			held--;
		}

		/* A new line. */
		line = NULL;
		length = 0;
		capacity = 0;
		if (value == EOF)
			break;
	}

	/* The lines held back are not written. */
	for (index = 0; index < size; index++)
		free(lines[index]);
	free(lines);
	free(lengths);
	free(line);
}

/* Reports the usage and ends head. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: head [-qvz] [-n [-]number | -c [-]number] "
		"[file...]\n");
	exit(1);
}
