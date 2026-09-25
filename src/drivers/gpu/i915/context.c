/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Logical ring contexts (see context.h).
 *
 * The reference functions this file stands for are __lrc_alloc_state() and
 * intel_engine_create_ring() (allocation), lrc_init_state() and
 * __lrc_init_regs() (the image), lrc_update_regs() (the ring registers, the
 * render power clock state, the two workaround batches and the descriptor)
 * and lrc_reset().
 */

#include "context.h"
#include "device-info.h"
#include "engine.h"
#include "ggtt.h"
#include "memory.h"
#include "ppgtt.h"
#include "request.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "intel/commands.h"
#include "intel/gt-regs.h"
#include "intel/lrc-offsets.h"

/* A masked register word that sets the named bits. */
#define I915_MASKED_ENABLE(bits)	((((uint32_t)(bits)) << 16) | ((uint32_t)(bits)))

/* A masked register word that clears the named bits. */
#define I915_MASKED_DISABLE(bits)	(((uint32_t)(bits)) << 16)

/* The size of one page of the context image. */
#define I915_LRC_PAGE_BYTES		4096U

/*
 * Register-state slots on Gen12, the values the reference's lrc_ring_*()
 * helpers return for GRAPHICS_VER 12.
 */
#define I915_LRC_RING_WA_BB_PER_CTX	0x12
#define I915_LRC_RING_INDIRECT_PTR	0x14
#define I915_LRC_RING_INDIRECT_OFFSET	0x16
#define I915_LRC_MI_MODE_INDEX		0x60
#define I915_LRC_BB_OFFSET_INDEX	0x70
#define I915_LRC_RING_GPR0		0x74

/* The CMD_BUF_CCTL slot, which only the render engine's image has. */
#define I915_LRC_RING_CMD_BUF_CCTL	0xb6

/* Where the INDIRECT_CTX offset field starts in its register. */
#define I915_INDIRECT_CTX_OFFSET_SHIFT	6

/* The PER_CTX_BB pointer flags: run the batch on every restore, and it is valid. */
#define I915_PER_CTX_BB_FORCE		(1U << 2)
#define I915_PER_CTX_BB_VALID		(1U << 0)

static int i915_lrc_alloc_objects(struct i915_gt_context *ce, struct i915_gt_mem *gm, uint32_t ring_size);
static unsigned i915_hweight8(uint8_t value);
static uint32_t i915_lrc_state_size(uint32_t context_size, unsigned *wa_bb_page);
static unsigned i915_lrc_set_offsets(uint32_t *regs, const uint8_t *data, uint32_t mmio_base, int close);
static uint32_t i915_sseu_make_rpcs(uint8_t slice_mask, int has_slice_pg);
static void i915_lrc_init_regs(struct i915_gt_context *ce, int inhibit);
static void i915_lrc_init_common_regs(struct i915_gt_context *ce, int inhibit);
static void i915_lrc_init_ppgtt_regs(struct i915_gt_context *ce);
static void i915_lrc_reset_stop_ring(struct i915_gt_context *ce);
static uint32_t *i915_lrc_wa_bb(struct i915_gt_context *ce, int per_ctx);
static uint32_t i915_lrc_indirect_bb(const struct i915_gt_context *ce);
static uint32_t *i915_lrc_emit_timestamp_wa(struct i915_gt_context *ce, uint32_t *cs);
static uint32_t *i915_lrc_emit_cmd_buf_wa(struct i915_gt_context *ce, uint32_t *cs);
static uint32_t *i915_lrc_emit_restore_scratch(struct i915_gt_context *ce, uint32_t *cs);
static uint32_t *i915_lrc_emit_invalidate_state_cache(uint32_t *cs);
static void i915_lrc_setup_predicate_disable_wa(struct i915_gt_context *ce, uint32_t *cs);
static void i915_lrc_setup_indirect_ctx_bb(struct i915_gt_context *ce);
static void i915_lrc_setup_per_ctx_bb(struct i915_gt_context *ce);
static uint32_t i915_lrc_descriptor(const struct i915_gt_context *ce);

