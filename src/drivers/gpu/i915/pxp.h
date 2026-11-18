/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Protected content.
 *
 * On a device with PXP, no media GT and a video decode engine, the state
 * is set up on the root GT: the KCR base, the session management state
 * (idle), a pinned context on the first video decode engine with a 4 KiB
 * ring and its timeline at I915_GT_HWS_PXP_ADDR, and the TEE component
 * with its streaming command page.
 *
 * The KCR and interrupt setup and the arbitration session start only once
 * a management engine component binds, and none exists here: nothing is
 * written to KCR, the component is recorded as added and never binds, and
 * the streaming command page is a driver object with no CPU map or DMA pin.
 */

#ifndef DRIVERS_GPU_I915_PXP_H
#define DRIVERS_GPU_I915_PXP_H

#include <stdint.h>

#include "context.h"

struct i915_gt_engines;
struct i915_gt_mem;
struct i915_gt_object;
struct i915_gt_ppgtt;

/*
 * The protected-content state of the device.
 *
 * The device owns one; it lives from drv_i915_pxp_init() to
 * drv_i915_pxp_fini().
 */
struct i915_pxp {
	/* Nonzero once a video decode engine was found, and which (the first). */
	int has_engine;
	unsigned engine_idx;

	/* The KCR register base. */
	uint32_t kcr_base;

	/* The pinned context and its timeline slot in the engine's status page. */
	struct i915_gt_context ce;
	uint32_t hwsp_ggtt;
	volatile uint32_t *hwsp_cpu;

	/* The streaming command page (alloc_streaming_command()). */
	struct i915_gt_object *stream_cmd;

	/* Nonzero once the TEE component is recorded as added. */
	int component_added;

	/* Nonzero when the full feature is set up on this GT. */
	int full_feature;

	/* Nonzero once the state is ready. */
	int inited;

	/* The first failure (a positive errno) and the step it came from. */
	int err;
	const char *err_where;
};

int drv_i915_pxp_init(struct i915_pxp *x, struct i915_gt_engines *es, struct i915_gt_ppgtt *gt_vm, struct i915_gt_mem *gm, int has_pxp);
void drv_i915_pxp_fini(struct i915_pxp *x, struct i915_gt_mem *gm);

#endif
