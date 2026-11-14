/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of svg-values: numbers, lengths and their resolution,
 * lists, the transform attribute, viewBox, preserveAspectRatio and the
 * transform of a viewBox, url references and paints.  Every expected value
 * is a hand calculation or a value of the standard, never the output of
 * another implementation.  The last group feeds random text, each string
 * in a block of exactly its size, so that the sanitizers see a read past
 * the NUL.
 */

#include "check.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "svg-test-tree.h"
#include "svg/svg-values.h"

/* The ratio of a circle's circumference to its diameter, in double precision, for the expected values. */
#define TEST_PI	3.14159265358979323846

/* A value no parser writes, to see that a failed parse leaves its output alone. */
#define TEST_SENTINEL	(-777.5f)

/* Copies text into a block of exactly its size (with the NUL), so that a read past the end is caught. */
static char *
exact_copy(
	const char *text)
{
	char *copy;
	size_t size;

	size = strlen(text) + 1;
	copy = malloc(size);
	if (copy == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	memcpy(copy, text, size);
	return copy;
}

/* Reads one number at the start of text, and checks the value, the error and how far the cursor moved. */
static void
check_number_read(
	const char *text,
	int expected_error,
	double expected_value,
	long expected_consumed,
	int line)
{
	char *copy;
	const char *cursor;
	float value;
	int error;
	double tolerance;

	copy = exact_copy(text);
	cursor = copy;
	value = TEST_SENTINEL;
	error = svg_number_read(&cursor, &value);
	check_record_long(error, expected_error, __FILE__, line, text);
	if (expected_error == 0) {
		tolerance = fabs(expected_value) * 1e-6;
		check_record_near(value, expected_value, tolerance, __FILE__, line, text);
		check_record_long((long)(cursor - copy), expected_consumed, __FILE__, line, text);
	} else {
		/* A failure leaves the cursor and the value alone. */
		check_record(cursor == copy, __FILE__, line, "cursor unchanged");
		check_record(value == TEST_SENTINEL, __FILE__, line, "value unchanged");
	}
	free(copy);
}

#define NUMBER_OK(text, value, consumed)	check_number_read(text, 0, value, consumed, __LINE__)
#define NUMBER_BAD(text)			check_number_read(text, EINVAL, 0.0, 0, __LINE__)

/* Checks svg_number_read on signs, fractions, exponents, leftovers, overflow and underflow. */
static void
test_number_read(void)
{
	char *copy;
	const char *cursor;
	float value;

	/* Plain numbers and signs. */
	NUMBER_OK("0", 0.0, 1);
	NUMBER_OK("7", 7.0, 1);
	NUMBER_OK("+7", 7.0, 2);
	NUMBER_OK("-7", -7.0, 2);
	NUMBER_OK("0012", 12.0, 4);
	NUMBER_OK("123456", 123456.0, 6);

	/* Fractions, with and without an integer part. */
	NUMBER_OK("1.5", 1.5, 3);
	NUMBER_OK(".5", 0.5, 2);
	NUMBER_OK("-.5", -0.5, 3);
	NUMBER_OK("+.25", 0.25, 4);
	NUMBER_OK("0.1", 0.1, 3);
	NUMBER_OK("0.05", 0.05, 4);
	NUMBER_OK("0.000001", 1e-6, 8);
	NUMBER_OK("100.0", 100.0, 5);

	/* A point with no digit after it is not part of the number (the fractional-constant needs a digit). */
	NUMBER_OK("5.", 5.0, 1);
	NUMBER_OK("5.e3", 5.0, 1);
	NUMBER_OK("1.5.5", 1.5, 3);

	/* Exponents: e or E, an optional sign, at least one digit. */
	NUMBER_OK("1e3", 1000.0, 3);
	NUMBER_OK("1E3", 1000.0, 3);
	NUMBER_OK("1e+3", 1000.0, 4);
	NUMBER_OK("1e-3", 0.001, 4);
	NUMBER_OK("2.5e2", 250.0, 5);
	NUMBER_OK("-1.5e-1", -0.15, 7);
	NUMBER_OK(".5e1", 5.0, 4);
	NUMBER_OK("1e0", 1.0, 3);
	NUMBER_OK("12e01", 120.0, 5);

	/* An exponent with no digit is not read: the number ends before the e. */
	NUMBER_OK("1e", 1.0, 1);
	NUMBER_OK("1e+", 1.0, 1);
	NUMBER_OK("1e-", 1.0, 1);
	NUMBER_OK("1em", 1.0, 1);
	NUMBER_OK("1ex", 1.0, 1);
	NUMBER_OK("1e-x", 1.0, 1);

	/* What follows a number stays unread. */
	NUMBER_OK("12abc", 12.0, 2);
	NUMBER_OK("3px", 3.0, 1);
	NUMBER_OK("3 4", 3.0, 1);
	NUMBER_OK("3,4", 3.0, 1);
	NUMBER_OK("3-4", 3.0, 1);
	NUMBER_OK("3+4", 3.0, 1);

	/* Digits beyond the precision of a float only move the point. */
	NUMBER_OK("123456789012345678901234567890", 1.2345678901234568e29, 30);
	NUMBER_OK("0.12345678901234567890123456789", 0.12345678901234568, 31);
	NUMBER_OK("100000000000000000000000", 1e23, 24);
	NUMBER_OK("0.00000000000000000000001", 1e-23, 25);
	NUMBER_OK("1000000000000000000000000000000000000000000000000000000000000000000000e-69", 1.0, 74);

	/* Large and small values near the limits of a float. */
	NUMBER_OK("3.4e38", 3.4e38, 6);
	NUMBER_OK("3.4028234e38", 3.4028234e38, 12);
	NUMBER_OK("1e-37", 1e-37, 5);
	NUMBER_OK("1e-45", 1.4012984643e-45, 5);

	/* Under half the smallest denormal, a value is zero. */
	NUMBER_OK("1e-50", 0.0, 5);
	NUMBER_OK("1e-400", 0.0, 6);
	NUMBER_OK("1e-99999", 0.0, 8);
	NUMBER_OK("0e99999", 0.0, 7);
	NUMBER_OK("0.0e9999999999999999999999", 0.0, 26);
	NUMBER_OK("-0", 0.0, 2);

	/* A value past the largest float is an error, however far. */
	NUMBER_BAD("1e39");
	NUMBER_BAD("-1e39");
	NUMBER_BAD("4e38");
	NUMBER_BAD("1e400");
	NUMBER_BAD("1e99999");
	NUMBER_BAD("1e99999999999999999999");
	NUMBER_BAD("1000000000000000000000000000000000000000");
	NUMBER_BAD("0.00000000000000000000001e62");

	/* Text that starts no number. */
	NUMBER_BAD("");
	NUMBER_BAD("abc");
	NUMBER_BAD("+");
	NUMBER_BAD("-");
	NUMBER_BAD(".");
	NUMBER_BAD("-.");
	NUMBER_BAD("e5");
	NUMBER_BAD(".e5");
	NUMBER_BAD("+-5");
	NUMBER_BAD("--5");
	NUMBER_BAD(" 5");
	NUMBER_BAD(",5");

	/* A NULL argument is an error, not a crash. */
	copy = exact_copy("5");
	cursor = copy;
	value = TEST_SENTINEL;
	CHECK_LONG(svg_number_read(NULL, &value), EINVAL);
	CHECK_LONG(svg_number_read(&cursor, NULL), EINVAL);
	free(copy);
}

/* Checks svg_parse_number: white space around, nothing else. */
static void
test_parse_number(void)
{
	float value;

	value = TEST_SENTINEL;
	CHECK_LONG(svg_parse_number("42", &value), 0);
	CHECK_NEAR(value, 42.0, 0.0);
	CHECK_LONG(svg_parse_number(" \t\r\n\f-1.5e1 \n", &value), 0);
	CHECK_NEAR(value, -15.0, 0.0);

	/* Anything else, and an empty text, is invalid and leaves the value. */
	value = TEST_SENTINEL;
	CHECK_LONG(svg_parse_number("", &value), EINVAL);
	CHECK_LONG(svg_parse_number("   ", &value), EINVAL);
	CHECK_LONG(svg_parse_number("5 6", &value), EINVAL);
	CHECK_LONG(svg_parse_number("5.", &value), EINVAL);
	CHECK_LONG(svg_parse_number("5px", &value), EINVAL);
	CHECK_LONG(svg_parse_number("5,", &value), EINVAL);
	CHECK_LONG(svg_parse_number("1e39", &value), EINVAL);
	CHECK_LONG(svg_parse_number(NULL, &value), EINVAL);
	CHECK_LONG(svg_parse_number("5", NULL), EINVAL);
	CHECK_NEAR(value, TEST_SENTINEL, 0.0);
}

/* Parses a length and checks its number and unit. */
static void
check_length(
	const char *text,
	double value,
	enum svg_unit unit,
	int line)
{
	char *copy;
	struct svg_length length;
	int error;

	copy = exact_copy(text);
	length.value = TEST_SENTINEL;
	length.unit = SVG_UNIT_NUMBER;
	error = svg_parse_length(copy, &length);
	check_record_long(error, 0, __FILE__, line, text);
	check_record_near(length.value, value, fabs(value) * 1e-6, __FILE__, line, text);
	check_record_long(length.unit, unit, __FILE__, line, text);
	free(copy);
}

/* Checks that a length is refused and the output is left alone. */
static void
check_length_bad(
	const char *text,
	int line)
{
	char *copy;
	struct svg_length length;
	int error;

	copy = exact_copy(text);
	length.value = TEST_SENTINEL;
	length.unit = SVG_UNIT_PC;
	error = svg_parse_length(copy, &length);
	check_record_long(error, EINVAL, __FILE__, line, text);
	check_record_near(length.value, TEST_SENTINEL, 0.0, __FILE__, line, text);
	check_record_long(length.unit, SVG_UNIT_PC, __FILE__, line, text);
	free(copy);
}

#define LENGTH_OK(text, value, unit)	check_length(text, value, unit, __LINE__)
#define LENGTH_BAD(text)		check_length_bad(text, __LINE__)

/* Checks svg_parse_length on every unit, in any case, and on the refusals. */
static void
test_length(void)
{
	/* A bare number is in user units. */
	LENGTH_OK("10", 10.0, SVG_UNIT_NUMBER);
	LENGTH_OK("-2.5", -2.5, SVG_UNIT_NUMBER);
	LENGTH_OK("  7  ", 7.0, SVG_UNIT_NUMBER);

	/* Each unit, and the same in other cases (units are ASCII case-insensitive). */
	LENGTH_OK("10px", 10.0, SVG_UNIT_PX);
	LENGTH_OK("10PX", 10.0, SVG_UNIT_PX);
	LENGTH_OK("10Px", 10.0, SVG_UNIT_PX);
	LENGTH_OK("50%", 50.0, SVG_UNIT_PERCENT);
	LENGTH_OK("1.5em", 1.5, SVG_UNIT_EM);
	LENGTH_OK("1.5EM", 1.5, SVG_UNIT_EM);
	LENGTH_OK("2ex", 2.0, SVG_UNIT_EX);
	LENGTH_OK("2EX", 2.0, SVG_UNIT_EX);
	LENGTH_OK("3rem", 3.0, SVG_UNIT_REM);
	LENGTH_OK("3REM", 3.0, SVG_UNIT_REM);
	LENGTH_OK("1in", 1.0, SVG_UNIT_IN);
	LENGTH_OK("1IN", 1.0, SVG_UNIT_IN);
	LENGTH_OK("2.54cm", 2.54, SVG_UNIT_CM);
	LENGTH_OK("2.54Cm", 2.54, SVG_UNIT_CM);
	LENGTH_OK("10mm", 10.0, SVG_UNIT_MM);
	LENGTH_OK("10MM", 10.0, SVG_UNIT_MM);
	LENGTH_OK("4Q", 4.0, SVG_UNIT_Q);
	LENGTH_OK("4q", 4.0, SVG_UNIT_Q);
	LENGTH_OK("12pt", 12.0, SVG_UNIT_PT);
	LENGTH_OK("12PT", 12.0, SVG_UNIT_PT);
	LENGTH_OK("1pc", 1.0, SVG_UNIT_PC);
	LENGTH_OK("1PC", 1.0, SVG_UNIT_PC);

	/* An exponent and a unit that starts with e: 1e3 is a number, 1em is a length, 1e3em both. */
	LENGTH_OK("1e3", 1000.0, SVG_UNIT_NUMBER);
	LENGTH_OK("1e3em", 1000.0, SVG_UNIT_EM);
	LENGTH_OK("1e-1ex", 0.1, SVG_UNIT_EX);

	/* Unknown units, text after the length, and no number. */
	LENGTH_BAD("10foo");
	LENGTH_BAD("10px5");
	LENGTH_BAD("10px-5");
	LENGTH_BAD("10 px");
	LENGTH_BAD("10%%");
	LENGTH_BAD("10%x");
	LENGTH_BAD("px");
	LENGTH_BAD("%");
	LENGTH_BAD("10e");
	LENGTH_BAD("1ems");
	LENGTH_BAD("5deg");
	LENGTH_BAD("5p");
	LENGTH_BAD("5abcdefghijklmnopqrstuvwxyz");
	LENGTH_BAD("5 6");
	LENGTH_BAD("5px,");
	LENGTH_BAD("");
	LENGTH_BAD("   ");
	LENGTH_BAD("1e39px");
}

/* Resolves a length and checks the result to a relative 1e-5. */
static void
check_resolve(
	const char *text,
	enum svg_axis axis,
	const struct svg_context *context,
	double expected,
	int line)
{
	struct svg_length length;
	float value;

	if (svg_parse_length(text, &length) != 0) {
		check_record(0, __FILE__, line, "length parses");
		return;
	}
	value = svg_length_resolve(&length, axis, context);
	check_record_near(value, expected, fabs(expected) * 1e-5, __FILE__, line, text);
}

#define RESOLVE(text, axis, expected)	check_resolve(text, axis, &context, expected, __LINE__)

/* Checks svg_length_resolve on every unit and the three axes of a percentage. */
static void
test_length_resolve(void)
{
	struct svg_context context;
	struct svg_length length;
	float value;

	/* A viewport 200 by 100, font size 16, root font size 10. */
	context.viewport_width = 200.0f;
	context.viewport_height = 100.0f;
	context.font_size = 16.0f;
	context.root_font_size = 10.0f;

	/* Absolute units: 96 px to the inch, 2.54 cm to the inch, 25.4 mm, 101.6 quarter-millimetres, 72 pt, 6 pc. */
	RESOLVE("10", SVG_AXIS_X, 10.0);
	RESOLVE("10px", SVG_AXIS_X, 10.0);
	RESOLVE("1in", SVG_AXIS_X, 96.0);
	RESOLVE("2.54cm", SVG_AXIS_X, 96.0);
	RESOLVE("25.4mm", SVG_AXIS_X, 96.0);
	RESOLVE("101.6Q", SVG_AXIS_X, 96.0);
	RESOLVE("72pt", SVG_AXIS_X, 96.0);
	RESOLVE("6pc", SVG_AXIS_X, 96.0);
	RESOLVE("1pc", SVG_AXIS_X, 16.0);
	RESOLVE("1pt", SVG_AXIS_X, 96.0 / 72.0);
	RESOLVE("-1in", SVG_AXIS_X, -96.0);

	/* Font relative units: em is the font size, ex half of it, rem the root's. */
	RESOLVE("2em", SVG_AXIS_X, 32.0);
	RESOLVE("2ex", SVG_AXIS_X, 16.0);
	RESOLVE("3rem", SVG_AXIS_X, 30.0);

	/* A percentage: of the width, of the height, of the normalized diagonal sqrt((200^2 + 100^2) / 2) = sqrt(25000). */
	RESOLVE("50%", SVG_AXIS_X, 100.0);
	RESOLVE("50%", SVG_AXIS_Y, 50.0);
	RESOLVE("50%", SVG_AXIS_OTHER, 0.5 * sqrt(25000.0));
	RESOLVE("100%", SVG_AXIS_OTHER, 158.11388300841898);
	RESOLVE("0%", SVG_AXIS_X, 0.0);

	/* A square viewport has the same normalized diagonal as its side. */
	context.viewport_width = 80.0f;
	context.viewport_height = 80.0f;
	RESOLVE("100%", SVG_AXIS_OTHER, 80.0);

	/* A result a float cannot hold, a missing context and an unknown unit give 0. */
	length.value = 3e38f;
	length.unit = SVG_UNIT_IN;
	value = svg_length_resolve(&length, SVG_AXIS_X, &context);
	CHECK_NEAR(value, 0.0, 0.0);
	length.value = 1.0f;
	length.unit = SVG_UNIT_PX;
	CHECK_NEAR(svg_length_resolve(&length, SVG_AXIS_X, NULL), 0.0, 0.0);
	CHECK_NEAR(svg_length_resolve(NULL, SVG_AXIS_X, &context), 0.0, 0.0);
	length.unit = (enum svg_unit)99;
	CHECK_NEAR(svg_length_resolve(&length, SVG_AXIS_X, &context), 0.0, 0.0);

	/* A context that is not finite gives 0 rather than a NaN. */
	context.font_size = nanf("");
	length.value = 2.0f;
	length.unit = SVG_UNIT_EM;
	CHECK_NEAR(svg_length_resolve(&length, SVG_AXIS_X, &context), 0.0, 0.0);
}

/* Checks svg_parse_numbers on separators, white space, errors and capacity. */
static void
test_numbers(void)
{
	float values[8];
	float *big;
	char *text;
	size_t count;
	size_t index;
	size_t length;
	int error;

	/* Separators: white space, one comma with white space around, a mix. */
	count = 99;
	CHECK_LONG(svg_parse_numbers("1 2 3", values, 8, &count), 0);
	CHECK_LONG(count, 3);
	CHECK_NEAR(values[0], 1.0, 0.0);
	CHECK_NEAR(values[2], 3.0, 0.0);
	CHECK_LONG(svg_parse_numbers("1,2,3", values, 8, &count), 0);
	CHECK_LONG(count, 3);
	CHECK_LONG(svg_parse_numbers("1, 2 ,3 , 4", values, 8, &count), 0);
	CHECK_LONG(count, 4);
	CHECK_NEAR(values[3], 4.0, 0.0);
	CHECK_LONG(svg_parse_numbers(" \t1 \n\r\f2\t ", values, 8, &count), 0);
	CHECK_LONG(count, 2);
	CHECK_LONG(svg_parse_numbers("1.5e1,-2.5", values, 8, &count), 0);
	CHECK_LONG(count, 2);
	CHECK_NEAR(values[0], 15.0, 0.0);
	CHECK_NEAR(values[1], -2.5, 0.0);

	/* An empty or all-white list has no numbers and is valid. */
	count = 99;
	CHECK_LONG(svg_parse_numbers("", values, 8, &count), 0);
	CHECK_LONG(count, 0);
	count = 99;
	CHECK_LONG(svg_parse_numbers(" \t\n ", values, 8, &count), 0);
	CHECK_LONG(count, 0);

	/* A leading comma is malformed with nothing read. */
	CHECK_LONG(svg_parse_numbers(",1", values, 8, &count), EINVAL);
	CHECK_LONG(count, 0);

	/* A trailing comma is malformed after the numbers before it. */
	CHECK_LONG(svg_parse_numbers("1,", values, 8, &count), EINVAL);
	CHECK_LONG(count, 1);
	CHECK_LONG(svg_parse_numbers("1 2 , ", values, 8, &count), EINVAL);
	CHECK_LONG(count, 2);

	/* Two commas are malformed. */
	CHECK_LONG(svg_parse_numbers("1,,2", values, 8, &count), EINVAL);
	CHECK_LONG(count, 1);
	CHECK_LONG(svg_parse_numbers("1 , , 2", values, 8, &count), EINVAL);
	CHECK_LONG(count, 1);

	/* A mistake in the middle keeps the numbers read before it. */
	CHECK_LONG(svg_parse_numbers("1 2 x 4", values, 8, &count), EINVAL);
	CHECK_LONG(count, 2);
	CHECK_NEAR(values[0], 1.0, 0.0);
	CHECK_NEAR(values[1], 2.0, 0.0);
	CHECK_LONG(svg_parse_numbers("1 2 3px", values, 8, &count), EINVAL);
	CHECK_LONG(count, 3);

	/* Two numbers with nothing between them need a separator (the comma-wsp of the grammar). */
	CHECK_LONG(svg_parse_numbers("1-2", values, 8, &count), EINVAL);
	CHECK_LONG(count, 1);
	CHECK_LONG(svg_parse_numbers("1.5.5", values, 8, &count), EINVAL);
	CHECK_LONG(count, 1);
	CHECK_LONG(svg_parse_numbers("1e 2", values, 8, &count), EINVAL);
	CHECK_LONG(count, 1);

	/* A number that overflows a float ends the list as malformed. */
	CHECK_LONG(svg_parse_numbers("1 1e39", values, 8, &count), EINVAL);
	CHECK_LONG(count, 1);

	/* More numbers than the capacity: ENOBUFS with the first ones kept. */
	CHECK_LONG(svg_parse_numbers("1 2 3", values, 2, &count), ENOBUFS);
	CHECK_LONG(count, 2);
	CHECK_NEAR(values[1], 2.0, 0.0);
	CHECK_LONG(svg_parse_numbers("1 2", values, 2, &count), 0);
	CHECK_LONG(count, 2);

	/* A capacity of 0 holds the empty list only (values may be NULL then). */
	CHECK_LONG(svg_parse_numbers("", NULL, 0, &count), 0);
	CHECK_LONG(count, 0);
	CHECK_LONG(svg_parse_numbers("7", NULL, 0, &count), ENOBUFS);
	CHECK_LONG(count, 0);

	/* NULL arguments. */
	CHECK_LONG(svg_parse_numbers(NULL, values, 8, &count), EINVAL);
	CHECK_LONG(svg_parse_numbers("1", values, 8, NULL), EINVAL);

	/* The largest list: SVG_NUMBERS_MAX numbers fit, one more does not, even with a larger capacity. */
	length = (SVG_NUMBERS_MAX + 1) * 2;
	text = malloc(length + 1);
	big = malloc((SVG_NUMBERS_MAX + 16) * sizeof(*big));
	if (text == NULL || big == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	for (index = 0; index < SVG_NUMBERS_MAX + 1; index++) {
		text[index * 2] = '1';
		text[index * 2 + 1] = ' ';
	}
	text[length] = '\0';
	error = svg_parse_numbers(text, big, SVG_NUMBERS_MAX + 16, &count);
	CHECK_LONG(error, ENOBUFS);
	CHECK_LONG(count, SVG_NUMBERS_MAX);
	text[SVG_NUMBERS_MAX * 2] = '\0';
	error = svg_parse_numbers(text, big, SVG_NUMBERS_MAX + 16, &count);
	CHECK_LONG(error, 0);
	CHECK_LONG(count, SVG_NUMBERS_MAX);
	free(text);
	free(big);
}

/* Checks svg_parse_lengths: the same separators, units, the keyword none, and what a mistake keeps. */
static void
test_lengths(void)
{
	struct svg_length values[8];
	size_t count;

	/* Lengths with units. */
	count = 99;
	CHECK_LONG(svg_parse_lengths("10px 5% 2em", values, 8, &count), 0);
	CHECK_LONG(count, 3);
	CHECK_NEAR(values[0].value, 10.0, 0.0);
	CHECK_LONG(values[0].unit, SVG_UNIT_PX);
	CHECK_NEAR(values[1].value, 5.0, 0.0);
	CHECK_LONG(values[1].unit, SVG_UNIT_PERCENT);
	CHECK_LONG(values[2].unit, SVG_UNIT_EM);
	CHECK_LONG(svg_parse_lengths("10px,5,1IN", values, 8, &count), 0);
	CHECK_LONG(count, 3);
	CHECK_LONG(values[1].unit, SVG_UNIT_NUMBER);
	CHECK_LONG(values[2].unit, SVG_UNIT_IN);
	CHECK_LONG(svg_parse_lengths(" 4 , 8 ", values, 8, &count), 0);
	CHECK_LONG(count, 2);

	/* The keyword none is the empty list, in any case. */
	count = 99;
	CHECK_LONG(svg_parse_lengths("none", values, 8, &count), 0);
	CHECK_LONG(count, 0);
	count = 99;
	CHECK_LONG(svg_parse_lengths(" NONE ", values, 8, &count), 0);
	CHECK_LONG(count, 0);
	count = 99;
	CHECK_LONG(svg_parse_lengths("", values, 8, &count), 0);
	CHECK_LONG(count, 0);

	/* A malformed list keeps nothing. */
	CHECK_LONG(svg_parse_lengths("none 5", values, 8, &count), EINVAL);
	CHECK_LONG(count, 0);
	CHECK_LONG(svg_parse_lengths("5 none", values, 8, &count), EINVAL);
	CHECK_LONG(count, 0);
	CHECK_LONG(svg_parse_lengths("5 6foo", values, 8, &count), EINVAL);
	CHECK_LONG(count, 0);
	CHECK_LONG(svg_parse_lengths("5,", values, 8, &count), EINVAL);
	CHECK_LONG(count, 0);
	CHECK_LONG(svg_parse_lengths(",5", values, 8, &count), EINVAL);
	CHECK_LONG(count, 0);
	CHECK_LONG(svg_parse_lengths("5px6", values, 8, &count), EINVAL);
	CHECK_LONG(count, 0);
	CHECK_LONG(svg_parse_lengths("nonex", values, 8, &count), EINVAL);
	CHECK_LONG(count, 0);

	/* Too many lengths: ENOBUFS, with the ones that fit kept. */
	CHECK_LONG(svg_parse_lengths("1 2 3", values, 2, &count), ENOBUFS);
	CHECK_LONG(count, 2);
	CHECK_NEAR(values[1].value, 2.0, 0.0);
	CHECK_LONG(svg_parse_lengths("", NULL, 0, &count), 0);
	CHECK_LONG(count, 0);
	CHECK_LONG(svg_parse_lengths("none", NULL, 0, &count), 0);
	CHECK_LONG(count, 0);

	/* NULL arguments. */
	CHECK_LONG(svg_parse_lengths(NULL, values, 8, &count), EINVAL);
	CHECK_LONG(svg_parse_lengths("1", values, 8, NULL), EINVAL);
}

/* Builds a matrix from its six entries. */
static struct vec_matrix
make_matrix(
	float a,
	float b,
	float c,
	float d,
	float e,
	float f)
{
	struct vec_matrix matrix;

	matrix.a = a;
	matrix.b = b;
	matrix.c = c;
	matrix.d = d;
	matrix.e = e;
	matrix.f = f;
	return matrix;
}

/* Parses a transform and checks the six entries of the matrix. */
static void
check_transform(
	const char *text,
	float a,
	float b,
	float c,
	float d,
	float e,
	float f,
	double tolerance,
	int line)
{
	char *copy;
	struct vec_matrix matrix;
	int error;

	copy = exact_copy(text);
	matrix = make_matrix(TEST_SENTINEL, TEST_SENTINEL, TEST_SENTINEL, TEST_SENTINEL, TEST_SENTINEL, TEST_SENTINEL);
	error = svg_parse_transform(copy, &matrix);
	check_record_long(error, 0, __FILE__, line, text);
	check_record_near(matrix.a, a, tolerance, __FILE__, line, text);
	check_record_near(matrix.b, b, tolerance, __FILE__, line, text);
	check_record_near(matrix.c, c, tolerance, __FILE__, line, text);
	check_record_near(matrix.d, d, tolerance, __FILE__, line, text);
	check_record_near(matrix.e, e, tolerance, __FILE__, line, text);
	check_record_near(matrix.f, f, tolerance, __FILE__, line, text);
	free(copy);
}

/* Checks that a transform is refused with an error and the matrix is left alone. */
static void
check_transform_bad(
	const char *text,
	int expected_error,
	int line)
{
	char *copy;
	struct vec_matrix matrix;
	int error;

	copy = exact_copy(text);
	matrix = make_matrix(TEST_SENTINEL, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f);
	error = svg_parse_transform(copy, &matrix);
	check_record_long(error, expected_error, __FILE__, line, text);
	check_record(matrix.a == TEST_SENTINEL && matrix.b == 2.0f && matrix.c == 3.0f && matrix.d == 4.0f &&
	    matrix.e == 5.0f && matrix.f == 6.0f, __FILE__, line, "matrix unchanged");
	free(copy);
}

#define TRANSFORM_OK(text, a, b, c, d, e, f, tolerance)	check_transform(text, a, b, c, d, e, f, tolerance, __LINE__)
#define TRANSFORM_BAD(text)				check_transform_bad(text, EINVAL, __LINE__)

/* Applies a transform text to a point and checks the result. */
static void
check_transform_point(
	const char *text,
	float x,
	float y,
	double expected_x,
	double expected_y,
	int line)
{
	struct vec_matrix matrix;
	struct vec_point point;

	if (svg_parse_transform(text, &matrix) != 0) {
		check_record(0, __FILE__, line, "transform parses");
		return;
	}
	point.x = x;
	point.y = y;
	point = vec_matrix_apply(&matrix, point);
	check_record_near(point.x, expected_x, 1e-4, __FILE__, line, text);
	check_record_near(point.y, expected_y, 1e-4, __FILE__, line, text);
}

/* Checks svg_parse_transform on every function, the order of composition, units and refusals. */
static void
test_transform(void)
{
	struct vec_matrix matrix;
	struct vec_matrix before;
	char *text;
	size_t index;
	size_t length;
	size_t count;
	const char *piece = "scale(1) ";

	/* An empty attribute is the identity (and replaces what was there). */
	TRANSFORM_OK("", 1, 0, 0, 1, 0, 0, 0.0);
	TRANSFORM_OK("  \t\n ", 1, 0, 0, 1, 0, 0, 0.0);

	/* matrix(a b c d e f), with spaces or commas. */
	TRANSFORM_OK("matrix(1 2 3 4 5 6)", 1, 2, 3, 4, 5, 6, 0.0);
	TRANSFORM_OK("matrix(1,2,3,4,5,6)", 1, 2, 3, 4, 5, 6, 0.0);
	TRANSFORM_OK("matrix( 1 , 2 3,4 5 , 6 )", 1, 2, 3, 4, 5, 6, 0.0);
	TRANSFORM_OK("matrix(-1.5 0 0 .5 1e1 -2e-1)", -1.5, 0, 0, 0.5, 10, -0.2, 1e-7);
	TRANSFORM_OK("  matrix (1 0 0 1 0 0)  ", 1, 0, 0, 1, 0, 0, 0.0);

	/* The grammar's separator between numbers is optional, so a sign can start the next. */
	TRANSFORM_OK("matrix(1-2-3-4-5-6)", 1, -2, -3, -4, -5, -6, 0.0);
	TRANSFORM_OK("translate(1-2)", 1, 0, 0, 1, 1, -2, 0.0);

	/* translate(tx [ty]): one argument gives ty = 0. */
	TRANSFORM_OK("translate(10)", 1, 0, 0, 1, 10, 0, 0.0);
	TRANSFORM_OK("translate(10,20)", 1, 0, 0, 1, 10, 20, 0.0);
	TRANSFORM_OK("translate(10 20)", 1, 0, 0, 1, 10, 20, 0.0);
	TRANSFORM_OK("translate( 10 , 20 )", 1, 0, 0, 1, 10, 20, 0.0);
	TRANSFORM_OK("translate (10)", 1, 0, 0, 1, 10, 0, 0.0);
	TRANSFORM_OK("translate(-3.5 .25)", 1, 0, 0, 1, -3.5, 0.25, 0.0);

	/* The attribute is a presentation attribute of the transform property: a length may carry px. */
	TRANSFORM_OK("translate(10px 5)", 1, 0, 0, 1, 10, 5, 0.0);
	TRANSFORM_OK("translate(10PX)", 1, 0, 0, 1, 10, 0, 0.0);
	TRANSFORM_OK("translate(1 5Px)", 1, 0, 0, 1, 1, 5, 0.0);

	/* scale(sx [sy]): one argument gives sy = sx. */
	TRANSFORM_OK("scale(2)", 2, 0, 0, 2, 0, 0, 0.0);
	TRANSFORM_OK("scale(2 3)", 2, 0, 0, 3, 0, 0, 0.0);
	TRANSFORM_OK("scale(-1,0.5)", -1, 0, 0, 0.5, 0, 0, 0.0);

	/* rotate(a): positive turns from x towards y. */
	TRANSFORM_OK("rotate(90)", 0, 1, -1, 0, 0, 0, 1e-6);
	TRANSFORM_OK("rotate(0)", 1, 0, 0, 1, 0, 0, 0.0);
	TRANSFORM_OK("rotate(-90)", 0, -1, 1, 0, 0, 0, 1e-6);
	TRANSFORM_OK("rotate(45)", sqrt(0.5), sqrt(0.5), -sqrt(0.5), sqrt(0.5), 0, 0, 1e-5);

	/* rotate(a cx cy) is translate(cx cy) rotate(a) translate(-cx -cy): the point (cx, cy) stays. */
	check_transform_point("rotate(90 10 0)", 11.0f, 0.0f, 10.0, 1.0, __LINE__);
	check_transform_point("rotate(90 10 0)", 10.0f, 0.0f, 10.0, 0.0, __LINE__);
	check_transform_point("rotate(90, 10, 20)", 10.0f, 20.0f, 10.0, 20.0, __LINE__);
	check_transform_point("rotate(180 5 5)", 0.0f, 0.0f, 10.0, 10.0, __LINE__);
	check_transform_point("rotate(90 10px 0px)", 11.0f, 0.0f, 10.0, 1.0, __LINE__);

	/* Angles may carry a unit: 45deg = 0.125turn = 50grad = pi/4 rad. */
	TRANSFORM_OK("rotate(45deg)", sqrt(0.5), sqrt(0.5), -sqrt(0.5), sqrt(0.5), 0, 0, 1e-5);
	TRANSFORM_OK("rotate(0.125turn)", sqrt(0.5), sqrt(0.5), -sqrt(0.5), sqrt(0.5), 0, 0, 1e-5);
	TRANSFORM_OK("rotate(50grad)", sqrt(0.5), sqrt(0.5), -sqrt(0.5), sqrt(0.5), 0, 0, 1e-5);
	TRANSFORM_OK("rotate(0.785398rad)", sqrt(0.5), sqrt(0.5), -sqrt(0.5), sqrt(0.5), 0, 0, 1e-5);
	TRANSFORM_OK("rotate(45DEG)", sqrt(0.5), sqrt(0.5), -sqrt(0.5), sqrt(0.5), 0, 0, 1e-5);
	TRANSFORM_OK("rotate(0.25Turn)", 0, 1, -1, 0, 0, 0, 1e-5);
	TRANSFORM_OK("rotate(100grad)", 0, 1, -1, 0, 0, 0, 1e-5);
	TRANSFORM_OK("rotate(1.5707963rad)", 0, 1, -1, 0, 0, 0, 1e-5);
	check_transform_point("rotate(90deg 10 0)", 11.0f, 0.0f, 10.0, 1.0, __LINE__);

	/* skewX(a): c = tan a; skewY(a): b = tan a, also with units. */
	TRANSFORM_OK("skewX(45)", 1, 0, 1, 1, 0, 0, 1e-5);
	TRANSFORM_OK("skewY(45)", 1, 1, 0, 1, 0, 0, 1e-5);
	TRANSFORM_OK("skewX(-45)", 1, 0, -1, 1, 0, 0, 1e-5);
	TRANSFORM_OK("skewX(0.125turn)", 1, 0, 1, 1, 0, 0, 1e-5);
	TRANSFORM_OK("skewY(50grad)", 1, 1, 0, 1, 0, 0, 1e-5);
	TRANSFORM_OK("skewX(30)", 1, 0, tan(30.0 * TEST_PI / 180.0), 1, 0, 0, 1e-5);

	/*
	 * The list composes M1 x M2 x ...: the leftmost applies last to a
	 * point.  translate(10) scale(2) maps (1, 0) to scale first (2, 0)
	 * then translate (12, 0); scale(2) translate(10) maps (1, 0) to (11, 0)
	 * then (22, 0).
	 */
	check_transform_point("translate(10) scale(2)", 1.0f, 0.0f, 12.0, 0.0, __LINE__);
	check_transform_point("scale(2) translate(10)", 1.0f, 0.0f, 22.0, 0.0, __LINE__);
	TRANSFORM_OK("translate(10) scale(2)", 2, 0, 0, 2, 10, 0, 0.0);
	TRANSFORM_OK("scale(2) translate(10)", 2, 0, 0, 2, 20, 0, 0.0);
	check_transform_point("rotate(90) translate(10 0)", 0.0f, 0.0f, 0.0, 10.0, __LINE__);
	check_transform_point("translate(10 0) rotate(90)", 0.0f, 0.0f, 10.0, 0.0, __LINE__);
	check_transform_point("translate(1 2) scale(3) translate(4 5)", 1.0f, 1.0f, 16.0, 20.0, __LINE__);

	/* Between functions: white space, a comma, or both. */
	TRANSFORM_OK("translate(1),scale(2)", 2, 0, 0, 2, 1, 0, 0.0);
	TRANSFORM_OK("translate(1) , scale(2)", 2, 0, 0, 2, 1, 0, 0.0);
	TRANSFORM_OK("  translate(1)\n\tscale(2)  ", 2, 0, 0, 2, 1, 0, 0.0);

	/* A failed parse leaves the matrix alone and the whole attribute invalid. */
	TRANSFORM_BAD("translate(10) foo");
	TRANSFORM_BAD("foo(1)");
	TRANSFORM_BAD("translate(10) translate()");
	TRANSFORM_BAD("translate()");
	TRANSFORM_BAD("translate(1,)");
	TRANSFORM_BAD("translate(,1)");
	TRANSFORM_BAD("translate(1,,2)");
	TRANSFORM_BAD("translate(1 2 3)");
	TRANSFORM_BAD("translate(1");
	TRANSFORM_BAD("translate 1)");
	TRANSFORM_BAD("translate");
	TRANSFORM_BAD("translate(a)");
	TRANSFORM_BAD("scale()");
	TRANSFORM_BAD("scale(1 2 3)");
	TRANSFORM_BAD("scale(2px)");
	TRANSFORM_BAD("scale(1 2deg)");
	TRANSFORM_BAD("rotate()");
	TRANSFORM_BAD("rotate(1 2)");
	TRANSFORM_BAD("rotate(1 2 3 4)");
	TRANSFORM_BAD("rotate(45px)");
	TRANSFORM_BAD("rotate(45 1deg 2)");
	TRANSFORM_BAD("skewX()");
	TRANSFORM_BAD("skewX(1 2)");
	TRANSFORM_BAD("skewX(1px)");
	TRANSFORM_BAD("matrix(1 2 3 4 5)");
	TRANSFORM_BAD("matrix(1 2 3 4 5 6 7)");
	TRANSFORM_BAD("matrix(1 2 3 4 5 6px)");

	/* Units that do not exist, or that a length does not take here. */
	TRANSFORM_BAD("translate(10em)");
	TRANSFORM_BAD("translate(10%)");
	TRANSFORM_BAD("translate(10deg)");
	TRANSFORM_BAD("translate(10foo)");
	TRANSFORM_BAD("translate(1e)");
	TRANSFORM_BAD("rotate(45foo)");
	TRANSFORM_BAD("rotate(45px5)");
	TRANSFORM_BAD("rotate(1 2 3deg)");

	/* The function names are case-sensitive, as the attribute grammar's literals are. */
	TRANSFORM_BAD("Translate(1)");
	TRANSFORM_BAD("SCALE(2)");
	TRANSFORM_BAD("skewx(1)");
	TRANSFORM_BAD("translatex(1)");

	/* A function that follows another needs a separator, and the list does not start or end with a comma. */
	TRANSFORM_BAD("translate(1)scale(2)");
	TRANSFORM_BAD("translate(1),,scale(2)");
	TRANSFORM_BAD(",translate(1)");
	TRANSFORM_BAD("translate(1),");
	TRANSFORM_BAD("translate(1) , ");
	TRANSFORM_BAD(",");

	/* An angle at which the tangent is not finite, and products that overflow. */
	TRANSFORM_BAD("skewX(90)");
	TRANSFORM_BAD("skewY(270)");
	TRANSFORM_BAD("scale(1e30) scale(1e30)");
	TRANSFORM_BAD("scale(1e38) scale(1e38) scale(0)");
	TRANSFORM_BAD("rotate(1e39)");

	/* Too many functions: 256 are fine, the 257th is ENOBUFS. */
	length = strlen(piece);
	text = malloc(length * 257 + 1);
	if (text == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	for (count = 256; count <= 257; count++) {
		for (index = 0; index < count; index++)
			memcpy(text + index * length, piece, length);
		text[count * length] = '\0';
		before = make_matrix(TEST_SENTINEL, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f);
		matrix = before;
		if (count == 256) {
			CHECK_LONG(svg_parse_transform(text, &matrix), 0);
			CHECK_NEAR(matrix.a, 1.0, 0.0);
		} else {
			CHECK_LONG(svg_parse_transform(text, &matrix), ENOBUFS);
			CHECK(memcmp(&matrix, &before, sizeof(matrix)) == 0);
		}
	}
	free(text);

	/* NULL arguments. */
	CHECK_LONG(svg_parse_transform(NULL, &matrix), EINVAL);
	CHECK_LONG(svg_parse_transform("scale(1)", NULL), EINVAL);
}

/* Parses a viewBox and checks its four edges. */
static void
check_view_box(
	const char *text,
	double left,
	double top,
	double right,
	double bottom,
	int line)
{
	char *copy;
	struct vec_rect rect;
	int error;

	copy = exact_copy(text);
	rect.left = TEST_SENTINEL;
	rect.top = TEST_SENTINEL;
	rect.right = TEST_SENTINEL;
	rect.bottom = TEST_SENTINEL;
	error = svg_parse_view_box(copy, &rect);
	check_record_long(error, 0, __FILE__, line, text);
	check_record_near(rect.left, left, 1e-6, __FILE__, line, text);
	check_record_near(rect.top, top, 1e-6, __FILE__, line, text);
	check_record_near(rect.right, right, 1e-6, __FILE__, line, text);
	check_record_near(rect.bottom, bottom, 1e-6, __FILE__, line, text);
	free(copy);
}

/* Checks that a viewBox is refused and the rectangle is left alone. */
static void
check_view_box_bad(
	const char *text,
	int line)
{
	char *copy;
	struct vec_rect rect;
	int error;

	copy = exact_copy(text);
	rect.left = TEST_SENTINEL;
	rect.top = TEST_SENTINEL;
	rect.right = TEST_SENTINEL;
	rect.bottom = TEST_SENTINEL;
	error = svg_parse_view_box(copy, &rect);
	check_record_long(error, EINVAL, __FILE__, line, text);
	check_record(rect.left == TEST_SENTINEL && rect.top == TEST_SENTINEL && rect.right == TEST_SENTINEL &&
	    rect.bottom == TEST_SENTINEL, __FILE__, line, "rectangle unchanged");
	free(copy);
}

#define VIEW_BOX_OK(text, l, t, r, b)	check_view_box(text, l, t, r, b, __LINE__)
#define VIEW_BOX_BAD(text)		check_view_box_bad(text, __LINE__)

/* Checks svg_parse_view_box: four numbers, a non-negative size. */
static void
test_view_box(void)
{
	/* The edges are min-x, min-y, min-x + width, min-y + height. */
	VIEW_BOX_OK("0 0 100 50", 0, 0, 100, 50);
	VIEW_BOX_OK("10,20,30,40", 10, 20, 40, 60);
	VIEW_BOX_OK("-5 -5 10 10", -5, -5, 5, 5);
	VIEW_BOX_OK("  0.5 , 1.5 , 2 , 3  ", 0.5, 1.5, 2.5, 4.5);
	VIEW_BOX_OK("1e1 0 1e1 1e1", 10, 0, 20, 10);

	/* A zero width or height is valid here (the element is not rendered). */
	VIEW_BOX_OK("0 0 0 10", 0, 0, 0, 10);
	VIEW_BOX_OK("0 0 10 0", 0, 0, 10, 0);

	/* A negative width or height is an error; anything but four numbers too. */
	VIEW_BOX_BAD("0 0 -1 10");
	VIEW_BOX_BAD("0 0 10 -1");
	VIEW_BOX_BAD("0 0 10");
	VIEW_BOX_BAD("0 0 10 10 10");
	VIEW_BOX_BAD("0 0 10 10,");
	VIEW_BOX_BAD(",0 0 10 10");
	VIEW_BOX_BAD("0 0 10 10px");
	VIEW_BOX_BAD("a b c d");
	VIEW_BOX_BAD("");
	VIEW_BOX_BAD("   ");
	VIEW_BOX_BAD("0 0 1e39 1");
	VIEW_BOX_BAD("3e38 0 3e38 1");
}

/* Checks svg_parse_aspect on the ten alignments, meet and slice, and the refusals. */
static void
test_aspect(void)
{
	static const struct {
		const char *name;
		enum svg_align align;
	} names[10] = {
		{ "none", SVG_ALIGN_NONE },
		{ "xMinYMin", SVG_ALIGN_XMIN_YMIN },
		{ "xMidYMin", SVG_ALIGN_XMID_YMIN },
		{ "xMaxYMin", SVG_ALIGN_XMAX_YMIN },
		{ "xMinYMid", SVG_ALIGN_XMIN_YMID },
		{ "xMidYMid", SVG_ALIGN_XMID_YMID },
		{ "xMaxYMid", SVG_ALIGN_XMAX_YMID },
		{ "xMinYMax", SVG_ALIGN_XMIN_YMAX },
		{ "xMidYMax", SVG_ALIGN_XMID_YMAX },
		{ "xMaxYMax", SVG_ALIGN_XMAX_YMAX }
	};
	struct svg_aspect aspect;
	char text[64];
	int index;

	/* Each alignment alone is meet; with meet or slice after white space it is that. */
	for (index = 0; index < 10; index++) {
		aspect.align = (enum svg_align)99;
		aspect.slice = 99;
		CHECK_LONG(svg_parse_aspect(names[index].name, &aspect), 0);
		CHECK_LONG(aspect.align, names[index].align);
		CHECK_LONG(aspect.slice, 0);

		sprintf(text, "%s meet", names[index].name);
		CHECK_LONG(svg_parse_aspect(text, &aspect), 0);
		CHECK_LONG(aspect.align, names[index].align);
		CHECK_LONG(aspect.slice, 0);

		sprintf(text, "  %s \t slice  ", names[index].name);
		CHECK_LONG(svg_parse_aspect(text, &aspect), 0);
		CHECK_LONG(aspect.align, names[index].align);
		CHECK_LONG(aspect.slice, 1);
	}

	/* The alignment codes are as the header numbers them, none being 0. */
	CHECK_LONG(SVG_ALIGN_NONE, 0);

	/* Anything else is refused and the output is left alone. */
	aspect.align = SVG_ALIGN_XMAX_YMAX;
	aspect.slice = 1;
	CHECK_LONG(svg_parse_aspect("", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("   ", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("xmidymid", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("XMidYMid", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("None", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("xMidYMid Meet", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("xMidYMid SLICE", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("xMidYMidslice", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("xMidYMid,slice", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("xMidYMid slice extra", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("xMidYMid meet slice", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("xMidYMid sliced", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("defer xMidYMid", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("xMidYMid defer", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("xMinYMin1", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("meet", &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect(NULL, &aspect), EINVAL);
	CHECK_LONG(svg_parse_aspect("none", NULL), EINVAL);
	CHECK_LONG(aspect.align, SVG_ALIGN_XMAX_YMAX);
	CHECK_LONG(aspect.slice, 1);
}

/* Builds a rectangle from its edges. */
static struct vec_rect
make_rect(
	float left,
	float top,
	float right,
	float bottom)
{
	struct vec_rect rect;

	rect.left = left;
	rect.top = top;
	rect.right = right;
	rect.bottom = bottom;
	return rect;
}

/* Builds an aspect from an alignment and slice. */
static struct svg_aspect
make_aspect(
	enum svg_align align,
	int slice)
{
	struct svg_aspect aspect;

	aspect.align = align;
	aspect.slice = slice;
	return aspect;
}

/* Computes the transform of a viewBox and checks a, d, e and f (b and c are 0). */
static void
check_view_box_transform(
	struct vec_rect view_box,
	struct svg_aspect aspect,
	float x,
	float y,
	float width,
	float height,
	double scale_x,
	double scale_y,
	double translate_x,
	double translate_y,
	int line)
{
	struct vec_matrix matrix;
	int error;

	matrix = make_matrix(TEST_SENTINEL, TEST_SENTINEL, TEST_SENTINEL, TEST_SENTINEL, TEST_SENTINEL, TEST_SENTINEL);
	error = svg_view_box_transform(&view_box, &aspect, x, y, width, height, &matrix);
	check_record_long(error, 0, __FILE__, line, "svg_view_box_transform");
	check_record_near(matrix.a, scale_x, 1e-6, __FILE__, line, "a");
	check_record_near(matrix.b, 0.0, 0.0, __FILE__, line, "b");
	check_record_near(matrix.c, 0.0, 0.0, __FILE__, line, "c");
	check_record_near(matrix.d, scale_y, 1e-6, __FILE__, line, "d");
	check_record_near(matrix.e, translate_x, 1e-5, __FILE__, line, "e");
	check_record_near(matrix.f, translate_y, 1e-5, __FILE__, line, "f");
}

#define VBT(vb, aspect, x, y, w, h, sx, sy, tx, ty) \
	check_view_box_transform(vb, aspect, x, y, w, h, sx, sy, tx, ty, __LINE__)

/* Checks svg_view_box_transform against the algorithm of SVG 2 §8.2 worked by hand. */
static void
test_view_box_transform(void)
{
	struct vec_matrix matrix;
	struct vec_matrix before;
	struct vec_rect view_box;
	struct svg_aspect aspect;
	struct vec_point corner;
	enum svg_align align;
	uint32_t seed;
	int iteration;
	int slice;
	float x;
	float y;
	float width;
	float height;
	double left;
	double right;
	double top;
	double bottom;
	double epsilon;

	/*
	 * A viewBox 0 0 100 50 in a viewport 200 by 200 at the origin:
	 * scale-x = 2, scale-y = 4.
	 * xMidYMid meet: both 2; tx = 0 + (200 - 100 * 2) / 2 = 0, ty = (200 - 50 * 2) / 2 = 50.
	 * xMidYMid slice: both 4; tx = (200 - 100 * 4) / 2 = -100, ty = (200 - 50 * 4) / 2 = 0.
	 * none: 2 and 4 with no alignment: tx = ty = 0.
	 */
	view_box = make_rect(0.0f, 0.0f, 100.0f, 50.0f);
	VBT(view_box, make_aspect(SVG_ALIGN_XMID_YMID, 0), 0.0f, 0.0f, 200.0f, 200.0f, 2.0, 2.0, 0.0, 50.0);
	VBT(view_box, make_aspect(SVG_ALIGN_XMID_YMID, 1), 0.0f, 0.0f, 200.0f, 200.0f, 4.0, 4.0, -100.0, 0.0);
	VBT(view_box, make_aspect(SVG_ALIGN_NONE, 0), 0.0f, 0.0f, 200.0f, 200.0f, 2.0, 4.0, 0.0, 0.0);

	/* none ignores slice, and an alignment is not applied for none (no xMid centring). */
	VBT(view_box, make_aspect(SVG_ALIGN_NONE, 1), 10.0f, 20.0f, 200.0f, 200.0f, 2.0, 4.0, 10.0, 20.0);

	/*
	 * A viewBox at (10, 20) of size 50 by 40 in a viewport at (5, 7) of 200 by 100:
	 * scale-x = 4, scale-y = 2.5.
	 * xMidYMid meet: s = 2.5; tx = 5 - 10 * 2.5 + (200 - 125) / 2 = 17.5;
	 *   ty = 7 - 20 * 2.5 + (100 - 100) / 2 = -43.
	 * xMidYMid slice: s = 4; tx = 5 - 40 + (200 - 200) / 2 = -35;
	 *   ty = 7 - 80 + (100 - 160) / 2 = -103.
	 * xMaxYMin meet: tx = 5 - 25 + (200 - 125) = 55; ty = -43.
	 * xMinYMax slice: tx = 5 - 40 = -35; ty = 7 - 80 + (100 - 160) = -133.
	 * xMinYMin meet: tx = -20; ty = -43.
	 * xMinYMid slice: tx = -35; ty = -103.
	 * xMidYMax meet: tx = 17.5; ty = -43 + 0 = -43.
	 */
	view_box = make_rect(10.0f, 20.0f, 60.0f, 60.0f);
	VBT(view_box, make_aspect(SVG_ALIGN_XMID_YMID, 0), 5.0f, 7.0f, 200.0f, 100.0f, 2.5, 2.5, 17.5, -43.0);
	VBT(view_box, make_aspect(SVG_ALIGN_XMID_YMID, 1), 5.0f, 7.0f, 200.0f, 100.0f, 4.0, 4.0, -35.0, -103.0);
	VBT(view_box, make_aspect(SVG_ALIGN_XMAX_YMIN, 0), 5.0f, 7.0f, 200.0f, 100.0f, 2.5, 2.5, 55.0, -43.0);
	VBT(view_box, make_aspect(SVG_ALIGN_XMIN_YMAX, 1), 5.0f, 7.0f, 200.0f, 100.0f, 4.0, 4.0, -35.0, -133.0);
	VBT(view_box, make_aspect(SVG_ALIGN_XMIN_YMIN, 0), 5.0f, 7.0f, 200.0f, 100.0f, 2.5, 2.5, -20.0, -43.0);
	VBT(view_box, make_aspect(SVG_ALIGN_XMIN_YMID, 1), 5.0f, 7.0f, 200.0f, 100.0f, 4.0, 4.0, -35.0, -103.0);
	VBT(view_box, make_aspect(SVG_ALIGN_XMID_YMAX, 0), 5.0f, 7.0f, 200.0f, 100.0f, 2.5, 2.5, 17.5, -43.0);
	VBT(view_box, make_aspect(SVG_ALIGN_NONE, 0), 5.0f, 7.0f, 200.0f, 100.0f, 4.0, 2.5, -35.0, -43.0);

	/* A viewport of the viewBox's own shape is the translation by the difference of the origins. */
	view_box = make_rect(-10.0f, -20.0f, 10.0f, 20.0f);
	VBT(view_box, make_aspect(SVG_ALIGN_XMID_YMID, 0), 3.0f, 4.0f, 20.0f, 40.0f, 1.0, 1.0, 13.0, 24.0);

	/* A viewport of zero width is allowed and scales to zero. */
	view_box = make_rect(0.0f, 0.0f, 10.0f, 10.0f);
	VBT(view_box, make_aspect(SVG_ALIGN_XMID_YMID, 0), 0.0f, 0.0f, 0.0f, 10.0f, 0.0, 0.0, 0.0, 5.0);

	/* The refusals: a viewBox without a positive size, a negative viewport, values not finite. */
	before = make_matrix(TEST_SENTINEL, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f);
	matrix = before;
	aspect = make_aspect(SVG_ALIGN_XMID_YMID, 0);
	view_box = make_rect(0.0f, 0.0f, 0.0f, 10.0f);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, 0.0f, 0.0f, 10.0f, 10.0f, &matrix), EINVAL);
	view_box = make_rect(0.0f, 0.0f, 10.0f, 0.0f);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, 0.0f, 0.0f, 10.0f, 10.0f, &matrix), EINVAL);
	view_box = make_rect(10.0f, 0.0f, 0.0f, 10.0f);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, 0.0f, 0.0f, 10.0f, 10.0f, &matrix), EINVAL);
	view_box = make_rect(0.0f, 0.0f, 10.0f, 10.0f);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, 0.0f, 0.0f, -1.0f, 10.0f, &matrix), EINVAL);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, 0.0f, 0.0f, 10.0f, -1.0f, &matrix), EINVAL);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, nanf(""), 0.0f, 10.0f, 10.0f, &matrix), EINVAL);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, 0.0f, INFINITY, 10.0f, 10.0f, &matrix), EINVAL);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, 0.0f, 0.0f, INFINITY, 10.0f, &matrix), EINVAL);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, 0.0f, 0.0f, 10.0f, nanf(""), &matrix), EINVAL);
	view_box = make_rect(nanf(""), 0.0f, 10.0f, 10.0f);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, 0.0f, 0.0f, 10.0f, 10.0f, &matrix), EINVAL);
	view_box = make_rect(0.0f, 0.0f, 1.0e-30f, 10.0f);
	aspect = make_aspect(SVG_ALIGN_XMID_YMID, 1);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, 0.0f, 0.0f, 3.0e38f, 10.0f, &matrix), EINVAL);
	aspect = make_aspect(SVG_ALIGN_XMID_YMID, 0);
	CHECK_LONG(svg_view_box_transform(NULL, &aspect, 0.0f, 0.0f, 10.0f, 10.0f, &matrix), EINVAL);
	view_box = make_rect(0.0f, 0.0f, 10.0f, 10.0f);
	CHECK_LONG(svg_view_box_transform(&view_box, NULL, 0.0f, 0.0f, 10.0f, 10.0f, &matrix), EINVAL);
	CHECK_LONG(svg_view_box_transform(&view_box, &aspect, 0.0f, 0.0f, 10.0f, 10.0f, NULL), EINVAL);
	CHECK(memcmp(&matrix, &before, sizeof(matrix)) == 0);

	/*
	 * A random check of the properties the algorithm gives.  For meet the
	 * mapped viewBox lies in the viewport and keeps its shape; for slice
	 * it covers the viewport; the aligned edges meet the viewport's.
	 */
	seed = 4242;
	for (iteration = 0; iteration < 20000; iteration++) {
		view_box = make_rect(check_random_float(&seed, -100.0f, 100.0f), check_random_float(&seed, -100.0f, 100.0f), 0.0f, 0.0f);
		view_box.right = view_box.left + check_random_float(&seed, 0.5f, 300.0f);
		view_box.bottom = view_box.top + check_random_float(&seed, 0.5f, 300.0f);
		x = check_random_float(&seed, -100.0f, 100.0f);
		y = check_random_float(&seed, -100.0f, 100.0f);
		width = check_random_float(&seed, 1.0f, 400.0f);
		height = check_random_float(&seed, 1.0f, 400.0f);
		align = (enum svg_align)(1 + check_random(&seed) % 9);
		slice = (int)(check_random(&seed) % 2);
		aspect = make_aspect(align, slice);
		CHECK_LONG(svg_view_box_transform(&view_box, &aspect, x, y, width, height, &matrix), 0);

		/* Maps the viewBox's two opposite corners. */
		corner.x = view_box.left;
		corner.y = view_box.top;
		corner = vec_matrix_apply(&matrix, corner);
		left = corner.x;
		top = corner.y;
		corner.x = view_box.right;
		corner.y = view_box.bottom;
		corner = vec_matrix_apply(&matrix, corner);
		right = corner.x;
		bottom = corner.y;
		epsilon = 0.05;

		/* The scale is uniform for every alignment but none. */
		CHECK_NEAR(matrix.a, matrix.d, 1e-6 * fabs(matrix.a));

		/* Meet fits inside the viewport, slice covers it. */
		if (!slice) {
			CHECK(left >= x - epsilon && right <= x + width + epsilon);
			CHECK(top >= y - epsilon && bottom <= y + height + epsilon);
			CHECK(fabs((right - left) - width) < epsilon || fabs((bottom - top) - height) < epsilon);
		} else {
			CHECK(left <= x + epsilon && right >= x + width - epsilon);
			CHECK(top <= y + epsilon && bottom >= y + height - epsilon);
		}

		/* The edges of the alignment: xMin and yMin touch the near edges, xMax and yMax the far ones, Mid centres. */
		if (align == SVG_ALIGN_XMIN_YMIN || align == SVG_ALIGN_XMIN_YMID || align == SVG_ALIGN_XMIN_YMAX)
			CHECK_NEAR(left, x, epsilon);
		if (align == SVG_ALIGN_XMAX_YMIN || align == SVG_ALIGN_XMAX_YMID || align == SVG_ALIGN_XMAX_YMAX)
			CHECK_NEAR(right, x + width, epsilon);
		if (align == SVG_ALIGN_XMID_YMIN || align == SVG_ALIGN_XMID_YMID || align == SVG_ALIGN_XMID_YMAX)
			CHECK_NEAR((left + right) / 2.0, x + width / 2.0, epsilon);
		if (align == SVG_ALIGN_XMIN_YMIN || align == SVG_ALIGN_XMID_YMIN || align == SVG_ALIGN_XMAX_YMIN)
			CHECK_NEAR(top, y, epsilon);
		if (align == SVG_ALIGN_XMIN_YMAX || align == SVG_ALIGN_XMID_YMAX || align == SVG_ALIGN_XMAX_YMAX)
			CHECK_NEAR(bottom, y + height, epsilon);
		if (align == SVG_ALIGN_XMIN_YMID || align == SVG_ALIGN_XMID_YMID || align == SVG_ALIGN_XMAX_YMID)
			CHECK_NEAR((top + bottom) / 2.0, y + height / 2.0, epsilon);
	}
}

