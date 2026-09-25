/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Execlists submission (see submit.h).
 */

#include "submit.h"
#include "context.h"
#include "engine.h"
#include "mmio.h"
#include "request.h"
#include "sync.h"
#include <kern/kcrt.h>

#include <kern/device-io.h>
#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>

#include "intel/gt-regs.h"

/* A masked register word that sets the named bits. */
#define I915_MASKED_ENABLE(bits)	((((uint32_t)(bits)) << 16) | ((uint32_t)(bits)))

/* A masked register word that clears the named bits. */
#define I915_MASKED_DISABLE(bits)	(((uint32_t)(bits)) << 16)

/* What a CSB entry holds before the engine has written it, and after it is consumed. */
#define I915_CSB_UNWRITTEN		(~(uint64_t)0)

/* How long an all-ones CSB entry is polled before the MMIO copy is read. */
#define I915_CSB_VISIBLE_POLL_US	10U

/* How many CSB entries the first MMIO status buffer holds; the rest are in the second. */
#define I915_CSB_MMIO_FIRST_ENTRIES	6U

/* The highest software context id plus one: the tag bits of the free mask. */
#define I915_CONTEXT_TAG_LIMIT		63

static void i915_ring_set_paused(struct i915_gt_engine *ge, int state);
static void i915_execlists_write(struct i915_gt_engine *ge, struct i915_mmio *mmio, uint32_t reg, uint32_t value);
static void i915_enable_error_interrupt(struct i915_gt_engine *ge, struct i915_mmio *mmio);
static int i915_gen12_csb_parse(uint64_t csb);
static int i915_schedule_in(struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_gt_request *rq);
static void i915_schedule_out(struct i915_execlists *el, struct i915_gt_request *rq);
static uint64_t i915_update_context(struct i915_gt_request *rq);
static void i915_write_desc(struct i915_gt_engine *ge, struct i915_mmio *mmio, uint64_t desc, unsigned port);
static uint64_t i915_csb_read(struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_mmio *mmio, unsigned index);

/*
 * Prepares the execlists state of an engine with nothing in flight.
 */
void
drv_i915_execlists_init(
	struct i915_execlists *el)
{
	/* Nothing to prepare without the state. */
	if (el == NULL)
		return;

	/* Starts with no ports loaded and no counts. */
	kern_memset(el, 0, sizeof(*el));

	/* Every software context id is free (GENMASK(BITS_PER_LONG - 2, 0)). */
	el->context_tag = (((uint64_t)1) << 63) - 1U;
}

/*
 * Points an engine's execlists state at its registers and status page
 * (intel_execlists_submission_setup()).
 *
 * Gen11+ submits through the 64-bit ELSQ register pair plus a control write,
 * not the four-dword ELSP, and keeps the CSB write pointer at status-page
 * dword 0x2f with 12 entries.
 */
void
drv_i915_execlists_submission_setup(
	struct i915_gt_engine *ge)
{
	uint32_t base;

	/* An engine without a status page has nothing to point at. */
	if (ge == NULL || ge->setup_done == 0)
		return;

	/* Names the submission and control registers. */
	base = ge->info->mmio_base;
	ge->submit_reg = RING_EXECLIST_SQ_CONTENTS(base);
	ge->ctrl_reg = RING_EXECLIST_CONTROL(base);

	/* Points at the CSB entries and the write pointer in the status page. */
	ge->csb_status = (volatile uint64_t *)&ge->hwsp[I915_HWS_CSB_BUF0_INDEX];
	ge->csb_write = &ge->hwsp[ICL_HWS_CSB_WRITE_INDEX];
	ge->csb_size = GEN11_CSB_ENTRIES;

	/*
	 * Gen11 to before 12.50 carries the engine class and instance in the
	 * descriptor's upper dword, so they are pre-shifted by 32 here exactly
	 * as the reference does.
	 */
	ge->ccid = 0U;
	ge->ccid |= (uint32_t)ge->info->instance << (GEN11_ENGINE_INSTANCE_SHIFT - 32);
	ge->ccid |= (uint32_t)ge->info->class << (GEN11_ENGINE_CLASS_SHIFT - 32);
}

/*
 * Enables execlists on an engine (enable_execlists()).
 *
 * Writes RING_HWSTAM, disables the legacy ring-buffer mode, clears STOP_RING,
 * points RING_HWS_PGA at the status page, and unmasks the instruction error.
 */
