/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Engine workaround verification by the GPU itself (see verify-workarounds.h).
 *
 * The requests go through the execution layer: the kernel context and
 * timeline of each engine, the ring, and the execlists submission.
 */

#include "i915.h"
#include "verify-workarounds.h"
#include "workarounds.h"
#include "device-info.h"
#include "engine.h"
#include "request.h"
#include "submit.h"
#include "memory.h"
#include "ggtt.h"
#include "sync.h"
#include <kern/kcrt.h>

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "intel/commands.h"
#include "intel/gt-regs.h"

/* The size of the page each engine stores its registers into. */
#define I915_VWA_SCRATCH_BYTES	4096U

/* How long one completion poll step waits, in microseconds. */
#define I915_VWA_POLL_STEP_US	50U

/* How many poll steps make one millisecond. */
#define I915_VWA_POLL_STEPS_PER_MS	20U

/*
 * One multicast register range, both ends inclusive.
 */
struct i915_mcr_range {
	uint32_t start;
	uint32_t end;
};

/*
 * The Gen12 multicast register ranges (Linux mcr_ranges_gen12).
 *
 * A command streamer read inside one of them reaches an unsteered instance,
 * so those entries are not verified by the GPU.  The table never changes.
 */
static const struct i915_mcr_range i915_mcr_ranges_gen12[] = {
#include "intel/mcr-ranges.inc"
};

static int i915_verify_wa_fail(struct i915_gt_verify_wa *verify, int error, const char *where);
static int i915_verify_wa_retired(struct i915_gt_request *rq, struct i915_execlists *execlists);
static int i915_verify_wa_wait_engine(struct i915_gt_verify_wa *verify, unsigned index, struct i915_gt_engines *engines, struct i915_mmio *mmio, int want, unsigned timeout_ms);

/*
 * Reports whether a register lies in a Gen12 multicast range.
 *
 * Linux mcr_range() for graphics version 12 below 12.50.
 */
int
drv_i915_gen12_mcr_range(
	uint32_t offset)
{
	unsigned count;
	unsigned index;

	/* Looks for a range that contains the register. */
	count = (unsigned)(sizeof(i915_mcr_ranges_gen12) / sizeof(i915_mcr_ranges_gen12[0]));
	for (index = 0U; index < count; index++) {
		/* A register below the range is not in it. */
		if (offset < i915_mcr_ranges_gen12[index].start)
			continue;

		/* Nor is a register above it. */
		if (offset > i915_mcr_ranges_gen12[index].end)
			continue;

		/* Succeeded: the register is multicast. */
		return 1;
	}

	/* Succeeded: the register is not multicast. */
	return 0;
}

/*
 * Emits the register stores of one workaround list into a request.
 *
 * Linux wa_list_srm().  Each entry outside the multicast ranges is stored to
 * scratch + 4 * its list index, so the skipped entries leave gaps.  The
 * emitted and skipped counts are reported before the ring is reserved.
 */
int
drv_i915_wa_list_srm(
	struct i915_gt_request *rq,
	const struct i915_wa_list *wal,
	uint32_t scratch_ggtt,
	unsigned *emitted,
	unsigned *skipped)
{
	uint32_t *cs;
	uint32_t command;
	uint32_t offset;
	unsigned count;
	unsigned index;
	int multicast;

	/* Refuses a missing request or list. */
	if (rq == NULL)
		return EINVAL;
	if (wal == NULL)
		return EINVAL;

	/* Counts the entries the command streamer can read. */
	count = 0U;
	for (index = 0U; index < wal->count; index++) {
		multicast = drv_i915_gen12_mcr_range(wal->list[index].reg);
		if (multicast == 0)
			count++;
	}

	/* Reports how the list splits between stored and skipped entries. */
	if (emitted != NULL)
		*emitted = count;
	if (skipped != NULL)
		*skipped = wal->count - count;

	/* Reserves four dwords per store. */
	cs = drv_i915_ring_begin(rq, 4U * count);
	if (cs == NULL) {
		/* Reports the request's own error when it has one. */
		if (rq->error != 0)
			return rq->error;

		return ENOSPC;
	}

	/* Stores each readable register into its slot of the scratch page. */
	command = MI_STORE_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT;
	for (index = 0U; index < wal->count; index++) {
		offset = wal->list[index].reg;

		/* A multicast register is not read by the command streamer. */
		multicast = drv_i915_gen12_mcr_range(offset);
		if (multicast != 0)
			continue;

		*cs++ = command;
		*cs++ = offset;
		*cs++ = scratch_ggtt + 4U * index;
		*cs++ = 0U;
	}

	/* Closes the reservation. */
	drv_i915_ring_advance(rq, cs);

	/* Succeeded: the stores are in the ring. */
	return 0;
}

