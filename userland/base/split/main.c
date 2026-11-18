/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Splits a file into pieces (POSIX XCU split).
 *
 *	split [-l line_count] [-a suffix_length] [file [name]]
 *	split -b n[k|m] [-a suffix_length] [file [name]]
 *
 * The input (standard input for - or no file) is written to files named
 * name (x by default) followed by a suffix of suffix_length lowercase
 * letters (2 by default): aa, ab, ... zz.  Each file holds line_count
 * lines (1000 by default), or with -b n bytes, n kilobytes or n
 * megabytes.  Empty input makes no file.  When the suffixes run out, split
 * stops with an error and keeps the files already written.
 *
 * GNU's extensions are taken too (ws045): -d (--numeric-suffixes[=FROM])
 * makes the suffixes digits counting from FROM, --additional-suffix puts
 * a string after them, --verbose writes each piece's name, -b takes GNU's
 * units (K, M, G ... powers of 1024, KB, MB ... of 1000, KiB, b for 512),
 * the options have their long forms, and options may follow operands
 * unless POSIXLY_CORRECT is set.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* The lines of a piece without -l or -b. */
#define SPLIT_DEFAULT_LINES 1000

/* The suffix length without -a. */
#define SPLIT_DEFAULT_SUFFIX 2

/* The size of the buffer the input is read through. */
#define SPLIT_BUFFER_SIZE 65536

/* The codes of the long options that have no letter. */
#define OPTION_ADDITIONAL 256
#define OPTION_VERBOSE 257
#define OPTION_NUMERIC 258
#define OPTION_HELP 259
#define OPTION_VERSION 260

/*
 * The options written in full, read by the scan of the command line; a
 * long option shares its code with its letter.
 */
static const struct command_long_option split_long_options[] = {
	{"additional-suffix", COMMAND_VALUE_REQUIRED, OPTION_ADDITIONAL},
	{"bytes", COMMAND_VALUE_REQUIRED, 'b'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"lines", COMMAND_VALUE_REQUIRED, 'l'},
	{"numeric-suffixes", COMMAND_VALUE_OPTIONAL, OPTION_NUMERIC},
	{"suffix-length", COMMAND_VALUE_REQUIRED, 'a'},
	{"verbose", COMMAND_VALUE_NONE, OPTION_VERBOSE},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{NULL, 0, 0}
};

/*
 * The pieces being written.
 *
 * One instance lives for the run.  name holds the prefix and the current
 * suffix; descriptor is the open piece, or -1 between pieces.  used counts
 * the lines or bytes already in the open piece.  piece counts the pieces
 * opened; the suffix is piece + first in letters, or digits with -d, and
 * additional (GNU) follows it.
 */
struct split_output {
	char name[PATH_MAX + 1];
	size_t prefix_length;
	size_t suffix_length;
	unsigned long long limit;
	unsigned long long used;
	unsigned long long piece;
	unsigned long long first;
	const char *additional;
	int bytes;
	int numeric;
	int verbose;
	int descriptor;
};

static int read_options(int argc, char **argv, struct split_output *output);
static int parse_size(const char *text, int bytes, unsigned long long *value);
static int parse_unit(const char *text, unsigned long long *multiplier);
static int split_input(int input, struct split_output *output);
static int open_piece(struct split_output *output);
static int next_suffix(struct split_output *output);
static int close_piece(struct split_output *output);
static int write_all(int descriptor, const char *data, size_t length);
static void usage(void);

/*
 * Runs split.
 */
int
main(
	int argc,
	char **argv)
{
	struct split_output output;
	const char *prefix;
	size_t additional_length;
	int count;
	int input;
	int status;
	int compare;
	size_t length;

	/* Reads the options; the file and the name are left from argv[1] on. */
	count = read_options(argc, argv, &output);
	if (count > 2)
		usage();

	/* The prefix of the pieces, x unless a name is given. */
	prefix = "x";
	if (count == 2)
		prefix = argv[2];
	length = strlen(prefix);
	additional_length = 0;
	if (output.additional != NULL)
		additional_length = strlen(output.additional);
	if (length + output.suffix_length + additional_length > PATH_MAX) {
		fprintf(stderr, "split: %s: %s\n", prefix, strerror(ENAMETOOLONG));
		return 1;
	}

	/* Starts the name with the prefix. */
	memcpy(output.name, prefix, length);
	output.prefix_length = length;

	/* Opens the input; - or none is standard input. */
	input = STDIN_FILENO;
	if (count >= 1) {
		compare = strcmp(argv[1], "-");
		if (compare != 0) {
			input = open(argv[1], O_RDONLY);
			if (input < 0) {
				fprintf(stderr, "split: %s: %s\n", argv[1], strerror(errno));
				return 1;
			}
		}
	}

	/* Splits it. */
	status = split_input(input, &output);
	if (status != 0)
		return 1;

	/* Succeeded: every piece was written. */
	return 0;
}

/*
 * Reads the options and returns the number of operands, which are left in
 * argv from argv[1] on.  An invalid option ends split with a diagnostic.
 */
