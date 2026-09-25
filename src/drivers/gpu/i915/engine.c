/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Engines: their setup, their kernel contexts, and the GT resume (see
 * engine.h).
 */

#include "engine.h"
#include "context.h"
#include "device-info.h"
#include "ggtt.h"
#include "gt-power.h"
#include "memory.h"
#include "mmio.h"
#include "reset.h"
#include "submit.h"
#include "sync.h"
#include "workarounds.h"
#include <kern/kcrt.h>

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>

#include "intel/gt-regs.h"

/* A masked register word that sets the named bits. */
#define I915_MASKED_ENABLE(bits)	((((uint32_t)(bits)) << 16) | ((uint32_t)(bits)))

/* How long the command streamer may take to go idle: the fast poll and the default stop timeout. */
#define I915_STOP_CS_FAST_US		1000U
#define I915_STOP_CS_TIMEOUT_MS		100U

/* How long GPM may take to complete the pending forcewakes. */
#define I915_MI_FW_COMPLETE_US		5000U

/* The ring size of a kernel context (SZ_4K). */
#define I915_KERNEL_RING_BYTES		4096U

static uint32_t i915_msg_idle_reg(int engine_id);
static void i915_execlists_sanitize(struct i915_gt_engines *es, unsigned index, struct i915_mmio *mmio);

/*
 * Sets up an engine's status page and software execlists state.
 *
 * This is init_status_page() and the software part of engine_setup_common():
 * one page, pinned in the GGTT and zeroed.  The reference maps the page
 * write-back and relies on LLC coherency; the DMA memory behind a GT object is
 * coherent for the same reason.  Returns 0, EINVAL, ENOMEM or the GGTT bind
 * error.
 */
int
drv_i915_engine_setup_common(
	struct i915_gt_engine *ge,
	struct i915_engine_info *info,
	struct i915_gt_mem *gm,
	const struct i915_sseu *sseu)
{
	int error;

	/* Refuses a setup without an engine, its information or a pool. */
	if (ge == NULL ||
	    info == NULL ||
	    gm == NULL)
		return EINVAL;

	/* Starts from an empty engine state bound to its information. */
	kern_memset(ge, 0, sizeof(*ge));
	ge->info = info;

	/* Allocates the status page. */
	ge->status_page = drv_i915_gt_object_create(gm, I915_GT_PAGE_BYTES);
	if (ge->status_page == NULL)
		return ENOMEM;

	/* Binds the status page into the GGTT; an unbound page is given back. */
	error = drv_i915_gt_ggtt_bind(gm, ge->status_page);
	if (error != 0) {
		drv_i915_gt_object_destroy(gm, ge->status_page);
		ge->status_page = NULL;
		return error;
	}

	/* Records the CPU view and the address the engine is given. */
	ge->hwsp = (volatile uint32_t *)ge->status_page->cpu;
	ge->hwsp_ggtt = ge->status_page->ggtt_offset;

	/* One port pair and nothing in flight (intel_engine_init_execlists()). */
	ge->port_mask = 1U;

	/* Uses the whole device by default. */
	if (sseu != NULL) {
		ge->sseu_slice_mask = sseu->slice_mask;
		ge->sseu_has_slice_pg = sseu->has_slice_pg;
	}

	/* Marks the engine ready for the execlists setup. */
	ge->setup_done = 1;

	/* Succeeded: the status page is allocated and bound. */
	return 0;
}

/*
 * Gives back an engine's status page and forgets its execlists pointers.
 */
void
drv_i915_engine_release(
	struct i915_gt_engine *ge,
	struct i915_gt_mem *gm)
{
	/* Nothing to release without an engine or a pool. */
	if (ge == NULL || gm == NULL)
		return;

	/* Destroys the status page. */
	if (ge->status_page != NULL) {
		drv_i915_gt_object_destroy(gm, ge->status_page);
		ge->status_page = NULL;
	}

	/* Forgets every view into the page and marks the engine unusable. */
	ge->hwsp = NULL;
	ge->csb_status = NULL;
	ge->csb_write = NULL;
	ge->setup_done = 0;
	ge->resumed = 0;
}

/*
 * Stops an engine's command streamer (intel_engine_stop_cs()).
 *
 * Sets STOP_RING, stops the prefetcher as well (Wa_22011802037, Gen11 to
 * before 12.70), and waits for MODE_IDLE.  MODE_IDLE sometimes stays clear on
 * an empty ring, so only a ring that still holds work is a timeout.  Returns
 * 0, EINVAL, EIO when the time base failed, or ETIMEDOUT.
 */
