/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD cmp userland command.
 *
 * GNU's extensions: -i SKIP1[:SKIP2] (--ignore-initial), -n LIMIT
 * (--bytes), --silent and --quiet for -s, --verbose for -l, and options
 * after operands (unless POSIXLY_CORRECT is set).
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

struct cmp_reader {
	const char *name;
	unsigned char buffer[4096];
	size_t position;
	size_t length;
	int descriptor;
	int owned;
	int eof;
};

/* The codes of the long options that have no letter. */
#define CMP_OPTION_HELP		256
#define CMP_OPTION_VERSION	257

/*
 * What the command line asks of the comparison.
 *
 * The skips of -i apply unless skip operands are given; limit (with -n)
 * is the most bytes compared.
 */
struct cmp_options {
	int list;
	int silent;
	int operand_count;
	unsigned long long first_skip;
	unsigned long long second_skip;
	unsigned long long limit;
	int limited;
};

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option cmp_long_options[] = {
	{"bytes", COMMAND_VALUE_REQUIRED, 'n'},
	{"help", COMMAND_VALUE_NONE, CMP_OPTION_HELP},
	{"ignore-initial", COMMAND_VALUE_REQUIRED, 'i'},
	{"quiet", COMMAND_VALUE_NONE, 's'},
	{"silent", COMMAND_VALUE_NONE, 's'},
	{"verbose", COMMAND_VALUE_NONE, 'l'},
	{"version", COMMAND_VALUE_NONE, CMP_OPTION_VERSION},
	{NULL, 0, 0}
};

static int cmp_parse_options(int argc, char **argv, struct cmp_options *options);
static void cmp_usage(void);
static int cmp_parse_skip(const char *text, unsigned long long *result);
static int cmp_parse_ignore(const char *text, struct cmp_options *options);
static int cmp_reader_open(struct cmp_reader *reader, const char *name);
static int cmp_reader_close(struct cmp_reader *reader);
static int cmp_reader_skip(struct cmp_reader *reader, unsigned long long count);
static int cmp_reader_next(struct cmp_reader *reader, unsigned char *value);
static int cmp_report_eof(const struct cmp_options *options, const char *name);
static int cmp_report_difference(const struct cmp_options *options, const char *first_name, const char *second_name, unsigned long long byte, unsigned long long line, unsigned char first, unsigned char second);

/*
 * Compares two byte streams using independent buffered reader state.
 */