/*
 * Compares the stored registers of one engine with its workaround list.
 *
 * Linux wa_verify() over the results page.  Reports ENXIO when any
 * workaround was lost.
 */
int
drv_i915_wa_list_check(
	struct i915_gt_verify_wa *verify,
	unsigned index,
	const struct i915_wa_list *wal,
	const char *from)
{
	const volatile uint32_t *results;
	const struct i915_wa *entry;
	const char *list_name;
	const char *entry_name;
	uint32_t current;
	unsigned entry_index;
	int multicast;
	int error;

	/* Refuses a missing verification or list. */
	if (verify == NULL)
		return EINVAL;
	if (wal == NULL)
		return EINVAL;

	/* Refuses an engine that has no results page. */
	if (index >= (unsigned)I915_MAX_ENGINES)
		return EINVAL;
	if (verify->scratch[index] == NULL)
		return EINVAL;

	/* Starts the engine's counts from zero. */
	results = (const volatile uint32_t *)verify->scratch[index]->cpu;
	verify->verified[index] = 0U;
	verify->mismatched[index] = 0U;
	verify->not_verifiable[index] = 0U;

	/* Compares every stored entry with the list. */
	error = 0;
	for (entry_index = 0U; entry_index < wal->count; entry_index++) {
		entry = &wal->list[entry_index];

		/* A multicast register was not stored. */
		multicast = drv_i915_gen12_mcr_range(entry->reg);
		if (multicast != 0)
			continue;

		/* Takes the value the command streamer stored for this entry. */
		current = results[entry_index];

		/* A read mask of 0 compares nothing, so the entry always passes. */
		if (entry->read_mask == 0U) {
			verify->not_verifiable[index]++;
			continue;
		}

		/* Reports a workaround the hardware did not keep. */
		if (((current ^ entry->set) & entry->read_mask) != 0U) {
			/* Names the list and the entry, with placeholders when unnamed. */
			list_name = "?";
			if (wal->name != NULL)
				list_name = wal->name;
			entry_name = "";
			if (entry->name != NULL)
				entry_name = entry->name;

			kern_logf("i915: %s workaround lost on %s! (reg[%x]=0x%x, relevant bits were 0x%x vs expected 0x%x) %s\n",
			    list_name,
			    from,
			    entry->reg,
			    current,
			    current & entry->read_mask,
			    entry->set & entry->read_mask,
			    entry_name);

			/* Counts the lost workaround and fails the engine. */
			verify->mismatched[index]++;
			error = ENXIO;
		} else {
			verify->verified[index]++;
		}
	}

	/* Reports that a workaround was lost. */
	if (error != 0)
		return error;

	/* Succeeded: every stored workaround is in place. */
	return 0;
}

/*
 * Submits the register stores of one engine.
 *
 * Linux engine_wa_list_verify() up to i915_request_add(): the scratch page,
 * the request on the kernel context, the stores, and the submission.  An
 * engine with an empty list submits nothing and stays parked.
 */
