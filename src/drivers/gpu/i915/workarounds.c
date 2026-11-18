/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT, engine and context workarounds, the register whitelist, MOCS and PAT
 * (see workarounds.h).
 *
 * Each list is built from a table of rules: what kind of entry, which
 * register (absolute, or relative to the engine's base), the bits, whether
 * the register is multicast, the workaround's name, and the condition on
 * the platform and stepping under which it applies.  A few entries depend
 * on the GT's state (the steering, the video engines, the MOCS index) and
 * are added by code before or after the table.  The lists cover graphics
 * version 12, Ice Lake (graphics version 11) and the Gen9 parts: Skylake,
 * Kaby Lake, Coffee Lake and Comet Lake.  Every register access goes through the held accessors: the device
 * start holds every GT forcewake domain across the GT initialization and
 * resume that call these functions.
 */

#include "i915.h"
#include "workarounds.h"
#include "gt-power.h"
#include "mmio.h"
#include "device-info.h"

#include <kern/klog.h>

#include <stddef.h>
#include <stdint.h>

#include "intel/gt-regs.h"
#include "intel/mocs.h"
#include "intel/workarounds.h"

/* How many PAT entries the private PAT has. */
#define I915_PAT_ENTRIES	8U

/* The number of entries of a table. */
#define I915_WA_COUNT(table)	(sizeof(table) / sizeof((table)[0]))

/* One row of a platform MOCS table: the entry index and its two values. */
struct i915_mocs_row {
	unsigned idx;
	uint32_t control;
	uint16_t l3cc;
};

/* The Gen12 MOCS table.  The table never changes. */
static const struct i915_mocs_row i915_gen12_mocs_table[] = {
#include "intel/mocs-table.inc"
};

/* The Gen9 MOCS table, which every engine is programmed with on its own.  The table never changes. */
static const struct i915_mocs_row i915_gen9_mocs_table[] = {
#include "intel/mocs-table-gen9.inc"
};

/* The Ice Lake MOCS table, which every engine is programmed with on its own.  The table never changes. */
static const struct i915_mocs_row i915_gen11_mocs_table[] = {
#include "intel/mocs-table-gen11.inc"
};

/*
 * The Gen12 private PAT: the memory type of every PAT index.  Gen12 has no
 * LLC or age settings, so each entry carries only a memory type.  The
 * table never changes.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.c
 * (tgl_setup_private_ppat).
 */
static const uint32_t i915_gen12_private_pat[I915_PAT_ENTRIES] = {
	I915_GT_GEN8_PPAT_WB,
	I915_GT_GEN8_PPAT_WC,
	I915_GT_GEN8_PPAT_WT,
	I915_GT_GEN8_PPAT_UC,
	I915_GT_GEN8_PPAT_WB,
	I915_GT_GEN8_PPAT_WB,
	I915_GT_GEN8_PPAT_WB,
	I915_GT_GEN8_PPAT_WB
};

/* What one rule adds. */
enum i915_wa_op {
	I915_WA_OP_OR,		/* sets bits, verifies them */
	I915_WA_OP_FIELD,	/* clears a field (a), sets a value (b), verifies both */
	I915_WA_OP_MASKED_BITS,	/* enables bits (a) of a masked register */
	I915_WA_OP_MASKED_FIELD,	/* sets a field (a) of a masked register to a value (b) */
	I915_WA_OP_NO_VERIFY,	/* clears (a), sets (b), and is not read back */
	I915_WA_OP_WHITELIST	/* whitelists a register with access flags (a) */
};

/* One workaround rule. */
struct i915_wa_rule {
	enum i915_wa_op op;

	/* The register; with relative set, the offset from the engine's base. */
	uint32_t reg;
	int relative;

	/* The bits, as the operation reads them. */
	uint32_t a;
	uint32_t b;

	/* Nonzero for a multicast register. */
	int mcr;

	/* The condition on the platform and stepping, or NULL for always. */
	int (*applies)(const struct i915_gt_info *gt);

	/* The workaround name, a static string, for the manifest. */
	const char *name;
};

static int i915_wa_skl_kbl(const struct i915_gt_info *gt);
static int i915_wa_not_skl_kbl(const struct i915_gt_info *gt);
static int i915_wa_kbl_before_c0(const struct i915_gt_info *gt);
static int i915_wa_in_place_decompress(const struct i915_gt_info *gt);
static int i915_wa_kbl_sema_poll(const struct i915_gt_info *gt);
static int i915_wa_push_const_fix(const struct i915_gt_info *gt);
static int i915_wa_not_skl(const struct i915_gt_info *gt);
static int i915_wa_cml(const struct i915_gt_info *gt);
static int i915_wa_render_psmi(const struct i915_gt_info *gt);

/*
 * The Ice Lake GT rules, after the steering.  The last rule is not a
 * workaround but lowers the sampler's power.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (icl_gt_workarounds_init).
 */
static const struct i915_wa_rule i915_wa_gt_gen11[] = {
	{ I915_WA_OP_FIELD, I915_GT_GEN11_GACB_PERF_CTRL, 0, I915_GT_GEN11_HASH_CTRL_MASK, I915_GT_GEN11_HASH_CTRL_BIT0 | I915_GT_GEN11_HASH_CTRL_BIT4, 0, NULL, "WaModifyGamTlbPartitioning" },
	{ I915_WA_OP_OR, I915_GT_GEN11_LSN_UNSLCVC, 0, I915_GT_GEN11_LSN_UNSLCVC_GAFS_HALF_SF_MAXALLOC | I915_GT_GEN11_LSN_UNSLCVC_GAFS_HALF_CL2_MAXALLOC, 0U, 0, NULL, "Wa_1405766107" },
	{ I915_WA_OP_OR, I915_GT_GEN8_GAMW_ECO_DEV_RW_IA, 0, I915_GT_GAMW_ECO_DEV_CTX_RELOAD_DISABLE, 0U, 0, NULL, "Wa_220166154" },
	{ I915_WA_OP_OR, I915_GT_GAMT_CHKN_BIT_REG, 0, I915_GT_GAMT_CHKN_DISABLE_L3_COH_PIPE, 0U, 0, NULL, "Wa_1406463099" },
	{ I915_WA_OP_OR, I915_GT_UNSLICE_UNIT_LEVEL_CLKGATE, 0, I915_GT_VSUNIT_CLKGATE_DIS | I915_GT_HSUNIT_CLKGATE_DIS, 0U, 0, NULL, "Wa_1408615072" },
	{ I915_WA_OP_OR, I915_GT_UNSLICE_UNIT_LEVEL_CLKGATE2, 0, I915_GT_PSDUNIT_CLKGATE_DIS, 0U, 0, NULL, "Wa_1407352427" },
	{ I915_WA_OP_OR, I915_GT_GEN11_SUBSLICE_UNIT_LEVEL_CLKGATE, 0, I915_GT_GWUNIT_CLKGATE_DIS, 0U, 1, NULL, "Wa_1406680159" },
	{ I915_WA_OP_OR, I915_GT_GEN11_SLICE_UNIT_LEVEL_CLKGATE, 0, I915_GT_L3_CLKGATE_DIS | I915_GT_L3_CR2X_CLKGATE_DIS, 0U, 0, NULL, "Wa_1607087056" },
	{ I915_WA_OP_FIELD, I915_GT_GEN10_DFR_RATIO_EN_AND_CHICKEN, 0, I915_GT_DFR_DISABLE, 0U, 1, NULL, "sampler power: DFR enabled" },
};

/*
 * The Ice Lake render engine's rules: the indirect state base address
 * override (not a workaround: sampler border colors relative to the
 * dynamic state base), the line quality fix (not a workaround either),
 * then the workarounds and the per-context preemption control.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (general_render_compute_wa_init and rcs_engine_wa_init: the graphics
 * version 11 entries).
 */
static const struct i915_wa_rule i915_wa_engine_gen11_render[] = {
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN10_SAMPLER_MODE, 0, I915_GT_GEN11_INDIRECT_STATE_BASE_ADDR_OVERRIDE, 0U, 1, NULL, "indirect state base address override" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN9_3D_CHICKEN3, 0, I915_GT_3D_CHICKEN3_AA_LINE_QUALITY_FIX_ENABLE, 0U, 0, NULL, "AA line quality fix" },
	{ I915_WA_OP_OR, I915_GT_GEN8_GARBCNTL, 0, I915_GT_GEN11_ARBITRATION_PRIO_ORDER_MASK, 0U, 0, NULL, "Wa_1405543622" },
	{ I915_WA_OP_FIELD, I915_GT_GEN8_GARBCNTL, 0, I915_GT_GEN11_HASH_CTRL_EXCL_MASK, I915_GT_GEN11_HASH_CTRL_EXCL_BIT0, 0, NULL, "Wa_1604223664" },
	{ I915_WA_OP_FIELD, I915_GT_GEN11_GLBLINVL, 0, I915_GT_GEN11_BANK_HASH_ADDR_EXCL_MASK, I915_GT_GEN11_BANK_HASH_ADDR_EXCL_BIT0, 0, NULL, "Wa_1604223664" },
	{ I915_WA_OP_OR, I915_GT_GEN8_L3SQCREG4, 0, I915_GT_GEN11_LQSC_CLEAN_EVICT_DISABLE, 0U, 1, NULL, "Wa_1405733216" },
	{ I915_WA_OP_OR, I915_GT_GEN7_SARCHKMD, 0, I915_GT_GEN7_DISABLE_SAMPLER_PREFETCH, 0U, 0, NULL, "Wa_1606682166" },
	{ I915_WA_OP_FIELD, I915_GT_GEN11_SCRATCH2, 0, I915_GT_GEN11_COHERENT_PARTIAL_WRITE_MERGE_ENABLE, 0U, 1, NULL, "Wa_1409178092" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN9_CSFE_CHICKEN1_RCS, 0, I915_GT_GEN11_ENABLE_32_PLANE_MODE, 0U, 0, NULL, "WaEnable32PlaneMode" },
	{ I915_WA_OP_OR, I915_GT_GEN7_FF_THREAD_MODE, 0, I915_GT_GEN12_FF_TESSELATION_DOP_GATE_DISABLE, 0U, 0, NULL, "Wa_1408767742" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN9_CS_DEBUG_MODE1, 0, I915_GT_FF_DOP_CLOCK_GATE_DISABLE, 0U, 0, NULL, "Wa_22010271021" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN7_FF_SLICE_CS_CHICKEN1, 0, I915_GT_GEN9_FFSC_PERCTX_PREEMPT_CTRL, 0U, 0, NULL, "perctx preempt ctrl" },
};

