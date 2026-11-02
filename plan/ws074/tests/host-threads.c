/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host unit tests' threads: every host-NAME test is linked with this
 * file, which gives the engine the POSIX threads before main runs (as the
 * browser's main does first), so that a test that makes an instance, a
 * view or a page needs no call of its own.  The browser program is not
 * linked with it (its main gives the threads itself).
 */

#include "userland/tests/browser-threads/threads-posix.h"

#include <stdio.h>
#include <stdlib.h>

static void host_threads_set(void) __attribute__((constructor));

/* Gives the engine the POSIX threads before main; a failure ends the test. */
static void
host_threads_set(void)
{
	int error;

	/* The table. */
	error = browser_threads_set(browser_threads_posix());
	if (error != 0) {
		fprintf(stderr, "host-threads: the engine refused the threads (%d)\n", error);
		exit(2);
	}
}
