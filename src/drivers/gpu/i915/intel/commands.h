/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU commands: the MI command opcodes and their flags, the blitter
 * commands, PIPE_CONTROL and its flags, and the invalidate and flush
 * selectors of a flush emission.  The registers are in gt-regs.h.
 */

#ifndef DRIVERS_GPU_I915_INTEL_COMMANDS_H
#define DRIVERS_GPU_I915_INTEL_COMMANDS_H

#include "bits.h"

/* The GPU commands. */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (INSTR_CLIENT_SHIFT).
 */
#define I915_CMD_INSTR_CLIENT_SHIFT	29

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (INSTR_MI_CLIENT).
 */
#define I915_CMD_INSTR_MI_CLIENT	0x0

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (INSTR_BC_CLIENT).
 */
#define I915_CMD_INSTR_BC_CLIENT	0x2

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (INSTR_RC_CLIENT).
 */
#define I915_CMD_INSTR_RC_CLIENT	0x3

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (__INSTR).
 */
#define I915_CMD_INSTR(client)	((client) << I915_CMD_INSTR_CLIENT_SHIFT)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_INSTR).
 */
#define I915_CMD_MI_INSTR(opcode, flags)	(I915_CMD_INSTR(I915_CMD_INSTR_MI_CLIENT) | (opcode) << 23 | (flags))

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_NOOP).
 */
#define I915_CMD_MI_NOOP	I915_CMD_MI_INSTR(0, 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_USER_INTERRUPT).
 */
#define I915_CMD_MI_USER_INTERRUPT	I915_CMD_MI_INSTR(0x02, 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_ARB_CHECK).
 */
#define I915_CMD_MI_ARB_CHECK	I915_CMD_MI_INSTR(0x05, 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_ARB_ON_OFF).
 */
#define I915_CMD_MI_ARB_ON_OFF	I915_CMD_MI_INSTR(0x08, 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_ARB_ENABLE).
 */
#define I915_CMD_MI_ARB_ENABLE	(1U << 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_ARB_DISABLE).
 */
#define I915_CMD_MI_ARB_DISABLE	(0U << 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_BATCH_BUFFER_END).
 */
#define I915_CMD_MI_BATCH_BUFFER_END	I915_CMD_MI_INSTR(0x0a, 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_SEMAPHORE_WAIT).
 */
#define I915_CMD_MI_SEMAPHORE_WAIT	I915_CMD_MI_INSTR(0x1c, 2) /* GEN8+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_SEMAPHORE_WAIT_TOKEN).
 */
#define I915_CMD_MI_SEMAPHORE_WAIT_TOKEN	I915_CMD_MI_INSTR(0x1c, 3) /* GEN12+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_SEMAPHORE_POLL).
 */
#define I915_CMD_MI_SEMAPHORE_POLL	(1U << 15)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_SEMAPHORE_SAD_EQ_SDD).
 */
#define I915_CMD_MI_SEMAPHORE_SAD_EQ_SDD	(4U << 12)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_SEMAPHORE_SAD_GTE_SDD).
 */
#define I915_CMD_MI_SEMAPHORE_SAD_GTE_SDD	(1U << 12)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_SEMAPHORE_GLOBAL_GTT).
 */
#define I915_CMD_MI_SEMAPHORE_GLOBAL_GTT	(1U << 22)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_STORE_DWORD_IMM_GEN4).
 */
#define I915_CMD_MI_STORE_DWORD_IMM_GEN4	I915_CMD_MI_INSTR(0x20, 2)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_USE_GGTT).
 */
#define I915_CMD_MI_USE_GGTT	(1U << 22) /* g4x+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_MEM_VIRTUAL).
 */
#define I915_CMD_MI_MEM_VIRTUAL	(1U << 22) /* 945,g33,965 */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_LOAD_REGISTER_IMM).
 */
#define I915_CMD_MI_LOAD_REGISTER_IMM(x)	I915_CMD_MI_INSTR(0x22, 2*(x)-1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_LRI_FORCE_POSTED).
 */
#define I915_CMD_MI_LRI_FORCE_POSTED	(1U << 12)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_LRI_LRM_CS_MMIO).
 */
