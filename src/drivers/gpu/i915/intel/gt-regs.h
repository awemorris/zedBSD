/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GT registers and fields: the engine classes, the PCI BARs and the
 * GMCH control word, forcewake, reset, the ring, execlist and
 * context-control registers of an engine, the GT interrupt registers, the
 * hardware status page slots, the logical ring context layout and
 * descriptor fields, and the GTT page-table entry bits.  The MOCS registers
 * (I915_GT_BLIT_CCTL and its fields, I915_GT_GEN12_GLOBAL_MOCS,
 * I915_GT_GEN9_LNCFCMOCS) are in mocs.h, the commands in commands.h.
 */

#ifndef DRIVERS_GPU_I915_INTEL_GT_REGS_H
#define DRIVERS_GPU_I915_INTEL_GT_REGS_H

#include "bits.h"

/* The GT, engine, interrupt, context and GTT registers. */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_types.h
 * (RENDER_CLASS).
 */
#define I915_GT_RENDER_CLASS	0

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_types.h
 * (COPY_ENGINE_CLASS).
 */
#define I915_GT_COPY_ENGINE_CLASS	3

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_types.h
 * (VIDEO_DECODE_CLASS).
 */
#define I915_GT_VIDEO_DECODE_CLASS	1

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_types.h
 * (VIDEO_ENHANCEMENT_CLASS).
 */
#define I915_GT_VIDEO_ENHANCEMENT_CLASS	2

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_types.h
 * (OTHER_CLASS).
 */
#define I915_GT_OTHER_CLASS	4

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/intel_pci_config.h
 * (GEN4_GTTMMADR_BAR).
 */
#define I915_GT_GEN4_GTTMMADR_BAR	0 /* MMIO+GTT */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/intel_pci_config.h
 * (GEN4_GMADR_BAR).
 */
#define I915_GT_GEN4_GMADR_BAR	2

/*
 * Values referenced: Linux v6.19 include/drm/intel/i915_drm.h (SNB_GMCH_CTRL).
 */
#define I915_GT_SNB_GMCH_CTRL	0x50

/*
 * Values referenced: Linux v6.19 include/drm/intel/i915_drm.h
 * (BDW_GMCH_GGMS_SHIFT).
 */
#define I915_GT_BDW_GMCH_GGMS_SHIFT	6

/*
 * Values referenced: Linux v6.19 include/drm/intel/i915_drm.h
 * (BDW_GMCH_GGMS_MASK).
 */
#define I915_GT_BDW_GMCH_GGMS_MASK	0x3

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (FORCEWAKE_GT_GEN9).
 */
#define I915_GT_FORCEWAKE_GT_GEN9	(0xa188)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (FORCEWAKE_ACK_GT_GEN9).
 */
#define I915_GT_FORCEWAKE_ACK_GT_GEN9	(0x130044)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (FORCEWAKE_RENDER_GEN9).
 */
#define I915_GT_FORCEWAKE_RENDER_GEN9	(0xa278)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (FORCEWAKE_ACK_RENDER_GEN9).
 */
#define I915_GT_FORCEWAKE_ACK_RENDER_GEN9	(0xd84)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (FORCEWAKE_MEDIA_VDBOX_GEN11).
 */
#define I915_GT_FORCEWAKE_MEDIA_VDBOX_GEN11(n)	(0xa540 + (n) * 4)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (FORCEWAKE_ACK_MEDIA_VDBOX_GEN11).
 */
#define I915_GT_FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(n)	(0xd50 + (n) * 4)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (FORCEWAKE_MEDIA_VEBOX_GEN11).
 */
#define I915_GT_FORCEWAKE_MEDIA_VEBOX_GEN11(n)	(0xa560 + (n) * 4)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (FORCEWAKE_ACK_MEDIA_VEBOX_GEN11).
 */
#define I915_GT_FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(n)	(0xd70 + (n) * 4)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (FORCEWAKE_KERNEL).
 */
#define I915_GT_FORCEWAKE_KERNEL	I915_INC_BIT(0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GFX_FLSH_CNTL_GEN6).
 */
#define I915_GT_GFX_FLSH_CNTL_GEN6	(0x101008)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GFX_FLSH_CNTL_EN).
 */
#define I915_GT_GFX_FLSH_CNTL_EN	(1U << 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_GDRST).
 */
#define I915_GT_GEN6_GDRST	(0x941c)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_GRDOM_FULL).
 */
#define I915_GT_GEN6_GRDOM_FULL	(1U << 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_GRDOM_RENDER).
 */
#define I915_GT_GEN6_GRDOM_RENDER	(1U << 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_GRDOM_RENDER).
 */
#define I915_GT_GEN11_GRDOM_RENDER	I915_GT_GEN6_GRDOM_RENDER

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_GRDOM_BLT).
 */