int
drv_i915_engine_stop_cs(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio)
{
	uint32_t base;
	uint32_t mode;
	uint32_t head;
	uint32_t tail;
	int error;

	/* Refuses a stop without an engine or register access. */
	if (ge == NULL || mmio == NULL)
		return EINVAL;

	/* Names the engine's MI_MODE register. */
	base = ge->info->mmio_base;
	mode = RING_MI_MODE(base);

	/* Asks the ring to stop. */
	drv_i915_write32(mmio, mode, I915_MASKED_ENABLE(STOP_RING));

	/* Stops the prefetcher too, so the streamer is really halted before a reset. */
	drv_i915_write32(mmio, RING_MODE_GEN7(base), I915_MASKED_ENABLE(GEN12_GFX_PREFETCH_DISABLE));

	/* Waits for MODE_IDLE (__intel_wait_for_register_fw(mode, MODE_IDLE, 1000 us, stop timeout)). */
	error = drv_i915_wait_reg(mmio, mode, MODE_IDLE, MODE_IDLE, I915_STOP_CS_FAST_US, I915_STOP_CS_TIMEOUT_MS, NULL);
	drv_i915_posting_read32(mmio, mode);

	/* A failed time base says nothing about the hardware. */
	if (error == EIO)
		return EIO;

	/* A timeout counts only when the ring still holds work. */
	if (error != 0) {
		head = drv_i915_read32(mmio, RING_HEAD(base)) & HEAD_ADDR;
		tail = drv_i915_read32(mmio, RING_TAIL(base)) & TAIL_ADDR;
		kern_logf("i915: %s stop_cs: MODE_IDLE timeout, head=%04x tail=%04x\n",
		    ge->info->name,
		    head,
		    tail);
		if (head != tail)
			return ETIMEDOUT;
	}

	/* Succeeded: the streamer is stopped or has nothing left to run. */
	return 0;
}

/*
 * Waits for the MI_FORCE_WAKEs an engine left pending (Wa_22011802037,
 * intel_engine_wait_for_pending_mi_fw()).
 *
 * The pending forcewakes are the bits set in both halves of the engine's
 * MSG_IDLE register.  GPM is given 1 us to see the stop, then up to 5 ms to
 * complete them, then the streamer is given 1 us to see the completion.
 */
void
drv_i915_engine_wait_for_pending_mi_fw(
	struct i915_gt_engine *ge,
	struct i915_mmio *mmio)
{
	uint32_t reg;
	uint32_t value;
	uint32_t pending;
	int error;

	/* Nothing to wait for without an engine or register access. */
	if (ge == NULL || mmio == NULL)
		return;

	/* An engine without a MSG_IDLE register has nothing pending. */
	reg = i915_msg_idle_reg(ge->info->id);
	if (reg == 0U)
		return;

	/* Reads which forcewakes are pending (__cs_pending_mi_force_wakes()). */
	value = drv_i915_read32(mmio, reg);
	pending = (value & (value >> 16) & MSG_IDLE_FW_MASK) >> MSG_IDLE_FW_SHIFT;
	ge->mi_fw_pending = pending;
	if (pending == 0U)
		return;

	/* Lets GPM see the stop. */
	(void)drv_i915_udelay(1U);

	/* Waits for GPM to complete the pending forcewakes (__gpm_wait_for_fw_complete()). */
	error = drv_i915_wait_reg(mmio, GEN9_PWRGT_DOMAIN_STATUS, pending, pending, I915_MI_FW_COMPLETE_US, 0U, NULL);
	if (error != 0) {
		kern_logf("i915: %s: pending forcewake 0x%x did not complete\n",
		    ge->info->name,
		    pending);
	}

	/* Lets the streamer see the completion. */
	(void)drv_i915_udelay(1U);
}

/*
 * Logs an engine's command-streamer state, its CSB and the software view of
 * it, as the hang report.
 */
