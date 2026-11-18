/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GT power-management registers: RC6 control, state and thresholds,
 * coarse power gating, the power-management interrupt mask, the frequency
 * capabilities and request, and the PCODE command that reports the
 * efficient frequency.
 */

#ifndef DRIVERS_GPU_I915_INTEL_GT_POWER_H
#define DRIVERS_GPU_I915_INTEL_GT_POWER_H

#include <stdint.h>

/*
 * RC6 control, state and thresholds.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_RC_CONTROL, GEN6_RC_STATE, GEN6_RC6_WAKE_RATE_LIMIT,
 * GEN10_MEDIA_WAKE_RATE_LIMIT, GEN6_RC_EVALUATION_INTERVAL,
 * GEN6_RC_IDLE_HYSTERSIS, GEN6_RC_SLEEP, GEN6_RC6_THRESHOLD,
 * GEN6_RC_CTL_RC6_ENABLE); drivers/gpu/drm/i915/gt/uc/intel_guc_reg.h
 * (GUC_MAX_IDLE_COUNT); drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_MAX_IDLE).
 */
#define I915_GT_GEN6_RC_CONTROL				0xa090U
#define I915_GT_GEN6_RC_STATE				0xa094U
#define I915_GT_GEN6_RC6_WAKE_RATE_LIMIT		0xa09cU
#define I915_GT_GEN10_MEDIA_WAKE_RATE_LIMIT		0xa0a0U
#define I915_GT_GEN6_RC_EVALUATION_INTERVAL		0xa0a8U
#define I915_GT_GEN6_RC_IDLE_HYSTERSIS			0xa0acU
#define I915_GT_GEN6_RC_SLEEP				0xa0b0U
#define I915_GT_GEN6_RC6_THRESHOLD			0xa0b8U
#define I915_GT_GEN6_RC_CTL_RC6_ENABLE			(1U << 18)

/*
 * RC_CONTROL's hardware control of RC6 entry, and its evaluation mode:
 * the hardware itself enters RC6 when the GT idles past the thresholds.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_RC_CTL_HW_ENABLE, GEN6_RC_CTL_EI_MODE).
 */
#define I915_GT_RC_CTL_HW_CONTROL			(1U << 31)
#define I915_GT_RC_CTL_EVALUATION_MODE_1		(1U << 27)
#define I915_GT_GUC_MAX_IDLE_COUNT			0xc3e4U
#define I915_GT_RING_MAX_IDLE(base)			((base) + 0x54U)

/*
 * The GT's RC6 residency counter: the time the GT has spent in RC6, a
 * 32-bit count that wraps.  It lies in the GT forcewake domain.
 *
 * Value referenced: Intel Iris Xe Graphics PRM for DG1, Volume 13
 * (General Assets), the table of registers a non-privileged batch may
 * read (GFXREG_GT_GFX_RC6, 138108h).
 */
#define I915_GT_GEN6_GT_GFX_RC6				0x138108U

/*
 * Coarse power gating.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN9_MEDIA_PG_IDLE_HYSTERESIS, GEN9_RENDER_PG_IDLE_HYSTERESIS,
 * GEN9_PG_ENABLE, GEN9_RENDER_PG_ENABLE, GEN9_MEDIA_PG_ENABLE,
 * GEN11_MEDIA_SAMPLER_PG_ENABLE, VDN_HCP_POWERGATE_ENABLE,
 * VDN_MFX_POWERGATE_ENABLE).
 */
#define I915_GT_GEN9_MEDIA_PG_IDLE_HYSTERESIS		0xa0c4U
#define I915_GT_GEN9_RENDER_PG_IDLE_HYSTERESIS		0xa0c8U
#define I915_GT_GEN9_PG_ENABLE				0xa210U
#define I915_GT_GEN9_RENDER_PG_ENABLE			(1U << 0)
#define I915_GT_GEN9_MEDIA_PG_ENABLE			(1U << 1)
#define I915_GT_GEN11_MEDIA_SAMPLER_PG_ENABLE		(1U << 2)
#define I915_GT_VDN_HCP_POWERGATE_ENABLE(n)		(1U << (3U + 2U * (unsigned)(n)))
#define I915_GT_VDN_MFX_POWERGATE_ENABLE(n)		(1U << (4U + 2U * (unsigned)(n)))