int
drv_i915_engine_verify_wa_submit(
	struct i915_gt_verify_wa *verify,
	unsigned index,
	struct i915_gt_engines *engines,
	const struct i915_wa_list *wal,
	struct i915_gt_mem *gt_mem,
	struct i915_mmio *mmio)
{
	struct i915_gt_engine *engine;
	struct i915_gt_request *rq;
	uint32_t kernel_seqno;
	int error;

	/* Refuses a missing argument. */
	if (verify == NULL ||
	    engines == NULL ||
	    wal == NULL ||
	    gt_mem == NULL ||
	    mmio == NULL)
		return EINVAL;

	/* Refuses an engine the GT does not have. */
	if (index >= engines->n)
		return EINVAL;
	if (index >= (unsigned)I915_MAX_ENGINES)
		return EINVAL;

	/* Records the engine's list size and clears its earlier error. */
	engine = &engines->ge[index];
	rq = &verify->rq[index];
	verify->list_count[index] = wal->count;
	verify->engine_err[index] = 0;

	/* Extends the verification to cover this engine. */
	if (index + 1U > verify->n)
		verify->n = index + 1U;

	/* An empty list submits nothing, and the engine stays parked. */
	if (wal->count == 0U) {
		verify->state[index] = I915_VWA_IDLE;
		return 0;
	}

	/* Allocates the scratch page the registers are stored into. */
	verify->scratch[index] = drv_i915_gt_object_create(gt_mem, I915_VWA_SCRATCH_BYTES);
	if (verify->scratch[index] == NULL)
		return i915_verify_wa_fail(verify, ENOMEM, "__vm_create_scratch_for_read");

	kern_memset(verify->scratch[index]->cpu, 0, I915_VWA_SCRATCH_BYTES);

	/* Binds the scratch page into the GGTT, where the stores address it. */
	error = drv_i915_gt_ggtt_bind(gt_mem, verify->scratch[index]);
	if (error != 0)
		return i915_verify_wa_fail(verify, error, "i915_vma_pin_ww");

	/*
	 * Creates a request on the kernel context, whose timeline is the next
	 * seqno of the engine's pinned kernel timeline (intel_engine_pm_get,
	 * i915_request_create).
	 */
	engines->kernel_tl_seqno[index]++;
	kernel_seqno = engines->kernel_tl_seqno[index];
	error = drv_i915_request_create(
		rq,
		&engines->kernel_ce[index],
		kernel_seqno,
		(uint32_t)engine->hwsp_ggtt + I915_GEM_HWS_SEQNO_ADDR,
		&engine->hwsp[I915_GEM_HWS_SEQNO_ADDR / 4U]);
	if (error != 0)
		return i915_verify_wa_fail(verify, error, "i915_request_create");

	/*
	 * Emits the stores.  i915_vma_move_to_active() is fence bookkeeping
	 * only and has nothing to do here.
	 */
	error = drv_i915_wa_list_srm(rq, wal, (uint32_t)verify->scratch[index]->ggtt_offset, &verify->emitted[index], &verify->mcr_skipped[index]);
	if (error != 0)
		return i915_verify_wa_fail(verify, error, "wa_list_srm");

	/* Closes the request with its breadcrumb. */
	error = drv_i915_request_add(rq);
	if (error != 0)
		return i915_verify_wa_fail(verify, error, "i915_request_add");

	/* Submits the request to the idle engine. */
	error = drv_i915_execlists_submit(engine, &engines->el[index], mmio, rq);
	if (error != 0)
		return i915_verify_wa_fail(verify, error, "execlists_submit");

	/* Succeeded: the stores are in flight. */
	verify->state[index] = I915_VWA_SRM;
	return 0;
}

/*
 * Processes the engine's completion events once.
 *
 * Reports nonzero while the engine still has a verification request or a
 * park switch in flight.
 */