/*
 * Allocates a context image and its ring and binds both into the GGTT.
 *
 * The image is sized as __lrc_alloc_state() sizes it and the ring as
 * intel_engine_create_ring() does; ring_size must be a power of two.  Nothing
 * is written into the image yet.  Returns 0, EINVAL, ENOMEM or the GGTT bind
 * error; on failure nothing stays allocated.
 */
int
drv_i915_lrc_alloc(
	struct i915_gt_context *ce,
	struct i915_gt_engine *ge,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_mem *gm,
	uint32_t ring_size,
	uint32_t sw_id)
{
	int error;

	/* Refuses a context without an engine, an address space, a pool or a ring. */
	if (ce == NULL ||
	    ge == NULL ||
	    vm == NULL ||
	    gm == NULL ||
	    ring_size == 0U)
		return EINVAL;

	/* Starts from an empty context bound to its engine and address space. */
	kern_memset(ce, 0, sizeof(*ce));
	ce->ge = ge;
	ce->vm = vm;
	ce->sw_id = sw_id;
	ce->tag = -1;

	/* Sizes the image for the engine: the context size plus the two batch pages. */
	ce->state_bytes = i915_lrc_state_size(ge->info->context_size, &ce->wa_bb_page);

	/* Allocates the image and the ring; a partial allocation is given back. */
	error = i915_lrc_alloc_objects(ce, gm, ring_size);
	if (error != 0) {
		drv_i915_lrc_release(ce, gm);
		return error;
	}

	/* Marks the context usable by the image and ring operations. */
	ce->allocated = 1;

	/* Succeeded: the image and the ring are allocated and bound. */
	return 0;
}

/*
 * Builds a context image the way lrc_init_state() does.
 *
 * An engine that has recorded its default state gives the image that state
 * and the restore is allowed; otherwise the restore is inhibited and the
 * engine starts the context from its own register state.  The per-process
 * status page and the INDIRECT_CTX page are cleared either way.
 */
void
drv_i915_lrc_init_state(
	struct i915_gt_context *ce)
{
	uint32_t copy_bytes;
	int inhibit;

	/* A context without an image has nothing to build. */
	if (ce == NULL || ce->allocated == 0)
		return;

	/*
	 * Without a default state the restore is inhibited, which is what the
	 * reference does until __engines_record_defaults() has run.
	 */
	inhibit = 1;

	/* Copies the recorded default state, bounded by both images. */
	if (ce->ge->default_state != NULL && ce->ge->default_state->cpu != NULL) {
		copy_bytes = ce->ge->info->context_size;
		if (copy_bytes > ce->ge->default_state->bytes)
			copy_bytes = ce->ge->default_state->bytes;
		if (copy_bytes > ce->state_bytes)
			copy_bytes = ce->state_bytes;
		kern_memcpy(ce->state->cpu, ce->ge->default_state->cpu, copy_bytes);

		/* A valid saved image may be restored (CONTEXT_VALID_BIT). */
		inhibit = 0;
	}

	/* Clears the per-process status page, including the per-context counters. */
	kern_memset(ce->state->cpu, 0, I915_LRC_PAGE_BYTES);

	/* Clears the indirect workaround and storage page. */
	if (ce->wa_bb_page != 0U) {
		kern_memset((char *)ce->state->cpu + ce->wa_bb_page * I915_LRC_PAGE_BYTES,
		       0,
		       I915_LRC_PAGE_BYTES);
	}

	/* Lays out and fills the register state. */
	i915_lrc_init_regs(ce, inhibit);
}

/*
 * Resets a context the way lrc_reset() does.
 *
 * The ring is emptied at its write position, the register state is rebuilt
 * with the restore inhibited, and the ring registers are written again.
 */
void
drv_i915_lrc_reset(
	struct i915_gt_context *ce)
{
	/* A context without an image has nothing to reset. */
	if (ce == NULL || ce->allocated == 0)
		return;

	/* Empties the ring at its write position (intel_ring_reset(ring, ring->emit)). */
	ce->ring.head = ce->ring.emit;
	ce->ring.tail = ce->ring.emit;
	ce->ring.space = (ce->ring.head - ce->ring.emit - I915_CACHELINE_BYTES) & (ce->ring.size - 1U);