/*
 * The power-management interrupt mask.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_PMINTRMSK).
 */
#define I915_GT_GEN6_PMINTRMSK				0xa168U

/*
 * The frequency capabilities, read through the MCHBAR mirror.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_mchbar_regs.h
 * (MCHBAR_MIRROR_BASE_SNB, GEN6_RP_STATE_CAP, GEN10_FREQ_INFO_REC, RPE_MASK);
 * drivers/gpu/drm/i915/gt/intel_rps.h (GEN9_FREQ_SCALER).
 */
#define I915_GT_MCHBAR_MIRROR_BASE_SNB			0x140000U
#define I915_GT_GEN6_RP_STATE_CAP			(I915_GT_MCHBAR_MIRROR_BASE_SNB + 0x5998U)
#define I915_GT_GEN10_FREQ_INFO_REC			(I915_GT_MCHBAR_MIRROR_BASE_SNB + 0x5ef0U)
#define I915_GT_RPE_MASK				0x0000ff00U
#define I915_GT_GEN9_FREQ_SCALER			3U

/*
 * The frequency request and the up/down hysteresis.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_RPNSWREQ, GEN9_FREQUENCY).
 */
#define I915_GT_GEN6_RPNSWREQ				0xa008U
#define I915_GT_GEN9_FREQUENCY(frequency)		((uint32_t)(frequency) << 23)

/*
 * The video frequency request, which Gen9 sets to RP1.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_RC_VIDEO_FREQ); drivers/gpu/drm/i915/gt/intel_rps.c
 * (gen9_rps_enable).
 */
#define I915_GT_GEN6_RC_VIDEO_FREQ			0xa00cU

/*
 * The Gen9 RP1 field of I915_GT_GEN6_RP_STATE_CAP
 * (__gen6_rps_get_freq_caps()).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_RP_IDLE_HYSTERSIS).
 */
#define I915_GT_GEN9_RP1_CAP_SHIFT			8U
#define I915_GT_GEN6_RP_IDLE_HYSTERSIS			0xa070U

/*
 * The frequency limits the interrupts are given between, and the RP control.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_RP_INTERRUPT_LIMITS, GEN6_RP_CONTROL, GEN6_RP_MEDIA_HW_NORMAL_MODE,
 * GEN6_RP_MEDIA_IS_GFX, GEN6_RP_ENABLE, GEN6_RP_UP_BUSY_AVG,
 * GEN6_RP_DOWN_IDLE_AVG).
 */
#define I915_GT_GEN6_RP_INTERRUPT_LIMITS		0xa014U
#define I915_GT_GEN6_RP_CONTROL				0xa024U
#define I915_GT_GEN6_RP_MEDIA_HW_NORMAL_MODE		(2U << 9)
#define I915_GT_GEN6_RP_MEDIA_IS_GFX			(1U << 8)
#define I915_GT_GEN6_RP_ENABLE				(1U << 7)
#define I915_GT_GEN6_RP_UP_BUSY_AVG			(0x2U << 3)
#define I915_GT_GEN6_RP_DOWN_IDLE_AVG			(0x2U << 0)

/*
 * The up and down busyness thresholds and their evaluation intervals.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_RP_UP_THRESHOLD, GEN6_RP_DOWN_THRESHOLD, GEN6_RP_UP_EI,
 * GEN6_RP_DOWN_EI).
 */
#define I915_GT_GEN6_RP_UP_THRESHOLD			0xa02cU
#define I915_GT_GEN6_RP_DOWN_THRESHOLD			0xa030U
#define I915_GT_GEN6_RP_UP_EI				0xa068U
#define I915_GT_GEN6_RP_DOWN_EI				0xa06cU

/*
 * The actual GT frequency (CAGF), in 16.67 MHz units, for the diagnostics.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN12_RPSTAT1, GEN12_CAGF_MASK).
 */
#define I915_GT_GEN12_RPSTAT1				0x1381b4U
#define I915_GT_GEN12_CAGF_SHIFT			11U
#define I915_GT_GEN12_CAGF_MASK				(0x1ffU << 11)

/*
 * The PCODE mailbox command that reports the efficient frequency.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL).
 */
#define I915_GT_HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL	0x1aU

/* How many video decode engines the power-gating mask can name. */
#define I915_MAX_VCS				8U

#endif /* DRIVERS_GPU_I915_INTEL_GT_POWER_H */
