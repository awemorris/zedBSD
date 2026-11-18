/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * When the graphical login has failed to start and the text console's
 * login takes over (fallback-rules.c, ws239-p001; BUG-122 for the count),
 * and when a session that crashed starts again for its user.  Nothing
 * here reads a file or starts a program, so the host tests run it alone.
 */

#ifndef SESSIOND_FALLBACK_RULES_H
#define SESSIOND_FALLBACK_RULES_H

/* How many greeters in a row may fail before the console's login takes over (BUG-122: six, about half a minute). */
#define SESSIOND_GREETER_FAILURES	6U

/* A greeter that ends with an error within this many seconds of its start has failed. */
#define SESSIOND_GREETER_QUICK_SECONDS	20L

/* How long a greeter may take to say READY (to take the display) before it is ended as failed, in seconds. */
#define SESSIOND_GREETER_START_SECONDS	60L

/*
 * How long greeters failing in a row may keep the screen from the console,
 * in milliseconds, counted from the start of the first of them: two that
 * hang without READY (about two minutes) reach it, not six (six minutes).
 */
#define SESSIOND_FAILURE_STREAK_MS	90000LL

/*
 * A session that crashed is started again for the same user without the
 * login screen, unless it is the SESSIOND_RESTART_CRASHES-th crash within
 * SESSIOND_RESTART_WINDOW_MS: the login screen comes then, so that a
 * session that cannot run is not started again without end.
 */
#define SESSIOND_RESTART_CRASHES	3U
#define SESSIOND_RESTART_WINDOW_MS	120000LL

/* The times of the seat's last crashes (the monotonic milliseconds), the oldest first. */
struct sessiond_crashes {
	long long at_ms[SESSIOND_RESTART_CRASHES];
	unsigned count;
};

/* What the wait for the display does next (sessiond_display_next, ws270-p006a). */
enum sessiond_display_step {
	SESSIOND_DISPLAY_READY,		/* the display is there and no GPU device is attaching */
	SESSIOND_DISPLAY_NONE,		/* no display and no GPU device attaching: it will not come */
	SESSIOND_DISPLAY_WAIT,		/* a GPU device is attaching: look again after a moment */
	SESSIOND_DISPLAY_TIMEOUT	/* a GPU device is still attaching after the bound */
};

int sessiond_greeter_failed(int ready, int status, long seconds);
enum sessiond_display_step sessiond_display_next(int present, unsigned attaching, long long waited_ms, long long limit_ms);
int sessiond_console_due(unsigned failures, long long streak_ms);
int sessiond_session_crashed(int status, int logged_out, int stopping);
int sessiond_restart_due(struct sessiond_crashes *crashes, long long now_ms);
void sessiond_crashes_clear(struct sessiond_crashes *crashes);

#endif
