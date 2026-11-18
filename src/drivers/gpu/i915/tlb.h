/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT TLB invalidation after page-table entries were removed.
 *
 * Every engine of a Gen9 or graphics version 12 GT drops its TLB.  A page
 * whose entries were pointed back at scratch may be reused only after the
 * invalidation succeeded, because an engine's TLB may still hold the old
 * translation.
 */

#ifndef DRIVERS_GPU_I915_TLB_H
#define DRIVERS_GPU_I915_TLB_H

#include <stdint.h>

struct i915_gt_engines;
struct i915_mmio;
struct spinlock;

/*
 * The TLB invalidation state of one GT.
 *
 * It lives in the GT's owner and is used only by the one thread that owns
 * the GT, which serializes the invalidations.
 */
struct i915_gt_tlb {
	/*
	 * The TLB generation: always even, and advanced by 2 after every
	 * completed invalidation, so a generation that failed is never taken
	 * as completed.
	 */
	uint32_t seqno;

	/* How many full invalidations completed, and how many engines the last one covered. */
	unsigned invalidations;
	unsigned engines_invalidated;

	/* How many engine waits timed out, how many failed on the time base, and how many forcewake takes failed. */
	unsigned timeouts;
	unsigned time_faults;
	unsigned fw_failures;

	/*
	 * Who is asking.  A timeout log line carries these, so an intended
	 * fault of a test is told apart mechanically from a hardware anomaly:
	 * a NULL backend reads as "HW" and a NULL test_id as "-".
	 */
	const char *backend;
	const char *test_id;
	int expected_fault;
};

int drv_i915_tlb_invalidate_all(struct i915_gt_tlb *tlb, struct i915_gt_engines *es, struct i915_mmio *m, struct spinlock *uncore_lock);

#endif
