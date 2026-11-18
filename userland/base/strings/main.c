/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Finds printable strings in files (POSIX XCU strings).
 *
 *	strings [-a] [-t format] [-n number] [file...]
 *
 * A string is a run of at least number (4 by default) printable bytes of
 * the POSIX locale, a tab counting as printable; each is written on a
 * line of its own.  -t writes the byte offset of the string first, in
 * decimal (d), octal (o) or hexadecimal (x), right-aligned in seven
 * columns.  The whole of every file is searched, so -a, which asks for
 * that, changes nothing.  - or no file reads standard input.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * What the command line asks for.
 *
 * One instance lives for the run.  radix is 0 without -t.
 */
struct strings_options {
	size_t minimum;
	int radix;
};

/*
 * The run of printable bytes being collected.
 *
 * It grows as long runs need; start is the offset of its first byte.
 */
struct strings_run {
	char *bytes;
	size_t length;
	size_t capacity;
	unsigned long long start;
};

static int read_options(int argc, char **argv, struct strings_options *options);
static int scan_operand(const char *path, const struct strings_options *options, struct strings_run *run);
static int scan_stream(FILE *input, const struct strings_options *options, struct strings_run *run);
static int add_byte(struct strings_run *run, int byte);
static void end_run(const struct strings_options *options, struct strings_run *run);
static int is_printable(int byte);
static void usage(void);

/*
 * Runs strings.
 */
int
main(
	int argc,
	char **argv)
{
	struct strings_options options;
	struct strings_run run;
	int first;
	int index;
	int failed;
	int status;

	/* Reads the options; the files follow them. */
	first = read_options(argc, argv, &options);

	/* Scans each file, or standard input. */
	memset(&run, 0, sizeof(run));
	failed = 0;
	if (first >= argc) {
		status = scan_operand("-", &options, &run);
		if (status != 0)
			failed = 1;
	}

	/* Each file named, in turn. */
	for (index = first; index < argc; index++) {
		status = scan_operand(argv[index], &options, &run);
		if (status != 0)
			failed = 1;
	}

	/* A failed write is an error too. */
	fflush(stdout);
	status = ferror(stdout);
	if (status != 0) {
		fprintf(stderr, "strings: write error\n");
		return 1;
	}

	/* Reports whether any file could not be read. */
	if (failed)
		return 1;

	/* Succeeded: every file was scanned. */
	return 0;
}

/*
 * Reads the options and returns the index of the first file.  An invalid
 * option ends strings with a diagnostic.
 */
static int
read_options(
	int argc,
	char **argv,
	struct strings_options *options)
{
	char *end;
	long number;
	int option;

	/* Four bytes, no offsets. */
	options->minimum = 4;
	options->radix = 0;

	/* Reads each option. */
	for (;;) {
		option = getopt(argc, argv, "an:t:");
		if (option == -1)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'a':
			break;
		case 'n':
			/* The length must be a positive number. */
			errno = 0;
			number = strtol(optarg, &end, 10);
			if (end == optarg || *end != '\0' || number <= 0 || errno != 0) {
				fprintf(stderr, "strings: invalid minimum string length: '%s'\n", optarg);
				exit(1);
			}

			/* Keeps the length. */
			options->minimum = (size_t)number;
			break;
		case 't':
			/* The radix is one letter. */
			if (optarg[0] == 'd' && optarg[1] == '\0') {
				options->radix = 10;
			} else if (optarg[0] == 'o' && optarg[1] == '\0') {
				options->radix = 8;
			} else if (optarg[0] == 'x' && optarg[1] == '\0') {
				options->radix = 16;
			} else {
				fprintf(stderr, "strings: invalid radix: '%s'\n", optarg);
				exit(1);
			}

			/* The radix is kept. */
			break;
		default:
			usage();
			break;
		}
	}

	/* Reports where the files start. */
	return optind;
}

/* Opens one operand, - being standard input, and scans it. */
static int
scan_operand(
	const char *path,
	const struct strings_options *options,
	struct strings_run *run)
{
	FILE *input;
	int status;
	int compare;

	/* - is standard input. */
	compare = strcmp(path, "-");
	if (compare == 0) {
		status = scan_stream(stdin, options, run);
		if (status != 0) {
			fprintf(stderr, "strings: standard input: %s\n", strerror(errno));
			return -1;
		}

		/* Succeeded: standard input was scanned. */
		return 0;
	}

	/* Opens the file. */
	input = fopen(path, "rb");
	if (input == NULL) {
		fprintf(stderr, "strings: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Scans it and closes it. */
	status = scan_stream(input, options, run);
	if (status != 0)
		fprintf(stderr, "strings: %s: %s\n", path, strerror(errno));
	fclose(input);

	/* Reports a read error. */
	if (status != 0)
		return -1;

	/* Succeeded: the file was scanned. */
	return 0;
}

/* Scans a stream, writing each long enough run of printable bytes. */
static int
scan_stream(
	FILE *input,
	const struct strings_options *options,
	struct strings_run *run)
{
	unsigned long long offset;
	int byte;
	int printable;
	int status;
	int failed;

	/* Reads byte by byte, counting offsets from the start of the file. */
	offset = 0;
	run->length = 0;
	for (;;) {
		byte = getc(input);
		if (byte == EOF)
			break;

		/* A printable byte extends the run; anything else ends it. */
		printable = is_printable(byte);
		if (printable) {
			if (run->length == 0)
				run->start = offset;
			status = add_byte(run, byte);
			if (status != 0)
				return -1;
		} else {
			end_run(options, run);
		}

		/* Counts the offset of the next byte. */
		offset++;
	}

	/* The end of the file ends the last run. */
	end_run(options, run);

	/* Tells a read error from the end of the file. */
	failed = ferror(input);
	if (failed)
		return -1;

	/* Succeeded: the stream was scanned. */
	return 0;
}

/* Adds a byte to the run, growing its buffer. */
static int
add_byte(
	struct strings_run *run,
	int byte)
{
	char *grown;
	size_t capacity;

	/* Grows the buffer when it is full. */
	if (run->length + 1 >= run->capacity) {
		capacity = run->capacity * 2;
		if (capacity < 256)
			capacity = 256;
		grown = realloc(run->bytes, capacity);
		if (grown == NULL) {
			errno = ENOMEM;
			return -1;
		}

		/* Uses the larger buffer from now on. */
		run->bytes = grown;
		run->capacity = capacity;
	}

	/* Adds the byte. */
	run->bytes[run->length] = (char)byte;
	run->length++;
	return 0;
}

/* Ends the run: writes it when it is long enough, then empties it. */
static void
end_run(
	const struct strings_options *options,
	struct strings_run *run)
{
	/* A short run is dropped. */
	if (run->length < options->minimum) {
		run->length = 0;
		return;
	}

	/* The offset first with -t. */
	if (options->radix == 10)
		printf("%7llu ", run->start);
	else if (options->radix == 8)
		printf("%7llo ", run->start);
	else if (options->radix == 16)
		printf("%7llx ", run->start);

	/* The string and its newline. */
	fwrite(run->bytes, 1, run->length, stdout);
	putchar('\n');
	run->length = 0;
}

/* Tells whether a byte is printable in the POSIX locale, or a tab. */
static int
is_printable(
	int byte)
{
	/* The ASCII graphic characters, the space and the tab. */
	if (byte >= ' ' && byte <= '~')
		return 1;
	if (byte == '\t')
		return 1;

	/* Anything else is not. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: strings [-a] [-t format] [-n number] [file...]\n");
	exit(1);
}
