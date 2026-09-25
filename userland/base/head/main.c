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
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_options(int argc, char **argv, unsigned long long *count, int *bytes);
static int parse_count(const char *text, unsigned long long *count);
static void copy_lines(FILE *stream, unsigned long long count, int bytes);
static void usage(void);

/*
 * Runs head.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned long long count;
	FILE *stream;
	const char *name;
	int first;
	int index;
	int status;
	int headers;
	int printed;
	int compare;
	int bytes;

	/* The options. */
	first = read_options(argc, argv, &count, &bytes);

	/* Standard input when there is no file. */
	if (first >= argc) {
		copy_lines(stdin, count, bytes);
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
		copy_lines(stream, count, bytes);
		if (stream != stdin)
			fclose(stream);
	}

	/* Succeeded. */
	return status;
}

/*
 * Reads the options: -n number, or the obsolescent -number first.
 * Returns the index of the first operand.
 */
static int
read_options(
	int argc,
	char **argv,
	unsigned long long *count,
	int *bytes)
{
	const char *word;
	const char *argument;
	int index;
	int valid;

	/* Ten lines unless -n or -c says otherwise. */
	*count = 10;
	*bytes = 0;
	for (index = 1; index < argc; index++) {
		/* An operand, or - alone, ends the options; so does --. */
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* -number (obsolescent). */
		if (word[1] >= '0' && word[1] <= '9') {
			valid = parse_count(word + 1, count);
			if (!valid)
				usage();
			continue;
		}

		/* -n number or -c number, or with the number in the word. */
		if (word[1] != 'n' && word[1] != 'c')
			usage();
		*bytes = 0;
		if (word[1] == 'c')
			*bytes = 1;
		argument = word + 2;
		if (*argument == '\0') {
			if (index + 1 >= argc)
				usage();
			index++;
			argument = argv[index];
		}

		/* The count. */
		valid = parse_count(argument, count);
		if (!valid) {
			fprintf(stderr, "head: invalid number of lines: '%s'\n",
				argument);
			exit(1);
		}
	}

	/* Succeeded: the first operand. */
	return index;
}

/* Reads a count of lines: decimal digits only.  Returns 0 when invalid. */
static int
parse_count(
	const char *text,
	unsigned long long *count)
{
	const char *cursor;

	/* At least one digit, and nothing else. */
	if (*text == '\0')
		return 0;
	*count = 0;
	for (cursor = text; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			return 0;
		*count = *count * 10U + (unsigned long long)(*cursor - '0');
	}

	/* Succeeded. */
	return 1;
}

/* Copies the first count lines (or bytes) of a stream. */
static void
copy_lines(
	FILE *stream,
	unsigned long long count,
	int bytes)
{
	unsigned long long done;
	int value;

	/* Byte by byte, counting the newlines or every byte. */
	done = 0;
	while (done < count) {
		value = getc(stream);
		if (value == EOF)
			break;
		putchar(value);
		if (bytes || value == '\n')
			done++;
	}
}

/* Reports the usage and ends head. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: head [-n number | -c number] [file...]\n");
	exit(1);
}
