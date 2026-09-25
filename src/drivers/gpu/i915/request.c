/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Requests (see request.h).
 *
 * The first part of this file writes requests into a context's ring: the
 * ring reservation, the flushes, the context workarounds and the final
 * breadcrumb.  The software request queue of the device follows it as a
 * separate part.
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

/* How many dwords the final breadcrumb takes on the render engine and on the others. */
#define I915_FINI_BREADCRUMB_RCS_DWORDS	22U
#define I915_FINI_BREADCRUMB_XCS_DWORDS	18U

/* How many dwords the render flush's flush block and invalidate block take. */
#define I915_FLUSH_RCS_FLUSH_DWORDS	6U
#define I915_FLUSH_RCS_INVALIDATE_DWORDS	16U

/* How many dwords the other engines' flush takes, and what an invalidate adds. */
#define I915_FLUSH_XCS_DWORDS		4U
#define I915_FLUSH_XCS_INVALIDATE_DWORDS	10U

static uint32_t i915_preparser_disable(int state);
static uint32_t *i915_emit_pipe_control(uint32_t *cs, uint32_t bit_group_0, uint32_t bit_group_1, uint32_t offset);
static int i915_emit_flush(struct i915_gt_request *rq, uint32_t mode);
static int i915_emit_flush_rcs(struct i915_gt_request *rq, uint32_t mode);
static int i915_emit_flush_xcs(struct i915_gt_request *rq, uint32_t mode);
static uint32_t *i915_emit_fini_breadcrumb_tail(struct i915_gt_request *rq, uint32_t *cs);

/*
 * Reserves room for commands in a request's ring (intel_ring_begin()).
 *
 * Returns where the commands go, or NULL after recording the refusal in
 * rq->error: EINVAL for an odd dword count (every command group the reference
 * emits is even, which keeps the ring tail qword aligned), ENOSPC when the
 * ring has no room or the request would wrap.  A request that already failed
 * is refused.
 */
