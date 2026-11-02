/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of libwebp-compat's host unit tests: each test counts its
 * checks and failures and ends with one line, "test-<name>: PASS (<n>
 * checks)" or "test-<name>: FAIL (<n> of <m>)".
 */

#ifndef WEBPC_TEST_CHECK_H
#define WEBPC_TEST_CHECK_H

#include <stdio.h>

/* The checks made so far and the ones that failed, for the summary line. */
static unsigned long check_count;
static unsigned long check_failures;

/*
 * Records one check; a failure prints the condition and where it is (the
 * first 50 failures, so a broken loop does not flood the log).
 */
static __inline void
check_report(
	int passed,
	const char *text,
	const char *file,
	int line)
{
	/* Every check counts, whether it passes or not. */
	check_count++;
	if (passed)
		return;

	/* A failure names the condition so the log alone says what broke. */
	check_failures++;
	if (check_failures <= 50UL)
		printf("FAIL %s:%d: %s\n", file, line, text);
}

/* Checks a condition, recording the source text of the condition. */
#define CHECK(condition) check_report((condition) != 0, #condition, __FILE__, __LINE__)

/*
 * Prints the summary line and gives the exit status: 0 when every check
 * passed and at least one was made.
 */
static __inline int
check_finish(
	const char *name)
{
	/* A test that made no check proves nothing and fails. */
	if (check_count == 0UL) {
		printf("test-%s: FAIL (no checks)\n", name);
		return 1;
	}

	/* Reports the failures out of the checks. */
	if (check_failures != 0UL) {
		printf("test-%s: FAIL (%lu of %lu)\n", name, check_failures, check_count);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("test-%s: PASS (%lu checks)\n", name, check_count);
	return 0;
}

/*
 * A small deterministic generator for the random inputs of the tests
 * (xorshift32; never 0 once seeded with a non-zero value).
 */
static __inline unsigned long
check_random(
	unsigned long *state)
{
	unsigned long x;

	/* One xorshift step, kept to 32 bits. */
	x = *state & 0xffffffffUL;
	x ^= (x << 13) & 0xffffffffUL;
	x ^= x >> 17;
	x ^= (x << 5) & 0xffffffffUL;
	*state = x;
	return x;
}

#endif
