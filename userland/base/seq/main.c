/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Prints a sequence of numbers (as GNU seq does; seq is not in POSIX).
 *
 *	seq [-w] [-s separator] [-f format] [first [increment]] last
 *
 * first and increment are 1 by default.  The numbers are written with as
 * many decimals as the operand with the most, -w pads them with zeros to
 * the same width, -f gives a printf format for a double, and -s puts a
 * separator between them instead of a newline (a newline still ends the
 * output).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The options. */
struct options {
	int equal_width;
	const char *separator;
	const char *format;
};

static int read_options(int argc, char **argv, struct options *options);
static double parse_number(const char *text, int *decimals);
static void usage(void);

/*
 * Runs seq.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	char format[64];
	char text[128];
	double first;
	double increment;
	double last;
	double value;
	double tolerance;
	long long step;
	int decimals;
	int most;
	int width;
	int length;
	int count;
	int index;

	/* The options, then one to three numbers. */
	memset(&options, 0, sizeof(options));
	options.separator = "\n";
	index = read_options(argc, argv, &options);
	count = argc - index;
	if (count < 1 || count > 3)
		usage();

	/* The numbers, and the most decimals among them. */
	first = 1;
	increment = 1;
	most = 0;
	last = parse_number(argv[argc - 1], &decimals);
	if (decimals > most)
		most = decimals;
	if (count >= 2) {
		first = parse_number(argv[index], &decimals);
		if (decimals > most)
			most = decimals;
	}

	/* The increment, with three numbers. */
	if (count == 3) {
		increment = parse_number(argv[index + 1], &decimals);
		if (decimals > most)
			most = decimals;
	}

	/* An increment of zero never ends. */
	if (increment == 0) {
		fprintf(stderr, "seq: invalid Zero increment value: '%s'\n", argv[index + 1]);
		return 1;
	}

	/* The format: -f, or the decimals, padded with -w to the widest end. */
	if (options.format != NULL) {
		snprintf(format, sizeof(format), "%s", options.format);
	} else if (options.equal_width) {
		width = snprintf(text, sizeof(text), "%.*f", most, first);
		length = snprintf(text, sizeof(text), "%.*f", most, last);
		if (length > width)
			width = length;
		snprintf(format, sizeof(format), "%%0%d.%df", width, most);
	} else {
		snprintf(format, sizeof(format), "%%.%df", most);
	}

	/* A small tolerance, so that a decimal step reaches the last number. */
	tolerance = increment;
	if (tolerance < 0)
		tolerance = -tolerance;
	tolerance *= 1e-10;

	/* Each number, counted in steps so that the sum does not drift. */
	for (step = 0;; step++) {
		value = first + (double)step * increment;
		if (increment > 0 && value > last + tolerance)
			break;
		if (increment < 0 && value < last - tolerance)
			break;
		if (step > 0)
			fputs(options.separator, stdout);
		printf(format, value);
	}

	/* Succeeded: a newline ends a non-empty output. */
	if (step > 0)
		putchar('\n');
	return 0;
}

/* Reads the options; returns the index of the first number. */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	const char *word;
	int index;

	/* Each option word; a negative number is not an option. */
	for (index = 1; index < argc; index++) {
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;
		if ((word[1] >= '0' && word[1] <= '9') || word[1] == '.')
			break;
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* -w, and -s and -f with their arguments. */
		if (word[1] == 'w' && word[2] == '\0') {
			options->equal_width = 1;
			continue;
		}

		/* Only -s and -f take an argument. */
		if (word[1] != 's' && word[1] != 'f')
			usage();
		if (word[2] != '\0') {
			if (word[1] == 's')
				options->separator = word + 2;
			else
				options->format = word + 2;
			continue;
		}

		/* The argument is the next word. */
		if (index + 1 >= argc)
			usage();
		index++;
		if (word[1] == 's')
			options->separator = argv[index];
		else
			options->format = argv[index];
	}

	/* Succeeded: the first number. */
	return index;
}

/* Reads a number and counts the decimals written after its point. */
static double
parse_number(
	const char *text,
	int *decimals)
{
	const char *point;
	const char *cursor;
	char *end;
	double value;

	/* The number, which must be the whole operand. */
	value = strtod(text, &end);
	if (end == text || *end != '\0') {
		fprintf(stderr, "seq: invalid floating point argument: '%s'\n", text);
		exit(1);
	}

	/* The digits after the point, up to an exponent. */
	*decimals = 0;
	point = strchr(text, '.');
	if (point == NULL)
		return value;
	for (cursor = point + 1; *cursor >= '0' && *cursor <= '9'; cursor++)
		(*decimals)++;

	/* Succeeded. */
	return value;
}

/* Reports the usage and ends seq. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: seq [-w] [-s separator] [-f format] [first [increment]] last\n");
	exit(1);
}
