/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Requests (see request.h).
 *
 * A request is written into a context's ring through the ring reservation
 * and commit.  What an engine writes for a flush and for the final
 * breadcrumb depends on its generation and on whether it is the render
 * engine; each of the four combinations is one emitter record, and the
 * request steps work only through the record.
 */

#include "request.h"
#include "context.h"
#include "device-info.h"
#include "engine.h"
#include "memory.h"
#include "mmio.h"
#include "workarounds.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>

#include "intel/commands.h"
#include "intel/gt-regs.h"

/*
 * The commands one kind of engine writes.
 *
 * flush writes the flush and invalidate of the mode into the ring itself.
 * seqno writes what comes before the breadcrumb's tail into reserved room,
 * and breadcrumb_dwords is the whole breadcrumb with its tail.
 */
struct i915_request_emitter {
	int (*flush)(struct i915_gt_request *rq, uint32_t mode);
	uint32_t *(*seqno)(struct i915_gt_request *rq, uint32_t *cs);
	uint32_t *(*tail)(struct i915_gt_request *rq, uint32_t *cs);
	unsigned breadcrumb_dwords;
};

static int i915_request_flush_gen12_render(struct i915_gt_request *rq, uint32_t mode);
static int i915_request_flush_gen12_other(struct i915_gt_request *rq, uint32_t mode);
static int i915_request_flush_gen11_render(struct i915_gt_request *rq, uint32_t mode);
static int i915_request_flush_gen9_render(struct i915_gt_request *rq, uint32_t mode);
static int i915_request_flush_gen9_other(struct i915_gt_request *rq, uint32_t mode);
static uint32_t *i915_request_seqno_pipe_control(struct i915_gt_request *rq, uint32_t *cs);
static uint32_t *i915_request_seqno_gen12_render(struct i915_gt_request *rq, uint32_t *cs);
static uint32_t *i915_request_seqno_gen12_other(struct i915_gt_request *rq, uint32_t *cs);
static uint32_t *i915_request_seqno_gen11_render(struct i915_gt_request *rq, uint32_t *cs);
static uint32_t *i915_request_seqno_gen9_render(struct i915_gt_request *rq, uint32_t *cs);
static uint32_t *i915_request_seqno_store_dword(struct i915_gt_request *rq, uint32_t *cs);
static uint32_t *i915_request_tail_gen12(struct i915_gt_request *rq, uint32_t *cs);
static uint32_t *i915_request_tail_gen9(struct i915_gt_request *rq, uint32_t *cs);

/*
 * The five emitters.  The breadcrumbs are 22 dwords on the render engines
 * (a flushing PIPE_CONTROL 6, the seqno write 6, the tail 10), 18 on the
 * other Gen12 engines (a stalling MI_FLUSH_DW 4, the seqno write 4, the
 * tail 10) and 14 on the other Gen9 and Gen11 engines (the seqno write 4,
 * the tail 10).  The Gen11 engines other than render take the Gen9
 * emitter.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/gt/intel_execlists_submission.c
 * (rcs_submission_override, logical_ring_default_vfuncs: the
 * GRAPHICS_VER 11 emitters).
 */
static const struct i915_request_emitter i915_request_gen12_render = {
	i915_request_flush_gen12_render, i915_request_seqno_gen12_render, i915_request_tail_gen12, 22U
};
static const struct i915_request_emitter i915_request_gen12_other = {
	i915_request_flush_gen12_other, i915_request_seqno_gen12_other, i915_request_tail_gen12, 18U
};
static const struct i915_request_emitter i915_request_gen11_render = {
	i915_request_flush_gen11_render, i915_request_seqno_gen11_render, i915_request_tail_gen9, 22U
};
static const struct i915_request_emitter i915_request_gen9_render = {
	i915_request_flush_gen9_render, i915_request_seqno_gen9_render, i915_request_tail_gen9, 22U
};
static const struct i915_request_emitter i915_request_gen9_other = {
	i915_request_flush_gen9_other, i915_request_seqno_store_dword, i915_request_tail_gen9, 14U
};

static const struct i915_request_emitter *i915_request_emitter_of(const struct i915_gt_request *rq);
static uint32_t i915_request_preparser(int off);
static uint32_t *i915_request_pipe_control(uint32_t *cs, uint32_t group0, uint32_t group1, uint32_t offset);
static uint32_t *i915_request_semaphore_preempt(struct i915_gt_request *rq, uint32_t *cs, uint32_t header);
static uint32_t *i915_request_end(struct i915_gt_request *rq, uint32_t *cs);
static int i915_request_flush(struct i915_gt_request *rq, uint32_t mode);

