/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Recording each engine's default context image (see defaults.h).
 */

#include "defaults.h"
#include "context.h"
#include "engine.h"
#include "ggtt.h"
#include "memory.h"
#include "reset.h"
#include "request.h"
#include "submit.h"
#include "sync.h"
#include "workarounds.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>

#include "intel/gt-regs.h"

/* The ring size of a record context and the size of its timeline page (SZ_4K). */
#define I915_DEFAULTS_RING_BYTES	4096U
#define I915_DEFAULTS_TIMELINE_BYTES	4096U

/* A timeline with an initial breadcrumb moves its seqno by 2 per request. */
#define I915_DEFAULTS_SEQNO_STEP	2U

/* The interval between two polls of the wait, and how many fit in one millisecond. */
#define I915_DEFAULTS_POLL_US		50U
#define I915_DEFAULTS_POLLS_PER_MS	20U

static void i915_defaults_fail(struct i915_gt_defaults *d, int error, const char *where);
static int i915_defaults_submit(struct i915_gt_defaults *d, struct i915_gt_engines *es, struct i915_gt_init *gi, struct i915_gt_mem *gm, struct i915_gt_ppgtt *pp, struct i915_mmio *mmio);
static int i915_defaults_submit_engine(struct i915_gt_defaults *d, struct i915_gt_engines *es, struct i915_gt_init *gi, struct i915_gt_mem *gm, struct i915_gt_ppgtt *pp, struct i915_mmio *mmio, unsigned index);
static int i915_defaults_retired(struct i915_gt_request *rq, struct i915_execlists *el);
static unsigned i915_defaults_poll(struct i915_gt_defaults *d, struct i915_gt_engines *es, struct i915_mmio *mmio);
static void i915_defaults_advance_record(struct i915_gt_defaults *d, struct i915_gt_engines *es, struct i915_mmio *mmio, unsigned index);
static void i915_defaults_advance_switch(struct i915_gt_defaults *d, struct i915_gt_engines *es, unsigned index);
static int i915_defaults_switch_to_kernel(struct i915_gt_engines *es, struct i915_gt_defaults *d, struct i915_mmio *mmio, unsigned index);
static int i915_defaults_finish(struct i915_gt_defaults *d, struct i915_gt_engines *es, struct i915_gt_mem *gm);
static void i915_defaults_release_contexts(struct i915_gt_defaults *d, struct i915_gt_mem *gm);

/*
 * Records every engine's default context image (__engines_record_defaults()).
 *
 * Submits a record request per engine, waits at least timeout_ms for every
 * engine to park, and copies the saved images into d->default_state.  On any
 * failure the engines are dumped, stopped and reset (the reference wedges the
 * GT).  The record contexts are released either way.  Returns 0 or the first
 * failure, whose step is in d->err_where.
 */
int
drv_i915_engines_record_defaults(
	struct i915_gt_defaults *d,
	struct i915_gt_engines *es,
	struct i915_gt_init *gi,
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *pp,
	struct i915_mmio *mmio,
	struct spinlock *uncore_lock,
	unsigned timeout_ms)
{
	unsigned budget;
	unsigned busy;
	unsigned poll;
	unsigned index;
	int error;

	/* Submits the record request of every engine; a failure is kept in d->err. */
	(void)i915_defaults_submit(d, es, gi, gm, pp, mmio);

	/* Waits for every engine to park (intel_gt_wait_for_idle(gt, I915_GEM_IDLE_TIMEOUT)). */
	if (d->err == 0) {
		budget = timeout_ms * I915_DEFAULTS_POLLS_PER_MS;
		busy = 1U;
		for (poll = 0U; poll < budget; poll++) {
			/* Advances every engine and stops once all are parked or one failed. */
			busy = i915_defaults_poll(d, es, mmio);
			if (busy == 0U || d->err != 0)
				break;

			/* Lets 50 us pass; a failed time base ends the wait. */
			error = drv_i915_udelay(I915_DEFAULTS_POLL_US);
			if (error != 0) {
				i915_defaults_fail(d, EIO, "time base");
				break;
			}
		}

		/* An engine still busy after the budget is a timeout (-ETIME). */
		if (busy != 0U && d->err == 0) {
			d->timed_out = 1;
			i915_defaults_fail(d, EIO, "intel_gt_wait_for_idle -ETIME");
		}
	}

	/* Copies the saved images once every engine parked. */
	if (d->err == 0)
		(void)i915_defaults_finish(d, es, gm);

	/*
	 * Wedges the GT on a failure: "the quickest way we can accomplish [idle
	 * engines ready for teardown] is by declaring ourselves wedged", which
	 * stops the engines and resets them.
	 */
	if (d->err != 0) {
		/* Dumps every engine, naming the failed step. */
		for (index = 0U; index < es->n; index++)
			drv_i915_engine_dump(&es->ge[index], &es->el[index], mmio, d->err_where);

		/* Stops every engine. */
		for (index = 0U; index < es->n; index++)
			drv_i915_execlists_reset_prepare(&es->ge[index], mmio);

		/* Resets the GT. */
		(void)drv_i915_gt_reset_all(uncore_lock, mmio, I915_GT_RESET_ACK_US);
		d->wedged = 1;
	}

	/* Puts the requests and their contexts. */
	i915_defaults_release_contexts(d, gm);

	/* Reports the first failure. */
	if (d->err != 0)
		return d->err;

	/* Succeeded: every engine has a default state in d->default_state. */
	return 0;
}

