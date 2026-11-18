/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Logical ring contexts (see context.h).
 *
 * What differs between the generations is kept in one layout record per
 * generation: the register-state tables, where the slots this file writes
 * sit, how the load-register headers are encoded, the context control bits,
 * the render power clock state and the workaround batches the context
 * carries.  The functions below work only through that record.
 */

#include "i915.h"
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
 * Where the Gen9 coherent-L3 workaround keeps L3SQCREG4 while it flushes:
 * a dword near the end of the context's own INDIRECT_CTX page, which the
 * batch never reaches.
 */
#define I915_GEN9_L3SQCREG4_SAVE_OFFSET		0xf00U

/* Where the INDIRECT_CTX offset field starts in its register. */
#define I915_INDIRECT_CTX_OFFSET_SHIFT	6

/* The PER_CTX_BB pointer flags: run the batch on every restore, and it is valid. */
#define I915_PER_CTX_BB_FORCE		(1U << 2)
#define I915_PER_CTX_BB_VALID		(1U << 0)

/*
 * The register-state layout of one generation.
 *
 * The slots are register-state indices of a register's offset dword; its
 * value is the dword after it.
 */
struct i915_lrc_layout {
	/* The encoded register tables of the render engine and of every other engine. */
	const uint8_t *render_table;
	const uint8_t *other_table;

	/* Nonzero: the load-register headers are engine-relative and the terminator has bit 0. */
	int cs_relative;

	/* The saved MI_MODE, the batch offset, and the INDIRECT_CTX pointer and offset. */
	uint32_t slot_mi_mode;
	uint32_t slot_bb_offset;
	uint32_t slot_indirect_ptr;
	uint32_t slot_indirect_offset;

	/* The default INDIRECT_CTX offset of the render engine. */
	uint32_t indirect_offset_default;

	/* Context control bits cleared in addition to the restore inhibit. */
	uint32_t control_cleared;

	/* Builds the render power clock state. */
	uint32_t (*power_state)(const struct i915_gt_engine *ge);

	/* Writes the workaround batches the context carries and names them in the image. */
	void (*write_batches)(struct i915_gt_context *ce);
};

static uint32_t i915_lrc_power_state_gen12(const struct i915_gt_engine *ge);
static uint32_t i915_lrc_power_state_gen11(const struct i915_gt_engine *ge);
static uint32_t i915_lrc_power_state_gen9(const struct i915_gt_engine *ge);
static void i915_lrc_batches_gen12(struct i915_gt_context *ce);
static void i915_lrc_batches_gen11(struct i915_gt_context *ce);
static void i915_lrc_batches_gen9(struct i915_gt_context *ce);

/*
 * The Gen12 layout.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.c
 * (lrc_ring_mi_mode, lrc_ring_bb_offset, lrc_ring_indirect_ptr,
 * lrc_ring_indirect_offset, lrc_ring_wa_bb_per_ctx, lrc_ring_gpr0,
 * lrc_ring_cmd_buf_cctl: the GRAPHICS_VER 12 values).
 */
static const struct i915_lrc_layout i915_lrc_gen12 = {
	i915_lrc_gen12_rcs, i915_lrc_gen12_xcs, 1,
	0x60U, 0x70U, 0x14U, 0x16U,
	I915_GT_GEN12_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT,
	0U,
	i915_lrc_power_state_gen12,
	i915_lrc_batches_gen12
};

/*
 * The Gen11 layout: the Gen11 render table and the Gen9 table for the other
 * engines, the engine-relative load-register headers of Gen11, the Gen9
 * slots, and no context workaround batch.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_lrc.c
 * (reg_offsets, set_offsets, lrc_ring_mi_mode, lrc_ring_bb_offset,
 * lrc_ring_indirect_ptr, lrc_ring_indirect_offset,
 * lrc_ring_indirect_offset_default, init_common_regs: the GRAPHICS_VER 11
 * values).
 */
static const struct i915_lrc_layout i915_lrc_gen11 = {
	i915_lrc_gen11_rcs, i915_lrc_gen9_xcs, 1,
	0x54U, 0x64U, 0x1aU, 0x1cU,
	I915_GT_GEN11_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT,
	0U,
	i915_lrc_power_state_gen11,
	i915_lrc_batches_gen11
};

/*
 * The Gen9 layout.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.c
 * (lrc_ring_mi_mode, lrc_ring_bb_offset, lrc_ring_indirect_ptr,
 * lrc_ring_indirect_offset: the GRAPHICS_VER 9 values).
 */
static const struct i915_lrc_layout i915_lrc_gen9_layout = {
	i915_lrc_gen9_rcs, i915_lrc_gen9_xcs, 0,
	0x54U, 0x64U, 0x1aU, 0x1cU,
	I915_GT_GEN9_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT,
	I915_GT_CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT | I915_GT_CTX_CTRL_RS_CTX_ENABLE,
	i915_lrc_power_state_gen9,
	i915_lrc_batches_gen9
};