/*
 * Reserves room for commands in a request's ring.
 *
 * Returns where the commands go, or NULL after recording the refusal in
 * rq->error: EINVAL for an odd dword count (every command group here is
 * even, which keeps the ring tail qword aligned), ENOSPC when the ring has
 * no room or the request would wrap.  A request that already failed is
 * refused.
 */
uint32_t *
drv_i915_ring_reserve(
	struct i915_gt_request *rq,
	unsigned num_dwords)
{
	struct i915_gt_ring *ring;
	uint32_t bytes;
	uint32_t space;

	/* Refuses a request without a context, or one that already failed. */
	if (rq == NULL ||
	    rq->ce == NULL ||
	    rq->error != 0)
		return NULL;

	/* Measures the request against the context's ring. */
	ring = &rq->ce->ring;
	bytes = (uint32_t)num_dwords * 4U;

	/* An odd count is a mistake in the caller, not something to pad over. */
	if ((num_dwords & 1U) != 0U) {
		rq->error = EINVAL;
		return NULL;
	}

	/* Keeps a cacheline between the write position and the head. */
	space = (ring->head - ring->emit - I915_CACHELINE_BYTES) & (ring->size - 1U);
	if (bytes > space) {
		rq->error = ENOSPC;
		return NULL;
	}

	/* Refuses a request that would wrap the ring. */
	if (ring->emit + bytes > ring->size) {
		rq->error = ENOSPC;
		return NULL;
	}

	/* Succeeded: the commands go at the write position. */
	return (uint32_t *)((char *)ring->vaddr + ring->emit);
}

/* Moves a request's ring write position past the commands written. */
void
drv_i915_ring_commit(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	struct i915_gt_ring *ring;

	/* The write position follows the last command and the free space shrinks with it. */
	ring = &rq->ce->ring;
	ring->emit = (uint32_t)((char *)cs - (char *)ring->vaddr);
	ring->space = (ring->head - ring->emit - I915_CACHELINE_BYTES) & (ring->size - 1U);
}

/*
 * Writes the AUX table invalidate of an engine and the wait for it.
 *
 * The invalidate is requested with an LRI and then a semaphore polls the
 * register until it reads back zero.  An engine without an AUX invalidate
 * register gets nothing.  The context's indirect batch uses the same
 * emitter, so the two cannot diverge.  Returns where the next command goes.
 */
uint32_t *
drv_i915_emit_aux_inv(
	int engine_id,
	uint32_t *cs)
{
	uint32_t reg;

	/* An engine without the register has nothing to invalidate. */
	reg = drv_i915_lrc_aux_inv_reg(engine_id);
	if (reg == 0U)
		return cs;

	/* Requests the invalidate; the GSI offset is 0 on the primary GT. */
	*cs++ = I915_CMD_MI_LOAD_REGISTER_IMM(1) | I915_CMD_MI_LRI_MMIO_REMAP_EN;
	*cs++ = reg;
	*cs++ = I915_GT_AUX_INV;

	/* Polls the register until it reads back zero. */
	*cs++ = I915_CMD_MI_SEMAPHORE_WAIT_TOKEN | I915_CMD_MI_SEMAPHORE_REGISTER_POLL | I915_CMD_MI_SEMAPHORE_POLL | I915_CMD_MI_SEMAPHORE_SAD_EQ_SDD;
	*cs++ = 0U;
	*cs++ = reg;
	*cs++ = 0U;
	*cs++ = 0U;

	/* Reports where the next command goes. */
	return cs;
}

/*
 * Writes the context workarounds into a request.
 *
 * A barrier flush, one LRI holding every entry, a NOOP, and another barrier
 * flush; nothing at all for an empty list.  A masked entry or one that sets
 * every bit is written as is; any other entry is read from the register,
 * which needs mmio with forcewake held.  Returns 0, EINVAL, or the ring
 * refusal.
 */
int
drv_i915_emit_context_wa(
	struct i915_gt_request *rq,
	const struct i915_wa_list *wal,
	struct i915_mmio *mmio)
{
	const struct i915_wa *wa;
	uint32_t *cs;
	uint32_t value;
	unsigned index;
	int error;

	/* Refuses a request or a list that is missing. */
	if (rq == NULL || wal == NULL)
		return EINVAL;

	/* An empty list writes nothing, not even the flushes. */
	if (wal->count == 0U)
		return 0;

	/* Flushes and invalidates before the register writes. */
	error = i915_request_flush(rq, I915_CMD_EMIT_BARRIER);
	if (error != 0)
		return error;

	/* Reserves the LRI header, a register-value pair per entry, and the NOOP. */
	cs = drv_i915_ring_reserve(rq, wal->count * 2U + 2U);
	if (cs == NULL)
		return rq->error;

