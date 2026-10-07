/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Execlists submission.
 *
 * An engine is given work by writing context descriptors into its ELSQ and
 * loading the queue; the engine reports what it did with them in the context
 * status buffer (CSB), a ring of entries in the engine's status page.  This
 * follows the reference's intel_execlists_submission.c for Gen12:
 *
 *   - schedule-in gives the context a software context id (its tag) and puts
 *     it with the engine's class and instance into the descriptor's upper
 *     dword;
 *   - updating the context writes the request's tail into the image and
 *     forces a full restore when the tail did not advance;
 *   - both ELSQ ports are written, the highest first, each as lower then
 *     upper dword, and then the queue is loaded;
 *   - the CSB is walked from the cached head to the write pointer, and each
 *     entry either promotes the pending ports to active or completes the
 *     active context;
 *   - an entry that still reads all-ones may not be globally visible yet: it
 *     is polled for 10 us and then read from the MMIO copy of the buffer.
 *
 * The reference runs the CSB processing from the CS interrupt with a
 * priority queue behind it.  Here one request is in flight per engine and
 * the processing runs synchronously -- which is what the reference does on
 * its wait path, where intel_engine_flush_submission() runs the tasklet body
 * inline.  The interrupt still arrives and is still acknowledged; it is not
 * what drives the processing.
 *
 * This file also holds the execlists side of the engine lifecycle: the
 * submission registers and CSB pointers, enable_execlists() at resume, the
 * CSB pointer reset, and the reset preparation.
 */

#ifndef DRIVERS_GPU_I915_SUBMIT_H
#define DRIVERS_GPU_I915_SUBMIT_H

#include <stdint.h>

struct i915_gt_engine;
struct i915_gt_request;
struct i915_mmio;

/*
 * The execlists state of one engine.
 *
 * It lives in the engine set next to its engine from the engine start to the
 * engine release.  The submitting thread is its only user.
 */
struct i915_execlists {
	/* The requests loaded into the ELSQ and not yet acknowledged. */
	struct i915_gt_request *pending[2];

	/* The requests the engine has acknowledged; inflight[0] is running. */
	struct i915_gt_request *inflight[2];

	/* Nonzero while inflight[0] runs. */
	int have_active;

	/* The free software context ids, one bit each. */
	uint64_t context_tag;

	/*
	 * The engine's serial moves on every submission; the park switch to
	 * the kernel context is skipped when wakeref_serial has caught up.
	 */
	unsigned serial;
	unsigned wakeref_serial;

	/* What the CSB reported, kept for the report. */
	unsigned csb_events;
	unsigned promotes;
	unsigned completes;

	/* Entries that read all-ones and became visible within the poll. */
	unsigned csb_late;

	/* Entries that never became visible and were read over MMIO. */
	unsigned csb_mmio_fallback;

	/* Events that had nothing to apply to; any is a failed request. */
	unsigned csb_errors;

	/* The last CSB entry, lower and upper dword. */
	uint32_t last_csb_lo;
	uint32_t last_csb_hi;

	/*
	 * The last I915_CSB_HISTORY entries, oldest overwritten first, and the
	 * next place to write (BUG-077: what came before an event with nothing
	 * to apply to).
	 */
	uint64_t csb_history[8];
	unsigned csb_history_next;

	/* How many submissions were made. */
	unsigned submits;
};

void drv_i915_execlists_init(struct i915_execlists *el);
void drv_i915_execlists_submission_setup(struct i915_gt_engine *ge);
void drv_i915_execlists_enable(struct i915_gt_engine *ge, struct i915_mmio *mmio);
void drv_i915_execlists_reset_csb_pointers(struct i915_gt_engine *ge, struct i915_mmio *mmio);
void drv_i915_execlists_reset_prepare(struct i915_gt_engine *ge, struct i915_mmio *mmio);
void drv_i915_execlists_reset_rewind(struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_mmio *mmio);
int drv_i915_execlists_submit(struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_mmio *mmio, struct i915_gt_request *rq);
struct i915_gt_request *drv_i915_execlists_process_csb(struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_mmio *mmio);

#endif
