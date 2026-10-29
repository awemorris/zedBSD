/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the host unit tests of the software H.264 decoder: each
 * test counts its checks and failures and ends with one line,
 * "test-<name>: PASS" or "test-<name>: FAIL (<n> of <m>)".
 */

#ifndef H264SW_TEST_CHECK_H
#define H264SW_TEST_CHECK_H

#include <stdio.h>

/* The checks made so far and the ones that failed, for the summary line. */
static unsigned check_count;
static unsigned check_failures;

/*
 * Records one check; a failure prints the condition and where it is.
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
	printf("FAIL %s:%d: %s\n", file, line, text);
}

/* Checks a condition, recording the source text of the condition. */
#define CHECK(condition) check_report((condition) != 0, #condition, __FILE__, __LINE__)

/*
 * Prints the summary line and gives the exit status: 0 when every
 * check passed and at least one was made.
 */
static __inline int
check_finish(
	const char *name)
{
	/* A test that made no check proves nothing and fails. */
	if (check_count == 0U) {
		printf("test-%s: FAIL (no checks)\n", name);
		return 1;
	}

	/* Reports the failures out of the checks. */
	if (check_failures != 0U) {
		printf("test-%s: FAIL (%u of %u)\n", name, check_failures, check_count);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("test-%s: PASS (%u checks)\n", name, check_count);
	return 0;
}

#endif