	/* Scrubs away whatever the image held. */
	i915_lrc_init_regs(ce, 1);

	/* Writes the ring registers for the empty ring; the descriptor stays in the context. */
	(void)drv_i915_lrc_update_regs(ce, ce->ring.tail);
}

/*
 * Writes the ring registers, the render power state and the workaround
 * batches into a context image, as lrc_update_regs() does.
 *
 * Returns the lower dword of the descriptor with CTX_DESC_FORCE_RESTORE, the
 * value the reference stores as ce->lrc.lrca; zero for a context without an
 * image.
 */
uint32_t
drv_i915_lrc_update_regs(
	struct i915_gt_context *ce,
	uint32_t head)
{
	uint32_t *regs;
	uint32_t lrca;

	/* A context without an image has no registers to write. */
	if (ce == NULL || ce->allocated == 0)
		return 0U;

	/* Points the image at the ring and gives it the ring's positions and size. */
	regs = ce->lrc_reg_state;
	regs[CTX_RING_START] = (uint32_t)ce->ring.ggtt_offset;
	regs[CTX_RING_HEAD] = head;
	regs[CTX_RING_TAIL] = ce->ring.tail;
	regs[CTX_RING_CTL] = RING_CTL_SIZE(ce->ring.size) | RING_VALID;

	/*
	 * Without the power clock state the render engine may come up with its
	 * execution units power gated: the fixed-function stages run and the
	 * first thread never dispatches.
	 */
	if (ce->ge->info->class == I915_RENDER_CLASS) {
		regs[CTX_R_PWR_CLK_STATE] = i915_sseu_make_rpcs(ce->ge->sseu_slice_mask,
								ce->ge->sseu_has_slice_pg);
	}

	/*
	 * The engine's global workaround batch is empty on Gen12, which uses
	 * these per-context batches instead, so both are built here.
	 */
	if (ce->wa_bb_page != 0U) {
		i915_lrc_setup_indirect_ctx_bb(ce);
		i915_lrc_setup_per_ctx_bb(ce);
	}

	/*
	 * The low dword of the descriptor carries CTX_DESC_FORCE_RESTORE until
	 * the first submission clears it; the high dword is filled when the
	 * context is scheduled in.
	 */
	ce->lrca = i915_lrc_descriptor(ce);
	lrca = ce->lrca | (uint32_t)CTX_DESC_FORCE_RESTORE;
	ce->lrc_desc = (ce->lrc_desc & 0xffffffff00000000ULL) | (uint64_t)lrca;

	/* Succeeded: reports the descriptor's low dword as the reference returns it. */
	return lrca;
}

/*
 * Reports the AUX table invalidate register of an engine, or zero.
 *
 * This is gen12_get_aux_inv_reg() for the engines of Alder Lake-P, each of
 * which has one.
 */
uint32_t
drv_i915_lrc_aux_inv_reg(
	int engine_id)
{
	/* Chooses the register by the engine. */
	switch (engine_id) {
	case I915_RCS0:
		return GEN12_CCS_AUX_INV;
	case I915_BCS0:
		return GEN12_BCS0_AUX_INV;
	case I915_VCS0:
		return GEN12_VD0_AUX_INV;
	case I915_VCS2:
		return GEN12_VD2_AUX_INV;
	case I915_VECS0:
		return GEN12_VE0_AUX_INV;
	default:
		break;
	}

	/* Any other engine has no AUX table to invalidate. */
	return 0U;
}

/*
 * Gives back a context's ring and image.
 */
void
drv_i915_lrc_release(
	struct i915_gt_context *ce,
	struct i915_gt_mem *gm)
{
	/* Nothing to release without a context or a pool. */
	if (ce == NULL || gm == NULL)
		return;

	/* Destroys the ring object. */
	if (ce->ring.obj != NULL) {
		drv_i915_gt_object_destroy(gm, ce->ring.obj);
		ce->ring.obj = NULL;
	}

	/* Destroys the image object. */
	if (ce->state != NULL) {
		drv_i915_gt_object_destroy(gm, ce->state);
		ce->state = NULL;
	}