int
drv_i915_engine_verify_wa_poll(
	struct i915_gt_verify_wa *verify,
	unsigned index,
	struct i915_gt_engines *engines,
	struct i915_mmio *mmio)
{
	struct i915_execlists *execlists;
	int retired;

	/* An engine the verification did not reach is never busy. */
	if (verify == NULL)
		return 0;
	if (engines == NULL)
		return 0;
	if (index >= verify->n)
		return 0;

	/* Applies whatever the engine has reported since the last poll. */
	execlists = &engines->el[index];
	verify->polls++;
	(void)drv_i915_execlists_process_csb(&engines->ge[index], execlists, mmio);

	/* Records an event the submission could not apply. */
	if (execlists->csb_errors != 0U)
		(void)i915_verify_wa_fail(verify, EIO, "execlists CSB error");

	/* Advances the engine's state once its request has retired. */
	switch (verify->state[index]) {
	case I915_VWA_SRM:
		/* The stores are complete once their request retires. */
		retired = i915_verify_wa_retired(&verify->rq[index], execlists);
		if (retired != 0)
			verify->state[index] = I915_VWA_DONE;
		break;
	case I915_VWA_SWITCH:
		/*
		 * The engine is parked once the switch retires and nothing ran
		 * since it was submitted.
		 */
		retired = i915_verify_wa_retired(&verify->krq[index], execlists);
		if (retired != 0 && execlists->wakeref_serial == execlists->serial)
			verify->state[index] = I915_VWA_PARKED;
		break;
	default:
		break;
	}

	/* Reports an engine that still has a request in flight as busy. */
	if (verify->state[index] == I915_VWA_SRM)
		return 1;
	if (verify->state[index] == I915_VWA_SWITCH)
		return 1;

	/* Succeeded: the engine is not busy. */
	return 0;
}

/*
 * Switches one engine back to its kernel context after its verification.
 *
 * Linux intel_engine_pm_put() -> switch_to_kernel_context().  An engine on
 * which nothing ran since the last switch is already there.
 */
int
drv_i915_engine_verify_wa_park(
	struct i915_gt_verify_wa *verify,
	unsigned index,
	struct i915_gt_engines *engines,
	struct i915_mmio *mmio)
{
	struct i915_gt_engine *engine;
	struct i915_execlists *execlists;
	struct i915_gt_request *krq;
	uint32_t kernel_seqno;
	int error;

	/* Refuses an engine whose stores have not completed. */
	if (verify == NULL)
		return EINVAL;
	if (engines == NULL)
		return EINVAL;
	if (index >= verify->n)
		return EINVAL;
	if (verify->state[index] != I915_VWA_DONE)
		return EINVAL;

	/* Resolves the engine, its submission state and its switch request. */
	engine = &engines->ge[index];
	execlists = &engines->el[index];
	krq = &verify->krq[index];

	/* An engine on which nothing ran since the last switch is already parked. */
	if (execlists->wakeref_serial == execlists->serial) {
		verify->state[index] = I915_VWA_PARKED;
		return 0;
	}

	/*
	 * Creates the switch request on the kernel context and closes it; no
	 * active barriers are pending, so i915_request_add_active_barriers()
	 * adds nothing.
	 */
	engines->kernel_tl_seqno[index]++;
	kernel_seqno = engines->kernel_tl_seqno[index];
	error = drv_i915_request_create(
		krq,
		&engines->kernel_ce[index],
		kernel_seqno,
		(uint32_t)engine->hwsp_ggtt + I915_GEM_HWS_SEQNO_ADDR,
		&engine->hwsp[I915_GEM_HWS_SEQNO_ADDR / 4U]);
	if (error == 0)
		error = drv_i915_request_add(krq);

	/*
	 * Moves the wakeref serial past the switch: "check again on the next
	 * retirement".  It moves even when the request could not be built.
	 */
	execlists->wakeref_serial = execlists->serial + 1U;

	/* Submits the switch when it was built. */
	if (error == 0)
		error = drv_i915_execlists_submit(engine, execlists, mmio, krq);