void
drv_i915_execlists_enable(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio)
{
	uint32_t base;

	/* Nothing to enable without a set-up engine and register access. */
	if (ge == NULL ||
	    ge->setup_done == 0 ||
	    mmio == NULL)
		return;

	/* The registers are relative to the engine's base. */
	base = ge->info->mmio_base;

	/* Lets the engine write every status-page dword (intel_engine_set_hwsp_writemask(engine, ~0u)). */
	i915_execlists_write(ge, mmio, RING_HWSTAM(base), ~0U);

	/* Disables the legacy ring-buffer mode; GFX_RUN_LIST_ENABLE is the pre-Gen11 bit. */
	i915_execlists_write(ge, mmio, RING_MODE_GEN7(base), I915_MASKED_ENABLE(GEN11_GFX_DISABLE_LEGACY_MODE));

	/* Lets the ring run. */
	i915_execlists_write(ge, mmio, RING_MI_MODE(base), I915_MASKED_DISABLE(STOP_RING));

	/* Gives the engine its status page. */
	i915_execlists_write(ge, mmio, RING_HWS_PGA(base), (uint32_t)ge->hwsp_ggtt);
	drv_i915_posting_read32(mmio, RING_HWS_PGA(base));

	/* Unmasks the instruction error and marks the engine resumed. */
	i915_enable_error_interrupt(ge, mmio);
	ge->resumed = 1;
}

/*
 * Resets the CSB pointers of an engine (reset_csb_pointers()).
 *
 * The reset preparation left PREEMPT at 1, and every breadcrumb tail ends in
 * a semaphore wait for it to be 0, so the engine is unpaused first.  Icelake
 * sometimes forgets to reset its pointers over a GPU reset, so the reference
 * writes them by hand -- twice, with the software head, the status-page write
 * pointer and the poisoned entries set between the two writes.
 */
void
drv_i915_execlists_reset_csb_pointers(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio)
{
	uint32_t base;
	uint32_t reset_value;
	uint32_t pointer;
	uint32_t index;

	/* Nothing to reset without a set-up engine and register access. */
	if (ge == NULL ||
	    ge->setup_done == 0 ||
	    mmio == NULL)
		return;

	/* Builds the pointer word: both pointers at the last entry, masked in. */
	base = ge->info->mmio_base;
	reset_value = (uint32_t)ge->csb_size - 1U;
	pointer = (0xffffU << 16) | (reset_value << 8) | reset_value;

	/* Unpauses the engine so the breadcrumb tails can finish. */
	i915_ring_set_paused(ge, 0);

	/* Writes the hardware pointers the first time. */
	drv_i915_write32(mmio, RING_CONTEXT_STATUS_PTR(base), pointer);
	drv_i915_posting_read32(mmio, RING_CONTEXT_STATUS_PTR(base));
	ge->csb_reset_writes++;

	/*
	 * After a reset the hardware starts writing at entry 0, so the head is
	 * parked one entry behind it: the first entry compared is entry 0 even
	 * though no interrupt has arrived yet.
	 */
	ge->csb_head = reset_value;
	*ge->csb_write = reset_value;
	kern_io_write_barrier();

	/* Poisons every entry, so a stale entry cannot pass for a real one. */
	for (index = 0U; index <= reset_value; index++)
		ge->csb_status[index] = I915_CSB_UNWRITTEN;

	/* Writes the hardware pointers the second time. */
	drv_i915_write32(mmio, RING_CONTEXT_STATUS_PTR(base), pointer);
	drv_i915_posting_read32(mmio, RING_CONTEXT_STATUS_PTR(base));
	ge->csb_reset_writes++;
}

/*
 * Prepares an engine for a reset (execlists_reset_prepare()).
 *
 * Pauses the engine, stops its command streamer, and drains the pending
 * MI_FORCE_WAKEs (Wa_22011802037).  There is no tasklet to disable.  The
 * streamer stop's result is kept in the engine for the caller.
 */
void
drv_i915_execlists_reset_prepare(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio)
{
	/* Nothing to prepare without an engine and register access. */
	if (ge == NULL || mmio == NULL)
		return;

	/* Pauses the engine at its next breadcrumb tail. */
	i915_ring_set_paused(ge, 1);

	/* Stops the command streamer and records how that went. */
	ge->stop_cs_rc = drv_i915_engine_stop_cs(ge, mmio);

	/* Waits for the forcewakes the streamer left pending. */
	drv_i915_engine_wait_for_pending_mi_fw(ge, mmio);
}

/*
 * Submits one request to an idle engine: schedule-in, the context update and
 * the ELSQ write.
 *
 * One request is in flight per engine at a time.  Returns 0, EINVAL for a
 * request that was not added, EBUSY while a request is in flight or no
 * context id is free.
 */