static int
read_options(
	int argc,
	char **argv,
	struct split_output *output)
{
	struct command_options scan;
	unsigned long long value;
	const char *argument;
	int option;
	int status;

	/* The defaults: 1000 lines, two letters. */
	memset(output, 0, sizeof(*output));
	output->limit = SPLIT_DEFAULT_LINES;
	output->suffix_length = SPLIT_DEFAULT_SUFFIX;
	output->descriptor = -1;

	/* Reads each option, and the long forms. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "split";
	scan.letters = "a:b:dl:";
	scan.names = split_long_options;
	command_options_start(&scan);
	for (;;) {
		option = command_options_next(&scan);
		if (option == COMMAND_OPTION_END)
			break;

		/* Records what the option asks for. */
		argument = scan.value;
		switch (option) {
		case 'a':
			status = parse_size(argument, 0, &value);
			if (status != 0 || value > PATH_MAX) {
				fprintf(stderr, "split: invalid suffix length: '%s'\n", argument);
				exit(1);
			}

			/* Keeps the suffix length. */
			output->suffix_length = (size_t)value;
			break;
		case 'b':
			status = parse_size(argument, 1, &value);
			if (status != 0) {
				fprintf(stderr, "split: invalid number of bytes: '%s'\n", argument);
				exit(1);
			}

			/* Pieces are counted in bytes. */
			output->limit = value;
			output->bytes = 1;
			break;
		case 'l':
			status = parse_size(argument, 0, &value);
			if (status != 0) {
				fprintf(stderr, "split: invalid number of lines: '%s'\n", argument);
				exit(1);
			}

			/* Pieces are counted in lines. */
			output->limit = value;
			output->bytes = 0;
			break;
		case 'd':
			output->numeric = 1;
			break;
		case OPTION_NUMERIC:
			output->numeric = 1;
			if (argument != NULL)
				output->first = strtoull(argument, NULL, 10);
			break;
		case OPTION_ADDITIONAL:
			output->additional = argument;
			break;
		case OPTION_VERBOSE:
			output->verbose = 1;
			break;
		case OPTION_VERSION:
			printf("split (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
			break;
		default:
			usage();
			break;
		}
	}

	/* Reports how many operands there are. */
	return scan.operand_count;
}

/*
 * Parses a positive decimal number.  For -b a unit follows: POSIX's k or
 * m, or one of GNU's.
 */
static int
parse_size(
	const char *text,
	int bytes,
	unsigned long long *value)
{
	unsigned long long number;
	unsigned long long multiplier;
	char *end;
	int status;

	/* Converts the digits. */
	if (text[0] < '0' || text[0] > '9')
		return -1;
	errno = 0;
	number = strtoull(text, &end, 10);
	if (errno != 0)
		return -1;

	/* -b takes a unit after the digits; nothing else may follow. */
	multiplier = 1;
	if (bytes) {
		status = parse_unit(end, &multiplier);
		if (status != 0)
			return -1;
	} else if (*end != '\0') {
		return -1;
	}

	/* The size must be positive and fit. */
	if (number == 0)
		return -1;
	if (number > ~0ULL / multiplier)
		return -1;

	/* Succeeded: the number. */
	*value = number * multiplier;
	return 0;
}

/*
 * Reads the unit of a -b count: none, b (512), POSIX's k and m (1024 and
 * its square), or GNU's K, M, G, T, P, E (powers of 1024, also KiB ...)
 * and KB, MB ... (powers of 1000).
 */
static int
parse_unit(
	const char *text,
	unsigned long long *multiplier)
{
	static const char letters[] = "KMGTPE";
	unsigned long long base;
	const char *found;
	char letter;
	int power;

	/* No unit, and b. */
	*multiplier = 1;
	if (text[0] == '\0')
		return 0;
	if (text[0] == 'b' && text[1] == '\0') {
		*multiplier = 512;
		return 0;
	}

	/* The letter of the power; k and m are K and M. */
	letter = text[0];
	if (letter == 'k' || letter == 'm')
		letter = (char)(letter - 'a' + 'A');
	found = NULL;
	if (letter != '\0')
		found = strchr(letters, letter);
	if (found == NULL)
		return -1;
	power = (int)(found - letters) + 1;

	/* The letter alone or with iB is a power of 1024; with B of 1000. */
	base = 1024;
	if (text[1] == 'B' && text[2] == '\0')
		base = 1000;
	else if (text[1] == 'i' && text[2] == 'B' && text[3] == '\0')
		base = 1024;
	else if (text[1] != '\0')
		return -1;

	/* Succeeded: the power of the base. */
	for (; power > 0; power--)
		*multiplier *= base;
	return 0;
}

/*
 * Copies the input into pieces.  A piece is opened only when a byte for
 * it arrives, so empty input makes none.
 */
static int
split_input(
	int input,
	struct split_output *output)
{
	static char buffer[SPLIT_BUFFER_SIZE];
	unsigned long long room;
	ssize_t got;
	size_t offset;
	size_t chunk;
	size_t index;
	int status;

