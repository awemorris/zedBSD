/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Execlists submission (see submit.h).
 *
 * What differs between the generations is one layout record: Gen12
 * submits through the ELSQ register pair and a control write, keeps 12 CSB
 * entries and puts the engine's class and instance into the descriptor; a
 * Gen9 engine submits through the four-dword ELSP, upper dword first and the
 * higher port first, keeps 6 CSB entries behind status-page dword 0x1f,
 * unmasks its own interrupts at resume, and reads a promotion from the
 * idle-to-active and preempted bits of an entry.
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

/* The execlists layout of one generation. */
struct i915_execlists_layout {
	/* Nonzero: the ELSQ pair and a control write; zero: the four-dword ELSP. */
	int queue;

	/* The status-page dword of the CSB write pointer, and the CSB length. */
	unsigned csb_write_index;
	unsigned csb_entries;

	/* Nonzero: the descriptor carries the engine's class and instance. */
	int engine_fields;

	/* The RING_MODE bit that turns the legacy ring-buffer mode off. */
	uint32_t mode_bit;

	/* Nonzero: the engine unmasks its own interrupts at resume. */
	int engine_irqs;

	/* Reports 1 when a CSB entry promotes the pending ports, 0 when it completes the active one. */
	int (*promotes)(uint64_t csb);
};

static int i915_execlists_promotes_gen12(uint64_t csb);
static int i915_execlists_promotes_gen9(uint64_t csb);

/* The Gen12 layout. */
static const struct i915_execlists_layout i915_execlists_gen12 = {
	1, I915_GT_ICL_HWS_CSB_WRITE_INDEX, I915_GT_GEN11_CSB_ENTRIES, 1,
	I915_GT_GEN11_GFX_DISABLE_LEGACY_MODE, 0, i915_execlists_promotes_gen12
};

/*
 * The Gen11 layout: the ELSQ pair, the descriptor's engine fields and the
 * Gen11 CSB of Gen12, but the CSB entries are the Gen8 events.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/gt/intel_execlists_submission.c
 * (intel_execlists_submission_setup, csb_parse: graphics versions below 12
 * take gen8_csb_parse).
 */
static const struct i915_execlists_layout i915_execlists_gen11 = {
	1, I915_GT_ICL_HWS_CSB_WRITE_INDEX, I915_GT_GEN11_CSB_ENTRIES, 1,
	I915_GT_GEN11_GFX_DISABLE_LEGACY_MODE, 0, i915_execlists_promotes_gen9
};

/* The Gen9 layout. */
static const struct i915_execlists_layout i915_execlists_gen9 = {
	0, I915_GT_HWS_CSB_WRITE_INDEX, I915_GT_GEN8_CSB_ENTRIES, 0,
	I915_GT_GFX_RUN_LIST_ENABLE, 1, i915_execlists_promotes_gen9
};

static const struct i915_execlists_layout *i915_execlists_layout_of(const struct i915_gt_engine *ge);
static void i915_execlists_pause(struct i915_gt_engine *ge, int paused);
static void i915_execlists_write(struct i915_gt_engine *ge, struct i915_mmio *mmio, uint32_t reg, uint32_t value);
static void i915_execlists_open_errors(struct i915_gt_engine *ge, struct i915_mmio *mmio);
static uint32_t i915_execlists_engine_irqs(const struct i915_gt_engine *ge);
static void i915_execlists_report(struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_mmio *mmio, const char *what, unsigned head, unsigned tail);
static int i915_execlists_take_tag(struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_gt_request *rq);
static void i915_execlists_give_tag(struct i915_execlists *el, struct i915_gt_request *rq);
static uint64_t i915_execlists_prepare_context(struct i915_gt_request *rq);
static void i915_execlists_write_port(struct i915_gt_engine *ge, struct i915_mmio *mmio, uint64_t desc, unsigned port);
static uint64_t i915_execlists_read_csb(struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_mmio *mmio, unsigned index);

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

	/* Every software context id below 63 is free. */
	el->context_tag = (((uint64_t)1) << 63) - 1U;
}

/*
 * Points an engine's execlists state at its registers and status page, by
 * the engine's layout.
 */
void
drv_i915_execlists_bind(
	struct i915_gt_engine *ge)
{
	const struct i915_execlists_layout *layout;
	uint32_t base;

	/* An engine without a status page has nothing to point at. */
	if (ge == NULL || ge->setup_done == 0)
		return;