int
drv_i915_execlists_submit(
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_mmio *mmio,
	struct i915_gt_request *rq)
{
	struct i915_gt_request *request;
	uint64_t desc;
	unsigned port;
	int error;

	/* Refuses a submission of anything but a completed request. */
	if (ge == NULL ||
	    el == NULL ||
	    mmio == NULL ||
	    rq == NULL ||
	    rq->added == 0)
		return EINVAL;

	/* Refuses a second request while one is in flight. */
	if (el->have_active != 0 || el->pending[0] != NULL)
		return EBUSY;

	/* Gives the context a software context id. */
	error = i915_schedule_in(ge, el, rq);
	if (error != 0)
		return error;

	/* Loads the request into the first port; the second stays empty. */
	el->pending[0] = rq;
	el->pending[1] = NULL;

	/*
	 * The ELSQ is not cleared after it is submitted, so both ports are
	 * always written -- the empty one with zero -- highest port first.
	 */
	port = 2U;
	while (port > 0U) {
		port--;
		request = el->pending[port];
		desc = 0U;
		if (request != NULL)
			desc = i915_update_context(request);
		i915_write_desc(ge, mmio, desc, port);
	}

	/* Loads the submit queue by hand. */
	drv_i915_write32(mmio, ge->ctrl_reg, EL_CTRL_LOAD);

	/* A submission moves the engine's serial, which the park switch compares. */
	el->serial++;
	el->submits++;

	/* Succeeded: the request is loaded. */
	return 0;
}

/*
 * Walks the CSB from the cached head to the write pointer (process_csb()).
 *
 * Returns the request that completed, or NULL.  Events that have nothing to
 * apply to, and a write pointer beyond the ring, count in el->csb_errors.
 */
struct i915_gt_request *
drv_i915_execlists_process_csb(
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_mmio *mmio)
{
	struct i915_gt_request *done;
	struct i915_gt_request *rq;
	uint64_t csb;
	unsigned head;
	unsigned tail;
	int promote;

	/* Nothing to walk without an engine and its state. */
	if (ge == NULL || el == NULL)
		return NULL;

	/* Nothing is new while the head has caught up with the write pointer. */
	head = ge->csb_head;
	tail = *ge->csb_write;
	if (head == tail)
		return NULL;

	/* A pointer beyond the ring is not something to walk. */
	if (tail >= ge->csb_size) {
		el->csb_errors++;
		return NULL;
	}

	/* Consumes up to the write pointer; the entries are read after it. */
	ge->csb_head = tail;
	kern_io_read_barrier();

	/* Applies every new entry in order. */
	done = NULL;
	do {
		/* Moves to the next entry, wrapping at the end of the ring. */
		head++;
		if (head == ge->csb_size)
			head = 0U;

		/* Reads the entry and keeps it for the report. */
		csb = i915_csb_read(ge, el, mmio, head);
		el->csb_events++;
		el->last_csb_lo = (uint32_t)csb;
		el->last_csb_hi = (uint32_t)(csb >> 32);

		/* Decides between a promotion and a completion. */
		promote = i915_gen12_csb_parse(csb);
		if (promote != 0) {
			/* A promotion with nothing pending is an error (ERROR_CSB). */
			if (el->pending[0] == NULL) {
				el->csb_errors++;
				break;
			}

			/* The pending ports become active; anything that was active is switched out. */
			el->inflight[0] = el->pending[0];
			el->inflight[1] = el->pending[1];
			el->have_active = 1;
			el->pending[0] = NULL;
			el->pending[1] = NULL;
			el->promotes++;
		} else {
			/* A completion with nothing active is an error (ERROR_CSB). */
			if (el->have_active == 0 || el->inflight[0] == NULL) {
				el->csb_errors++;
				break;
			}

			/* The active request leaves; the second port, if any, becomes active. */
			rq = el->inflight[0];
			el->inflight[0] = el->inflight[1];
			el->inflight[1] = NULL;
			if (el->inflight[0] == NULL)
				el->have_active = 0;

			/* Gives its context id back and reports it as done. */
			i915_schedule_out(el, rq);
			el->completes++;
			done = rq;
		}
	} while (head != tail);

	/* Reports the last request that completed, or NULL. */
	return done;
}