/* Reads an url reference and checks the id, the error and where the cursor stopped. */
static void
check_url(
	const char *text,
	int bare,
	int expected_error,
	const char *expected_id,
	long expected_consumed,
	int line)
{
	char *copy;
	const char *cursor;
	char id[SVG_ID_MAX + 1];
	int error;

	copy = exact_copy(text);
	cursor = copy;
	memset(id, 'Z', sizeof(id));
	error = svg_url_read(&cursor, bare, id);
	check_record_long(error, expected_error, __FILE__, line, text);
	if (expected_error == 0) {
		check_record(strcmp(id, expected_id) == 0, __FILE__, line, expected_id);
		check_record_long((long)(cursor - copy), expected_consumed, __FILE__, line, text);
	} else {
		/* A failure leaves the cursor and the id alone. */
		check_record(cursor == copy, __FILE__, line, "cursor unchanged");
		check_record(id[0] == 'Z' && id[SVG_ID_MAX] == 'Z', __FILE__, line, "id unchanged");
	}
	free(copy);
}

#define URL_OK(text, id, consumed)	check_url(text, 0, 0, id, consumed, __LINE__)
#define URL_BARE_OK(text, id, consumed)	check_url(text, 1, 0, id, consumed, __LINE__)
#define URL_BAD(text, error)		check_url(text, 0, error, "", 0, __LINE__)
#define URL_BARE_BAD(text, error)	check_url(text, 1, error, "", 0, __LINE__)

