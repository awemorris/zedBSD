/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Encodes or decodes Base64 (GNU base64, RFC 4648).
 *
 *	base64 [-di] [-w cols] [file]
 *
 * The file (standard input for - or none) is encoded, in lines of 76
 * characters, or of cols with -w (0 for one line); -d decodes it instead,
 * skipping newlines.  A character that is not Base64 ends the decoding
 * with an error, unless -i skips such characters.  The long options are
 * --decode, --ignore-garbage and --wrap=cols.
 *
 * base64 is not in POSIX; this is the utility of the GNU core utilities
 * that scripts use (the user's decision for WS045, 2026-09-27).
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* The line length without -w. */
#define BASE64_WRAP_DEFAULT 76

/* The size of the buffers input is read and output written through. */
#define BASE64_BUFFER_SIZE 49152

/* The codes of the long options that have no letter. */
#define OPTION_HELP 256
#define OPTION_VERSION 257

/*
 * The options written in full, read by the scan of the command line; a
 * long option shares its code with its letter.
 */
static const struct command_long_option base64_long_options[] = {
	{"decode", COMMAND_VALUE_NONE, 'd'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"ignore-garbage", COMMAND_VALUE_NONE, 'i'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{"wrap", COMMAND_VALUE_REQUIRED, 'w'},
	{NULL, 0, 0}
};

/*
 * The output being written, and for encoding the column of its line.
 */
struct base64_output {
	unsigned char data[BASE64_BUFFER_SIZE];
	size_t used;
	unsigned long wrap;
	unsigned long column;
	int failed;
};

static const char base64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int encode(int input, struct base64_output *output);
static int decode(int input, struct base64_output *output, int ignore_garbage);
static int value_of(int character);
static void put_encoded(struct base64_output *output, int character);
static void put_byte(struct base64_output *output, int byte);
static void flush_output(struct base64_output *output);
static ssize_t read_some(int input, unsigned char *data, size_t size);
static void usage(void);

/*
 * Runs base64.
 */
int
main(
	int argc,
	char **argv)
{
	static struct base64_output output;
	struct command_options scan;
	unsigned long wrap;
	char *end;
	int option;
	int decoding;
	int ignore_garbage;
	int input;
	int status;
	int compare;

	/* Reads the options. */
	decoding = 0;
	ignore_garbage = 0;
	wrap = BASE64_WRAP_DEFAULT;
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "base64";
	scan.letters = "diw:";
	scan.names = base64_long_options;
	command_options_start(&scan);
	for (;;) {
		option = command_options_next(&scan);
		if (option == COMMAND_OPTION_END)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'd':
			decoding = 1;
			break;
		case 'i':
			ignore_garbage = 1;
			break;
		case 'w':
			errno = 0;
			wrap = strtoul(scan.value, &end, 10);
			if (scan.value[0] < '0' || scan.value[0] > '9' || *end != '\0' || errno != 0) {
				fprintf(stderr, "base64: invalid wrap size: '%s'\n", scan.value);
				return 1;
			}

			/* The wrap is taken. */
			break;
		case OPTION_VERSION:
			printf("base64 (" ZEDBSD_OS_NAME ") 1.0\n");
			return 0;
		default:
			usage();
			break;
		}
	}

	/* One file at most; - or none is standard input. */
	if (scan.operand_count > 1) {
		fprintf(stderr, "base64: extra operand '%s'\n", argv[2]);
		usage();
	}

	/* The input. */
	input = STDIN_FILENO;
	if (scan.operand_count == 1) {
		compare = strcmp(argv[1], "-");
		if (compare != 0) {
			input = open(argv[1], O_RDONLY);
			if (input < 0) {
				command_error("base64", argv[1]);
				return 1;
			}
		}
	}

	/* Encodes or decodes it. */
	output.wrap = wrap;
	if (decoding)
		status = decode(input, &output, ignore_garbage);
	else
		status = encode(input, &output);
	flush_output(&output);
	if (output.failed) {
		command_error("base64", "standard output");
		return 1;
	}

	/* A failure of the conversion. */
	if (status != 0)
		return 1;

	/* Succeeded: all of the input was converted. */
	return 0;
}

/*
 * Encodes the input: each three bytes as four characters, with = for the
 * bytes missing at the end, in lines of the wrap length.
 */
static int
encode(
	int input,
	struct base64_output *output)
{
	static unsigned char data[BASE64_BUFFER_SIZE];
	unsigned long group;
	size_t have;
	size_t index;
	ssize_t got;

	/* Reads whole groups of three, keeping a partial one for the next read. */
	have = 0;
	for (;;) {
		got = read_some(input, data + have, sizeof(data) - have);
		if (got < 0) {
			command_error("base64", "read error");
			return -1;
		}

		/* Counts what came. */
		have += (size_t)got;
		if (got == 0 || have == sizeof(data)) {
			for (index = 0; index + 3 <= have; index += 3) {
				group = ((unsigned long)data[index] << 16) | ((unsigned long)data[index + 1] << 8) | data[index + 2];
				put_encoded(output, base64_alphabet[(group >> 18) & 63]);
				put_encoded(output, base64_alphabet[(group >> 12) & 63]);
				put_encoded(output, base64_alphabet[(group >> 6) & 63]);
				put_encoded(output, base64_alphabet[group & 63]);
			}

			/* The bytes of a partial group move to the front. */
			memmove(data, data + index, have - index);
			have -= index;
		}

		/* The end of the input ends the reading. */
		if (got == 0)
			break;
	}

	/* The last one or two bytes, padded. */
	if (have > 0) {
		group = (unsigned long)data[0] << 16;
		if (have == 2)
			group |= (unsigned long)data[1] << 8;
		put_encoded(output, base64_alphabet[(group >> 18) & 63]);
		put_encoded(output, base64_alphabet[(group >> 12) & 63]);
		if (have == 2)
			put_encoded(output, base64_alphabet[(group >> 6) & 63]);
		else
			put_encoded(output, '=');
		put_encoded(output, '=');
	}

	/* A last line that is not empty ends with a newline. */
	if (output->wrap != 0 && output->column > 0)
		put_byte(output, '\n');
	return 0;
}

/*
 * Decodes the input: newlines are skipped, and other characters that are
 * not Base64 too with -i; padding ends a group.  Returns -1 after a
 * diagnostic for input that is not Base64.
 */
static int
decode(
	int input,
	struct base64_output *output,
	int ignore_garbage)
{
	static unsigned char data[BASE64_BUFFER_SIZE];
	unsigned long group;
	ssize_t got;
	ssize_t index;
	int count;
	int padding;
	int value;

	/* Collects four values at a time. */
	group = 0;
	count = 0;
	padding = 0;
	for (;;) {
		got = read_some(input, data, sizeof(data));
		if (got < 0) {
			command_error("base64", "read error");
			return -1;
		}

		/* The end of the input. */
		if (got == 0)
			break;

		/* Each character. */
		for (index = 0; index < got; index++) {
			if (data[index] == '\n')
				continue;

			/* Padding: only in the third and fourth place. */
			if (data[index] == '=' && (count >= 2 || padding > 0)) {
				padding++;
				count++;
			} else {
				value = value_of(data[index]);
				if (value < 0 && ignore_garbage)
					continue;
				if (value < 0 || padding > 0) {
					fprintf(stderr, "base64: invalid input\n");
					return -1;
				}

				/* One more value of the group. */
				group = (group << 6) | (unsigned long)value;
				count++;
			}

			/* Waits for the rest of the group. */
			if (count < 4)
				continue;

			/* A whole group: three bytes, less the padding. */
			group <<= 6 * padding;
			put_byte(output, (int)((group >> 16) & 0xff));
			if (padding < 2)
				put_byte(output, (int)((group >> 8) & 0xff));
			if (padding < 1)
				put_byte(output, (int)(group & 0xff));
			group = 0;
			count = 0;
			padding = 0;
		}
	}

	/*
	 * A last group of two or three characters without padding holds one
	 * or two bytes, as other systems take it; one character alone, or
	 * padding cut short, is not Base64.
	 */
	if (count == 1 || (count != 0 && padding > 0)) {
		fprintf(stderr, "base64: invalid input\n");
		return -1;
	}

	/* The bytes of the last group. */
	if (count != 0) {
		group <<= 6 * (4 - count);
		put_byte(output, (int)((group >> 16) & 0xff));
		if (count == 3)
			put_byte(output, (int)((group >> 8) & 0xff));
	}

	/* Succeeded: the input was Base64. */
	return 0;
}

/* Returns the value of a Base64 character, or -1. */
static int
value_of(
	int character)
{
	/* The four ranges of the alphabet. */
	if (character >= 'A' && character <= 'Z')
		return character - 'A';
	if (character >= 'a' && character <= 'z')
		return character - 'a' + 26;
	if (character >= '0' && character <= '9')
		return character - '0' + 52;
	if (character == '+')
		return 62;
	if (character == '/')
		return 63;
	return -1;
}

/* Writes an encoded character, and a newline at the end of each line. */
static void
put_encoded(
	struct base64_output *output,
	int character)
{
	/* The character, then the newline when the line is full. */
	put_byte(output, character);
	output->column++;
	if (output->wrap != 0 && output->column == output->wrap) {
		put_byte(output, '\n');
		output->column = 0;
	}
}

/* Adds a byte to the output, writing it out when the buffer is full. */
static void
put_byte(
	struct base64_output *output,
	int byte)
{
	/* A full buffer goes out first. */
	if (output->used == sizeof(output->data))
		flush_output(output);
	output->data[output->used] = (unsigned char)byte;
	output->used++;
}

/* Writes what the output holds. */
static void
flush_output(
	struct base64_output *output)
{
	int status;

	/* Writes it all; a failure is remembered. */
	if (output->used > 0) {
		status = command_write_all(STDOUT_FILENO, output->data, output->used);
		if (status != 0)
			output->failed = 1;
	}

	/* The buffer is empty again. */
	output->used = 0;
}

/* Reads what is there, retrying an interrupted read. */
static ssize_t
read_some(
	int input,
	unsigned char *data,
	size_t size)
{
	ssize_t got;

	/* Reads, again after a signal. */
	for (;;) {
		got = read(input, data, size);
		if (got >= 0 || errno != EINTR)
			break;
	}

	/* What was read, or -1. */
	return got;
}

/* Writes the usage message and exits. */
static void
usage(void)
{
	/* Names the form. */
	fprintf(stderr, "usage: base64 [-di] [-w cols] [file]\n");
	exit(1);
}