uint32_t *
drv_i915_ring_begin(
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

	/* An odd count is a porting mistake, not something to pad over. */
	if ((num_dwords & 1U) != 0U) {
		rq->error = EINVAL;
		return NULL;
	}

	/* Keeps a cacheline between the write position and the head (__intel_ring_space()). */
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

/*
 * Moves a request's ring write position past the commands written
 * (intel_ring_advance()).
 */
void
drv_i915_ring_advance(
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
 * Writes the AUX table invalidate of an engine and the wait for it
 * (gen12_emit_aux_table_inv()).
 *
 * The invalidate is requested with an LRI and then a semaphore polls the
 * register until it reads back zero.  An engine without an AUX invalidate
 * register gets nothing.  The context's indirect batch uses the same
 * emitter, so the two cannot diverge.  Returns where the next command goes.
 */
uint32_t *
drv_i915_gen12_emit_aux_table_inv(
	int engine_id,
	uint32_t *cs)
{
	uint32_t inv_reg;

	/* An engine without the register has nothing to invalidate. */
	inv_reg = drv_i915_lrc_aux_inv_reg(engine_id);
	if (inv_reg == 0U)
		return cs;

	/* Requests the invalidate; the GSI offset is 0 on the primary GT. */
	*cs++ = MI_LOAD_REGISTER_IMM(1) | MI_LRI_MMIO_REMAP_EN;
	*cs++ = inv_reg;
	*cs++ = AUX_INV;

	/* Polls the register until it reads back zero. */
	*cs++ = MI_SEMAPHORE_WAIT_TOKEN | MI_SEMAPHORE_REGISTER_POLL | MI_SEMAPHORE_POLL | MI_SEMAPHORE_SAD_EQ_SDD;
	*cs++ = 0U;
	*cs++ = inv_reg;
	*cs++ = 0U;
	*cs++ = 0U;

	/* Reports where the next command goes. */
	return cs;
}

/*
 * Writes the context workarounds into a request (intel_engine_emit_ctx_wa()).
 *
 * A barrier flush, one LRI holding every entry, a NOOP, and another barrier
 * flush; nothing at all for an empty list.  A masked entry or one that sets
 * every bit is written as is; any other entry is read from the register,
 * which needs mmio with forcewake held.  Returns 0, EINVAL, or the ring
 * refusal.
 */
int
drv_i915_emit_ctx_wa(
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
	error = i915_emit_flush(rq, EMIT_BARRIER);
	if (error != 0)
		return error;

	/* Reserves the LRI header, a register-value pair per entry, and the NOOP. */
	cs = drv_i915_ring_begin(rq, wal->count * 2U + 2U);
	if (cs == NULL)
		return rq->error;

	/* Writes one LRI with every entry. */
	*cs++ = MI_LOAD_REGISTER_IMM(wal->count);
	for (index = 0U; index < wal->count; index++) {
		wa = &wal->list[index];

		/* Skips reading the register when the value does not depend on it. */
		if (wa->kind == I915_WA_MASKED || (wa->clr | wa->set) == 0xffffffffU) {
			value = wa->set;
		} else {
			/* Refuses an entry that needs a read without register access. */
			if (mmio == NULL) {
				rq->error = EINVAL;
				return EINVAL;
			}

			/* Merges the entry into the current value; no entry is MCR on Alder Lake-P. */
			value = drv_i915_read32(mmio, wa->reg);
			value &= ~wa->clr;
			value |= wa->set;
		}

		*cs++ = wa->reg;
		*cs++ = value;
	}

	/* Pads the LRI to an even count and moves the write position past it. */
	*cs++ = MI_NOOP;
	drv_i915_ring_advance(rq, cs);

	/* Flushes and invalidates after the register writes. */
	error = i915_emit_flush(rq, EMIT_BARRIER);
	if (error != 0)
		return error;

	/* Succeeded: the workarounds are in the ring. */
	return 0;
}

/*
 * Starts a request on a context (i915_request_create() on an execlists
 * engine).
 *
 * Records where the request starts and where its breadcrumb lands, then
 * writes the invalidate flush of execlists_request_alloc(); the address space
 * has four levels, so no page directories are reloaded.  The breadcrumb is a
 * qword write and MI_FLUSH_DW needs bit 5 of its address clear, so a slot
 * that breaks either is refused.  Returns 0, EINVAL, or the ring refusal.
 */
int
drv_i915_request_create(
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
	rq->preempt_ggtt = (uint32_t)ce->ge->hwsp_ggtt + I915_GEM_HWS_PREEMPT_ADDR;
	rq->head = ce->ring.emit;

	/* Refuses a breadcrumb slot that is not qword aligned or has bit 5 set. */
	if ((hwsp_ggtt & 7U) != 0U || (hwsp_ggtt & (1U << 5)) != 0U)
		return EINVAL;

	/* Invalidates the caches before the request's commands. */
	error = i915_emit_flush(rq, EMIT_INVALIDATE);
	if (error != 0)
		return error;

	/* Succeeded: the request is open for commands. */
	return 0;
}

/*
 * Closes a request with its final breadcrumb (i915_request_add()).
 *
 * __i915_request_commit() reserves exactly the breadcrumb's size and the
 * breadcrumb fills it: 22 dwords on the render engine (a flushing
 * PIPE_CONTROL 6, the seqno write 6, the tail 10) and 18 on the others
 * (MI_FLUSH_DW 4, the seqno write 4, the tail 10).  Sets rq->tail and
 * rq->wa_tail.  Returns 0, EINVAL, or the request's earlier failure.
 */
int
drv_i915_request_add(
	struct i915_gt_request *rq)
{
	uint32_t *cs;
	uint32_t flags;
	unsigned dwords;

	/* Refuses a missing request, one that already failed, or one without a context. */
	if (rq == NULL)
		return EINVAL;
	if (rq->error != 0)
		return rq->error;
	if (rq->ce == NULL)
		return EINVAL;

	/* Reserves the breadcrumb, whose size depends on the engine class. */
	dwords = I915_FINI_BREADCRUMB_XCS_DWORDS;
	if (rq->ce->ge->info->class == I915_RENDER_CLASS)
		dwords = I915_FINI_BREADCRUMB_RCS_DWORDS;
	cs = drv_i915_ring_begin(rq, dwords);
	if (cs == NULL)
		return rq->error;

	/* Flushes everything, then writes the seqno to the timeline's slot. */
	if (rq->ce->ge->info->class == I915_RENDER_CLASS) {
		/*
		 * The render flush: L3 is flushed before 12.70, Wa_14016712196
		 * (12.70 to 12.74, DG2) is not taken, and Wa_1409600907 adds
		 * the depth stall.
		 */
		flags = PIPE_CONTROL_CS_STALL |
			PIPE_CONTROL_TLB_INVALIDATE |
			PIPE_CONTROL_TILE_CACHE_FLUSH |
			PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
			PIPE_CONTROL_DEPTH_CACHE_FLUSH |
			PIPE_CONTROL_DC_FLUSH_ENABLE |
			PIPE_CONTROL_FLUSH_ENABLE;
		flags |= PIPE_CONTROL_FLUSH_L3;
		flags |= PIPE_CONTROL_DEPTH_STALL;
		cs = i915_emit_pipe_control(cs, PIPE_CONTROL0_HDC_PIPELINE_FLUSH, flags, 0U);

		/* Writes the seqno (gen12_emit_ggtt_write_rcs(seqno, hwsp, 0, FLUSH_ENABLE | CS_STALL)). */
		*cs++ = GFX_OP_PIPE_CONTROL(6);
		*cs++ = PIPE_CONTROL_FLUSH_ENABLE |
			PIPE_CONTROL_CS_STALL |
			PIPE_CONTROL_GLOBAL_GTT_IVB |
			PIPE_CONTROL_QW_WRITE;
		*cs++ = rq->hwsp_ggtt;
		*cs++ = 0U;
		*cs++ = rq->seqno;

		/* The reference writes one extra dword of the qword. */
		*cs++ = 0U;
	} else {
		/* A stalling flush before the seqno write; the post-sync write is not. */
		*cs++ = MI_FLUSH_DW + 1U;
		*cs++ = 0U;
		*cs++ = 0U;
		*cs++ = 0U;

		/* Writes the seqno (gen8_emit_ggtt_write(seqno, hwsp, 0)). */
		*cs++ = (MI_FLUSH_DW + 1U) | MI_FLUSH_DW_OP_STOREDW;
		*cs++ = rq->hwsp_ggtt | MI_FLUSH_DW_USE_GTT;
		*cs++ = 0U;
		*cs++ = rq->seqno;
	}

	/* Writes the tail: the interrupt, the preemption busywait and the workaround tail. */
	cs = i915_emit_fini_breadcrumb_tail(rq, cs);
	drv_i915_ring_advance(rq, cs);

	/* The request is complete and may be submitted. */
	rq->added = 1;

	/* Succeeded: the request is closed. */
	return 0;
}

/*
 * Reports nonzero once a request's breadcrumb has landed.
 *
 * This is i915_seqno_passed(*hwsp, seqno): a signed difference, so the seqno
 * may wrap.  A request without a breadcrumb slot never completes.
 */
int
drv_i915_request_completed(
	const struct i915_gt_request *rq)
{
	int32_t distance;

	/* A request without a slot has nothing to land. */
	if (rq == NULL || rq->hwsp_cpu == NULL)
		return 0;

	/* Compares the landed seqno with the request's. */
	distance = (int32_t)(*rq->hwsp_cpu - rq->seqno);
	if (distance >= 0)
		return 1;

	/* The breadcrumb has not landed yet. */
	return 0;
}

/* Builds the MI_ARB_CHECK that turns the pre-parser off (1) or on (0). */
static uint32_t
i915_preparser_disable(
	int state)
{
	uint32_t command;

	/* Bit 8 selects the pre-parser control; bit 0 disables it. */
	command = MI_ARB_CHECK | (1U << 8);
	if (state != 0)
		command |= 1U;

	/* Reports the command. */
	return command;
}

/* Writes a six-dword PIPE_CONTROL whose last three dwords are zero (__gen8_emit_pipe_control()). */
static uint32_t *
i915_emit_pipe_control(
	uint32_t *cs,
	uint32_t bit_group_0,
	uint32_t bit_group_1,
	uint32_t offset)
{
	/* Writes the header, both flag groups and the post-sync address. */
	cs[0] = GFX_OP_PIPE_CONTROL(6) | bit_group_0;
	cs[1] = bit_group_1;
	cs[2] = offset;
	cs[3] = 0U;
	cs[4] = 0U;
	cs[5] = 0U;

	/* Reports where the next command goes. */
	return cs + 6;
}

/* Writes the flush of the request's engine class. */
static int
i915_emit_flush(
	struct i915_gt_request *rq,
	uint32_t mode)
{
	int error;

	/* Refuses a request without a context. */
	if (rq == NULL || rq->ce == NULL)
		return EINVAL;

	/* Chooses the render flush or the flush of the other engines. */
	if (rq->ce->ge->info->class == I915_RENDER_CLASS) {
		error = i915_emit_flush_rcs(rq, mode);
	} else {
		error = i915_emit_flush_xcs(rq, mode);
	}

	/* Reports the ring refusal. */
	if (error != 0)
		return error;

	/* Succeeded: the flush is in the ring. */
	return 0;
}

/* Writes the render engine's flush (gen12_emit_flush_rcs()). */
static int
i915_emit_flush_rcs(
	struct i915_gt_request *rq,
	uint32_t mode)
{
	uint32_t *cs;
	uint32_t bit_group_0;
	uint32_t bit_group_1;
	uint32_t flags;
	int engine_id;

	/* The AUX invalidate is chosen by the engine. */
	engine_id = rq->ce->ge->info->id;

	/*
	 * The flush block runs when EMIT_FLUSH is asked for or the engine needs
	 * the AUX invalidate; the second is true on Alder Lake-P, so it runs
	 * even for a pure invalidate.  The 12.70+ CCS flush and the dummy
	 * PIPE_CONTROL of Wa_14016712196 do not apply, and nothing is masked off
	 * because the engine has the 3D pipeline.
	 */
	bit_group_0 = PIPE_CONTROL0_HDC_PIPELINE_FLUSH;
	bit_group_1 = 0U;
	if ((mode & EMIT_FLUSH) != 0U)
		bit_group_1 |= PIPE_CONTROL_FLUSH_L3;
	bit_group_1 |= PIPE_CONTROL_TILE_CACHE_FLUSH;
	bit_group_1 |= PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH;
	bit_group_1 |= PIPE_CONTROL_DEPTH_CACHE_FLUSH;
	bit_group_1 |= PIPE_CONTROL_DEPTH_STALL;
	bit_group_1 |= PIPE_CONTROL_DC_FLUSH_ENABLE;
	bit_group_1 |= PIPE_CONTROL_FLUSH_ENABLE;
	bit_group_1 |= PIPE_CONTROL_STORE_DATA_INDEX;
	bit_group_1 |= PIPE_CONTROL_QW_WRITE;
	bit_group_1 |= PIPE_CONTROL_CS_STALL;

	/* Writes the flush block. */
	cs = drv_i915_ring_begin(rq, I915_FLUSH_RCS_FLUSH_DWORDS);
	if (cs == NULL)
		return rq->error;
	cs = i915_emit_pipe_control(cs, bit_group_0, bit_group_1, LRC_PPHWSP_SCRATCH_ADDR);
	drv_i915_ring_advance(rq, cs);

	/* An invalidate follows with the caches and the TLB. */
	if ((mode & EMIT_INVALIDATE) != 0U) {
		flags = 0U;
		flags |= PIPE_CONTROL_COMMAND_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_TLB_INVALIDATE;
		flags |= PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_VF_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_CONST_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_STATE_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_STORE_DATA_INDEX;
		flags |= PIPE_CONTROL_QW_WRITE;
		flags |= PIPE_CONTROL_CS_STALL;

		/* Reserves 8 dwords and 8 more for the AUX invalidate. */
		cs = drv_i915_ring_begin(rq, I915_FLUSH_RCS_INVALIDATE_DWORDS);
		if (cs == NULL)
			return rq->error;

		/*
		 * Keeps the pre-parser from running past the TLB invalidate and
		 * fetching a stale page for the request payload.
		 */
		*cs++ = i915_preparser_disable(1);
		cs = i915_emit_pipe_control(cs, 0U, flags, LRC_PPHWSP_SCRATCH_ADDR);
		cs = drv_i915_gen12_emit_aux_table_inv(engine_id, cs);
		*cs++ = i915_preparser_disable(0);
		drv_i915_ring_advance(rq, cs);
	}

	/* Succeeded: the flush is in the ring. */
	return 0;
}

/* Writes the flush of a copy, video or video enhancement engine (gen12_emit_flush_xcs()). */
static int
i915_emit_flush_xcs(
	struct i915_gt_request *rq,
	uint32_t mode)
{
	uint32_t *cs;
	uint32_t command;
	uint32_t count;
	uint32_t inv_reg;
	int engine_class;
	int engine_id;

	/* The flush depends on the engine's class and, for the AUX invalidate, on the engine. */
	engine_class = rq->ce->ge->info->class;
	engine_id = rq->ce->ge->info->id;

	/* An invalidate adds the pre-parser pair and the AUX invalidate. */
	count = I915_FLUSH_XCS_DWORDS;
	if ((mode & EMIT_INVALIDATE) != 0U)
		count += I915_FLUSH_XCS_INVALIDATE_DWORDS;

	/* Reserves the flush. */
	cs = drv_i915_ring_begin(rq, count);
	if (cs == NULL)
		return rq->error;

	/* Keeps the pre-parser behind the invalidate. */
	if ((mode & EMIT_INVALIDATE) != 0U)
		*cs++ = i915_preparser_disable(1);

	/*
	 * A command barrier is always required, so the commands after it (the
	 * breadcrumb interrupt) are ordered after the write-cache flush.
	 */
	command = (MI_FLUSH_DW + 1U) | MI_FLUSH_DW_STORE_INDEX | MI_FLUSH_DW_OP_STOREDW;
	if ((mode & EMIT_INVALIDATE) != 0U) {
		/* Invalidates the TLB, the BSD cache of a video engine, and the CCS of a copy engine. */
		command |= MI_INVALIDATE_TLB;
		if (engine_class == I915_VIDEO_DECODE_CLASS)
			command |= MI_INVALIDATE_BSD;
		if (engine_class == I915_COPY_ENGINE_CLASS)
			command |= MI_FLUSH_DW_CCS;
	}

	/* Writes the flush with its post-sync write to the scratch slot: upper address and value zero. */
	*cs++ = command;
	*cs++ = LRC_PPHWSP_SCRATCH_ADDR;
	*cs++ = 0U;
	*cs++ = 0U;

	/*
	 * The reference writes the AUX invalidate unconditionally but reserves
	 * room for it only with EMIT_INVALIDATE; it relies on its callers never
	 * asking only for EMIT_FLUSH on an engine that needs the invalidate.  A
	 * flush-only call on such an engine is refused instead of overrunning.
	 */
	if ((mode & EMIT_INVALIDATE) != 0U) {
		cs = drv_i915_gen12_emit_aux_table_inv(engine_id, cs);
		*cs++ = i915_preparser_disable(0);
	} else {
		inv_reg = drv_i915_lrc_aux_inv_reg(engine_id);
		if (inv_reg != 0U) {
			rq->error = EINVAL;
			return EINVAL;
		}
	}

	/* Moves the write position past the flush. */
	drv_i915_ring_advance(rq, cs);

	/* Succeeded: the flush is in the ring. */
	return 0;
}

/*
 * Writes the tail of the final breadcrumb and records rq->tail and
 * rq->wa_tail.
 *
 * The engines have semaphores and no GuC runs, so the tail waits on the
 * status page's PREEMPT dword to be zero (gen12_emit_preempt_busywait()).
 */
static uint32_t *
i915_emit_fini_breadcrumb_tail(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	struct i915_gt_ring *ring;

	/* The tail offsets are measured from the start of the ring. */
	ring = &rq->ce->ring;

	/* Raises the interrupt and allows arbitration. */
	*cs++ = MI_USER_INTERRUPT;
	*cs++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;

	/* Triggers IDLE to ACTIVE first, then waits for PREEMPT to be zero. */
	*cs++ = MI_ARB_CHECK;
	*cs++ = MI_SEMAPHORE_WAIT_TOKEN | MI_SEMAPHORE_GLOBAL_GTT | MI_SEMAPHORE_POLL | MI_SEMAPHORE_SAD_EQ_SDD;
	*cs++ = 0U;
	*cs++ = rq->preempt_ggtt;
	*cs++ = 0U;
	*cs++ = 0U;

	/* The request ends here; Wa_14014475959 is DG2 only. */
	rq->tail = (uint32_t)((char *)cs - (char *)ring->vaddr);

	/* Leaves at least one preemption point per request (gen8_emit_wa_tail()). */
	*cs++ = MI_ARB_CHECK;
	*cs++ = MI_NOOP;
	rq->wa_tail = (uint32_t)((char *)cs - (char *)ring->vaddr);

	/* Reports where the next command goes. */
	return cs;
}