/* Checks svg_url_read: the three quoting forms, white space, other documents and long ids. */
static void
test_url_read(void)
{
	char text[SVG_ID_MAX + 64];
	char *copy;
	const char *cursor;
	char id[SVG_ID_MAX + 1];
	size_t index;

	/* The three forms of url( ), and the cursor ends after the parenthesis. */
	URL_OK("url(#a)", "a", 7);
	URL_OK("url('#a')", "a", 9);
	URL_OK("url(\"#a\")", "a", 9);
	URL_OK("URL(#a)", "a", 7);
	URL_OK("Url(#abc-1_2.x)", "abc-1_2.x", 15);

	/* White space inside the parentheses, and before the reference. */
	URL_OK("url( #a )", "a", 9);
	URL_OK("url(\t#a\n)", "a", 9);
	URL_OK("url( '#a' )", "a", 11);
	URL_OK("  url(#a)", "a", 9);

	/* The text after the reference is left. */
	URL_OK("url(#a) none", "a", 7);
	URL_OK("url(#a)none", "a", 7);

	/* A quoted id may hold white space and parentheses. */
	URL_OK("url('#a b')", "a b", 11);
	URL_OK("url('#a)b')", "a)b", 11);

	/* A bare reference, as href is written; the id ends at white space. */
	URL_BARE_OK("#a", "a", 2);
	URL_BARE_OK("#abc", "abc", 4);
	URL_BARE_OK("  #a", "a", 4);
	URL_BARE_OK("#a b", "a", 2);
	URL_BARE_OK("#a)", "a)", 3);

	/* Malformed: missing parts, an empty id, unbalanced quotes. */
	URL_BAD("", EINVAL);
	URL_BAD("foo", EINVAL);
	URL_BAD("ur", EINVAL);
	URL_BAD("url", EINVAL);
	URL_BAD("url (#a)", EINVAL);
	URL_BAD("#a", EINVAL);
	URL_BAD("url(", EINVAL);
	URL_BAD("url()", EINVAL);
	URL_BAD("url( )", EINVAL);
	URL_BAD("url(#)", EINVAL);
	URL_BAD("url(#a", EINVAL);
	URL_BAD("url(#a b)", EINVAL);
	URL_BAD("url('#a)", EINVAL);
	URL_BAD("url('#a'", EINVAL);
	URL_BAD("url('#a\")", EINVAL);
	URL_BAD("url('')", EINVAL);
	URL_BAD("url('#')", EINVAL);
	URL_BAD("url(\"\")", EINVAL);
	URL_BARE_BAD("", EINVAL);
	URL_BARE_BAD("#", EINVAL);
	URL_BARE_BAD("   ", EINVAL);
	URL_BARE_BAD("# a", EINVAL);

	/* A reference to another document is not supported. */
	URL_BAD("url(other.svg#a)", ENOTSUP);
	URL_BAD("url('http://example.com/x.svg#a')", ENOTSUP);
	URL_BAD("url(\"x#a\")", ENOTSUP);
	URL_BAD("url(a)", ENOTSUP);
	URL_BARE_BAD("other.svg#a", ENOTSUP);
	URL_BARE_BAD("a", ENOTSUP);

	/* An id of exactly SVG_ID_MAX bytes is kept; one more is ENOBUFS. */
	memset(text, 0, sizeof(text));
	strcpy(text, "url(#");
	for (index = 0; index < SVG_ID_MAX; index++)
		text[5 + index] = 'x';
	strcpy(text + 5 + SVG_ID_MAX, ")");
	copy = exact_copy(text);
	cursor = copy;
	CHECK_LONG(svg_url_read(&cursor, 0, id), 0);
	CHECK_LONG(strlen(id), SVG_ID_MAX);
	CHECK_LONG(id[SVG_ID_MAX - 1], 'x');
	CHECK_LONG(cursor - copy, 5 + SVG_ID_MAX + 1);
	free(copy);
	memset(text, 0, sizeof(text));
	strcpy(text, "url(#");
	for (index = 0; index < SVG_ID_MAX + 1; index++)
		text[5 + index] = 'x';
	strcpy(text + 5 + SVG_ID_MAX + 1, ")");
	URL_BAD(text, ENOBUFS);
	memset(text, 0, sizeof(text));
	text[0] = '#';
	for (index = 0; index < SVG_ID_MAX + 1; index++)
		text[1 + index] = 'y';
	URL_BARE_BAD(text, ENOBUFS);

	/* NULL arguments. */
	CHECK_LONG(svg_url_read(NULL, 0, id), EINVAL);
	cursor = NULL;
	CHECK_LONG(svg_url_read(&cursor, 0, id), EINVAL);
	cursor = "url(#a)";
	CHECK_LONG(svg_url_read(&cursor, 0, NULL), EINVAL);
}

