/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Copies the end of files (POSIX XCU tail).
 *
 *	tail [-f] [-c number|-n number] [file...]
 *	tail -number | +number [file]	(obsolescent)
 *
 * number is the lines (or bytes, -c) from the end, or with a + the line (or
 * byte) to start at, counting from 1.  Ten lines by default.  With several
 * files each is headed by "==> name <==", with a blank line between them.
 * -f goes on copying what is added to the last file (a regular file).
 *
 * GNU's extensions: a number may have a unit (b, kB, K, MB, M, ...); -q
 * and -v leave out or force the headers; -z ends lines with a NUL byte;
 * -F is taken as -f; the long options (--lines, --bytes, --follow ...);
 * options after operands (unless POSIXLY_CORRECT is set).
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* Whether headers are written. */
#define HEADERS_AUTO	0	/* with several files */
#define HEADERS_NEVER	1	/* -q */
#define HEADERS_ALWAYS	2	/* -v */

/* The codes of the long options that have no letter. */
#define OPTION_HELP	256
#define OPTION_VERSION	257
#define OPTION_IGNORED	258

/* What to copy. */
struct options {
	unsigned long long count;
	int from_start;		/* +number: start at that line or byte */
	int bytes;		/* -c */
	int follow;		/* -f */
	int headers;
	int delimiter;		/* the end of a line: newline, or NUL (-z) */
};

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option tail_long_options[] = {
	{"bytes", COMMAND_VALUE_REQUIRED, 'c'},
	{"follow", COMMAND_VALUE_OPTIONAL, 'f'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"lines", COMMAND_VALUE_REQUIRED, 'n'},
	{"max-unchanged-stats", COMMAND_VALUE_REQUIRED, OPTION_IGNORED},
	{"pid", COMMAND_VALUE_REQUIRED, OPTION_IGNORED},
	{"quiet", COMMAND_VALUE_NONE, 'q'},
	{"retry", COMMAND_VALUE_NONE, OPTION_IGNORED},
	{"silent", COMMAND_VALUE_NONE, 'q'},
	{"sleep-interval", COMMAND_VALUE_REQUIRED, 's'},
	{"verbose", COMMAND_VALUE_NONE, 'v'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{"zero-terminated", COMMAND_VALUE_NONE, 'z'},
	{NULL, 0, 0}
};

/* The whole of an input. */
struct contents {
	char *data;
	size_t length;
};

static int read_options(int argc, char **argv, struct options *options, int *count);
static int parse_number(const char *text, struct options *options);
static int parse_unit(const char *text, unsigned long long *multiplier);
static int tail_stream(const struct options *options, FILE *stream, const char *name, int last);
static int read_all(FILE *stream, struct contents *contents);
static size_t start_of_output(const struct options *options, const struct contents *contents);
static size_t last_lines(const struct contents *contents, unsigned long long count, int delimiter);
static size_t from_line(const struct contents *contents, unsigned long long line, int delimiter);
static void follow(FILE *stream);
static void usage(void);

/*
 * Runs tail.
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
	int last;
	int ok;

	/* The options; the operands follow them in argv. */
	memset(&options, 0, sizeof(options));
	first = read_options(argc, argv, &options, &count);
	argc = first + count;

	/* Standard input when there is no file. */
	if (first >= argc) {
		if (options.headers == HEADERS_ALWAYS)
			printf("==> standard input <==\n");
		ok = tail_stream(&options, stdin, "standard input", 0);
		if (!ok)
			return 1;
		return 0;
	}

	/* Nothing written yet; a header for each file when there are several. */
	status = 0;
	printed = 0;
	headers = 0;
	if (argc - first > 1)
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
			fprintf(stderr, "tail: cannot open '%s' for reading: "
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

		/* The end of the file; -f follows the last one. */
		last = 0;
		if (index + 1 == argc)
			last = 1;
		ok = tail_stream(&options, stream, name, last);
		if (!ok)
			status = 1;
		if (stream != stdin)
			fclose(stream);
	}

	/* Succeeded. */
	return status;
}

/*
 * Copies the end of one input, and follows it with -f when it is the last.
 * Returns 0 when reading it failed.
 */
static int
tail_stream(
	const struct options *options,
	FILE *stream,
	const char *name,
	int last)
{
	struct contents contents;
	size_t start;
	int ok;

	/* The whole input, and its end. */
	ok = read_all(stream, &contents);
	if (!ok)
		fprintf(stderr, "tail: %s: %s\n", name, strerror(errno));
	start = start_of_output(options, &contents);
	if (start < contents.length)
		fwrite(contents.data + start, 1, contents.length - start, stdout);
	free(contents.data);

	/* -f follows the last file, not standard input. */
	if (options->follow && last && stream != stdin)
		follow(stream);

	/* Succeeded unless reading failed. */
	return ok;
}

/*
 * Reads the options; returns the index of the first operand, and their
 * number in count.  The operands are gathered from argv[1] on, since
 * options may follow them.
 */
static int
read_options(
	int argc,
	char **argv,
	struct options *options,
	int *count)
{
	struct command_options scan;
	const char *word;
	int valid;
	int code;

	/* Ten lines unless the options say otherwise. */
	options->count = 10;
	options->delimiter = '\n';

	/* The obsolescent -number or +number, alone before the file. */
	if (argc > 1) {
		word = argv[1];
		if ((word[0] == '-' || word[0] == '+') && word[1] >= '0' &&
		    word[1] <= '9') {
			valid = parse_number(word, options);
			if (!valid)
				usage();
			*count = argc - 2;
			return 2;
		}
	}

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "tail";
	scan.letters = "c:n:fFqvzs:";
	scan.names = tail_long_options;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;

		/* The option of its code. */
		switch (code) {
		case 'f':
		case 'F':
			options->follow = 1;
			break;
		case 'c':
		case 'n':
			/* The count, which may be signed. */
			options->bytes = 0;
			if (code == 'c')
				options->bytes = 1;
			valid = parse_number(scan.value, options);
			if (!valid) {
				fprintf(stderr, "tail: invalid number: '%s'\n",
					scan.value);
				exit(1);
			}

			/* The count is taken. */
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
		case 's':
		case OPTION_IGNORED:
			/* How -f watches is tail's own. */
			break;
		case OPTION_VERSION:
			printf("tail (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
		default:
			usage();
		}
	}

	/* Succeeded: the operands follow argv[0]. */
	*count = scan.operand_count;
	return 1;
}

