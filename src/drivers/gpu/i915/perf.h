/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Frame timing of the node.
 *
 * The executor and the present path add the time their stages took to
 * totals kept by the device; once about five seconds have passed the totals
 * are logged as milliseconds per submission and per presentation, and a
 * new window starts.  Nothing is logged per frame, and the cost of a sample
 * is two reads of the monotonic counter.
 */

#ifndef DRIVERS_GPU_I915_PERF_H
#define DRIVERS_GPU_I915_PERF_H

#include <kern/lock.h>

#include <stdint.h>

/* The stages whose time is kept. */
enum i915_perf_stage {
	/* A whole vkQueueSubmit: every command buffer built and run. */
	I915_PERF_SUBMIT = 0,

	/* An executor batch from its queuing to the worker until its caller wakes. */
	I915_PERF_RUN,

	/* An executor batch from its submission to the engine until its end. */
	I915_PERF_GPU,

	/* A whole presentation, from the display ioctl to its return. */
	I915_PERF_PRESENT,

	/* The copy of a presented frame into the panel buffer (built and run). */
	I915_PERF_PRESENT_COPY,

	/* A marker request (a fence with no batch) from its queuing until the worker takes it. */
	I915_PERF_MARKER_WAIT,

	/* A marker request from the worker taking it until its completion is delivered. */
	I915_PERF_MARKER_RUN,

	/* The CPU cache flush that publishes a panel buffer to the display engine. */
	I915_PERF_PRESENT_PUBLISH,

	/* The flip to the panel buffer, including any wait for its completion. */
	I915_PERF_PRESENT_FLIP,

	/* How many stages there are. */
	I915_PERF_STAGES
};

/*
 * The timing totals of one device.
 *
 * The device owns it for its whole life; the lock protects the totals and
 * the window, since the executor, the display ioctls and the worker add to
 * them from their own threads.  A zero window start means no sample was
 * taken since the last report.
 */
struct i915_perf {
	/* Protects the totals and the window. */
	struct spinlock lock;

	/* Nonzero once the lock is prepared; samples before that are dropped. */
	int ready;

	/* When the current window started, in nanoseconds of the monotonic counter. */
	uint64_t window_start;

	/* The nanoseconds and the samples of each stage in the current window. */
	uint64_t total[I915_PERF_STAGES];
	uint32_t count[I915_PERF_STAGES];
};

void drv_i915_perf_init(struct i915_perf *perf);
uint64_t drv_i915_perf_now(void);
void drv_i915_perf_add(struct i915_perf *perf, enum i915_perf_stage stage, uint64_t start);
void drv_i915_perf_report(struct i915_perf *perf, int final);

#endif