/* A color parser that records what it was given: it reports the length as the color, so the tests see the trimming. */
static int
recording_parse_color(
	void *context,
	const char *text,
	size_t length,
	uint32_t *color)
{
	char *seen;

	seen = context;
	if (length >= 63)
		return EINVAL;
	memcpy(seen, text, length);
	seen[length] = '\0';
	if (strncmp(text, "bad", 3) == 0)
		return EINVAL;
	*color = (uint32_t)(0xff000000U | (uint32_t)length);
	return 0;
}

/* Parses a paint with the test tree and checks the result. */
static void
check_paint(
	const struct svg_tree *tree,
	const char *text,
	enum svg_paint_kind kind,
	uint32_t color,
	const char *id,
	enum svg_paint_kind fallback_kind,
	uint32_t fallback_color,
	int line)
{
	char *copy;
	struct svg_paint paint;
	int error;

	copy = exact_copy(text);
	memset(&paint, 0x5a, sizeof(paint));
	error = svg_parse_paint(tree, copy, &paint);
	check_record_long(error, 0, __FILE__, line, text);
	check_record_long(paint.kind, kind, __FILE__, line, text);
	check_record_color(paint.color, color, 0, __FILE__, line, text);
	check_record(strcmp(paint.id, id) == 0, __FILE__, line, "id");
	check_record_long(paint.fallback_kind, fallback_kind, __FILE__, line, text);
	check_record_color(paint.fallback_color, fallback_color, 0, __FILE__, line, text);
	free(copy);
}

