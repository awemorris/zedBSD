/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the recent list's stamp (ws177-p008,
 * userland/desktop/libkeiland/recent.c, KL_VERSION 66) in a data folder of
 * its own (XDG_DATA_HOME, given by the script): the stamp of a list not
 * written yet holds still, and changes when a file is added, when the
 * same file is added again, when the list is emptied, stopped and started
 * again.
 */

#include <keiland/keiland.h>

#include <stdio.h>

/* The number of failed checks. */
static int test_failures;

static void test_check(int condition, const char *what);

/*
 * Runs the steps.
 */
int
main(void)
{
	uint64_t before;
	uint64_t after;
	int error;

	/* A list not written yet: the stamp holds still. */
	error = kl_recent_stamp(&before);
	test_check(error == 0, "a stamp of no list");
	error = kl_recent_stamp(&after);
	test_check(error == 0 && after == before, "the stamp holds while nothing changes");

	/* A file added. */
	error = kl_recent_add("/tmp/a.txt", "test");
	(void)kl_recent_stamp(&after);
	test_check(error == 0 && after != before, "an addition changes the stamp");

	/* The same file again (moved to the newest place). */
	before = after;
	error = kl_recent_add("/tmp/b.txt", "test");
	(void)kl_recent_stamp(&after);
	test_check(error == 0 && after != before, "a second file changes the stamp");

	/* Emptied. */
	before = after;
	error = kl_recent_clear();
	(void)kl_recent_stamp(&after);
	test_check(error == 0 && after != before, "emptying changes the stamp");

	/* Stopped, then started again. */
	before = after;
	error = kl_recent_set_keep(0);
	(void)kl_recent_stamp(&after);
	test_check(error == 0 && after != before, "stopping changes the stamp");
	before = after;
	error = kl_recent_set_keep(1);
	(void)kl_recent_stamp(&after);
	test_check(error == 0 && after != before, "starting again changes the stamp");

	/* The verdict. */
	if (test_failures != 0) {
		printf("host-recent-stamp: %d FAILED\n", test_failures);
		return 1;
	}

	/* Succeeded: every step held. */
	printf("host-recent-stamp: PASS\n");
	return 0;
}

/* Counts and names a failed check. */
static void
test_check(
	int condition,
	const char *what)
{
	/* A failure is named. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		test_failures++;
	}
}