void
drv_i915_engine_dump(
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_mmio *mmio,
	const char *why)
{
	uint32_t base;
	uint32_t head;
	uint32_t tail;
	uint32_t start;
	uint32_t ctl;
	uint32_t acthd_udw;
	uint32_t acthd;
	uint32_t ipehr;
	uint32_t instdone;
	uint32_t mi_mode;
	uint32_t esr;
	uint32_t eir;
	uint32_t status_hi;
	uint32_t status_lo;
	uint32_t csb_ptr;
	unsigned submits;
	unsigned promotes;
	unsigned completes;
	unsigned csb_errors;
	unsigned csb_late;
	unsigned csb_mmio_fallback;
	uint32_t last_csb_hi;
	uint32_t last_csb_lo;
	unsigned index;

	/* Nothing to dump without an engine or register access. */
	if (ge == NULL || mmio == NULL)
		return;

	/* Reads the ring registers in the order the report prints them. */
	base = ge->info->mmio_base;
	head = drv_i915_read32(mmio, RING_HEAD(base));
	tail = drv_i915_read32(mmio, RING_TAIL(base));
	start = drv_i915_read32(mmio, RING_START(base));
	ctl = drv_i915_read32(mmio, RING_CTL(base));
	acthd_udw = drv_i915_read32(mmio, RING_ACTHD_UDW(base));
	acthd = drv_i915_read32(mmio, RING_ACTHD(base));
	ipehr = drv_i915_read32(mmio, RING_IPEHR(base));
	instdone = drv_i915_read32(mmio, RING_INSTDONE(base));
	mi_mode = drv_i915_read32(mmio, RING_MI_MODE(base));
	esr = drv_i915_read32(mmio, RING_ESR(base));
	eir = drv_i915_read32(mmio, RING_EIR(base));
	kern_logf("i915: %s dump(%s): HEAD=%08x TAIL=%08x START=%08x CTL=%08x "
	    "ACTHD=%08x:%08x IPEHR=%08x INSTDONE=%08x MI_MODE=%08x ESR=%08x EIR=%08x\n",
	    ge->info->name,
	    why,
	    head,
	    tail,
	    start,
	    ctl,
	    acthd_udw,
	    acthd,
	    ipehr,
	    instdone,
	    mi_mode,
	    esr,
	    eir);

	/* Takes the software view of the CSB, or zeros without one. */
	submits = 0U;
	promotes = 0U;
	completes = 0U;
	csb_errors = 0U;
	csb_late = 0U;
	csb_mmio_fallback = 0U;
	last_csb_hi = 0U;
	last_csb_lo = 0U;
	if (el != NULL) {
		submits = el->submits;
		promotes = el->promotes;
		completes = el->completes;
		csb_errors = el->csb_errors;
		csb_late = el->csb_late;
		csb_mmio_fallback = el->csb_mmio_fallback;
		last_csb_hi = el->last_csb_hi;
		last_csb_lo = el->last_csb_lo;
	}

	/* Reads the execlist status and the CSB pointer, then reports them with the status page. */
	status_hi = drv_i915_read32(mmio, RING_EXECLIST_STATUS_HI(base));
	status_lo = drv_i915_read32(mmio, RING_EXECLIST_STATUS_LO(base));
	csb_ptr = drv_i915_read32(mmio, RING_CONTEXT_STATUS_PTR(base));
	kern_logf("i915: %s dump(%s): EXECLIST_STATUS=%08x:%08x CSB_PTR=%08x "
	    "hwsp: csb_write=%u preempt=%u seqno=%u | sw: csb_head=%u submits=%u "
	    "promotes=%u completes=%u errors=%u late=%u mmio=%u last=%08x:%08x\n",
	    ge->info->name,
	    why,
	    status_hi,
	    status_lo,
	    csb_ptr,
	    *ge->csb_write,
	    ge->hwsp[I915_GEM_HWS_PREEMPT],
	    ge->hwsp[I915_GEM_HWS_SEQNO_ADDR / 4U],
	    ge->csb_head,
	    submits,
	    promotes,
	    completes,
	    csb_errors,
	    csb_late,
	    csb_mmio_fallback,
	    last_csb_hi,
	    last_csb_lo);

	/* Reports every CSB entry, upper dword first. */
	for (index = 0U; index < ge->csb_size; index++) {
		kern_logf("i915: %s dump(%s): csb[%u]=%08x:%08x\n",
		    ge->info->name,
		    why,
		    index,
		    (uint32_t)(ge->csb_status[index] >> 32),
		    (uint32_t)ge->csb_status[index]);
	}
}