/* Checks that a paint is refused and the output is left alone. */
static void
check_paint_bad(
	const struct svg_tree *tree,
	const char *text,
	int expected_error,
	int line)
{
	char *copy;
	struct svg_paint paint;
	struct svg_paint before;
	int error;

	copy = exact_copy(text);
	memset(&paint, 0x5a, sizeof(paint));
	before = paint;
	error = svg_parse_paint(tree, copy, &paint);
	check_record_long(error, expected_error, __FILE__, line, text);
	check_record(memcmp(&paint, &before, sizeof(paint)) == 0, __FILE__, line, "paint unchanged");
	free(copy);
}

#define PAINT_OK(text, kind, color, id, fkind, fcolor)	check_paint(&tree, text, kind, color, id, fkind, fcolor, __LINE__)
#define PAINT_BAD(text, error)				check_paint_bad(&tree, text, error, __LINE__)

/* Checks svg_parse_paint: the keywords, colors through the tree, references with fallbacks, refusals. */
static void
test_paint(void)
{
	struct test_document document;
	struct svg_tree tree;
	struct svg_tree recording;
	struct svg_paint paint;
	char seen[64];

	memset(&document, 0, sizeof(document));
	test_tree_make(&tree, &document);

	/* none and currentColor, in any case and with white space around. */
	PAINT_OK("none", SVG_PAINT_NONE, 0, "", SVG_PAINT_NONE, 0);
	PAINT_OK("  NONE\t", SVG_PAINT_NONE, 0, "", SVG_PAINT_NONE, 0);
	PAINT_OK("currentColor", SVG_PAINT_CURRENT_COLOR, 0, "", SVG_PAINT_NONE, 0);
	PAINT_OK("CURRENTCOLOR", SVG_PAINT_CURRENT_COLOR, 0, "", SVG_PAINT_NONE, 0);
	PAINT_OK(" currentcolor ", SVG_PAINT_CURRENT_COLOR, 0, "", SVG_PAINT_NONE, 0);

	/* A color goes through the tree's parse_color. */
	PAINT_OK("red", SVG_PAINT_COLOR, 0xffff0000U, "", SVG_PAINT_NONE, 0);
	PAINT_OK("  #00f  ", SVG_PAINT_COLOR, 0xff0000ffU, "", SVG_PAINT_NONE, 0);
	PAINT_OK("#11223344", SVG_PAINT_COLOR, 0x44112233U, "", SVG_PAINT_NONE, 0);
	PAINT_OK("transparent", SVG_PAINT_COLOR, 0x00000000U, "", SVG_PAINT_NONE, 0);

	/* A reference with no fallback has the fallback kind URL (none given). */
	PAINT_OK("url(#g)", SVG_PAINT_URL, 0, "g", SVG_PAINT_URL, 0);
	PAINT_OK("  url( '#grad-1' )  ", SVG_PAINT_URL, 0, "grad-1", SVG_PAINT_URL, 0);
	PAINT_OK("URL(#g)", SVG_PAINT_URL, 0, "g", SVG_PAINT_URL, 0);

	/* A fallback: none, currentColor or a color. */
	PAINT_OK("url(#g) none", SVG_PAINT_URL, 0, "g", SVG_PAINT_NONE, 0);
	PAINT_OK("url(#g)   currentColor ", SVG_PAINT_URL, 0, "g", SVG_PAINT_CURRENT_COLOR, 0);
	PAINT_OK("url(#g) red", SVG_PAINT_URL, 0, "g", SVG_PAINT_COLOR, 0xffff0000U);
	PAINT_OK("url(#g) #0f08", SVG_PAINT_URL, 0, "g", SVG_PAINT_COLOR, 0x8800ff00U);
	PAINT_OK("url(#g)none", SVG_PAINT_URL, 0, "g", SVG_PAINT_NONE, 0);

	/* Refused: empty, unknown words, a second reference, trailing words, bad references. */
	PAINT_BAD("", EINVAL);
	PAINT_BAD("   ", EINVAL);
	PAINT_BAD("foo", EINVAL);
	PAINT_BAD("none x", EINVAL);
	PAINT_BAD("red blue", EINVAL);
	PAINT_BAD("url(#g) foo", EINVAL);
	PAINT_BAD("url(#g) url(#h)", EINVAL);
	PAINT_BAD("url(#g) none none", EINVAL);
	PAINT_BAD("url(#)", EINVAL);
	PAINT_BAD("url(#g", EINVAL);
	PAINT_BAD("url(", EINVAL);
	PAINT_BAD("url", EINVAL);
	PAINT_BAD("url(#g) context-fill", EINVAL);

	/* context-fill and context-stroke are not supported; neither is another document. */
	PAINT_BAD("context-fill", ENOTSUP);
	PAINT_BAD("context-stroke", ENOTSUP);
	PAINT_BAD("CONTEXT-FILL", ENOTSUP);
	PAINT_BAD(" Context-Stroke ", ENOTSUP);
	PAINT_BAD("url(other.svg#g)", ENOTSUP);
	PAINT_BAD("url('http://x/y.svg#g') red", ENOTSUP);

	/* The text given to the color parser is trimmed at both ends and its length is exact. */
	recording = tree;
	recording.context = seen;
	recording.parse_color = recording_parse_color;
	memset(seen, 0, sizeof(seen));
	CHECK_LONG(svg_parse_paint(&recording, "  rgb(1, 2, 3) \t", &paint), 0);
	CHECK_LONG(paint.kind, SVG_PAINT_COLOR);
	CHECK(strcmp(seen, "rgb(1, 2, 3)") == 0);
	CHECK_COLOR(paint.color, 0xff00000cU, 0);
	CHECK_LONG(svg_parse_paint(&recording, "url(#g)  rgb(1, 2, 3)  ", &paint), 0);
	CHECK_LONG(paint.fallback_kind, SVG_PAINT_COLOR);
	CHECK(strcmp(seen, "rgb(1, 2, 3)") == 0);
	CHECK_COLOR(paint.fallback_color, 0xff00000cU, 0);

	/* A color the tree refuses is invalid. */
	CHECK_LONG(svg_parse_paint(&recording, "bad", &paint), EINVAL);
	CHECK_LONG(svg_parse_paint(&recording, "url(#g) bad", &paint), EINVAL);

	/* Without a tree, the keywords still work and a color is refused. */
	CHECK_LONG(svg_parse_paint(NULL, "none", &paint), 0);
	CHECK_LONG(paint.kind, SVG_PAINT_NONE);
	CHECK_LONG(svg_parse_paint(NULL, "url(#g) currentColor", &paint), 0);
	CHECK_LONG(paint.fallback_kind, SVG_PAINT_CURRENT_COLOR);
	CHECK_LONG(svg_parse_paint(NULL, "red", &paint), EINVAL);
	CHECK_LONG(svg_parse_paint(&tree, NULL, &paint), EINVAL);
	CHECK_LONG(svg_parse_paint(&tree, "none", NULL), EINVAL);
}