#define I915_GT_GEN11_GRDOM_BLT	I915_INC_BIT(2)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_RESET_CTL).
 */
#define I915_GT_RING_RESET_CTL(base)	((base) + 0xd0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RESET_CTL_REQUEST_RESET).
 */
#define I915_GT_RESET_CTL_REQUEST_RESET	I915_INC_BIT(0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RESET_CTL_READY_TO_RESET).
 */
#define I915_GT_RESET_CTL_READY_TO_RESET	I915_INC_BIT(1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RESET_CTL_CAT_ERROR).
 */
#define I915_GT_RESET_CTL_CAT_ERROR	I915_INC_BIT(2)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (RENDER_RING_BASE).
 */
#define I915_GT_RENDER_RING_BASE	0x02000

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (BLT_RING_BASE).
 */
#define I915_GT_BLT_RING_BASE	0x22000

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_TAIL).
 */
#define I915_GT_RING_TAIL(base)	((base) + 0x30)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_HEAD).
 */
#define I915_GT_RING_HEAD(base)	((base) + 0x34)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (HEAD_ADDR).
 */
#define I915_GT_HEAD_ADDR	0x001FFFFC

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (TAIL_ADDR).
 */
#define I915_GT_TAIL_ADDR	0x001FFFF8

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_START).
 */
#define I915_GT_RING_START(base)	((base) + 0x38)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_CTL).
 */
#define I915_GT_RING_CTL(base)	((base) + 0x3c)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_CTL_SIZE).
 */
#define I915_GT_RING_CTL_SIZE(size)	((size) - I915_INC_PAGE_SIZE) /* in bytes -> pages */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_VALID).
 */
#define I915_GT_RING_VALID	0x00000001

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_HWS_PGA).
 */
#define I915_GT_RING_HWS_PGA(base)	((base) + 0x80)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_HWSTAM).
 */
#define I915_GT_RING_HWSTAM(base)	((base) + 0x98)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_MI_MODE).
 */
#define I915_GT_RING_MI_MODE(base)	((base) + 0x9c)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (STOP_RING).
 */
#define I915_GT_STOP_RING	I915_INC_BIT(8)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (MODE_IDLE).
 */
#define I915_GT_MODE_IDLE	I915_INC_BIT(9)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_IMR).
 */
#define I915_GT_RING_IMR(base)	((base) + 0xa8)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_EIR).
 */
#define I915_GT_RING_EIR(base)	((base) + 0xb0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_EMR).
 */
#define I915_GT_RING_EMR(base)	((base) + 0xb4)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_ESR).
 */
#define I915_GT_RING_ESR(base)	((base) + 0xb8)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_ACTHD).
 */
#define I915_GT_RING_ACTHD(base)	((base) + 0x74)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_ACTHD_UDW).
 */
#define I915_GT_RING_ACTHD_UDW(base)	((base) + 0x5c)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_BBADDR).
 */
#define I915_GT_RING_BBADDR(base)	((base) + 0x140)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_BBADDR_UDW).
 */
#define I915_GT_RING_BBADDR_UDW(base)	((base) + 0x168) /* gen8+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_IPEIR).
 */
#define I915_GT_RING_IPEIR(base)	((base) + 0x64)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_IPEHR).
 */
#define I915_GT_RING_IPEHR(base)	((base) + 0x68)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_INSTDONE).
 */
#define I915_GT_RING_INSTDONE(base)	((base) + 0x6c)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_CONTEXT_CONTROL).
 */
#define I915_GT_RING_CONTEXT_CONTROL(base)	((base) + 0x244)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (CTX_CTRL_INHIBIT_SYN_CTX_SWITCH).
 */
#define I915_GT_CTX_CTRL_INHIBIT_SYN_CTX_SWITCH	I915_INC_BIT(3)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT).
 */
#define I915_GT_CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT	I915_INC_BIT(0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (GEN8_RING_PDP_UDW).
 */
#define I915_GT_GEN8_RING_PDP_UDW(base, n)	((base) + 0x270 + (n) * 8 + 4)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (GEN8_RING_PDP_LDW).
 */
#define I915_GT_GEN8_RING_PDP_LDW(base, n)	((base) + 0x270 + (n) * 8)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_MODE_GEN7).
 */
#define I915_GT_RING_MODE_GEN7(base)	((base) + 0x29c)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (GFX_RUN_LIST_ENABLE).
 */
#define I915_GT_GFX_RUN_LIST_ENABLE	(1U << 15)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (GEN11_GFX_DISABLE_LEGACY_MODE).
 */
#define I915_GT_GEN11_GFX_DISABLE_LEGACY_MODE	(1U << 3)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (GEN12_GFX_PREFETCH_DISABLE).
 */
#define I915_GT_GEN12_GFX_PREFETCH_DISABLE	I915_INC_BIT(10)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_ELSP).
 */