/*
 * Sets up every engine of the GT with its execlists state and pinned kernel
 * context (intel_engines_init()).
 *
 * The kernel context is what create_kernel_context() pins: a 4 KiB ring, and
 * pinning a never-initialised context runs lrc_init_state() and then
 * lrc_update_regs() on the empty ring.  Returns 0, EINVAL, or the first
 * engine's failure; on failure every engine is released.
 */
int
drv_i915_engines_init(
	struct i915_gt_engines *es,
	struct i915_gt_info *gt,
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *pp)
{
	struct i915_gt_engine *ge;
	struct i915_gt_context *ce;
	unsigned index;
	int error;

	/* Refuses an engine set without the GT, a pool or the kernel address space. */
	if (es == NULL ||
	    gt == NULL ||
	    gm == NULL ||
	    pp == NULL)
		return EINVAL;

	/* Starts from an empty engine set. */
	kern_memset(es, 0, sizeof(*es));

	/* Sets up each engine the GT reports, up to the engines the set can hold. */
	error = 0;
	for (index = 0U; index < gt->num_engines && index < (unsigned)I915_MAX_ENGINES; index++) {
		ge = &es->ge[index];
		ce = &es->kernel_ce[index];

		/* Creates the status page and the software execlists state. */
		error = drv_i915_engine_setup_common(ge, &gt->engines[index], gm, &gt->sseu);
		if (error != 0)
			break;

		/* Points the execlists state at the engine's registers and status page. */
		drv_i915_execlists_submission_setup(ge);
		drv_i915_execlists_init(&es->el[index]);

		/* Creates the pinned kernel context with its 4 KiB ring. */
		error = drv_i915_lrc_alloc(ce, ge, pp, gm, I915_KERNEL_RING_BYTES, 0U);
		if (error != 0)
			break;

		/* Pins it: builds the image and writes the empty ring into it. */
		drv_i915_lrc_init_state(ce);
		(void)drv_i915_lrc_update_regs(ce, ce->ring.tail);

		/* The kernel timeline starts at zero; the engine now counts as set up. */
		es->kernel_tl_seqno[index] = 0U;
		es->n = index + 1U;
	}

	/* A failed engine is released with every engine before it. */
	if (error != 0) {
		kern_logf("i915: intel_engines_init: engine %u failed rc=%d\n", index, error);
		es->n = index + 1U;
		drv_i915_engines_release(es, gm);
		return error;
	}

	/* Marks the set ready for the resume. */
	es->inited = 1;

	/* Succeeded: every engine has its status page and kernel context. */
	return 0;
}

/*
 * Gives back every engine's kernel context and status page, last engine
 * first.
 */
void
drv_i915_engines_release(
	struct i915_gt_engines *es,
	struct i915_gt_mem *gm)
{
	unsigned index;

	/* Nothing to release without an engine set or a pool. */
	if (es == NULL || gm == NULL)
		return;

	/* Releases each engine that was set up, in reverse order. */
	index = es->n;
	while (index > 0U) {
		index--;
		drv_i915_lrc_release(&es->kernel_ce[index], gm);
		drv_i915_engine_release(&es->ge[index], gm);
	}

	/* The set is empty again. */
	es->n = 0U;
	es->inited = 0;
}

/*
 * Resumes the GT in the reference order of intel_gt_resume().
 *
 *   gt_sanitize(gt, force):   per engine the reset preparation (pause,
 *                             stop the streamer, drain MI_FORCE_WAKEs) and the
 *                             sanitize (CSB pointers, status page, the pinned
 *                             kernel context); then a full GT reset, which
 *                             always happens because the reference evaluates
 *                             reset_engines(gt) before `|| force'; nothing is
 *                             in flight to rewind; then the RPS sanitize
 *   intel_rc6_sanitize
 *   intel_gt_init_hw          GT workarounds with their check, MOCS
 *   intel_rps_enable          (intel_llc_enable is not done)
 *   per engine                serial++, the engine workarounds and
 *                             whitelist, then execlists_resume: the render
 *                             engine's L3CC table again, the breadcrumbs
 *                             reset (software only), enable_execlists
 *   intel_rc6_enable
 *
 * A failed GT reset is logged and the resume continues, as the reference
 * does.  The caller holds every forcewake domain.  Returns 0 or EINVAL.
 */
int
drv_i915_gt_resume(
	struct i915_gt_engines *es,
	struct i915_gt_init *gi,
	const struct i915_gt_info *gt,
	struct i915_mmio *mmio,
	struct spinlock *uncore_lock)
{
	struct i915_gt_engine *ge;
	unsigned index;

