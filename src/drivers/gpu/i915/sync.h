/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Time base, delays, register waits and completions.
 *
 * The delays and register waits have a busy stage that never sleeps, so
 * it is safe with a spinlock held or interrupts off, and an optional slow
 * stage that sleeps between reads and may only be used from a context that
 * can sleep.  Both measure time against the kernel's monotonic
 * counter, which neither depends on interrupts nor jumps when the thread
 * moves to another CPU.
 *
 * A failure of the time base itself is kept apart from a hardware timeout:
 * it is reported as EIO and latched once for the whole driver, so a caller
 * never mistakes a broken clock for an elapsed delay or an unresponsive
 * register.
 *
 * A completion counts its signals: each complete adds one permit and each successful wait consumes one.
 */

#ifndef DRIVERS_GPU_I915_SYNC_H
#define DRIVERS_GPU_I915_SYNC_H

#include <kern/lock.h>
#include <kern/waitq.h>
#include <stdint.h>

struct i915_mmio;

/*
 * Why the time base was declared unusable.
 *
 * Only the first cause is latched; drv_i915_time_base_faulted() reports it
 * and zero means the time base has never failed.
 */
enum i915_time_base_fault {
	/* The time base has not failed. */
	I915_TIME_BASE_OK = 0,

	/* The counter or its frequency was unavailable when a wait started. */
	I915_TIME_BASE_NO_COUNTER,

	/* A counter read failed, or the frequency changed, during a wait. */
	I915_TIME_BASE_READ_FAIL,

	/* The kernel wait interface reported an error during a slow stage. */
	I915_TIME_BASE_WAIT_API
};

/*
 * A counting completion that an interrupt handler or a worker signals.
 *
 * One instance stands for one event a thread waits on, such as a vertical
 * blank of one pipe.  It lives inside its owner, is prepared once with
 * drv_i915_completion_init() and needs no teardown.  The lock is taken with
 * interrupts disabled, so complete may be called from an interrupt handler.
 */
struct i915_completion {
	/* Protects the permit count and orders it against the wait queue. */
	struct spinlock lock;

	/* Where waiters sleep until a permit arrives or their deadline passes. */
	struct wait_queue waitq;

	/* How many signals no wait has consumed yet. */
	unsigned done;
};

int drv_i915_time_base_ok(void);
int drv_i915_time_base_faulted(void);
int drv_i915_udelay(unsigned microseconds);
int drv_i915_wait_reg(struct i915_mmio *mmio, uint32_t reg, uint32_t mask, uint32_t value, unsigned fast_us, unsigned slow_ms, uint32_t *last);

void drv_i915_completion_init(struct i915_completion *completion, const char *name);
void drv_i915_complete(struct i915_completion *completion);
void drv_i915_reinit_completion(struct i915_completion *completion);
int drv_i915_wait_for_completion(struct i915_completion *completion, uint64_t deadline);

#endif