/* The Gen12 PER_CTX_BB pointer, GPR0 and CMD_BUF_CCTL (render only) slots. */
#define I915_LRC_GEN12_SLOT_PER_CTX		0x12U
#define I915_LRC_GEN12_SLOT_GPR0		0x74U
#define I915_LRC_GEN12_SLOT_CMD_BUF_CCTL	0xb6U

static const struct i915_lrc_layout *i915_lrc_layout_of(const struct i915_gt_context *ce);
static int i915_lrc_alloc_objects(struct i915_gt_context *ce, struct i915_gt_mem *gm, uint32_t ring_size);
static unsigned i915_lrc_count_bits(uint8_t value);
static uint32_t i915_lrc_state_size(uint32_t context_size, unsigned *batch_page);
static uint32_t i915_lrc_table_offset(const uint8_t **data);
static unsigned i915_lrc_lay_table(uint32_t *regs, const uint8_t *data, uint32_t mmio_base, int terminate, int cs_relative);
static void i915_lrc_lay_regs(struct i915_gt_context *ce, int inhibit);
static uint32_t *i915_lrc_batch_page(struct i915_gt_context *ce, unsigned which);
static uint32_t i915_lrc_batch_address(const struct i915_gt_context *ce);
static uint32_t i915_lrc_saved_value(const struct i915_gt_context *ce, uint32_t index);
static uint32_t *i915_lrc_emit_load_gpr0(const struct i915_gt_context *ce, uint32_t *cs, uint32_t address);
static uint32_t *i915_lrc_emit_copy_gpr0(const struct i915_gt_context *ce, uint32_t *cs, uint32_t reg);
static uint32_t *i915_lrc_emit_pipe_control(uint32_t *cs, uint32_t flags, uint32_t offset);
static uint32_t *i915_lrc_pad(uint32_t *start, uint32_t *cs);
static void i915_lrc_name_indirect(struct i915_gt_context *ce, const struct i915_lrc_layout *layout, uint32_t *start, uint32_t *end);
static void i915_lrc_write_predicate(struct i915_gt_context *ce, uint32_t *cs);
static uint32_t i915_lrc_descriptor(const struct i915_gt_context *ce);

/*
 * Allocates a context image and its ring and binds both into the GGTT.
 *
 * The image is the engine's context rounded up to pages plus the two batch
 * pages; ring_size must be a power of two.  Nothing is written into the
 * image yet.  Returns 0, EINVAL, ENOMEM or the GGTT bind error; on failure
 * nothing stays allocated.
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
 * Builds a context image.
 *
 * An engine that has recorded its default state gives the image that state
 * and the restore is allowed; otherwise the restore is inhibited and the
 * engine starts the context from its own register state.  The per-process
 * status page and the INDIRECT_CTX page are cleared either way.
 */
void
drv_i915_lrc_build_image(
	struct i915_gt_context *ce)
{
	const struct i915_gt_object *defaults;
	uint32_t copy_bytes;
	int inhibit;

	/* A context without an image has nothing to build. */
	if (ce == NULL || ce->allocated == 0)
		return;

	/* Without a recorded default state there is nothing to restore. */
	inhibit = 1;

	/* Copies the recorded default state, bounded by both images; it may be restored. */
	defaults = ce->ge->default_state;
	if (defaults != NULL && defaults->cpu != NULL) {
		copy_bytes = ce->ge->info->context_size;
		if (copy_bytes > defaults->bytes)
			copy_bytes = defaults->bytes;
		if (copy_bytes > ce->state_bytes)
			copy_bytes = ce->state_bytes;
		kern_memcpy(ce->state->cpu, defaults->cpu, copy_bytes);
		inhibit = 0;
	}

	/* Clears the per-process status page, including the per-context counters. */
	kern_memset(ce->state->cpu, 0, I915_LRC_PAGE_BYTES);

	/* Clears the indirect workaround and storage page. */
	if (ce->wa_bb_page != 0U)
		kern_memset(i915_lrc_batch_page(ce, 0U), 0, I915_LRC_PAGE_BYTES);

	/* Lays out and fills the register state. */
	i915_lrc_lay_regs(ce, inhibit);
}

/*
 * Scrubs a context after a reset.
 *
 * The ring is emptied at its write position, the register state is rebuilt
 * with the restore inhibited, and the ring registers are written again.
 */
void
drv_i915_lrc_scrub(
	struct i915_gt_context *ce)
{
	/* A context without an image has nothing to scrub. */
	if (ce == NULL || ce->allocated == 0)
		return;

	/* Empties the ring at its write position. */
	ce->ring.head = ce->ring.emit;
	ce->ring.tail = ce->ring.emit;
	ce->ring.space = (ce->ring.head - ce->ring.emit - I915_CACHELINE_BYTES) & (ce->ring.size - 1U);

	/* Rebuilds the register state from a clean page. */
	i915_lrc_lay_regs(ce, 1);

	/* Writes the ring registers for the empty ring; the descriptor stays in the context. */
	(void)drv_i915_lrc_set_ring(ce, ce->ring.tail);
}

