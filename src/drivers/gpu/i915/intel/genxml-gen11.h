/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Ice Lake (graphics version 11) forms of the 3D and compute state where
 * they differ from both the Gen9 forms of genxml-gen9.h and the Gen12 forms
 * of genxml.h: thread limits, the L3 partition and the registers the state
 * setup of a context writes once.  Where Ice Lake takes the Gen9 or the
 * Gen12 form, the render code says so at the place it chooses.
 */

#ifndef DRIVERS_GPU_I915_INTEL_GENXML_GEN11_H
#define DRIVERS_GPU_I915_INTEL_GENXML_GEN11_H

/* The graphics version a batch carries to select these forms. */
#define GEN11_GRAPHICS_VER			11U

/*
 * The thread limits of Ice Lake: 364 vertex threads (the Maximum Number of
 * Threads of 3DSTATE_VS is bits 31:22 of dword 7, as on Gen12) and 224
 * geometry threads.
 *
 * Values referenced: Mesa 25.0.7 src/intel/dev/intel_device_info.c
 * (GFX11_HW_INFO max_vs_threads, max_gs_threads).
 */
#define GEN11_MAX_VS_THREADS			364U
#define GEN11_MAX_GS_THREADS			224U

/*
 * L3CNTLREG (0x7034) on Ice Lake: the URB (7:1), Error Detection Behavior
 * Control (9), Use Full Ways (10) and All (31:25) allocations.  The one
 * validated configuration gives the URB 32 ways and All 64; shared local
 * memory is not part of the L3 partition from Gen11 on.  The register is
 * saved with the context, which the kernel's context workaround clears to
 * the error detection bit alone, so the state setup writes it.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml (L3CNTLREG),
 * src/intel/common/intel_l3_config.c (icl_l3_configs) and
 * src/intel/vulkan/genX_init_state.c (genX(emit_l3_config): Wa_1406697149).
 */
#define GEN11_REG_L3CNTLREG			0x7034U
#define GEN11_L3CNTLREG_DEFAULT			((32U << 1) | (1U << 9) | (1U << 10) | (64U << 25))

/*
 * The registers the state setup of an Ice Lake context writes once:
 * HALF_SLICE_CHICKEN7's texel offset precision fix (1) and CS_CHICKEN1's
 * replay mode (0) at mid-command-buffer preemption, both masked (bit n + 16
 * enables writing bit n), and TCCNTLREG, written whole, with the partial
 * write merging of the URB (0), colour and depth (1) and L3 data (2) and
 * the TC disabled (3).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (HALF_SLICE_CHICKEN7, TCCNTLREG, CS_CHICKEN1) and
 * src/intel/vulkan/genX_init_state.c (init_render_queue_state: the GFX_VER
 * 11 and >= 11 writes).
 */
#define GEN11_REG_HALF_SLICE_CHICKEN7		0xe194U
#define GEN11_HALF_SLICE_CHICKEN7_VALUE		((1U << 1) << 16 | (1U << 1))
#define GEN11_REG_TCCNTLREG			0xb0a4U
#define GEN11_TCCNTLREG_VALUE			((1U << 0) | (1U << 1) | (1U << 2) | (1U << 3))
#define GEN11_REG_CS_CHICKEN1			0x2580U
#define GEN11_CS_CHICKEN1_VALUE			((1U << 0) << 16)

/*
 * 3DSTATE_CPS, nine dwords, sent with every field zero: no coarse pixel
 * shading.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml (3DSTATE_CPS).
 */
#define GEN11_CMD_3DSTATE_CPS			0x7822U
#define GEN11_3DSTATE_CPS_DWORDS		9U

#endif /* DRIVERS_GPU_I915_INTEL_GENXML_GEN11_H */