/* Sets the status page's PREEMPT dword the breadcrumb tails wait on (ring_set_paused()). */
static void
i915_ring_set_paused(
	struct i915_gt_engine *ge,
	int state)
{
	/* Nothing to pause without a status page. */
	if (ge == NULL || ge->hwsp == NULL)
		return;

	/* A nonzero PREEMPT holds every breadcrumb tail; a pause must be visible first. */
	ge->hwsp[I915_GEM_HWS_PREEMPT] = (uint32_t)state;
	if (state != 0)
		kern_io_write_barrier();
}

/* Writes one enable register and counts it. */
static void
i915_execlists_write(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio,
	uint32_t reg,
	uint32_t value)
{
	/* Writes the register. */
	drv_i915_write32(mmio, reg, value);

	/* Counts the write for the report. */
	ge->enable_writes++;
}

/* Clears the engine's errors, records any left, and unmasks only the fatal one (enable_error_interrupt()). */
static void
i915_enable_error_interrupt(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio)
{
	uint32_t base;

	/* The registers are relative to the engine's base. */
	base = ge->info->mmio_base;

	/* Masks every error and clears all existing ones. */
	i915_execlists_write(ge, mmio, RING_EMR(base), ~0U);
	i915_execlists_write(ge, mmio, RING_EIR(base), ~0U);

	/*
	 * The reference resets the engine when an error remains.  It is logged
	 * instead of resetting behind the caller's back: a nonzero ESR at resume
	 * is a condition the caller must see.
	 */
	ge->esr_at_resume = drv_i915_read32(mmio, RING_ESR(base));
	if (ge->esr_at_resume != 0U) {
		kern_logf("i915: engine '%s' resumed still in error: %08x\n",
		    ge->info->name,
		    ge->esr_at_resume);
	}

	/*
	 * Unmasks only the instruction error.  The privilege error fires for
	 * cases the hardware already suppresses, so the reference leaves it
	 * masked.
	 */
	i915_execlists_write(ge, mmio, RING_EMR(base), ~(uint32_t)I915_ERROR_INSTRUCTION);
}

/* Reports 1 when a Gen12 CSB entry promotes the pending ports, 0 when it completes the active one. */
static int
i915_gen12_csb_parse(
	uint64_t csb)
{
	uint32_t lower;
	uint32_t upper;
	uint32_t away_id;

	/* Splits the entry into its lower and upper dword. */
	lower = (uint32_t)csb;
	upper = (uint32_t)(csb >> 32);

	/*
	 * Nothing switched away, or a new queue was loaded, means the pending
	 * ports were promoted (__gen12_csb_parse()).
	 */
	away_id = (upper & GEN12_CSB_SW_CTX_ID_MASK) >> 15;
	if (away_id == GEN12_IDLE_CTX_ID)
		return 1;
	if ((lower & GEN12_CTX_STATUS_SWITCHED_TO_NEW_QUEUE) != 0U)
		return 1;

	/*
	 * Anything else is the active context completing.  The switch detail
	 * is zero: an unsuccessful semaphore wait is never used, the tail
	 * always polls.
	 */
	return 0;
}

/* Gives the request's context the lowest free context id and fills the descriptor's upper dword. */
static int
i915_schedule_in(
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_gt_request *rq)
{
	struct i915_gt_context *ce;
	uint32_t ccid;
	int tag;

	/* Refuses when every context id is taken. */
	if (el->context_tag == 0U)
		return EBUSY;

	/* Finds the lowest free id (__ffs()) and takes it. */
	tag = 0;
	while (((el->context_tag >> tag) & 1U) == 0U)
		tag++;
	el->context_tag &= ~(((uint64_t)1) << tag);

	/* Gen12 before 12.50: (1 + tag) << (GEN11_SW_CTX_ID_SHIFT - 32), with the engine fields. */
	ccid = ((uint32_t)(1 + tag)) << (GEN11_SW_CTX_ID_SHIFT - 32);
	ccid |= ge->ccid;

	/* Puts the fields into the descriptor's upper dword and records the id. */
	ce = rq->ce;
	ce->lrc_desc = (((uint64_t)ccid) << 32) | (uint32_t)ce->lrc_desc;
	ce->tag = tag;

	/* Succeeded: the context is scheduled in. */
	return 0;
}

/* Gives the request's context id back (__execlists_schedule_out()). */
static void
i915_schedule_out(
	struct i915_execlists *el,
	struct i915_gt_request *rq)
{
	struct i915_gt_context *ce;

	/* Frees the id the context held, if it held one. */
	ce = rq->ce;
	if (ce->tag >= 0 && ce->tag < I915_CONTEXT_TAG_LIMIT) {
		el->context_tag |= ((uint64_t)1) << ce->tag;
		ce->tag = -1;
	}
}