	/* Writes one LRI with every entry. */
	*cs++ = I915_CMD_MI_LOAD_REGISTER_IMM(wal->count);
	for (index = 0U; index < wal->count; index++) {
		wa = &wal->list[index];

		/* The value does not depend on the register for a masked or a full entry. */
		value = wa->set;
		if (wa->kind != I915_WA_MASKED && (wa->clr | wa->set) != 0xffffffffU) {
			/* Refuses an entry that needs a read without register access. */
			if (mmio == NULL) {
				rq->error = EINVAL;
				return EINVAL;
			}

			/* Merges the entry into the current value; no entry is MCR. */
			value = (drv_i915_read32(mmio, wa->reg) & ~wa->clr) | wa->set;
		}

		*cs++ = wa->reg;
		*cs++ = value;
	}

	/* Pads the LRI to an even count and moves the write position past it. */
	*cs++ = I915_CMD_MI_NOOP;
	drv_i915_ring_commit(rq, cs);

	/* Flushes and invalidates after the register writes. */
	error = i915_request_flush(rq, I915_CMD_EMIT_BARRIER);
	if (error != 0)
		return error;

	/* Succeeded: the workarounds are in the ring. */
	return 0;
}

/*
 * Opens a request on a context.
 *
 * Records where the request starts and where its breadcrumb lands, then
 * writes the invalidate flush; the address space has four levels, so no
 * page directories are reloaded.  The breadcrumb is a qword write and
 * MI_FLUSH_DW needs bit 5 of its address clear, so a slot that breaks
 * either is refused.  Returns 0, EINVAL, or the ring refusal.
 */
int
drv_i915_request_open(
	struct i915_gt_request *rq,
	struct i915_gt_context *ce,
	uint32_t seqno,
	uint32_t hwsp_ggtt,
	volatile uint32_t *hwsp_cpu)
{
	int error;

	/* Refuses a request on a context without an image. */
	if (rq == NULL ||
	    ce == NULL ||
	    ce->allocated == 0)
		return EINVAL;

	/* Starts the request at the ring's write position. */
	kern_memset(rq, 0, sizeof(*rq));
	rq->ce = ce;
	rq->seqno = seqno;
	rq->hwsp_ggtt = hwsp_ggtt;
	rq->hwsp_cpu = hwsp_cpu;
	rq->preempt_ggtt = (uint32_t)ce->ge->hwsp_ggtt + I915_GT_HWS_PREEMPT_ADDR;
	rq->head = ce->ring.emit;

	/* Refuses a breadcrumb slot that is not qword aligned or has bit 5 set. */
	if ((hwsp_ggtt & 7U) != 0U || (hwsp_ggtt & (1U << 5)) != 0U)
		return EINVAL;

	/* Invalidates the caches before the request's commands. */
	error = i915_request_flush(rq, I915_CMD_EMIT_INVALIDATE);
	if (error != 0)
		return error;

	/* Succeeded: the request is open for commands. */
	return 0;
}

/*
 * Closes a request with its final breadcrumb.
 *
 * Exactly the breadcrumb's size is reserved and the breadcrumb fills it:
 * the flush and seqno write of the engine, then the tail.  Sets rq->tail and
 * rq->wa_tail.  Returns 0, EINVAL, or the request's earlier failure.
 */
int
drv_i915_request_close(
	struct i915_gt_request *rq)
{
	const struct i915_request_emitter *emitter;
	uint32_t *cs;

	/* Refuses a missing request, one that already failed, or one without a context. */
	if (rq == NULL)
		return EINVAL;
	if (rq->error != 0)
		return rq->error;
	if (rq->ce == NULL)
		return EINVAL;

	/* Reserves the breadcrumb of the engine. */
	emitter = i915_request_emitter_of(rq);
	cs = drv_i915_ring_reserve(rq, emitter->breadcrumb_dwords);
	if (cs == NULL)
		return rq->error;

	/* Writes the seqno, then the tail, and moves the write position past them. */
	cs = emitter->seqno(rq, cs);
	cs = emitter->tail(rq, cs);
	drv_i915_ring_commit(rq, cs);

	/* The request is complete and may be submitted. */
	rq->added = 1;

	/* Succeeded: the request is closed. */
	return 0;
}

/*
 * Reports nonzero once a request's breadcrumb has landed.
 *
 * The landed seqno is compared by signed difference, so the seqno may wrap.
 * A request without a breadcrumb slot never completes.
 */
