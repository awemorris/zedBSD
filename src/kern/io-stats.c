/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "kern/atomic.h"
#include "kern/io-stats.h"

static struct io_stat_value counters[IO_STAT_COUNT];

/*
 * Records one event without allocating memory or taking an I/O lock.
 * Counters are cumulative since boot and intentionally cannot be reset.
 */
void
io_stats_record(
	enum io_stat_event event,
	uint64_t bytes)
{
	/* Keeps a malformed instrumentation site outside the counter array. */
	if ((unsigned)event >= IO_STAT_COUNT)
		return;

	/* Counts the request and its byte quantity independently. */
	atomic_u64_fetch_add_relaxed(&counters[event].calls, 1);
	atomic_u64_fetch_add_relaxed(&counters[event].bytes, bytes);
}

/*
 * Copies a versioned snapshot of the cumulative event counters.
 * Quiescent before/after samples give exact deltas; live fields may skew.
 */
void
io_stats_snapshot(
	struct io_stats *snapshot)
{
	unsigned i;

	/* Permits callers to skip an optional snapshot. */
	if (snapshot == NULL)
		return;

	/* Reads each field through the architecture's atomic implementation. */
	snapshot->version = IO_STATS_VERSION;
	snapshot->count = IO_STAT_COUNT;
	for (i = 0; i < IO_STAT_COUNT; i++) {
		snapshot->events[i].calls =
		    atomic_u64_load_acquire(&counters[i].calls);
		snapshot->events[i].bytes =
		    atomic_u64_load_acquire(&counters[i].bytes);
	}
}