#define I915_GT_RING_ELSP(base)	((base) + 0x230)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_EXECLIST_STATUS_LO).
 */
#define I915_GT_RING_EXECLIST_STATUS_LO(base)	((base) + 0x234)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_EXECLIST_STATUS_HI).
 */
#define I915_GT_RING_EXECLIST_STATUS_HI(base)	((base) + 0x234 + 4)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_CONTEXT_STATUS_PTR).
 */
#define I915_GT_RING_CONTEXT_STATUS_PTR(base)	((base) + 0x3a0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_EXECLIST_SQ_CONTENTS).
 */
#define I915_GT_RING_EXECLIST_SQ_CONTENTS(base)	((base) + 0x510)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_EXECLIST_CONTROL).
 */
#define I915_GT_RING_EXECLIST_CONTROL(base)	((base) + 0x550)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (EL_CTRL_LOAD).
 */
#define I915_GT_EL_CTRL_LOAD	I915_INC_BIT(0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (GEN8_EXECLISTS_STATUS_BUF).
 */
#define I915_GT_GEN8_EXECLISTS_STATUS_BUF	0x370

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (GEN11_EXECLISTS_STATUS_BUF2).
 */
#define I915_GT_GEN11_EXECLISTS_STATUS_BUF2	0x3c0

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_BB_PER_CTX_PTR).
 */
#define I915_GT_RING_BB_PER_CTX_PTR(base)	((base) + 0x1c0) /* gen8+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_INDIRECT_CTX).
 */
#define I915_GT_RING_INDIRECT_CTX(base)	((base) + 0x1c4) /* gen8+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_INDIRECT_CTX_OFFSET).
 */
#define I915_GT_RING_INDIRECT_CTX_OFFSET(base)	((base) + 0x1c8) /* gen8+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (GEN11_GFX_MSTR_IRQ).
 */
#define I915_GT_GEN11_GFX_MSTR_IRQ	(0x190010)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (GEN11_MASTER_IRQ).
 */
#define I915_GT_GEN11_MASTER_IRQ	(1U << 31)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (GEN11_DISPLAY_IRQ).
 */
#define I915_GT_GEN11_DISPLAY_IRQ	(1U << 16)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (GEN11_GU_MISC_IRQ).
 */
#define I915_GT_GEN11_GU_MISC_IRQ	(1U << 29)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (GEN11_GT_DW_IRQ).
 */
#define I915_GT_GEN11_GT_DW_IRQ(x)	(1U << (x))

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_GT_INTR_DW).
 */
#define I915_GT_GEN11_GT_INTR_DW(x)	(0x190018 + ((x) * 4))

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_INTR_IDENTITY_REG).
 */
#define I915_GT_GEN11_INTR_IDENTITY_REG(x)	(0x190060 + ((x) * 4))

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_INTR_DATA_VALID).
 */
#define I915_GT_GEN11_INTR_DATA_VALID	(1U << 31)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_INTR_ENGINE_CLASS).
 */
#define I915_GT_GEN11_INTR_ENGINE_CLASS(x)	(((x) & I915_INC_GENMASK(18, 16)) >> 16)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_INTR_ENGINE_INSTANCE).
 */
#define I915_GT_GEN11_INTR_ENGINE_INSTANCE(x)	(((x) & I915_INC_GENMASK(25, 20)) >> 20)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_INTR_ENGINE_INTR).
 */
#define I915_GT_GEN11_INTR_ENGINE_INTR(x)	((x) & 0xffff)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_IIR_REG_SELECTOR).
 */
#define I915_GT_GEN11_IIR_REG_SELECTOR(x)	(0x190070 + ((x) * 4))

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_RENDER_COPY_INTR_ENABLE).
 */
#define I915_GT_GEN11_RENDER_COPY_INTR_ENABLE	(0x190030)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_VCS_VECS_INTR_ENABLE).
 */
#define I915_GT_GEN11_VCS_VECS_INTR_ENABLE	(0x190034)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_RCS0_RSVD_INTR_MASK).
 */
#define I915_GT_GEN11_RCS0_RSVD_INTR_MASK	(0x190090)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_BCS_RSVD_INTR_MASK).
 */
#define I915_GT_GEN11_BCS_RSVD_INTR_MASK	(0x1900a0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_VCS0_VCS1_INTR_MASK).
 */
#define I915_GT_GEN11_VCS0_VCS1_INTR_MASK	(0x1900a8)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_VCS2_VCS3_INTR_MASK).
 */
#define I915_GT_GEN11_VCS2_VCS3_INTR_MASK	(0x1900ac)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_VECS0_VECS1_INTR_MASK).
 */
#define I915_GT_GEN11_VECS0_VECS1_INTR_MASK	(0x1900d0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_GPM_WGBOXPERF_INTR_ENABLE).
 */
