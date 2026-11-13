/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clock as a program sees it, for the variable tick's tests.
 *
 *   clocktool res              clock_getres() of both clocks, and the
 *                              smallest and the median nonzero step
 *                              between two clock_gettime() calls
 *   clocktool sleep N US       N sleeps of US microseconds: the mean,
 *                              the largest and the smallest time one took
 *   clocktool spin SECONDS     keeps a CPU busy for the time
 *
 * Each prints one line that starts with its name.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STEP_SAMPLES 4096U

static uint64_t
now_ns(
	clockid_t clock)
{
	struct timespec value;

	/* The clock now, in nanoseconds. */
	if (clock_gettime(clock, &value) != 0)
		return 0U;
	return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static int
compare(
	const void *a,
	const void *b)
{
	uint64_t x;
	uint64_t y;

	/* Ascending. */
	x = *(const uint64_t *)a;
	y = *(const uint64_t *)b;
	if (x < y)
		return -1;
	if (x > y)
		return 1;
	return 0;
}

static int
run_res(
	void)
{
	static uint64_t steps[STEP_SAMPLES];
	struct timespec monotonic;
	struct timespec realtime;
	uint64_t before;
	uint64_t after;
	uint64_t tries;
	unsigned count;

	/* What the kernel says. */
	if (clock_getres(CLOCK_MONOTONIC, &monotonic) != 0 ||
	    clock_getres(CLOCK_REALTIME, &realtime) != 0) {
		printf("res: clock_getres failed\n");
		return 1;
	}

	/* What two calls in a row see: the first nonzero step of each pair. */
	count = 0U;
	for (tries = 0U; tries < 100000000ULL && count < STEP_SAMPLES; tries++) {
		before = now_ns(CLOCK_MONOTONIC);
		after = now_ns(CLOCK_MONOTONIC);
		while (after == before)
			after = now_ns(CLOCK_MONOTONIC);
		steps[count] = after - before;
		count++;
	}
	qsort(steps, count, sizeof(steps[0]), compare);
	printf("res: getres-monotonic-ns=%ld getres-realtime-ns=%ld step-min-ns=%llu step-median-ns=%llu samples=%u\n",
	    monotonic.tv_nsec + (long)monotonic.tv_sec * 1000000000L,
	    realtime.tv_nsec + (long)realtime.tv_sec * 1000000000L,
	    (unsigned long long)steps[0], (unsigned long long)steps[count / 2U], count);
	return 0;
}

static int
run_sleep(
	unsigned rounds,
	unsigned microseconds)
{
	struct timespec request;
	uint64_t start;
	uint64_t end;
	uint64_t took;
	uint64_t total;
	uint64_t longest;
	uint64_t shortest;
	unsigned round;

	/* Sleeps the time, each measured on the monotonic clock. */
	request.tv_sec = (time_t)(microseconds / 1000000U);
	request.tv_nsec = (long)(microseconds % 1000000U) * 1000L;
	total = 0U;
	longest = 0U;
	shortest = UINT64_MAX;
	for (round = 0U; round < rounds; round++) {
		start = now_ns(CLOCK_MONOTONIC);
		(void)nanosleep(&request, NULL);
		end = now_ns(CLOCK_MONOTONIC);
		took = end - start;
		total += took;
		if (took > longest)
			longest = took;
		if (took < shortest)
			shortest = took;
	}
	if (rounds == 0U)
		return 1;
	printf("sleep: rounds=%u request-us=%u mean-us=%llu max-us=%llu min-us=%llu\n", rounds, microseconds,
	    (unsigned long long)(total / rounds / 1000U), (unsigned long long)(longest / 1000U),
	    (unsigned long long)(shortest / 1000U));
	return 0;
}

static int
run_spin(
	unsigned seconds)
{
	volatile uint64_t sink;
	uint64_t end;

	/* Busy until the time is up. */
	sink = 0U;
	end = now_ns(CLOCK_MONOTONIC) + (uint64_t)seconds * 1000000000ULL;
	while (now_ns(CLOCK_MONOTONIC) < end)
		sink++;
	printf("spin: seconds=%u loops=%llu\n", seconds, (unsigned long long)sink);
	return 0;
}

int
main(
	int argc,
	char **argv)
{
	/* The command. */
	if (argc >= 2 && strcmp(argv[1], "res") == 0)
		return run_res();
	if (argc >= 4 && strcmp(argv[1], "sleep") == 0)
		return run_sleep((unsigned)strtoul(argv[2], NULL, 10), (unsigned)strtoul(argv[3], NULL, 10));
	if (argc >= 3 && strcmp(argv[1], "spin") == 0)
		return run_spin((unsigned)strtoul(argv[2], NULL, 10));
	fprintf(stderr, "usage: clocktool res | sleep N US | spin SECONDS\n");
	return 2;
}
