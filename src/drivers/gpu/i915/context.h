/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Logical ring contexts.
 *
 * A context is the image the engine loads when it switches to the context
 * and saves when it switches away, together with the ring the context's
 * commands are written to:
 *
 *   - the image is page 0 = the per-process status page, then the register
 *     state from I915_GT_LRC_STATE_OFFSET on, then two extra pages: the
 *     INDIRECT_CTX batch the engine runs as part of a context restore, and
 *     the PER_CTX_BB batch it runs after it;
 *   - the register state is laid out from the encoded register tables
 *     (intel/lrc-offsets.h) and then filled with the ring, page directory
 *     and power clock values;
 *   - the Gen12 indirect batch is not empty: it reloads the context
 *     timestamp, restores a scratch register and invalidates the AUX table,
 *     and the AUX invalidate ends in a semaphore poll, so a wrong register
 *     there hangs the context restore rather than the batch.
 *
 * The register-state indices (I915_GT_CTX_RING_TAIL and the rest) and the
 * MI commands come from intel/gt-regs.h and intel/commands.h.
 */

#ifndef DRIVERS_GPU_I915_CONTEXT_H
#define DRIVERS_GPU_I915_CONTEXT_H

#include <stdint.h>

struct i915_gt_engine;
struct i915_gt_mem;
struct i915_gt_object;
struct i915_gt_ppgtt;

/*
 * The distance a ring keeps between its write position and the engine's
 * read position, and the unit a batch's executed size is measured in.
 */
#define I915_CACHELINE_BYTES	64U

/*
 * The ring of one context.
 *
 * The ring object is allocated with the context and lives until the context
 * is released.  head, tail and emit are byte offsets into the ring.
 */
struct i915_gt_ring {
	/* The ring's backing object and its CPU view. */
	struct i915_gt_object *obj;
	uint32_t *vaddr;

	/* The ring size in bytes, a power of two. */
	uint32_t size;

	/* Where the engine reads from, and where the last submitted request ends. */
	uint32_t head;
	uint32_t tail;

	/* Where the next command is written. */
	uint32_t emit;

	/* The free bytes between emit and head, keeping one cacheline apart. */
	uint32_t space;

	/* The ring's GGTT address, which the context image names. */
	uint64_t ggtt_offset;
};

/*
 * One logical ring context: the image, its ring and its descriptor.
 *
 * The owner embeds it (an engine's kernel context, a record context, the
 * migrate and PXP contexts, a session's context); it holds objects from
 * drv_i915_lrc_alloc() until drv_i915_lrc_release().
 */
struct i915_gt_context {
	/* The engine the context runs on and the address space it uses. */
	struct i915_gt_engine *ge;
	struct i915_gt_ppgtt *vm;

	/* The context image and the CPU view of its register state. */
	struct i915_gt_object *state;
	uint32_t *regs;
	uint32_t state_bytes;

	/* The page index of the INDIRECT_CTX page, the first of the two extra pages. */
	unsigned wa_bb_page;

	struct i915_gt_ring ring;

	/* The software context id for the descriptor. */
	uint32_t sw_id;

	/* The lower dword of the descriptor. */
	uint32_t lrca;

	/*
	 * The whole descriptor: lrca in the low dword, with
	 * I915_GT_CTX_DESC_FORCE_RESTORE until the first submission, and the context id
	 * fields in the high dword, filled when the context is scheduled in.
	 */
	uint64_t desc;

	/* The software context id while scheduled in, else -1. */
	int tag;

	/* How many register-state dwords the offset table laid out. */
	unsigned reg_state_dwords;

	/* The GGTT address of the INDIRECT_CTX page and how many dwords run there. */
	uint32_t indirect_bb_ggtt;
	unsigned indirect_bb_dwords;

	/* Nonzero once the PER_CTX_BB pointer has been written into the image. */
	int per_ctx_bb_set;

	/* Nonzero while the image and the ring are allocated. */
	int allocated;
};

int drv_i915_lrc_alloc(struct i915_gt_context *ce, struct i915_gt_engine *ge, struct i915_gt_ppgtt *vm, struct i915_gt_mem *gm, uint32_t ring_size, uint32_t sw_id);
void drv_i915_lrc_build_image(struct i915_gt_context *ce);
void drv_i915_lrc_scrub(struct i915_gt_context *ce);
uint32_t drv_i915_lrc_set_ring(struct i915_gt_context *ce, uint32_t head);
uint32_t drv_i915_lrc_aux_inv_reg(int engine_id);
void drv_i915_lrc_release(struct i915_gt_context *ce, struct i915_gt_mem *gm);

#endif