#define I915_GT_GEN11_GPM_WGBOXPERF_INTR_ENABLE	(0x19003c)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_GPM_WGBOXPERF_INTR_MASK).
 */
#define I915_GT_GEN11_GPM_WGBOXPERF_INTR_MASK	(0x1900ec)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_GUC_SG_INTR_ENABLE).
 */
#define I915_GT_GEN11_GUC_SG_INTR_ENABLE	(0x190038)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_GUC_SG_INTR_MASK).
 */
#define I915_GT_GEN11_GUC_SG_INTR_MASK	(0x1900e8)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_CRYPTO_RSVD_INTR_ENABLE).
 */
#define I915_GT_GEN11_CRYPTO_RSVD_INTR_ENABLE	(0x190040)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_CRYPTO_RSVD_INTR_MASK).
 */
#define I915_GT_GEN11_CRYPTO_RSVD_INTR_MASK	(0x1900f0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (GT_RENDER_USER_INTERRUPT).
 */
#define I915_GT_GT_RENDER_USER_INTERRUPT	(1U <<  0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (GT_CONTEXT_SWITCH_INTERRUPT).
 */
#define I915_GT_GT_CONTEXT_SWITCH_INTERRUPT	(1U <<  8)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (GT_CS_MASTER_ERROR_INTERRUPT).
 */
#define I915_GT_GT_CS_MASTER_ERROR_INTERRUPT	I915_INC_BIT(3)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (GT_WAIT_SEMAPHORE_INTERRUPT).
 */
#define I915_GT_GT_WAIT_SEMAPHORE_INTERRUPT	I915_INC_BIT(11) /* bdw+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (I915_ERROR_INSTRUCTION).
 */
#define I915_GT_ERROR_INSTRUCTION	(1U << 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (LRC_PPHWSP_PN).
 */
#define I915_GT_LRC_PPHWSP_PN	(0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (LRC_PPHWSP_SZ).
 */
#define I915_GT_LRC_PPHWSP_SZ	(1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (LRC_STATE_PN).
 */
#define I915_GT_LRC_STATE_PN	(I915_GT_LRC_PPHWSP_PN + I915_GT_LRC_PPHWSP_SZ)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (LRC_STATE_OFFSET).
 */
#define I915_GT_LRC_STATE_OFFSET	(I915_GT_LRC_STATE_PN * I915_INC_PAGE_SIZE)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (LRC_PPHWSP_SCRATCH).
 */
#define I915_GT_LRC_PPHWSP_SCRATCH	0x34

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (LRC_PPHWSP_SCRATCH_ADDR).
 */
#define I915_GT_LRC_PPHWSP_SCRATCH_ADDR	(I915_GT_LRC_PPHWSP_SCRATCH * 4U)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_cs.c
 * (GEN11_LR_CONTEXT_RENDER_SIZE).
 */
#define I915_GT_GEN11_LR_CONTEXT_RENDER_SIZE	(14 * I915_INC_PAGE_SIZE)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_cs.c
 * (GEN8_LR_CONTEXT_OTHER_SIZE).
 */
#define I915_GT_GEN8_LR_CONTEXT_OTHER_SIZE	(2 * I915_INC_PAGE_SIZE)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (CTX_CONTEXT_CONTROL).
 */
#define I915_GT_CTX_CONTEXT_CONTROL	(0x02 + 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (CTX_RING_HEAD).
 */
#define I915_GT_CTX_RING_HEAD	(0x04 + 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (CTX_RING_TAIL).
 */
#define I915_GT_CTX_RING_TAIL	(0x06 + 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (CTX_RING_START).
 */
#define I915_GT_CTX_RING_START	(0x08 + 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (CTX_RING_CTL).
 */
#define I915_GT_CTX_RING_CTL	(0x0a + 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (CTX_BB_STATE).
 */
#define I915_GT_CTX_BB_STATE	(0x10 + 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (CTX_TIMESTAMP).
 */
#define I915_GT_CTX_TIMESTAMP	(0x22 + 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (CTX_PDP0_UDW).
 */
#define I915_GT_CTX_PDP0_UDW	(0x30 + 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (CTX_PDP0_LDW).
 */
#define I915_GT_CTX_PDP0_LDW	(0x32 + 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (CTX_R_PWR_CLK_STATE).
 */
#define I915_GT_CTX_R_PWR_CLK_STATE	(0x42 + 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (GEN8_CTX_VALID).
 */
#define I915_GT_GEN8_CTX_VALID	(1U << 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (GEN8_CTX_FORCE_RESTORE).
 */
#define I915_GT_GEN8_CTX_FORCE_RESTORE	(1U << 2)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (GEN8_CTX_L3LLC_COHERENT).
 */