int
main(
	int argc,
	char **argv)
{
	struct cmp_options options;
	struct cmp_reader first;
	struct cmp_reader second;
	unsigned long long first_skip;
	unsigned long long second_skip;
	unsigned long long byte;
	unsigned long long line;
	unsigned char first_value;
	unsigned char second_value;
	int first_status;
	int second_status;
	int first_operand;
	int operand_count;
	int result;
	int status;

	/* The options; the operands follow argv[0]. */
	memset(&options, 0, sizeof(options));
	first_operand = cmp_parse_options(argc, argv, &options);

	/* Handles the first operand condition. */
	if (first_operand < 0) {
		cmp_usage();

		/* Reports operation failure. */
		return 2;
	}

	/* Two to four operands: the files and their skips. */
	operand_count = options.operand_count;
	if (operand_count < 2 || operand_count > 4) {
		cmp_usage();

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the selected command-line operation. */
	if (strcmp(argv[first_operand], "-") == 0 &&
	    strcmp(argv[first_operand + 1], "-") == 0) {
		fprintf(stderr, "cmp: both operands cannot be standard input\n");

		/* Reports operation failure. */
		return 2;
	}

	/* The skips of -i, unless the operands give their own. */
	first_skip = options.first_skip;
	second_skip = options.second_skip;

	/* Validates the command-line arguments. */
	if (operand_count >= 3 &&
	    cmp_parse_skip(argv[first_operand + 2], &first_skip) != 0) {
		command_error("cmp", argv[first_operand + 2]);

		/* Reports operation failure. */
		return 2;
	}

	/* Validates the command-line arguments. */
	if (operand_count == 4 &&
	    cmp_parse_skip(argv[first_operand + 3], &second_skip) != 0) {
		command_error("cmp", argv[first_operand + 3]);

		/* Reports operation failure. */
		return 2;
	}

	status = cmp_reader_open(&first, argv[first_operand]);

	/* Checks the operation status. */
	if (status != 0) {
		command_error("cmp", argv[first_operand]);

		/* Reports operation failure. */
		return 2;
	}
	status = cmp_reader_open(&second, argv[first_operand + 1]);

	/* Checks the operation status. */
	if (status != 0) {
		command_error("cmp", argv[first_operand + 1]);
		cmp_reader_close(&first);

		/* Reports operation failure. */
		return 2;
	}

	result = 0;
	status = cmp_reader_skip(&first, first_skip);

	/* Checks the operation status. */
	if (status != 0) {
		command_error("cmp", first.name);
		result = 2;
	}

	/* Checks the operation result. */
	if (result == 0) {
		status = cmp_reader_skip(&second, second_skip);

		/* Checks the operation status. */
		if (status != 0) {
			command_error("cmp", second.name);
			result = 2;
		}
	}

	byte = 1;
	line = 1;

	/* Compare one logical byte at a time across arbitrary read boundaries. */
	while (result < 2) {
		/* -n: the bytes after the limit are not compared. */
		if (options.limited && byte > options.limit)
			break;

		/* The next byte of each. */
		first_status = cmp_reader_next(&first, &first_value);

		/* Handles the first status condition. */
		if (first_status < 0) {
			command_error("cmp", first.name);
			result = 2;
			break;
		}
		second_status = cmp_reader_next(&second, &second_value);

		/* Handles the second status condition. */
		if (second_status < 0) {
			command_error("cmp", second.name);
			result = 2;
			break;
		}

		/* Handles the first status condition. */
		if (first_status == 0 || second_status == 0) {
			/* Handles the first status condition. */
			if (first_status != second_status) {
				status = cmp_report_eof(
					&options,
					first_status == 0 ? first.name : second.name);
				result = status == 0 ? 1 : 2;
			}
			break;
		}

		/* Handles the first value condition. */
		if (first_value != second_value) {
			status = cmp_report_difference(
				&options,
				first.name,
				second.name,
				byte,
				line,
				first_value,
				second_value);

			/* Checks the operation status. */
			if (status != 0) {
				result = 2;
				break;
			}
			result = 1;

			/* Checks the selected options. */
			if (!options.list)
				break;
		}

		/* Handles the first value condition. */
		if (first_value == '\n') {
			/* Handles the line condition. */
			if (line == ULLONG_MAX) {
				errno = EOVERFLOW;
				command_error("cmp", "line number");
				result = 2;
				break;
			}
			line++;
		}

		/* Classifies the current byte. */
		if (byte == ULLONG_MAX) {
			errno = EOVERFLOW;
			command_error("cmp", "byte number");
			result = 2;
			break;
		}
		byte++;
	}

	status = cmp_reader_close(&first);

	/* Checks the operation status. */
	if (status != 0) {
		command_error("cmp", first.name);
		result = 2;
	}
	status = cmp_reader_close(&second);

	/* Checks the operation status. */
	if (status != 0) {
		command_error("cmp", second.name);
		result = 2;
	}

	/* Handles the end-of-file condition. */
	if (fflush(stdout) == EOF) {
		command_error("cmp", "standard output");
		result = 2;
	}

	/* Returns the computed result. */
	return result;
}

/*
 * Parses the options: the output modes (-l and -s, which exclude each
 * other), and GNU's -i and -n.  Returns the index of the first operand
 * (the operands are gathered from argv[1] on), or -1 for a usage error.
 */
static int
cmp_parse_options(
	int argc,
	char **argv,
	struct cmp_options *options)
{
	struct command_options scan;
	int valid;
	int code;

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "cmp";
	scan.letters = "lsi:n:";
	scan.names = cmp_long_options;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;

		/* The option of its code. */
		switch (code) {
		case 'l':
			options->list = 1;
			break;
		case 's':
			options->silent = 1;
			break;
		case 'i':
			valid = cmp_parse_ignore(scan.value, options);
			if (!valid)
				return -1;
			break;
		case 'n':
			valid = cmp_parse_skip(scan.value, &options->limit);
			if (valid != 0)
				return -1;
			options->limited = 1;
			break;
		case CMP_OPTION_VERSION:
			printf("cmp (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
		default:
			return -1;
		}
	}

	/* -l and -s exclude each other. */
	if (options->list && options->silent)
		return -1;

	/* Succeeded: the operands follow argv[0]. */
	options->operand_count = scan.operand_count;
	return 1;
}

/*
 * Parses GNU's -i SKIP1[:SKIP2]: the bytes skipped at the start of each
 * file (SKIP2 defaults to SKIP1).  Returns 0 when invalid.
 */
static int
cmp_parse_ignore(
	const char *text,
	struct cmp_options *options)
{
	char first[32];
	const char *colon;
	size_t length;
	int status;

	/* One value for both files. */
	colon = strchr(text, ':');
	if (colon == NULL) {
		status = cmp_parse_skip(text, &options->first_skip);
		if (status != 0)
			return 0;
		options->second_skip = options->first_skip;
		return 1;
	}

	/* The first value, before the colon. */
	length = (size_t)(colon - text);
	if (length >= sizeof(first))
		return 0;
	memcpy(first, text, length);
	first[length] = '\0';
	status = cmp_parse_skip(first, &options->first_skip);
	if (status != 0)
		return 0;

	/* The second, after it. */
	status = cmp_parse_skip(colon + 1, &options->second_skip);
	if (status != 0)
		return 0;

	/* Succeeded. */
	return 1;
}

/* Prints the accepted POSIX and historical operand surface. */
static void
cmp_usage(
	void)
{
	fprintf(stderr, "usage: cmp [-l | -s] file1 file2 [skip1 [skip2]]\n");
}

/* Parses a nonnegative decimal or leading-zero octal skip value. */
static int
cmp_parse_skip(
	const char *text,
	unsigned long long *result)
{
	char *end;
	unsigned long long value;
	int base;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0' || *text == '-' || *text == '+') {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	base = text[0] == '0' && text[1] != '\0' ? 8 : 10;
	errno = 0;
	value = strtoull(text, &end, base);

	/* Handles the reported system error. */
	if (errno != 0 || *end != '\0') {
		/* Handles the reported system error. */
		if (errno == 0)
			errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	*result = value;

	/* Reports successful completion. */
	return 0;
}

/* Opens one operand and initializes its reader state. */
static int
cmp_reader_open(
	struct cmp_reader *reader,
	const char *name)
{
	memset(reader, 0, sizeof(*reader));
	reader->name = name;

	/* Selects the matching value. */
	if (strcmp(name, "-") == 0) {
		reader->descriptor = STDIN_FILENO;
		reader->owned = 0;
	} else {
		reader->descriptor = open(name, O_RDONLY);

		/* Handles the reader condition. */
		if (reader->descriptor < 0)
			return -1;
		reader->owned = 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Closes an owned input descriptor. */
static int
cmp_reader_close(
	struct cmp_reader *reader)
{
	int function_result;

	/* Handles the reader condition. */
	if (!reader->owned)
		return 0;

	reader->owned = 0;

	/* Obtains the close result. */
	function_result = close(reader->descriptor);

	/* Returns the computed result. */
	return function_result;
}

/* Discards an optional historical byte-offset operand. */
static int
cmp_reader_skip(
	struct cmp_reader *reader,
	unsigned long long count)
{
	unsigned char value;
	int status;

	/* Consume rather than seek so every supported file type behaves alike. */
	while (count != 0) {
		status = cmp_reader_next(reader, &value);

		/* Checks the operation status. */
		if (status < 0)
			return -1;

		/* Checks the operation status. */
		if (status == 0)
			return 0;
		count--;
	}

	/* Reports successful completion. */
	return 0;
}

/* Returns one byte, zero at EOF, or a negative value on error. */
static int
cmp_reader_next(
	struct cmp_reader *reader,
	unsigned char *value)
{
	ssize_t count;

	/* Handles the reader condition. */
	if (reader->position < reader->length) {
		*value = reader->buffer[reader->position++];

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the reader condition. */
	if (reader->eof)
		return 0;

	/* Refill this reader independently of the other input. */
	for (;;) {
		count = read(
			reader->descriptor,
			reader->buffer,
			sizeof(reader->buffer));

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		break;
	}

	/* Checks the remaining item count. */
	if (count < 0)
		return -1;

	/* Checks the remaining item count. */
	if (count == 0) {
		reader->eof = 1;

		/* Reports successful completion. */
		return 0;
	}

	reader->position = 1;
	reader->length = (size_t)count;
	*value = reader->buffer[0];

	/* Reports operation failure. */
	return 1;
}

/* Reports an unequal-length comparison unless silent mode was selected. */
static int
cmp_report_eof(
	const struct cmp_options *options,
	const char *name)
{
	int status;

	/* Checks the selected options. */
	if (options->silent)
		return 0;

	status = fprintf(stderr, "cmp: EOF on %s\n", name);

	/* Checks the operation status. */
	if (status < 0)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Emits one byte-difference record in the selected format. */
static int
cmp_report_difference(
	const struct cmp_options *options,
	const char *first_name,
	const char *second_name,
	unsigned long long byte,
	unsigned long long line,
	unsigned char first,
	unsigned char second)
{
	int status;

	/* Checks the selected options. */
	if (options->silent)
		return 0;

	/* Checks the selected options. */
	if (options->list) {
		status = printf(
			"%llu %o %o\n",
			byte,
			(unsigned)first,
			(unsigned)second);
	} else {
		status = printf(
			"%s %s differ: char %llu, line %llu\n",
			first_name,
			second_name,
			byte,
			line);
	}

	/* Checks the operation status. */
	if (status < 0)
		return -1;

	/* Reports successful completion. */
	return 0;
}
