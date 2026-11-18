/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT TLB invalidation (see tlb.h).
 *
 * Every engine class has an invalidation control register with one bit per
 * engine instance: writing the bit asks the engine to drop its TLB, and the
 * hardware clears the bit when the engine is done.  On graphics version 12
 * the video decode, video enhancement and compute registers are masked (the
 * bit is written with its enable in the upper half); the others take the
 * plain bit.  On Gen9 and Gen11 the registers are the Gen8 ones and are
 * not masked; Gen11's third video decode engine has its bit 2 in the first
 * video register.
 *
 * One invalidation covers every engine of the GT: all the forcewake domains
 * are held, every engine's bit is written under the uncore lock (so a GT
 * reset cannot run between the writes), Alder Lake-P's OA unit is
 * invalidated with the engines, and then each engine's bit is waited for.
 *
 * Every engine is invalidated, parked or not: a parked engine is a software
 * state that powers nothing down, so its TLB may still hold entries.  A
 * timeout is returned, so the caller keeps every page it meant to free, and
 * the generation advances only on success.  The engine registers of Alder
 * Lake-P are not multicast, so no steering is needed.
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
#include <stdbool.h>
#include <stddef.h>

/* The compute engine class, which has an invalidation register of its own. */
#define I915_TLB_COMPUTE_CLASS		5

/* How many engines one invalidation covers at most. */
#define I915_TLB_MAX_ENGINES		16U

/*
 * The OA unit's invalidation register, written with the engines'
 * (Wa_2207587034).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_perf_oa_regs.h
 * (GEN12_OA_TLB_INV_CR).
 */
#define I915_TLB_OA_CONTROL		0xceecU

/*
 * How long one engine's bit may take to clear: a busy stage, then a
 * sleeping one.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_tlb.c
 * (wait_for_invalidate).
 */
#define I915_TLB_BUSY_US		100U
#define I915_TLB_SLEEP_MS		4U

/* One engine class's invalidation register. */
struct i915_tlb_control {
	int class;
	uint32_t reg;
	bool masked;
};

/* What a GT generation invalidates with. */
struct i915_tlb_layout {
	const struct i915_tlb_control *controls;
	size_t control_count;
	int max_instance;
	const int *domains;
	unsigned domain_count;
	bool oa;
};

/*
 * The graphics version 12 registers.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN12_GFX_TLB_INV_CR, GEN12_VD_TLB_INV_CR, GEN12_VE_TLB_INV_CR,
 * GEN12_BLT_TLB_INV_CR, GEN12_COMPCTX_TLB_INV_CR) and
 * drivers/gpu/drm/i915/gt/intel_engine_cs.c
 * (intel_engine_init_tlb_invalidation, the masked classes).
 */
static const struct i915_tlb_control i915_tlb_controls_gen12[] = {
	{ I915_RENDER_CLASS,		0xced8U, false },
	{ I915_VIDEO_DECODE_CLASS,	0xcedcU, true },
	{ I915_VIDEO_ENHANCEMENT_CLASS,	0xcee0U, true },
	{ I915_COPY_ENGINE_CLASS,	0xcee4U, false },
	{ I915_TLB_COMPUTE_CLASS,	0xcf04U, true },
};

/*
 * The Gen9 registers.  The second video decode engine's register is not
 * listed because that engine is not used.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN8_RTCR, GEN8_M1TCR, GEN8_VTCR, GEN8_BTCR).
 */
static const struct i915_tlb_control i915_tlb_controls_gen9[] = {
	{ I915_RENDER_CLASS,		0x4260U, false },
	{ I915_VIDEO_DECODE_CLASS,	0x4264U, false },
	{ I915_VIDEO_ENHANCEMENT_CLASS,	0x4270U, false },
	{ I915_COPY_ENGINE_CLASS,	0x426cU, false },
};

/* The forcewake domains of each GT, taken in this order and released in the reverse order. */
static const int i915_tlb_domains_gen12[] = {
	I915_FORCEWAKE_RENDER,
	I915_FORCEWAKE_GT,
	I915_FORCEWAKE_MEDIA_VDBOX0,
	I915_FORCEWAKE_MEDIA_VDBOX2,
	I915_FORCEWAKE_MEDIA_VEBOX0
};
static const int i915_tlb_domains_gen9[] = {
	I915_FORCEWAKE_RENDER,
	I915_FORCEWAKE_GT,
	I915_FORCEWAKE_MEDIA_GEN9
};

/*
 * The layouts: Gen12 has 16 instances a class and the OA unit, Gen11 the
 * Gen8 registers with a bit an instance and the Gen12 media domains, Gen9
 * one instance a class.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_cs.c
 * (intel_engine_init_tlb_invalidation: graphics versions 8 to 11 take the
 * Gen8 registers, the bit of the instance).
 */
static const struct i915_tlb_layout i915_tlb_gen12 = {
	i915_tlb_controls_gen12, sizeof(i915_tlb_controls_gen12) / sizeof(i915_tlb_controls_gen12[0]), 15,
	i915_tlb_domains_gen12, sizeof(i915_tlb_domains_gen12) / sizeof(i915_tlb_domains_gen12[0]), true
};
static const struct i915_tlb_layout i915_tlb_gen11 = {
	i915_tlb_controls_gen9, sizeof(i915_tlb_controls_gen9) / sizeof(i915_tlb_controls_gen9[0]), 3,
	i915_tlb_domains_gen12, sizeof(i915_tlb_domains_gen12) / sizeof(i915_tlb_domains_gen12[0]), false
};
static const struct i915_tlb_layout i915_tlb_gen9 = {
	i915_tlb_controls_gen9, sizeof(i915_tlb_controls_gen9) / sizeof(i915_tlb_controls_gen9[0]), 0,
	i915_tlb_domains_gen9, sizeof(i915_tlb_domains_gen9) / sizeof(i915_tlb_domains_gen9[0]), false
};

