/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the ssh unit tests.  Each failed check prints one line
 * "FAIL <file>:<line>: <what>" and counts; check_finish prints the last
 * line "test-<name>: PASS (<n> checks)" or "test-<name>: FAIL (<m> of <n>
 * checks)" and returns the exit status.  ANSI C; the functions are static
 * so that one test file includes this header once.
 */

#ifndef SSH_TEST_CHECK_H
#define SSH_TEST_CHECK_H

#include <stdio.h>
#include <string.h>

/* Keeps a helper a test does not call from being warned about. */
#if defined(__GNUC__)
#define CHECK_UNUSED __attribute__((unused))
#else
#define CHECK_UNUSED
#endif

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

	/* Reports a failure. */
	if (!condition) {
		check_failed++;
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

	/* Reports a failure with both values. */
	if (actual != expected) {
		check_failed++;
		printf("FAIL %s:%d: %s (got %ld, expected %ld)\n", file, line, text, actual, expected);
	}
}

/* Records a check that size bytes are equal, printing the first difference on a failure. */
CHECK_UNUSED static void
check_record_bytes(
	const void *actual,
	const void *expected,
	size_t size,
	const char *file,
	int line,
	const char *text)
{
	const unsigned char *left;
	const unsigned char *right;
	size_t index;

	left = actual;
	right = expected;

	/* Counts the check. */
	check_count++;

	/* Finds the first difference, if there is one. */
	for (index = 0; index < size; index++) {
		if (left[index] != right[index])
			break;
	}

	/* Reports a failure with the first differing byte. */
	if (index < size) {
		check_failed++;
		printf("FAIL %s:%d: %s (byte %lu: got %02x, expected %02x)\n", file, line, text,
		    (unsigned long)index, left[index], right[index]);
	}
}

/* Prints the last line for the test name and returns the exit status (0 when every check passed). */
CHECK_UNUSED static int
check_finish(
	const char *name)
{
	/* Reports the whole test. */
	if (check_failed == 0) {
		printf("test-%s: PASS (%lu checks)\n", name, check_count);
		return 0;
	}

	/* Reports the failures. */
	printf("test-%s: FAIL (%lu of %lu checks)\n", name, check_failed, check_count);
	return 1;
}

/* Checks a condition. */
#define CHECK(condition) check_record((condition) != 0, __FILE__, __LINE__, #condition)

/* Checks that two integer values are equal. */
#define CHECK_EQ(actual, expected) check_record_long((long)(actual), (long)(expected), __FILE__, __LINE__, #actual " == " #expected)

/* Checks that size bytes are equal. */
#define CHECK_BYTES(actual, expected, size) check_record_bytes((actual), (expected), (size), __FILE__, __LINE__, #actual " == " #expected)

#endif