#define I915_GT_GEN8_CTX_L3LLC_COHERENT	(1U << 5)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (GEN8_CTX_PRIVILEGE).
 */
#define I915_GT_GEN8_CTX_PRIVILEGE	(1U << 8)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (GEN8_CTX_ADDRESSING_MODE_SHIFT).
 */
#define I915_GT_GEN8_CTX_ADDRESSING_MODE_SHIFT	3

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (GEN8_CTX_ID_SHIFT).
 */
#define I915_GT_GEN8_CTX_ID_SHIFT	32

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (GEN11_SW_CTX_ID_SHIFT).
 */
#define I915_GT_GEN11_SW_CTX_ID_SHIFT	37

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (GEN11_SW_CTX_ID_WIDTH).
 */
#define I915_GT_GEN11_SW_CTX_ID_WIDTH	11

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (GEN11_ENGINE_CLASS_SHIFT).
 */
#define I915_GT_GEN11_ENGINE_CLASS_SHIFT	61

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (GEN11_ENGINE_INSTANCE_SHIFT).
 */
#define I915_GT_GEN11_ENGINE_INSTANCE_SHIFT	48

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (GEN11_MAX_CONTEXT_HW_ID).
 */
#define I915_GT_GEN11_MAX_CONTEXT_HW_ID	(1U << 11) /* exclusive */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (GEN12_MAX_CONTEXT_HW_ID).
 */
#define I915_GT_GEN12_MAX_CONTEXT_HW_ID	(I915_GT_GEN11_MAX_CONTEXT_HW_ID - 1)

/*
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/i915/gt/intel_execlists_submission.c (GEN12_IDLE_CTX_ID).
 */
#define I915_GT_GEN12_IDLE_CTX_ID	0x7FF

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (CTX_DESC_FORCE_RESTORE).
 */
#define I915_GT_CTX_DESC_FORCE_RESTORE	I915_INC_BIT64(2)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (GEN11_CSB_ENTRIES).
 */
#define I915_GT_GEN11_CSB_ENTRIES	12

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine.h
 * (I915_HWS_CSB_BUF0_INDEX).
 */
#define I915_GT_HWS_CSB_BUF0_INDEX	0x10

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine.h
 * (ICL_HWS_CSB_WRITE_INDEX).
 */
#define I915_GT_ICL_HWS_CSB_WRITE_INDEX	0x2f

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine.h
 * (I915_GEM_HWS_PREEMPT).
 */
#define I915_GT_HWS_PREEMPT	0x32

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine.h
 * (I915_GEM_HWS_SEQNO).
 */
#define I915_GT_HWS_SEQNO	0x40

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine.h
 * (I915_GEM_HWS_SEQNO_ADDR).
 */
#define I915_GT_HWS_SEQNO_ADDR	(I915_GT_HWS_SEQNO * 4U)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine.h
 * (I915_GEM_HWS_SCRATCH).
 */
#define I915_GT_HWS_SCRATCH	0x80

/*
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/i915/gt/intel_execlists_submission.c
 * (GEN12_CTX_STATUS_SWITCHED_TO_NEW_QUEUE).
 */
#define I915_GT_GEN12_CTX_STATUS_SWITCHED_TO_NEW_QUEUE	(0x1) /* lower csb dword */

/*
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/i915/gt/intel_execlists_submission.c
 * (GEN12_CTX_SWITCH_DETAIL).
 */
#define I915_GT_GEN12_CTX_SWITCH_DETAIL(csb_dw)	((csb_dw) & 0xF) /* upper csb dword */

/*
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/i915/gt/intel_execlists_submission.c
 * (GEN12_CSB_SW_CTX_ID_MASK).
 */
#define I915_GT_GEN12_CSB_SW_CTX_ID_MASK	I915_INC_GENMASK(25, 15)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_engine_types.h
 * (EXECLIST_MAX_PORTS).
 */
#define I915_GT_EXECLIST_MAX_PORTS	2

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (I915_GTT_PAGE_SIZE_4K).
 */
#define I915_GT_PAGE_4K	I915_INC_BIT64(12)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (I915_PDES).
 */
#define I915_GT_PDES	512

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (I915_PDE_MASK).
 */
#define I915_GT_PDE_MASK	(I915_GT_PDES - 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (GEN8_3LVL_PDPES).
 */
#define I915_GT_GEN8_3LVL_PDPES	4

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (GEN8_PAGE_PRESENT).
 */
#define I915_GT_GEN8_PAGE_PRESENT	I915_INC_BIT64(0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (GEN8_PAGE_RW).
 */
#define I915_GT_GEN8_PAGE_RW	I915_INC_BIT64(1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (GEN12_PPGTT_PTE_PAT0).
 */
#define I915_GT_GEN12_PPGTT_PTE_PAT0	I915_INC_BIT64(3)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (GEN12_PPGTT_PTE_PAT1).
 */
