/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cache-control definitions: the MOCS registers of the command
 * streamer, the copy engine, the global table and the L3 cache control
 * pairs, the fields of one MOCS entry and of one L3 cache control
 * half-word, and the Gen12 private PAT and its memory types.  The rows of
 * the MOCS tables are in mocs-table.inc and mocs-table-gen9.inc.
 */

#ifndef DRIVERS_GPU_I915_INTEL_MOCS_H
#define DRIVERS_GPU_I915_INTEL_MOCS_H

#include <stdint.h>

#include "bits.h"

/* The MOCS registers. */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (BLIT_CCTL).
 */
#define I915_GT_BLIT_CCTL(base)	((base) + 0x204)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (BLIT_CCTL_DST_MOCS_MASK).
 */
#define I915_GT_BLIT_CCTL_DST_MOCS_MASK	I915_INC_GENMASK(14, 8)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (BLIT_CCTL_SRC_MOCS_MASK).
 */
#define I915_GT_BLIT_CCTL_SRC_MOCS_MASK	I915_INC_GENMASK(6, 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN12_GLOBAL_MOCS).
 */
#define I915_GT_GEN12_GLOBAL_MOCS(i)	(0x4000 + (i) * 4) /* Global MOCS regs */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN9_LNCFCMOCS).
 */
#define I915_GT_GEN9_LNCFCMOCS(i)	(0xb020 + (i) * 4) /* L3 Cache Control */

/* The MOCS and PAT registers and values. */

/*
 * engine_fake_wa_init: the command streamer's default MOCS.  Both fields
 * take a MOCS value, which is the table index shifted left by one.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_CMD_CCTL, CMD_CCTL_MOCS_MASK, CMD_CCTL_MOCS_OVERRIDE).
 */
#define I915_GT_RING_CMD_CCTL(base)			((base) + 0xc4U)
#define I915_GT_CMD_CCTL_MOCS_MASK			0x00003fffU
#define I915_GT_CMD_CCTL_MOCS_OVERRIDE(write, read) \
	(((((uint32_t)(write) << 1) << 7) & 0x3f80U) | (((uint32_t)(read) << 1) & 0x7fU))

/*
 * The Gen12 context MOCS: the copy engine's source and destination MOCS.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (BLIT_CCTL_MASK, BLIT_CCTL_MOCS).
 */
#define I915_GT_BLIT_CCTL_MASK				0x00007f7fU
#define I915_GT_BLIT_CCTL_MOCS(destination, source) \
	(((((uint32_t)(destination) << 1) << 8) & 0x7f00U) | (((uint32_t)(source) << 1) & 0x7fU))

/* intel_mocs.c: the global MOCS table and the L3 cache control pairs. */

/*
 * intel_mocs.c: one MOCS control value (XXX_MOCS_0 - XXX_MOCS_63).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_mocs.c
 * (_LE_CACHEABILITY, _LE_TGT_CACHE, LE_LRUM, LE_AOM, LE_RSC, LE_SCC, LE_SCF,
 * LE_SSE, LE_1_UC, LE_3_WB, LE_TC_1_LLC).
 */
#define I915_GT__LE_CACHEABILITY(value)			((uint32_t)(value) << 0)
#define I915_GT__LE_TGT_CACHE(value)			((uint32_t)(value) << 2)
#define I915_GT_LE_LRUM(value)				((uint32_t)(value) << 4)
#define I915_GT_LE_AOM(value)				((uint32_t)(value) << 6)
#define I915_GT_LE_RSC(value)				((uint32_t)(value) << 7)
#define I915_GT_LE_SCC(value)				((uint32_t)(value) << 8)
#define I915_GT_LE_SCF(value)				((uint32_t)(value) << 14)
#define I915_GT_LE_SSE(value)				((uint32_t)(value) << 17)
#define I915_GT_LE_1_UC					I915_GT__LE_CACHEABILITY(1)
#define I915_GT_LE_3_WB					I915_GT__LE_CACHEABILITY(3)
#define I915_GT_LE_TC_1_LLC				I915_GT__LE_TGT_CACHE(1)