/*
 * The Ice Lake render engine's context rules.  The float blend entry is
 * write-only and the FBC upper base is written outright; neither is read
 * back.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (icl_ctx_workarounds_init).
 */
static const struct i915_wa_rule i915_wa_context_gen11_render[] = {
	{ I915_WA_OP_FIELD, I915_GT_GEN8_L3CNTLREG, 0, 0xffffffffU, I915_GT_GEN8_ERRDETBCTRL, 0, NULL, "Wa_1406697149" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_ICL_HDC_MODE, 0, I915_GT_HDC_FORCE_NON_COHERENT, 0U, 1, NULL, "WaForceEnableNonCoherent" },
	{ I915_WA_OP_NO_VERIFY, I915_GT_GEN10_CACHE_MODE_SS, 0, 0U, (I915_GT_FLOAT_BLEND_OPTIMIZATION_ENABLE << 16) | I915_GT_FLOAT_BLEND_OPTIMIZATION_ENABLE, 1, NULL, "WaEnableFloatBlendOptimization" },
	{ I915_WA_OP_MASKED_FIELD, I915_GT_GEN8_CS_CHICKEN1, 0, I915_GT_GEN9_PREEMPT_GPGPU_LEVEL_MASK, I915_GT_GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL, 0, NULL, "WaDisableGPGPUMidThreadPreemption" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN10_SAMPLER_MODE, 0, I915_GT_GEN11_SAMPLER_ENABLE_HEADLESS_MSG, 0U, 1, NULL, "headerless sampler messages" },
	{ I915_WA_OP_FIELD, I915_GT_IVB_FBC_RT_BASE, 0, 0xffffffffU, 0xffffffffU & ~I915_GT_ILK_FBC_RT_VALID, 0, NULL, "Wa_1604278689" },
	{ I915_WA_OP_NO_VERIFY, I915_GT_IVB_FBC_RT_BASE_UPPER, 0, 0U, 0xffffffffU, 0, NULL, "Wa_1604278689" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN9_ROW_CHICKEN4, 0, I915_GT_GEN11_DIS_PICK_2ND_EU, 0U, 1, NULL, "Wa_1406306137" },
};

/*
 * The Ice Lake render engine's whitelist: two sampler chickens, the state
 * cache redirect, and the four depth and invocation counters.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (icl_whitelist_build).
 */
static const struct i915_wa_rule i915_wa_whitelist_gen11_render[] = {
	{ I915_WA_OP_WHITELIST, I915_GT_GEN9_HALF_SLICE_CHICKEN7, 0, 0U, 0U, 0, NULL, "WaAllowUMDToModifyHalfSliceChicken7" },
	{ I915_WA_OP_WHITELIST, I915_GT_GEN10_SAMPLER_MODE, 0, 0U, 0U, 0, NULL, "WaAllowUMDToModifySamplerMode" },
	{ I915_WA_OP_WHITELIST, I915_GT_GEN9_SLICE_COMMON_ECO_CHICKEN1, 0, 0U, 0U, 0, NULL, "WaEnableStateCacheRedirectToCS" },
	{ I915_WA_OP_WHITELIST, I915_GT_PS_INVOCATION_COUNT, 0, I915_GT_RING_FORCE_TO_NONPRIV_ACCESS_RD | I915_GT_RING_FORCE_TO_NONPRIV_RANGE_4, 0U, 0, NULL, "WaAllowPMDepthAndInvocationCountAccessFromUMD" },
};

/*
 * The Ice Lake video decode engine's whitelist after its context
 * timestamp: the three HuC status registers, read only.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (icl_whitelist_build: VIDEO_DECODE_CLASS).
 */
static const struct i915_wa_rule i915_wa_whitelist_gen11_video[] = {
	{ I915_WA_OP_WHITELIST, I915_GT_HUC_STATUS(0U), 1, I915_GT_RING_FORCE_TO_NONPRIV_ACCESS_RD, 0U, 0, NULL, "HuC status" },
	{ I915_WA_OP_WHITELIST, I915_GT_HUC_UKERNEL_HDR_INFO(0U), 1, I915_GT_RING_FORCE_TO_NONPRIV_ACCESS_RD, 0U, 0, NULL, "HuC kernel header" },
	{ I915_WA_OP_WHITELIST, I915_GT_HUC_STATUS2(0U), 1, I915_GT_RING_FORCE_TO_NONPRIV_ACCESS_RD, 0U, 0, NULL, "HuC status 2" },
};

/*
 * The Gen12 GT rules, after the steering and the video engines' entries.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (gen12_gt_workarounds_init: Wa_14011059788, Wa_14015795083).
 */
static const struct i915_wa_rule i915_wa_gt_gen12[] = {
	{ I915_WA_OP_OR, I915_GT_GEN10_DFR_RATIO_EN_AND_CHICKEN, 0, I915_GT_DFR_DISABLE, 0U, 1, NULL, "Wa_14011059788" },
	/* The firmware may lock the register, so the clear is not read back. */
	{ I915_WA_OP_NO_VERIFY, I915_GT_GEN7_MISCCPCTL, 0, I915_GT_GEN12_DOP_CLOCK_GATE_RENDER_ENABLE, 0U, 0, NULL, "Wa_14015795083" },
};

/*
 * The Gen9 GT rules, after the steering.  The stepping is the PCI
 * revision.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (gen9_gt_workarounds_init, skl_gt_workarounds_init,
 * kbl_gt_workarounds_init, cfl_gt_workarounds_init).
 */
static const struct i915_wa_rule i915_wa_gt_gen9[] = {
	{ I915_WA_OP_OR, I915_GT_GAM_ECOCHK, 0, I915_GT_ECOCHK_DIS_TLB, 0U, 0, i915_wa_skl_kbl, "WaDisableKillLogic" },
	/* Matches the display engine's hash mode. */
	{ I915_WA_OP_OR, I915_GT_MMCD_MISC_CTRL, 0, I915_GT_MMCD_PCLA | I915_GT_MMCD_HOTSPOT_EN, 0U, 0, NULL, "WaCompressedResourceSamplerPbeMediaNewHashMode" },
	{ I915_WA_OP_OR, I915_GT_GAM_ECOCHK, 0, I915_GT_BDW_DISABLE_HDC_INVALIDATION, 0U, 0, NULL, "WaDisableHDCInvalidation" },
	{ I915_WA_OP_OR, I915_GT_GAMT_CHKN_BIT_REG, 0, I915_GT_GAMT_CHKN_DISABLE_DYNAMIC_CREDIT_SHARING, 0U, 0, i915_wa_kbl_before_c0, "WaDisableDynamicCreditSharing" },
	{ I915_WA_OP_OR, I915_GT_GEN7_UCGCTL4, 0, I915_GT_GEN8_EU_GAUNIT_CLOCK_GATE_DISABLE, 0U, 0, NULL, "WaDisableGafsUnitClkGating" },
	{ I915_WA_OP_OR, I915_GT_GEN9_GAMT_ECO_REG_RW_IA, 0, I915_GT_GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS, 0U, 0, i915_wa_in_place_decompress, "WaInPlaceDecompressionHang" },
};

/*
 * The Gen12 render engine's rules.
 *
 * The PSMI rule (Wa_1607297627) applies only to the platforms whose product
 * row lists it.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (rcs_engine_wa_init: the graphics version 12 entries).
 */
static const struct i915_wa_rule i915_wa_engine_gen12_render[] = {
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN9_CS_DEBUG_MODE1, 0, I915_GT_FF_DOP_CLOCK_GATE_DISABLE, 0U, 0, NULL, "Wa_1606700617" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN8_ROW_CHICKEN2, 0, I915_GT_GEN12_DISABLE_EARLY_READ, 0U, 1, NULL, "Wa_1606931601" },
	{ I915_WA_OP_OR, I915_GT_GEN7_FF_THREAD_MODE, 0, I915_GT_GEN12_FF_TESSELATION_DOP_GATE_DISABLE, 0U, 0, NULL, "Wa_14010919138" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN10_SAMPLER_MODE, 0, I915_GT_ENABLE_SMALLPL, 0U, 1, NULL, "Wa_1406941453" },
	/* Merges into the ROW_CHICKEN2 entry above. */
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN8_ROW_CHICKEN2, 0, I915_GT_GEN12_PUSH_CONST_DEREF_HOLD_DIS, 0U, 1, NULL, "Wa_1409804808" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN9_ROW_CHICKEN4, 0, I915_GT_GEN12_DISABLE_TDL_PUSH, 0U, 1, NULL, "Wa_14010229206" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_RING_PSMI_CTL(0U), 1, I915_GT_GEN12_WAIT_FOR_EVENT_POWER_DOWN_DISABLE | I915_GT_GEN8_RC_SEMA_IDLE_MSG_DISABLE, 0U, 0, i915_wa_render_psmi, "Wa_1607297627" },
};

/*
 * The Gen12 render engine's context rules.
 *
 * FF_MODE2 reads back the wrong value from the CPU (Wa_1608008084), so its
 * clear mask is all ones -- the wanted value is written outright -- and it
 * is not read back.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (gen12_ctx_workarounds_init).
 */
static const struct i915_wa_rule i915_wa_context_gen12_render[] = {
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN11_COMMON_SLICE_CHICKEN3, 0, I915_GT_GEN12_DISABLE_CPS_AWARE_COLOR_PIPE, 0U, 0, NULL, "Wa_1409142259" },
	{ I915_WA_OP_MASKED_FIELD, I915_GT_GEN8_CS_CHICKEN1, 0, I915_GT_GEN9_PREEMPT_GPGPU_LEVEL_MASK, I915_GT_GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL, 0, NULL, "WaDisableGPGPUMidThreadPreemption" },
	{ I915_WA_OP_NO_VERIFY, I915_GT_GEN12_FF_MODE2, 0, 0xffffffffU, I915_GT_FF_MODE2_TDS_TIMER_128 | I915_GT_FF_MODE2_GS_TIMER_224, 0, NULL, "Wa_16011163337 (FF_MODE2)" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_HIZ_CHICKEN, 0, I915_GT_HZ_DEPTH_TEST_LE_GE_OPT_DISABLE, 0U, 0, NULL, "Wa_1806527549" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_COMMON_SLICE_CHICKEN4, 0, I915_GT_DISABLE_TDC_LOAD_BALANCING_CALC, 0U, 0, NULL, "Wa_1606376872" },
};

/*
 * The Gen12 render engine's whitelist: four consecutive counters, hence
 * RANGE_4, then three chicken registers, the last for a tuning setting.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (tgl_whitelist_build).
 */
