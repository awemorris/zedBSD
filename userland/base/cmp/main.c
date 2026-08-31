/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct cmp_reader {
	const char *name;
	unsigned char buffer[4096];
	size_t position;
	size_t length;
	int descriptor;
	int owned;
	int eof;
};

struct cmp_options {
	int list;
	int silent;
};

static void cmp_usage(void);
static int cmp_parse_options(int argc, char **argv, struct cmp_options *options);
static int cmp_parse_skip(const char *text, unsigned long long *result);
static int cmp_reader_open(struct cmp_reader *reader, const char *name);
static int cmp_reader_close(struct cmp_reader *reader);
static int cmp_reader_next(struct cmp_reader *reader, unsigned char *value);
static int cmp_reader_skip(struct cmp_reader *reader, unsigned long long count);
static int cmp_report_difference(const struct cmp_options *options, const char *first_name, const char *second_name, unsigned long long byte, unsigned long long line, unsigned char first, unsigned char second);
static int cmp_report_eof(const struct cmp_options *options, const char *name);

/* Compares two byte streams using independent buffered reader state. */
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

	memset(&options, 0, sizeof(options));
	first_operand = cmp_parse_options(argc, argv, &options);
	if (first_operand < 0) {
		cmp_usage();

		return 2;
	}

	operand_count = argc - first_operand;
	if (operand_count < 2 || operand_count > 4) {
		cmp_usage();

		return 2;
	}
	if (strcmp(argv[first_operand], "-") == 0 &&
	    strcmp(argv[first_operand + 1], "-") == 0) {
		fprintf(stderr, "cmp: both operands cannot be standard input\n");

		return 2;
	}

	first_skip = 0;
	second_skip = 0;
	if (operand_count >= 3 &&
	    cmp_parse_skip(argv[first_operand + 2], &first_skip) != 0) {
		command_error("cmp", argv[first_operand + 2]);

		return 2;
	}
	if (operand_count == 4 &&
	    cmp_parse_skip(argv[first_operand + 3], &second_skip) != 0) {
		command_error("cmp", argv[first_operand + 3]);

		return 2;
	}

	status = cmp_reader_open(&first, argv[first_operand]);
	if (status != 0) {
		command_error("cmp", argv[first_operand]);

		return 2;
	}
	status = cmp_reader_open(&second, argv[first_operand + 1]);
	if (status != 0) {
		command_error("cmp", argv[first_operand + 1]);
		cmp_reader_close(&first);

		return 2;
	}

	result = 0;
	status = cmp_reader_skip(&first, first_skip);
	if (status != 0) {
		command_error("cmp", first.name);
		result = 2;
	}
	if (result == 0) {
		status = cmp_reader_skip(&second, second_skip);
		if (status != 0) {
			command_error("cmp", second.name);
			result = 2;
		}
	}

	byte = 1;
	line = 1;

	/* Compare one logical byte at a time across arbitrary read boundaries. */
	while (result < 2) {
		first_status = cmp_reader_next(&first, &first_value);
		if (first_status < 0) {
			command_error("cmp", first.name);
			result = 2;
			break;
		}
		second_status = cmp_reader_next(&second, &second_value);
		if (second_status < 0) {
			command_error("cmp", second.name);
			result = 2;
			break;
		}

		if (first_status == 0 || second_status == 0) {
			if (first_status != second_status) {
				status = cmp_report_eof(
					&options,
					first_status == 0 ? first.name : second.name);
				result = status == 0 ? 1 : 2;
			}
			break;
		}

		if (first_value != second_value) {
			status = cmp_report_difference(
				&options,
				first.name,
				second.name,
				byte,
				line,
				first_value,
				second_value);
			if (status != 0) {
				result = 2;
				break;
			}
			result = 1;
			if (!options.list)
				break;
		}

		if (first_value == '\n') {
			if (line == ULLONG_MAX) {
				errno = EOVERFLOW;
				command_error("cmp", "line number");
				result = 2;
				break;
			}
			line++;
		}
		if (byte == ULLONG_MAX) {
			errno = EOVERFLOW;
			command_error("cmp", "byte number");
			result = 2;
			break;
		}
		byte++;
	}

	status = cmp_reader_close(&first);
	if (status != 0) {
		command_error("cmp", first.name);
		result = 2;
	}
	status = cmp_reader_close(&second);
	if (status != 0) {
		command_error("cmp", second.name);
		result = 2;
	}
	if (fflush(stdout) == EOF) {
		command_error("cmp", "standard output");
		result = 2;
	}

	return result;
}

/* Prints the accepted POSIX and historical operand surface. */
static void
cmp_usage(
	void)
{
	fprintf(stderr, "usage: cmp [-l | -s] file1 file2 [skip1 [skip2]]\n");
}

/* Parses the mutually exclusive comparison output modes. */
static int
cmp_parse_options(
	int argc,
	char **argv,
	struct cmp_options *options)
{
	int option;

	opterr = 0;

	/* Accept only the two standard output modes. */
	while ((option = getopt(argc, argv, "ls")) != -1) {
		switch (option) {
		case 'l':
			options->list = 1;
			break;
		case 's':
			options->silent = 1;
			break;
		default:
			return -1;
		}
	}
	if (options->list && options->silent)
		return -1;

	return optind;
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

	if (text == NULL || *text == '\0' || *text == '-' || *text == '+') {
		errno = EINVAL;

		return -1;
	}
	base = text[0] == '0' && text[1] != '\0' ? 8 : 10;
	errno = 0;
	value = strtoull(text, &end, base);
	if (errno != 0 || *end != '\0') {
		if (errno == 0)
			errno = EINVAL;

		return -1;
	}

	*result = value;

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
	if (strcmp(name, "-") == 0) {
		reader->descriptor = STDIN_FILENO;
		reader->owned = 0;
	} else {
		reader->descriptor = open(name, O_RDONLY);
		if (reader->descriptor < 0)
			return -1;
		reader->owned = 1;
	}

	return 0;
}

/* Closes an owned input descriptor. */
static int
cmp_reader_close(
	struct cmp_reader *reader)
{
	if (!reader->owned)
		return 0;

	reader->owned = 0;

	return close(reader->descriptor);
}

/* Returns one byte, zero at EOF, or a negative value on error. */
static int
cmp_reader_next(
	struct cmp_reader *reader,
	unsigned char *value)
{
	ssize_t count;

	if (reader->position < reader->length) {
		*value = reader->buffer[reader->position++];

		return 1;
	}
	if (reader->eof)
		return 0;

	/* Refill this reader independently of the other input. */
	for (;;) {
		count = read(
			reader->descriptor,
			reader->buffer,
			sizeof(reader->buffer));
		if (count < 0 && errno == EINTR)
			continue;

		break;
	}
	if (count < 0)
		return -1;
	if (count == 0) {
		reader->eof = 1;

		return 0;
	}

	reader->position = 1;
	reader->length = (size_t)count;
	*value = reader->buffer[0];

	return 1;
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
		if (status < 0)
			return -1;
		if (status == 0)
			return 0;
		count--;
	}

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

	if (options->silent)
		return 0;
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
	if (status < 0)
		return -1;

	return 0;
}

/* Reports an unequal-length comparison unless silent mode was selected. */
static int
cmp_report_eof(
	const struct cmp_options *options,
	const char *name)
{
	int status;

	if (options->silent)
		return 0;

	status = fprintf(stderr, "cmp: EOF on %s\n", name);
	if (status < 0)
		return -1;

	return 0;
}