	/* Forgets the CPU views and marks the context unusable. */
	ce->ring.vaddr = NULL;
	ce->lrc_reg_state = NULL;
	ce->allocated = 0;
}

/* Allocates and binds the image, then the ring, and describes the empty ring. */
static int
i915_lrc_alloc_objects(
	struct i915_gt_context *ce,
	struct i915_gt_mem *gm,
	uint32_t ring_size)
{
	int error;

	/* Allocates the context image. */
	ce->state = drv_i915_gt_object_create(gm, ce->state_bytes);
	if (ce->state == NULL)
		return ENOMEM;

	/* Binds the image into the GGTT, where the descriptor names it. */
	error = drv_i915_gt_ggtt_bind(gm, ce->state);
	if (error != 0)
		return error;

	/* The register state starts after the per-process status page. */
	ce->lrc_reg_state = (uint32_t *)((char *)ce->state->cpu + LRC_STATE_OFFSET);

	/* Refuses a ring whose size is not a power of two (intel_engine_create_ring()). */
	if ((ring_size & (ring_size - 1U)) != 0U)
		return EINVAL;

	/* Allocates the ring. */
	ce->ring.obj = drv_i915_gt_object_create(gm, ring_size);
	if (ce->ring.obj == NULL)
		return ENOMEM;

	/* Binds the ring into the GGTT, where the image names it. */
	error = drv_i915_gt_ggtt_bind(gm, ce->ring.obj);
	if (error != 0)
		return error;

	/* Describes the empty ring. */
	ce->ring.vaddr = (uint32_t *)ce->ring.obj->cpu;
	ce->ring.size = ring_size;
	ce->ring.ggtt_offset = ce->ring.obj->ggtt_offset;
	ce->ring.head = 0U;
	ce->ring.tail = 0U;
	ce->ring.emit = 0U;

	/* A fresh ring has everything but one qword free (intel_ring_update_space()). */
	ce->ring.space = ring_size - 8U;

	/* Succeeded: the image and the ring are allocated and bound. */
	return 0;
}

/* Counts the set bits of a byte. */
static unsigned
i915_hweight8(
	uint8_t value)
{
	unsigned count;

	/* Adds the low bit and shifts it out until no bit is left. */
	count = 0U;
	while (value != 0U) {
		count += (unsigned)(value & 1U);
		value = (uint8_t)(value >> 1);
	}

	/* Reports the number of set bits. */
	return count;
}

/* Sizes a context image as __lrc_alloc_state() does and names the first batch page. */
static uint32_t
i915_lrc_state_size(
	uint32_t context_size,
	unsigned *wa_bb_page)
{
	uint32_t size;

	/* Rounds the engine's context up to whole pages; no debug redzone page is added. */
	size = (context_size + 4095U) & ~4095U;

	/* The INDIRECT_CTX page is the first page after the context proper. */
	if (wa_bb_page != NULL)
		*wa_bb_page = size / I915_LRC_PAGE_BYTES;

	/* Gen12 adds one page for INDIRECT_CTX and one for PER_CTX_BB. */
	size += I915_LRC_PAGE_BYTES * 2U;

	/* Reports the whole image size. */
	return size;
}

/*
 * Lays the LRI headers and register offsets of an encoded table into the
 * register state, as the reference's set_offsets() does.
 */
