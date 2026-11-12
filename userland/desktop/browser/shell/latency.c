/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The diagnostic clock of the window's thread (a diagnostic interface, on
 * when the environment has BROWSER_LATENCY): how long the window's thread
 * leaves an event of the application waiting.  A thread of its own
 * ("browser-latency") posts a tick to the application (SHELL_POST_TICK,
 * kl_app_post) every few milliseconds, each once the last was taken, and
 * the window's thread times how long each waited from its post to its
 * taking (what an input of the compositor would wait).  When the window
 * closes one line says how many were timed, the longest wait, and how
 * many waited more than 20 and more than 100 milliseconds; each wait over
 * 100 milliseconds has a line of its own as it is taken (with the seconds
 * since the clock started):
 *
 *   ZBROWSER LATENCY long waited_ms=X at_s=S
 *   ZBROWSER LATENCY samples=N longest_ms=X over_20ms=A over_100ms=B
 */

#include "shell/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __GCC_ATOMIC_LLONG_LOCK_FREE != 2
/*
 * Where an 8-byte atomic is not lock-free (i386), the C library's runtime
 * serializes the probe's sending time under a lock.  Clang notes that such an
 * operation is slower than a lock-free one, which that target accepts.  The
 * time stays within this process.
 */
#pragma clang diagnostic ignored "-Watomic-alignment"
#endif

/* The name of the clock's thread. */
#define LATENCY_THREAD_NAME	"browser-latency"

/* How often the clock looks whether to post a tick, in milliseconds. */
#define LATENCY_PERIOD_MS	5U

/* The waits counted apart, in microseconds. */
#define LATENCY_LONG_US		20000U
#define LATENCY_LONGER_US	100000U

static void latency_run(void *argument);

/*
 * Starts the clock when the environment asks for it (BROWSER_LATENCY set
 * and not empty), posting to the application.  Returns 0 (also when it is
 * not asked for), or the error of the thread's start.
 */
int
shell_latency_start(
	struct shell_latency *latency,
	struct kl_app *app)
{
	struct kl_thread_options options;
	const char *asked;
	int error;

	/* Nothing runs unless asked for. */
	memset(latency, 0, sizeof(*latency));
	asked = getenv("BROWSER_LATENCY");
	if (asked == NULL || asked[0] == '\0')
		return 0;

	/* The clock's thread, from now. */
	latency->app = app;
	latency->started_us = kl_clock_us();
	memset(&options, 0, sizeof(options));
	options.name = LATENCY_THREAD_NAME;
	options.run = latency_run;
	options.argument = latency;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	error = kl_thread_start(&options, &latency->thread);
	if (error != 0) {
		latency->thread = NULL;
		return error;
	}

	/* Succeeded: the window's thread times the ticks from now on. */
	return 0;
}

/*
 * Times a tick the window's thread took (SHELL_POST_TICK), on the window's
 * thread.
 */
void
shell_latency_take(
	struct shell_latency *latency)
{
	uint64_t sent;
	uint64_t now;
	uint64_t waited;

	/* How long the tick waited. */
	now = kl_clock_us();
	sent = __atomic_load_n(&latency->sent_us, __ATOMIC_ACQUIRE);
	waited = 0;
	if (now > sent)
		waited = now - sent;

	/* Counted. */
	latency->samples++;
	if (waited > latency->longest_us)
		latency->longest_us = waited;
	if (waited > LATENCY_LONG_US)
		latency->over_20ms++;
	if (waited > LATENCY_LONGER_US) {
		latency->over_100ms++;
		printf("ZBROWSER LATENCY long waited_ms=%.1f at_s=%.1f\n", (double)waited / 1000.0,
		    (double)(now - latency->started_us) / 1e6);
		fflush(stdout);
	}

	/* The clock may post the next tick. */
	__atomic_store_n(&latency->waiting, 0, __ATOMIC_RELEASE);
}

/*
 * Stops the clock and says what it measured in one line, on the window's
 * thread.  Nothing when it did not run.
 */
void
shell_latency_stop(
	struct shell_latency *latency)
{
	/* Not running. */
	if (latency->thread == NULL)
		return;

	/* The thread, ended. */
	__atomic_store_n(&latency->stop, 1, __ATOMIC_RELEASE);
	(void)kl_thread_join(latency->thread);
	latency->thread = NULL;

	/* The line the tests read. */
	printf("ZBROWSER LATENCY samples=%lu longest_ms=%.1f over_20ms=%lu over_100ms=%lu\n", latency->samples,
	    (double)latency->longest_us / 1000.0, latency->over_20ms, latency->over_100ms);
	fflush(stdout);
}

/* The clock's thread: a tick posted whenever the last was taken, until the stop. */
static void
latency_run(
	void *argument)
{
	struct shell_latency *latency;
	uint64_t now;
	int waiting;
	int stop;
	int error;

	/* Looks every few milliseconds. */
	latency = argument;
	for (;;) {
		stop = __atomic_load_n(&latency->stop, __ATOMIC_ACQUIRE);
		if (stop)
			break;

		/* A tick, when the last was taken: its time first, then the post. */
		waiting = __atomic_load_n(&latency->waiting, __ATOMIC_ACQUIRE);
		if (!waiting) {
			now = kl_clock_us();
			__atomic_store_n(&latency->sent_us, now, __ATOMIC_RELEASE);
			__atomic_store_n(&latency->waiting, 1, __ATOMIC_RELEASE);
			error = kl_app_post(latency->app, SHELL_POST_TICK, NULL);
			if (error != 0)
				__atomic_store_n(&latency->waiting, 0, __ATOMIC_RELEASE);
		}

		/* Until the next look. */
		kl_thread_sleep(LATENCY_PERIOD_MS);
	}
}