/*
 * Writes the request's tail into the context image and returns the
 * descriptor to submit (execlists_update_context()).
 */
static uint64_t
i915_update_context(
	struct i915_gt_request *rq)
{
	struct i915_gt_context *ce;
	uint64_t desc;
	uint32_t previous;
	uint32_t tail;
	uint32_t size;
	unsigned wrap;
	int32_t direction;

	/* Starts from the context's descriptor and the ring's previous tail. */
	ce = rq->ce;
	desc = ce->lrc_desc;
	previous = ce->ring.tail;
	tail = rq->tail;

	/* Moves the ring's tail to the request's (intel_ring_set_tail()). */
	ce->ring.tail = tail;

	/*
	 * intel_ring_direction(): the sign of (next - previous) within the
	 * ring, shifted up by ring->wrap = 32 - ilog2(size).
	 */
	wrap = 32U;
	size = ce->ring.size;
	while (size > 1U) {
		size >>= 1;
		wrap--;
	}

	/* Resubmitting the same or an earlier tail must force a full restore. */
	direction = (int32_t)((tail - previous) << wrap);
	if (direction <= 0)
		desc |= CTX_DESC_FORCE_RESTORE;

	/* Writes the tail into the image; the ring tail moves on past the workaround tail. */
	ce->lrc_reg_state[CTX_RING_TAIL] = tail;
	rq->tail = rq->wa_tail;

	/* The context image must be complete before the uncached ELSQ write. */
	kern_io_write_barrier();

	/* The forced restore applies to this submission only. */
	ce->lrc_desc &= ~(uint64_t)CTX_DESC_FORCE_RESTORE;

	/* Reports the descriptor to write. */
	return desc;
}

/* Writes one ELSQ port: lower dword first, then upper (execlists_submit_ports()). */
static void
i915_write_desc(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio,
	uint64_t desc,
	unsigned port)
{
	/* Each port is two dwords from the queue's first register. */
	drv_i915_write32(mmio, ge->submit_reg + port * 8U, (uint32_t)desc);
	drv_i915_write32(mmio, ge->submit_reg + port * 8U + 4U, (uint32_t)(desc >> 32));
}

/*
 * Reads and consumes one CSB entry (csb_read() and wa_csb_read()).
 *
 * The GPU does not always make an entry visible before the write pointer
 * (tgl:HSDES#22011248461): an all-ones entry is polled for 10 us, as the
 * reference's preempt-off busy wait does, and then read from the MMIO copy of
 * the buffer.  The entry is poisoned after reading so a reuse of its slot can
 * be spotted.
 */
static uint64_t
i915_csb_read(
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_mmio *mmio,
	unsigned index)
{
	volatile uint64_t *slot;
	uint64_t entry;
	uint32_t status;
	uint32_t reg;
	uint32_t lower;
	uint32_t upper;
	unsigned buffer_index;
	unsigned waited_us;

	/* Reads the entry from the status page. */
	slot = &ge->csb_status[index];
	entry = *slot;

	/* Gives an entry that is not visible yet up to 10 us. */
	if (entry == I915_CSB_UNWRITTEN) {
		for (waited_us = 0U; waited_us < I915_CSB_VISIBLE_POLL_US; waited_us++) {
			if (entry != I915_CSB_UNWRITTEN)
				break;
			(void)drv_i915_udelay(1U);
			entry = *slot;
		}

		/* Falls back to the MMIO copy when the entry never became visible. */
		if (entry == I915_CSB_UNWRITTEN) {
			/* Chooses the MMIO status buffer that holds this entry. */
			status = GEN8_EXECLISTS_STATUS_BUF;
			buffer_index = index;
			if (buffer_index >= I915_CSB_MMIO_FIRST_ENTRIES) {
				status = GEN11_EXECLISTS_STATUS_BUF2;
				buffer_index -= I915_CSB_MMIO_FIRST_ENTRIES;
			}

			/*
			 * Reads the entry over MMIO (intel_uncore_read64()); register
			 * access is 32-bit, so the lower half is read first.
			 */
			reg = ge->info->mmio_base + status + 8U * buffer_index;
			lower = drv_i915_read32(mmio, reg);
			upper = drv_i915_read32(mmio, reg + 4U);
			entry = (uint64_t)lower | ((uint64_t)upper << 32);
			el->csb_mmio_fallback++;
		} else {
			/* The entry became visible within the poll. */
			el->csb_late++;
		}
	}

	/* Consumes the entry so a future reuse of the slot can be spotted. */
	*slot = I915_CSB_UNWRITTEN;

	/* Reports the entry. */
	return entry;
}