static unsigned
i915_lrc_set_offsets(
	uint32_t *regs,
	const uint8_t *data,
	uint32_t mmio_base,
	int close)
{
	uint32_t *start;
	uint32_t offset;
	uint8_t count;
	uint8_t flags;
	uint8_t byte;

	/* Walks the encoded table until its terminating zero. */
	start = regs;
	while (*data != 0U) {
		/* A byte with the high bit set skips that many state dwords. */
		if ((*data & 0x80U) != 0U) {
			count = (uint8_t)(*data & (uint8_t)~0x80U);
			data++;
			regs += count;
			continue;
		}

		/* Decodes the register count and the flags of one LRI header. */
		count = (uint8_t)(*data & 0x3fU);
		flags = (uint8_t)(*data >> 6);
		data++;

		/* Writes the header: posted when the table says so, CS-relative on Gen11+. */
		*regs = MI_LOAD_REGISTER_IMM(count);
		if ((flags & I915_LRC_POSTED) != 0U)
			*regs |= MI_LRI_FORCE_POSTED;
		*regs |= MI_LRI_LRM_CS_MMIO;
		regs++;

		/* Writes the offset of every register the header loads, leaving its value slot. */
		do {
			/* Decodes seven-bit groups, high group first, while bit 7 continues. */
			offset = 0U;
			do {
				byte = *data;
				data++;
				offset <<= 7;
				offset |= (uint32_t)(byte & (uint8_t)~0x80U);
			} while ((byte & 0x80U) != 0U);

			/* Names the register; its value slot follows it. */
			regs[0] = mmio_base + (offset << 2);
			regs += 2;
			count--;
		} while (count != 0U);
	}

	/*
	 * The reference closes the batch only when it also inhibited the
	 * restore (set_offsets() is called with close equal to inhibit); Gen11+
	 * sets bit 0 of the terminator.
	 */
	if (close != 0)
		*regs = MI_BATCH_BUFFER_END | 1U;

	/* Reports how many state dwords the table laid out. */
	return (unsigned)(regs - start);
}

/*
 * Builds the render power clock state of Gen12 (intel_sseu_make_rpcs()).
 */
static uint32_t
i915_sseu_make_rpcs(
	uint8_t slice_mask,
	int has_slice_pg)
{
	uint32_t slices;
	uint32_t rpcs;

	/*
	 * Gen12 reports only slice power gating, so neither the subslice nor
	 * the EU fields are emitted; the subslice count field is three bits wide
	 * and the reference documents that path as Gen11-specific.
	 */
	if (has_slice_pg == 0)
		return 0U;

	/* Requests every enabled slice. */
	slices = (uint32_t)i915_hweight8(slice_mask);
	slices <<= GEN11_RPCS_S_CNT_SHIFT;
	slices &= GEN11_RPCS_S_CNT_MASK;

	/* Enables the request and the slice count. */
	rpcs = 0U;
	rpcs |= GEN8_RPCS_ENABLE | GEN8_RPCS_S_CNT_ENABLE | slices;

	/* Reports the power clock state word. */
	return rpcs;
}

/* Builds the register state of the image (__lrc_init_regs()). */
static void
i915_lrc_init_regs(
	struct i915_gt_context *ce,
	int inhibit)
{
	const uint8_t *offsets;

	/* A context without an image has no register state. */
	if (ce == NULL || ce->allocated == 0)
		return;

	/* Starts an inhibited image from a clean register page. */
	if (inhibit != 0)
		kern_memset(ce->lrc_reg_state, 0, I915_LRC_PAGE_BYTES);

	/* Chooses the layout by the engine class. */
	offsets = gen12_xcs_offsets;
	if (ce->ge->info->class == I915_RENDER_CLASS)
		offsets = gen12_rcs_offsets;

	/* Lays out the LRI headers and register offsets. */
	ce->reg_state_dwords = i915_lrc_set_offsets(ce->lrc_reg_state,
						    offsets,
						    ce->ge->info->mmio_base,
						    inhibit);

	/* Fills the context control, the timestamp and the page directory. */
	i915_lrc_init_common_regs(ce, inhibit);
	i915_lrc_init_ppgtt_regs(ce);

	/*
	 * init_wa_bb_regs() acts only when the engine has a global workaround
	 * batch; Gen12 installs its batches from drv_i915_lrc_update_regs().
	 */

	/* Clears STOP_RING in the saved MI_MODE. */
	i915_lrc_reset_stop_ring(ce);
}

