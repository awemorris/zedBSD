/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of process_itimer_tick() (cut out of src/kern/process.c by
 * run.sh): an interval timer counted down by the ticks that passed, one at
 * the busy rate and ten at the idle one.
 *  - a timer expires when the ticks reach what remained, and not before;
 *  - the ticks past the expiry come off the reloaded interval, so a late
 *    tick keeps the timer's phase, and a whole interval late is one expiry;
 *  - a one-shot timer ends at 0, an unarmed one stays unarmed;
 *  - with one tick at a time it matches the old count-down by one.
 */

#include <stdint.h>
#include <stdio.h>

#include "kern/atomic.h"

#define KERN_TEST_CHECKPOINT(point, argument) do { } while (0)

struct process {
	volatile unsigned itimer_sequence[3];
	volatile uint64_t itimer_remaining[3];
	volatile uint64_t itimer_interval[3];
};

#include "itimer-function.inc"

static unsigned failures;
static unsigned checks;

#define CHECK(condition, ...)						\
	do {								\
		checks++;						\
		if (!(condition)) {					\
			failures++;					\
			printf("FAIL %s:%d: ", __FILE__, __LINE__);	\
			printf(__VA_ARGS__);				\
			printf("\n");					\
		}							\
	} while (0)

static void
arm(struct process *process, uint64_t remaining, uint64_t interval)
{
	process->itimer_remaining[0] = remaining;
	process->itimer_interval[0] = interval;
}

int
main(void)
{
	struct process p = { { 0 }, { 0 }, { 0 } };
	uint64_t old_remaining;
	uint64_t ticks;
	unsigned old_expiries;
	unsigned new_expiries;
	int expired;

	/* 25 left, every 30: ticks of 10 expire at the third, 5 into the next interval. */
	arm(&p, 25U, 30U);
	CHECK(process_itimer_tick(&p, 0, 10U) == 0 && p.itimer_remaining[0] == 15U, "first 10");
	CHECK(process_itimer_tick(&p, 0, 10U) == 0 && p.itimer_remaining[0] == 5U, "second 10");
	expired = process_itimer_tick(&p, 0, 10U);
	CHECK(expired == 1 && p.itimer_remaining[0] == 25U, "third 10: %d left %llu", expired, (unsigned long long)p.itimer_remaining[0]);

	/* Exactly the remainder: expired, the full interval next. */
	arm(&p, 10U, 30U);
	CHECK(process_itimer_tick(&p, 0, 10U) == 1 && p.itimer_remaining[0] == 30U, "exact");

	/* A tick a whole interval late: one expiry, the phase kept. */
	arm(&p, 5U, 30U);
	CHECK(process_itimer_tick(&p, 0, 65U) == 1 && p.itimer_remaining[0] == 30U, "late by two intervals: %llu", (unsigned long long)p.itimer_remaining[0]);

	/* One-shot ends at 0; unarmed stays. */
	arm(&p, 3U, 0U);
	CHECK(process_itimer_tick(&p, 0, 10U) == 1 && p.itimer_remaining[0] == 0U, "one-shot");
	CHECK(process_itimer_tick(&p, 0, 10U) == 0 && p.itimer_remaining[0] == 0U, "unarmed");

	/* No ticks pass nothing. */
	arm(&p, 3U, 7U);
	CHECK(process_itimer_tick(&p, 0, 0U) == 0 && p.itimer_remaining[0] == 3U, "zero ticks");

	/* One tick at a time is the old count-down by one, over many periods. */
	arm(&p, 4U, 7U);
	old_remaining = 4U;
	old_expiries = 0U;
	new_expiries = 0U;
	for (ticks = 0; ticks < 1000U; ticks++) {
		if (old_remaining == 1U) {
			old_remaining = 7U;
			old_expiries++;
		} else {
			old_remaining--;
		}
		if (process_itimer_tick(&p, 0, 1U))
			new_expiries++;
		if (p.itimer_remaining[0] != old_remaining)
			break;
	}
	CHECK(ticks == 1000U && old_expiries == new_expiries, "by one: diverged at %llu", (unsigned long long)ticks);

	/* The sequence is even again after every call. */
	CHECK((p.itimer_sequence[0] & 1U) == 0U, "sequence");

	printf("itimer-host: %u checks, %u failures\n", checks, failures);
	if (failures != 0) {
		printf("itimer-host: FAIL\n");
		return 1;
	}
	printf("itimer-host: PASS\n");
	return 0;
}