/* The pieces random text is made of: tokens of every grammar here, and some that are nearly right. */
static const char *const fuzz_tokens[] = {
	"0", "1", "-5.5", "+.5", "1e3", "1e", "1e-", "99999999999999999999", "3e38", "1e39", "1e-50", ".", "-", "+",
	" ", "  ", "\t", "\n", ",", ", ", "px", "PX", "%", "em", "ex", "rem", "in", "cm", "mm", "Q", "pt", "pc", "deg",
	"grad", "rad", "turn", "foo", "translate", "scale", "rotate", "skewX", "skewY", "matrix", "(", ")", "url(", "url",
	"#", "#id", "'", "\"", "none", "currentColor", "red", "#f00", "context-fill", "meet", "slice", "xMidYMid",
	"xMinYMax", "defer", "A", "z", "_", "\xc3\xa9", "\x80", "\xff"
};

/* Makes a random string of tokens in a block of exactly its size. */
static char *
fuzz_string(
	uint32_t *seed)
{
	char buffer[512];
	size_t token_count;
	size_t length;
	size_t token_length;
	size_t pieces;
	size_t index;
	const char *token;

	token_count = sizeof(fuzz_tokens) / sizeof(fuzz_tokens[0]);
	pieces = check_random(seed) % 14;
	length = 0;
	for (index = 0; index < pieces; index++) {
		token = fuzz_tokens[check_random(seed) % token_count];
		token_length = strlen(token);
		if (length + token_length >= sizeof(buffer))
			break;
		memcpy(buffer + length, token, token_length);
		length += token_length;
	}
	buffer[length] = '\0';
	return exact_copy(buffer);
}