/*
 * Writes the ring registers, the render power state and the workaround
 * batches into a context image, and builds the descriptor.
 *
 * Returns the lower dword of the descriptor with
 * I915_GT_CTX_DESC_FORCE_RESTORE; zero for a context without an image.
 */
uint32_t
drv_i915_lrc_set_ring(
	struct i915_gt_context *ce,
	uint32_t head)
{
	const struct i915_lrc_layout *layout;
	uint32_t *regs;
	uint32_t low;

	/* A context without an image has no registers to write. */
	if (ce == NULL || ce->allocated == 0)
		return 0U;

	/* Points the image at the ring and gives it the ring's positions and size. */
	layout = i915_lrc_layout_of(ce);
	regs = ce->regs;
	regs[I915_GT_CTX_RING_START] = (uint32_t)ce->ring.ggtt_offset;
	regs[I915_GT_CTX_RING_HEAD] = head;
	regs[I915_GT_CTX_RING_TAIL] = ce->ring.tail;
	regs[I915_GT_CTX_RING_CTL] = I915_GT_RING_CTL_SIZE(ce->ring.size) | I915_GT_RING_VALID;

	/*
	 * Without the power clock state the render engine may come up with its
	 * execution units power gated: the fixed-function stages run and the
	 * first thread never dispatches.
	 */
	if (ce->ge->info->class == I915_RENDER_CLASS)
		regs[I915_GT_CTX_R_PWR_CLK_STATE] = layout->power_state(ce->ge);

	/* The workaround batches live in the context's own pages. */
	if (ce->wa_bb_page != 0U)
		layout->write_batches(ce);

	/*
	 * The low dword of the descriptor carries I915_GT_CTX_DESC_FORCE_RESTORE
	 * until the first submission clears it; the high dword is filled when
	 * the context is scheduled in.
	 */
	ce->lrca = i915_lrc_descriptor(ce);
	low = ce->lrca | (uint32_t)I915_GT_CTX_DESC_FORCE_RESTORE;
	ce->desc = (ce->desc & 0xffffffff00000000ULL) | (uint64_t)low;

	/* Succeeded: reports the descriptor's low dword. */
	return low;
}

/*
 * Reports the AUX table invalidate register of an engine, or zero for an
 * engine without one.
 */