static const struct i915_wa_rule i915_wa_whitelist_gen12_render[] = {
	{ I915_WA_OP_WHITELIST, I915_GT_PS_INVOCATION_COUNT, 0, I915_GT_RING_FORCE_TO_NONPRIV_ACCESS_RD | I915_GT_RING_FORCE_TO_NONPRIV_RANGE_4, 0U, 0, NULL, "Wa_1408556865 (PS_INVOCATION_COUNT x4)" },
	{ I915_WA_OP_WHITELIST, I915_GT_GEN7_COMMON_SLICE_CHICKEN1, 0, 0U, 0U, 0, NULL, "Wa_1508744258" },
	{ I915_WA_OP_WHITELIST, I915_GT_HIZ_CHICKEN, 0, 0U, 0U, 0, NULL, "Wa_1806527549 (whitelist)" },
	{ I915_WA_OP_WHITELIST, I915_GT_GEN11_COMMON_SLICE_CHICKEN3, 0, 0U, 0U, 0, NULL, "tuning" },
};

/* A non-render engine's whitelist: its context timestamp, read only. */
static const struct i915_wa_rule i915_wa_whitelist_timestamp[] = {
	{ I915_WA_OP_WHITELIST, I915_GT_RING_CTX_TIMESTAMP(0U), 1, I915_GT_RING_FORCE_TO_NONPRIV_ACCESS_RD, 0U, 0, NULL, "ctx timestamp RD" },
};

/* The Comet Lake non-render engine's whitelist: its context timestamp, read only. */
static const struct i915_wa_rule i915_wa_whitelist_gen9_other[] = {
	{ I915_WA_OP_WHITELIST, I915_GT_RING_CTX_TIMESTAMP(0U), 1, I915_GT_RING_FORCE_TO_NONPRIV_ACCESS_RD, 0U, 0, i915_wa_cml, "ctx timestamp RD" },
};

/*
 * The Gen9 non-render engine's rule: the Kaby Lake semaphore wait poll,
 * steppings A0 to before F0 (the revisions 0 to 3 and the C0 respin 5).
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (xcs_engine_wa_init).
 */
static const struct i915_wa_rule i915_wa_engine_gen9_other[] = {
	{ I915_WA_OP_FIELD, I915_GT_RING_SEMA_WAIT_POLL(0U), 1, 0xffffffffU, 1U, 0, i915_wa_kbl_sema_poll, "WaKBLVECSSemaphoreWaitPoll" },
};

/*
 * The Gen9 render engine's rules: per-context preemption control, then the
 * L3 atomics turned off, which can hang the GPU unrecoverably.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (rcs_engine_wa_init: the graphics version 9 entries).
 */
static const struct i915_wa_rule i915_wa_engine_gen9_render[] = {
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN7_FF_SLICE_CS_CHICKEN1, 0, I915_GT_GEN9_FFSC_PERCTX_PREEMPT_CTRL, 0U, 0, NULL, "perctx preempt ctrl" },
	{ I915_WA_OP_OR, I915_GT_GEN8_GARBCNTL, 0, I915_GT_GEN9_GAPS_TSV_CREDIT_DISABLE, 0U, 0, NULL, "WaEnableGapsTsvCreditFix" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN9_CSFE_CHICKEN1_RCS, 0, I915_GT_GEN9_PREEMPT_GPGPU_SYNC_SWITCH_DISABLE, 0U, 0, NULL, "WaContextSwitchWithConcurrentTLBInvalidate" },
	{ I915_WA_OP_OR, I915_GT_BDW_SCRATCH1, 0, I915_GT_GEN9_LBS_SLA_RETRY_TIMER_DECREMENT_ENABLE, 0U, 1, NULL, "WaEnableLbsSlaRetryTimerDecrement" },
	{ I915_WA_OP_OR, I915_GT_GEN8_L3SQCREG4, 0, I915_GT_GEN8_LQSC_FLUSH_COHERENT_LINES, 0U, 1, NULL, "WaOCLCoherentLineFlush" },
	{ I915_WA_OP_FIELD, I915_GT_GEN9_SCRATCH_LNCF1, 0, I915_GT_GEN9_LNCF_NONIA_COHERENT_ATOMICS_ENABLE, 0U, 0, NULL, "L3 atomics off (LNCF1)" },
	{ I915_WA_OP_FIELD, I915_GT_GEN8_L3SQCREG4, 0, I915_GT_GEN8_LQSQ_NONIA_COHERENT_ATOMICS_ENABLE, 0U, 1, NULL, "L3 atomics off (L3SQCREG4)" },
	{ I915_WA_OP_FIELD, I915_GT_BDW_SCRATCH1, 0, I915_GT_EVICTION_PERF_FIX_ENABLE, 0U, 1, NULL, "L3 atomics off (SCRATCH1)" },
};

/*
 * The Gen9 render engine's context rules.  The partial instruction
 * shootdown entry also clears the flow-control save, the YV12 entry also
 * enables GPGPU preemption, and the 4x4 STC entry also disables the partial
 * resolve.  The IZ hashing balance of parts with exactly one 7-EU subslice
 * is not done.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (gen9_ctx_workarounds_init, kbl_ctx_workarounds_init,
 * cfl_ctx_workarounds_init).
 */
static const struct i915_wa_rule i915_wa_context_gen9_render[] = {
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN9_COMMON_SLICE_CHICKEN2, 0, I915_GT_GEN9_PBE_COMPRESSED_HASH_SELECTION, 0U, 0, NULL, "WaCompressedResourceSamplerPbeMediaNewHashMode" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN9_HALF_SLICE_CHICKEN7, 0, I915_GT_GEN9_SAMPLER_HASH_COMPRESSED_READ_ADDR, 0U, 1, NULL, "WaCompressedResourceSamplerPbeMediaNewHashMode" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN8_ROW_CHICKEN, 0, I915_GT_FLOW_CONTROL_ENABLE | I915_GT_PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE, 0U, 1, NULL, "WaDisablePartialInstShootdown" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN9_HALF_SLICE_CHICKEN7, 0, I915_GT_GEN9_ENABLE_YV12_BUGFIX | I915_GT_GEN9_ENABLE_GPGPU_PREEMPTION, 0U, 1, NULL, "WaEnableSamplerGPGPUPreemptionSupport" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_CACHE_MODE_1, 0, I915_GT_GEN8_4x4_STC_OPTIMIZATION_DISABLE | I915_GT_GEN9_PARTIAL_RESOLVE_IN_VC_DISABLE, 0U, 0, NULL, "WaDisablePartialResolveInVc" },
	{ I915_WA_OP_MASKED_FIELD, I915_GT_GEN9_HALF_SLICE_CHICKEN5, 0, I915_GT_GEN9_CCS_TLB_PREFETCH_ENABLE, 0U, 1, NULL, "WaCcsTlbPrefetchDisable" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_HDC_CHICKEN0, 0, I915_GT_HDC_FORCE_CONTEXT_SAVE_RESTORE_NON_COHERENT | I915_GT_HDC_FORCE_CSR_NON_COHERENT_OVR_DISABLE, 0U, 0, NULL, "WaForceContextSaveRestoreNonCoherent" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_HDC_CHICKEN0, 0, I915_GT_HDC_FORCE_NON_COHERENT, 0U, 0, NULL, "WaForceEnableNonCoherent" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN8_HALF_SLICE_CHICKEN3, 0, I915_GT_GEN8_SAMPLER_POWER_BYPASS_DIS, 0U, 1, NULL, "WaDisableSamplerPowerBypassForSOPingPong" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_HALF_SLICE_CHICKEN2, 0, I915_GT_GEN8_ST_PO_DISABLE, 0U, 1, NULL, "WaDisableSTUnitPowerOptimization" },
	{ I915_WA_OP_MASKED_FIELD, I915_GT_GEN8_CS_CHICKEN1, 0, I915_GT_GEN9_PREEMPT_3D_OBJECT_LEVEL, 0U, 0, NULL, "WaDisable3DMidCmdPreemption" },
	{ I915_WA_OP_MASKED_FIELD, I915_GT_GEN8_CS_CHICKEN1, 0, I915_GT_GEN9_PREEMPT_GPGPU_LEVEL_MASK, I915_GT_GEN9_PREEMPT_GPGPU_COMMAND_LEVEL, 0, NULL, "WaDisableGPGPUMidCmdPreemption" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN9_COMMON_SLICE_CHICKEN2, 0, I915_GT_GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION, 0U, 0, i915_wa_push_const_fix, "WaToEnableHwFixForPushConstHWBug" },
	{ I915_WA_OP_MASKED_BITS, I915_GT_GEN8_HALF_SLICE_CHICKEN1, 0, I915_GT_GEN7_SBE_SS_CACHE_DISPATCH_PORT_SHARING_DISABLE, 0U, 1, i915_wa_not_skl, "WaDisableSbeCacheDispatchPortSharing" },
};

/*
 * The Gen9 render engine's whitelist: the preemption, HDC and
 * push-constant chickens, L3SQCREG4 on Skylake and Kaby Lake, and the four
 * depth and invocation counters on Coffee Lake and Comet Lake.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_workarounds.c
 * (gen9_whitelist_build, skl_whitelist_build, kbl_whitelist_build,
 * cfl_whitelist_build).
 */
static const struct i915_wa_rule i915_wa_whitelist_gen9_render[] = {
	{ I915_WA_OP_WHITELIST, I915_GT_GEN9_CTX_PREEMPT_REG, 0, 0U, 0U, 0, NULL, "WaVFEStateAfterPipeControlwithMediaStateClear" },
	{ I915_WA_OP_WHITELIST, I915_GT_GEN8_CS_CHICKEN1, 0, 0U, 0U, 0, NULL, "WaEnablePreemptionGranularityControlByUMD" },
	{ I915_WA_OP_WHITELIST, I915_GT_GEN8_HDC_CHICKEN1, 0, 0U, 0U, 0, NULL, "WaAllowUMDToModifyHDCChicken1" },
	{ I915_WA_OP_WHITELIST, I915_GT_GEN9_COMMON_SLICE_CHICKEN2, 0, 0U, 0U, 0, NULL, "WaSendPushConstantsFromMMIO" },
	{ I915_WA_OP_WHITELIST, I915_GT_GEN8_L3SQCREG4, 0, 0U, 0U, 0, i915_wa_skl_kbl, "WaDisableLSQCROPERFforOCL" },
	{ I915_WA_OP_WHITELIST, I915_GT_PS_INVOCATION_COUNT, 0, I915_GT_RING_FORCE_TO_NONPRIV_ACCESS_RD | I915_GT_RING_FORCE_TO_NONPRIV_RANGE_4, 0U, 0, i915_wa_not_skl_kbl, "WaAllowPMDepthAndInvocationCountAccessFromUMD" },
};