int
drv_i915_request_completed(
	const struct i915_gt_request *rq)
{
	/* A request without a slot has nothing to land. */
	if (rq == NULL || rq->hwsp_cpu == NULL)
		return 0;

	/* The breadcrumb has landed once the slot reaches the request's seqno. */
	if ((int32_t)(*rq->hwsp_cpu - rq->seqno) >= 0)
		return 1;

	/* The breadcrumb has not landed yet. */
	return 0;
}

/* Returns the emitter of a request's engine. */
static const struct i915_request_emitter *
i915_request_emitter_of(
	const struct i915_gt_request *rq)
{
	const struct i915_engine_info *info;

	/* The generation and whether the engine is the render engine pick the emitter. */
	info = rq->ce->ge->info;
	if (info->graphics_ver == 9) {
		if (info->class == I915_RENDER_CLASS)
			return &i915_request_gen9_render;
		return &i915_request_gen9_other;
	}

	/* An Ice Lake engine takes the Gen11 render emitter or the Gen9 one. */
	if (info->graphics_ver == 11) {
		if (info->class == I915_RENDER_CLASS)
			return &i915_request_gen11_render;
		return &i915_request_gen9_other;
	}

	/* Every other engine takes a Gen12 emitter. */
	if (info->class == I915_RENDER_CLASS)
		return &i915_request_gen12_render;
	return &i915_request_gen12_other;
}

/* Writes the flush of the request's engine. */
static int
i915_request_flush(
	struct i915_gt_request *rq,
	uint32_t mode)
{
	/* Refuses a request without a context. */
	if (rq == NULL || rq->ce == NULL)
		return EINVAL;

	/* Succeeded or refused as the engine's flush reports. */
	return i915_request_emitter_of(rq)->flush(rq, mode);
}

/* Builds the MI_ARB_CHECK that turns the pre-parser off (nonzero) or on (zero). */
static uint32_t
i915_request_preparser(
	int off)
{
	/* Bit 8 selects the pre-parser control; bit 0 disables it. */
	if (off != 0)
		return I915_CMD_MI_ARB_CHECK | (1U << 8) | 1U;
	return I915_CMD_MI_ARB_CHECK | (1U << 8);
}

/* Writes a six-dword PIPE_CONTROL whose last three dwords are zero. */
static uint32_t *
i915_request_pipe_control(
	uint32_t *cs,
	uint32_t group0,
	uint32_t group1,
	uint32_t offset)
{
	/* The header with the first flag group, the second group and the post-sync address. */
	cs[0] = I915_CMD_GFX_OP_PIPE_CONTROL(6) | group0;
	cs[1] = group1;
	cs[2] = offset;
	cs[3] = 0U;
	cs[4] = 0U;
	cs[5] = 0U;

	/* Reports where the next command goes. */
	return cs + 6;
}

/*
 * Writes the Gen12 render flush.
 *
 * Every engine needs the AUX table invalidate, so the flush block runs even
 * for a pure invalidate; L3 is flushed only when a flush was asked for, and
 * Wa_1409600907 adds the depth stall.  An invalidate follows with the
 * caches and the TLB, behind the pre-parser, and the AUX invalidate.
 */
static int
i915_request_flush_gen12_render(
	struct i915_gt_request *rq,
	uint32_t mode)
{
	uint32_t *cs;
	uint32_t flags;

	/* The flush block. */
	flags = I915_CMD_PIPE_CONTROL_TILE_CACHE_FLUSH |
		I915_CMD_PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
		I915_CMD_PIPE_CONTROL_DEPTH_CACHE_FLUSH |
		I915_CMD_PIPE_CONTROL_DEPTH_STALL |
		I915_CMD_PIPE_CONTROL_DC_FLUSH_ENABLE |
		I915_CMD_PIPE_CONTROL_FLUSH_ENABLE |
		I915_CMD_PIPE_CONTROL_STORE_DATA_INDEX |
		I915_CMD_PIPE_CONTROL_QW_WRITE |
		I915_CMD_PIPE_CONTROL_CS_STALL;
	if ((mode & I915_CMD_EMIT_FLUSH) != 0U)
		flags |= I915_CMD_PIPE_CONTROL_FLUSH_L3;
	cs = drv_i915_ring_reserve(rq, 6U);
	if (cs == NULL)
		return rq->error;
	cs = i915_request_pipe_control(cs, I915_CMD_PIPE_CONTROL0_HDC_PIPELINE_FLUSH, flags, I915_GT_LRC_PPHWSP_SCRATCH_ADDR);
	drv_i915_ring_commit(rq, cs);

	/* Nothing more for a pure flush. */
	if ((mode & I915_CMD_EMIT_INVALIDATE) == 0U)
		return 0;

	/* The invalidate: the pre-parser off, the PIPE_CONTROL, the AUX invalidate, the pre-parser on. */
	flags = I915_CMD_PIPE_CONTROL_COMMAND_CACHE_INVALIDATE |
		I915_CMD_PIPE_CONTROL_TLB_INVALIDATE |
		I915_CMD_PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE |
		I915_CMD_PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
		I915_CMD_PIPE_CONTROL_VF_CACHE_INVALIDATE |
		I915_CMD_PIPE_CONTROL_CONST_CACHE_INVALIDATE |
		I915_CMD_PIPE_CONTROL_STATE_CACHE_INVALIDATE |
		I915_CMD_PIPE_CONTROL_STORE_DATA_INDEX |
		I915_CMD_PIPE_CONTROL_QW_WRITE |
		I915_CMD_PIPE_CONTROL_CS_STALL;
	cs = drv_i915_ring_reserve(rq, 16U);
	if (cs == NULL)
		return rq->error;

	/* Keeps the pre-parser from fetching a stale page for the request past the TLB invalidate. */
	*cs++ = i915_request_preparser(1);
	cs = i915_request_pipe_control(cs, 0U, flags, I915_GT_LRC_PPHWSP_SCRATCH_ADDR);
	cs = drv_i915_emit_aux_inv(rq->ce->ge->info->id, cs);
	*cs++ = i915_request_preparser(0);
	drv_i915_ring_commit(rq, cs);

	/* Succeeded: the flush is in the ring. */
	return 0;
}