/*
 * intel_mocs.c: one L3 cache control half-word (LNCFMOCS0 - LNCFMOCS31).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_mocs.c
 * (_L3_CACHEABILITY, L3_1_UC, L3_3_WB).
 */
#define I915_GT__L3_CACHEABILITY(value)			((uint16_t)((value) << 4))
#define I915_GT_L3_1_UC					I915_GT__L3_CACHEABILITY(1)
#define I915_GT_L3_3_WB					I915_GT__L3_CACHEABILITY(3)

/*
 * intel_mocs.c: I915_GT_GEN9_NUM_MOCS_ENTRIES, and the Gen12 table's special
 * indices.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_mocs.c
 * (GEN9_NUM_MOCS_ENTRIES).
 */
#define I915_GT_GEN9_NUM_MOCS_ENTRIES			64U
#define I915_GT_GEN12_MOCS_UC_INDEX			3U
#define I915_GT_GEN12_MOCS_UNUSED_ENTRIES_INDEX		2U

/*
 * intel_gtt.h: the Gen12 private PAT and its memory types.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN12_PAT_INDEX); drivers/gpu/drm/i915/gt/intel_gtt.h (GEN8_PPAT_WB,
 * GEN8_PPAT_WT, GEN8_PPAT_WC, GEN8_PPAT_UC).
 */
#define I915_GT_GEN12_PAT_INDEX(index)			(0x4800U + (unsigned)(index) * 4U)
#define I915_GT_GEN8_PPAT_WB				(3U << 0)
#define I915_GT_GEN8_PPAT_WT				(2U << 0)
#define I915_GT_GEN8_PPAT_WC				(1U << 0)
#define I915_GT_GEN8_PPAT_UC				(0U << 0)

/*
 * The Gen9 MOCS and PAT values.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_mocs.c
 * (LE_0_PAGETABLE, LE_TC_0_PAGETABLE, LE_TC_2_LLC_ELLC);
 * drivers/gpu/drm/i915/gt/intel_gt_regs.h (GEN8_PRIVATE_PAT_LO,
 * GEN8_PRIVATE_PAT_HI); drivers/gpu/drm/i915/gt/intel_gtt.h (GEN8_PPAT_AGE,
 * GEN8_PPAT_LLCELLC, GEN8_PPAT_LLC, GEN8_PPAT_ELLC_OVERRIDE).
 */
#define I915_GT_LE_0_PAGETABLE				I915_GT__LE_CACHEABILITY(0)
#define I915_GT_LE_TC_0_PAGETABLE			I915_GT__LE_TGT_CACHE(0)
#define I915_GT_LE_TC_2_LLC_ELLC			I915_GT__LE_TGT_CACHE(2)
#define I915_GT_GEN9_MOCS_UC_INDEX			0U
#define I915_GT_GEN9_MOCS_UNUSED_ENTRIES_INDEX		1U
#define I915_GT_GEN9_RCS0_MOCS0				0xc800U
#define I915_GT_GEN9_VCS0_MOCS0				0xc900U
#define I915_GT_GEN9_VCS1_MOCS0				0xca00U
#define I915_GT_GEN9_VECS0_MOCS0			0xcb00U
#define I915_GT_GEN9_BCS0_MOCS0				0xcc00U
#define I915_GT_GEN8_PRIVATE_PAT_LO			0x40e0U
#define I915_GT_GEN8_PRIVATE_PAT_HI			0x40e4U
#define I915_GT_GEN8_PPAT_AGE(x)			((uint32_t)(x) << 4)
#define I915_GT_GEN8_PPAT_LLCELLC			(2U << 2)
#define I915_GT_GEN8_PPAT_LLC				(1U << 2)
#define I915_GT_GEN8_PPAT_ELLC_OVERRIDE			(0U << 2)

#endif /* DRIVERS_GPU_I915_INTEL_MOCS_H */