	/* Reports a switch that could not be built or submitted. */
	if (error != 0)
		return i915_verify_wa_fail(verify, error, "switch_to_kernel_context");

	/* Succeeded: the switch is in flight. */
	verify->state[index] = I915_VWA_SWITCH;
	return 0;
}

/*
 * Verifies the workarounds of every engine with the GPU.
 *
 * Linux __engines_verify_workarounds(): for each engine, store the listed
 * registers, wait, compare and park; then wait for every park switch.
 * Reports the first error recorded, or zero.
 */
int
drv_i915_engines_verify_workarounds(
	struct i915_gt_verify_wa *verify,
	struct i915_gt_engines *engines,
	struct i915_gt_init *gt_init,
	struct i915_gt_mem *gt_mem,
	struct i915_mmio *mmio,
	unsigned timeout_ms)
{
	const struct i915_wa_list *wal;
	const char *where;
	unsigned index;
	int error;

	/* Refuses a missing argument. */
	if (verify == NULL ||
	    engines == NULL ||
	    gt_init == NULL ||
	    gt_mem == NULL ||
	    mmio == NULL)
		return EINVAL;

	/* Starts from a verification that has reached no engine. */
	kern_memset(verify, 0, sizeof(*verify));

	/* Verifies the engines one at a time, as intel_engine_verify_workarounds(engine, "load"). */
	for (index = 0U; index < engines->n && index < (unsigned)I915_WA_ENGINES; index++) {
		wal = &gt_init->engine_wa[index];

		/* Submits the stores; a failure fails the engine and moves on. */
		error = drv_i915_engine_verify_wa_submit(verify, index, engines, wal, gt_mem, mmio);
		if (error != 0) {
			verify->engine_err[index] = error;
			(void)i915_verify_wa_fail(verify, EIO, verify->err_where);
			continue;
		}

		/* An engine with an empty list has nothing to wait for. */
		if (verify->state[index] == I915_VWA_IDLE)
			continue;

		/* Waits for the stores, as i915_request_wait(rq, 0, HZ / 5). */
		error = i915_verify_wa_wait_engine(verify, index, engines, mmio, I915_VWA_DONE, timeout_ms);
		if (error != 0) {
			verify->engine_err[index] = error;
			if (error == ETIMEDOUT) {
				/* The request did not complete in time. */
				verify->timed_out = 1;
				(void)i915_verify_wa_fail(verify, EIO, "i915_request_wait -ETIME");
			} else {
				/* The wait itself failed. */
				(void)i915_verify_wa_fail(verify, EIO, "i915_request_wait");
			}

			/* No park switch is queued behind a hung request. */
			continue;
		}

		/* Compares the stored values with the list. */
		error = drv_i915_wa_list_check(verify, index, wal, "load");
		if (error != 0) {
			verify->engine_err[index] = error;
			(void)i915_verify_wa_fail(verify, EIO, "wa_verify");
		}

		/* Parks the engine by switching back to its kernel context. */
		error = drv_i915_engine_verify_wa_park(verify, index, engines, mmio);
		if (error != 0) {
			verify->engine_err[index] = error;
			(void)i915_verify_wa_fail(verify, EIO, "switch_to_kernel_context");
		}
	}

	/* Waits for every park switch: "flush and restore the kernel context for safety". */
	for (index = 0U; index < verify->n; index++) {
		/* Only an engine with a switch in flight is waited for. */
		if (verify->state[index] != I915_VWA_SWITCH)
			continue;

		/* Waits for the engine to park, as intel_gt_wait_for_idle() does. */
		error = i915_verify_wa_wait_engine(verify, index, engines, mmio, I915_VWA_PARKED, timeout_ms);
		if (error != 0) {
			/* Keeps an earlier engine error in preference to this one. */
			if (verify->engine_err[index] == 0)
				verify->engine_err[index] = error;

			/* Records the wait failure, naming a timeout as such. */
			where = "intel_gt_wait_for_idle";
			if (error == ETIMEDOUT) {
				verify->timed_out = 1;
				where = "intel_gt_wait_for_idle -ETIME";
			}

			(void)i915_verify_wa_fail(verify, EIO, where);
		}
	}

	/* Reports the first error any engine recorded. */
	if (verify->err != 0)
		return verify->err;

	/* Succeeded: every engine kept its workarounds and parked. */
	return 0;
}

