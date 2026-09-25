/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT TLB invalidation (see tlb.h).
 *
 * The register numbers and the request and done encodings are Linux's
 * (gt/intel_gt_regs.h, i915_perf_oa_regs.h, intel_engine_cs.c
 * intel_engine_init_tlb_invalidation(), gt/intel_tlb.c
 * mmio_invalidate_full()).
 *
 * Where this differs from Linux, on purpose:
 *   - Linux skips engines that are not awake (intel_engine_pm_is_awake) and a
 *     GT that is asleep, because their TLBs do not survive power-down.  Here
 *     a parked engine is a software state that powers nothing down, so no
 *     engine is skipped.
 *   - Linux returns nothing and logs a timeout.  Here the error is returned,
 *     so the caller keeps every page (the release contract), and the seqno
 *     advances only on success.
 *   - No MCR lock is taken: the Alder Lake-P engine registers are not MCR.
 *   - The forcewake is returned at once; Linux uses a delayed put.
 *   - There is no invalidate_lock: the one owner of the GT serializes the
 *     callers.
 */

#include "i915.h"
#include "tlb.h"
#include "mmio.h"
#include "sync.h"
#include "device-info.h"
#include "engine.h"

#include <kern/klog.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The per-class TLB invalidation registers of Gen12. */
#define GEN12_GFX_TLB_INV_CR		0xced8U
#define GEN12_VD_TLB_INV_CR		0xcedcU
#define GEN12_VE_TLB_INV_CR		0xcee0U
#define GEN12_BLT_TLB_INV_CR		0xcee4U
#define GEN12_COMPCTX_TLB_INV_CR	0xcf04U

/* The OA unit's TLB invalidation register (Wa_2207587034). */
#define GEN12_OA_TLB_INV_CR		0xceecU

/* How long one engine's done bit may take to clear: a busy stage, then a sleeping one. */
#define I915_TLB_INVAL_TIMEOUT_US	100U
#define I915_TLB_INVAL_TIMEOUT_MS	4U

/* The compute engine class, which has an invalidation register of its own. */
#define I915_TLB_COMPUTE_CLASS		5

/* How many engines one invalidation covers at most. */
#define I915_TLB_MAX_ENGINES		16U

/* How many forcewake domains an invalidation wakes. */
#define I915_TLB_FORCEWAKE_DOMAINS	5U

/*
 * The forcewake domains FORCEWAKE_ALL stands for on this GT.
 *
 * They are taken in this order and released in the reverse order.  The
 * table is constant.
 */
static const int i915_tlb_forcewake_domains[I915_TLB_FORCEWAKE_DOMAINS] = {
	I915_FORCEWAKE_RENDER,
	I915_FORCEWAKE_GT,
	I915_FORCEWAKE_MEDIA_VDBOX0,
	I915_FORCEWAKE_MEDIA_VDBOX2,
	I915_FORCEWAKE_MEDIA_VEBOX0
};

static int i915_tlb_engine_register(int class, int instance, uint32_t *reg, uint32_t *request, uint32_t *done);

/*
 * Invalidates every engine's TLB and waits for each to finish.
 *
 * This is intel_gt_invalidate_tlb_full(): each engine's invalidation
 * register is written, then GEN12_OA_TLB_INV_CR (Wa_2207587034, Alder
 * Lake-P), then each engine's done bit is waited for until it reads back 0
 * (wait_for_invalidate(): mask = done, value = 0).  Every forcewake domain
 * is held for the writes, and the uncore lock serializes them with a
 * reset.  Returns 0; EINVAL; ETIMEDOUT when an engine's bit did not clear
 * in 4 ms; or EIO when the time base or the forcewake failed.
 */
int
drv_i915_gt_invalidate_tlb_full(
	struct i915_gt_tlb *tlb,
	struct i915_gt_engines *es,
	struct i915_mmio *m,
	struct spinlock *uncore_lock)
{
	uint32_t reg[I915_TLB_MAX_ENGINES];
	uint32_t done[I915_TLB_MAX_ENGINES];
	uint32_t request;
	const char *backend;
	const char *test_id;
	unsigned long irq;
	unsigned count;
	unsigned held;
	unsigned index;
	int wake_error;
	int known;
	int waited;
	int error;

	/* Refuses missing state and more engines than the invalidation tracks. */
	if (tlb == NULL)
		return EINVAL;
	if (es == NULL)
		return EINVAL;
	if (m == NULL)
		return EINVAL;
	if (uncore_lock == NULL)
		return EINVAL;
	if (es->n > I915_TLB_MAX_ENGINES)
		return EINVAL;

	/* Wakes every domain, stopping at the first one that does not come up (FORCEWAKE_ALL). */
	error = 0;
	held = 0U;
	while (held < I915_TLB_FORCEWAKE_DOMAINS) {
		wake_error = drv_i915_forcewake_get(m, i915_tlb_forcewake_domains[held]);
		if (wake_error != 0)
			break;
		held++;
	}