/* Feeds random text to every parser; the sanitizers watch for reads past the NUL and for undefined arithmetic. */
static void
test_random(void)
{
	struct test_document document;
	struct svg_tree tree;
	struct svg_paint paint;
	struct svg_length lengths[16];
	struct svg_length length;
	struct svg_aspect aspect;
	struct svg_context context;
	struct vec_matrix matrix;
	struct vec_rect rect;
	const char *cursor;
	char id[SVG_ID_MAX + 1];
	float numbers[16];
	float value;
	char *text;
	size_t count;
	uint32_t seed;
	int iteration;
	int error;
	int finite;

	memset(&document, 0, sizeof(document));
	test_tree_make(&tree, &document);
	context.viewport_width = 300.0f;
	context.viewport_height = 150.0f;
	context.font_size = 16.0f;
	context.root_font_size = 16.0f;
	seed = 987654321;

	for (iteration = 0; iteration < 60000; iteration++) {
		text = fuzz_string(&seed);

		/* A number at the start of the text: the cursor stays inside the string. */
		cursor = text;
		error = svg_number_read(&cursor, &value);
		CHECK(cursor >= text && cursor <= text + strlen(text));
		if (error == 0) {
			CHECK(cursor > text);
			CHECK(vec_finite(value));
		}

		/* A whole-attribute number and length. */
		error = svg_parse_number(text, &value);
		if (error == 0)
			CHECK(vec_finite(value));
		error = svg_parse_length(text, &length);
		if (error == 0)
			CHECK(vec_finite(svg_length_resolve(&length, SVG_AXIS_OTHER, &context)));

		/* The lists keep a count within the capacity. */
		count = 99;
		error = svg_parse_numbers(text, numbers, 16, &count);
		CHECK(count <= 16);
		if (error == 0)
			CHECK(count <= 16);
		count = 99;
		error = svg_parse_lengths(text, lengths, 16, &count);
		CHECK(count <= 16);

		/* A transform is finite when it parses. */
		error = svg_parse_transform(text, &matrix);
		if (error == 0) {
			finite = vec_finite(matrix.a) && vec_finite(matrix.b) && vec_finite(matrix.c) &&
			    vec_finite(matrix.d) && vec_finite(matrix.e) && vec_finite(matrix.f);
			CHECK(finite);
		}

		/* A viewBox has a non-negative size when it parses. */
		error = svg_parse_view_box(text, &rect);
		if (error == 0) {
			CHECK(rect.right >= rect.left);
			CHECK(rect.bottom >= rect.top);
		}

		/* An aspect has one of the ten alignments. */
		error = svg_parse_aspect(text, &aspect);
		if (error == 0) {
			CHECK(aspect.align >= SVG_ALIGN_NONE && aspect.align <= SVG_ALIGN_XMAX_YMAX);
			CHECK(aspect.slice == 0 || aspect.slice == 1);
		}

		/* A reference keeps its id inside the buffer and ends it. */
		cursor = text;
		error = svg_url_read(&cursor, 0, id);
		if (error == 0)
			CHECK(strlen(id) <= SVG_ID_MAX);
		cursor = text;
		error = svg_url_read(&cursor, 1, id);
		if (error == 0)
			CHECK(strlen(id) <= SVG_ID_MAX);

		/* A paint through the test tree's colors. */
		error = svg_parse_paint(&tree, text, &paint);
		if (error == 0)
			CHECK(strlen(paint.id) <= SVG_ID_MAX);

		free(text);
	}
}