static bool i915_tlb_control_of(const struct i915_tlb_layout *layout, int class, int instance, uint32_t *reg, uint32_t *request, uint32_t *done);
static void i915_tlb_report_timeout(struct i915_gt_tlb *tlb, uint32_t reg, uint32_t done);

/*
 * Invalidates every engine's TLB and waits for each to finish.
 *
 * Returns 0; EINVAL; ETIMEDOUT when an engine's bit did not clear in 4 ms;
 * or EIO when the time base or the forcewake failed.
 */
int
drv_i915_tlb_invalidate_all(
	struct i915_gt_tlb *tlb,
	struct i915_gt_engines *es,
	struct i915_mmio *m,
	struct spinlock *uncore_lock)
{
	const struct i915_tlb_layout *layout;
	uint32_t reg[I915_TLB_MAX_ENGINES];
	uint32_t done[I915_TLB_MAX_ENGINES];
	uint32_t request;
	unsigned long irq;
	unsigned count;
	unsigned held;
	unsigned i;
	int waited;
	int error;

	if (tlb == NULL || es == NULL || m == NULL || uncore_lock == NULL)
		return EINVAL;
	if (es->n > I915_TLB_MAX_ENGINES)
		return EINVAL;

	layout = &i915_tlb_gen12;
	if (es->n != 0U && es->ge[0].info->graphics_ver == 9)
		layout = &i915_tlb_gen9;
	if (es->n != 0U && es->ge[0].info->graphics_ver == 11)
		layout = &i915_tlb_gen11;

	/* Every domain must be up, or the writes would be dropped. */
	error = 0;
	held = 0U;
	while (held < layout->domain_count && drv_i915_forcewake_get(m, layout->domains[held]) == 0)
		held++;
	if (held != layout->domain_count) {
		tlb->fw_failures++;
		error = EIO;
		goto out;
	}

	/* The requests, with no reset in between. */
	count = 0U;
	irq = spin_lock_irqsave(uncore_lock);
	for (i = 0U; i < es->n; i++) {
		if (!i915_tlb_control_of(layout, es->ge[i].info->class, es->ge[i].info->instance, &reg[count], &request, &done[count]))
			continue;
		drv_i915_raw_write32(m, reg[count], request);
		count++;
	}
	if (count != 0U && layout->oa)
		drv_i915_raw_write32(m, I915_TLB_OA_CONTROL, 1U);
	spin_unlock_irqrestore(uncore_lock, irq);

	/* Each engine's bit clears when its TLB is clean; the first failure is the one returned. */
	for (i = 0U; i < count; i++) {
		waited = drv_i915_wait_reg(m, reg[i], done[i], 0U, I915_TLB_BUSY_US, I915_TLB_SLEEP_MS, NULL);
		if (waited == ETIMEDOUT) {
			tlb->timeouts++;
			i915_tlb_report_timeout(tlb, reg[i], done[i]);
			if (error == 0)
				error = ETIMEDOUT;
		} else if (waited != 0) {
			tlb->time_faults++;
			error = EIO;
		}
	}

	/* A completed invalidation starts a new TLB generation. */
	tlb->engines_invalidated = count;
	if (error == 0) {
		tlb->invalidations++;
		tlb->seqno += 2U;
	}

out:
	while (held > 0U) {
		held--;
		(void)drv_i915_forcewake_put(m, layout->domains[held]);
	}
	return error;
}

/*
 * Finds an engine's register, request word and done bit; false for a class
 * or an instance the layout has no bit for.
 */
static bool
i915_tlb_control_of(
	const struct i915_tlb_layout *layout,
	int class,
	int instance,
	uint32_t *reg,
	uint32_t *request,
	uint32_t *done)
{
	uint32_t bit;
	size_t i;

	if (instance < 0 || instance > layout->max_instance)
		return false;
	bit = 1U << (unsigned)instance;

	for (i = 0; i < layout->control_count; i++) {
		if (layout->controls[i].class != class)
			continue;
		*reg = layout->controls[i].reg;
		*done = bit;
		*request = layout->controls[i].masked ? (bit << 16) | bit : bit;
		return true;
	}
	return false;
}

/* Logs a timeout with who asked, so a test's intended fault is told from a hardware anomaly. */
static void
i915_tlb_report_timeout(
	struct i915_gt_tlb *tlb,
	uint32_t reg,
	uint32_t done)
{
	const char *backend;
	const char *test_id;

	backend = tlb->backend != NULL ? tlb->backend : "HW";
	test_id = tlb->test_id != NULL ? tlb->test_id : "-";
	kern_logf("i915: TLB invalidation did not complete in %ums (reg 0x%x done bit 0x%x still set) backend=%s test=%s expected_fault=%d\n",
		  I915_TLB_SLEEP_MS, reg, done, backend, test_id, tlb->expected_fault);
}