#define I915_GT_GEN12_PPGTT_PTE_PAT1	I915_INC_BIT64(4)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (GEN12_PPGTT_PTE_PAT2).
 */
#define I915_GT_GEN12_PPGTT_PTE_PAT2	I915_INC_BIT64(7)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (GEN12_GGTT_PTE_LM).
 */
#define I915_GT_GEN12_GGTT_PTE_LM	I915_INC_BIT64(1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (GEN12_GGTT_PTE_ADDR_MASK).
 */
#define I915_GT_GEN12_GGTT_PTE_ADDR_MASK	I915_INC_GENMASK64(45, 12)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (GEN8_PDE_PS_2M).
 */
#define I915_GT_GEN8_PDE_PS_2M	I915_INC_BIT(7)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (GEN8_PDE_IPS_64K).
 */
#define I915_GT_GEN8_PDE_IPS_64K	I915_INC_BIT(11)

/*
 * The engine registers and context definitions the execution path uses
 * beside the ones above.
 */

/*
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine.h
 * (I915_GEM_HWS_PREEMPT_ADDR, I915_GEM_HWS_MIGRATE, I915_GEM_HWS_PXP,
 * I915_GEM_HWS_PXP_ADDR).
 */
#define I915_GT_HWS_PREEMPT_ADDR	(I915_GT_HWS_PREEMPT * 4U)
#define I915_GT_HWS_MIGRATE	(0x42 * 4U)
#define I915_GT_HWS_PXP	0x60
#define I915_GT_HWS_PXP_ADDR	(I915_GT_HWS_PXP * 4U)

/*
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_CMD_BUF_CCTL, RING_CTX_TIMESTAMP, GEN8_RING_CS_GPR).
 */
#define I915_GT_RING_CMD_BUF_CCTL(base)	((base) + 0x84)
#define I915_GT_RING_CTX_TIMESTAMP(base)	((base) + 0x3a8) /* gen8+ */
#define I915_GT_GEN8_RING_CS_GPR(base, n)	((base) + 0x600 + (n) * 8)

/*
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN12_CS_DEBUG_MODE2, INSTRUCTION_STATE_CACHE_INVALIDATE,
 * GEN12_CCS_AUX_INV, GEN12_VD0_AUX_INV, GEN12_VE0_AUX_INV, GEN12_BCS0_AUX_INV,
 * GEN12_VD2_AUX_INV, AUX_INV, MSG_IDLE_CS, MSG_IDLE_VCS0, MSG_IDLE_BCS,
 * MSG_IDLE_VECS0, MSG_IDLE_VCS2, MSG_IDLE_FW_MASK, MSG_IDLE_FW_SHIFT,
 * GEN9_PWRGT_DOMAIN_STATUS); drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (GEN8_RPCS_ENABLE, GEN8_RPCS_S_CNT_ENABLE, GEN11_RPCS_S_CNT_SHIFT,
 * GEN11_RPCS_S_CNT_MASK).
 */
#define I915_GT_GEN12_CS_DEBUG_MODE2	(0x20d8)
#define I915_GT_INSTRUCTION_STATE_CACHE_INVALIDATE	I915_INC_BIT(6)
#define I915_GT_GEN12_CCS_AUX_INV	(0x4208)
#define I915_GT_GEN12_VD0_AUX_INV	(0x4218)
#define I915_GT_GEN12_VE0_AUX_INV	(0x4238)
#define I915_GT_GEN12_BCS0_AUX_INV	(0x4248)
#define I915_GT_GEN12_VD2_AUX_INV	(0x4298)
#define I915_GT_AUX_INV	I915_INC_BIT(0)
#define I915_GT_MSG_IDLE_CS	(0x8000)
#define I915_GT_MSG_IDLE_VCS0	(0x8004)
#define I915_GT_MSG_IDLE_BCS	(0x800C)
#define I915_GT_MSG_IDLE_VECS0	(0x8010)
#define I915_GT_MSG_IDLE_VCS2	(0x80C0)
#define I915_GT_MSG_IDLE_FW_MASK	I915_INC_GENMASK(13, 9)
#define I915_GT_MSG_IDLE_FW_SHIFT	9
#define I915_GT_GEN9_PWRGT_DOMAIN_STATUS	(0xA2A0)
#define I915_GT_GEN8_RPCS_ENABLE	(1U << 31)
#define I915_GT_GEN8_RPCS_S_CNT_ENABLE	(1U << 18)
#define I915_GT_GEN11_RPCS_S_CNT_SHIFT	12
#define I915_GT_GEN11_RPCS_S_CNT_MASK	(0x3fU << I915_GT_GEN11_RPCS_S_CNT_SHIFT)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (INTEL_LEGACY_64B_CONTEXT).
 */
#define I915_GT_INTEL_LEGACY_64B_CONTEXT	3