uint32_t
drv_i915_lrc_aux_inv_reg(
	int engine_id)
{
	/* Chooses the register by the engine. */
	switch (engine_id) {
	case I915_RCS0:
		return I915_GT_GEN12_CCS_AUX_INV;
	case I915_BCS0:
		return I915_GT_GEN12_BCS0_AUX_INV;
	case I915_VCS0:
		return I915_GT_GEN12_VD0_AUX_INV;
	case I915_VCS2:
		return I915_GT_GEN12_VD2_AUX_INV;
	case I915_VECS0:
		return I915_GT_GEN12_VE0_AUX_INV;
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
	ce->regs = NULL;
	ce->allocated = 0;
}

/* Returns the register-state layout of a context's engine. */
static const struct i915_lrc_layout *
i915_lrc_layout_of(
	const struct i915_gt_context *ce)
{
	/* A Gen9 engine has the Gen9 layout. */
	if (ce->ge->info->graphics_ver == 9)
		return &i915_lrc_gen9_layout;

	/* A Gen11 engine has the Gen11 layout. */
	if (ce->ge->info->graphics_ver == 11)
		return &i915_lrc_gen11;

	/* Every other engine has the Gen12 layout. */
	return &i915_lrc_gen12;
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
	ce->regs = (uint32_t *)((char *)ce->state->cpu + I915_GT_LRC_STATE_OFFSET);

	/* Refuses a ring whose size is not a power of two. */
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

	/* Describes the empty ring: everything but one qword is free. */
	ce->ring.vaddr = (uint32_t *)ce->ring.obj->cpu;
	ce->ring.size = ring_size;
	ce->ring.ggtt_offset = ce->ring.obj->ggtt_offset;
	ce->ring.head = 0U;
	ce->ring.tail = 0U;
	ce->ring.emit = 0U;
	ce->ring.space = ring_size - 8U;

	/* Succeeded: the image and the ring are allocated and bound. */
	return 0;
}

/* Counts the set bits of a byte. */
static unsigned
i915_lrc_count_bits(
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

/*
 * Sizes a context image and names its first batch page: the engine's
 * context rounded up to whole pages, then the INDIRECT_CTX page and the
 * PER_CTX_BB page.
 */
static uint32_t
i915_lrc_state_size(
	uint32_t context_size,
	unsigned *batch_page)
{
	uint32_t size;

	/* Rounds the engine's context up to whole pages. */
	size = (context_size + 4095U) & ~4095U;

	/* The INDIRECT_CTX page is the first page after the context proper. */
	if (batch_page != NULL)
		*batch_page = size / I915_LRC_PAGE_BYTES;

	/* Reports the whole image size, with the two batch pages. */
	return size + I915_LRC_PAGE_BYTES * 2U;
}

/* Reads one register offset of an encoded table: seven-bit groups, high group first, while bit 7 continues. */
static uint32_t
i915_lrc_table_offset(
	const uint8_t **data)
{
	uint32_t offset;
	uint8_t byte;

	/* Collects the groups. */
	offset = 0U;
	do {
		byte = **data;
		(*data)++;
		offset = (offset << 7) | (uint32_t)(byte & 0x7fU);
	} while ((byte & 0x80U) != 0U);

	/* Succeeded: the offset in dwords from the engine's base. */
	return offset;
}

/*
 * Lays an encoded register table into the register state: the load-register
 * headers and the register offsets, leaving each value slot alone.
 *
 * The table is a list of entries ending in zero.  An entry with bit 7 set
 * skips that many dwords; any other is a header byte (register count in
 * bits 0 to 5, flags in bits 6 and 7) followed by the count's offsets.
 * With terminate set, a batch end follows the last entry.  Returns how many
 * dwords the table laid out.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.c
 * (set_offsets: the table encoding and its decode).
 */
static unsigned
i915_lrc_lay_table(
	uint32_t *regs,
	const uint8_t *data,
	uint32_t mmio_base,
	int terminate,
	int cs_relative)
{
	uint32_t *start;
	uint32_t header;
	unsigned count;

	/* Walks the table until its terminating zero. */
	start = regs;
	while (*data != 0U) {
		/* A skip entry. */
		if ((*data & 0x80U) != 0U) {
			regs += *data & 0x7fU;
			data++;
			continue;
		}

		/* A header: posted when the table says so, engine-relative where the layout is. */
		count = *data & 0x3fU;
		header = I915_CMD_MI_LOAD_REGISTER_IMM(count);
		if (((*data >> 6) & I915_LRC_POSTED) != 0U)
			header |= I915_CMD_MI_LRI_FORCE_POSTED;
		if (cs_relative != 0)
			header |= I915_CMD_MI_LRI_LRM_CS_MMIO;
		data++;
		*regs++ = header;

		/* Names each register; its value slot follows it. */
		for (; count != 0U; count--) {
			regs[0] = mmio_base + (i915_lrc_table_offset(&data) << 2);
			regs += 2;
		}
	}

	/* Ends the batch when asked; the engine-relative encoding sets bit 0 of the end. */
	if (terminate != 0) {
		*regs = I915_CMD_MI_BATCH_BUFFER_END;
		if (cs_relative != 0)
			*regs |= 1U;
	}

	/* Reports how many state dwords the table laid out. */
	return (unsigned)(regs - start);
}

/*
 * Builds the register state of the image: the table of the engine, the
 * context control, the timestamp, the batch offset, the page directory and
 * a saved MI_MODE that lets the ring run.
 */
static void
i915_lrc_lay_regs(
	struct i915_gt_context *ce,
	int inhibit)
{
	const struct i915_lrc_layout *layout;
	const uint8_t *table;
	uint32_t *regs;
	uint32_t control;
	uint64_t top;

	/* A context without an image has no register state. */
	if (ce == NULL || ce->allocated == 0)
		return;

	/* Starts an inhibited image from a clean register page. */
	regs = ce->regs;
	if (inhibit != 0)
		kern_memset(regs, 0, I915_LRC_PAGE_BYTES);

	/* Lays out the table of the engine's class; an inhibited image ends after it. */
	layout = i915_lrc_layout_of(ce);
	table = layout->other_table;
	if (ce->ge->info->class == I915_RENDER_CLASS)
		table = layout->render_table;
	ce->reg_state_dwords = i915_lrc_lay_table(regs, table, ce->ge->info->mmio_base, inhibit, layout->cs_relative);

	/*
	 * Synchronous context switches are inhibited; the restore is inhibited
	 * only for an image that has nothing to restore.
	 */
	control = I915_MASKED_ENABLE(I915_GT_CTX_CTRL_INHIBIT_SYN_CTX_SWITCH);
	control |= I915_MASKED_DISABLE(I915_GT_CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT);
	if (inhibit != 0)
		control |= I915_GT_CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT;
	control |= I915_MASKED_DISABLE(layout->control_cleared);

	/* Writes the control word, a zero runtime and a zero batch offset. */
	regs[I915_GT_CTX_CONTEXT_CONTROL] = control;
	regs[I915_GT_CTX_TIMESTAMP] = 0U;
	regs[layout->slot_bb_offset + 1U] = 0U;

	/* A 4-level address space is named by PDP0 alone. */
	top = ce->vm->top_pd_dma;
	regs[I915_GT_CTX_PDP0_UDW] = (uint32_t)(top >> 32);
	regs[I915_GT_CTX_PDP0_LDW] = (uint32_t)top;

	/* The restored MI_MODE lets the ring run: STOP_RING clear, and masked in. */
	regs[layout->slot_mi_mode + 1U] &= ~I915_GT_STOP_RING;
	regs[layout->slot_mi_mode + 1U] |= I915_GT_STOP_RING << 16;
}

/* Returns the CPU view of a batch page: 0 is INDIRECT_CTX, 1 is PER_CTX_BB. */
static uint32_t *
i915_lrc_batch_page(
	struct i915_gt_context *ce,
	unsigned which)
{
	/* The batch pages follow the context proper. */
	return (uint32_t *)((char *)ce->state->cpu + (ce->wa_bb_page + which) * I915_LRC_PAGE_BYTES);
}

/* Returns the GGTT address of the INDIRECT_CTX page. */
static uint32_t
i915_lrc_batch_address(
	const struct i915_gt_context *ce)
{
	/* The page sits wa_bb_page pages into the image. */
	return (uint32_t)ce->state->ggtt_offset + ce->wa_bb_page * I915_LRC_PAGE_BYTES;
}

/* Returns the GGTT address of one register-state dword of the image. */
static uint32_t
i915_lrc_saved_value(
	const struct i915_gt_context *ce,
	uint32_t index)
{
	/* The register state starts after the per-process status page. */
	return (uint32_t)ce->state->ggtt_offset + I915_GT_LRC_STATE_OFFSET + index * 4U;
}

/* Loads GPR0 of the engine from a GGTT address. */
static uint32_t *
i915_lrc_emit_load_gpr0(
	const struct i915_gt_context *ce,
	uint32_t *cs,
	uint32_t address)
{
	/* A load-register-from-memory, engine-relative, through the GGTT. */
	*cs++ = I915_CMD_MI_LOAD_REGISTER_MEM_GEN8 | I915_CMD_MI_SRM_LRM_GLOBAL_GTT | I915_CMD_MI_LRI_LRM_CS_MMIO;
	*cs++ = I915_GT_GEN8_RING_CS_GPR(ce->ge->info->mmio_base, 0);
	*cs++ = address;
	*cs++ = 0U;

	/* Reports where the next command goes. */
	return cs;
}

/* Copies GPR0 of the engine into one of its registers. */
static uint32_t *
i915_lrc_emit_copy_gpr0(
	const struct i915_gt_context *ce,
	uint32_t *cs,
	uint32_t reg)
{
	/* A load-register-from-register, both engine-relative. */
	*cs++ = I915_CMD_MI_LOAD_REGISTER_REG | I915_CMD_MI_LRR_SOURCE_CS_MMIO | I915_CMD_MI_LRI_LRM_CS_MMIO;
	*cs++ = I915_GT_GEN8_RING_CS_GPR(ce->ge->info->mmio_base, 0);
	*cs++ = reg;

	/* Reports where the next command goes. */
	return cs;
}

/* Writes a six-dword PIPE_CONTROL with a post-sync address; the data dwords are zero. */
static uint32_t *
i915_lrc_emit_pipe_control(
	uint32_t *cs,
	uint32_t flags,
	uint32_t offset)
{
	/* The command, its flags, the address and the data. */
	*cs++ = I915_CMD_GFX_OP_PIPE_CONTROL(6);
	*cs++ = flags;
	*cs++ = offset;
	*cs++ = 0U;
	*cs++ = 0U;
	*cs++ = 0U;

	/* Reports where the next command goes. */
	return cs;
}

/* Pads a batch with no-ops to a whole number of cachelines from its start. */
static uint32_t *
i915_lrc_pad(
	uint32_t *start,
	uint32_t *cs)
{
	/* Adds no-ops until the size is a multiple of a cacheline. */
	while ((((uintptr_t)cs - (uintptr_t)start) % I915_CACHELINE_BYTES) != 0U)
		*cs++ = I915_CMD_MI_NOOP;

	/* Reports the padded end. */
	return cs;
}

/* Names the INDIRECT_CTX batch, its size in cachelines and the default offset in the image. */
static void
i915_lrc_name_indirect(
	struct i915_gt_context *ce,
	const struct i915_lrc_layout *layout,
	uint32_t *start,
	uint32_t *end)
{
	uint32_t bytes;

	/* The pointer carries the size in cachelines in its low bits. */
	bytes = (uint32_t)((end - start) * 4U);
	ce->regs[layout->slot_indirect_ptr + 1U] = i915_lrc_batch_address(ce) | (bytes / I915_CACHELINE_BYTES);
	ce->regs[layout->slot_indirect_offset + 1U] = layout->indirect_offset_default << I915_INDIRECT_CTX_OFFSET_SHIFT;

	/* Records where the batch is and how much of it runs. */
	ce->indirect_bb_ggtt = i915_lrc_batch_address(ce);
	ce->indirect_bb_dwords = (unsigned)(end - start);
}

/*
 * Writes the predicate-disable batch at its fixed offset in the INDIRECT_CTX
 * page.
 *
 * The offset lies past the executed range, so these dwords are present but
 * never run; the page holds them so its contents do not depend on the
 * platform.
 */
static void
i915_lrc_write_predicate(
	struct i915_gt_context *ce,
	uint32_t *cs)
{
	uint32_t result;

	/* The predicate result dword near the end of the page. */
	result = i915_lrc_batch_address(ce) + I915_GT_DG2_PREDICATE_RESULT_WA;

	/* Clears the predicate result: no predication. */
	*cs++ = I915_CMD_MI_STORE_DWORD_IMM_GEN4 | I915_CMD_MI_USE_GGTT;
	*cs++ = result;
	*cs++ = 0U;
	*cs++ = 0U;

	/* Ends the predicated batch and disables predication. */
	*cs++ = I915_CMD_MI_BATCH_BUFFER_END | (1U << 15);
	*cs++ = I915_CMD_MI_SET_PREDICATE | I915_CMD_MI_SET_PREDICATE_DISABLE;

	/* Sets the predicate result: predication is enabled before the next batch. */
	*cs++ = I915_CMD_MI_STORE_DWORD_IMM_GEN4 | I915_CMD_MI_USE_GGTT;
	*cs++ = result;
	*cs++ = 0U;
	*cs++ = 1U;

	/* Ends the batch. */
	*cs++ = I915_CMD_MI_BATCH_BUFFER_END;
}

/*
 * Writes the Gen12 batches: INDIRECT_CTX and PER_CTX_BB.
 *
 * The INDIRECT_CTX batch runs as part of every restore.  Through GPR0 it
 * reloads the context timestamp (written twice, which the register needs)
 * and, on the render engine, CMD_BUF_CCTL from their saved values, then
 * restores GPR0 itself; it invalidates the AUX table and waits for it, and
 * the render engine also invalidates its instruction state cache
 * (Wa_18022495364).  PER_CTX_BB holds only its terminator.
 */
static void
i915_lrc_batches_gen12(
	struct i915_gt_context *ce)
{
	uint32_t *start;
	uint32_t *cs;
	uint32_t base;
	int render;

	/* Writes from the start of the INDIRECT_CTX page. */
	start = i915_lrc_batch_page(ce, 0U);
	base = ce->ge->info->mmio_base;
	render = ce->ge->info->class == I915_RENDER_CLASS;

	/* Reloads the context timestamp through GPR0, twice. */
	cs = i915_lrc_emit_load_gpr0(ce, start, i915_lrc_saved_value(ce, I915_GT_CTX_TIMESTAMP));
	cs = i915_lrc_emit_copy_gpr0(ce, cs, I915_GT_RING_CTX_TIMESTAMP(base));
	cs = i915_lrc_emit_copy_gpr0(ce, cs, I915_GT_RING_CTX_TIMESTAMP(base));

	/* The render engine reloads CMD_BUF_CCTL too. */
	if (render != 0) {
		cs = i915_lrc_emit_load_gpr0(ce, cs, i915_lrc_saved_value(ce, I915_LRC_GEN12_SLOT_CMD_BUF_CCTL + 1U));
		cs = i915_lrc_emit_copy_gpr0(ce, cs, I915_GT_RING_CMD_BUF_CCTL(base));
	}

	/* Restores GPR0 from its own saved value. */
	cs = i915_lrc_emit_load_gpr0(ce, cs, i915_lrc_saved_value(ce, I915_LRC_GEN12_SLOT_GPR0 + 1U));

	/* Invalidates the AUX table and waits; the ring flush uses the same emitter. */
	cs = drv_i915_emit_aux_inv(ce->ge->info->id, cs);

	/* The render engine invalidates its instruction state cache. */
	if (render != 0) {
		*cs++ = I915_CMD_MI_LOAD_REGISTER_IMM(1);
		*cs++ = I915_GT_GEN12_CS_DEBUG_MODE2;
		*cs++ = I915_MASKED_ENABLE(I915_GT_INSTRUCTION_STATE_CACHE_INVALIDATE);
	}

	/* Pads the executed part, names it, and writes the predicate batch past it. */
	cs = i915_lrc_pad(start, cs);
	i915_lrc_write_predicate(ce, start + I915_GT_DG2_PREDICATE_RESULT_BB / 4U);
	i915_lrc_name_indirect(ce, &i915_lrc_gen12, start, cs);

	/* PER_CTX_BB is only its terminator, and runs on every restore. */
	*i915_lrc_batch_page(ce, 1U) = I915_CMD_MI_BATCH_BUFFER_END;
	ce->regs[I915_LRC_GEN12_SLOT_PER_CTX + 1U] = (i915_lrc_batch_address(ce) + I915_LRC_PAGE_BYTES) |
						      I915_PER_CTX_BB_FORCE |
						      I915_PER_CTX_BB_VALID;
	ce->per_ctx_bb_set = 1;
}

/*
 * Writes the Gen9 render engine's INDIRECT_CTX batch; the other engines
 * carry none.
 *
 * With arbitration off it saves L3SQCREG4, asks L3 to flush its coherent
 * lines and waits through a data-cache flush, restores L3SQCREG4
 * (WaFlushCoherentL3CacheLinesAtContextSwitch), clears the shared local
 * memory through an L3 flush (WaClearSlmSpaceAtContextSwitch), sets the
 * gather and provoking-vertex chicken bits
 * (WaDisableGatherAtSetShaderCommonSlice), and turns arbitration on again.
 */
static void
i915_lrc_batches_gen9(
	struct i915_gt_context *ce)
{
	uint32_t *start;
	uint32_t *cs;
	uint32_t save;

	/* Only the render engine carries the batch. */
	if (ce->ge->info->class != I915_RENDER_CLASS)
		return;

	/* Writes from the start of the INDIRECT_CTX page; L3SQCREG4 is kept near its end. */
	start = i915_lrc_batch_page(ce, 0U);
	save = i915_lrc_batch_address(ce) + I915_GEN9_L3SQCREG4_SAVE_OFFSET;
	cs = start;

	/* Nothing preempts the batch. */
	*cs++ = I915_CMD_MI_ARB_ON_OFF | I915_CMD_MI_ARB_DISABLE;

	/* Saves L3SQCREG4. */
	*cs++ = I915_CMD_MI_STORE_REGISTER_MEM_GEN8 | I915_CMD_MI_SRM_LRM_GLOBAL_GTT;
	*cs++ = I915_GT_GEN8_L3SQCREG4;
	*cs++ = save;
	*cs++ = 0U;

	/* Asks L3 to flush its coherent lines, and waits through a data-cache flush. */
	*cs++ = I915_CMD_MI_LOAD_REGISTER_IMM(1);
	*cs++ = I915_GT_GEN8_L3SQCREG4;
	*cs++ = 0x40400000U | I915_GT_GEN8_LQSC_FLUSH_COHERENT_LINES;
	cs = i915_lrc_emit_pipe_control(cs, I915_CMD_PIPE_CONTROL_CS_STALL | I915_CMD_PIPE_CONTROL_DC_FLUSH_ENABLE, 0U);

	/* Restores L3SQCREG4. */
	*cs++ = I915_CMD_MI_LOAD_REGISTER_MEM_GEN8 | I915_CMD_MI_SRM_LRM_GLOBAL_GTT;
	*cs++ = I915_GT_GEN8_L3SQCREG4;
	*cs++ = save;
	*cs++ = 0U;

	/* Clears the shared local memory with an L3 flush. */
	cs = i915_lrc_emit_pipe_control(
		cs,
		I915_CMD_PIPE_CONTROL_FLUSH_L3 | I915_CMD_PIPE_CONTROL_STORE_DATA_INDEX | I915_CMD_PIPE_CONTROL_CS_STALL | I915_CMD_PIPE_CONTROL_QW_WRITE,
		I915_GT_LRC_PPHWSP_SCRATCH_ADDR);

	/* Sets the gather and provoking-vertex chicken bits (BSpec 11391, 11299). */
	*cs++ = I915_CMD_MI_LOAD_REGISTER_IMM(3);
	*cs++ = I915_GT_GEN9_COMMON_SLICE_CHICKEN2;
	*cs++ = I915_GT_GEN9_DISABLE_GATHER_AT_SET_SHADER_COMMON_SLICE << 16;
	*cs++ = I915_GT_GEN9_FF_SLICE_CHICKEN;
	*cs++ = I915_MASKED_ENABLE(I915_GT_GEN9_FF_SLICE_CHICKEN_CL_PROVOKING_VERTEX_FIX);
	*cs++ = I915_GT_GEN9_3D_CHICKEN3;
	*cs++ = I915_MASKED_ENABLE(I915_GT_GEN9_3D_CHICKEN_SF_PROVOKING_VERTEX_FIX);
	*cs++ = I915_CMD_MI_NOOP;

	/* Lets the engine be preempted again. */
	*cs++ = I915_CMD_MI_ARB_ON_OFF | I915_CMD_MI_ARB_ENABLE;

	/* Pads the executed part and names it. */
	cs = i915_lrc_pad(start, cs);
	i915_lrc_name_indirect(ce, &i915_lrc_gen9_layout, start, cs);
}

/* Writes no workaround batch: a Gen11 context carries none. */
static void
i915_lrc_batches_gen11(
	struct i915_gt_context *ce)
{
	UNUSED_PARAMETER(ce);
}

/*
 * Builds the Gen12 render power clock state: a request for every enabled
 * slice where slices can be power gated, and nothing otherwise.  Gen12
 * reports only slice power gating, so no subslice or EU field is set.
 */
static uint32_t
i915_lrc_power_state_gen12(
	const struct i915_gt_engine *ge)
{
	uint32_t slices;

	/* Without slice power gating there is nothing to request. */
	if (ge->sseu_has_slice_pg == 0)
		return 0U;

	/* Requests every enabled slice. */
	slices = (uint32_t)i915_lrc_count_bits(ge->sseu_slice_mask) << I915_GT_GEN11_RPCS_S_CNT_SHIFT;
	slices &= I915_GT_GEN11_RPCS_S_CNT_MASK;

	/* Reports the request with the slice count enabled. */
	return I915_GT_GEN8_RPCS_ENABLE | I915_GT_GEN8_RPCS_S_CNT_ENABLE | slices;
}

/*
 * Builds the Gen11 render power clock state.
 *
 * The slice count field of Gen11 is wider than Gen9's, but the subslice
 * count is three bits and at most four: a request for more subslices than
 * that, or than half of the present ones, is made as two whole slices
 * instead, which the hardware takes as every subslice of the one slice.
 * Every EU of a subslice is asked for, as the minimum and the maximum.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_sseu.c
 * (intel_sseu_make_rpcs).
 */
static uint32_t
i915_lrc_power_state_gen11(
	const struct i915_gt_engine *ge)
{
	uint32_t slices;
	uint32_t subslices;
	uint32_t half;
	uint32_t eus;
	uint32_t rpcs;
	int subslice_request;

	/* Starts with no explicit request, for the whole device. */
	rpcs = 0U;
	slices = (uint32_t)i915_lrc_count_bits(ge->sseu_slice_mask);
	subslices = (uint32_t)i915_lrc_count_bits((uint8_t)ge->sseu_subslice_mask);
	subslice_request = 1;

	/* More subslices than the count field holds are asked for as two whole slices. */
	half = subslices / 2U;
	if (half > 4U)
		half = 4U;
	if (slices == 1U && subslices > half) {
		subslice_request = 0;
		slices *= 2U;
	}

	/* Requests the slices. */
	if (ge->sseu_has_slice_pg != 0) {
		rpcs |= I915_GT_GEN8_RPCS_ENABLE | I915_GT_GEN8_RPCS_S_CNT_ENABLE;
		rpcs |= (slices << I915_GT_GEN11_RPCS_S_CNT_SHIFT) & I915_GT_GEN11_RPCS_S_CNT_MASK;
	}

	/* Requests the subslices, when they were not asked for as slices. */
	if (subslice_request != 0) {
		rpcs |= I915_GT_GEN8_RPCS_ENABLE | I915_GT_GEN8_RPCS_SS_CNT_ENABLE;
		rpcs |= (subslices << I915_GT_GEN8_RPCS_SS_CNT_SHIFT) & I915_GT_GEN8_RPCS_SS_CNT_MASK;
	}

	/* Requests every EU of each subslice, as the minimum and the maximum. */
	eus = (uint32_t)ge->sseu_max_eus_per_subslice;
	rpcs |= (eus << I915_GT_GEN8_RPCS_EU_MIN_SHIFT) & I915_GT_GEN8_RPCS_EU_MIN_MASK;
	rpcs |= (eus << I915_GT_GEN8_RPCS_EU_MAX_SHIFT) & I915_GT_GEN8_RPCS_EU_MAX_MASK;
	rpcs |= I915_GT_GEN8_RPCS_ENABLE;

	/* Reports the power clock state word. */
	return rpcs;
}

/*
 * Builds the Gen9 render power clock state.
 *
 * Gen9 asks for every slice when it can power-gate slices, and for every
 * EU of a subslice when it can power-gate EUs (more than one pair per
 * subslice); it never power-gates single subslices.
 */
static uint32_t
i915_lrc_power_state_gen9(
	const struct i915_gt_engine *ge)
{
	uint32_t slices;
	uint32_t eus;
	uint32_t rpcs;

	/* Starts with no explicit request. */
	rpcs = 0U;

	/* Requests every enabled slice. */
	if (ge->sseu_has_slice_pg != 0) {
		slices = (uint32_t)i915_lrc_count_bits(ge->sseu_slice_mask) << I915_GT_GEN8_RPCS_S_CNT_SHIFT;
		slices &= I915_GT_GEN8_RPCS_S_CNT_MASK;
		rpcs |= I915_GT_GEN8_RPCS_ENABLE | I915_GT_GEN8_RPCS_S_CNT_ENABLE | slices;
	}

	/* Requests every EU of each subslice, as the minimum and the maximum. */
	if (ge->sseu_eu_per_subslice > 2U) {
		eus = (uint32_t)ge->sseu_max_eus_per_subslice;
		rpcs |= (eus << I915_GT_GEN8_RPCS_EU_MIN_SHIFT) & I915_GT_GEN8_RPCS_EU_MIN_MASK;
		rpcs |= (eus << I915_GT_GEN8_RPCS_EU_MAX_SHIFT) & I915_GT_GEN8_RPCS_EU_MAX_MASK;
		rpcs |= I915_GT_GEN8_RPCS_ENABLE;
	}

	/* Reports the power clock state word. */
	return rpcs;
}

/* Builds the descriptor's low dword: the image address and the context flags. */
static uint32_t
i915_lrc_descriptor(
	const struct i915_gt_context *ce)
{
	uint32_t flags;

	/* A 4-level address space, valid and privileged. */
	flags = I915_GT_INTEL_LEGACY_64B_CONTEXT << I915_GT_GEN8_CTX_ADDRESSING_MODE_SHIFT;
	flags |= I915_GT_GEN8_CTX_VALID | I915_GT_GEN8_CTX_PRIVILEGE;

	/* Reports the image address with the flags. */
	return (uint32_t)ce->state->ggtt_offset | flags;
}