#define I915_CMD_MI_LRI_LRM_CS_MMIO	I915_INC_BIT(19)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_LRI_MMIO_REMAP_EN).
 */
#define I915_CMD_MI_LRI_MMIO_REMAP_EN	I915_INC_BIT(17)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_STORE_REGISTER_MEM_GEN8).
 */
#define I915_CMD_MI_STORE_REGISTER_MEM_GEN8	I915_CMD_MI_INSTR(0x24, 2)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_SRM_LRM_GLOBAL_GTT).
 */
#define I915_CMD_MI_SRM_LRM_GLOBAL_GTT	(1U << 22)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_FLUSH_DW).
 */
#define I915_CMD_MI_FLUSH_DW	I915_CMD_MI_INSTR(0x26, 1) /* for GEN6 */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_FLUSH_DW_STORE_INDEX).
 */
#define I915_CMD_MI_FLUSH_DW_STORE_INDEX	(1U << 21)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_FLUSH_DW_OP_STOREDW).
 */
#define I915_CMD_MI_FLUSH_DW_OP_STOREDW	(1U << 14)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_FLUSH_DW_USE_GTT).
 */
#define I915_CMD_MI_FLUSH_DW_USE_GTT	(1U << 2)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_INVALIDATE_TLB).
 */
#define I915_CMD_MI_INVALIDATE_TLB	(1U << 18)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_BATCH_BUFFER_START_GEN8).
 */
#define I915_CMD_MI_BATCH_BUFFER_START_GEN8	I915_CMD_MI_INSTR(0x31, 1)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (MI_BATCH_NON_SECURE_I965).
 */
#define I915_CMD_MI_BATCH_NON_SECURE_I965	(1U << 8)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (XY_SRC_COPY_BLT_CMD).
 */
#define I915_CMD_XY_SRC_COPY_BLT_CMD	(2U << 29 | 0x53 << 22)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (XY_COLOR_BLT_CMD).
 */
#define I915_CMD_XY_COLOR_BLT_CMD	(2U << 29 | 0x50 << 22)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (BLT_WRITE_RGBA).
 */
#define I915_CMD_BLT_WRITE_RGBA	(I915_CMD_BLT_WRITE_RGB | I915_CMD_BLT_WRITE_A)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (BLT_WRITE_RGB).
 */
#define I915_CMD_BLT_WRITE_RGB	(1U << 20)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (BLT_WRITE_A).
 */
#define I915_CMD_BLT_WRITE_A	(2U << 20)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (BLT_DEPTH_32).
 */
#define I915_CMD_BLT_DEPTH_32	(3U << 24)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (BLT_ROP_SRC_COPY).
 */
#define I915_CMD_BLT_ROP_SRC_COPY	(0xcc<<16)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (BLT_ROP_COLOR_COPY).
 */
#define I915_CMD_BLT_ROP_COLOR_COPY	(0xf0<<16)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (XY_SRC_COPY_BLT_SRC_TILED).
 */
#define I915_CMD_XY_SRC_COPY_BLT_SRC_TILED	(1U << 15) /* 965+ only */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (XY_SRC_COPY_BLT_DST_TILED).
 */
#define I915_CMD_XY_SRC_COPY_BLT_DST_TILED	(1U << 11) /* 965+ only */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (GFX_OP_PIPE_CONTROL).
 */
#define I915_CMD_GFX_OP_PIPE_CONTROL(len)	((0x3<<29)|(0x3<<27)|(0x2<<24)|((len)-2))

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_CS_STALL).
 */
#define I915_CMD_PIPE_CONTROL_CS_STALL	(1U << 20)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_QW_WRITE).
 */
#define I915_CMD_PIPE_CONTROL_QW_WRITE	(1U << 14)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_GLOBAL_GTT_IVB).
 */
#define I915_CMD_PIPE_CONTROL_GLOBAL_GTT_IVB	(1U << 24) /* gen7+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_FLUSH_ENABLE).
 */
#define I915_CMD_PIPE_CONTROL_FLUSH_ENABLE	(1U << 7) /* gen7+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_TLB_INVALIDATE).
 */
