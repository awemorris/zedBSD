/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of sessiond's rules for a session that crashed
 * (fallback-rules.c): what a crash is, and when the same user's session
 * starts again or the login screen comes.
 */

#include "userland/desktop/sessiond/fallback-rules.h"

#include <stdio.h>
#include <sys/wait.h>

/* The checks that failed. */
static int failures;

static void check(int condition, const char *what);

int
main(void)
{
	struct sessiond_crashes crashes;
	long long now;

	/* A crash: a signal, or an exit with an error, without a Log Out. */
	check(sessiond_session_crashed(9, 0, 0), "SIGKILL is a crash");
	check(sessiond_session_crashed(11, 0, 0), "SIGSEGV is a crash");
	check(sessiond_session_crashed(1 << 8, 0, 0), "exit 1 is a crash");
	check(!sessiond_session_crashed(0, 0, 0), "exit 0 is the session's own end");
	check(!sessiond_session_crashed(9, 1, 0), "a Log Out is not a crash");
	check(!sessiond_session_crashed(15, 0, 1), "an end sessiond asked for is not a crash");
	check(!sessiond_session_crashed(1 << 8, 1, 0), "an error after the Log Out is not a crash");

	/* Two crashes start the session again; the third within the window brings the login screen. */
	sessiond_crashes_clear(&crashes);
	now = 1000000LL;
	check(sessiond_restart_due(&crashes, now), "the first crash starts again");
	check(sessiond_restart_due(&crashes, now + 5000LL), "the second crash starts again");
	check(!sessiond_restart_due(&crashes, now + 10000LL), "the third crash in the window: the login screen");
	check(crashes.count == 0U, "the crashes forgotten at the login screen");
	check(sessiond_restart_due(&crashes, now + 11000LL), "after the login screen the count starts again");

	/* Crashes far apart start again each time. */
	sessiond_crashes_clear(&crashes);
	check(sessiond_restart_due(&crashes, now), "far apart: first");
	check(sessiond_restart_due(&crashes, now + SESSIOND_RESTART_WINDOW_MS), "far apart: second, the first out of the window");
	check(sessiond_restart_due(&crashes, now + 2LL * SESSIOND_RESTART_WINDOW_MS), "far apart: third");
	check(sessiond_restart_due(&crashes, now + 3LL * SESSIOND_RESTART_WINDOW_MS), "far apart: fourth");
	check(crashes.count == 1U, "far apart: one in the window");

	/* Two old and one new: only the new one counts, then two more in the window reach the bound. */
	sessiond_crashes_clear(&crashes);
	check(sessiond_restart_due(&crashes, now), "old one");
	check(sessiond_restart_due(&crashes, now + 1000LL), "old two");
	check(sessiond_restart_due(&crashes, now + SESSIOND_RESTART_WINDOW_MS + 2000LL), "new one after the window");
	check(sessiond_restart_due(&crashes, now + SESSIOND_RESTART_WINDOW_MS + 3000LL), "new two");
	check(!sessiond_restart_due(&crashes, now + SESSIOND_RESTART_WINDOW_MS + 4000LL), "new three: the login screen");

	/* The result. */
	if (failures != 0) {
		printf("restart-rules-host-test: %d failures\n", failures);
		return 1;
	}
	printf("restart-rules-host-test: PASS\n");
	return 0;
}

/* Notes a check that failed. */
static void
check(
	int condition,
	const char *what)
{
	if (!condition) {
		printf("FAIL %s\n", what);
		failures++;
	}
}