	/* Names the submission registers: the ELSQ and its control, or the ELSP alone. */
	layout = i915_execlists_layout_of(ge);
	base = ge->info->mmio_base;
	if (layout->queue != 0) {
		ge->submit_reg = I915_GT_RING_EXECLIST_SQ_CONTENTS(base);
		ge->ctrl_reg = I915_GT_RING_EXECLIST_CONTROL(base);
	} else {
		ge->submit_reg = I915_GT_RING_ELSP(base);
		ge->ctrl_reg = 0U;
	}

	/* Points at the CSB entries and the write pointer in the status page. */
	ge->csb_status = (volatile uint64_t *)&ge->hwsp[I915_GT_HWS_CSB_BUF0_INDEX];
	ge->csb_write = &ge->hwsp[layout->csb_write_index];
	ge->csb_size = layout->csb_entries;

	/* The engine fields of the descriptor's upper dword, pre-shifted down by 32. */
	ge->ccid = 0U;
	if (layout->engine_fields != 0) {
		ge->ccid |= (uint32_t)ge->info->instance << (I915_GT_GEN11_ENGINE_INSTANCE_SHIFT - 32);
		ge->ccid |= (uint32_t)ge->info->class << (I915_GT_GEN11_ENGINE_CLASS_SHIFT - 32);
	}
}

/*
 * Enables execlists on an engine.
 *
 * Writes I915_GT_RING_HWSTAM, disables the legacy ring-buffer mode, clears I915_GT_STOP_RING,
 * points I915_GT_RING_HWS_PGA at the status page, and unmasks the instruction error.
 */
void
drv_i915_execlists_enable(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio)
{
	const struct i915_execlists_layout *layout;
	uint32_t base;
	uint32_t engine_irqs;

	/* Nothing to enable without a set-up engine and register access. */
	if (ge == NULL ||
	    ge->setup_done == 0 ||
	    mmio == NULL)
		return;

	/* The registers are relative to the engine's base. */
	layout = i915_execlists_layout_of(ge);
	base = ge->info->mmio_base;

	/* Masks no status-page write: the engine writes every dword. */
	i915_execlists_write(ge, mmio, I915_GT_RING_HWSTAM(base), ~0U);

	/* Turns the legacy ring-buffer mode off. */
	i915_execlists_write(ge, mmio, I915_GT_RING_MODE_GEN7(base), I915_MASKED_ENABLE(layout->mode_bit));

	/* Lets the ring run. */
	i915_execlists_write(ge, mmio, I915_GT_RING_MI_MODE(base), I915_MASKED_DISABLE(I915_GT_STOP_RING));

	/* Gives the engine its status page. */
	i915_execlists_write(ge, mmio, I915_GT_RING_HWS_PGA(base), (uint32_t)ge->hwsp_ggtt);
	drv_i915_posting_read32(mmio, I915_GT_RING_HWS_PGA(base));

	/*
	 * A Gen9 engine also unmasks its own interrupts: the user interrupt,
	 * the context switch, the error and the semaphore wait, at the engine's
	 * shift in its GT bank.
	 */
	if (layout->engine_irqs != 0) {
		engine_irqs = i915_execlists_engine_irqs(ge);
		i915_execlists_write(ge, mmio, I915_GT_RING_IMR(base), ~engine_irqs);
		drv_i915_posting_read32(mmio, I915_GT_RING_IMR(base));
	}

	/* Unmasks the instruction error and marks the engine resumed. */
	i915_execlists_open_errors(ge, mmio);
	ge->resumed = 1;
}

/*
 * Restarts the CSB of an engine.
 *
 * The quiesce left PREEMPT at 1, and every breadcrumb tail ends in a
 * semaphore wait for it to be 0, so the engine is unpaused first.  The
 * engine does not always reset its pointers over a GPU reset, so they are
 * written by hand -- twice, with the software head, the status-page write
 * pointer and the poisoned entries set between the two writes.
 */
void
drv_i915_execlists_csb_restart(
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
	i915_execlists_pause(ge, 0);

	/* Writes the hardware pointers the first time. */
	drv_i915_write32(mmio, I915_GT_RING_CONTEXT_STATUS_PTR(base), pointer);
	drv_i915_posting_read32(mmio, I915_GT_RING_CONTEXT_STATUS_PTR(base));
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
	drv_i915_write32(mmio, I915_GT_RING_CONTEXT_STATUS_PTR(base), pointer);
	drv_i915_posting_read32(mmio, I915_GT_RING_CONTEXT_STATUS_PTR(base));
	ge->csb_reset_writes++;
}