/* Fills the context control, the timestamp and the batch offset (init_common_regs()). */
static void
i915_lrc_init_common_regs(
	struct i915_gt_context *ce,
	int inhibit)
{
	uint32_t *regs;
	uint32_t control;

	/*
	 * Synchronous context switches are inhibited; the restore is inhibited
	 * only for an image that has nothing to restore.  The pre-Gen11 bits and
	 * the run-alone bit of a protected context do not apply.
	 */
	control = I915_MASKED_ENABLE(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH);
	control |= I915_MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT);
	if (inhibit != 0)
		control |= CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT;

	/* Writes the control word and a zero runtime (ce->stats.runtime.last). */
	regs = ce->lrc_reg_state;
	regs[CTX_CONTEXT_CONTROL] = control;
	regs[CTX_TIMESTAMP] = 0U;

	/* Clears the batch offset, whose slot exists on Gen12. */
	regs[I915_LRC_BB_OFFSET_INDEX + 1] = 0U;
}

/* Names the address space's top directory in the image (init_ppgtt_regs()). */
static void
i915_lrc_init_ppgtt_regs(
	struct i915_gt_context *ce)
{
	uint32_t *regs;
	uint64_t address;

	/*
	 * A 4-level address space is named by PDP0 alone (ASSIGN_CTX_PML4);
	 * writing the other PDP pairs would be the 3-level layout, not extra
	 * safety.
	 */
	regs = ce->lrc_reg_state;
	address = ce->vm->top_pd_dma;
	regs[CTX_PDP0_UDW] = (uint32_t)(address >> 32);
	regs[CTX_PDP0_LDW] = (uint32_t)address;
}

/* Clears STOP_RING in the saved MI_MODE and masks it in (__reset_stop_ring()). */
static void
i915_lrc_reset_stop_ring(
	struct i915_gt_context *ce)
{
	uint32_t *regs;

	/* The restored MI_MODE lets the ring run. */
	regs = ce->lrc_reg_state;
	regs[I915_LRC_MI_MODE_INDEX + 1] &= ~STOP_RING;
	regs[I915_LRC_MI_MODE_INDEX + 1] |= STOP_RING << 16;
}

/* Returns the CPU view of the INDIRECT_CTX page, or of the PER_CTX_BB page. */
static uint32_t *
i915_lrc_wa_bb(
	struct i915_gt_context *ce,
	int per_ctx)
{
	char *page;

	/* The INDIRECT_CTX page comes first and PER_CTX_BB follows it. */
	page = (char *)ce->state->cpu;
	page += ce->wa_bb_page * I915_LRC_PAGE_BYTES;
	if (per_ctx != 0)
		page += I915_LRC_PAGE_BYTES;

	/* Reports the start of the page. */
	return (uint32_t *)page;
}

/* Returns the GGTT address of the INDIRECT_CTX page. */
static uint32_t
i915_lrc_indirect_bb(
	const struct i915_gt_context *ce)
{
	/* The page sits wa_bb_page pages into the image. */
	return (uint32_t)ce->state->ggtt_offset + ce->wa_bb_page * I915_LRC_PAGE_BYTES;
}

/*
 * Reloads the context timestamp from the saved state through GPR0
 * (gen12_emit_timestamp_wa()).
 */
static uint32_t *
i915_lrc_emit_timestamp_wa(
	struct i915_gt_context *ce,
	uint32_t *cs)
{
	uint32_t base;

	/* The registers are relative to the engine's base. */
	base = ce->ge->info->mmio_base;

	/* Loads GPR0 with the saved timestamp. */
	*cs++ = MI_LOAD_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT | MI_LRI_LRM_CS_MMIO;
	*cs++ = GEN8_RING_CS_GPR(base, 0);
	*cs++ = (uint32_t)ce->state->ggtt_offset + LRC_STATE_OFFSET + CTX_TIMESTAMP * 4U;
	*cs++ = 0U;

	/* Copies GPR0 into the context timestamp. */
	*cs++ = MI_LOAD_REGISTER_REG | MI_LRR_SOURCE_CS_MMIO | MI_LRI_LRM_CS_MMIO;
	*cs++ = GEN8_RING_CS_GPR(base, 0);
	*cs++ = RING_CTX_TIMESTAMP(base);

	/* Copies it a second time: the register needs the write twice. */
	*cs++ = MI_LOAD_REGISTER_REG | MI_LRR_SOURCE_CS_MMIO | MI_LRI_LRM_CS_MMIO;
	*cs++ = GEN8_RING_CS_GPR(base, 0);
	*cs++ = RING_CTX_TIMESTAMP(base);

	/* Reports where the next command goes. */
	return cs;
}