/*
 * Writes the Gen12 flush of a copy, video or enhancement engine.
 *
 * One MI_FLUSH_DW, always a command barrier with a post-sync write to the
 * scratch slot; an invalidate adds the TLB, BSD on a video engine and CCS
 * on a copy engine, behind the pre-parser, and the AUX invalidate.  A
 * flush-only call on an engine that needs the AUX invalidate is refused,
 * since only an invalidate has room for it.
 */
static int
i915_request_flush_gen12_other(
	struct i915_gt_request *rq,
	uint32_t mode)
{
	uint32_t *cs;
	uint32_t command;
	int engine_class;
	int engine_id;
	int invalidate;

	/* The engine and the kind of flush. */
	engine_class = rq->ce->ge->info->class;
	engine_id = rq->ce->ge->info->id;
	invalidate = (mode & I915_CMD_EMIT_INVALIDATE) != 0U;

	/* Reserves the flush, with the pre-parser pair and the AUX invalidate for an invalidate. */
	cs = drv_i915_ring_reserve(rq, invalidate != 0 ? 14U : 4U);
	if (cs == NULL)
		return rq->error;
	if (invalidate != 0)
		*cs++ = i915_request_preparser(1);

	/* The command barrier, with the invalidate bits of the engine. */
	command = (I915_CMD_MI_FLUSH_DW + 1U) | I915_CMD_MI_FLUSH_DW_STORE_INDEX | I915_CMD_MI_FLUSH_DW_OP_STOREDW;
	if (invalidate != 0) {
		command |= I915_CMD_MI_INVALIDATE_TLB;
		if (engine_class == I915_VIDEO_DECODE_CLASS)
			command |= I915_CMD_MI_INVALIDATE_BSD;
		if (engine_class == I915_COPY_ENGINE_CLASS)
			command |= I915_CMD_MI_FLUSH_DW_CCS;
	}
	*cs++ = command;
	*cs++ = I915_GT_LRC_PPHWSP_SCRATCH_ADDR;
	*cs++ = 0U;
	*cs++ = 0U;

	/* The AUX invalidate goes with an invalidate only. */
	if (invalidate != 0) {
		cs = drv_i915_emit_aux_inv(engine_id, cs);
		*cs++ = i915_request_preparser(0);
	} else if (drv_i915_lrc_aux_inv_reg(engine_id) != 0U) {
		rq->error = EINVAL;
		return EINVAL;
	}

	/* Moves the write position past the flush. */
	drv_i915_ring_commit(rq, cs);

	/* Succeeded: the flush is in the ring. */
	return 0;
}

/*
 * Writes the Gen11 render flush.
 *
 * The flush and the invalidate are two PIPE_CONTROLs, each written only
 * when asked for: the flush writes back the tile, render target, depth and
 * data caches; the invalidate drops the command, instruction, texture, VF,
 * constant and state caches and the TLB.  Both stall the command streamer
 * and post a write to the scratch slot.  There is no pre-parser control or
 * AUX table on Gen11.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/gt/gen8_engine_cs.c (gen11_emit_flush_rcs).
 */