static void i915_wa_add_entry(struct i915_wa_list *wal, uint32_t reg, uint32_t clr, uint32_t set, uint32_t read_mask, int kind, int is_mcr, const char *name);
static void i915_wa_list_reset(struct i915_wa_list *wal, const char *name);
static void i915_wa_apply_result_reset(struct i915_wa_apply_result *result);
static void i915_wa_add_rules(struct i915_wa_list *wal, const struct i915_wa_rule *rules, unsigned count, const struct i915_gt_info *gt, uint32_t base);
static unsigned i915_wa_lowest_bit(uint32_t mask, unsigned limit);
static void i915_wa_steer_gen12(struct i915_wa_list *wal, const struct i915_gt_info *gt);
static void i915_wa_iecp_clock_gating(struct i915_wa_list *wal, const struct i915_gt_info *gt);
static void i915_wa_whitelist_add(struct i915_wa_list *wal, uint32_t reg, uint32_t flags, const char *name);
static void i915_wa_steer_gen9(struct i915_wa_list *wal, const struct i915_gt_info *gt);
static void i915_wa_build_engine_gen9(struct i915_wa_list *wal, const struct i915_gt_info *gt, const struct i915_engine_info *engine);
static void i915_wa_build_context_gen9(struct i915_wa_list *wal, const struct i915_gt_info *gt, const struct i915_engine_info *engine);
static void i915_wa_build_whitelist_gen9(struct i915_wa_list *wal, const struct i915_gt_info *gt, const struct i915_engine_info *engine);
static void i915_gen9_gtt_write_workarounds(struct i915_mmio *mmio);
static void i915_wa_build_engine_gen11(struct i915_wa_list *wal, const struct i915_gt_info *gt, const struct i915_engine_info *engine);
static void i915_wa_build_context_gen11(struct i915_wa_list *wal, const struct i915_gt_info *gt, const struct i915_engine_info *engine);
static void i915_wa_build_whitelist_gen11(struct i915_wa_list *wal, const struct i915_gt_info *gt, const struct i915_engine_info *engine);
static void i915_gen11_gtt_write_workarounds(struct i915_mmio *mmio);
static void i915_mocs_fill(struct i915_mocs *table, const struct i915_mocs_row *rows, unsigned row_count);

/* Adds a workaround that sets bits and verifies them. */
void
drv_i915_wa_add_or(
	struct i915_wa_list *wal,
	uint32_t reg,
	uint32_t set,
	int is_mcr,
	const char *name)
{
	/* Clears and verifies exactly the bits it sets. */
	i915_wa_add_entry(wal, reg, set, set, set, I915_WA_PLAIN, is_mcr, name);
}

/* Adds a workaround that clears a field and sets a value inside it. */
void
drv_i915_wa_add_field(
	struct i915_wa_list *wal,
	uint32_t reg,
	uint32_t clr,
	uint32_t set,
	int is_mcr,
	const char *name)
{
	/* Verifies both the cleared field and the set value. */
	i915_wa_add_entry(wal, reg, clr, set, clr | set, I915_WA_PLAIN, is_mcr, name);
}

/*
 * Adds a workaround that enables bits of a masked register: the entry
 * clears nothing and sets the bits with their mask in the upper half; the
 * verify pass compares the low half.
 */
void
drv_i915_wa_add_masked_bits(
	struct i915_wa_list *wal,
	uint32_t reg,
	uint32_t bits,
	int is_mcr,
	const char *name)
{
	/* Writes the bits under their own mask and verifies them. */
	i915_wa_add_entry(wal, reg, 0U, (bits << 16) | bits, bits, I915_WA_MASKED, is_mcr, name);
}

/* Adds a workaround that sets a field of a masked register. */
void
drv_i915_wa_add_masked_field(
	struct i915_wa_list *wal,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	int is_mcr,
	const char *name)
{
	/* Writes the value under the field mask and verifies the field. */
	i915_wa_add_entry(wal, reg, 0U, (mask << 16) | value, mask, I915_WA_MASKED, is_mcr, name);
}

/* Adds a workaround whose readback is not trusted: it is written and not compared. */
void
drv_i915_wa_add_no_verify(
	struct i915_wa_list *wal,
	uint32_t reg,
	uint32_t clr,
	uint32_t set,
	int is_mcr,
	const char *name)
{
	/* Writes the value and leaves nothing to compare. */
	i915_wa_add_entry(wal, reg, clr, set, 0U, I915_WA_NO_VERIFY, is_mcr, name);
}

/*
 * Builds the Gen12 GT workaround list.  The list needs the GT state: the
 * steering picks the lowest subslice that is not fused off, and the IECP
 * entry walks the video decode engines.
 */
void
drv_i915_wa_build_gt_gen12(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt)
{
	/* Starts the GT list empty. */
	i915_wa_list_reset(wal, "gt_wa");

	/* The steering, the video engines' entries, then the rules. */
	i915_wa_steer_gen12(wal, gt);
	i915_wa_iecp_clock_gating(wal, gt);
	i915_wa_add_rules(wal, i915_wa_gt_gen12, I915_WA_COUNT(i915_wa_gt_gen12), gt, 0U);
}

/*
 * Builds the Ice Lake GT workaround list: the steering at the lowest
 * enabled subslice, as on Gen12, then the Ice Lake rules.
 */
void
drv_i915_wa_build_gt_gen11(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt)
{
	/* Starts the GT list empty. */
	i915_wa_list_reset(wal, "gt_wa");

	/* The steering, then the rules. */
	i915_wa_steer_gen12(wal, gt);
	i915_wa_add_rules(wal, i915_wa_gt_gen11, I915_WA_COUNT(i915_wa_gt_gen11), gt, 0U);
}

/*
 * Builds the Gen9 GT workaround list: the steering at the first enabled
 * slice and subslice, then the rules of the platform and stepping.  Every
 * part has an LLC.
 */
void
drv_i915_wa_build_gt_gen9(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt)
{
	/* Starts the GT list empty. */
	i915_wa_list_reset(wal, "gt_wa");

	/* The steering (WaProgramMgsrForCorrectSliceSpecificMmioReads), then the rules. */
	i915_wa_steer_gen9(wal, gt);
	i915_wa_add_rules(wal, i915_wa_gt_gen9, I915_WA_COUNT(i915_wa_gt_gen9), gt, 0U);
}

/*
 * Builds the workaround list of one engine.
 *
 * Every engine names the default MOCS entry of its command streamer in
 * RING_CMD_CCTL; with no L3 CCS read both halves take the uncached index.
 * The render engine adds its rules and the per-context preemption control.
 */
void
drv_i915_wa_build_engine(
	struct i915_wa_list *wal,
	const struct i915_engine_info *engine,
	const struct i915_gt_info *gt,
	unsigned mocs_uc_index)
{
	/* Starts the engine list empty, named after the engine. */
	i915_wa_list_reset(wal, engine->name);

	/* Graphics versions before 4 have no engine workarounds. */
	if (gt->graphics_ver < 4)
		return;

	/* The default MOCS entry of the command streamer. */
	drv_i915_wa_add_masked_field(
		wal,
		I915_GT_RING_CMD_CCTL(engine->mmio_base),
		I915_GT_CMD_CCTL_MOCS_MASK,
		I915_GT_CMD_CCTL_MOCS_OVERRIDE(mocs_uc_index, mocs_uc_index),
		0,
		"fake: RING_CMD_CCTL MOCS");

	/* A non-render engine gets nothing more. */
	if (engine->class != I915_RENDER_CLASS)
		return;

	/* The render engine's rules. */
	i915_wa_add_rules(wal, i915_wa_engine_gen12_render, I915_WA_COUNT(i915_wa_engine_gen12_render), gt, engine->mmio_base);

	/* Graphics version 9 and later: per-context preemption control. */
	if (gt->graphics_ver >= 9)
		drv_i915_wa_add_masked_bits(wal, I915_GT_GEN7_FF_SLICE_CS_CHICKEN1, I915_GT_GEN9_FFSC_PERCTX_PREEMPT_CTRL, 0, "perctx preempt ctrl");
}

/*
 * Builds the context workaround list of one engine: the workarounds
 * applied inside every context by MI_LOAD_REGISTER_IMM.
 *
 * On graphics version 12 the copy engine's BLIT_CCTL names the uncached
 * MOCS entry for both halves; the render engine adds its rules.
 */
void
drv_i915_wa_build_context(
	struct i915_wa_list *wal,
	const struct i915_engine_info *engine,
	int graphics_ver,
	unsigned mocs_uc_index)
{
	/* Starts the context list empty. */
	i915_wa_list_reset(wal, "ctx_wa");

	/* The copy engine's MOCS. */
	if (graphics_ver >= 12 && engine->class == I915_COPY_ENGINE_CLASS) {
		drv_i915_wa_add_field(
			wal,
			I915_GT_BLIT_CCTL(engine->mmio_base),
			I915_GT_BLIT_CCTL_MASK,
			I915_GT_BLIT_CCTL_MOCS(mocs_uc_index, mocs_uc_index),
			0,
			"Gen12 context MOCS: BLIT_CCTL");
	}

	/* The render engine's rules. */
	if (engine->class == I915_RENDER_CLASS)
		i915_wa_add_rules(wal, i915_wa_context_gen12_render, I915_WA_COUNT(i915_wa_context_gen12_render), NULL, engine->mmio_base);
}

/* Builds the register whitelist of one engine; only graphics version 12 has one. */
void
drv_i915_wa_build_whitelist(
	struct i915_wa_list *wal,
	const struct i915_engine_info *engine,
	int graphics_ver)
{
	/* Starts the whitelist empty. */
	i915_wa_list_reset(wal, "whitelist");

	/* Only the graphics version 12 whitelist is built. */
	if (graphics_ver != 12)
		return;

	/* A non-render engine whitelists only its context timestamp; the render engine its rules. */
	if (engine->class != I915_RENDER_CLASS) {
		i915_wa_add_rules(wal, i915_wa_whitelist_timestamp, I915_WA_COUNT(i915_wa_whitelist_timestamp), NULL, engine->mmio_base);
	} else {
		i915_wa_add_rules(wal, i915_wa_whitelist_gen12_render, I915_WA_COUNT(i915_wa_whitelist_gen12_render), NULL, engine->mmio_base);
	}
}