/* Restores CMD_BUF_CCTL from the saved state through GPR0 (gen12_emit_cmd_buf_wa()). */
static uint32_t *
i915_lrc_emit_cmd_buf_wa(
	struct i915_gt_context *ce,
	uint32_t *cs)
{
	uint32_t base;

	/* The registers are relative to the engine's base. */
	base = ce->ge->info->mmio_base;

	/* Loads GPR0 with the saved CMD_BUF_CCTL. */
	*cs++ = MI_LOAD_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT | MI_LRI_LRM_CS_MMIO;
	*cs++ = GEN8_RING_CS_GPR(base, 0);
	*cs++ = (uint32_t)ce->state->ggtt_offset + LRC_STATE_OFFSET + (I915_LRC_RING_CMD_BUF_CCTL + 1) * 4U;
	*cs++ = 0U;

	/* Copies GPR0 into CMD_BUF_CCTL. */
	*cs++ = MI_LOAD_REGISTER_REG | MI_LRR_SOURCE_CS_MMIO | MI_LRI_LRM_CS_MMIO;
	*cs++ = GEN8_RING_CS_GPR(base, 0);
	*cs++ = RING_CMD_BUF_CCTL(base);

	/* Reports where the next command goes. */
	return cs;
}

/* Restores GPR0 itself from the saved state (gen12_emit_restore_scratch()). */
static uint32_t *
i915_lrc_emit_restore_scratch(
	struct i915_gt_context *ce,
	uint32_t *cs)
{
	uint32_t base;

	/* The registers are relative to the engine's base. */
	base = ce->ge->info->mmio_base;

	/* Loads GPR0 with its own saved value. */
	*cs++ = MI_LOAD_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT | MI_LRI_LRM_CS_MMIO;
	*cs++ = GEN8_RING_CS_GPR(base, 0);
	*cs++ = (uint32_t)ce->state->ggtt_offset + LRC_STATE_OFFSET + (I915_LRC_RING_GPR0 + 1) * 4U;
	*cs++ = 0U;

	/* Reports where the next command goes. */
	return cs;
}

/* Invalidates the instruction state cache (Wa_18022495364, IP 12.0 to 12.10). */
static uint32_t *
i915_lrc_emit_invalidate_state_cache(
	uint32_t *cs)
{
	/* Sets the invalidate bit of CS_DEBUG_MODE2. */
	*cs++ = MI_LOAD_REGISTER_IMM(1);
	*cs++ = GEN12_CS_DEBUG_MODE2;
	*cs++ = I915_MASKED_ENABLE(INSTRUCTION_STATE_CACHE_INVALIDATE);

	/* Reports where the next command goes. */
	return cs;
}

/*
 * Writes the predicate-disable batch at its fixed offset in the INDIRECT_CTX
 * page (setup_predicate_disable_wa()).
 *
 * The offset lies outside the executed range -- the indirect size covers only
 * the emitted commands -- so on Alder Lake-P these dwords are present but never
 * run.  They are written so the page matches the reference byte for byte.
 */
static void
i915_lrc_setup_predicate_disable_wa(
	struct i915_gt_context *ce,
	uint32_t *cs)
{
	/* Clears the predicate result: no predication. */
	*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
	*cs++ = i915_lrc_indirect_bb(ce) + DG2_PREDICATE_RESULT_WA;
	*cs++ = 0U;
	*cs++ = 0U;

	/* Ends the predicated batch and disables predication. */
	*cs++ = MI_BATCH_BUFFER_END | (1U << 15);
	*cs++ = MI_SET_PREDICATE | MI_SET_PREDICATE_DISABLE;

	/* Sets the predicate result: predication is enabled before the next batch. */
	*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
	*cs++ = i915_lrc_indirect_bb(ce) + DG2_PREDICATE_RESULT_WA;
	*cs++ = 0U;
	*cs++ = 1U;

	/* Ends the batch. */
	*cs++ = MI_BATCH_BUFFER_END;
}

