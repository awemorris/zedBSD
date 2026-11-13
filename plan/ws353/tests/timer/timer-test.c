/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the compositor's timers (userland/desktop/wayland/threads.c):
 * the heap's order under arms, moves and disarms, timers due at the same time,
 * a handler that arms its timer again, a full heap, and the timer thread's
 * wakes on a pipe (none while nothing is due, one byte for a deadline, a new
 * earliest deadline taken at once, one byte for many firings before a take,
 * the stop).
 */

#include "threads.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* How many timers the order checks arm. */
#define TEST_TIMERS	100U

/* A record of the firings a take made, in their order. */
struct test_log {
	struct kwl_timer *order[KWL_TIMERS_MAX];
	unsigned count;
	struct kwl_timers *timers;
	uint64_t again_ms;
};

/* The number of failed checks. */
static unsigned test_failures;

static void check(int condition, const char *what);
static void fired(void *context, struct kwl_timer *timer);
static void fired_again(void *context, struct kwl_timer *timer);
static unsigned pipe_bytes(int descriptor, int wait_ms);
static void test_order(void);
static void test_same_and_move(void);
static void test_again_and_full(void);
static void test_thread(void);

/*
 * Runs every check and says PASS or how many failed.
 */
int
main(
	void)
{
	/* The heap, then the thread. */
	test_order();
	test_same_and_move();
	test_again_and_full();
	test_thread();

	/* The verdict. */
	if (test_failures != 0U) {
		printf("timer-test: FAIL (%u)\n", test_failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("timer-test: PASS\n");
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

/* Records a firing in the take's log. */
static void
fired(
	void *context,
	struct kwl_timer *timer)
{
	struct test_log *log;

	/* The next entry of the log. */
	log = context;
	if (log->count < KWL_TIMERS_MAX)
		log->order[log->count] = timer;
	log->count++;
}

/* Records a firing and arms the timer again at again_ms (a time past fires at the next take). */
static void
fired_again(
	void *context,
	struct kwl_timer *timer)
{
	struct test_log *log;

	/* The firing, then the arm. */
	log = context;
	fired(context, timer);
	(void)kwl_timer_arm(log->timers, timer, log->again_ms);
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

/* Arms many timers in a scrambled order, moves and disarms some, and checks that they fire by time. */
static void
test_order(void)
{
	static struct kwl_timer timers_array[TEST_TIMERS];
	struct kwl_timers timers;
	struct test_log log;
	unsigned index;
	unsigned in_order;
	int error;

	/* Readied timers, no thread. */
	error = kwl_timers_init(&timers);
	check(error == 0, "order: init");
	memset(&log, 0, sizeof(log));

	/* Each at a scrambled time (37 is prime to 100, so every time is used once). */
	for (index = 0U; index < TEST_TIMERS; index++) {
		kwl_timer_init(&timers_array[index], "t", fired);
		error = kwl_timer_arm(&timers, &timers_array[index], 1000U + (uint64_t)((index * 37U) % TEST_TIMERS) * 10U);
		check(error == 0, "order: arm");
	}

	/* All armed, the earliest first. */
	check(timers.count == TEST_TIMERS, "order: count");
	check(kwl_timers_next(&timers) == 1000U, "order: earliest");

	/* Every seventh moved late, then every fifth disarmed. */
	for (index = 1U; index < TEST_TIMERS; index += 7U)
		(void)kwl_timer_arm(&timers, &timers_array[index], 5000U + index);
	for (index = 0U; index < TEST_TIMERS; index += 5U)
		kwl_timer_disarm(&timers, &timers_array[index]);

	/* A take before anything is due fires nothing. */
	check(kwl_timers_take(&timers, 999U, &log) == 0U, "order: nothing early");

	/* A take at the end fires the rest, in time order. */
	(void)kwl_timers_take(&timers, 10000U, &log);
	check(log.count == TEST_TIMERS - 20U, "order: fired the armed ones");
	in_order = 1U;
	for (index = 1U; index < log.count; index++) {
		if (log.order[index - 1U]->due_ms > log.order[index]->due_ms)
			in_order = 0U;
	}

	/* Earliest first, and nothing left. */
	check(in_order, "order: by time");
	check(timers.count == 0U, "order: heap empty");
	check(kwl_timers_next(&timers) == UINT64_MAX, "order: none next");

	/* No timer still says it is armed. */
	for (index = 0U; index < TEST_TIMERS; index++)
		check(!kwl_timer_armed(&timers_array[index]), "order: not armed after the take");
	kwl_timers_destroy(&timers);
}

/* Checks timers due at the same time, a new earliest, a move later and a disarm of the earliest. */
static void
test_same_and_move(void)
{
	struct kwl_timer first;
	struct kwl_timer second;
	struct kwl_timer third;
	struct kwl_timers timers;
	struct test_log log;
	int error;

	/* Three timers, two at the same time. */
	error = kwl_timers_init(&timers);
	check(error == 0, "same: init");
	memset(&log, 0, sizeof(log));
	kwl_timer_init(&first, "first", fired);
	kwl_timer_init(&second, "second", fired);
	kwl_timer_init(&third, "third", fired);
	(void)kwl_timer_arm(&timers, &first, 200U);
	(void)kwl_timer_arm(&timers, &second, 200U);
	(void)kwl_timer_arm(&timers, &third, 300U);
	check(timers.published == 200U, "same: published the earliest");

	/* A new earliest replaces the head, and is what the thread would be told. */
	(void)kwl_timer_arm(&timers, &third, 100U);
	check(kwl_timers_next(&timers) == 100U, "same: new head");
	check(timers.published == 100U, "same: new head published");

	/* The head disarmed: the next is the pair's time. */
	kwl_timer_disarm(&timers, &third);
	check(kwl_timers_next(&timers) == 200U, "same: head disarmed");
	check(timers.published == 200U, "same: next published");

	/* A disarm of one not armed changes nothing. */
	kwl_timer_disarm(&timers, &third);
	check(timers.count == 2U, "same: disarm twice");

	/* Both of the same time fire in one take. */
	(void)kwl_timers_take(&timers, 200U, &log);
	check(log.count == 2U, "same: both fired");
	check(timers.published == UINT64_MAX, "same: none published");

	/* A move later: it does not fire before its new time. */
	(void)kwl_timer_arm(&timers, &first, 400U);
	(void)kwl_timer_arm(&timers, &first, 600U);
	log.count = 0U;
	(void)kwl_timers_take(&timers, 500U, &log);
	check(log.count == 0U, "move: not before its new time");
	(void)kwl_timers_take(&timers, 600U, &log);
	check(log.count == 1U && log.order[0] == &first, "move: at its new time");
	kwl_timers_destroy(&timers);
}

/* Checks a handler that arms its timer for a time past, and a full heap. */
static void
test_again_and_full(void)
{
	static struct kwl_timer many[KWL_TIMERS_MAX + 1U];
	struct kwl_timer again;
	struct kwl_timers timers;
	struct test_log log;
	unsigned index;
	int error;

	/* A timer whose handler arms it again at a time already past. */
	error = kwl_timers_init(&timers);
	check(error == 0, "again: init");
	memset(&log, 0, sizeof(log));
	log.timers = &timers;
	log.again_ms = 50U;
	kwl_timer_init(&again, "again", fired_again);
	(void)kwl_timer_arm(&timers, &again, 100U);

	/* One take fires it once (not again and again), and leaves it armed for the next take. */
	(void)kwl_timers_take(&timers, 100U, &log);
	check(log.count == 1U, "again: once per take");
	check(kwl_timer_armed(&again), "again: armed again");
	check(timers.published == 50U, "again: the next take is due at once");
	(void)kwl_timers_take(&timers, 100U, &log);
	check(log.count == 2U, "again: the next take fires it");
	kwl_timer_disarm(&timers, &again);

	/* The heap takes KWL_TIMERS_MAX, and refuses one more. */
	for (index = 0U; index < KWL_TIMERS_MAX; index++) {
		kwl_timer_init(&many[index], "many", fired);
		error = kwl_timer_arm(&timers, &many[index], (uint64_t)index);
		check(error == 0, "full: arm");
	}

	/* One more does not fit. */
	kwl_timer_init(&many[KWL_TIMERS_MAX], "one more", fired);
	error = kwl_timer_arm(&timers, &many[KWL_TIMERS_MAX], 1U);
	check(error == ENOSPC, "full: refused");
	check(!kwl_timer_armed(&many[KWL_TIMERS_MAX]), "full: not armed");
	kwl_timers_destroy(&timers);
}

/* Checks the timer thread's wakes on a pipe. */
static void
test_thread(void)
{
	struct kwl_timer near;
	struct kwl_timer far;
	struct kwl_timers timers;
	struct test_log log;
	uint64_t now;
	uint64_t started;
	unsigned bytes;
	int descriptors[2];
	int error;

	/* The pipe the thread writes, read without waiting. */
	error = pipe(descriptors);
	check(error == 0, "thread: pipe");
	(void)fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(descriptors[1], F_SETFL, O_NONBLOCK);

	/* Timers and their thread, nothing armed: no byte. */
	error = kwl_timers_init(&timers);
	check(error == 0, "thread: init");
	memset(&log, 0, sizeof(log));
	kwl_timer_init(&near, "near", fired);
	kwl_timer_init(&far, "far", fired);
	error = kwl_timers_start(&timers, descriptors[1]);
	check(error == 0, "thread: start");
	bytes = pipe_bytes(descriptors[0], 300);
	check(bytes == 0U, "thread: no wake while nothing is armed");

	/* A far deadline, then a near one replaces it as the earliest: the near one wakes at its time. */
	now = kwl_timers_now();
	(void)kwl_timer_arm(&timers, &far, now + 5000U);
	(void)kwl_timer_arm(&timers, &near, now + 100U);
	started = kwl_timers_now();
	bytes = pipe_bytes(descriptors[0], 2000);
	now = kwl_timers_now();
	check(bytes == 1U, "thread: one byte for the near deadline");
	check(now >= started + 99U, "thread: not before the deadline");
	check(now < started + 1000U, "thread: soon after the deadline");

	/* Until the take, no more bytes (the thread waits for the next deadline from the take). */
	bytes = pipe_bytes(descriptors[0], 200);
	check(bytes == 0U, "thread: no more bytes before the take");
	(void)kwl_timers_take(&timers, now, &log);
	check(log.count == 1U && log.order[0] == &near, "thread: the take fires the near one");

	/* The far one stays: nothing in the next 300 ms. */
	bytes = pipe_bytes(descriptors[0], 300);
	check(bytes == 0U, "thread: the far one is not due");

	/* Two deadlines past before one take: one byte for both, and one take fires both. */
	now = kwl_timers_now();
	(void)kwl_timer_arm(&timers, &near, now + 20U);
	(void)kwl_timer_arm(&timers, &far, now + 40U);
	bytes = pipe_bytes(descriptors[0], 2000);
	check(bytes == 1U, "thread: one byte");
	bytes = pipe_bytes(descriptors[0], 100);
	check(bytes == 0U, "thread: one byte for both deadlines");
	log.count = 0U;
	(void)kwl_timers_take(&timers, kwl_timers_now(), &log);
	check(log.count == 2U, "thread: both fired by one take");

	/* An armed timer disarmed before its time: no byte. */
	now = kwl_timers_now();
	(void)kwl_timer_arm(&timers, &near, now + 100U);
	kwl_timer_disarm(&timers, &near);
	bytes = pipe_bytes(descriptors[0], 300);
	check(bytes == 0U, "thread: disarmed before its time");

	/* The stop ends the thread with a deadline still armed; its wakes were the two above. */
	(void)kwl_timer_arm(&timers, &far, kwl_timers_now() + 60000U);
	kwl_timers_stop(&timers);
	check(!timers.running, "thread: stopped");
	check(timers.wakes == 2U, "thread: two wakes");
	kwl_timers_destroy(&timers);
	(void)close(descriptors[0]);
	(void)close(descriptors[1]);
}