static int
i915_request_flush_gen11_render(
	struct i915_gt_request *rq,
	uint32_t mode)
{
	uint32_t *cs;
	uint32_t flags;

	/* Writes back the caches. */
	if ((mode & I915_CMD_EMIT_FLUSH) != 0U) {
		flags = I915_CMD_PIPE_CONTROL_CS_STALL |
			I915_CMD_PIPE_CONTROL_TILE_CACHE_FLUSH |
			I915_CMD_PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
			I915_CMD_PIPE_CONTROL_DEPTH_CACHE_FLUSH |
			I915_CMD_PIPE_CONTROL_DC_FLUSH_ENABLE |
			I915_CMD_PIPE_CONTROL_FLUSH_ENABLE |
			I915_CMD_PIPE_CONTROL_QW_WRITE |
			I915_CMD_PIPE_CONTROL_STORE_DATA_INDEX;
		cs = drv_i915_ring_reserve(rq, 6U);
		if (cs == NULL)
			return rq->error;

		/* Writes it with its post-sync write to the scratch slot. */
		cs = i915_request_pipe_control(cs, 0U, flags, I915_GT_LRC_PPHWSP_SCRATCH_ADDR);
		drv_i915_ring_commit(rq, cs);
	}

	/* Drops the read caches and the TLB. */
	if ((mode & I915_CMD_EMIT_INVALIDATE) != 0U) {
		flags = I915_CMD_PIPE_CONTROL_CS_STALL |
			I915_CMD_PIPE_CONTROL_COMMAND_CACHE_INVALIDATE |
			I915_CMD_PIPE_CONTROL_TLB_INVALIDATE |
			I915_CMD_PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE |
			I915_CMD_PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
			I915_CMD_PIPE_CONTROL_VF_CACHE_INVALIDATE |
			I915_CMD_PIPE_CONTROL_CONST_CACHE_INVALIDATE |
			I915_CMD_PIPE_CONTROL_STATE_CACHE_INVALIDATE |
			I915_CMD_PIPE_CONTROL_QW_WRITE |
			I915_CMD_PIPE_CONTROL_STORE_DATA_INDEX;
		cs = drv_i915_ring_reserve(rq, 6U);
		if (cs == NULL)
			return rq->error;

		/* Writes it with its post-sync write to the scratch slot. */
		cs = i915_request_pipe_control(cs, 0U, flags, I915_GT_LRC_PPHWSP_SCRATCH_ADDR);
		drv_i915_ring_commit(rq, cs);
	}

	/* Succeeded: the flush is in the ring. */
	return 0;
}

/*
 * Writes the Gen9 render flush.
 *
 * One PIPE_CONTROL carries the flush and the invalidate together; Gen9
 * needs a null PIPE_CONTROL before a VF cache invalidate.  There is no
 * pre-parser control, AUX table, tile cache or HDC pipeline flush on Gen9.
 * WaForGAMHang (Kaby Lake before stepping C0) is not done.
 */
static int
i915_request_flush_gen9_render(
	struct i915_gt_request *rq,
	uint32_t mode)
{
	uint32_t *cs;
	uint32_t flags;
	int invalidate;

	/* Every flush stalls the command streamer; a flush writes back the render target, depth and data caches. */
	flags = I915_CMD_PIPE_CONTROL_CS_STALL;
	if ((mode & I915_CMD_EMIT_FLUSH) != 0U) {
		flags |= I915_CMD_PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
			 I915_CMD_PIPE_CONTROL_DEPTH_CACHE_FLUSH |
			 I915_CMD_PIPE_CONTROL_DC_FLUSH_ENABLE |
			 I915_CMD_PIPE_CONTROL_FLUSH_ENABLE;
	}

	/* An invalidate drops the TLB and the read caches, with a post-sync write to the scratch slot. */
	invalidate = (mode & I915_CMD_EMIT_INVALIDATE) != 0U;
	if (invalidate != 0) {
		flags |= I915_CMD_PIPE_CONTROL_TLB_INVALIDATE |
			 I915_CMD_PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE |
			 I915_CMD_PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
			 I915_CMD_PIPE_CONTROL_VF_CACHE_INVALIDATE |
			 I915_CMD_PIPE_CONTROL_CONST_CACHE_INVALIDATE |
			 I915_CMD_PIPE_CONTROL_STATE_CACHE_INVALIDATE |
			 I915_CMD_PIPE_CONTROL_QW_WRITE |
			 I915_CMD_PIPE_CONTROL_STORE_DATA_INDEX;
	}

	/* Reserves the flush, and the null PIPE_CONTROL for an invalidate. */
	cs = drv_i915_ring_reserve(rq, invalidate != 0 ? 12U : 6U);
	if (cs == NULL)
		return rq->error;
	if (invalidate != 0)
		cs = i915_request_pipe_control(cs, 0U, 0U, 0U);

	/* Writes the flush and the invalidate. */
	cs = i915_request_pipe_control(cs, 0U, flags, I915_GT_LRC_PPHWSP_SCRATCH_ADDR);
	drv_i915_ring_commit(rq, cs);

	/* Succeeded: the flush is in the ring. */
	return 0;
}