	/* A domain that stayed asleep would drop the writes; nothing is written. */
	if (held != I915_TLB_FORCEWAKE_DOMAINS) {
		tlb->fw_failures++;
		error = EIO;
		goto out;
	}

	/* Requests every engine's invalidation, serialized with a GT reset. */
	count = 0U;
	irq = spin_lock_irqsave(uncore_lock);

	for (index = 0U; index < es->n; index++) {
		/* An engine class without an invalidation register is skipped. */
		known = i915_tlb_engine_register(es->ge[index].info->class, es->ge[index].info->instance, &reg[count], &request, &done[count]);
		if (known != 0)
			continue;

		drv_i915_raw_write32(m, reg[count], request);
		count++;
	}

	/* Wa_2207587034:tgl,dg1,rkl,adl-s,adl-p: the OA unit's TLB goes with the engines'. */
	if (count != 0U)
		drv_i915_raw_write32(m, GEN12_OA_TLB_INV_CR, 1U);

	spin_unlock_irqrestore(uncore_lock, irq);

	/* Waits for every requested engine's done bit to clear. */
	for (index = 0U; index < count; index++) {
		waited = drv_i915_wait_reg(m, reg[index], done[index], 0U, I915_TLB_INVAL_TIMEOUT_US, I915_TLB_INVAL_TIMEOUT_MS, NULL);

		/* A timeout is logged with who asked; a time-base failure is counted apart. */
		if (waited == ETIMEDOUT) {
			tlb->timeouts++;

			/* Names the hardware when no backend is recorded. */
			backend = tlb->backend;
			if (backend == NULL)
				backend = "HW";

			/* Names no test when none is recorded. */
			test_id = tlb->test_id;
			if (test_id == NULL)
				test_id = "-";

			kern_logf("i915: TLB invalidation did not complete in %ums (reg 0x%x done bit 0x%x still set) backend=%s test=%s expected_fault=%d\n",
				  I915_TLB_INVAL_TIMEOUT_MS,
				  reg[index],
				  done[index],
				  backend,
				  test_id,
				  tlb->expected_fault);

			/* The first failure is the one reported. */
			if (error == 0)
				error = ETIMEDOUT;
		} else if (waited != 0) {
			tlb->time_faults++;
			error = EIO;
		}
	}

	/* A completed invalidation starts a new TLB generation (write_seqcount_invalidate). */
	tlb->engines_invalidated = count;
	if (error == 0) {
		tlb->invalidations++;
		tlb->seqno += 2U;
	}

out:
	/* Releases the domains in the reverse order they were taken. */
	while (held > 0U) {
		held--;
		(void)drv_i915_forcewake_put(m, i915_tlb_forcewake_domains[held]);
	}

	/* Reports why the TLBs may still hold the old translations. */
	if (error != 0)
		return error;

	/* Succeeded: no engine's TLB holds a translation older than this call. */
	return 0;
}

/* Reports an engine's invalidation register, its request word and its done bit (the Gen12 table). */
static int
i915_tlb_engine_register(
	int class,
	int instance,
	uint32_t *reg,
	uint32_t *request,
	uint32_t *done)
{
	uint32_t selected;
	uint32_t bit;

	/* Picks the class's invalidation register. */
	switch (class) {
	case I915_RENDER_CLASS:
		selected = GEN12_GFX_TLB_INV_CR;
		break;
	case I915_VIDEO_DECODE_CLASS:
		selected = GEN12_VD_TLB_INV_CR;
		break;
	case I915_VIDEO_ENHANCEMENT_CLASS:
		selected = GEN12_VE_TLB_INV_CR;
		break;
	case I915_COPY_ENGINE_CLASS:
		selected = GEN12_BLT_TLB_INV_CR;
		break;
	case I915_TLB_COMPUTE_CLASS:
		selected = GEN12_COMPCTX_TLB_INV_CR;
		break;
	default:
		return ERANGE;
	}

	/* Refuses an instance the register has no bit for. */
	if (instance < 0 || instance > 15)
		return ERANGE;

	/* Each instance has its own bit, which is both the request and the done bit. */
	bit = 1U << (unsigned)instance;
	*reg = selected;
	*done = bit;

	/* On graphics version 12 the video decode, video enhancement and compute registers are masked. */
	if (class == I915_VIDEO_DECODE_CLASS) {
		*request = (bit << 16) | bit;
	} else if (class == I915_VIDEO_ENHANCEMENT_CLASS) {
		*request = (bit << 16) | bit;
	} else if (class == I915_TLB_COMPUTE_CLASS) {
		*request = (bit << 16) | bit;
	} else {
		*request = bit;
	}

	/* Succeeded: the engine has an invalidation register. */
	return 0;
}
