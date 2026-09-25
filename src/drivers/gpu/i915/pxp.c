/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Protected content (see pxp.h).
 */

#include "pxp.h"
#include "context.h"
#include "device-info.h"
#include "engine.h"
#include "memory.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>

#include "intel/gt-regs.h"

/* The ring size of the PXP context (SZ_4K) and the size of the streaming command page. */
#define I915_PXP_RING_BYTES		4096U
#define I915_PXP_STREAM_CMD_BYTES	4096U

static void i915_pxp_fail(struct i915_pxp *x, int error, const char *where);

/*
 * Sets up protected content on the GT (intel_pxp_init()).
 *
 * Returns 0, EINVAL, ENODEV when the device has no PXP or the GT has no video
 * decode engine, or the failure of the pinned context or the streaming
 * command page, whose step is in x->err_where.
 */
int
drv_i915_pxp_init(
	struct i915_pxp *x,
	struct i915_gt_engines *es,
	struct i915_gt_ppgtt *gt_vm,
	struct i915_gt_mem *gm,
	int has_pxp)
{
	struct i915_gt_engine *ge;
	unsigned index;
	int vdbox;
	int error;

	/* Refuses a setup without the engines, the GT address space or a pool. */
	if (x == NULL ||
	    es == NULL ||
	    gt_vm == NULL ||
	    gm == NULL)
		return EINVAL;

	/* Starts from an empty state. */
	kern_memset(x, 0, sizeof(*x));

	/* Finds the first video decode engine of the root GT. */
	vdbox = 0;
	for (index = 0U; index < es->n; index++) {
		if (es->ge[index].info->class == I915_VIDEO_DECODE_CLASS) {
			if (vdbox == 0)
				x->engine_idx = index;
			vdbox = 1;
		}
	}

	/*
	 * The GT needs PXP support and a video decode engine
	 * (find_gt_for_required_protected_content(), the MEI PXP path).
	 */
	if (has_pxp == 0 || vdbox == 0) {
		i915_pxp_fail(x, ENODEV, "find_gt_for_required_protected_content");
		return ENODEV;
	}

	/* The full feature is set up on this GT, on that engine. */
	x->full_feature = 1;
	x->has_engine = 1;

	/*
	 * Sets the KCR base (pxp_init_full()); the session management is only
	 * its lock and an idle work item.
	 */
	x->kcr_base = GEN12_KCR_BASE;

	/* Creates the pinned context on the GT address space (create_vcs_context()). */
	ge = &es->ge[x->engine_idx];
	error = drv_i915_lrc_alloc(&x->ce, ge, gt_vm, gm, I915_PXP_RING_BYTES, 0U);
	if (error != 0) {
		i915_pxp_fail(x, error, "create_vcs_context");
		return error;
	}

	/* Pins it: builds the image and writes the empty ring into it. */
	drv_i915_lrc_init_state(&x->ce);
	(void)drv_i915_lrc_update_regs(&x->ce, x->ce.ring.tail);

	/* The context's timeline lives in the engine's status page. */
	x->hwsp_ggtt = (uint32_t)ge->hwsp_ggtt + I915_GEM_HWS_PXP_ADDR;
	x->hwsp_cpu = &ge->hwsp[I915_GEM_HWS_PXP_ADDR / 4U];

	/* Allocates the streaming command page; a failure destroys the context. */
	x->stream_cmd = drv_i915_gt_object_create(gm, I915_PXP_STREAM_CMD_BYTES);
	if (x->stream_cmd == NULL) {
		i915_pxp_fail(x, ENOMEM, "alloc_streaming_command");
		drv_i915_lrc_release(&x->ce, gm);
		return ENOMEM;
	}

	/* Records the TEE component as added; the component framework does not exist here. */
	x->component_added = 1;
	x->inited = 1;

	/* Succeeded: the full feature is set up and waits for a component bind. */
	return 0;
}

/*
 * Tears protected content down (intel_pxp_fini()).
 */
void
drv_i915_pxp_fini(
	struct i915_pxp *x,
	struct i915_gt_mem *gm)
{
	/* Nothing to release without the state or a pool. */
	if (x == NULL || gm == NULL)
		return;

	/* Removes the TEE component and its streaming command page. */
	x->component_added = 0;
	if (x->stream_cmd != NULL) {
		drv_i915_gt_object_destroy(gm, x->stream_cmd);
		x->stream_cmd = NULL;
	}

	/* Destroys the pinned context (destroy_vcs_context()). */
	if (x->ce.allocated != 0)
		drv_i915_lrc_release(&x->ce, gm);

	/* Marks the state empty. */
	x->inited = 0;
}

/* Records the first failure and the step it came from; a later one is not recorded. */
static void
i915_pxp_fail(
	struct i915_pxp *x,
	int error,
	const char *where)
{
	/* Only the first failure names the step. */
	if (x->err == 0) {
		x->err = error;
		x->err_where = where;
	}
}