	/* Refuses a resume of an engine set that was never set up. */
	if (es == NULL ||
	    es->inited == 0 ||
	    gi == NULL ||
	    gt == NULL ||
	    mmio == NULL)
		return EINVAL;

	/* Stops and sanitizes every engine (gt_sanitize()). */
	for (index = 0U; index < es->n; index++) {
		drv_i915_execlists_reset_prepare(&es->ge[index], mmio);
		if (es->ge[index].stop_cs_rc != 0)
			es->stop_cs_timeouts++;
		i915_execlists_sanitize(es, index, mmio);
	}

	/* Resets every engine (reset_engines(gt): __intel_gt_reset(gt, ALL_ENGINES)). */
	es->reset_rc = drv_i915_gt_reset_all(uncore_lock, mmio, I915_GT_RESET_ACK_US);
	if (es->reset_rc != 0) {
		kern_logf("i915: gt_sanitize: reset_engines rc=%d (continuing, as the reference does)\n",
		    es->reset_rc);
	}

	/*
	 * Nothing was ever submitted, so the rewind has nothing to unwind, and
	 * the reset finish re-enables a tasklet this driver does not have.
	 * Quiets the RPS interrupts.
	 */
	drv_i915_rps_sanitize(&gi->rps, mmio);

	/* Turns RC6 and power gating off (intel_rc6_sanitize()). */
	drv_i915_rc6_sanitize(&gi->rc6, mmio);

	/* Programs the GT workarounds and MOCS (intel_gt_init_hw()). */
	drv_i915_gt_init_hw_core(gi, gt, mmio);

	/* Enables RPS. */
	drv_i915_rps_enable(&gi->rps, mmio);

	/* Resumes every engine (intel_engine_resume()). */
	for (index = 0U; index < es->n; index++) {
		ge = &es->ge[index];

		/* The kernel context was lost, so the next park must switch to it again. */
		es->el[index].serial++;

		/* Applies the engine workarounds and the whitelist. */
		drv_i915_engine_apply_resume_wa(gi, gt, index, mmio);

		/*
		 * execlists_resume() starts with intel_mocs_init_engine(): global
		 * MOCS means no per-engine table, but the render engine's L3CC
		 * table is programmed again.
		 */
		if (ge->info->class == I915_RENDER_CLASS)
			drv_i915_init_l3cc_table(&gi->mocs, mmio, &es->l3cc_writes_rcs);

		/* Enables execlists; the breadcrumbs reset is software only. */
		drv_i915_execlists_enable(ge, mmio);
		es->resumed++;
	}

	/* Enables RC6. */
	drv_i915_gen11_rc6_enable(&gi->rc6, mmio, gt);

	/* Succeeded: the GT is resumed. */
	return 0;
}

/* Returns an engine's MSG_IDLE register, or zero for an engine without one. */
static uint32_t
i915_msg_idle_reg(
	int engine_id)
{
	/* Chooses the register by the engine. */
	switch (engine_id) {
	case I915_RCS0:
		return MSG_IDLE_CS;
	case I915_VCS0:
		return MSG_IDLE_VCS0;
	case I915_BCS0:
		return MSG_IDLE_BCS;
	case I915_VECS0:
		return MSG_IDLE_VECS0;
	case I915_VCS2:
		return MSG_IDLE_VCS2;
	default:
		break;
	}

	/* Any other engine has no MSG_IDLE register. */
	return 0U;
}

/* Sanitizes one engine after its reset preparation (execlists_sanitize()). */
static void
i915_execlists_sanitize(
	struct i915_gt_engines *es,
	unsigned index,
	struct i915_mmio *mmio)
{
	struct i915_gt_engine *ge;

	/* Resets the CSB pointers; the debug poisoning of the page is off. */
	ge = &es->ge[index];
	drv_i915_execlists_reset_csb_pointers(ge, mmio);

	/*
	 * The kernel context's timeline lives in the status page, which may
	 * have been lost, so its seqno is written back (sanitize_hwsp()).
	 */
	ge->hwsp[I915_GEM_HWS_SEQNO_ADDR / 4U] = es->kernel_tl_seqno[index];

	/* Resets the pinned kernel context (intel_engine_reset_pinned_contexts()). */
	drv_i915_lrc_reset(&es->kernel_ce[index]);
}