/* Prints a float with enough digits to read back exactly, for the round trip below. */
static void
print_exact(
	char *buffer,
	size_t size,
	float value)
{
	snprintf(buffer, size, "%.9g", (double)value);
}

/* Checks that numbers printed with 9 digits read back to the same float, over many random values. */
static void
test_round_trip(void)
{
	char buffer[64];
	uint32_t seed;
	uint32_t bits;
	float value;
	float read_back;
	int iteration;
	int error;

	seed = 31337;
	for (iteration = 0; iteration < 200000; iteration++) {
		/* A random bit pattern that is a finite float. */
		bits = check_random(&seed);
		memcpy(&value, &bits, sizeof(value));
		if (!vec_finite(value))
			continue;
		print_exact(buffer, sizeof(buffer), value);
		error = svg_parse_number(buffer, &read_back);
		CHECK_LONG(error, 0);

		/* The reading is within one unit in the last place of the float. */
		CHECK_NEAR(read_back, value, fabs((double)value) * 1.3e-7 + 1e-45);
	}
}

int
main(void)
{
	/* Runs every group of checks. */
	test_number_read();
	test_parse_number();
	test_length();
	test_length_resolve();
	test_numbers();
	test_lengths();
	test_transform();
	test_view_box();
	test_aspect();
	test_view_box_transform();
	test_url_read();
	test_paint();
	test_random();
	test_round_trip();

	/* Reports the result. */
	return check_finish("values");
}