/*
 * Programs one workaround list into the hardware, and reads it back.
 *
 * A masked entry is always written, since its value
 * carries its own mask.  A plain entry is read, modified and written, and is
 * written only when the value changes or when there is nothing to clear.
 * With verify set, every entry that can be verified is read back and
 * compared; it always runs as a diagnostic that changes no hardware state.
 */
void
drv_i915_wa_list_apply(
	const struct i915_wa_list *wal,
	struct i915_mmio *mmio,
	int verify,
	struct i915_wa_apply_result *result)
{
	const struct i915_wa *entry;
	const char *entry_name;
	uint32_t old_value;
	uint32_t new_value;
	uint32_t readback;
	uint32_t expected;
	unsigned index;
	int write;

	/* Starts the counts from zero. */
	if (result != NULL)
		i915_wa_apply_result_reset(result);

	/* Applies, and verifies, the entries in list order. */
	for (index = 0U; index < wal->count; index++) {
		entry = &wal->list[index];

		/*
		 * Computes the value to write: a masked entry is its own value, a
		 * plain one merges into what the register holds.
		 */
		old_value = 0U;
		if (entry->kind == I915_WA_MASKED) {
			new_value = entry->set;
		} else {
			/* Only an entry that clears something needs the old value. */
			if (entry->clr != 0U)
				old_value = drv_i915_read32(mmio, entry->reg);

			new_value = (old_value & ~entry->clr) | entry->set;
		}

		/* Decides whether the register has to be written. */
		write = 0;
		if (entry->kind == I915_WA_MASKED) {
			/* A masked value is always written. */
			write = 1;
		} else if (new_value != old_value) {
			/* The value changes. */
			write = 1;
		} else if (entry->clr == 0U) {
			/* Nothing was read, so nothing proves the write unnecessary. */
			write = 1;
		}

		/* Writes the register, or counts it as already right. */
		if (write != 0) {
			drv_i915_write32(mmio, entry->reg, new_value);

			/* Counts the write. */
			if (result != NULL)
				result->written++;
		} else if (result != NULL) {
			result->skipped_unchanged++;
		}

		/* Without verification the entry is done. */
		if (verify == 0)
			continue;

		/* An entry with a read mask of 0 has a readback that is not trusted. */
		if (entry->read_mask == 0U) {
			/* Counts the entry as one that could not be verified. */
			if (result != NULL)
				result->not_verifiable++;

			continue;
		}

		/* Reads the register back; a masked entry is compared in its low half. */
		readback = drv_i915_read32(mmio, entry->reg);
		expected = entry->set;
		if (entry->kind == I915_WA_MASKED)
			expected = entry->set & 0xffffU;

		/* Reports a workaround the hardware did not keep. */
		if ((readback & entry->read_mask) != (expected & entry->read_mask)) {
			/* Names the entry, or marks it unnamed. */
			entry_name = "?";
			if (entry->name != NULL)
				entry_name = entry->name;

			kern_logf("i915: WA MISMATCH %s reg=0x%05x expected 0x%08x got 0x%08x (mask 0x%08x)\n",
			    entry_name,
			    entry->reg,
			    expected,
			    readback,
			    entry->read_mask);

			/* Counts the lost workaround. */
			if (result != NULL)
				result->mismatched++;
		} else if (result != NULL) {
			result->verified++;
		}
	}
}

/*
 * Programs the register whitelist of one engine.
 *
 * Each entry fills one
 * I915_GT_RING_FORCE_TO_NONPRIV slot, and every remaining slot is pointed at
 * I915_GT_RING_NOPID so no garbage stays whitelisted.
 */
void
drv_i915_engine_apply_whitelist(
	const struct i915_wa_list *wal,
	const struct i915_engine_info *engine,
	struct i915_mmio *mmio)
{
	unsigned slot;

	/* An empty whitelist leaves the slots as they are. */
	if (wal->count == 0U)
		return;

	/* Writes each whitelisted address with its access flags into a slot. */
	for (slot = 0U; slot < wal->count && slot < I915_GT_RING_MAX_NONPRIV_SLOTS; slot++) {
		drv_i915_write32(
			mmio,
			I915_GT_RING_FORCE_TO_NONPRIV(engine->mmio_base, slot),
			wal->list[slot].reg | wal->list[slot].set);
	}

	/* Points the slots that are left at a harmless register. */
	for (;
	     slot < I915_GT_RING_MAX_NONPRIV_SLOTS;
	     slot++)
		drv_i915_write32(mmio, I915_GT_RING_FORCE_TO_NONPRIV(engine->mmio_base, slot), I915_GT_RING_NOPID(engine->mmio_base));
}

/*
 * Logs one workaround list in manifest form.
 *
 * The manifest can be compared line by line with a dump of the same lists
 * taken under another driver.
 */
void
drv_i915_wa_list_dump(
	const struct i915_wa_list *wal,
	const char *what)
{
	const struct i915_wa *entry;
	const char *list_name;
	const char *overflow_note;
	const char *entry_name;
	unsigned index;

	/* Names the list and says whether entries were lost to overflow. */
	list_name = "?";
	if (wal->name != NULL)
		list_name = wal->name;
	overflow_note = "";
	if (wal->overflow != 0U)
		overflow_note = " (OVERFLOW)";

	kern_logf("i915: WA-MANIFEST %s '%s': %u entries%s\n",
	    what,
	    list_name,
	    wal->count,
	    overflow_note);

	/* Logs every entry with its masks, kind and name. */
	for (index = 0U; index < wal->count; index++) {
		entry = &wal->list[index];

		/* Names the entry, or leaves the name empty. */
		entry_name = "";
		if (entry->name != NULL)
			entry_name = entry->name;

		kern_logf("i915: WA %s reg=0x%05x clr=0x%08x set=0x%08x rmask=0x%08x kind=%d mcr=%d %s\n",
		    what,
		    entry->reg,
		    entry->clr,
		    entry->set,
		    entry->read_mask,
		    entry->kind,
		    entry->is_mcr,
		    entry_name);
	}
}

/*
 * Builds the MOCS table of the platform.
 *
 * Gen12 takes its table with the uncached index 3 and the unused-entries
 * index 2.  Every index the table
 * does not define takes the values of the unused-entries index.
 */
void
drv_i915_mocs_build(
	struct i915_mocs *table,
	int graphics_ver)
{
	unsigned row_count;
	unsigned index;

	/* Starts from an empty, invalid table. */
	for (index = 0U; index < (unsigned)I915_MOCS_ENTRIES; index++) {
		table->control[index] = 0U;
		table->l3cc[index] = 0U;
	}

	table->n_entries = 0U;
	table->uc_index = 0U;
	table->unused_entries_index = 0U;
	table->valid = 0;
	table->engine_mocs = 0;

	/*
	 * Gen9 takes its table with the uncached index 0 and the
	 * unused-entries index 1, one table per engine.
	 */
	if (graphics_ver == 9) {
		table->n_entries = I915_GT_GEN9_NUM_MOCS_ENTRIES;
		table->uc_index = I915_GT_GEN9_MOCS_UC_INDEX;
		table->unused_entries_index = I915_GT_GEN9_MOCS_UNUSED_ENTRIES_INDEX;
		table->engine_mocs = 1;
		row_count = (unsigned)(sizeof(i915_gen9_mocs_table) / sizeof(i915_gen9_mocs_table[0]));
		i915_mocs_fill(table, i915_gen9_mocs_table, row_count);
		return;
	}

	/*
	 * Ice Lake takes its table with the uncached index 0 and the
	 * unused-entries index 1, one table per engine, as Gen9 does.
	 *
	 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_mocs.c
	 * (get_mocs_settings: the GRAPHICS_VER 11 table and indices).
	 */
	if (graphics_ver == 11) {
		table->n_entries = I915_GT_GEN9_NUM_MOCS_ENTRIES;
		table->uc_index = I915_GT_GEN9_MOCS_UC_INDEX;
		table->unused_entries_index = I915_GT_GEN9_MOCS_UNUSED_ENTRIES_INDEX;
		table->engine_mocs = 1;
		row_count = (unsigned)(sizeof(i915_gen11_mocs_table) / sizeof(i915_gen11_mocs_table[0]));
		i915_mocs_fill(table, i915_gen11_mocs_table, row_count);
		return;
	}

	/* Only the graphics version 9, 11 and 12 tables exist in this driver. */
	if (graphics_ver < 12) {
		kern_logf("i915: MOCS: only the gen9, gen11 and gen12 tables are ported\n");
		return;
	}

	/* Takes the size and the two special indices of the Gen12 table. */
	table->n_entries = I915_GT_GEN9_NUM_MOCS_ENTRIES;
	table->uc_index = I915_GT_GEN12_MOCS_UC_INDEX;
	table->unused_entries_index = I915_GT_GEN12_MOCS_UNUSED_ENTRIES_INDEX;

	/* Fills the table from the Gen12 rows. */
	row_count = (unsigned)(sizeof(i915_gen12_mocs_table) / sizeof(i915_gen12_mocs_table[0]));
	i915_mocs_fill(table, i915_gen12_mocs_table, row_count);
}

/*
 * Programs the global MOCS table and the L3 cache control table.
 *
 * Gen12 has global MOCS, so the GLOBAL_MOCS registers are written, then the
 * L3 cache control table.
 */
void
drv_i915_mocs_program(
	const struct i915_mocs *table,
	struct i915_mmio *mmio,
	unsigned *global_writes,
	unsigned *l3cc_writes)
{
	unsigned global_count;
	unsigned l3cc_count;
	unsigned index;

	/* Nothing has been written yet. */
	global_count = 0U;
	l3cc_count = 0U;

	/* A table that was never built is not programmed. */
	if (table->valid == 0)
		return;

	/* Writes every control value into the global MOCS registers; Gen9 has per-engine tables instead. */
	if (table->engine_mocs == 0) {
		for (index = 0U; index < table->n_entries; index++) {
			drv_i915_write32(mmio, I915_GT_GEN12_GLOBAL_MOCS(index), table->control[index]);
			global_count++;
		}
	}

	/* Writes the L3 cache control pairs. */
	drv_i915_mocs_program_l3cc(table, mmio, &l3cc_count);

	/* Reports how many registers of each kind were written. */
	if (global_writes != NULL)
		*global_writes = global_count;
	if (l3cc_writes != NULL)
		*l3cc_writes = l3cc_count;
}

/*
 * Programs the L3 cache control table.
 *
 * Two 16-bit entries are packed per register.  It
 * runs once for the GT and again when the render engine resumes.
 */