/*
 * Gives back the record contexts and the default-state copies.
 */
void
drv_i915_engines_defaults_release(
	struct i915_gt_defaults *d,
	struct i915_gt_mem *gm)
{
	unsigned index;

	/* Nothing to release without the recording or a pool. */
	if (d == NULL || gm == NULL)
		return;

	/* Releases what the recording itself still holds. */
	i915_defaults_release_contexts(d, gm);

	/* Destroys every default-state copy. */
	for (index = 0U; index < I915_MAX_ENGINES; index++) {
		if (d->default_state[index] != NULL) {
			drv_i915_gt_object_destroy(gm, d->default_state[index]);
			d->default_state[index] = NULL;
		}
	}

	/* No engine has a record context any more. */
	d->n = 0U;
}

/* Records the first failure and the step it came from; a later one is not recorded. */
static void
i915_defaults_fail(
	struct i915_gt_defaults *d,
	int error,
	const char *where)
{
	/* Only the first failure names the step. */
	if (d->err == 0) {
		d->err = error;
		d->err_where = where;
	}
}

/* Creates each engine's record context and submits its record request. */
static int
i915_defaults_submit(
	struct i915_gt_defaults *d,
	struct i915_gt_engines *es,
	struct i915_gt_init *gi,
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *pp,
	struct i915_mmio *mmio)
{
	unsigned index;
	int error;

	/* Refuses a recording without its engines, tables, pool, address space or register access. */
	if (d == NULL ||
	    es == NULL ||
	    gi == NULL ||
	    gm == NULL ||
	    pp == NULL ||
	    mmio == NULL)
		return EINVAL;

	/* Starts from an empty recording. */
	kern_memset(d, 0, sizeof(*d));

	/* Submits the record request of every engine; the first failure stops the pass. */
	for (index = 0U; index < es->n; index++) {
		error = i915_defaults_submit_engine(d, es, gi, gm, pp, mmio, index);
		if (error != 0)
			return error;
	}

	/* Succeeded: every engine has its record request in flight. */
	return 0;
}