/* Builds the INDIRECT_CTX batch and points the image at it (lrc_setup_indirect_ctx()). */
static void
i915_lrc_setup_indirect_ctx_bb(
	struct i915_gt_context *ce)
{
	uint32_t *start;
	uint32_t *regs;
	uint32_t *cs;
	uint32_t size;

	/* Writes from the start of the INDIRECT_CTX page into the register state. */
	start = i915_lrc_wa_bb(ce, 0);
	regs = ce->lrc_reg_state;

	/* Reloads the timestamp; the render engine also restores CMD_BUF_CCTL. */
	cs = start;
	cs = i915_lrc_emit_timestamp_wa(ce, cs);
	if (ce->ge->info->class == I915_RENDER_CLASS)
		cs = i915_lrc_emit_cmd_buf_wa(ce, cs);

	/* Restores the scratch register the two reloads used. */
	cs = i915_lrc_emit_restore_scratch(ce, cs);

	/*
	 * Invalidates the AUX table and waits for it (Wa_16013000631 is DG2_G11
	 * only).  The ring flush uses the same emitter, so the two cannot diverge.
	 */
	cs = drv_i915_gen12_emit_aux_table_inv(ce->ge->info->id, cs);

	/* The render engine also invalidates its state cache (Wa_16014892111 does not apply). */
	if (ce->ge->info->class == I915_RENDER_CLASS)
		cs = i915_lrc_emit_invalidate_state_cache(cs);

	/* Pads the executed size to a whole number of cachelines. */
	while ((((uintptr_t)cs - (uintptr_t)start) % I915_CACHELINE_BYTES) != 0U)
		*cs++ = MI_NOOP;

	/* Measures the executed batch, then writes the predicate batch past it. */
	size = (uint32_t)((cs - start) * 4U);
	i915_lrc_setup_predicate_disable_wa(ce, start + DG2_PREDICATE_RESULT_BB / 4U);

	/* Names the batch and its size in cachelines, and the default offset. */
	regs[I915_LRC_RING_INDIRECT_PTR + 1] = i915_lrc_indirect_bb(ce) | (size / I915_CACHELINE_BYTES);
	regs[I915_LRC_RING_INDIRECT_OFFSET + 1] = GEN12_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT << I915_INDIRECT_CTX_OFFSET_SHIFT;

	/* Records where the batch is and how much of it runs. */
	ce->indirect_bb_ggtt = i915_lrc_indirect_bb(ce);
	ce->indirect_bb_dwords = (unsigned)(cs - start);
}

/* Builds the PER_CTX_BB batch and points the image at it. */
static void
i915_lrc_setup_per_ctx_bb(
	struct i915_gt_context *ce)
{
	uint32_t *regs;
	uint32_t *cs;

	/*
	 * The fastcolor BLT workaround is Xe_HP only, so on Alder Lake-P the
	 * batch is only its terminator -- which PER_CTX_BB must still carry.
	 */
	cs = i915_lrc_wa_bb(ce, 1);
	*cs = MI_BATCH_BUFFER_END;

	/* Names the batch, to be run on every restore. */
	regs = ce->lrc_reg_state;
	regs[I915_LRC_RING_WA_BB_PER_CTX + 1] = (i915_lrc_indirect_bb(ce) + I915_LRC_PAGE_BYTES) |
						I915_PER_CTX_BB_FORCE |
						I915_PER_CTX_BB_VALID;

	/* Records that the image names its PER_CTX_BB batch. */
	ce->per_ctx_bb_set = 1;
}

/* Builds the descriptor's low dword: the image address and the context flags. */
static uint32_t
i915_lrc_descriptor(
	const struct i915_gt_context *ce)
{
	uint32_t flags;

	/*
	 * The context always has a 4-level address space on Alder Lake-P; the
	 * L3LLC coherence bit is Gen8 only.
	 */
	flags = INTEL_LEGACY_64B_CONTEXT;
	flags <<= GEN8_CTX_ADDRESSING_MODE_SHIFT;
	flags |= GEN8_CTX_VALID | GEN8_CTX_PRIVILEGE;

	/* Reports the image address with the flags. */
	return (uint32_t)ce->state->ggtt_offset | flags;
}