/*
 * Quiesces an engine before a reset.
 *
 * Pauses the engine, stops its command streamer, and drains the pending
 * MI_FORCE_WAKEs (Wa_22011802037).  There is no tasklet to disable.  The
 * streamer stop's result is kept in the engine for the caller.
 */
void
drv_i915_execlists_quiesce(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio)
{
	/* Nothing to prepare without an engine and register access. */
	if (ge == NULL || mmio == NULL)
		return;

	/* Pauses the engine at its next breadcrumb tail. */
	i915_execlists_pause(ge, 1);

	/* Stops the command streamer and records how that went. */
	ge->stop_cs_rc = drv_i915_engine_stop_cs(ge, mmio);

	/* Waits for the forcewakes the streamer left pending. */
	drv_i915_engine_wait_for_pending_mi_fw(ge, mmio);
}

/*
 * Abandons the requests an engine reset ended and starts the CSB over.
 *
 * The engine runs one request at a time and the worker abandons the one
 * that hung, so nothing is unwound into a ring: each request in a port gives
 * its context id back, the ports are emptied, the CSB errors of the run that
 * hung are forgotten, and the CSB pointers are reset, which also unpauses
 * the engine.  The CSB is not walked first: what it says about the
 * abandoned request no longer matters.
 */
void
drv_i915_execlists_abandon(
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_mmio *mmio)
{
	unsigned port;

	/* Nothing to rewind without the state. */
	if (ge == NULL || el == NULL || mmio == NULL)
		return;

	/* Gives back the context ids of the requests the engine had acknowledged. */
	for (port = 0U; port < 2U; port++) {
		if (el->inflight[port] != NULL)
			i915_execlists_give_tag(el, el->inflight[port]);
	}

	/* Gives back those of the requests still waiting for their acknowledge, each once. */
	for (port = 0U; port < 2U; port++) {
		if (el->pending[port] == NULL)
			continue;
		if (el->pending[port] == el->inflight[0] || el->pending[port] == el->inflight[1])
			continue;

		i915_execlists_give_tag(el, el->pending[port]);
	}

	/* Empties the ports: the engine holds nothing from now on. */
	el->pending[0] = NULL;
	el->pending[1] = NULL;
	el->inflight[0] = NULL;
	el->inflight[1] = NULL;
	el->have_active = 0;

	/* A CSB error fails every later request; those of the run that hung end with it. */
	el->csb_errors = 0U;

	/* Starts the CSB over and unpauses the engine. */
	drv_i915_execlists_csb_restart(ge, mmio);
}

/*
 * Submits one request to an idle engine: a context id, the context's tail
 * and the port writes.
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
	error = i915_execlists_take_tag(ge, el, rq);
	if (error != 0)
		return error;

	/* Loads the request into the first port; the second stays empty. */
	el->pending[0] = rq;
	el->pending[1] = NULL;

	/*
	 * The ELSQ is not cleared after it is submitted, so both ports are
	 * always written -- the empty one with zero -- highest port first.
	 * Gen9's ELSP takes the same order.
	 */
	port = 2U;
	while (port > 0U) {
		port--;
		request = el->pending[port];
		desc = 0U;
		if (request != NULL)
			desc = i915_execlists_prepare_context(request);
		i915_execlists_write_port(ge, mmio, desc, port);
	}

	/* Loads the submit queue by hand; the ELSP loads itself on its last dword. */
	if (ge->ctrl_reg != 0U)
		drv_i915_write32(mmio, ge->ctrl_reg, I915_GT_EL_CTRL_LOAD);

	/* A submission moves the engine's serial, which the park switch compares. */
	el->serial++;
	el->submits++;

	/* Succeeded: the request is loaded. */
	return 0;
}

/*
 * Walks the CSB from the cached head to the write pointer.
 *
 * Returns the request that completed, or NULL.  Events that have nothing to
 * apply to, and a write pointer beyond the ring, count in el->csb_errors.
 */