/*
 * Writes the Gen9 flush of a copy, video or enhancement engine: one
 * MI_FLUSH_DW with a post-sync write, always a command barrier, and the TLB
 * (and video BSD) invalidate on request.
 */
static int
i915_request_flush_gen9_other(
	struct i915_gt_request *rq,
	uint32_t mode)
{
	uint32_t *cs;
	uint32_t command;

	/* Reserves the flush. */
	cs = drv_i915_ring_reserve(rq, 4U);
	if (cs == NULL)
		return rq->error;

	/* The command barrier, with the invalidate bits of the engine. */
	command = (I915_CMD_MI_FLUSH_DW + 1U) | I915_CMD_MI_FLUSH_DW_STORE_INDEX | I915_CMD_MI_FLUSH_DW_OP_STOREDW;
	if ((mode & I915_CMD_EMIT_INVALIDATE) != 0U) {
		command |= I915_CMD_MI_INVALIDATE_TLB;
		if (rq->ce->ge->info->class == I915_VIDEO_DECODE_CLASS)
			command |= I915_CMD_MI_INVALIDATE_BSD;
	}

	/* Writes the flush with its post-sync write to the scratch slot. */
	*cs++ = command;
	*cs++ = I915_GT_LRC_PPHWSP_SCRATCH_ADDR;
	*cs++ = 0U;
	*cs++ = 0U;
	drv_i915_ring_commit(rq, cs);

	/* Succeeded: the flush is in the ring. */
	return 0;
}

/* Writes the seqno with a stalling PIPE_CONTROL qword write to the slot. */
static uint32_t *
i915_request_seqno_pipe_control(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	/* The qword write carries the seqno in its low dword. */
	*cs++ = I915_CMD_GFX_OP_PIPE_CONTROL(6);
	*cs++ = I915_CMD_PIPE_CONTROL_FLUSH_ENABLE |
		I915_CMD_PIPE_CONTROL_CS_STALL |
		I915_CMD_PIPE_CONTROL_GLOBAL_GTT_IVB |
		I915_CMD_PIPE_CONTROL_QW_WRITE;
	*cs++ = rq->hwsp_ggtt;
	*cs++ = 0U;
	*cs++ = rq->seqno;
	*cs++ = 0U;

	/* Reports where the next command goes. */
	return cs;
}

/*
 * Writes the Gen12 render breadcrumb's seqno: a full flush first (L3
 * flushed, Wa_1409600907's depth stall), then the seqno write.
 */
static uint32_t *
i915_request_seqno_gen12_render(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	/* Flushes everything. */
	cs = i915_request_pipe_control(cs,
	    I915_CMD_PIPE_CONTROL0_HDC_PIPELINE_FLUSH,
	    I915_CMD_PIPE_CONTROL_CS_STALL |
	    I915_CMD_PIPE_CONTROL_TLB_INVALIDATE |
	    I915_CMD_PIPE_CONTROL_TILE_CACHE_FLUSH |
	    I915_CMD_PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
	    I915_CMD_PIPE_CONTROL_DEPTH_CACHE_FLUSH |
	    I915_CMD_PIPE_CONTROL_DC_FLUSH_ENABLE |
	    I915_CMD_PIPE_CONTROL_FLUSH_ENABLE |
	    I915_CMD_PIPE_CONTROL_FLUSH_L3 |
	    I915_CMD_PIPE_CONTROL_DEPTH_STALL,
	    0U);

	/* Writes the seqno. */
	return i915_request_seqno_pipe_control(rq, cs);
}

/* Writes a Gen12 copy, video or enhancement breadcrumb's seqno: a stalling flush, then the store. */
static uint32_t *
i915_request_seqno_gen12_other(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	/* A stalling flush before the seqno write; the post-sync write is not. */
	*cs++ = I915_CMD_MI_FLUSH_DW + 1U;
	*cs++ = 0U;
	*cs++ = 0U;
	*cs++ = 0U;

	/* Writes the seqno. */
	return i915_request_seqno_store_dword(rq, cs);
}

/*
 * Writes the Gen11 render breadcrumb's seqno: a cache flush with the tile
 * cache, then the seqno write.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/gt/gen8_engine_cs.c (gen11_emit_fini_breadcrumb_rcs).
 */
