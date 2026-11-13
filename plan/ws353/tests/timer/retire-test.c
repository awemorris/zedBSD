/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the pace of the gone clients' releases (objects.c's
 * kwl_retire_due and kwl_retire_pace, with threads.c's timers and their
 * thread): while the list waits and no release may go, the event loop is
 * not woken (no byte on the wake pipe), neither by the loop itself nor by
 * the timer; when the time the list waits for comes, the timer wakes it
 * once; when a release may go at once after a pass's release, the loop
 * wakes itself once; a frame in flight is waited for without the clock.
 */

#include "kwl.h"
#include "compose.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The number of failed checks. */
static unsigned test_failures;

/* How often objects.c fell back to the event loop's own deadline (retire-stubs.c). */
extern unsigned retire_stub_due_calls;

static void check(int condition, const char *what);
static unsigned pipe_bytes(int descriptor, int wait_ms);
static void list_add(struct kwl_server *server, unsigned count);
static void pass_end(struct kwl_server *server);

/*
 * Runs the pace's checks with a server of its own and says PASS or how
 * many failed.
 */
int
main(
	void)
{
	struct kwl_server *server;
	struct kwl_compose *compose;
	uint64_t now;
	uint64_t started;
	unsigned bytes;
	int descriptors[2];
	int error;

	/* A server with a compose (nothing in flight), the wake pipe, the timers and their thread. */
	server = calloc(1U, sizeof(*server));
	compose = calloc(1U, sizeof(*compose));
	if (server == NULL || compose == NULL) {
		printf("retire-test: FAIL (memory)\n");
		return 1;
	}
	server->compose = compose;
	error = pipe(descriptors);
	check(error == 0, "pipe");
	(void)fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(descriptors[1], F_SETFL, O_NONBLOCK);
	server->wake[0] = descriptors[0];
	server->wake[1] = descriptors[1];
	error = kwl_timers_init(&server->timers);
	check(error == 0, "timers init");
	kwl_retire_init(server);
	error = kwl_timers_start(&server->timers, server->wake[1]);
	check(error == 0, "timers start");

	/* Nothing waits: no wake, nothing armed. */
	pass_end(server);
	check(!kwl_timer_armed(&server->retire_timer), "empty: not armed");
	bytes = pipe_bytes(server->wake[0], 100);
	check(bytes == 0U, "empty: no wake");

	/*
	 * Three wait, and the user typed just now: none may go until the input's
	 * quiet (200 ms).  The pass does not wake itself, and the timer wakes
	 * the loop once, at the quiet's end.
	 */
	now = kwl_milliseconds();
	server->lock_input_ms = now;
	list_add(server, 3U);
	started = kwl_milliseconds();
	pass_end(server);
	check(kwl_timer_armed(&server->retire_timer), "quiet: armed");
	check(server->retire_timer.due_ms == now + 200U, "quiet: due at the quiet's end");
	bytes = pipe_bytes(server->wake[0], 120);
	check(bytes == 0U, "quiet: no wake before the quiet's end");
	bytes = pipe_bytes(server->wake[0], 2000);
	now = kwl_milliseconds();
	check(bytes == 1U, "quiet: one wake from the timer");
	check(now >= started + 190U, "quiet: not before the quiet's end");

	/*
	 * The timer's pass: one released, and as the next may go at once (a
	 * release takes no time here) the loop wakes itself once.
	 */
	(void)kwl_timers_take(&server->timers, now, server);
	pass_end(server);
	check(server->retiring_count == 2U, "pass: one released");
	check(!kwl_timer_armed(&server->retire_timer), "pass: not armed when one may go at once");
	bytes = pipe_bytes(server->wake[0], 100);
	check(bytes == 1U, "pass: the loop wakes itself once");

	/*
	 * A frame goes in flight: none may go, whatever the time, and nothing
	 * wakes the loop (not even the list's limit of a second) until the
	 * frame's completion, which is an event of its own.
	 */
	compose->in_flight = 1U;
	pass_end(server);
	check(server->retiring_count == 2U, "flight: none released");
	check(!kwl_timer_armed(&server->retire_timer), "flight: not armed");
	bytes = pipe_bytes(server->wake[0], 1300);
	check(bytes == 0U, "flight: no wake while the frame flies");

	/* The completion's pass: the next may go, the loop wakes itself once more. */
	compose->in_flight = 0U;
	pass_end(server);
	check(server->retiring_count == 1U, "completed: one released");
	bytes = pipe_bytes(server->wake[0], 100);
	check(bytes == 1U, "completed: one wake");

	/*
	 * Something is to be drawn and the user typed: the release waits for
	 * the list's limit (a second after the last release), on the timer.
	 */
	now = kwl_milliseconds();
	server->dirty = 1U;
	server->lock_input_ms = now;
	server->retire_next_ms = now;
	server->retire_last_ms = now;
	pass_end(server);
	check(server->retiring_count == 1U, "dirty: none released");
	check(kwl_timer_armed(&server->retire_timer), "dirty: armed");
	check(server->retire_timer.due_ms == now + 1000U, "dirty: due at the list's limit");
	bytes = pipe_bytes(server->wake[0], 700);
	check(bytes == 0U, "dirty: no wake before the limit");
	bytes = pipe_bytes(server->wake[0], 2000);
	check(bytes == 1U, "dirty: one wake at the limit");

	/* The limit's pass: the last one released, nothing waits, nothing armed, no wake. */
	(void)kwl_timers_take(&server->timers, kwl_milliseconds(), server);
	pass_end(server);
	check(server->retiring == NULL, "drained: the list is empty");
	check(!kwl_timer_armed(&server->retire_timer), "drained: not armed");
	bytes = pipe_bytes(server->wake[0], 300);
	check(bytes == 0U, "drained: no wake");
	check(retire_stub_due_calls == 0U, "no fallback to the loop's deadline");

	/* The thread ends; it woke the loop twice (the quiet and the limit). */
	kwl_timers_stop(&server->timers);
	check(server->timers.wakes == 2U, "two timer wakes");
	kwl_timers_destroy(&server->timers);
	(void)close(descriptors[0]);
	(void)close(descriptors[1]);
	free(compose);
	free(server);

	/* The verdict. */
	if (test_failures != 0U) {
		printf("retire-test: FAIL (%u)\n", test_failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("retire-test: PASS\n");
	return 0;
}

/* Counts a failed check and names it. */
static void
check(
	int condition,
	const char *what)
{
	/* A held check says nothing. */
	if (condition)
		return;

	/* A failed one is named. */
	printf("FAIL %s\n", what);
	test_failures++;
}

/* Reads what a pipe holds after waiting up to wait_ms for its first byte, and reports how many bytes. */
static unsigned
pipe_bytes(
	int descriptor,
	int wait_ms)
{
	struct pollfd entry;
	unsigned char bytes[64];
	unsigned count;
	ssize_t got;
	int ready;

	/* The first byte, within the wait. */
	entry.fd = descriptor;
	entry.events = POLLIN;
	entry.revents = 0;
	ready = poll(&entry, 1, wait_ms);
	if (ready <= 0)
		return 0U;

	/* Every byte there now. */
	count = 0U;
	for (;;) {
		got = read(descriptor, bytes, sizeof(bytes));
		if (got <= 0)
			break;
		count += (unsigned)got;
	}

	/* Reports the bytes read. */
	return count;
}

/* Puts gone clients' buffers on the server's retiring list, as a client's teardown does. */
static void
list_add(
	struct kwl_server *server,
	unsigned count)
{
	struct kwl_object *object;
	unsigned index;

	/* Each a bare buffer record, oldest first. */
	for (index = 0U; index < count; index++) {
		object = calloc(1U, sizeof(*object));
		if (object == NULL)
			return;
		if (server->retiring_tail != NULL) {
			server->retiring_tail->retire_next = object;
		} else {
			server->retiring = object;
			server->retire_started_ms = kwl_milliseconds();
			server->retire_released = 0U;
			server->retire_last_ms = 0U;
			server->retire_next_ms = 0U;
		}
		server->retiring_tail = object;
		server->retiring_count++;
	}
}

/* Ends a pass of the event loop as main.c does: a release when one may go, then the pace. */
static void
pass_end(
	struct kwl_server *server)
{
	int ready;

	/* One release, when it may go now. */
	ready = kwl_retire_ready(server, kwl_milliseconds());
	if (ready)
		(void)kwl_retire_tick(server);

	/* The next one's pace. */
	kwl_retire_pace(server);
}
