/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Gen9 (Skylake, Kaby Lake, Coffee Lake, Comet Lake) forms of the 3D and
 * compute state where they differ from the Gen12 ones of genxml.h: packet
 * lengths, field positions, the URB packets, the MOCS index, thread limits,
 * the L3 partition, and the instructions of a thread that only ends itself.
 * Everything not here is the same on both and is taken from genxml.h.
 */

#ifndef DRIVERS_GPU_I915_INTEL_GENXML_GEN9_H
#define DRIVERS_GPU_I915_INTEL_GENXML_GEN9_H

/* The graphics version a batch carries to select these forms. */
#define GEN9_GRAPHICS_VER			9U

/*
 * PIPELINE_SELECT: only the selection bits (1:0) are masked in on Gen9, and
 * no media sampler DOP clock gate is set.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml (PIPELINE_SELECT
 * Mask Bits).
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_buffer.c
 * (genX(flush_pipeline_select)).
 */
#define GEN9_PIPELINE_SELECT_MASK		(0x3U << 8)

/*
 * STATE_BASE_ADDRESS has no bindless sampler state base on Gen9: 19 dwords.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml
 * (STATE_BASE_ADDRESS).
 */
#define GEN9_STATE_BASE_ADDRESS_DWORDS		19U

/*
 * The MOCS index of uncached memory: entry 0 of the Gen9 table; index 2,
 * write-back, is the same entry on both versions.  A MOCS field holds the
 * index shifted left by one.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_mocs.c
 * (skl_mocs_table).
 */
#define GEN9_MOCS_UNCACHED_INDEX		0U

/*
 * The URB of Gen9 is allocated per stage with 3DSTATE_URB_VS, _HS, _DS and
 * _GS (two dwords each): dword 1 holds the starting address in 8 KiB chunks
 * (31:25), the entry allocation size in 64-byte units less one (24:16) and
 * the number of entries (15:0).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml (3DSTATE_URB_VS,
 * 3DSTATE_URB_HS, 3DSTATE_URB_DS, 3DSTATE_URB_GS).
 */
#define GEN9_CMD_3DSTATE_URB_VS			0x7830U
#define GEN9_CMD_3DSTATE_URB_HS			0x7831U
#define GEN9_CMD_3DSTATE_URB_DS			0x7832U
#define GEN9_CMD_3DSTATE_URB_GS			0x7833U
#define GEN9_3DSTATE_URB_DWORDS			2U
#define GEN9_URB_START_SHIFT			25U
#define GEN9_URB_ENTRY_SIZE_SHIFT		16U

/*
 * One allocation fits every Gen9 GT: the default L3 partition gives the URB
 * 48 ways, 192 KiB a slice on GT1 (two banks) and more on the others.  The
 * push constants take the first 32 KiB (chunks 0-3), the vertex stage the
 * next 16 chunks (128 KiB), split 10 and 6 with the geometry stage when the
 * draw has one; the hull and domain stages start after them with no
 * entries.  The entries are capped at GT1's limits, the smallest of the
 * family: 928 vertex entries (the GT1 vertex fetch bug) and 256 geometry
 * entries.
 *
 * Values referenced: Mesa 25.0.7 src/intel/dev/intel_device_info.c (the GT1
 * urb.max_entries of the Gen9 devices).
 */
#define GEN9_URB_VS_START_CHUNK			4U
#define GEN9_URB_VS_CHUNKS			16U
#define GEN9_URB_VS_SPLIT_CHUNKS		10U
#define GEN9_URB_GS_CHUNKS			6U
#define GEN9_URB_UNUSED_START_CHUNK		20U
#define GEN9_URB_VS_ENTRIES			928U
#define GEN9_URB_GS_ENTRIES			256U

/*
 * 3DSTATE_VS dword 7: the Maximum Number of Threads less one is bits 31:23
 * (31:22 on Gen12), at most 336 threads.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml (3DSTATE_VS
 * Maximum Number of Threads), src/intel/dev/intel_device_info.c (GFX9_HW_INFO
 * max_vs_threads).
 */
#define GEN9_VS_MAX_THREADS_SHIFT		23U
#define GEN9_MAX_VS_THREADS			336U

/*
 * 3DSTATE_DEPTH_BUFFER: dword 1 the Surface Type (31:29), Depth Write
 * Enable (28), Stencil Write Enable (27), the Surface Format (20:18) and the
 * pitch less one (17:0); dword 4 the Height (31:18) and Width (17:4) less
 * one; dword 5 the MOCS (6:0), the Minimum Array Element (20:10) and the
 * Depth (31:21); dword 7 the QPitch (14:0) and the Render Target View
 * Extent (31:21).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml
 * (3DSTATE_DEPTH_BUFFER).
 */