struct i915_gt_request *
drv_i915_execlists_drain_csb(
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_mmio *mmio)
{
	const struct i915_execlists_layout *layout;
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
		i915_execlists_report(ge, el, mmio, "write pointer beyond the ring", head, tail);
		return NULL;
	}

	/* Consumes up to the write pointer; the entries are read after it. */
	ge->csb_head = tail;
	kern_io_read_barrier();

	/* Applies every new entry in order. */
	layout = i915_execlists_layout_of(ge);
	done = NULL;
	do {
		/* Moves to the next entry, wrapping at the end of the ring. */
		head++;
		if (head == ge->csb_size)
			head = 0U;

		/* Reads the entry and keeps it for the report and the history. */
		csb = i915_execlists_read_csb(ge, el, mmio, head);
		el->csb_events++;
		el->last_csb_lo = (uint32_t)csb;
		el->last_csb_hi = (uint32_t)(csb >> 32);
		el->csb_history[el->csb_history_next % 8U] = csb;
		el->csb_history_next++;

		/* Decides between a promotion and a completion, by the engine's CSB format. */
		promote = layout->promotes(csb);

		if (promote != 0) {
			/* A promotion with nothing pending is an error. */
			if (el->pending[0] == NULL) {
				el->csb_errors++;
				i915_execlists_report(ge, el, mmio, "promotion with nothing pending", head, tail);
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
			/* A completion with nothing active is an error. */
			if (el->have_active == 0 || el->inflight[0] == NULL) {
				el->csb_errors++;
				i915_execlists_report(ge, el, mmio, "completion with nothing active", head, tail);
				break;
			}

			/* The active request leaves; the second port, if any, becomes active. */
			rq = el->inflight[0];
			el->inflight[0] = el->inflight[1];
			el->inflight[1] = NULL;
			if (el->inflight[0] == NULL)
				el->have_active = 0;

			/* Gives its context id back and reports it as done. */
			i915_execlists_give_tag(el, rq);
			el->completes++;
			done = rq;
		}
	} while (head != tail);

	/* Reports the last request that completed, or NULL. */
	return done;
}

/* Returns the execlists layout of an engine. */
static const struct i915_execlists_layout *
i915_execlists_layout_of(
	const struct i915_gt_engine *ge)
{
	/* A Gen9 engine has the Gen9 layout, a Gen11 one the Gen11 layout; every other one the Gen12 layout. */
	if (ge->info->graphics_ver == 9)
		return &i915_execlists_gen9;
	if (ge->info->graphics_ver == 11)
		return &i915_execlists_gen11;
	return &i915_execlists_gen12;
}