#define I915_CMD_PIPE_CONTROL_TLB_INVALIDATE	(1U << 18)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH).
 */
#define I915_CMD_PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH	(1U << 12) /* gen6+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_DEPTH_CACHE_FLUSH).
 */
#define I915_CMD_PIPE_CONTROL_DEPTH_CACHE_FLUSH	(1U << 0)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_DC_FLUSH_ENABLE).
 */
#define I915_CMD_PIPE_CONTROL_DC_FLUSH_ENABLE	(1U << 5)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_TILE_CACHE_FLUSH).
 */
#define I915_CMD_PIPE_CONTROL_TILE_CACHE_FLUSH	(1U << 28) /* gen11+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_FLUSH_L3).
 */
#define I915_CMD_PIPE_CONTROL_FLUSH_L3	(1U << 27)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_DEPTH_STALL).
 */
#define I915_CMD_PIPE_CONTROL_DEPTH_STALL	(1U << 13)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_STORE_DATA_INDEX).
 */
#define I915_CMD_PIPE_CONTROL_STORE_DATA_INDEX	(1U << 21)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL0_HDC_PIPELINE_FLUSH).
 */
#define I915_CMD_PIPE_CONTROL0_HDC_PIPELINE_FLUSH	I915_INC_BIT(9) /* gen12 */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_COMMAND_CACHE_INVALIDATE).
 */
#define I915_CMD_PIPE_CONTROL_COMMAND_CACHE_INVALIDATE	(1U << 29) /* gen11+ */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE).
 */
#define I915_CMD_PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE	(1U << 11) /* MBZ on ILK */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE).
 */
#define I915_CMD_PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE	(1U << 10) /* GM45+ only */

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_VF_CACHE_INVALIDATE).
 */
#define I915_CMD_PIPE_CONTROL_VF_CACHE_INVALIDATE	(1U << 4)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_CONST_CACHE_INVALIDATE).
 */
#define I915_CMD_PIPE_CONTROL_CONST_CACHE_INVALIDATE	(1U << 3)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gpu_commands.h
 * (PIPE_CONTROL_STATE_CACHE_INVALIDATE).
 */
#define I915_CMD_PIPE_CONTROL_STATE_CACHE_INVALIDATE	(1U << 2)

/* The commands the execution path uses beside the ones above. */

/*
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/gt/intel_gpu_commands.h (MI_SET_PREDICATE,
 * MI_SET_PREDICATE_DISABLE, MI_SEMAPHORE_REGISTER_POLL,
 * MI_LOAD_REGISTER_MEM_GEN8, MI_LOAD_REGISTER_REG, MI_LRR_SOURCE_CS_MMIO,
 * MI_FLUSH_DW_CCS, MI_INVALIDATE_BSD).
 */
#define I915_CMD_MI_SET_PREDICATE	I915_CMD_MI_INSTR(0x01, 0)
#define I915_CMD_MI_SET_PREDICATE_DISABLE	(0U << 0)
#define I915_CMD_MI_SEMAPHORE_REGISTER_POLL	(1U << 16)
#define I915_CMD_MI_LOAD_REGISTER_MEM_GEN8	I915_CMD_MI_INSTR(0x29, 2)
#define I915_CMD_MI_LOAD_REGISTER_REG	I915_CMD_MI_INSTR(0x2A, 1)
#define I915_CMD_MI_LRR_SOURCE_CS_MMIO	I915_INC_BIT(18)
#define I915_CMD_MI_FLUSH_DW_CCS	(1U << 16)
#define I915_CMD_MI_INVALIDATE_BSD	(1U << 7)

/*
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/gt/intel_engine_types.h (EMIT_INVALIDATE, EMIT_FLUSH,
 * EMIT_BARRIER).
 */
#define I915_CMD_EMIT_INVALIDATE	I915_INC_BIT(0)
#define I915_CMD_EMIT_FLUSH	I915_INC_BIT(1)
#define I915_CMD_EMIT_BARRIER	(I915_CMD_EMIT_INVALIDATE | I915_CMD_EMIT_FLUSH)

#endif /* DRIVERS_GPU_I915_INTEL_COMMANDS_H */