	/* Reads the input chunk by chunk. */
	for (;;) {
		got = read(input, buffer, sizeof(buffer));
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			fprintf(stderr, "split: read error: %s\n", strerror(errno));
			close_piece(output);
			return -1;
		}

		/* The end of the input ends the copy. */
		if (got == 0)
			break;

		/* Hands the chunk out to pieces. */
		offset = 0;
		while (offset < (size_t)got) {
			/* Opens a piece when none is open. */
			if (output->descriptor < 0) {
				status = open_piece(output);
				if (status != 0)
					return -1;
			}

			/*
			 * Takes what fits in the piece: bytes up to the limit, or
			 * lines up to the newline that completes the limit.
			 */
			room = output->limit - output->used;
			chunk = (size_t)got - offset;
			if (output->bytes) {
				if (chunk > room)
					chunk = (size_t)room;
				output->used += chunk;
			} else {
				for (index = offset; index < (size_t)got; index++) {
					if (buffer[index] != '\n')
						continue;
					output->used++;
					if (output->used == output->limit)
						break;
				}

				/* A piece completed inside the chunk ends at that newline. */
				if (index < (size_t)got)
					chunk = index + 1 - offset;
			}

			/* Writes it. */
			status = write_all(output->descriptor, buffer + offset, chunk);
			if (status != 0) {
				fprintf(stderr, "split: %s: %s\n", output->name, strerror(errno));
				close_piece(output);
				return -1;
			}

			/* Goes on after what was written. */
			offset += chunk;

			/* A full piece is closed; the next byte opens another. */
			if (output->used >= output->limit) {
				status = close_piece(output);
				if (status != 0)
					return -1;
			}
		}
	}

	/* Closes the last piece. */
	status = close_piece(output);
	if (status != 0)
		return -1;

	/* Succeeded: the input is in pieces. */
	return 0;
}

/* Opens the piece of the next suffix, failing when the suffixes run out. */
static int
open_piece(
	struct split_output *output)
{
	int status;

	/* The name of the next piece. */
	status = next_suffix(output);
	if (status != 0) {
		fprintf(stderr, "split: output file suffixes exhausted\n");
		return -1;
	}

	/* Creates the piece, replacing a file of that name. */
	output->descriptor = open(output->name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (output->descriptor < 0) {
		fprintf(stderr, "split: %s: %s\n", output->name, strerror(errno));
		return -1;
	}

	/* Succeeded: the piece is empty and open, which --verbose tells. */
	if (output->verbose)
		printf("creating file '%s'\n", output->name);
	output->used = 0;
	return 0;
}

/*
 * Names the next piece: the prefix, the piece's number (from -d's first)
 * in suffix_length letters counting from a (or digits with -d), and the
 * additional suffix.  Returns -1 when the number no longer fits.
 */
static int
next_suffix(
	struct split_output *output)
{
	unsigned long long number;
	unsigned long long radix;
	size_t length;
	size_t index;
	char *suffix;

	/* The piece's number in the radix, from the last place. */
	suffix = output->name + output->prefix_length;
	radix = 26;
	if (output->numeric)
		radix = 10;
	number = output->piece + output->first;
	for (index = output->suffix_length; index > 0; index--) {
		if (output->numeric)
			suffix[index - 1] = (char)('0' + number % radix);
		else
			suffix[index - 1] = (char)('a' + number % radix);
		number /= radix;
	}

	/* A number too big for the suffix: none is left. */
	if (number != 0)
		return -1;

	/* The additional suffix and the end of the name. */
	length = 0;
	if (output->additional != NULL) {
		length = strlen(output->additional);
		memcpy(suffix + output->suffix_length, output->additional, length);
	}

	/* Ends the name; the next piece gets the next number. */
	suffix[output->suffix_length + length] = '\0';
	output->piece++;
	return 0;
}

/* Closes the open piece, if any. */
static int
close_piece(
	struct split_output *output)
{
	int status;

	/* Nothing is open between pieces. */
	if (output->descriptor < 0)
		return 0;

	/* Closes the piece; a failed close can lose written data. */
	status = close(output->descriptor);
	output->descriptor = -1;
	if (status != 0) {
		fprintf(stderr, "split: %s: %s\n", output->name, strerror(errno));
		return -1;
	}

	/* Succeeded: the piece is complete. */
	return 0;
}

/* Writes a whole buffer, however many writes it takes. */
static int
write_all(
	int descriptor,
	const char *data,
	size_t length)
{
	ssize_t written;
	size_t offset;

	/* Writes until every byte is out. */
	offset = 0;
	while (offset < length) {
		written = write(descriptor, data + offset, length - offset);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		/* Goes on after what was written. */
		offset += (size_t)written;
	}

	/* Succeeded: the whole buffer was written. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX forms. */
	fprintf(stderr,
		"usage: split [-l line_count] [-a suffix_length] [file [name]]\n"
		"       split -b n[k|m] [-a suffix_length] [file [name]]\n");
	exit(1);
}