/*
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_lrc.h
 * (DG2_PREDICATE_RESULT_WA, DG2_PREDICATE_RESULT_BB).
 */
#define I915_GT_DG2_PREDICATE_RESULT_WA	(I915_INC_PAGE_SIZE - 8U)
#define I915_GT_DG2_PREDICATE_RESULT_BB	(2048)

/*
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (GEN12_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT).
 */
#define I915_GT_GEN12_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT	0xD

/*
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/pxp/intel_pxp_regs.h
 * (GEN12_KCR_BASE).
 */
#define I915_GT_GEN12_KCR_BASE	0x32000

/*
 * The Gen9 (Skylake to Comet Lake) GT registers and fields.
 */

/*
 * the Gen8 master interrupt control and GT
 * banks
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (GEN8_MASTER_IRQ, GEN8_MASTER_IRQ_CONTROL, GEN8_PCU_IRQ, GEN8_DE_PCH_IRQ,
 * GEN8_DE_MISC_IRQ, GEN8_DE_PORT_IRQ, GEN8_DE_PIPE_IRQ, GEN8_GT_VECS_IRQ,
 * GEN8_GT_GUC_IRQ, GEN8_GT_PM_IRQ, GEN8_GT_VCS1_IRQ, GEN8_GT_VCS0_IRQ,
 * GEN8_GT_BCS_IRQ, GEN8_GT_RCS_IRQ, GEN8_GT_ISR, GEN8_GT_IMR, GEN8_GT_IIR,
 * GEN8_GT_IER, GEN8_RCS_IRQ_SHIFT, GEN8_BCS_IRQ_SHIFT, GEN8_VCS0_IRQ_SHIFT,
 * GEN8_VCS1_IRQ_SHIFT, GEN8_VECS_IRQ_SHIFT, GEN8_DE_PIPE_IMR,
 * GEN8_DE_PIPE_IIR, GEN8_DE_PIPE_IER, GEN8_DE_PORT_IMR, GEN8_DE_PORT_IIR,
 * GEN8_DE_PORT_IER, GEN8_DE_MISC_IMR, GEN8_DE_MISC_IIR, GEN8_DE_MISC_IER).
 */
#define I915_GT_GEN8_MASTER_IRQ	(0x44200)
#define I915_GT_GEN8_MASTER_IRQ_CONTROL	(1U << 31)
#define I915_GT_GEN8_PCU_IRQ	(1U << 30)
#define I915_GT_GEN8_DE_PCH_IRQ	(1U << 23)
#define I915_GT_GEN8_DE_MISC_IRQ	(1U << 22)
#define I915_GT_GEN8_DE_PORT_IRQ	(1U << 20)
#define I915_GT_GEN8_DE_PIPE_IRQ(pipe)	(1U << (16 + (pipe)))
#define I915_GT_GEN8_GT_VECS_IRQ	(1U << 6)
#define I915_GT_GEN8_GT_GUC_IRQ	(1U << 5)
#define I915_GT_GEN8_GT_PM_IRQ	(1U << 4)
#define I915_GT_GEN8_GT_VCS1_IRQ	(1U << 3)
#define I915_GT_GEN8_GT_VCS0_IRQ	(1U << 2)
#define I915_GT_GEN8_GT_BCS_IRQ	(1U << 1)
#define I915_GT_GEN8_GT_RCS_IRQ	(1U << 0)
#define I915_GT_GEN8_GT_ISR(which)	(0x44300 + (0x10 * (which)))
#define I915_GT_GEN8_GT_IMR(which)	(0x44304 + (0x10 * (which)))
#define I915_GT_GEN8_GT_IIR(which)	(0x44308 + (0x10 * (which)))
#define I915_GT_GEN8_GT_IER(which)	(0x4430c + (0x10 * (which)))
#define I915_GT_GEN8_RCS_IRQ_SHIFT	0
#define I915_GT_GEN8_BCS_IRQ_SHIFT	16
#define I915_GT_GEN8_VCS0_IRQ_SHIFT	0
#define I915_GT_GEN8_VCS1_IRQ_SHIFT	16
#define I915_GT_GEN8_VECS_IRQ_SHIFT	0
#define I915_GT_GEN8_DE_PIPE_IMR(pipe)	(0x44404 + (0x10 * (pipe)))
#define I915_GT_GEN8_DE_PIPE_IIR(pipe)	(0x44408 + (0x10 * (pipe)))
#define I915_GT_GEN8_DE_PIPE_IER(pipe)	(0x4440c + (0x10 * (pipe)))
#define I915_GT_GEN8_DE_PORT_IMR	(0x44444)
#define I915_GT_GEN8_DE_PORT_IIR	(0x44448)
#define I915_GT_GEN8_DE_PORT_IER	(0x4444c)
#define I915_GT_GEN8_DE_MISC_IMR	(0x44464)
#define I915_GT_GEN8_DE_MISC_IIR	(0x44468)
#define I915_GT_GEN8_DE_MISC_IER	(0x4446c)

