/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Recording each engine's default context image.
 *
 * Per engine, a record context is created with a 4 KiB ring and its own
 * timeline in a 4 KiB status page (its seqnos advance by 2, the first step
 * being the initial breadcrumb).  The image is built with the restore
 * inhibited, and one record request is written: the invalidate flush, the
 * context workarounds, the Gen9 render engine's null render state, and the
 * breadcrumb.  The request is submitted.
 *
 * The recording then polls the CSB and the status pages for at least the
 * timeout of real time.  Once a record request has completed in the CSB, the
 * engine parks: a request on the pinned kernel context switches the record
 * context out, so its image is written back, unless the engine's serial has
 * not moved since the last park.  Only when that second request has
 * completed is the engine idle.  A CSB error fails the recording with EIO;
 * otherwise the record context's whole image becomes the engine's default
 * state.  Any error resets the engines.
 *
 * Because the restore was inhibited, the engine does not load the record
 * context: it runs from its post-reset register state and saves that into
 * the image on switch-out.  That saved image is the default state.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt.c
 * (__engines_record_defaults: record an inhibited context per engine and
 * keep the image saved on the switch to the kernel context).
 */

#ifndef DRIVERS_GPU_I915_DEFAULTS_H
#define DRIVERS_GPU_I915_DEFAULTS_H

#include <stdint.h>

#include "context.h"
#include "device-info.h"
#include "request.h"

struct i915_gt_engines;
struct i915_gt_init;
struct i915_gt_mem;
struct i915_gt_object;
struct i915_gt_ppgtt;
struct i915_mmio;
struct spinlock;

/* Each engine's progress through the recording. */
#define I915_DEF_IDLE		0	/* nothing submitted */
#define I915_DEF_RECORD		1	/* the record request is in flight */
#define I915_DEF_SWITCH		2	/* the park switch to the kernel context is in flight */
#define I915_DEF_PARKED		3	/* the engine is idle and the image written back */

/*
 * The recording of every engine's default state.
 *
 * The device owns one.  The record contexts and their timeline pages live
 * only during drv_i915_engines_record_defaults(); the default-state copies
 * live until drv_i915_engines_defaults_release(), and the engines point at
 * them once the caller has published them.
 */
struct i915_gt_defaults {
	/* How many engines have a record context. */
	unsigned n;

	/* Each engine's record context, its timeline page and the timeline's last seqno. */
	struct i915_gt_context ce[I915_MAX_ENGINES];
	struct i915_gt_object *tl_page[I915_MAX_ENGINES];
	uint32_t tl_seqno[I915_MAX_ENGINES];

	/*
	 * The Gen9 render engine's null render state batch, which the record
	 * request runs to give the default image a golden 3D state; it lives
	 * as long as the record context.
	 */
	struct i915_gt_object *null_state[I915_MAX_ENGINES];

	/* The record request and the park switch request of each engine. */
	struct i915_gt_request rq[I915_MAX_ENGINES];
	struct i915_gt_request krq[I915_MAX_ENGINES];

	/* The copies of the saved images, one per engine. */
	struct i915_gt_object *default_state[I915_MAX_ENGINES];

	/* Each engine's I915_DEF_* progress. */
	int state[I915_MAX_ENGINES];

	/* The first failure (a positive errno) and the step it came from. */
	int err;
	const char *err_where;

	/* How many polls the wait made. */
	unsigned polls;

	/* Nonzero when the wait ran out, and when the GT was wedged. */
	int timed_out;
	int wedged;
};

int drv_i915_engines_record_defaults(struct i915_gt_defaults *d, struct i915_gt_engines *es, struct i915_gt_init *gi, struct i915_gt_mem *gm, struct i915_gt_ppgtt *pp, struct i915_mmio *mmio, struct spinlock *uncore_lock, unsigned timeout_ms);
void drv_i915_engines_defaults_release(struct i915_gt_defaults *d, struct i915_gt_mem *gm);

#endif