void
drv_i915_mocs_program_l3cc(
	const struct i915_mocs *table,
	struct i915_mmio *mmio,
	unsigned *writes)
{
	uint32_t pair;
	unsigned write_count;
	unsigned index;

	/* Nothing has been written yet. */
	write_count = 0U;

	/* A table that was never built is not programmed. */
	if (table->valid == 0)
		return;

	/* Writes each pair of entries into one LNCFCMOCS register. */
	for (index = 0U; index < (table->n_entries + 1U) / 2U; index++) {
		pair = (uint32_t)table->l3cc[2U * index] | ((uint32_t)table->l3cc[2U * index + 1U] << 16);
		drv_i915_write32(mmio, I915_GT_GEN9_LNCFCMOCS(index), pair);
		write_count++;
	}

	/* Reports how many registers were written. */
	if (writes != NULL)
		*writes = write_count;
}

/*
 * Programs one engine's own MOCS table (Gen9 and Ice Lake; Ice Lake's
 * third video decode engine has its table elsewhere).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_mocs.c
 * (mocs_offset).
 *
 * A platform with global MOCS has nothing to write per engine.  The caller
 * holds every forcewake domain.
 */
void
drv_i915_mocs_program_engine(
	const struct i915_mocs *table,
	const struct i915_engine_info *engine,
	struct i915_mmio *mmio,
	unsigned *writes)
{
	uint32_t base;
	unsigned index;
	unsigned count;

	/* Nothing has been written yet. */
	count = 0U;
	if (writes != NULL)
		*writes = 0U;

	/* A table that was never built, or a global one, is not written per engine. */
	if (table->valid == 0 || table->engine_mocs == 0)
		return;

	/* Picks the engine's table. */
	switch (engine->id) {
	case I915_RCS0:
		base = I915_GT_GEN9_RCS0_MOCS0;
		break;
	case I915_BCS0:
		base = I915_GT_GEN9_BCS0_MOCS0;
		break;
	case I915_VCS0:
		base = I915_GT_GEN9_VCS0_MOCS0;
		break;
	case I915_VECS0:
		base = I915_GT_GEN9_VECS0_MOCS0;
		break;
	case I915_VCS2:
		base = I915_GT_GEN11_VCS2_MOCS0;
		break;
	default:
		/* An engine without a table of its own is left alone. */
		return;
	}

	/* Writes every control value into the engine's registers. */
	for (index = 0U; index < table->n_entries; index++) {
		drv_i915_write32(mmio, base + index * 4U, table->control[index]);
		count++;
	}

	/* Reports how many registers were written. */
	if (writes != NULL)
		*writes = count;
}

/*
 * Programs the Gen8 private PAT that Gen9 uses.
 *
 * Index 0 is write-back in the LLC, 1 write-combining, 2 write-back with the
 * eLLC override, 3 uncached, 4 to 7 write-back in LLC and eLLC with the four
 * ages.  The eight one-byte entries fill the low and high registers.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.c
 * (bdw_setup_private_ppat).
 */
void
drv_i915_pat_program_gen9(
	struct i915_mmio *mmio,
	unsigned *writes)
{
	uint32_t low;
	uint32_t high;

	/* Entries 0 to 3. */
	low = (I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_LLC) << 0;
	low |= (I915_GT_GEN8_PPAT_WC | I915_GT_GEN8_PPAT_LLCELLC) << 8;
	low |= (I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_ELLC_OVERRIDE) << 16;
	low |= I915_GT_GEN8_PPAT_UC << 24;

	/* Entries 4 to 7. */
	high = (I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_LLCELLC | I915_GT_GEN8_PPAT_AGE(0)) << 0;
	high |= (I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_LLCELLC | I915_GT_GEN8_PPAT_AGE(1)) << 8;
	high |= (I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_LLCELLC | I915_GT_GEN8_PPAT_AGE(2)) << 16;
	high |= (I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_LLCELLC | I915_GT_GEN8_PPAT_AGE(3)) << 24;

	/* Writes both halves. */
	drv_i915_write32(mmio, I915_GT_GEN8_PRIVATE_PAT_LO, low);
	drv_i915_write32(mmio, I915_GT_GEN8_PRIVATE_PAT_HI, high);

	/* Reports that every entry was written. */
	if (writes != NULL)
		*writes = I915_PAT_ENTRIES;
}

/*
 * Programs the Ice Lake private PAT, one index register an entry, with the
 * values the Gen8 PAT of Gen9 takes.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gtt.c
 * (icl_setup_private_ppat).
 */
void
drv_i915_pat_program_gen11(
	struct i915_mmio *mmio,
	unsigned *writes)
{
	/* Write-back in the LLC, write-combining, write-back as the eLLC says, and uncached. */
	drv_i915_write32(mmio, I915_GT_GEN10_PAT_INDEX(0), I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_LLC);
	drv_i915_write32(mmio, I915_GT_GEN10_PAT_INDEX(1), I915_GT_GEN8_PPAT_WC | I915_GT_GEN8_PPAT_LLCELLC);
	drv_i915_write32(mmio, I915_GT_GEN10_PAT_INDEX(2), I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_ELLC_OVERRIDE);
	drv_i915_write32(mmio, I915_GT_GEN10_PAT_INDEX(3), I915_GT_GEN8_PPAT_UC);

	/* Write-back in the LLC and eLLC with the four ages. */
	drv_i915_write32(mmio, I915_GT_GEN10_PAT_INDEX(4), I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_LLCELLC | I915_GT_GEN8_PPAT_AGE(0));
	drv_i915_write32(mmio, I915_GT_GEN10_PAT_INDEX(5), I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_LLCELLC | I915_GT_GEN8_PPAT_AGE(1));
	drv_i915_write32(mmio, I915_GT_GEN10_PAT_INDEX(6), I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_LLCELLC | I915_GT_GEN8_PPAT_AGE(2));
	drv_i915_write32(mmio, I915_GT_GEN10_PAT_INDEX(7), I915_GT_GEN8_PPAT_WB | I915_GT_GEN8_PPAT_LLCELLC | I915_GT_GEN8_PPAT_AGE(3));

	/* Reports that every entry was written. */
	if (writes != NULL)
		*writes = I915_PAT_ENTRIES;
}

/*
 * Programs the Gen12 private PAT.
 *
 * It belongs with the page tables and moves to ppgtt.c when those are
 * integrated.
 */
void
drv_i915_pat_program_gen12(
	struct i915_mmio *mmio,
	unsigned *writes)
{
	unsigned index;

	/* Writes the memory type of every PAT index. */
	for (index = 0U; index < I915_PAT_ENTRIES; index++)
		drv_i915_write32(mmio, I915_GT_GEN12_PAT_INDEX(index), i915_gen12_private_pat[index]);

	/* Reports that every entry was written. */
	if (writes != NULL)
		*writes = I915_PAT_ENTRIES;
}

/*
 * Builds every workaround list, the MOCS table, and the RC6 and RPS state.
 *
 * Nothing is programmed yet; the only hardware accesses are the RC6
 * disable and the frequency and PCODE reads that RPS needs.
 */
void
drv_i915_gt_init_tables(
	struct i915_gt_init *gt_init,
	const struct i915_gt_info *gt,
	int graphics_ver,
	struct mutex *sb_lock,
	struct i915_mmio *mmio)
{
	unsigned index;

	/* Builds the MOCS table first: the engine lists need its uncached index. */
	drv_i915_mocs_build(&gt_init->mocs, graphics_ver);

	/* Builds the GT workaround list of the graphics version. */
	if (graphics_ver == 9) {
		drv_i915_wa_build_gt_gen9(&gt_init->gt_wa, gt);
	} else if (graphics_ver == 11) {
		drv_i915_wa_build_gt_gen11(&gt_init->gt_wa, gt);
	} else {
		drv_i915_wa_build_gt_gen12(&gt_init->gt_wa, gt);
	}

	/* Builds the engine, whitelist and context lists of every engine. */
	for (index = 0U; index < gt->num_engines && index < (unsigned)I915_WA_ENGINES; index++) {
		/* A Gen9 engine takes the Gen9 lists, which depend on the platform and stepping. */
		if (graphics_ver == 9) {
			i915_wa_build_engine_gen9(&gt_init->engine_wa[index], gt, &gt->engines[index]);
			i915_wa_build_whitelist_gen9(&gt_init->whitelist[index], gt, &gt->engines[index]);
			i915_wa_build_context_gen9(&gt_init->ctx_wa[index], gt, &gt->engines[index]);
			continue;
		}

		/* An Ice Lake engine takes the Ice Lake lists. */
		if (graphics_ver == 11) {
			i915_wa_build_engine_gen11(&gt_init->engine_wa[index], gt, &gt->engines[index]);
			i915_wa_build_whitelist_gen11(&gt_init->whitelist[index], gt, &gt->engines[index]);
			i915_wa_build_context_gen11(&gt_init->ctx_wa[index], gt, &gt->engines[index]);
			continue;
		}

		/* A Gen12 engine takes the Gen12 lists. */
		drv_i915_wa_build_engine(&gt_init->engine_wa[index], &gt->engines[index], gt, gt_init->mocs.uc_index);
		drv_i915_wa_build_whitelist(&gt_init->whitelist[index], &gt->engines[index], graphics_ver);
		drv_i915_wa_build_context(&gt_init->ctx_wa[index], &gt->engines[index], graphics_ver, gt_init->mocs.uc_index);
	}

	/* Prepares RC6, then RPS. */
	drv_i915_rc6_init(&gt_init->rc6, mmio);
	drv_i915_rps_init(&gt_init->rps, sb_lock, mmio);

	/*
	 * A Gen9 GT reads RP1 from the capabilities (it has no
	 * I915_GT_GEN10_FREQ_INFO_REC) and keeps its frequency fixed in its first
	 * version.
	 */
	if (graphics_ver == 9)
		drv_i915_rps_init_gen9(&gt_init->rps, mmio);

	/* The tables can now be programmed. */
	gt_init->tables_built = 1;
}

/*
 * Programs the PAT, the GT workarounds and the MOCS tables.
 *
 * The PAT comes first, so it precedes everything that relies on a PAT
 * index; then the GT workarounds, the Gen9 GTT workarounds and MOCS.  The
 * caller holds every forcewake domain.
 */
void
drv_i915_gt_program(
	struct i915_gt_init *gt_init,
	const struct i915_gt_info *gt,
	struct i915_mmio *mmio)
{
	unsigned pat_writes;

