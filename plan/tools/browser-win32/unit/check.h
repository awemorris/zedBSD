/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the Windows port's unit tests: each test counts its checks and
 * failures and ends with one line, "test-<name>: PASS (<n> checks)" or
 * "test-<name>: FAIL (<n> of <m>)".  The same file builds on the host and
 * with llvm-mingw for Windows.  CHECK and CHECK_EQ may be used from any
 * thread; check_finish is called once, by the main thread, after the
 * other threads were joined.  The counters' lock is a spin lock of atomic
 * operations, so a test needs no thread library of the system's (on
 * Windows no POSIX thread layer is linked).
 */

#ifndef BROWSER_WIN32_TEST_CHECK_H
#define BROWSER_WIN32_TEST_CHECK_H

#include <stdio.h>

/* The checks made so far and the ones that failed, for the summary line (under check_lock). */
static unsigned check_count;
static unsigned check_failures;

/*
 * The lock of the counters and the output: a test may check from several
 * threads.  0 free, 1 held; read and written only with atomic operations.
 */
static int check_lock;

/* Takes the lock of the counters, spinning while another thread holds it (a check is short). */
static __inline void
check_lock_take(void)
{
	int held;

	/* Spins until this thread turned the lock from free to held. */
	for (;;) {
		held = __atomic_exchange_n(&check_lock, 1, __ATOMIC_ACQUIRE);
		if (held == 0)
			break;
	}
}

/* Gives back the lock of the counters. */
static __inline void
check_lock_give(void)
{
	/* Publishes the counters to the next thread that takes the lock. */
	__atomic_store_n(&check_lock, 0, __ATOMIC_RELEASE);
}

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
	check_lock_take();

	/* Every check counts; a failure names the condition so the log alone says what broke. */
	check_count++;
	if (!passed) {
		check_failures++;
		printf("FAIL %s:%d: %s\n", file, line, text);
		fflush(stdout);
	}

	check_lock_give();
}

/* Checks a condition, recording the source text of the condition. */
#define CHECK(condition) check_report((condition) != 0, #condition, __FILE__, __LINE__)

/* Checks that an int result equals the expected one, printing both on a failure. */
#define CHECK_EQ(actual, expected) check_equal((long long)(actual), (long long)(expected), #actual, __FILE__, __LINE__)

/*
 * Records one comparison of integers; a failure prints both values.
 */
static __inline void
check_equal(
	long long actual,
	long long expected,
	const char *text,
	const char *file,
	int line)
{
	check_lock_take();

	/* Every check counts; a failure names the expression and both values. */
	check_count++;
	if (actual != expected) {
		check_failures++;
		printf("FAIL %s:%d: %s is %lld, expected %lld\n", file, line, text, actual, expected);
		fflush(stdout);
	}

	check_lock_give();
}

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