/* Creates one engine's record context, writes its record request and submits it. */
static int
i915_defaults_submit_engine(
	struct i915_gt_defaults *d,
	struct i915_gt_engines *es,
	struct i915_gt_init *gi,
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *pp,
	struct i915_mmio *mmio,
	unsigned index)
{
	struct i915_gt_engine *ge;
	struct i915_gt_context *ce;
	struct i915_gt_request *rq;
	int error;

	/* Names the engine, its record context and its record request. */
	ge = &es->ge[index];
	ce = &d->ce[index];
	rq = &d->rq[index];

	/* Creates the record context with a 4 KiB ring (intel_context_create(engine)). */
	error = drv_i915_lrc_alloc(ce, ge, pp, gm, I915_DEFAULTS_RING_BYTES, 0U);
	if (error != 0) {
		i915_defaults_fail(d, error, "intel_context_create");
		return error;
	}

	/* The engine now has a record context to release. */
	d->n = index + 1U;

	/* Allocates the timeline's own status page (intel_timeline_create(): hwsp_alloc()). */
	d->tl_page[index] = drv_i915_gt_object_create(gm, I915_DEFAULTS_TIMELINE_BYTES);
	if (d->tl_page[index] == NULL) {
		i915_defaults_fail(d, ENOMEM, "intel_timeline_create");
		return ENOMEM;
	}

	/* Pins the timeline page into the GGTT. */
	error = drv_i915_gt_ggtt_bind(gm, d->tl_page[index]);
	if (error != 0) {
		i915_defaults_fail(d, error, "intel_timeline_pin");
		return error;
	}

	/* The new timeline starts at zero. */
	d->tl_seqno[index] = 0U;

	/*
	 * Pins the context (intel_renderstate_init()): never initialised, so the
	 * image is built with the restore inhibited and no default state yet,
	 * and the empty ring is written into it.
	 */
	drv_i915_lrc_init_state(ce);
	(void)drv_i915_lrc_update_regs(ce, ce->ring.tail);

	/* Opens the request; the timeline's initial breadcrumb moves the seqno by 2. */
	d->tl_seqno[index] += I915_DEFAULTS_SEQNO_STEP;
	error = drv_i915_request_create(rq,
					ce,
					d->tl_seqno[index],
					(uint32_t)d->tl_page[index]->ggtt_offset,
					(volatile uint32_t *)d->tl_page[index]->cpu);
	if (error != 0) {
		i915_defaults_fail(d, error, "i915_request_create");
		return error;
	}

	/* Writes the engine's context workarounds. */
	error = drv_i915_emit_ctx_wa(rq, &gi->ctx_wa[index], mmio);
	if (error != 0) {
		i915_defaults_fail(d, error, "intel_engine_emit_ctx_wa");
		return error;
	}

	/* intel_renderstate_emit() writes nothing on Gen12.  Closes the request. */
	error = drv_i915_request_add(rq);
	if (error != 0) {
		i915_defaults_fail(d, error, "i915_request_add");
		return error;
	}

	/* Submits it. */
	error = drv_i915_execlists_submit(ge, &es->el[index], mmio, rq);
	if (error != 0) {
		i915_defaults_fail(d, error, "execlists_submit");
		return error;
	}

	/* The record request is in flight. */
	d->state[index] = I915_DEF_RECORD;

	/* Succeeded: the engine is recording. */
	return 0;
}

/* Reports nonzero once a request has landed and the engine has reported its context complete. */
static int
i915_defaults_retired(
	struct i915_gt_request *rq,
	struct i915_execlists *el)
{
	int completed;

	/* The breadcrumb must have landed. */
	completed = drv_i915_request_completed(rq);
	if (completed == 0)
		return 0;

	/* The engine must have switched the context out. */
	if (el->have_active != 0)
		return 0;
	if (el->pending[0] != NULL)
		return 0;

	/* The request is retired. */
	return 1;
}

/* Advances every engine one step and reports how many are not parked yet. */
static unsigned
i915_defaults_poll(
	struct i915_gt_defaults *d,
	struct i915_gt_engines *es,
	struct i915_mmio *mmio)
{
	struct i915_gt_engine *ge;
	struct i915_execlists *el;
	unsigned busy;
	unsigned index;

	/* Nothing to poll without the recording or the engines. */
	if (d == NULL || es == NULL)
		return 0U;

	/* Counts the pass for the report. */
	d->polls++;

	/* Advances each engine that has a record context. */
	busy = 0U;
	for (index = 0U; index < d->n; index++) {
		ge = &es->ge[index];
		el = &es->el[index];

		/* Applies what the engine reported; any CSB error fails the recording. */
		(void)drv_i915_execlists_process_csb(ge, el, mmio);
		if (el->csb_errors != 0U)
			i915_defaults_fail(d, EIO, "execlists CSB error");

		/* Moves the engine on by its progress. */
		switch (d->state[index]) {
		case I915_DEF_RECORD:
			i915_defaults_advance_record(d, es, mmio, index);
			break;
		case I915_DEF_SWITCH:
			i915_defaults_advance_switch(d, es, index);
			break;
		default:
			break;
		}

		/* An engine with a request in flight is still busy. */
		if (d->state[index] == I915_DEF_RECORD || d->state[index] == I915_DEF_SWITCH)
			busy++;
	}

	/* Reports how many engines are not parked. */
	return busy;
}

/* Parks an engine whose record request retired, switching to the kernel context when needed. */
static void
i915_defaults_advance_record(
	struct i915_gt_defaults *d,
	struct i915_gt_engines *es,
	struct i915_mmio *mmio,
	unsigned index)
{
	struct i915_execlists *el;
	int retired;
	int error;

	/* Waits for the record request to retire. */
	el = &es->el[index];
	retired = i915_defaults_retired(&d->rq[index], el);
	if (retired == 0)
		return;

	/*
	 * The engine parks.  It is already in the kernel context only if no
	 * request ran since the last park -- not the case here, because the
	 * submission moved the serial.
	 */
	if (el->wakeref_serial == el->serial) {
		d->state[index] = I915_DEF_PARKED;
		return;
	}

	/* Switches to the kernel context (switch_to_kernel_context()). */
	error = i915_defaults_switch_to_kernel(es, d, mmio, index);
	if (error != 0) {
		i915_defaults_fail(d, error, "switch_to_kernel_context");
		return;
	}

	/* The park switch is in flight. */
	d->state[index] = I915_DEF_SWITCH;
}