	/* Programs the PAT; Gen9 has the Gen8 private PAT, Ice Lake its index registers. */
	pat_writes = 0U;
	if (gt->graphics_ver == 9) {
		drv_i915_pat_program_gen9(mmio, &pat_writes);
	} else if (gt->graphics_ver == 11) {
		drv_i915_pat_program_gen11(mmio, &pat_writes);
	} else {
		drv_i915_pat_program_gen12(mmio, &pat_writes);
	}

	gt_init->pat_programmed = 0;
	if (pat_writes == I915_PAT_ENTRIES)
		gt_init->pat_programmed = 1;

	/* Applies the GT workarounds and verifies them; the readback changes no hardware state. */
	drv_i915_wa_list_apply(&gt_init->gt_wa, mmio, 1, &gt_init->gt_wa_applied);

	/* Programs the Gen9 and Ice Lake GTT workarounds; Gen12 has none. */
	if (gt->graphics_ver == 9)
		i915_gen9_gtt_write_workarounds(mmio);
	if (gt->graphics_ver == 11)
		i915_gen11_gtt_write_workarounds(mmio);

	/* Programs the MOCS tables. */
	drv_i915_mocs_program(&gt_init->mocs, mmio, &gt_init->mocs_global_writes, &gt_init->mocs_l3cc_writes);

	/* The GT now holds its workarounds and MOCS. */
	gt_init->hw_programmed = 1;
}

/*
 * Applies the workarounds and the whitelist of one engine on resume.
 *
 * The engine workarounds, then the whitelist; then the caller resumes the
 * engine.
 */
void
drv_i915_engine_apply_resume_wa(
	struct i915_gt_init *gt_init,
	const struct i915_gt_info *gt,
	unsigned index,
	struct i915_mmio *mmio)
{
	/* An engine the GT does not have is left alone. */
	if (index >= gt->num_engines)
		return;

	/* An engine beyond the lists that were built is left alone. */
	if (index >= (unsigned)I915_WA_ENGINES)
		return;

	/* Applies and verifies the engine workarounds. */
	drv_i915_wa_list_apply(&gt_init->engine_wa[index], mmio, 1, &gt_init->engine_wa_applied[index]);

	/* Programs the engine's whitelist. */
	drv_i915_engine_apply_whitelist(&gt_init->whitelist[index], &gt->engines[index], mmio);

	/*
	 * Counts every non-privileged slot as written and the engine as
	 * resumed.  The slot count is added even for an empty whitelist, which
	 * writes nothing.
	 */
	gt_init->whitelist_writes += I915_GT_RING_MAX_NONPRIV_SLOTS;
	gt_init->engines_resumed++;
}

/* Adds one entry to a list, merging it into an entry for the same register. */
static void
i915_wa_add_entry(
	struct i915_wa_list *wal,
	uint32_t reg,
	uint32_t clr,
	uint32_t set,
	uint32_t read_mask,
	int kind,
	int is_mcr,
	const char *name)
{
	struct i915_wa *entry;
	unsigned index;

	/* Merges into an entry for the same register. */
	for (index = 0U; index < wal->count; index++) {
		entry = &wal->list[index];

		/* Another register is not merged into. */
		if (entry->reg != reg)
			continue;

		/* The same offset with the other multicast flag is another register. */
		if (entry->is_mcr != is_mcr)
			continue;

		/*
		 * Adds the new clear bits, replaces the cleared bits of the value
		 * with the new set bits, and widens the verified bits.  The merged
		 * entry keeps its first name.
		 */
		entry->clr |= clr;
		entry->set = (entry->set & ~clr) | set;
		entry->read_mask |= read_mask;

		/* An entry that cannot be verified makes the merged entry unverifiable. */
		if (kind == I915_WA_NO_VERIFY)
			entry->kind = I915_WA_NO_VERIFY;

		return;
	}

	/* Refuses, and counts, an entry that does not fit. */
	if (wal->count >= (unsigned)I915_WA_MAX) {
		wal->overflow++;
		return;
	}

	/* Appends the entry. */
	entry = &wal->list[wal->count];
	entry->reg = reg;
	entry->clr = clr;
	entry->set = set;
	entry->read_mask = read_mask;
	entry->kind = kind;
	entry->is_mcr = is_mcr;
	entry->name = name;
	wal->count++;
}

/* Empties a list and gives it its name. */
static void
i915_wa_list_reset(
	struct i915_wa_list *wal,
	const char *name)
{
	wal->count = 0U;
	wal->overflow = 0U;
	wal->name = name;
}

/* Zeroes the counts of an apply result. */
static void
i915_wa_apply_result_reset(
	struct i915_wa_apply_result *result)
{
	result->written = 0U;
	result->skipped_unchanged = 0U;
	result->verified = 0U;
	result->mismatched = 0U;
	result->not_verifiable = 0U;
}

/* Adds the rules of a table that apply to the GT; a relative register is added to base. */
static void
i915_wa_add_rules(
	struct i915_wa_list *wal,
	const struct i915_wa_rule *rules,
	unsigned count,
	const struct i915_gt_info *gt,
	uint32_t base)
{
	const struct i915_wa_rule *rule;
	uint32_t reg;
	unsigned index;

	/* Each rule in the table's order. */
	for (index = 0U; index < count; index++) {
		rule = &rules[index];

		/* A rule whose condition does not hold adds nothing. */
		if (rule->applies != NULL && rule->applies(gt) == 0)
			continue;

		/* The register, relative to the engine's base where the rule says so. */
		reg = rule->reg;
		if (rule->relative != 0)
			reg += base;

		/* The entry of the rule's kind. */
		switch (rule->op) {
		case I915_WA_OP_OR:
			drv_i915_wa_add_or(wal, reg, rule->a, rule->mcr, rule->name);
			break;
		case I915_WA_OP_FIELD:
			drv_i915_wa_add_field(wal, reg, rule->a, rule->b, rule->mcr, rule->name);
			break;
		case I915_WA_OP_MASKED_BITS:
			drv_i915_wa_add_masked_bits(wal, reg, rule->a, rule->mcr, rule->name);
			break;
		case I915_WA_OP_MASKED_FIELD:
			drv_i915_wa_add_masked_field(wal, reg, rule->a, rule->b, rule->mcr, rule->name);
			break;
		case I915_WA_OP_NO_VERIFY:
			drv_i915_wa_add_no_verify(wal, reg, rule->a, rule->b, rule->mcr, rule->name);
			break;
		case I915_WA_OP_WHITELIST:
			i915_wa_whitelist_add(wal, reg, rule->a, rule->name);
			break;
		default:
			break;
		}
	}
}

/* Skylake and Kaby Lake. */
static int
i915_wa_skl_kbl(
	const struct i915_gt_info *gt)
{
	/* The two oldest Gen9 platforms. */
	return gt->platform == I915_PLATFORM_SKYLAKE || gt->platform == I915_PLATFORM_KABYLAKE;
}

/* Coffee Lake and Comet Lake. */
static int
i915_wa_not_skl_kbl(
	const struct i915_gt_info *gt)
{
	/* Every Gen9 platform but the two oldest. */
	return i915_wa_skl_kbl(gt) == 0;
}

/* Kaby Lake before stepping C0 (revision 2). */
static int
i915_wa_kbl_before_c0(
	const struct i915_gt_info *gt)
{
	/* The early Kaby Lake steppings. */
	return gt->platform == I915_PLATFORM_KABYLAKE && gt->revision < 2U;
}

/* Every Kaby Lake, Coffee Lake and Comet Lake, and Skylake before stepping H0 (revision 7). */
static int
i915_wa_in_place_decompress(
	const struct i915_gt_info *gt)
{
	/* Skylake only before H0. */
	return gt->platform != I915_PLATFORM_SKYLAKE || gt->revision < 7U;
}

/* Kaby Lake steppings A0 to before F0: the revisions 0 to 3 and the C0 respin 5. */
static int
i915_wa_kbl_sema_poll(
	const struct i915_gt_info *gt)
{
	/* Kaby Lake, without revision 4 and anything after 5. */
	return gt->platform == I915_PLATFORM_KABYLAKE && gt->revision != 4U && gt->revision <= 5U;
}

/* Every Coffee Lake and Comet Lake, and Kaby Lake from stepping C0 (revision 2) on. */
static int
i915_wa_push_const_fix(
	const struct i915_gt_info *gt)
{
	/* Not Skylake, and Kaby Lake only from C0. */
	if (gt->platform == I915_PLATFORM_SKYLAKE)
		return 0;
	return gt->platform != I915_PLATFORM_KABYLAKE || gt->revision >= 2U;
}

/* Every Gen9 platform but Skylake. */
static int
i915_wa_not_skl(
	const struct i915_gt_info *gt)
{
	/* Kaby Lake, Coffee Lake and Comet Lake. */
	return gt->platform != I915_PLATFORM_SKYLAKE;
}

/* Comet Lake. */
static int
i915_wa_cml(
	const struct i915_gt_info *gt)
{
	/* The newest Gen9 platform. */
	return gt->platform == I915_PLATFORM_COMETLAKE;
}

/* The Gen12 platforms whose render engine takes the PSMI rule. */
static int
i915_wa_render_psmi(
	const struct i915_gt_info *gt)
{
	/* The product row says whether the platform takes it. */
	if ((gt->platform_wa & I915_PLATFORM_WA_RENDER_PSMI) == 0U)
		return 0;

	/* Succeeded: the platform takes the rule. */
	return 1;
}

/* Returns the lowest set bit of a mask below limit, or 0 when none is set. */
static unsigned
i915_wa_lowest_bit(
	uint32_t mask,
	unsigned limit)
{
	unsigned bit;

	/* The first set bit. */
	for (bit = 0U; bit < limit; bit++) {
		if ((mask & (1U << bit)) != 0U)
			return bit;
	}

	/* None is set. */
	return 0U;
}

/*
 * Steers multicast reads at the lowest subslice that is not fused off.
 * With render power gating on, forcewake alone powers only the
 * minimum-configuration subslice, so steering at a higher one reads back
 * zeros or garbage.  The slice is always 0 on graphics version 12.
 */
static void
i915_wa_steer_gen12(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt)
{
	unsigned subslice;

	/* The lowest enabled subslice; without GT state it is 0. */
	subslice = 0U;
	if (gt != NULL)
		subslice = i915_wa_lowest_bit(gt->sseu.subslice_mask, 16U);

	/* Adds the steering to the GT list. */
	drv_i915_wa_add_field(
		wal,
		I915_GT_GEN8_MCR_SELECTOR,
		I915_GT_GEN11_MCR_SLICE_MASK | I915_GT_GEN11_MCR_SUBSLICE_MASK,
		I915_GT_GEN11_MCR_SLICE(0) | I915_GT_GEN11_MCR_SUBSLICE(subslice),
		0,
		"MCR steering (icl_wa_init_mcr)");
}

