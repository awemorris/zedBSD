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
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* What to copy. */
struct options {
	unsigned long long count;
	int from_start;		/* +number: start at that line or byte */
	int bytes;		/* -c */
	int follow;		/* -f */
};

/* The whole of an input. */
struct contents {
	char *data;
	size_t length;
};

static int read_options(int argc, char **argv, struct options *options);
static int parse_number(const char *text, struct options *options);
static int tail_stream(const struct options *options, FILE *stream, const char *name, int last);
static int read_all(FILE *stream, struct contents *contents);
static size_t start_of_output(const struct options *options, const struct contents *contents);
static size_t last_lines(const struct contents *contents, unsigned long long count);
static size_t from_line(const struct contents *contents, unsigned long long line);
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
	int first;
	int index;
	int status;
	int headers;
	int printed;
	int compare;
	int last;
	int ok;

	/* The options. */
	memset(&options, 0, sizeof(options));
	first = read_options(argc, argv, &options);

	/* Standard input when there is no file. */
	if (first >= argc) {
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
	int valid;

	/* Ten lines unless the options say otherwise. */
	options->count = 10;

	/* The obsolescent -number or +number, alone before the file. */
	if (argc > 1) {
		word = argv[1];
		if ((word[0] == '-' || word[0] == '+') && word[1] >= '0' &&
		    word[1] <= '9') {
			valid = parse_number(word, options);
			if (!valid)
				usage();
			return 2;
		}
	}

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
			/* -f. */
			if (*letter == 'f') {
				options->follow = 1;
				continue;
			}

			/* -c number and -n number. */
			if (*letter != 'c' && *letter != 'n')
				usage();
			options->bytes = 0;
			if (*letter == 'c')
				options->bytes = 1;
			argument = letter + 1;
			if (*argument == '\0') {
				if (index + 1 >= argc)
					usage();
				index++;
				argument = argv[index];
			}

			/* The count, which may be signed. */
			valid = parse_number(argument, options);
			if (!valid) {
				fprintf(stderr, "tail: invalid number: '%s'\n",
					argument);
				exit(1);
			}

			break;
		}
	}

	/* Succeeded: the first operand. */
	return index;
}

/* Reads [+|-]digits: + counts from the start.  Returns 0 when invalid. */
static int
parse_number(
	const char *text,
	struct options *options)
{
	const char *cursor;

	/* The sign. */
	options->from_start = 0;
	cursor = text;
	if (*cursor == '+') {
		options->from_start = 1;
		cursor++;
	} else if (*cursor == '-') {
		cursor++;
	}

	/* Digits, and nothing else. */
	if (*cursor == '\0')
		return 0;
	options->count = 0;
	for (; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			return 0;
		options->count = options->count * 10U +
		    (unsigned long long)(*cursor - '0');
	}

	/* Succeeded. */
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
		start = from_line(contents, options->count);
	else
		start = last_lines(contents, options->count);

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
	unsigned long long count)
{
	unsigned long long lines;
	size_t position;

	/* Nothing asked for. */
	if (count == 0)
		return contents->length;

	/* Back from the end; the final newline ends the last line. */
	position = contents->length;
	if (position > 0 && contents->data[position - 1U] == '\n')
		position--;
	lines = 0;
	while (position > 0) {
		if (contents->data[position - 1U] == '\n') {
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
	unsigned long long line)
{
	unsigned long long current;
	size_t position;

	/* Line 0 and line 1 are the start. */
	current = 1;
	for (position = 0; position < contents->length && current < line;
	     position++) {
		if (contents->data[position] == '\n')
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
	fprintf(stderr, "usage: tail [-f] [-c number|-n number] [file...]\n");
	exit(1);
}
