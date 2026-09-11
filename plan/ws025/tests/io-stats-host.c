/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kern/io-stats.h"

#define THREADS 4
#define ITERATIONS 100000
#define AMOUNT UINT64_C(4294967808)

static void *record_events(void *argument)
{
	unsigned i;
	(void)argument;
	for (i = 0; i < ITERATIONS; i++)
		io_stats_record(IO_DRIVER_WRITE, AMOUNT);
	return NULL;
}

int main(void)
{
	struct io_stats before, after, live;
	pthread_t threads[THREADS];
	unsigned i;

	io_stats_snapshot(NULL);
	io_stats_snapshot(&before);
	assert(before.version == IO_STATS_VERSION && before.count == IO_STAT_COUNT);
	io_stats_record((enum io_stat_event)-1, UINT64_MAX);
	io_stats_record(IO_STAT_COUNT, UINT64_MAX);
	io_stats_snapshot(&after);
	assert(memcmp(&before, &after, sizeof(before)) == 0);
	for (i = 0; i < THREADS; i++)
		assert(pthread_create(&threads[i], NULL, record_events, NULL) == 0);
	for (i = 0; i < 1000; i++) {
		io_stats_snapshot(&live);
		assert(live.events[IO_DRIVER_WRITE].calls <= THREADS * ITERATIONS);
		assert(live.events[IO_DRIVER_WRITE].bytes <=
		    AMOUNT * THREADS * ITERATIONS);
	}
	for (i = 0; i < THREADS; i++)
		assert(pthread_join(threads[i], NULL) == 0);
	io_stats_snapshot(&after);
	assert(after.events[IO_DRIVER_WRITE].calls == THREADS * ITERATIONS);
	assert(after.events[IO_DRIVER_WRITE].bytes == AMOUNT * THREADS * ITERATIONS);
	for (i = 0; i < IO_STAT_COUNT; i++) {
		if (i == IO_DRIVER_WRITE)
			continue;
		assert(after.events[i].calls == 0 && after.events[i].bytes == 0);
	}
	puts("IO-STATS PASS: concurrent writers, live snapshots, 64-bit bytes, invalid IDs");
	return 0;
}