/* Sets the status page's PREEMPT dword the breadcrumb tails wait on. */
static void
i915_execlists_pause(
	struct i915_gt_engine *ge,
	int paused)
{
	/* Nothing to pause without a status page. */
	if (ge == NULL || ge->hwsp == NULL)
		return;

	/* A nonzero PREEMPT holds every breadcrumb tail; a pause must be visible first. */
	ge->hwsp[I915_GT_HWS_PREEMPT] = (uint32_t)paused;
	if (paused != 0)
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

/* Clears the engine's errors, records any left, and unmasks only the fatal one. */
static void
i915_execlists_open_errors(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio)
{
	uint32_t base;

	/* The registers are relative to the engine's base. */
	base = ge->info->mmio_base;

	/* Masks every error and clears all existing ones. */
	i915_execlists_write(ge, mmio, I915_GT_RING_EMR(base), ~0U);
	i915_execlists_write(ge, mmio, I915_GT_RING_EIR(base), ~0U);

	/*
	 * An error that remains is logged rather than reset behind the
	 * caller's back: a nonzero ESR at resume is a condition the caller must
	 * see.
	 */
	ge->esr_at_resume = drv_i915_read32(mmio, I915_GT_RING_ESR(base));
	if (ge->esr_at_resume != 0U) {
		kern_logf("i915: engine '%s' resumed still in error: %08x\n",
		    ge->info->name,
		    ge->esr_at_resume);
	}

	/*
	 * Unmasks only the instruction error.  The privilege error fires for
	 * cases the hardware already suppresses, so it stays masked.
	 */
	i915_execlists_write(ge, mmio, I915_GT_RING_EMR(base), ~(uint32_t)I915_GT_ERROR_INSTRUCTION);
}

/*
 * Logs the first CSB events that have nothing to apply to: the
 * event, the walk's head and write pointer, the ports, the counters, the
 * last eight entries (newest last) and the engine's own view: the
 * execlist status, the CSB pointers register, the ring head and tail, the
 * active head and the error registers.  Later errors are only counted.
 */
static void
i915_execlists_report(
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_mmio *mmio,
	const char *what,
	unsigned head,
	unsigned tail)
{
	uint32_t base;
	unsigned index;
	unsigned slot;

	/* Only the first few errors are worth a report. */
	if (el->csb_errors > 4U)
		return;

	/* The walk and the ports. */
	base = ge->info->mmio_base;
	kern_logf("i915: execlists: CSB error %u (%s): head %u tail %u size %u; pending %d/%d inflight %d/%d active %d; "
	    "events %u promotes %u completes %u submits %u late %u mmio %u\n",
	    el->csb_errors,
	    what,
	    head,
	    tail,
	    ge->csb_size,
	    el->pending[0] != NULL,
	    el->pending[1] != NULL,
	    el->inflight[0] != NULL,
	    el->inflight[1] != NULL,
	    el->have_active,
	    el->csb_events,
	    el->promotes,
	    el->completes,
	    el->submits,
	    el->csb_late,
	    el->csb_mmio_fallback);

	/* The last eight entries, oldest first. */
	for (index = 0U; index < 8U; index++) {
		slot = (el->csb_history_next + index) % 8U;
		kern_logf("i915: execlists: CSB history %u: %08x:%08x\n",
		    index,
		    (uint32_t)(el->csb_history[slot] >> 32),
		    (uint32_t)el->csb_history[slot]);
	}

	/* The engine's registers. */
	kern_logf("i915: execlists: status %08x:%08x csb_ptr %08x head %08x tail %08x acthd %08x ipehr %08x eir %08x esr %08x\n",
	    drv_i915_read32(mmio, I915_GT_RING_EXECLIST_STATUS_HI(base)),
	    drv_i915_read32(mmio, I915_GT_RING_EXECLIST_STATUS_LO(base)),
	    drv_i915_read32(mmio, I915_GT_RING_CONTEXT_STATUS_PTR(base)),
	    drv_i915_read32(mmio, I915_GT_RING_HEAD(base)),
	    drv_i915_read32(mmio, I915_GT_RING_TAIL(base)),
	    drv_i915_read32(mmio, I915_GT_RING_ACTHD(base)),
	    drv_i915_read32(mmio, I915_GT_RING_IPEHR(base)),
	    drv_i915_read32(mmio, I915_GT_RING_EIR(base)),
	    drv_i915_read32(mmio, I915_GT_RING_ESR(base)));
}

/* Reports 1 when a Gen12 CSB entry promotes the pending ports, 0 when it completes the active one. */
static int
i915_execlists_promotes_gen12(
	uint64_t csb)
{
	uint32_t lower;
	uint32_t upper;
	uint32_t away_id;

	/* Splits the entry into its lower and upper dword. */
	lower = (uint32_t)csb;
	upper = (uint32_t)(csb >> 32);

	/* Nothing switched away, or a new queue was loaded, means the pending ports were promoted. */
	away_id = (upper & I915_GT_GEN12_CSB_SW_CTX_ID_MASK) >> 15;
	if (away_id == I915_GT_GEN12_IDLE_CTX_ID)
		return 1;
	if ((lower & I915_GT_GEN12_CTX_STATUS_SWITCHED_TO_NEW_QUEUE) != 0U)
		return 1;

	/*
	 * Anything else is the active context completing.  The switch detail
	 * is zero: an unsuccessful semaphore wait is never used, the tail
	 * always polls.
	 */
	return 0;
}

/*
 * Reports 1 when a Gen9 CSB entry promotes the pending ports, 0 when it
 * completes the active one: an idle-to-active switch or a preemption loads
 * the pending ports.
 */
static int
i915_execlists_promotes_gen9(
	uint64_t csb)
{
	uint32_t lower;

	/* The status bits are in the lower dword. */
	lower = (uint32_t)csb;

	/* The engine went from idle to the pending ports. */
	if ((lower & I915_GT_GEN8_CTX_STATUS_IDLE_ACTIVE) != 0U)
		return 1;

	/* The pending ports preempted the active ones. */
	if ((lower & I915_GT_GEN8_CTX_STATUS_PREEMPTED) != 0U)
		return 1;

	/* Anything else is the active context completing. */
	return 0;
}

/*
 * Reports the interrupts a Gen9 engine keeps unmasked in its I915_GT_RING_IMR:
 * the user interrupt, the context switch, the error and the semaphore wait,
 * shifted to the engine's place in its GT bank.
 */
static uint32_t
i915_execlists_engine_irqs(
	const struct i915_gt_engine *ge)
{
	uint32_t irqs;
	unsigned shift;

	/* The copy engine sits in the upper half of its bank; the second video engine is not used. */
	shift = 0U;
	if (ge->info->id == I915_BCS0)
		shift = I915_GT_GEN8_BCS_IRQ_SHIFT;

	/* The engine's user, context switch, error and semaphore interrupts. */
	irqs = I915_GT_GT_RENDER_USER_INTERRUPT;
	irqs |= I915_GT_GT_CONTEXT_SWITCH_INTERRUPT;
	irqs |= I915_GT_GT_CS_MASTER_ERROR_INTERRUPT;
	irqs |= I915_GT_GT_WAIT_SEMAPHORE_INTERRUPT;

	/* Reports them at the engine's shift. */
	return irqs << shift;
}

/* Gives the request's context the lowest free context id and fills the descriptor's upper dword. */
static int
i915_execlists_take_tag(
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

	/* Finds the lowest free id and takes it. */
	tag = 0;
	while (((el->context_tag >> tag) & 1U) == 0U)
		tag++;
	el->context_tag &= ~(((uint64_t)1) << tag);

	/*
	 * The id field holds 1 + tag, with the engine fields where the layout
	 * has them; Gen9 takes the same id with no engine fields.
	 */
	ccid = ((uint32_t)(1 + tag)) << (I915_GT_GEN11_SW_CTX_ID_SHIFT - 32);
	ccid |= ge->ccid;

	/* Puts the fields into the descriptor's upper dword and records the id. */
	ce = rq->ce;
	ce->desc = (((uint64_t)ccid) << 32) | (uint32_t)ce->desc;
	ce->tag = tag;

	/* Succeeded: the context is scheduled in. */
	return 0;
}

/* Gives the request's context id back. */
static void
i915_execlists_give_tag(
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
 * descriptor to submit.
 */
static uint64_t
i915_execlists_prepare_context(
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
	desc = ce->desc;
	previous = ce->ring.tail;
	tail = rq->tail;

	/* Moves the ring's tail to the request's. */
	ce->ring.tail = tail;

	/*
	 * The direction of the move within the ring: the sign of
	 * (next - previous) shifted up by 32 - log2(size).
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
		desc |= I915_GT_CTX_DESC_FORCE_RESTORE;

	/* Writes the tail into the image; the ring tail moves on past the workaround tail. */
	ce->regs[I915_GT_CTX_RING_TAIL] = tail;
	rq->tail = rq->wa_tail;

	/* The context image must be complete before the uncached ELSQ write. */
	kern_io_write_barrier();

	/* The forced restore applies to this submission only. */
	ce->desc &= ~(uint64_t)I915_GT_CTX_DESC_FORCE_RESTORE;

	/* Reports the descriptor to write. */
	return desc;
}

/* Writes one port: on the ELSQ lower dword first, then upper; on the ELSP upper first. */
static void
i915_execlists_write_port(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio,
	uint64_t desc,
	unsigned port)
{
	/* The ELSP of Gen9 takes every dword in one register, upper dword first. */
	if (ge->ctrl_reg == 0U) {
		drv_i915_write32(mmio, ge->submit_reg, (uint32_t)(desc >> 32));
		drv_i915_write32(mmio, ge->submit_reg, (uint32_t)desc);
		return;
	}

	/* Each port is two dwords from the queue's first register. */
	drv_i915_write32(mmio, ge->submit_reg + port * 8U, (uint32_t)desc);
	drv_i915_write32(mmio, ge->submit_reg + port * 8U + 4U, (uint32_t)(desc >> 32));
}

/*
 * Reads and consumes one CSB entry.
 *
 * The GPU does not always make an entry visible before the write pointer
 * (tgl:HSDES#22011248461): an all-ones entry is polled for 10 us and then
 * read from the MMIO copy of the buffer.  The entry is poisoned after reading so a reuse of its slot can
 * be spotted.
 */
static uint64_t
i915_execlists_read_csb(
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
			status = I915_GT_GEN8_EXECLISTS_STATUS_BUF;
			buffer_index = index;
			if (buffer_index >= I915_CSB_MMIO_FIRST_ENTRIES) {
				status = I915_GT_GEN11_EXECLISTS_STATUS_BUF2;
				buffer_index -= I915_CSB_MMIO_FIRST_ENTRIES;
			}

			/* Reads the entry over MMIO; register access is 32-bit, so the lower half is read first. */
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