static uint32_t *
i915_request_seqno_gen11_render(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	/* Flushes the tile, render target, depth and data caches. */
	cs = i915_request_pipe_control(cs,
	    0U,
	    I915_CMD_PIPE_CONTROL_CS_STALL |
	    I915_CMD_PIPE_CONTROL_TLB_INVALIDATE |
	    I915_CMD_PIPE_CONTROL_TILE_CACHE_FLUSH |
	    I915_CMD_PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
	    I915_CMD_PIPE_CONTROL_DEPTH_CACHE_FLUSH |
	    I915_CMD_PIPE_CONTROL_DC_FLUSH_ENABLE,
	    0U);

	/* Writes the seqno. */
	return i915_request_seqno_pipe_control(rq, cs);
}

/* Writes the Gen9 render breadcrumb's seqno: a cache flush, then the seqno write. */
static uint32_t *
i915_request_seqno_gen9_render(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	/* Flushes the render target, depth and data caches. */
	cs = i915_request_pipe_control(cs,
	    0U,
	    I915_CMD_PIPE_CONTROL_CS_STALL |
	    I915_CMD_PIPE_CONTROL_TLB_INVALIDATE |
	    I915_CMD_PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
	    I915_CMD_PIPE_CONTROL_DEPTH_CACHE_FLUSH |
	    I915_CMD_PIPE_CONTROL_DC_FLUSH_ENABLE,
	    0U);

	/* Writes the seqno. */
	return i915_request_seqno_pipe_control(rq, cs);
}

/* Writes the seqno with an MI_FLUSH_DW store to the slot. */
static uint32_t *
i915_request_seqno_store_dword(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	/* The post-sync store through the GGTT. */
	*cs++ = (I915_CMD_MI_FLUSH_DW + 1U) | I915_CMD_MI_FLUSH_DW_OP_STOREDW;
	*cs++ = rq->hwsp_ggtt | I915_CMD_MI_FLUSH_DW_USE_GTT;
	*cs++ = 0U;
	*cs++ = rq->seqno;

	/* Reports where the next command goes. */
	return cs;
}

/*
 * Writes the interrupt, arbitration and the busy wait on the status page's
 * PREEMPT dword, with the given semaphore header.  The wait triggers IDLE
 * to ACTIVE first through an arbitration check.
 */
static uint32_t *
i915_request_semaphore_preempt(
	struct i915_gt_request *rq,
	uint32_t *cs,
	uint32_t header)
{
	/* Raises the interrupt and allows arbitration. */
	*cs++ = I915_CMD_MI_USER_INTERRUPT;
	*cs++ = I915_CMD_MI_ARB_ON_OFF | I915_CMD_MI_ARB_ENABLE;

	/* Waits for PREEMPT to be zero. */
	*cs++ = I915_CMD_MI_ARB_CHECK;
	*cs++ = header | I915_CMD_MI_SEMAPHORE_GLOBAL_GTT | I915_CMD_MI_SEMAPHORE_POLL | I915_CMD_MI_SEMAPHORE_SAD_EQ_SDD;
	*cs++ = 0U;
	*cs++ = rq->preempt_ggtt;
	*cs++ = 0U;

	/* Reports where the next command goes. */
	return cs;
}

/*
 * Ends a request: records rq->tail, then leaves one more preemption point
 * and records rq->wa_tail, which the ring tail is programmed to.
 */
static uint32_t *
i915_request_end(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	struct i915_gt_ring *ring;

	/* The offsets are measured from the start of the ring. */
	ring = &rq->ce->ring;
	rq->tail = (uint32_t)((char *)cs - (char *)ring->vaddr);
	*cs++ = I915_CMD_MI_ARB_CHECK;
	*cs++ = I915_CMD_MI_NOOP;
	rq->wa_tail = (uint32_t)((char *)cs - (char *)ring->vaddr);

	/* Reports where the next command goes. */
	return cs;
}

/* Writes the Gen12 tail: the five-dword semaphore with its token form. */
static uint32_t *
i915_request_tail_gen12(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	/* The busy wait, whose fifth dword is zero. */
	cs = i915_request_semaphore_preempt(rq, cs, I915_CMD_MI_SEMAPHORE_WAIT_TOKEN);
	*cs++ = 0U;

	/* Ends the request. */
	return i915_request_end(rq, cs);
}

/* Writes the Gen9 tail: the four-dword semaphore and a no-op. */
static uint32_t *
i915_request_tail_gen9(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	/* The busy wait in its four-dword form. */
	cs = i915_request_semaphore_preempt(rq, cs, I915_CMD_MI_SEMAPHORE_WAIT);
	*cs++ = I915_CMD_MI_NOOP;

	/* Ends the request. */
	return i915_request_end(rq, cs);
}
