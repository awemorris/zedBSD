/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the vector and SVG unit tests.  Each failed check prints
 * one line "FAIL <file>:<line>: <what>" (at most CHECK_PRINT_MAX of them;
 * the rest are counted) and counts; check_finish prints the last line
 * "test-<name>: PASS (<n> checks)" or "test-<name>: FAIL (<m> of <n>
 * checks)" and returns the exit status.  The functions are static, so one
 * test file includes this header once.
 */

#ifndef WS328_TEST_CHECK_H
#define WS328_TEST_CHECK_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Keeps a helper a test does not call from being warned about. */
#if defined(__GNUC__)
#define CHECK_UNUSED __attribute__((unused))
#else
#define CHECK_UNUSED
#endif

/* The most failure lines printed (a loop over a million pixels must not flood the log). */
#define CHECK_PRINT_MAX	200UL

/* The number of checks made and failed so far in this test program. */
static unsigned long check_count;
static unsigned long check_failed;

/* Records one check of a condition; text is the condition as written. */
CHECK_UNUSED static void
check_record(
	int condition,
	const char *file,
	int line,
	const char *text)
{
	/* Counts the check. */
	check_count++;

	/* Reports a failure, while there is room in the log. */
	if (!condition) {
		check_failed++;
		if (check_failed <= CHECK_PRINT_MAX)
			printf("FAIL %s:%d: %s\n", file, line, text);
	}
}

/* Records a check that two integers are equal, printing both on a failure. */
CHECK_UNUSED static void
check_record_long(
	long actual,
	long expected,
	const char *file,
	int line,
	const char *text)
{
	/* Counts the check. */
	check_count++;

	/* Reports a failure with the two values. */
	if (actual != expected) {
		check_failed++;
		if (check_failed <= CHECK_PRINT_MAX)
			printf("FAIL %s:%d: %s: got %ld, expected %ld\n", file, line, text, actual, expected);
	}
}

/*
 * Records a check that a float is within a tolerance of the expected
 * value (|actual - expected| <= tolerance; a NaN never is).
 */
CHECK_UNUSED static void
check_record_near(
	double actual,
	double expected,
	double tolerance,
	const char *file,
	int line,
	const char *text)
{
	double difference;
	int close;

	/* Counts the check. */
	check_count++;

	/* Tells whether the values are close (written so that a NaN fails). */
	difference = actual - expected;
	if (difference < 0.0)
		difference = -difference;
	close = difference <= tolerance;

	/* Reports a failure with the two values. */
	if (!close) {
		check_failed++;
		if (check_failed <= CHECK_PRINT_MAX)
			printf("FAIL %s:%d: %s: got %.9g, expected %.9g (tolerance %.3g)\n", file, line, text, actual, expected, tolerance);
	}
}

/*
 * Records a check that two premultiplied or straight 0xAARRGGBB colors
 * differ by at most tolerance in every channel.
 */
CHECK_UNUSED static void
check_record_color(
	uint32_t actual,
	uint32_t expected,
	int tolerance,
	const char *file,
	int line,
	const char *text)
{
	int channel;
	int a;
	int e;
	int close;

	/* Counts the check. */
	check_count++;

	/* Compares the four channels. */
	close = 1;
	for (channel = 0; channel < 4; channel++) {
		a = (int)((actual >> (channel * 8)) & 0xffU);
		e = (int)((expected >> (channel * 8)) & 0xffU);
		if (a - e > tolerance || e - a > tolerance)
			close = 0;
	}

	/* Reports a failure with the two colors. */
	if (!close) {
		check_failed++;
		if (check_failed <= CHECK_PRINT_MAX)
			printf("FAIL %s:%d: %s: got %08lx, expected %08lx (tolerance %d)\n", file, line, text,
			    (unsigned long)actual, (unsigned long)expected, tolerance);
	}
}

/* Prints the last line and gives the exit status. */
CHECK_UNUSED static int
check_finish(
	const char *name)
{
	/* Reports a failed run. */
	if (check_failed != 0) {
		printf("test-%s: FAIL (%lu of %lu checks)\n", name, check_failed, check_count);
		return 1;
	}

	/* Reports a run with every check passed. */
	printf("test-%s: PASS (%lu checks)\n", name, check_count);
	return 0;
}

/* The checks as statements. */
#define CHECK(condition)		check_record((condition) != 0, __FILE__, __LINE__, #condition)
#define CHECK_LONG(actual, expected)	check_record_long((long)(actual), (long)(expected), __FILE__, __LINE__, #actual)
#define CHECK_NEAR(actual, expected, tolerance) \
	check_record_near((double)(actual), (double)(expected), (double)(tolerance), __FILE__, __LINE__, #actual)
#define CHECK_COLOR(actual, expected, tolerance) \
	check_record_color((uint32_t)(actual), (uint32_t)(expected), (tolerance), __FILE__, __LINE__, #actual)

/*
 * A small deterministic pseudo-random generator for the randomized tests
 * (xorshift32 from Marsaglia, "Xorshift RNGs", 2003): the same seed gives
 * the same sequence on every host.  The state must not be 0.
 */
CHECK_UNUSED static uint32_t
check_random(
	uint32_t *state)
{
	uint32_t x;

	/* Advances the state by the three shifts. */
	x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;

	/* Reports the new state as the number. */
	return x;
}

/* A pseudo-random float in [low, high). */
CHECK_UNUSED static float
check_random_float(
	uint32_t *state,
	float low,
	float high)
{
	uint32_t bits;
	float unit;

	/* Takes 24 bits as a fraction in [0, 1). */
	bits = check_random(state) >> 8;
	unit = (float)bits / 16777216.0f;

	/* Reports the value scaled into the range. */
	return low + (high - low) * unit;
}

#endif