/* Disables the IECP unit clock gating of every even-instance video decode engine (Wa_14011060649). */
static void
i915_wa_iecp_clock_gating(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt)
{
	const struct i915_engine_info *engine;
	unsigned index;

	/* Without GT state there are no engines to walk. */
	if (gt == NULL)
		return;

	/* Each even video decode engine; the odd instances share the unit of the even one. */
	for (index = 0U; index < gt->num_engines; index++) {
		engine = &gt->engines[index];
		if (engine->class != I915_VIDEO_DECODE_CLASS || (engine->instance % 2) != 0)
			continue;

		drv_i915_wa_add_or(wal, I915_GT_VDBOX_CGCTL3F10(engine->mmio_base), I915_GT_IECPUNIT_CLKGATE_DIS, 0, "Wa_14011060649");
	}
}

/*
 * Adds one whitelisted register with its access flags.  The entry's
 * register field holds the address and the flags, the word the apply pass
 * writes into a non-privileged slot; it is not a register to program.
 */
static void
i915_wa_whitelist_add(
	struct i915_wa_list *wal,
	uint32_t reg,
	uint32_t flags,
	const char *name)
{
	struct i915_wa *entry;

	/* Refuses, and counts, an entry that does not fit. */
	if (wal->count >= (unsigned)I915_WA_MAX) {
		wal->overflow++;
		return;
	}

	/* Appends the address and flags; nothing is cleared, set or verified. */
	entry = &wal->list[wal->count];
	entry->reg = reg | flags;
	entry->clr = 0U;
	entry->set = 0U;
	entry->read_mask = 0U;
	entry->kind = I915_WA_PLAIN;
	entry->is_mcr = 0;
	entry->name = name;
	wal->count++;
}

/*
 * Steers slice-specific reads at the first enabled slice and subslice.
 * Gen9 has no multicast default: a read of a slice-specific register goes
 * to the slice and subslice the selector names, and a fused-off one answers
 * with garbage.
 */
static void
i915_wa_steer_gen9(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt)
{
	unsigned slice;
	unsigned subslice;

	/* The lowest enabled slice, and the lowest enabled subslice, which every enabled slice has. */
	slice = i915_wa_lowest_bit(gt->sseu.slice_mask, 3U);
	subslice = i915_wa_lowest_bit(gt->sseu.subslice_mask, 4U);

	/* Adds the steering to the GT list. */
	drv_i915_wa_add_field(
		wal,
		I915_GT_GEN8_MCR_SELECTOR,
		I915_GT_GEN8_MCR_SLICE_MASK | I915_GT_GEN8_MCR_SUBSLICE_MASK,
		I915_GT_GEN8_MCR_SLICE(slice) | I915_GT_GEN8_MCR_SUBSLICE(subslice),
		0,
		"MCR steering (Gen9)");
}

/* Builds the workaround list of one Gen9 engine; Gen9 has no default MOCS in RING_CMD_CCTL. */
static void
i915_wa_build_engine_gen9(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine)
{
	/* Starts the engine list empty, named after the engine. */
	i915_wa_list_reset(wal, engine->name);

	/* The rules of the engine's kind. */
	if (engine->class != I915_RENDER_CLASS) {
		i915_wa_add_rules(wal, i915_wa_engine_gen9_other, I915_WA_COUNT(i915_wa_engine_gen9_other), gt, engine->mmio_base);
	} else {
		i915_wa_add_rules(wal, i915_wa_engine_gen9_render, I915_WA_COUNT(i915_wa_engine_gen9_render), gt, engine->mmio_base);
	}
}

/* Builds the context workaround list of one Gen9 engine; only the render engine has one. */
static void
i915_wa_build_context_gen9(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine)
{
	/* Starts the context list empty. */
	i915_wa_list_reset(wal, "ctx_wa");

	/* The render engine's rules. */
	if (engine->class == I915_RENDER_CLASS)
		i915_wa_add_rules(wal, i915_wa_context_gen9_render, I915_WA_COUNT(i915_wa_context_gen9_render), gt, engine->mmio_base);
}

/* Builds the register whitelist of one Gen9 engine. */
static void
i915_wa_build_whitelist_gen9(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine)
{
	/* Starts the whitelist empty. */
	i915_wa_list_reset(wal, "whitelist");

	/* The rules of the engine's kind. */
	if (engine->class != I915_RENDER_CLASS) {
		i915_wa_add_rules(wal, i915_wa_whitelist_gen9_other, I915_WA_COUNT(i915_wa_whitelist_gen9_other), gt, engine->mmio_base);
	} else {
		i915_wa_add_rules(wal, i915_wa_whitelist_gen9_render, I915_WA_COUNT(i915_wa_whitelist_gen9_render), gt, engine->mmio_base);
	}
}

/*
 * Programs the Gen9 GTT workarounds: more TLB
 * entries for the GPGPU, the 64 KiB page-size field of the PDE, and the GTT
 * cache, which Gen9 may use because it has no 2 MiB pages.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.c
 * (gtt_write_workarounds).
 */
static void
i915_gen9_gtt_write_workarounds(
	struct i915_mmio *mmio)
{
	uint32_t value;

	/* WaIncreaseDefaultTLBEntries:skl,kbl,cfl */
	drv_i915_write32(mmio, I915_GT_GEN8_L3_LRA_1_GPGPU, I915_GT_GEN9_L3_LRA_1_GPGPU_DEFAULT_VALUE_SKL);

	/* Lets the page walker honour the IPS bit of a PDE. */
	value = drv_i915_read32(mmio, I915_GT_GEN8_GAMW_ECO_DEV_RW_IA);
	drv_i915_write32(mmio, I915_GT_GEN8_GAMW_ECO_DEV_RW_IA, value | I915_GT_GAMW_ECO_ENABLE_64K_IPS_FIELD);

	/* WaGttCachingOffByDefault */
	drv_i915_write32(mmio, I915_GT_HSW_GTT_CACHE_EN, I915_GT_GTT_CACHE_EN_ALL);
}

/* Builds the workaround list of one Ice Lake engine; only the render engine has rules. */
static void
i915_wa_build_engine_gen11(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine)
{
	/* Starts the engine list empty, named after the engine. */
	i915_wa_list_reset(wal, engine->name);

	/* The render engine's rules. */
	if (engine->class == I915_RENDER_CLASS)
		i915_wa_add_rules(wal, i915_wa_engine_gen11_render, I915_WA_COUNT(i915_wa_engine_gen11_render), gt, engine->mmio_base);
}

/* Builds the context workaround list of one Ice Lake engine; only the render engine has rules. */
static void
i915_wa_build_context_gen11(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine)
{
	/* Starts the context list empty. */
	i915_wa_list_reset(wal, "ctx_wa");

	/* The render engine's rules. */
	if (engine->class == I915_RENDER_CLASS)
		i915_wa_add_rules(wal, i915_wa_context_gen11_render, I915_WA_COUNT(i915_wa_context_gen11_render), gt, engine->mmio_base);
}

/*
 * Builds the register whitelist of one Ice Lake engine: the render
 * engine's registers, or a non-render engine's context timestamp, followed
 * on a video decode engine by its HuC status registers.
 */
static void
i915_wa_build_whitelist_gen11(
	struct i915_wa_list *wal,
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine)
{
	/* Starts the whitelist empty. */
	i915_wa_list_reset(wal, "whitelist");

	/* The render engine has its own registers. */
	if (engine->class == I915_RENDER_CLASS) {
		i915_wa_add_rules(wal, i915_wa_whitelist_gen11_render, I915_WA_COUNT(i915_wa_whitelist_gen11_render), gt, engine->mmio_base);
		return;
	}

	/* Every other engine opens its context timestamp. */
	i915_wa_add_rules(wal, i915_wa_whitelist_timestamp, I915_WA_COUNT(i915_wa_whitelist_timestamp), gt, engine->mmio_base);

	/* A video decode engine also opens its HuC status registers. */
	if (engine->class == I915_VIDEO_DECODE_CLASS)
		i915_wa_add_rules(wal, i915_wa_whitelist_gen11_video, I915_WA_COUNT(i915_wa_whitelist_gen11_video), gt, engine->mmio_base);
}

/*
 * Programs the Ice Lake GTT workarounds: more TLB entries for the GPGPU,
 * and the GTT cache, which the driver may use because it maps no 2 MiB
 * pages.  The 64 KiB page-size field of the PDE is not needed after
 * graphics version 10.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gtt.c
 * (gtt_write_workarounds: graphics versions 9 to 11).
 */
static void
i915_gen11_gtt_write_workarounds(
	struct i915_mmio *mmio)
{
	/* WaIncreaseDefaultTLBEntries */
	drv_i915_write32(mmio, I915_GT_GEN8_L3_LRA_1_GPGPU, I915_GT_GEN9_L3_LRA_1_GPGPU_DEFAULT_VALUE_SKL);

	/* WaGttCachingOffByDefault */
	drv_i915_write32(mmio, I915_GT_HSW_GTT_CACHE_EN, I915_GT_GTT_CACHE_EN_ALL);
}

/*
 * Fills a MOCS table from its platform rows: every index first takes the
 * values of the unused-entries index, then the rows overwrite theirs.
 */
static void
i915_mocs_fill(
	struct i915_mocs *table,
	const struct i915_mocs_row *rows,
	unsigned row_count)
{
	uint32_t unused_control;
	uint16_t unused_l3cc;
	unsigned index;
	unsigned row;

	/* Finds the values every undefined index takes. */
	unused_control = 0U;
	unused_l3cc = 0U;
	for (row = 0U; row < row_count; row++) {
		if (rows[row].idx == table->unused_entries_index) {
			unused_control = rows[row].control;
			unused_l3cc = rows[row].l3cc;
		}
	}

	/* Fills every index with the unused-entry values first. */
	for (index = 0U; index < (unsigned)I915_MOCS_ENTRIES; index++) {
		table->control[index] = unused_control;
		table->l3cc[index] = unused_l3cc;
	}

	/* Then overwrites the indices the table defines. */
	for (row = 0U; row < row_count; row++) {
		index = rows[row].idx;

		/* A row beyond the table size is ignored. */
		if (index >= (unsigned)I915_MOCS_ENTRIES)
			continue;

		/* Stores the row's control and L3 cache values. */
		table->control[index] = rows[row].control;
		table->l3cc[index] = rows[row].l3cc;
	}

	/* The table can now be programmed. */
	table->valid = 1;
}