/* Reads [+|-]digits: + counts from the start.  Returns 0 when invalid. */
static int
parse_number(
	const char *text,
	struct options *options)
{
	unsigned long long multiplier;
	const char *cursor;
	int valid;

	/* The sign. */
	options->from_start = 0;
	cursor = text;
	if (*cursor == '+') {
		options->from_start = 1;
		cursor++;
	} else if (*cursor == '-') {
		cursor++;
	}

	/* Digits. */
	if (*cursor < '0' || *cursor > '9')
		return 0;
	options->count = 0;
	for (; *cursor >= '0' && *cursor <= '9'; cursor++) {
		options->count = options->count * 10U +
		    (unsigned long long)(*cursor - '0');
	}

	/* A unit after them, or nothing. */
	valid = parse_unit(cursor, &multiplier);
	if (!valid)
		return 0;

	/* Succeeded. */
	options->count *= multiplier;
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

/* Reads the whole of a stream.  Returns 0 when reading failed. */
static int
read_all(
	FILE *stream,
	struct contents *contents)
{
	char chunk[8192];
	size_t count;
	size_t capacity;
	char *grown;
	int failed;

	/* Nothing read yet. */
	contents->data = NULL;
	contents->length = 0;
	capacity = 0;
	for (;;) {
		/* The next chunk. */
		count = fread(chunk, 1, sizeof(chunk), stream);
		if (count == 0)
			break;

		/* Room for it. */
		if (contents->length + count > capacity) {
			capacity = (contents->length + count) * 2U;
			grown = realloc(contents->data, capacity);
			if (grown == NULL) {
				fprintf(stderr, "tail: out of memory\n");
				exit(1);
			}

			/* The block, grown. */
			contents->data = grown;
		}

		/* The chunk, at the end. */
		memcpy(contents->data + contents->length, chunk, count);
		contents->length += count;
	}

	/* Succeeded unless the stream failed. */
	failed = ferror(stream);
	if (failed)
		return 0;
	return 1;
}

/* Returns where the output starts in the input. */
static size_t
start_of_output(
	const struct options *options,
	const struct contents *contents)
{
	size_t start;

	/* Bytes: from byte number, or the last count bytes. */
	if (options->bytes && options->from_start) {
		if (options->count == 0)
			return 0;
		if (options->count - 1U >= contents->length)
			return contents->length;
		return (size_t)(options->count - 1U);
	}

	/* -c counts bytes from the end. */
	if (options->bytes) {
		if (options->count >= contents->length)
			return 0;
		return contents->length - (size_t)options->count;
	}

	/* Lines: from a line number, or the last count lines. */
	if (options->from_start)
		start = from_line(contents, options->count, options->delimiter);
	else
		start = last_lines(contents, options->count, options->delimiter);

	/* Succeeded. */
	return start;
}

/*
 * Returns where the last count lines start.  A last line without a newline
 * is a line.
 */
static size_t
last_lines(
	const struct contents *contents,
	unsigned long long count,
	int delimiter)
{
	unsigned long long lines;
	size_t position;

	/* Nothing asked for. */
	if (count == 0)
		return contents->length;

	/* Back from the end; the final newline ends the last line. */
	position = contents->length;
	if (position > 0 && contents->data[position - 1U] == (char)delimiter)
		position--;
	lines = 0;
	while (position > 0) {
		if (contents->data[position - 1U] == (char)delimiter) {
			lines++;
			if (lines == count)
				return position;
		}

		/* The byte before. */
		position--;
	}

	/* Succeeded: fewer lines than asked, so all of them. */
	return 0;
}

/* Returns where line number line (from 1) starts. */
static size_t
from_line(
	const struct contents *contents,
	unsigned long long line,
	int delimiter)
{
	unsigned long long current;
	size_t position;

	/* Line 0 and line 1 are the start. */
	current = 1;
	for (position = 0; position < contents->length && current < line;
	     position++) {
		if (contents->data[position] == (char)delimiter)
			current++;
	}

	/* Succeeded. */
	return position;
}

/* -f: copies what is added to a regular file, checking every second. */
static void
follow(
	FILE *stream)
{
	struct stat status;
	char chunk[8192];
	size_t count;
	int error;
	int regular;

	/* Only a regular file grows in a way that can be followed. */
	error = fstat(fileno(stream), &status);
	if (error != 0)
		return;
	regular = S_ISREG(status.st_mode);
	if (!regular)
		return;

	/* What was written before comes out first. */
	fflush(stdout);

	/* Until tail is killed. */
	for (;;) {
		/* What is there now, then a pause. */
		clearerr(stream);
		count = fread(chunk, 1, sizeof(chunk), stream);
		if (count > 0) {
			fwrite(chunk, 1, count, stdout);
			fflush(stdout);
			continue;
		}

		/* A pause before looking again. */
		sleep(1);
	}
}

/* Reports the usage and ends tail. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: tail [-fqvz] [-c number|-n number] [file...]\n");
	exit(1);
}