/* Parks an engine once its switch to the kernel context retired and no request ran since. */
static void
i915_defaults_advance_switch(
	struct i915_gt_defaults *d,
	struct i915_gt_engines *es,
	unsigned index)
{
	struct i915_execlists *el;
	int retired;

	/* Waits for the switch request to retire. */
	el = &es->el[index];
	retired = i915_defaults_retired(&d->krq[index], el);
	if (retired == 0)
		return;

	/* A submission since the switch keeps the engine awake. */
	if (el->wakeref_serial != el->serial)
		return;

	/* The engine is idle and the record image written back. */
	d->state[index] = I915_DEF_PARKED;
}

/* Writes and submits a request on the engine's pinned kernel context. */
static int
i915_defaults_switch_to_kernel(
	struct i915_gt_engines *es,
	struct i915_gt_defaults *d,
	struct i915_mmio *mmio,
	unsigned index)
{
	struct i915_gt_engine *ge;
	struct i915_execlists *el;
	struct i915_gt_request *krq;
	uint32_t seqno;
	int error;

	/* Names the engine, its execlists state and the switch request. */
	ge = &es->ge[index];
	el = &es->el[index];
	krq = &d->krq[index];

	/* Takes the kernel timeline's next seqno; its slot is in the engine's status page. */
	es->kernel_tl_seqno[index]++;
	seqno = es->kernel_tl_seqno[index];

	/* Writes the request; there are no active barriers pending to add. */
	error = drv_i915_request_create(krq,
					&es->kernel_ce[index],
					seqno,
					(uint32_t)ge->hwsp_ggtt + I915_GEM_HWS_SEQNO_ADDR,
					&ge->hwsp[I915_GEM_HWS_SEQNO_ADDR / 4U]);
	if (error == 0)
		error = drv_i915_request_add(krq);

	/*
	 * "Check again on the next retirement": the engine counts as parked
	 * once no submission has moved the serial past this one.
	 */
	el->wakeref_serial = el->serial + 1U;

	/* Reports a request that could not be written. */
	if (error != 0)
		return error;

	/* Submits the switch. */
	error = drv_i915_execlists_submit(ge, el, mmio, krq);
	if (error != 0)
		return error;

	/* Succeeded: the switch to the kernel context is in flight. */
	return 0;
}

/* Fails on any CSB error, otherwise copies every record context's whole image. */
static int
i915_defaults_finish(
	struct i915_gt_defaults *d,
	struct i915_gt_engines *es,
	struct i915_gt_mem *gm)
{
	struct i915_gt_object *copy;
	unsigned index;

	/* Refuses a finish without the recording, the engines or a pool. */
	if (d == NULL ||
	    es == NULL ||
	    gm == NULL)
		return EINVAL;

	/* An engine that reported a CSB error failed its request (rq->fence.error). */
	for (index = 0U; index < d->n; index++) {
		if (es->el[index].csb_errors != 0U) {
			i915_defaults_fail(d, EIO, "rq->fence.error");
			return EIO;
		}
	}

	/* Copies each saved image whole (shmem_create_from_object(rq->context->state->obj)). */
	for (index = 0U; index < d->n; index++) {
		copy = drv_i915_gt_object_create(gm, d->ce[index].state_bytes);
		if (copy == NULL) {
			i915_defaults_fail(d, ENOMEM, "shmem_create_from_object");
			return ENOMEM;
		}

		/* Copies the whole image and hands the copy to the engine's slot. */
		kern_memcpy(copy->cpu, d->ce[index].state->cpu, d->ce[index].state_bytes);
		d->default_state[index] = copy;
	}

	/* Succeeded: every engine has its default state. */
	return 0;
}

/* Gives back each record context and its timeline page, last engine first. */
static void
i915_defaults_release_contexts(
	struct i915_gt_defaults *d,
	struct i915_gt_mem *gm)
{
	unsigned index;

	/* Releases each engine's timeline page and context in reverse order. */
	index = d->n;
	while (index > 0U) {
		index--;
		if (d->tl_page[index] != NULL) {
			drv_i915_gt_object_destroy(gm, d->tl_page[index]);
			d->tl_page[index] = NULL;
		}

		/* Releases the context image and its ring. */
		drv_i915_lrc_release(&d->ce[index], gm);
	}
}