/*
 * the pre-Gen11 context control
 * bits and the Gen8 power clock fields
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (CTX_CTRL_RS_CTX_ENABLE, CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT,
 * GEN8_RPCS_S_CNT_SHIFT, GEN8_RPCS_S_CNT_MASK, GEN8_RPCS_EU_MAX_SHIFT,
 * GEN8_RPCS_EU_MAX_MASK, GEN8_RPCS_EU_MIN_SHIFT, GEN8_RPCS_EU_MIN_MASK).
 */
#define I915_GT_CTX_CTRL_RS_CTX_ENABLE	(1U << 1)
#define I915_GT_CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT	(1U << 2)
#define I915_GT_GEN8_RPCS_S_CNT_SHIFT	15
#define I915_GT_GEN8_RPCS_S_CNT_MASK	(0x7U << I915_GT_GEN8_RPCS_S_CNT_SHIFT)
#define I915_GT_GEN8_RPCS_EU_MAX_SHIFT	4
#define I915_GT_GEN8_RPCS_EU_MAX_MASK	(0xfU << I915_GT_GEN8_RPCS_EU_MAX_SHIFT)
#define I915_GT_GEN8_RPCS_EU_MIN_SHIFT	0
#define I915_GT_GEN8_RPCS_EU_MIN_MASK	(0xfU << I915_GT_GEN8_RPCS_EU_MIN_SHIFT)

/*
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (GEN9_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT).
 */
#define I915_GT_GEN9_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT	0x26

/*
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_lrc_reg.h
 * (GEN11_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT).
 */
#define I915_GT_GEN11_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT	0x1A

/*
 * The subslice count field of the power clock state.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (GEN8_RPCS_SS_CNT_ENABLE, GEN8_RPCS_SS_CNT_SHIFT, GEN8_RPCS_SS_CNT_MASK).
 */
#define I915_GT_GEN8_RPCS_SS_CNT_ENABLE	(1U << 11)
#define I915_GT_GEN8_RPCS_SS_CNT_SHIFT	8
#define I915_GT_GEN8_RPCS_SS_CNT_MASK	(0x7U << I915_GT_GEN8_RPCS_SS_CNT_SHIFT)

/*
 * the registers of the Gen9 indirect
 * context batch
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN9_DISABLE_GATHER_AT_SET_SHADER_COMMON_SLICE, GEN8_L3SQCREG4,
 * GEN8_LQSC_FLUSH_COHERENT_LINES).
 */
#define I915_GT_GEN9_FF_SLICE_CHICKEN	(0x2088)
#define I915_GT_GEN9_FF_SLICE_CHICKEN_CL_PROVOKING_VERTEX_FIX	(1U << 1)
#define I915_GT_GEN9_3D_CHICKEN3	(0x2090)
#define I915_GT_GEN9_3D_CHICKEN_SF_PROVOKING_VERTEX_FIX	(1U << 12)
#define I915_GT_GEN9_COMMON_SLICE_CHICKEN2	(0x7014)
#define I915_GT_GEN9_DISABLE_GATHER_AT_SET_SHADER_COMMON_SLICE	(1U << 12)
#define I915_GT_GEN8_L3SQCREG4	(0xb118)
#define I915_GT_GEN8_LQSC_FLUSH_COHERENT_LINES	(1U << 21)

/*
 * drivers/gpu/drm/i915/gt/intel_engine_regs.h, intel_engine.h,
 * intel_lrc_reg.h, intel_execlists_submission.c: Gen8 execlists
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (RING_IMR, RING_ELSP, GFX_RUN_LIST_ENABLE);
 * drivers/gpu/drm/i915/gt/intel_lrc_reg.h (GEN8_CSB_ENTRIES);
 * drivers/gpu/drm/i915/gt/intel_execlists_submission.c
 * (GEN8_CTX_STATUS_IDLE_ACTIVE, GEN8_CTX_STATUS_PREEMPTED).
 */
#define I915_GT_RING_IMR(base)	((base) + 0xa8)
#define I915_GT_RING_ELSP(base)	((base) + 0x230)
#define I915_GT_GFX_RUN_LIST_ENABLE	(1U << 15)
#define I915_GT_HWS_CSB_WRITE_INDEX	0x1f
#define I915_GT_GEN8_CSB_ENTRIES	6
#define I915_GT_GEN8_CTX_STATUS_IDLE_ACTIVE	(1U << 0)
#define I915_GT_GEN8_CTX_STATUS_PREEMPTED	(1U << 1)

#endif /* DRIVERS_GPU_I915_INTEL_GT_REGS_H */