/*
 * Releases the scratch pages of a verification.
 */
void
drv_i915_engines_verify_wa_release(
	struct i915_gt_verify_wa *verify,
	struct i915_gt_mem *gt_mem)
{
	unsigned index;

	/* Nothing can be released without both. */
	if (verify == NULL)
		return;
	if (gt_mem == NULL)
		return;

	/* Destroys every scratch page the verification allocated. */
	for (index = 0U; index < (unsigned)I915_MAX_ENGINES; index++) {
		/* An engine that never got a page has nothing to release. */
		if (verify->scratch[index] == NULL)
			continue;

		drv_i915_gt_object_destroy(gt_mem, verify->scratch[index]);
		verify->scratch[index] = NULL;
	}

	/* The verification no longer covers any engine. */
	verify->n = 0U;
}

/* Records the first failure of a verification and passes the error back. */
static int
i915_verify_wa_fail(
	struct i915_gt_verify_wa *verify,
	int error,
	const char *where)
{
	/* Keeps only the first failure and the step that produced it. */
	if (verify->err == 0) {
		verify->err = error;
		verify->err_where = where;
	}

	/* Reports the error the caller is failing with. */
	return error;
}

/* Reports whether a request has landed and the engine has gone idle after it. */
static int
i915_verify_wa_retired(
	struct i915_gt_request *rq,
	struct i915_execlists *execlists)
{
	int completed;

	/* The breadcrumb has not landed yet. */
	completed = drv_i915_request_completed(rq);
	if (completed == 0)
		return 0;

	/* The engine still reports a context running. */
	if (execlists->have_active != 0)
		return 0;

	/* A submission is still waiting for the engine to take it. */
	if (execlists->pending[0] != NULL)
		return 0;

	/* Succeeded: the request has retired. */
	return 1;
}

/*
 * Polls one engine until it reaches a state.
 *
 * Reports zero, ETIMEDOUT, EIO, or the verification's first error when that
 * is not EIO.
 */
static int
i915_verify_wa_wait_engine(
	struct i915_gt_verify_wa *verify,
	unsigned index,
	struct i915_gt_engines *engines,
	struct i915_mmio *mmio,
	int want,
	unsigned timeout_ms)
{
	unsigned budget;
	unsigned step;
	int delay_error;

	/* Polls every 50 microseconds for the whole timeout. */
	budget = timeout_ms * I915_VWA_POLL_STEPS_PER_MS;
	for (step = 0U; step < budget; step++) {
		(void)drv_i915_engine_verify_wa_poll(verify, index, engines, mmio);

		/* The engine reached the state. */
		if (verify->state[index] == want)
			return 0;

		/*
		 * Stops on an error that is already recorded.
		 *
		 * XXX: this includes a non-EIO error recorded by an earlier engine
		 * (for example an allocation failure), which then fails this
		 * engine's wait on its first poll.  Kept as it was.
		 */
		if (verify->err != 0 && verify->err != EIO)
			return verify->err;

		/* The submission saw an event it could not apply. */
		if (engines->el[index].csb_errors != 0U)
			return EIO;

		/* Waits one step; a faulted time base ends the wait. */
		delay_error = drv_i915_udelay(I915_VWA_POLL_STEP_US);
		if (delay_error != 0)
			return i915_verify_wa_fail(verify, EIO, "time base");
	}

	/* The engine did not reach the state in time. */
	return ETIMEDOUT;
}