#define GEN9_DEPTH_STENCIL_WRITE_ENABLE		(1U << 27)
#define GEN9_DEPTH_FORMAT_SHIFT			18U
#define GEN9_DEPTH_WIDTH_SHIFT			4U
#define GEN9_DEPTH_HEIGHT_SHIFT			18U
#define GEN9_DEPTH_MIN_ARRAY_SHIFT		10U
#define GEN9_DEPTH_DEPTH_SHIFT			21U

/*
 * 3DSTATE_STENCIL_BUFFER is five dwords on Gen9: dword 1 Stencil Buffer
 * Enable (31), the MOCS (28:22) and the pitch less one (16:0); dwords 2-3
 * the address; dword 4 the QPitch (14:0).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml
 * (3DSTATE_STENCIL_BUFFER).
 */
#define GEN9_3DSTATE_STENCIL_BUFFER_DWORDS	5U
#define GEN9_STENCIL_BUFFER_ENABLE		(1U << 31)
#define GEN9_STENCIL_MOCS_SHIFT			22U

/*
 * MEDIA_VFE_STATE dword 3 bit 7 Reset Gateway Timer, set below Gen11; the
 * Maximum Number of Threads is 56 a subslice times the subslices, less one.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml (MEDIA_VFE_STATE),
 * src/intel/dev/intel_device_info.c (GFX9_HW_INFO max_cs_threads).
 */
#define GEN9_VFE_RESET_GATEWAY_TIMER		(1U << 7)
#define GEN9_VFE_THREADS_PER_SUBSLICE		56U

/*
 * L3CNTLREG (0x7034): SLM Enable (0), the URB (7:1), RO (17:11), DC (24:18)
 * and All (31:25) allocations, in ways.  Its default is the first Skylake
 * configuration, URB 48 and All 48 with no shared local memory; a kernel
 * with shared local memory needs the one with 32 ways of it, URB 16 and
 * All 48.
 *
 * Values referenced: Intel Skylake PRM IHD-OS-SKL-Vol 7-05.16 (L3CNTLREG - L3
 * Control Register), Mesa 25.0.7 src/intel/common/intel_l3_config.c
 * (chv_l3_configs).
 */
#define GEN9_REG_L3CNTLREG			0x7034U
#define GEN9_L3CNTLREG_DEFAULT			0x60000060U
#define GEN9_L3CNTLREG_SLM			0x60000021U

/*
 * CACHE_MODE_1 (0x7004, masked: bit n + 16 enables writing bit n), written
 * once on Gen9: Partial Resolve Disable In VC (1), Float Blend Optimization
 * Enable (4) and MSC RAW Hazard Avoidance (9).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml (CACHE_MODE_1),
 * src/intel/vulkan/genX_init_state.c (init_render_queue_state).
 */
#define GEN9_REG_CACHE_MODE_1			0x7004U
#define GEN9_CACHE_MODE_1_VALUE \
	(((1U << 1) | (1U << 4) | (1U << 9)) << 16 | (1U << 1) | (1U << 4) | (1U << 9))

/*
 * The SKL PRM (PIPE_CONTROL) wants a null PIPE_CONTROL before one that
 * invalidates the VF cache, and a post-sync write on the invalidating one:
 * Post Sync Operation 1, Write Immediate Data (bits 15:14 of dword 1).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml (PIPE_CONTROL Post
 * Sync Operation).
 */
#define GEN9_PIPE_CONTROL_WRITE_IMMEDIATE	(1U << 14)

/*
 * A thread that only ends itself on Gen9: a SIMD8 render target write of
 * g112-g115 to binding table entry 0 with end of thread, sent to the render
 * cache (SFID 5) as the 128-bit Gen9 instruction:
 *   sendc(8) null<1>UW g112<8,8,1>UD 0x88031400
 *            render MsgDesc: RT write SIMD8 LastRT Surface = 0 mlen 4 rlen 0
 *            { align1 1Q EOT };
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_asm_tool.c (the
 * instruction above, assembled for Skylake).
 */
#define GEN9_EOT_ONLY_DWORD0			0x05600032U
#define GEN9_EOT_ONLY_DWORD1			0x20000240U
#define GEN9_EOT_ONLY_DWORD2			0x068d0e00U
#define GEN9_EOT_ONLY_DWORD3			0x88031400U

#endif /* DRIVERS_GPU_I915_INTEL_GENXML_GEN9_H */
