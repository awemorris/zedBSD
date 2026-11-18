/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Engine workaround verification by the GPU itself, which runs always, as
 * a diagnostic.  For each engine with a workaround list:
 *
 *   - a one-page scratch object from the fixed pool is bound into the GGTT
 *   - a request on the engine's kernel context stores every listed register
 *     to scratch + 4 * list index with MI_STORE_REGISTER_MEM, skipping the
 *     multicast ranges: the MCR selector steers CPU accesses only, so the
 *     command streamer would read some other instance
 *   - the request is submitted, and the CSB and the status page are polled
 *     for at least the requested time
 *   - every stored value is compared with the list; an entry whose read
 *     mask is 0 always passes
 *   - once the request has completed, the engine parks by switching back to
 *     its kernel context (one request is kept in flight per engine); a
 *     timed-out engine gets no park switch, since it is hung and a switch
 *     queued behind the hang would fail the final wait the same way
 *
 * Any engine failure makes the whole verification fail, and so does a GT
 * that does not go idle afterwards.
 */

#ifndef DRIVERS_GPU_I915_VERIFY_WORKAROUNDS_H
#define DRIVERS_GPU_I915_VERIFY_WORKAROUNDS_H

#include <stdint.h>

#include "device-info.h"
#include "request.h"

struct i915_mmio;
struct i915_gt_init;
struct i915_gt_mem;
struct i915_gt_engines;
struct i915_gt_object;
struct i915_wa_list;

/* Where the verification of one engine stands. */
enum i915_vwa_state {
	/* The engine has no list, or its verification has not started. */
	I915_VWA_IDLE = 0,

	/* The store request is in flight. */
	I915_VWA_SRM = 1,

	/* The store request completed; the stored values can be read. */
	I915_VWA_DONE = 2,

	/* The switch back to the kernel context is in flight. */
	I915_VWA_SWITCH = 3,

	/* The engine is back on its kernel context. */
	I915_VWA_PARKED = 4
};

/*
 * The verification of every engine's workarounds.
 *
 * The device start owns it from the verification to the device stop; the
 * scratch objects stay allocated until drv_i915_engines_verify_wa_release().
 */
struct i915_gt_verify_wa {
	/* How many engines the verification reached. */
	unsigned n;

	/* The page each engine stores its registers into. */
	struct i915_gt_object *scratch[I915_MAX_ENGINES];

	/* The store request, and the switch back to the kernel context. */
	struct i915_gt_request rq[I915_MAX_ENGINES];
	struct i915_gt_request krq[I915_MAX_ENGINES];

	/* One of enum i915_vwa_state per engine. */
	int state[I915_MAX_ENGINES];

	/* The list size, the stores emitted and the multicast entries skipped. */
	unsigned list_count[I915_MAX_ENGINES];
	unsigned emitted[I915_MAX_ENGINES];
	unsigned mcr_skipped[I915_MAX_ENGINES];

	/* The entries found right, found lost, and not compared (read mask 0). */
	unsigned verified[I915_MAX_ENGINES];
	unsigned mismatched[I915_MAX_ENGINES];
	unsigned not_verifiable[I915_MAX_ENGINES];

	/* The positive errno each engine failed with, or zero. */
	int engine_err[I915_MAX_ENGINES];

	/*
	 * The graphics version of each engine's stores, which decides the
	 * multicast ranges the stores and the comparison skip (Gen9 has the
	 * Gen8 ranges); zero takes the Gen12 ranges.
	 */
	int graphics_ver[I915_MAX_ENGINES];

	/* The first positive errno recorded, and the step that produced it. */
	int err;
	const char *err_where;

	/* How many completion polls ran. */
	unsigned polls;

	/* Nonzero once a wait ran out of time. */
	int timed_out;
};

int drv_i915_wa_multicast_gen12(uint32_t offset);
int drv_i915_wa_list_store_regs(struct i915_gt_request *rq, const struct i915_wa_list *wal, uint32_t scratch_ggtt, unsigned *emitted, unsigned *skipped);
int drv_i915_wa_list_check(struct i915_gt_verify_wa *verify, unsigned index, const struct i915_wa_list *wal, const char *from);
int drv_i915_engine_verify_wa_submit(struct i915_gt_verify_wa *verify, unsigned index, struct i915_gt_engines *engines, const struct i915_wa_list *wal, struct i915_gt_mem *gt_mem, struct i915_mmio *mmio);
int drv_i915_engine_verify_wa_poll(struct i915_gt_verify_wa *verify, unsigned index, struct i915_gt_engines *engines, struct i915_mmio *mmio);
int drv_i915_engine_verify_wa_park(struct i915_gt_verify_wa *verify, unsigned index, struct i915_gt_engines *engines, struct i915_mmio *mmio);
int drv_i915_engines_verify_workarounds(struct i915_gt_verify_wa *verify, struct i915_gt_engines *engines, struct i915_gt_init *gt_init, struct i915_gt_mem *gt_mem, struct i915_mmio *mmio, unsigned timeout_ms);
void drv_i915_engines_verify_wa_release(struct i915_gt_verify_wa *verify, struct i915_gt_mem *gt_mem);

#endif
