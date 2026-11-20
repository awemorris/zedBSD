/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The command packets and the registers the amdrdna executor writes to
 * draw and dispatch on a GFX10 (RDNA1) graphics queue, and the writer of
 * an indirect buffer of packets.
 *
 * A register is named by its byte offset, as the packets that set it count
 * from the base of its space: the shader registers from 0xb000, the
 * context registers from 0x28000 and the user configuration registers from
 * 0x30000.  Every packet is the type-3 form: a header word with the body's
 * word count less one, the operation and the compute bit, then the body.
 */

#ifndef DRIVERS_GPU_AMDRDNA_VK_AMDRDNA_VK_GFX10_H
#define DRIVERS_GPU_AMDRDNA_VK_AMDRDNA_VK_GFX10_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The packet header, the opcodes and the register spaces' first registers
 are the driver's (amdrdna-hw.h), which the driver's ring commands use
 * too.  What only the executor writes: the draw's generated-index source
 * (2), the end-of-pipe write's destination (memory) and interrupt (none,
 * after the write is confirmed) fields, and the place of an event's index.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/sid.h (EOP_DST_SEL_MEM,
 * EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM, EVENT_INDEX) and
 * src/amd/registers/gfx10.json (the enum VGT_DI_SOURCE_SELECT:
 * DI_SRC_SEL_AUTO_INDEX 2).
 */
#define AMDRDNA_PM4_DRAW_GENERATED_INDICES	2U
#define AMDRDNA_PM4_FENCE_TO_MEMORY		(0U << 16)
#define AMDRDNA_PM4_FENCE_INTERRUPT_AFTER_WRITE	(3U << 24)
#define AMDRDNA_PM4_EVENT_INDEX_SHIFT		8U

/*
 * The events the executor sends: the wait for the pixel and the compute
 * shaders, the flush of the colour units' metadata (index 0), and the
 * end-of-pipe flush and invalidation of the colour and depth caches with
 * a timestamp (index 5); and the field positions of the release's cache
 * actions (GLM, GLV, GL1, GL2 write back and invalidate, sequence).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json (the enum
 * VGT_EVENT_TYPE: CS_PARTIAL_FLUSH 7, PS_PARTIAL_FLUSH 16,
 * CACHE_FLUSH_AND_INV_TS_EVENT 20, FLUSH_AND_INV_CB_META 46),
 * src/amd/registers/pkt3.json (RELEASE_MEM_OP: EVENT_INDEX 11:8, GLM_WB 12,
 * GLM_INV 13, GLV_INV 14, GL1_INV 15, GL2_INV 20, GL2_WB 21, SEQ 23:22;
 * the enum GCR_SEQ: SEQ_FORWARD 1) and src/amd/vulkan/radv_cs.c
 * (gfx10_cs_emit_cache_flush: the partial flushes at index 4, the
 * timestamp events at index 5).
 */
#define AMDRDNA_PM4_EVENT_COMPUTE_DRAINED	7U
#define AMDRDNA_PM4_EVENT_PIXELS_DRAINED	16U
#define AMDRDNA_PM4_EVENT_CACHES_FLUSHED	20U
#define AMDRDNA_PM4_EVENT_COLOUR_META_FLUSHED	46U
#define AMDRDNA_PM4_EVENT_INDEX_DRAIN		4U

/*
 * The flush of the geometry engine (index 0), which GFX10 needs between a
 * draw by a primitive shader and a later draw by a legacy vertex program
 * (and which RADV also sends at the first pipeline of a command buffer).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json (the enum
 * VGT_EVENT_TYPE: VGT_FLUSH 36) and src/amd/vulkan/radv_cs.c
 * (gfx10_cs_emit_cache_flush: VGT_FLUSH at index 0, line 494).
 */
#define AMDRDNA_PM4_EVENT_GEOMETRY_FLUSHED	36U

/*
 * The queries' events: the depth units' sample counts written to memory
 * (index 1, the address after the event word), and the bottom of the pipe
 * (index 5) whose end-of-pipe write is the GPU's timestamp; the
 * end-of-pipe write's data select of the timestamp.  The depth count
 * control (DB_COUNT_CONTROL): the counting disabled (0), perfect counts
 * (1), exact (not conservative) counts (2), the samples' power (6:4), the
 * passing samples counted (8) on the even and the odd slices (24, 28).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json (the enum
 * VGT_EVENT_TYPE: ZPASS_DONE 21, BOTTOM_OF_PIPE_TS 40; DB_COUNT_CONTROL
 * 0x28004: ZPASS_INCREMENT_DISABLE 0, PERFECT_ZPASS_COUNTS 1,
 * DISABLE_CONSERVATIVE_ZPASS_COUNTS 2, SAMPLE_RATE 6:4, ZPASS_ENABLE 11:8,
 * SLICE_EVEN_ENABLE 27:24, SLICE_ODD_ENABLE 31:28), src/amd/common/sid.h
 * (EOP_DATA_SEL_TIMESTAMP 3 in 31:29), src/amd/vulkan/radv_query.c
 * (radv_begin_occlusion_query: EVENT_WRITE ZPASS_DONE EVENT_INDEX(1) with
 * the address; radv_write_timestamp: BOTTOM_OF_PIPE_TS with
 * EOP_DATA_SEL_TIMESTAMP) and src/amd/vulkan/radv_cmd_buffer.c
 * (radv_flush_occlusion_query_state: the counts of GFX7 and later).
 */
#define AMDRDNA_PM4_EVENT_DEPTH_COUNTS		21U
#define AMDRDNA_PM4_EVENT_INDEX_DEPTH_COUNTS	1U
#define AMDRDNA_PM4_EVENT_BOTTOM_OF_PIPE	40U
#define AMDRDNA_PM4_FENCE_DATA_TIMESTAMP	(3U << 29)
#define AMDRDNA_CX_DEPTH_COUNT			0x28004U
#define AMDRDNA_CX_DEPTH_COUNT_DISABLED		(1U << 0)
#define AMDRDNA_CX_DEPTH_COUNT_PERFECT		(1U << 1)
#define AMDRDNA_CX_DEPTH_COUNT_EXACT		(1U << 2)
#define AMDRDNA_CX_DEPTH_COUNT_SAMPLES_SHIFT	4U
#define AMDRDNA_CX_DEPTH_COUNT_PASSED		((1U << 8) | (1U << 24) | (1U << 28))
#define AMDRDNA_PM4_EVENT_INDEX_TIMESTAMP	5U
#define AMDRDNA_PM4_RELEASE_METADATA_WRITE_BACK	(1U << 12)
#define AMDRDNA_PM4_RELEASE_METADATA_INVALIDATE	(1U << 13)
#define AMDRDNA_PM4_RELEASE_VECTOR_INVALIDATE	(1U << 14)
#define AMDRDNA_PM4_RELEASE_LEVEL1_INVALIDATE	(1U << 15)
#define AMDRDNA_PM4_RELEASE_LEVEL2_INVALIDATE	(1U << 20)
#define AMDRDNA_PM4_RELEASE_LEVEL2_WRITE_BACK	(1U << 21)
#define AMDRDNA_PM4_RELEASE_IN_ORDER		(1U << 22)

/*
 * The cache actions of a memory acquire (its last word): the instruction
 * cache invalidated (bits 1:0 = 1), the metadata cache written back and
 * invalidated (4, 5), the scalar cache invalidated (7), the vector cache
 * (8) and the per-array cache (9) invalidated, the level-2 cache
 * invalidated (14) and written back (15).  Its fixed words (the whole
 * address space, the poll interval) are the driver's.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/pkt3.json (GCR_CNTL:
 * GLI_INV 1:0, GLM_WB 4, GLM_INV 5, GLK_INV 7, GLV_INV 8, GL1_INV 9,
 * GL2_INV 14, GL2_WB 15; the enum GCR_GLI_INV: GLI_ALL 1) and
 * src/amd/vulkan/radv_cs.c (gfx10_cs_emit_cache_flush: the acquire's
 * words 0, 0xffffffff, 0xffffff, 0, 0, 0xa, then the controls).
 */
#define AMDRDNA_PM4_ACQUIRE_INSTRUCTIONS	(1U << 0)
#define AMDRDNA_PM4_ACQUIRE_METADATA_WRITE_BACK	(1U << 4)
#define AMDRDNA_PM4_ACQUIRE_METADATA_INVALIDATE	(1U << 5)
#define AMDRDNA_PM4_ACQUIRE_SCALAR		(1U << 7)
#define AMDRDNA_PM4_ACQUIRE_VECTOR		(1U << 8)
#define AMDRDNA_PM4_ACQUIRE_LEVEL1		(1U << 9)
#define AMDRDNA_PM4_ACQUIRE_LEVEL2_INVALIDATE	(1U << 14)
#define AMDRDNA_PM4_ACQUIRE_LEVEL2_WRITE_BACK	(1U << 15)

/*
 * The shader registers (bytes from 0): of the pixel program its code
 * address (low, high), its two resource words, its first user SGPR, its
 * CU masks (two) and its request control; of the vertex program the same;
 * of the compute program its code address, resource words, workgroup
 * size, start, first user SGPR, resource limits, dispatch tunnel and the
 * CU masks of its four shader engines.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (SPI_SHADER_PGM_LO_PS 0xb020, SPI_SHADER_PGM_HI_PS 0xb024,
 * SPI_SHADER_PGM_RSRC1_PS 0xb028, SPI_SHADER_PGM_RSRC2_PS 0xb02c,
 * SPI_SHADER_USER_DATA_PS_0 0xb030, SPI_SHADER_PGM_RSRC3_PS 0xb01c,
 * SPI_SHADER_PGM_RSRC4_PS 0xb004, SPI_SHADER_REQ_CTRL_PS 0xb0c0,
 * SPI_SHADER_PGM_LO_VS 0xb120, SPI_SHADER_PGM_HI_VS 0xb124,
 * SPI_SHADER_PGM_RSRC1_VS 0xb128, SPI_SHADER_PGM_RSRC2_VS 0xb12c,
 * SPI_SHADER_USER_DATA_VS_0 0xb130, SPI_SHADER_PGM_RSRC3_VS 0xb118,
 * SPI_SHADER_PGM_RSRC4_VS 0xb104, SPI_SHADER_REQ_CTRL_VS 0xb1c0,
 * COMPUTE_PGM_LO 0xb830, COMPUTE_PGM_HI 0xb834, COMPUTE_PGM_RSRC1 0xb848,
 * COMPUTE_PGM_RSRC2 0xb84c, COMPUTE_PGM_RSRC3 0xb8a0,
 * COMPUTE_NUM_THREAD_X 0xb81c, COMPUTE_START_X 0xb810,
 * COMPUTE_USER_DATA_0 0xb900, COMPUTE_RESOURCE_LIMITS 0xb854,
 * COMPUTE_DISPATCH_TUNNEL 0xb9f4) and src/amd/registers/gfx9.json
 * (COMPUTE_STATIC_THREAD_MGMT_SE0 0xb858, SE1 0xb85c, SE2 0xb864, SE3
 * 0xb868, which src/amd/common/ac_cmdbuf.c sets on GFX10 too).
 */
#define AMDRDNA_SH_PIXEL_CODE_LOW		0xb020U
#define AMDRDNA_SH_PIXEL_CODE_HIGH		0xb024U
#define AMDRDNA_SH_PIXEL_RESOURCES		0xb028U
#define AMDRDNA_SH_PIXEL_INPUTS			0xb02cU
#define AMDRDNA_SH_PIXEL_USER_DATA		0xb030U
#define AMDRDNA_SH_PIXEL_UNITS			0xb01cU
#define AMDRDNA_SH_PIXEL_UNITS_HIGH		0xb004U
#define AMDRDNA_SH_PIXEL_REQUESTS		0xb0c0U
#define AMDRDNA_SH_VERTEX_CODE_LOW		0xb120U
#define AMDRDNA_SH_VERTEX_CODE_HIGH		0xb124U
#define AMDRDNA_SH_VERTEX_RESOURCES		0xb128U
#define AMDRDNA_SH_VERTEX_INPUTS		0xb12cU
#define AMDRDNA_SH_VERTEX_USER_DATA		0xb130U
#define AMDRDNA_SH_VERTEX_UNITS			0xb118U
#define AMDRDNA_SH_VERTEX_UNITS_HIGH		0xb104U
#define AMDRDNA_SH_VERTEX_REQUESTS		0xb1c0U
#define AMDRDNA_SH_KERNEL_CODE_LOW		0xb830U
#define AMDRDNA_SH_KERNEL_CODE_HIGH		0xb834U
#define AMDRDNA_SH_KERNEL_RESOURCES		0xb848U
#define AMDRDNA_SH_KERNEL_INPUTS		0xb84cU
#define AMDRDNA_SH_KERNEL_MORE_RESOURCES	0xb8a0U
#define AMDRDNA_SH_KERNEL_GROUP_SIZE		0xb81cU
#define AMDRDNA_SH_KERNEL_START			0xb810U
#define AMDRDNA_SH_KERNEL_USER_DATA		0xb900U
#define AMDRDNA_SH_KERNEL_LIMITS		0xb854U
#define AMDRDNA_SH_KERNEL_TUNNEL		0xb9f4U
#define AMDRDNA_SH_KERNEL_UNITS_ENGINE0		0xb858U
#define AMDRDNA_SH_KERNEL_UNITS_ENGINE1		0xb85cU
#define AMDRDNA_SH_KERNEL_UNITS_ENGINE2		0xb864U
#define AMDRDNA_SH_KERNEL_UNITS_ENGINE3		0xb868U

/*
 * The shader registers of a primitive shader (bytes from 0): its code
 * address (low, high), its two resource words, the table's address (the
 * stage's user data address, low and high), its first user SGPR (s8), and
 * its CU masks (two, the second with the late allocation).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (SPI_SHADER_PGM_LO_ES 0xb320, SPI_SHADER_PGM_HI_ES 0xb324,
 * SPI_SHADER_PGM_RSRC1_GS 0xb228, SPI_SHADER_PGM_RSRC2_GS 0xb22c,
 * SPI_SHADER_USER_DATA_ADDR_LO_GS 0xb208, SPI_SHADER_USER_DATA_ADDR_HI_GS
 * 0xb20c, SPI_SHADER_USER_DATA_GS_0 0xb230, SPI_SHADER_PGM_RSRC3_GS
 * 0xb21c, SPI_SHADER_PGM_RSRC4_GS 0xb204) and src/amd/vulkan/radv_shader.c
 * (the NGG program's registers on GFX10: the code at
 * R_00B320_SPI_SHADER_PGM_LO_ES, the resources at RSRC1_GS and RSRC2_GS,
 * lines 1763-1772).
 */
#define AMDRDNA_SH_PRIMITIVE_CODE_LOW		0xb320U
#define AMDRDNA_SH_PRIMITIVE_CODE_HIGH		0xb324U
#define AMDRDNA_SH_PRIMITIVE_RESOURCES		0xb228U
#define AMDRDNA_SH_PRIMITIVE_INPUTS		0xb22cU
#define AMDRDNA_SH_PRIMITIVE_TABLE		0xb208U
#define AMDRDNA_SH_PRIMITIVE_USER_DATA		0xb230U
#define AMDRDNA_SH_PRIMITIVE_UNITS		0xb21cU
#define AMDRDNA_SH_PRIMITIVE_UNITS_HIGH		0xb204U

/*
 * The CU masks the programs may run on (all, and a wave limit of 63), the
 * pixel program's request control (soft grouping, four requests per CU),
 * the compute dispatch's start word (the shader, the start at 0, the order
 * mode and wave32), and the code address shifts.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (SPI_SHADER_PGM_RSRC3_PS: CU_EN 15:0, WAVE_LIMIT 21:16;
 * SPI_SHADER_PGM_RSRC4_PS: CU_EN; SPI_SHADER_REQ_CTRL_PS:
 * SOFT_GROUPING_EN 0, NUMBER_OF_REQUESTS_PER_CU 4:1;
 * COMPUTE_DISPATCH_INITIATOR: COMPUTE_SHADER_EN 0, FORCE_START_AT_000 2,
 * ORDER_MODE 6, CS_W32_EN 15; SPI_SHADER_PGM_HI_PS: MEM_BASE 7:0) and
 * src/amd/common/ac_cmdbuf.c (gfx10_init_graphics_preamble_state: the CU
 * masks, the wave limit 0x3f and the request control).
 */
#define AMDRDNA_SH_UNITS_ALL			(0xffffU | (0x3fU << 16))
#define AMDRDNA_SH_UNITS_INDEX			3U
#define AMDRDNA_SH_UNITS_HIGH_ALL		0xffffU
#define AMDRDNA_SH_PIXEL_REQUEST_CONTROL	(1U | (3U << 1))
#define AMDRDNA_SH_DISPATCH_START		(1U | (1U << 2) | (1U << 6) | (1U << 15))
#define AMDRDNA_SH_CODE_LOW_SHIFT		8U
#define AMDRDNA_SH_CODE_HIGH_SHIFT		40U

/*
 * The context registers (bytes from 0x28000 on): the colour target 0 (its
 * address, high address, view, layout, attributes, size and surface), the
 * colour control, write and export masks, the blend of target 0 and its
 * optimisation, the export down-conversion, the depth state, the
 * scissors, the viewport 0, the clip, the rasteriser, the vertex output
 * and the pixel input words, and the stage enable.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (CB_COLOR0_BASE 0x28c60, CB_COLOR0_BASE_EXT 0x28e40, CB_COLOR0_VIEW
 * 0x28c6c, CB_COLOR0_INFO 0x28c70, CB_COLOR0_ATTRIB 0x28c74,
 * CB_COLOR0_ATTRIB2 0x28ec0, CB_COLOR0_ATTRIB3 0x28ee0, CB_COLOR_CONTROL
 * 0x28808, CB_TARGET_MASK 0x28238, CB_SHADER_MASK 0x2823c,
 * CB_BLEND0_CONTROL 0x28780, SX_MRT0_BLEND_OPT 0x28760, SX_PS_DOWNCONVERT
 * 0x28754, SX_BLEND_OPT_EPSILON 0x28758, SX_BLEND_OPT_CONTROL 0x2875c,
 * DB_RENDER_CONTROL 0x28000, DB_Z_INFO 0x28040, DB_STENCIL_INFO 0x28044,
 * DB_DEPTH_CONTROL 0x28800, DB_SHADER_CONTROL 0x2880c,
 * PA_SC_WINDOW_OFFSET 0x28200, PA_SC_WINDOW_SCISSOR_TL 0x28204,
 * PA_SC_WINDOW_SCISSOR_BR 0x28208, PA_SC_CLIPRECT_RULE 0x2820c,
 * PA_SC_SCREEN_SCISSOR_TL 0x28030, PA_SC_SCREEN_SCISSOR_BR 0x28034,
 * PA_SC_GENERIC_SCISSOR_TL 0x28240, PA_SC_GENERIC_SCISSOR_BR 0x28244,
 * PA_SC_VPORT_SCISSOR_0_TL 0x28250, PA_SC_VPORT_SCISSOR_0_BR 0x28254,
 * PA_SC_VPORT_ZMIN_0 0x282d0, PA_SC_VPORT_ZMAX_0 0x282d4,
 * PA_CL_VPORT_XSCALE 0x2843c, PA_CL_CLIP_CNTL 0x28810, PA_SU_SC_MODE_CNTL
 * 0x28814, PA_CL_VTE_CNTL 0x28818, PA_CL_VS_OUT_CNTL 0x2881c,
 * PA_SU_VTX_CNTL 0x28be4, PA_CL_GB_VERT_CLIP_ADJ 0x28be8, PA_SC_MODE_CNTL_0
 * 0x28a48, PA_SC_AA_CONFIG 0x28be0, PA_SC_AA_MASK_X0Y0_X1Y0 0x28c38,
 * PA_SC_BINNER_CNTL_0 0x28c44, PA_SC_EDGERULE 0x28230, PA_SU_POINT_SIZE
 * 0x28a00, PA_SU_POINT_MINMAX 0x28a04, PA_SU_LINE_CNTL 0x28a08,
 * PA_SU_SMALL_PRIM_FILTER_CNTL 0x28830, SPI_PS_INPUT_CNTL_0 0x28644,
 * SPI_VS_OUT_CONFIG 0x286c4, SPI_PS_INPUT_ENA 0x286cc, SPI_PS_INPUT_ADDR
 * 0x286d0, SPI_INTERP_CONTROL_0 0x286d4, SPI_PS_IN_CONTROL 0x286d8,
 * SPI_BARYC_CNTL 0x286e0, SPI_SHADER_POS_FORMAT 0x2870c,
 * SPI_SHADER_Z_FORMAT 0x28710, SPI_SHADER_COL_FORMAT 0x28714,
 * VGT_SHADER_STAGES_EN 0x28b54, VGT_GS_MODE 0x28a40, VGT_PRIMITIVEID_EN
 * 0x28a84, VGT_REUSE_OFF 0x28ab4, VGT_VERTEX_REUSE_BLOCK_CNTL 0x28c58).
 */
#define AMDRDNA_CX_TARGET_ADDRESS		0x28c60U
#define AMDRDNA_CX_TARGET_ADDRESS_HIGH		0x28e40U
#define AMDRDNA_CX_TARGET_VIEW			0x28c6cU
#define AMDRDNA_CX_TARGET_LAYOUT		0x28c70U
#define AMDRDNA_CX_TARGET_ATTRIBUTES		0x28c74U
#define AMDRDNA_CX_TARGET_SIZE			0x28ec0U
#define AMDRDNA_CX_TARGET_SURFACE		0x28ee0U
#define AMDRDNA_CX_COLOUR_CONTROL		0x28808U
#define AMDRDNA_CX_COLOUR_WRITE_MASK		0x28238U
#define AMDRDNA_CX_COLOUR_EXPORT_MASK		0x2823cU
#define AMDRDNA_CX_BLEND			0x28780U
#define AMDRDNA_CX_BLEND_SHORTCUT		0x28760U
#define AMDRDNA_CX_EXPORT_CONVERSION		0x28754U
#define AMDRDNA_CX_BLEND_SHORTCUT_EPSILON	0x28758U
#define AMDRDNA_CX_BLEND_SHORTCUT_CONTROL	0x2875cU
#define AMDRDNA_CX_DEPTH_RENDER			0x28000U

/*
 * The colour targets after the first: each one's registers the stride on
 * from target 0's in the block of BASE .. ATTRIB (CB_COLOR1_BASE 0x28c9c),
 * and four bytes on in each of the runs of BASE_EXT, ATTRIB2 and ATTRIB3
 * (CB_COLOR1_BASE_EXT 0x28e44, CB_COLOR1_ATTRIB2 0x28ec4,
 * CB_COLOR1_ATTRIB3 0x28ee4); each target's blend and its shortcut four
 * bytes on (CB_BLEND1_CONTROL 0x28784, SX_MRT1_BLEND_OPT 0x28764).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json (the
 * registers named) and src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_framebuffer_state: R_028C60_CB_COLOR0_BASE + i * 0x3c,
 * R_028E40_CB_COLOR0_BASE_EXT + i * 4, R_028EC0_CB_COLOR0_ATTRIB2 + i * 4,
 * R_028EE0_CB_COLOR0_ATTRIB3 + i * 4).
 */
#define AMDRDNA_CX_TARGET_STRIDE		0x3cU
#define AMDRDNA_CX_TARGET_RUN_STRIDE		4U
#define AMDRDNA_CX_DEPTH_LAYOUT			0x28040U
#define AMDRDNA_CX_STENCIL_LAYOUT		0x28044U
#define AMDRDNA_CX_DEPTH_CONTROL		0x28800U
#define AMDRDNA_CX_DEPTH_SHADER_CONTROL		0x2880cU
#define AMDRDNA_CX_WINDOW_OFFSET		0x28200U
#define AMDRDNA_CX_WINDOW_SCISSOR		0x28204U
#define AMDRDNA_CX_CLIP_RECTANGLE_RULE		0x2820cU
#define AMDRDNA_CX_SCREEN_SCISSOR		0x28030U
#define AMDRDNA_CX_GENERIC_SCISSOR		0x28240U
#define AMDRDNA_CX_VIEWPORT_SCISSOR		0x28250U
#define AMDRDNA_CX_VIEWPORT_DEPTH_RANGE		0x282d0U
#define AMDRDNA_CX_VIEWPORT_TRANSFORM		0x2843cU
#define AMDRDNA_CX_CLIP_CONTROL			0x28810U
#define AMDRDNA_CX_RASTER_MODE			0x28814U
#define AMDRDNA_CX_VIEWPORT_ENABLE		0x28818U
#define AMDRDNA_CX_VERTEX_OUTPUT_CONTROL	0x2881cU
#define AMDRDNA_CX_VERTEX_ROUNDING		0x28be4U
#define AMDRDNA_CX_GUARD_BAND			0x28be8U
#define AMDRDNA_CX_SCAN_MODE			0x28a48U
#define AMDRDNA_CX_SAMPLE_CONFIG		0x28be0U
#define AMDRDNA_CX_SAMPLE_MASK			0x28c38U
#define AMDRDNA_CX_BINNER_CONTROL		0x28c44U
#define AMDRDNA_CX_EDGE_RULE			0x28230U
#define AMDRDNA_CX_POINT_SIZE			0x28a00U
#define AMDRDNA_CX_POINT_LIMITS			0x28a04U
#define AMDRDNA_CX_LINE_WIDTH			0x28a08U
#define AMDRDNA_CX_SMALL_PRIMITIVE_FILTER	0x28830U
#define AMDRDNA_CX_ATTRIBUTE_CONTROL		0x28644U
#define AMDRDNA_CX_VERTEX_EXPORT_CONFIG		0x286c4U
#define AMDRDNA_CX_PIXEL_INPUT_ENABLE		0x286ccU
#define AMDRDNA_CX_PIXEL_INPUT_ADDRESS		0x286d0U
#define AMDRDNA_CX_INTERPOLATION_CONTROL	0x286d4U
#define AMDRDNA_CX_PIXEL_INPUT_CONTROL		0x286d8U
#define AMDRDNA_CX_BARYCENTRIC_CONTROL		0x286e0U
#define AMDRDNA_CX_POSITION_FORMATS		0x2870cU
#define AMDRDNA_CX_DEPTH_EXPORT_FORMAT		0x28710U
#define AMDRDNA_CX_COLOUR_EXPORT_FORMATS	0x28714U
#define AMDRDNA_CX_STAGES			0x28b54U
#define AMDRDNA_CX_GEOMETRY_MODE		0x28a40U
#define AMDRDNA_CX_PRIMITIVE_ID_ENABLE		0x28a84U
#define AMDRDNA_CX_REUSE_OFF			0x28ab4U
#define AMDRDNA_CX_VERTEX_REUSE			0x28c58U

/*
 * The context registers of a primitive shader: the primitive export's
 * format, the most vertices one subgroup exports, the subgroup's
 * amplification, its vertex and primitive counts, the instancing of a
 * geometry program, and the type of the primitives it exports.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (SPI_SHADER_IDX_FORMAT 0x28708, GE_MAX_OUTPUT_PER_SUBGROUP 0x287fc,
 * GE_NGG_SUBGRP_CNTL 0x28b4c, VGT_GS_ONCHIP_CNTL 0x28a44,
 * VGT_GS_INSTANCE_CNT 0x28b90, VGT_GS_OUT_PRIM_TYPE 0x28a6c) and
 * src/amd/vulkan/radv_cmd_buffer.c (radv_emit_hw_ngg: the registers it
 * sets on GFX10, lines 2081-2196).
 */
#define AMDRDNA_CX_PRIMITIVE_EXPORT_FORMAT	0x28708U
#define AMDRDNA_CX_SUBGROUP_OUTPUT		0x287fcU
#define AMDRDNA_CX_SUBGROUP_CONTROL		0x28b4cU
#define AMDRDNA_CX_SUBGROUP_SIZE		0x28a44U
#define AMDRDNA_CX_GEOMETRY_INSTANCES		0x28b90U
#define AMDRDNA_CX_OUTPUT_PRIMITIVE		0x28a6cU

/*
 * The user configuration registers (bytes from 0x30000 on): the primitive
 * type (set through the indexed setter with index 1), the geometry
 * engine's control and its index limits and offset, the instance base,
 * and the coherency start delay.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (VGT_PRIMITIVE_TYPE 0x30908, GE_CNTL 0x3096c, GE_MIN_VTX_INDX 0x30924,
 * GE_INDX_OFFSET 0x30928, GE_MAX_VTX_INDX 0x30964, VGT_INSTANCE_BASE_ID
 * 0x30968, GE_USER_VGPR_EN 0x30988, CP_COHER_START_DELAY 0x301ec) and
 * src/amd/vulkan/radv_cmd_buffer.c (the primitive type set with index 1).
 */
#define AMDRDNA_UC_PRIMITIVE_TYPE		0x30908U
#define AMDRDNA_UC_PRIMITIVE_TYPE_INDEX		1U
#define AMDRDNA_UC_GEOMETRY_CONTROL		0x3096cU
#define AMDRDNA_UC_INDEX_MINIMUM		0x30924U
#define AMDRDNA_UC_INDEX_OFFSET			0x30928U
#define AMDRDNA_UC_INDEX_MAXIMUM		0x30964U
#define AMDRDNA_UC_INSTANCE_BASE		0x30968U
#define AMDRDNA_UC_USER_VGPRS			0x30988U
#define AMDRDNA_UC_COHERENCY_DELAY		0x301ecU

/*
 * The vertex programs' late allocation (the legacy stage's limit, and the
 * geometry engine's parameter cache oversubscription): none, the cache's
 * line count field all ones as RADV writes it without oversubscription.
 * Written at every start, so nothing another driver left there is used
 * (late allocation on all CUs can deadlock GFX10).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (SPI_SHADER_LATE_ALLOC_VS 0xb11c; GE_PC_ALLOC 0x30980: OVERSUB_EN 0,
 * NUM_PC_LINES 10:1) and src/amd/vulkan/radv_shader.c (ge_pc_alloc with
 * oversub_pc_lines 0: S_030980_NUM_PC_LINES(0 - 1), lines 1457-1463) and
 * src/amd/common/ac_shader_util.c (ac_compute_late_alloc: on GFX10 late
 * allocation needs CUs 2 and 3 masked, a hardware deadlock otherwise).
 */
#define AMDRDNA_SH_VERTEX_LATE_ALLOC		0xb11cU
#define AMDRDNA_UC_PARAMETER_CACHE		0x30980U
#define AMDRDNA_UC_PARAMETER_CACHE_PLAIN	(0x3ffU << 1)

/*
 * A primitive shader's late allocation as RADV sets it on this board: the
 * parameter cache oversubscribed by 256 lines (a quarter of its 1024),
 * the geometry stage's late allocation of 32 waves, and CUs 2 and 3 of
 * each array left out of the geometry stage (GFX10 deadlocks otherwise).
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_shader.c
 * (radv_precompute_registers_hw_ngg: ge_pc_alloc with OVERSUB_EN and
 * pc_lines / 4 - 1, spi_shader_pgm_rsrc3_gs and _rsrc4_gs from
 * ac_compute_late_alloc), src/amd/common/ac_shader_util.c
 * (ac_compute_late_alloc: CUs 2 and 3 masked on GFX10) and the registers
 * RADV left while it drew NGG on the RX 5600 XT (MMIO reads 2026-10-27:
 * GE_PC_ALLOC 0x1ff, SPI_SHADER_PGM_RSRC3_GS 0x003ffff3,
 * SPI_SHADER_PGM_RSRC4_GS 0x0020ffff).
 */
#define AMDRDNA_UC_PARAMETER_CACHE_NGG		(1U | (0xffU << 1))
#define AMDRDNA_SH_PRIMITIVE_UNITS_NGG		(0xfff3U | (0x3fU << 16))
#define AMDRDNA_SH_PRIMITIVE_UNITS_HIGH_NGG	(0xffffU | (32U << 16))

/*
 * A culling primitive shader's: the parameter cache oversubscribed by its
 * quarter (256 lines) times 2, 3 or 4 by the parameters each vertex
 * exports (up to 2, up to 4, more: most vertices are culled and export
 * none), and the geometry stage's late allocation of 64 waves.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_shader.c
 * (radv_precompute_registers_hw_ngg: oversub_pc_lines times the culling
 * factor; ac_compute_late_alloc in src/amd/common/ac_shader_util.c: ten
 * waves a good CU with culling, at most 64 on GFX10) and the registers
 * RADV wrote culling mview's scene on the RX 5600 XT (its command
 * buffer, 2026-10-28: GE_PC_ALLOC 0x3ff, SPI_SHADER_PGM_RSRC4_GS
 * 0x0040ffff).
 */
#define AMDRDNA_NGG_CULL_PARAMETER_LINES	256U
#define AMDRDNA_UC_PARAMETER_CACHE_OVERSUB	1U
#define AMDRDNA_UC_PARAMETER_CACHE_LINES_SHIFT	1U
#define AMDRDNA_SH_PRIMITIVE_UNITS_HIGH_CULL	(0xffffU | (64U << 16))

/*
 * The index type register (set through the indexed setter with index 2)
 * and its values, and the draw packets of an index buffer: its size in
 * indices, and the draw from its address.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (VGT_INDEX_TYPE 0x3090c; the enum VGT_INDEX_TYPE_MODE: VGT_INDEX_16 0,
 * VGT_INDEX_32 1; VGT_DRAW_INITIATOR: SOURCE_SELECT, the enum
 * VGT_DI_SOURCE_SELECT: DI_SRC_SEL_DMA 0), src/amd/common/sid.h
 * (PKT3_INDEX_BUFFER_SIZE 0x13, PKT3_DRAW_INDEX_2 0x27) and
 * src/amd/vulkan/radv_cmd_buffer.c (radv_emit_index_buffer: the type set
 * with index 2 on GFX9 and later; radv_cs_emit_draw_indexed_packet: the
 * draw's most indices, address, count and initiator).
 */
#define AMDRDNA_UC_INDEX_TYPE			0x3090cU
#define AMDRDNA_UC_INDEX_TYPE_INDEX		2U
#define AMDRDNA_INDEX_16			0U
#define AMDRDNA_INDEX_32			1U
#define AMDRDNA_PM4_OP_INDEX_BUFFER_SIZE	0x13U
#define AMDRDNA_PM4_OP_DRAW_INDEXED		0x27U
#define AMDRDNA_PM4_DRAW_FROM_MEMORY		0U

/*
 * The colour target's fields: the layout's format in bits 6:2 (32 is 4,
 * 8, 8, 8, 8 is 10, 32, 32 is 11, 16, 16, 16, 16 is 12, 32, 32, 32, 32 is
 * 14), number type in 10:8 (unorm 0, snorm 1, uint 4, sint 5, sRGB 6,
 * float 7), component swap in 12:11 (standard 0, alternate 1), blend clamp
 * 15, blend bypass 16, simple float 17 and round mode 18; the size's
 * height less one in 13:0 and width less one in 27:14; the surface's
 * resource type 2D (1) in 25:24 and resource level 1 in 29:27 (the linear
 * swizzle mode is 0).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (CB_COLOR0_INFO: FORMAT, NUMBER_TYPE, COMP_SWAP, BLEND_CLAMP,
 * BLEND_BYPASS, SIMPLE_FLOAT, ROUND_MODE; CB_COLOR0_ATTRIB2: MIP0_HEIGHT,
 * MIP0_WIDTH; CB_COLOR0_ATTRIB3: COLOR_SW_MODE, RESOURCE_TYPE,
 * RESOURCE_LEVEL; the enums ColorFormat: COLOR_32 4, COLOR_8_8_8_8 10,
 * COLOR_32_32 11, COLOR_16_16_16_16 12, COLOR_32_32_32_32 14,
 * SurfaceNumber: NUMBER_UNORM 0, NUMBER_SNORM 1, NUMBER_UINT 4,
 * NUMBER_SINT 5, NUMBER_SRGB 6, NUMBER_FLOAT 7, SurfaceSwap: SWAP_STD 0,
 * SWAP_ALT 1).
 */
#define AMDRDNA_CX_TARGET_FORMAT_32		(4U << 2)
#define AMDRDNA_CX_TARGET_FORMAT_8888		(10U << 2)
#define AMDRDNA_CX_TARGET_FORMAT_32_32		(11U << 2)
#define AMDRDNA_CX_TARGET_FORMAT_16_16_16_16	(12U << 2)
#define AMDRDNA_CX_TARGET_FORMAT_32_32_32_32	(14U << 2)
#define AMDRDNA_CX_TARGET_NUMBER_UNORM		(0U << 8)
#define AMDRDNA_CX_TARGET_NUMBER_SNORM		(1U << 8)
#define AMDRDNA_CX_TARGET_NUMBER_UINT		(4U << 8)
#define AMDRDNA_CX_TARGET_NUMBER_SINT		(5U << 8)
#define AMDRDNA_CX_TARGET_NUMBER_SRGB		(6U << 8)
#define AMDRDNA_CX_TARGET_NUMBER_FLOAT		(7U << 8)
#define AMDRDNA_CX_TARGET_SWAP_STANDARD		(0U << 11)
#define AMDRDNA_CX_TARGET_SWAP_ALTERNATE	(1U << 11)
#define AMDRDNA_CX_TARGET_BLEND_CLAMP		(1U << 15)
#define AMDRDNA_CX_TARGET_BLEND_BYPASS		(1U << 16)
#define AMDRDNA_CX_TARGET_SIMPLE_FLOAT		(1U << 17)
#define AMDRDNA_CX_TARGET_ROUND_MODE		(1U << 18)
#define AMDRDNA_CX_TARGET_WIDTH_SHIFT		14U
#define AMDRDNA_CX_TARGET_SURFACE_2D		((1U << 24) | (1U << 27))

/*
 * A colour target of samples: the view's last slice (25:13, the first in
 * 12:0), the attributes' samples and fragments (powers of two, 14:12 and
 * 16:15), the surface's render swizzle (SW_64KB_R_X 27 in 18:14; its
 * layers less one in 12:0), and the addresses of its CMASK and FMASK
 * (CB_COLOR0_CMASK 0x28c7c, CB_COLOR0_FMASK 0x28c84 in the target's block;
 * CB_COLOR0_CMASK_BASE_EXT 0x28e60 and CB_COLOR0_FMASK_BASE_EXT 0x28e80 in
 * runs of four bytes).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (CB_COLOR0_VIEW: SLICE_START, SLICE_MAX; CB_COLOR0_ATTRIB: NUM_SAMPLES,
 * NUM_FRAGMENTS; CB_COLOR0_ATTRIB3: MIP0_DEPTH, COLOR_SW_MODE; the
 * registers named) and src/amd/addrlib/inc/addrtypes.h (ADDR_SW_64KB_R_X
 * 27).
 */
#define AMDRDNA_CX_TARGET_SLICE_MAX_SHIFT	13U
#define AMDRDNA_CX_TARGET_SAMPLES_SHIFT		12U
#define AMDRDNA_CX_TARGET_FRAGMENTS_SHIFT	15U
#define AMDRDNA_CX_TARGET_SWIZZLE_RENDER	(27U << 14)
#define AMDRDNA_CX_TARGET_CMASK			0x28c7cU
#define AMDRDNA_CX_TARGET_FMASK			0x28c84U
#define AMDRDNA_CX_TARGET_CMASK_HIGH		0x28e60U
#define AMDRDNA_CX_TARGET_FMASK_HIGH		0x28e80U

/*
 * The colour control (normal mode 1 in 6:4, the copy raster operation 0xcc
 * in 23:16); the blend's fields (colour source 4:0, function 7:5,
 * destination 12:8, alpha source 20:16, function 23:21, destination 28:24,
 * separate alpha 29, enable 30); its factors and functions; the blend
 * shortcut that is off for a target that does not blend (6 in both
 * function fields, 10:8 and 26:24).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (CB_COLOR_CONTROL: DISABLE_DUAL_QUAD, MODE, ROP3; the enum CBMode: CB_NORMAL 1;
 * CB_BLEND0_CONTROL and the enums BlendOp: BLEND_ZERO 0, BLEND_ONE 1,
 * BLEND_SRC_COLOR 2, BLEND_ONE_MINUS_SRC_COLOR 3, BLEND_SRC_ALPHA 4,
 * BLEND_ONE_MINUS_SRC_ALPHA 5, BLEND_DST_ALPHA 6, BLEND_ONE_MINUS_DST_ALPHA
 * 7, BLEND_DST_COLOR 8, BLEND_ONE_MINUS_DST_COLOR 9,
 * BLEND_SRC_ALPHA_SATURATE 10, BLEND_CONSTANT_COLOR 13,
 * BLEND_ONE_MINUS_CONSTANT_COLOR 14, BLEND_SRC1_COLOR 15,
 * BLEND_INV_SRC1_COLOR 16, BLEND_SRC1_ALPHA 17, BLEND_INV_SRC1_ALPHA 18,
 * BLEND_CONSTANT_ALPHA 19, BLEND_ONE_MINUS_CONSTANT_ALPHA 20, CombFunc:
 * COMB_DST_PLUS_SRC 0, COMB_SRC_MINUS_DST 1, COMB_MIN_DST_SRC 2,
 * COMB_MAX_DST_SRC 3, COMB_DST_MINUS_SRC 4; SX_MRT0_BLEND_OPT: COLOR_COMB_FCN,
 * ALPHA_COMB_FCN; the enum SX_OPT_COMB_FCN: OPT_COMB_NONE 0,
 * OPT_COMB_BLEND_DISABLED 6) and src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_color_blend: the shortcut off without blending).
 */
#define AMDRDNA_CX_COLOUR_CONTROL_NORMAL	((1U << 4) | (0xccU << 16))
#define AMDRDNA_CX_COLOUR_CONTROL_NO_DUAL_QUAD	(1U << 0)
#define AMDRDNA_CX_BLEND_DESTINATION_SHIFT	8U
#define AMDRDNA_CX_BLEND_FUNCTION_SHIFT		5U
#define AMDRDNA_CX_BLEND_ALPHA_SHIFT		16U
#define AMDRDNA_CX_BLEND_SEPARATE_ALPHA		(1U << 29)
#define AMDRDNA_CX_BLEND_ENABLE			(1U << 30)
#define AMDRDNA_BLEND_ZERO			0U
#define AMDRDNA_BLEND_ONE			1U
#define AMDRDNA_BLEND_SOURCE_COLOUR		2U
#define AMDRDNA_BLEND_ONE_MINUS_SOURCE_COLOUR	3U
#define AMDRDNA_BLEND_SOURCE_ALPHA		4U
#define AMDRDNA_BLEND_ONE_MINUS_SOURCE_ALPHA	5U
#define AMDRDNA_BLEND_TARGET_ALPHA		6U
#define AMDRDNA_BLEND_ONE_MINUS_TARGET_ALPHA	7U
#define AMDRDNA_BLEND_TARGET_COLOUR		8U
#define AMDRDNA_BLEND_ONE_MINUS_TARGET_COLOUR	9U
#define AMDRDNA_BLEND_SOURCE_ALPHA_SATURATED	10U
#define AMDRDNA_BLEND_CONSTANT_COLOUR		13U
#define AMDRDNA_BLEND_ONE_MINUS_CONSTANT_COLOUR	14U
#define AMDRDNA_BLEND_SECOND_COLOUR		15U
#define AMDRDNA_BLEND_ONE_MINUS_SECOND_COLOUR	16U
#define AMDRDNA_BLEND_SECOND_ALPHA		17U
#define AMDRDNA_BLEND_ONE_MINUS_SECOND_ALPHA	18U
#define AMDRDNA_BLEND_CONSTANT_ALPHA		19U
#define AMDRDNA_BLEND_ONE_MINUS_CONSTANT_ALPHA	20U
#define AMDRDNA_BLEND_ADD			0U
#define AMDRDNA_BLEND_SUBTRACT			1U
#define AMDRDNA_BLEND_MINIMUM			2U
#define AMDRDNA_BLEND_MAXIMUM			3U
#define AMDRDNA_BLEND_REVERSE_SUBTRACT		4U
#define AMDRDNA_CX_BLEND_SHORTCUT_OFF		((6U << 8) | (6U << 24))
#define AMDRDNA_CX_BLEND_SHORTCUT_NONE		0U

/*
 * The shortcut of a blending target: every source and target channel kept
 * (the export unit leaves nothing out; 7 in each of the colour's and the
 * alpha's source and target fields, bits 2:0, 6:4, 18:16 and 22:20), and
 * the blend's own combination in the colour's (10:8) and the alpha's
 * (26:24) function fields: add 1, subtract 2, minimum 3, maximum 4,
 * reverse subtract 5.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (SX_MRT0_BLEND_OPT: COLOR_SRC_OPT, COLOR_DST_OPT, COLOR_COMB_FCN,
 * ALPHA_SRC_OPT, ALPHA_DST_OPT, ALPHA_COMB_FCN; the enums SX_BLEND_OPT:
 * BLEND_OPT_PRESERVE_NONE_IGNORE_NONE 7, and SX_OPT_COMB_FCN: OPT_COMB_ADD
 * 1, OPT_COMB_SUBTRACT 2, OPT_COMB_MIN 3, OPT_COMB_MAX 4,
 * OPT_COMB_REVSUBTRACT 5) and src/amd/vulkan/radv_pipeline_graphics.h
 * (radv_translate_blend_opt_function).
 */
#define AMDRDNA_CX_BLEND_SHORTCUT_KEEP_ALL	((7U << 0) | (7U << 4) | (7U << 16) | (7U << 20))
#define AMDRDNA_CX_BLEND_SHORTCUT_COLOUR_SHIFT	8U
#define AMDRDNA_CX_BLEND_SHORTCUT_ALPHA_SHIFT	24U
#define AMDRDNA_SHORTCUT_ADD			1U
#define AMDRDNA_SHORTCUT_SUBTRACT		2U
#define AMDRDNA_SHORTCUT_MINIMUM		3U
#define AMDRDNA_SHORTCUT_MAXIMUM		4U
#define AMDRDNA_SHORTCUT_REVERSE_SUBTRACT	5U

/*
 * The depth state of a pass without depth: the pixel program's depth
 * control with early depth then late (1 in 5:4), the kill enable (6) for a
 * program that discards, and the dual-quad disable (15); the depth and
 * stencil layouts invalid (0).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (DB_SHADER_CONTROL: Z_ORDER, KILL_ENABLE, DUAL_QUAD_DISABLE; the enum
 * ZOrder: EARLY_Z_THEN_LATE_Z 1; DB_Z_INFO: FORMAT; the enum ZFormat:
 * Z_INVALID 0).
 */
#define AMDRDNA_CX_DEPTH_SHADER_EARLY		(1U << 4)
#define AMDRDNA_CX_DEPTH_SHADER_KILL		(1U << 6)
#define AMDRDNA_CX_DEPTH_SHADER_NO_DUAL_QUAD	(1U << 15)

/*
 * The depth unit's surface (a pass with a depth attachment): its view, the
 * two overrides (none: the viewport's depth clamp on), the hierarchical
 * tile base, the size, the depth clear, the info word, the bases of depth and stencil (read and write, low
 * words from 0x28048, high from 0x28068) and the hierarchical tile
 * surface.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (DB_DEPTH_VIEW 0x28008, DB_RENDER_OVERRIDE 0x2800c, DB_RENDER_OVERRIDE2
 * 0x28010, DB_HTILE_DATA_BASE 0x28014, DB_DEPTH_SIZE_XY 0x2801c, DB_DEPTH_CLEAR 0x2802c,
 * DB_RESERVED_REG_2 0x2803c, DB_Z_READ_BASE 0x28048, DB_STENCIL_READ_BASE
 * 0x2804c, DB_Z_WRITE_BASE 0x28050, DB_STENCIL_WRITE_BASE 0x28054,
 * DB_Z_READ_BASE_HI 0x28068 .. DB_HTILE_DATA_BASE_HI 0x28078,
 * DB_HTILE_SURFACE 0x28abc) and src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_fb_ds_state on GFX10: seven words from 0x2803c, the first
 * its resource level, then the five high words).
 */
#define AMDRDNA_CX_DEPTH_VIEW			0x28008U
#define AMDRDNA_CX_DEPTH_OVERRIDE		0x2800cU
#define AMDRDNA_CX_DEPTH_OVERRIDE2		0x28010U
#define AMDRDNA_CX_DEPTH_TILES_BASE		0x28014U
#define AMDRDNA_CX_DEPTH_SIZE			0x2801cU
#define AMDRDNA_CX_DEPTH_CLEAR			0x2802cU
#define AMDRDNA_CX_DEPTH_INFO			0x2803cU
#define AMDRDNA_CX_DEPTH_BASE_HIGH		0x28068U
#define AMDRDNA_CX_DEPTH_TILES_SURFACE		0x28abcU

/*
 * The depth surface's fields: the info word's resource level 1 (31:28);
 * the depth layout's format (1:0, Z_16 1, Z_32_FLOAT 3), samples (a power
 * of two, 3:2) and swizzle mode (8:4, SW_64KB_Z_X 24); the size's x and y less one (13:0, 29:16); the render
 * control's depth and stencil compression off (6, 5: the surface has no
 * hierarchical tiles); the depth control's test (1), write (2) and
 * compare (6:4, in VkCompareOp's order).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (DB_RESERVED_REG_2: RESOURCE_LEVEL; DB_Z_INFO: FORMAT, NUM_SAMPLES, SW_MODE; the enum
 * ZFormat: Z_16 1, Z_32_FLOAT 3; DB_DEPTH_SIZE_XY: X_MAX, Y_MAX;
 * DB_RENDER_CONTROL: STENCIL_COMPRESS_DISABLE, DEPTH_COMPRESS_DISABLE;
 * DB_DEPTH_CONTROL: Z_ENABLE, Z_WRITE_ENABLE, ZFUNC; the enum CompareFrag:
 * FRAG_NEVER 0 .. FRAG_ALWAYS 7), src/amd/addrlib/inc/addrtypes.h
 * (ADDR_SW_64KB_Z_X 24) and src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_fb_ds_state: the compression off for an uncompressed
 * surface).
 */
#define AMDRDNA_CX_DEPTH_INFO_LEVEL		(1U << 28)
#define AMDRDNA_CX_DEPTH_FORMAT_16		1U
#define AMDRDNA_CX_DEPTH_FORMAT_FLOAT		3U
#define AMDRDNA_CX_DEPTH_SWIZZLE_Z		(24U << 4)
#define AMDRDNA_CX_DEPTH_SAMPLES_SHIFT		2U
#define AMDRDNA_CX_DEPTH_SIZE_Y_SHIFT		16U
#define AMDRDNA_CX_DEPTH_RENDER_UNCOMPRESSED	((1U << 5) | (1U << 6))
#define AMDRDNA_CX_DEPTH_TEST			(1U << 1)
#define AMDRDNA_CX_DEPTH_WRITE			(1U << 2)
#define AMDRDNA_CX_DEPTH_COMPARE_SHIFT		4U
#define AMDRDNA_CX_DEPTH_COMPARE_ALWAYS		7U

/*
 * The stencil: its control (the operations of the front face's failed
 * stencil test, passed test and failed depth test in 3:0, 7:4 and 11:8,
 * the back face's in 15:12, 19:16 and 23:20) and each face's reference
 * and masks (the reference 7:0, the compare mask 15:8, the write mask
 * 23:16 and the operation value 31:24, which RADV sets to 1), one after
 * the other; the depth control's stencil test (0), back face test (7) and
 * each face's compare (10:8, 22:20); the stencil layout's format
 * (STENCIL_8 1 in bit 0) and swizzle mode (8:4); the bases of the
 * stencil plane, read and written, after the depth's (one word each,
 * low and high).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (DB_STENCIL_CONTROL 0x2842c: STENCILFAIL, STENCILZPASS, STENCILZFAIL
 * and their _BF; DB_STENCILREFMASK 0x28430 and DB_STENCILREFMASK_BF
 * 0x28434: STENCILTESTVAL, STENCILMASK, STENCILWRITEMASK, STENCILOPVAL;
 * DB_DEPTH_CONTROL: STENCIL_ENABLE, BACKFACE_ENABLE, STENCILFUNC,
 * STENCILFUNC_BF; DB_STENCIL_INFO: FORMAT, SW_MODE and the enum
 * StencilFormat: STENCIL_8 1; DB_STENCIL_READ_BASE 0x2804c,
 * DB_STENCIL_WRITE_BASE 0x28054, DB_STENCIL_READ_BASE_HI 0x2806c,
 * DB_STENCIL_WRITE_BASE_HI 0x28074) and src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_depth_stencil_state: STENCILOPVAL 1 for both faces).
 */
#define AMDRDNA_CX_STENCIL_CONTROL		0x2842cU
#define AMDRDNA_CX_STENCIL_BACK_SHIFT		12U
#define AMDRDNA_CX_STENCIL_PASS_SHIFT		4U
#define AMDRDNA_CX_STENCIL_DEPTH_FAIL_SHIFT	8U
#define AMDRDNA_CX_STENCIL_COMPARE_MASK_SHIFT	8U
#define AMDRDNA_CX_STENCIL_WRITE_MASK_SHIFT	16U
#define AMDRDNA_CX_STENCIL_OPERATION_ONE	(1U << 24)
#define AMDRDNA_CX_DEPTH_STENCIL_TEST		(1U << 0)
#define AMDRDNA_CX_DEPTH_STENCIL_BACK		(1U << 7)
#define AMDRDNA_CX_DEPTH_STENCIL_COMPARE_SHIFT	8U
#define AMDRDNA_CX_DEPTH_STENCIL_BACK_COMPARE_SHIFT	20U
#define AMDRDNA_CX_STENCIL_FORMAT_8		1U

/*
 * The depth unit's stencil operations.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json (the enum
 * StencilOp: STENCIL_KEEP 0, STENCIL_ZERO 1, STENCIL_REPLACE_TEST 3,
 * STENCIL_ADD_CLAMP 5, STENCIL_SUB_CLAMP 6, STENCIL_INVERT 7,
 * STENCIL_ADD_WRAP 8, STENCIL_SUB_WRAP 9).
 */
#define AMDRDNA_STENCIL_KEEP			0U
#define AMDRDNA_STENCIL_ZERO			1U
#define AMDRDNA_STENCIL_REPLACE			3U
#define AMDRDNA_STENCIL_ADD_CLAMP		5U
#define AMDRDNA_STENCIL_SUBTRACT_CLAMP		6U
#define AMDRDNA_STENCIL_INVERT			7U
#define AMDRDNA_STENCIL_ADD_WRAP		8U
#define AMDRDNA_STENCIL_SUBTRACT_WRAP		9U

/*
 * The depth bias: the clamp, the front face's scale and offset and the
 * back face's, one after the other (floats), and the depth format's
 * control before them (the negated count of the depth's bits in 7:0, a
 * float depth in bit 8); a 32-bit float depth is -23 bits and float, a
 * 16-bit normalized depth -16 bits.  The slope factor is written sixteen
 * times larger.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (PA_SU_POLY_OFFSET_DB_FMT_CNTL 0x28b78: POLY_OFFSET_NEG_NUM_DB_BITS,
 * POLY_OFFSET_DB_IS_FLOAT_FMT; PA_SU_POLY_OFFSET_CLAMP 0x28b7c) and
 * src/amd/vulkan/radv_cmd_buffer.c (radv_emit_depth_bias: the five
 * words from the clamp, the slope times 16.0, a D32_SFLOAT depth's
 * NEG_NUM_DB_BITS(-23) and DB_IS_FLOAT_FMT, a D16_UNORM depth's
 * NEG_NUM_DB_BITS(-16)).
 */
#define AMDRDNA_CX_DEPTH_BIAS_FORMAT		0x28b78U
#define AMDRDNA_CX_DEPTH_BIAS			0x28b7cU
#define AMDRDNA_CX_DEPTH_BIAS_FLOAT32		((0x100U - 23U) | (1U << 8))
#define AMDRDNA_CX_DEPTH_BIAS_UNORM16		(0x100U - 16U)
#define AMDRDNA_CX_DEPTH_BIAS_SLOPE_POWER	4U

/*
 * The blend constant's red, green, blue and alpha, one after the other
 * (floats).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (CB_BLEND_RED 0x28414 .. CB_BLEND_ALPHA 0x28420).
 */
#define AMDRDNA_CX_BLEND_CONSTANTS		0x28414U

/*
 * The scissor words: x in 14:0, y in 30:16 (the screen scissor's 15:0 and
 * 31:16), and the window offset off (31); the edge rule and the clip
 * rectangle rule of a plain rasteriser; the viewport scissor enable (bit 1
 * of the scan mode); the clip control of a D3D clip space with linear
 * attribute clipping (19, 24); the viewport enables (5:0) with w as 1/w
 * (10); the vertex rounding (the pixel centre at 0.5 (0), to even (2 in
 * 2:1), 1/256 fixed point (5 in 5:3)); the guard band words of 1.0; the
 * rasteriser's cull bits (front 0, back 1) and its clockwise front face (2).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (PA_SC_VPORT_SCISSOR_0_TL: TL_X, TL_Y, WINDOW_OFFSET_DISABLE;
 * PA_SC_SCREEN_SCISSOR_TL; PA_SC_MODE_CNTL_0: VPORT_SCISSOR_ENABLE;
 * PA_CL_CLIP_CNTL: DX_CLIP_SPACE_DEF, DX_LINEAR_ATTR_CLIP_ENA;
 * PA_CL_VTE_CNTL: VPORT_*_ENA, VTX_W0_FMT; PA_SU_VTX_CNTL: PIX_CENTER,
 * ROUND_MODE, QUANT_MODE and their enums X_ROUND_TO_EVEN 2,
 * X_16_8_FIXED_POINT_1_256TH 5; PA_SU_SC_MODE_CNTL: CULL_FRONT,
 * CULL_BACK, FACE) and src/amd/vulkan/radv_queue.c (the edge rule
 * 0xaaaaaaaa, the vertex rounding and the viewport enables of the
 * preamble) and src/amd/vulkan/radv_cmd_buffer.c (the clip rectangle rule
 * 0xffff).
 */
#define AMDRDNA_CX_SCISSOR_Y_SHIFT		16U
#define AMDRDNA_CX_SCISSOR_NO_WINDOW_OFFSET	(1U << 31)
#define AMDRDNA_CX_EDGE_RULE_PLAIN		0xaaaaaaaaU
#define AMDRDNA_CX_CLIP_RECTANGLE_ALL		0xffffU
#define AMDRDNA_CX_SCAN_VIEWPORT_SCISSOR	(1U << 1)
#define AMDRDNA_CX_CLIP_D3D			((1U << 19) | (1U << 24))
#define AMDRDNA_CX_VIEWPORT_ALL			(0x3fU | (1U << 10))
#define AMDRDNA_CX_VERTEX_ROUNDING_PLAIN	(1U | (2U << 1) | (5U << 3))
#define AMDRDNA_CX_RASTER_CULL_FRONT		(1U << 0)
#define AMDRDNA_CX_RASTER_CULL_BACK		(1U << 1)
#define AMDRDNA_CX_RASTER_CLOCKWISE		(1U << 2)

/*
 * The rasteriser's depth bias of front faces, back faces, and points and
 * lines (11, 12, 13), all on with the bias as RADV turns them on.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (PA_SU_SC_MODE_CNTL: POLY_OFFSET_FRONT_ENABLE, POLY_OFFSET_BACK_ENABLE,
 * POLY_OFFSET_PARA_ENABLE) and src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_culling: the three from the depth bias's enable).
 */
#define AMDRDNA_CX_RASTER_DEPTH_BIAS		((1U << 11) | (1U << 12) | (1U << 13))

/*
 * The rasteriser's polygon types of front and back faces: triangles (2 in
 * bits 7:5 and 10:8), what RADV writes for a filled polygon mode.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (PA_SU_SC_MODE_CNTL: POLYMODE_FRONT_PTYPE, POLYMODE_BACK_PTYPE, the enum
 * X_DRAW_TRIANGLES 2) and src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_culling: both from radv_translate_fill, line 10873).
 */
#define AMDRDNA_CX_RASTER_FILLED		((2U << 5) | (2U << 8))

/*
 * The point size of 1.0 (8 in each 12.4 half: height 15:0, width 31:16),
 * the point limits 0 to 4095.9375 (12.4), the line width 1.0 (8), the
 * small primitive filter on (bit 0), the sample configuration and mask of
 * one sample, the stages of a legacy vertex program in wave32 (two
 * primitive groups a wave in 18:15, wave32 23), and the geometry engine's
 * 128 primitives and 256 vertices a group (8:0, 17:9).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (PA_SU_POINT_SIZE, PA_SU_POINT_MINMAX, PA_SU_LINE_CNTL,
 * PA_SU_SMALL_PRIM_FILTER_CNTL, VGT_SHADER_STAGES_EN: MAX_PRIMGRP_IN_WAVE,
 * VS_W32_EN, GE_CNTL: PRIM_GRP_SIZE, VERT_GRP_SIZE),
 * src/amd/vulkan/radv_queue.c (radv_emit_graphics: the point size and
 * limits) and src/amd/vulkan/radv_cmd_buffer.c (the stages' two primitive
 * groups; gfx10_emit_ge_cntl: the group sizes 128 and 256 without a
 * geometry or tessellation program).
 */
#define AMDRDNA_CX_POINT_SIZE_ONE		((8U << 16) | 8U)
#define AMDRDNA_CX_POINT_LIMITS_ALL		(0xffffU << 16)
#define AMDRDNA_CX_LINE_WIDTH_ONE		8U
#define AMDRDNA_CX_SMALL_PRIMITIVE_FILTER_ON	1U
#define AMDRDNA_CX_SAMPLE_MASK_ALL		0xffffffffU
#define AMDRDNA_CX_STAGES_LEGACY_VERTEX		((2U << 15) | (1U << 23))
#define AMDRDNA_UC_GEOMETRY_GROUPS		(128U | (256U << 9))

/*
 * A primitive shader in wave32: the stages (the export stage real, 2 in
 * 4:3; the primitive generator, 13; two primitive groups a wave, 18:15;
 * the geometry stage in wave32, 22, and the vertex stage's wave32 bit, 23,
 * which RADV sets with it; the passthrough mode, 25), and the
 * subgroup's size, RADV's for a vertex program (128 vertices and 128
 * primitives, four waves).  GFX10 counts a subgroup's vertices only after
 * it has taken a whole triangle, so a subgroup of 128 vertices is asked
 * for as 126; the geometry engine's vertex group is that less 5 (a GFX10
 * hang otherwise), its primitive group 128.  The primitive export is one
 * channel; one primitive comes from each input primitive.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (VGT_SHADER_STAGES_EN: ES_EN, PRIMGEN_EN, MAX_PRIMGRP_IN_WAVE,
 * GS_W32_EN, VS_W32_EN, PRIMGEN_PASSTHRU_EN, the enum VGT_STAGES_ES_EN: ES_STAGE_REAL
 * 2; VGT_GS_ONCHIP_CNTL: ES_VERTS_PER_SUBGRP 10:0, GS_PRIMS_PER_SUBGRP
 * 21:11, GS_INST_PRIMS_IN_SUBGRP 31:22; GE_CNTL: PRIM_GRP_SIZE 8:0,
 * VERT_GRP_SIZE 17:9; GE_NGG_SUBGRP_CNTL: PRIM_AMP_FACTOR 8:0;
 * SPI_SHADER_IDX_FORMAT: IDX0_EXPORT_FORMAT, the enum SPI_SHADER_FORMAT:
 * SPI_SHADER_1COMP 1; VGT_GS_INSTANCE_CNT: CNT 8:2, one instance as RADV's
 * gs_num_invocations of 1), src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_vgt_shader_config_gfx6: the NGG stages; radv_emit_hw_ngg: the
 * GFX10 VERT_GRP_SIZE of hw_max_esverts less 5),
 * src/amd/vulkan/radv_pipeline_graphics.c (radv_get_vgt_shader_key: the
 * vertex and geometry wave sizes both the last stage's) and
 * src/amd/vulkan/radv_shader_info.c (gfx10_get_ngg_info: GFX10's
 * max_esverts_base and max_gsprims_base 128, GFX10's hw_max_esverts of
 * max_esverts - max_verts_per_prim + 1, lines 1486-1654).
 */
#define AMDRDNA_CX_STAGES_PRIMITIVE		((2U << 3) | (1U << 13) | (2U << 15) | (1U << 22) | (1U << 23))
#define AMDRDNA_CX_STAGES_PASSTHROUGH		(1U << 25)
#define AMDRDNA_NGG_VERTICES			128U
#define AMDRDNA_NGG_PRIMITIVES			128U
#define AMDRDNA_NGG_VERTICES_ASKED		(AMDRDNA_NGG_VERTICES - 3U + 1U)
#define AMDRDNA_CX_SUBGROUP_SIZE_NGG	(AMDRDNA_NGG_VERTICES_ASKED | (AMDRDNA_NGG_PRIMITIVES << 11) | (AMDRDNA_NGG_PRIMITIVES << 22))
#define AMDRDNA_UC_GEOMETRY_GROUPS_NGG		(AMDRDNA_NGG_PRIMITIVES | ((AMDRDNA_NGG_VERTICES_ASKED - 5U) << 9))
#define AMDRDNA_CX_SUBGROUP_ONE_PRIMITIVE	1U
#define AMDRDNA_CX_GEOMETRY_ONE_INSTANCE	(1U << 2)
#define AMDRDNA_CX_PRIMITIVE_EXPORT_ONE		1U

/*
 * The type of the primitives a primitive shader exports: points 0, line
 * strips 1, triangle strips 2.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json (the enum
 * VGT_GS_OUTPRIM_TYPE: POINTLIST 0, LINESTRIP 1, TRISTRIP 2) and
 * src/amd/vulkan/radv_pipeline_graphics.h (radv_conv_prim_to_gs_out: the
 * lists and strips of each kind to the strip type).
 */
#define AMDRDNA_OUTPUT_POINTS			0U
#define AMDRDNA_OUTPUT_LINES			1U
#define AMDRDNA_OUTPUT_TRIANGLES		2U

/*
 * The binner of GFX10 off: binning disabled with the new scan converter
 * (2 in 1:0), bins of 128 by 128 (2 in 6:4 and 9:7), no start of
 * primitive (18) and a flush on a binning transition (28).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (PA_SC_BINNER_CNTL_0: BINNING_MODE, BIN_SIZE_X_EXTEND, BIN_SIZE_Y_EXTEND,
 * DISABLE_START_OF_PRIM, FLUSH_ON_BINNING_TRANSITION; the enums
 * BinningMode: DISABLE_BINNING_USE_NEW_SC 2, BinSizeExtend:
 * BIN_SIZE_128_PIXELS 2) and src/amd/vulkan/radv_cmd_buffer.c
 * (radv_get_disabled_binning_state, the GFX10 form for targets of four
 * bytes a pixel).
 */
#define AMDRDNA_CX_BINNER_OFF			(2U | (2U << 4) | (2U << 7) | (1U << 18) | (1U << 28))

/*
 * The primitive types: a triangle list (4) and strip (6), a point list
 * (1), a line list (2) and strip (3).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json (the enum
 * VGT_DI_PRIM_TYPE: DI_PT_POINTLIST 1, DI_PT_LINELIST 2, DI_PT_LINESTRIP 3,
 * DI_PT_TRILIST 4, DI_PT_TRISTRIP 6).
 */
#define AMDRDNA_PRIMITIVE_POINTS		1U
#define AMDRDNA_PRIMITIVE_LINES			2U
#define AMDRDNA_PRIMITIVE_LINE_STRIP		3U
#define AMDRDNA_PRIMITIVE_TRIANGLES		4U
#define AMDRDNA_PRIMITIVE_TRIANGLE_STRIP	6U

/*
 * The most bytes one memory move takes: its 26-bit count, rounded down to
 * 32 bytes so the next move starts aligned.
 *
 * Value referenced: Mesa 25.0.7 src/amd/vulkan/radv_cp_dma.c
 * (cp_dma_max_byte_count: the GFX9 and later count, less the 32-byte
 * alignment).
 */
#define AMDRDNA_VK_MOVE_MAX			0x03ffffe0U

/* The context registers (0x28000 to 0x28ffc) and the user configuration registers tracked (0x30000 to 0x30ffc), one word each. */
#define AMDRDNA_VK_CONTEXT_REGISTERS		1024U
#define AMDRDNA_VK_USER_REGISTERS		1024U

/*
 * An indirect buffer of packets being written: its words, how many are
 * written, and its capacity.  A packet that would pass the end is not
 * written and full is set; the writer is then used no more.
 * vertex_stage is the kind of vertex stage the buffer's last draw used
 * (AMDRDNA_VK_STAGE_*): unknown at the start, as the buffer before is not
 * known; a draw by the other kind flushes the geometry engine first.
 * context_values holds what the buffer last wrote to each context register
 * whose bit in context_known is set (none at the start and after the
 * context's reset): a single register set again to the value it holds is
 * not written, since every context register write after a draw rolls the
 * context the GPU draws with.  user_values and user_known do the same for
 * the user configuration registers (the draws' primitive and index types
 * and groups), and instances and index_room (known when draws_known is
 * set) for the draw packets' instance count and index buffer size, which
 * the command processor keeps until set again.
 */
struct amdrdna_vk_pm4 {
	uint32_t *words;
	uint32_t count;
	uint32_t capacity;
	bool full;
	uint32_t vertex_stage;
	uint32_t context_values[AMDRDNA_VK_CONTEXT_REGISTERS];
	uint32_t context_known[AMDRDNA_VK_CONTEXT_REGISTERS / 32U];
	uint32_t user_values[AMDRDNA_VK_USER_REGISTERS];
	uint32_t user_known[AMDRDNA_VK_USER_REGISTERS / 32U];
	bool instances_known;
	uint32_t instances;
	bool index_room_known;
	uint32_t index_room;
};

/* The kinds of vertex stage a buffer's draws use: not known yet, a legacy vertex program, a primitive shader. */
#define AMDRDNA_VK_STAGE_UNKNOWN		0U
#define AMDRDNA_VK_STAGE_LEGACY			1U
#define AMDRDNA_VK_STAGE_PRIMITIVE		2U

void amdrdna_vk_pm4_start(struct amdrdna_vk_pm4 *pm4, uint32_t *words, uint32_t capacity);
void amdrdna_vk_pm4_packet(struct amdrdna_vk_pm4 *pm4, uint32_t operation, uint32_t body_words, bool compute);
void amdrdna_vk_pm4_word(struct amdrdna_vk_pm4 *pm4, uint32_t word);
void amdrdna_vk_pm4_context_start(struct amdrdna_vk_pm4 *pm4);
void amdrdna_vk_pm4_set_context(struct amdrdna_vk_pm4 *pm4, uint32_t reg, uint32_t value);
void amdrdna_vk_pm4_set_context_run(struct amdrdna_vk_pm4 *pm4, uint32_t reg, const uint32_t *values, uint32_t count);
void amdrdna_vk_pm4_set_shader(struct amdrdna_vk_pm4 *pm4, uint32_t reg, uint32_t value);
void amdrdna_vk_pm4_set_shader_run(struct amdrdna_vk_pm4 *pm4, uint32_t reg, const uint32_t *values, uint32_t count);
void amdrdna_vk_pm4_set_user_config(struct amdrdna_vk_pm4 *pm4, uint32_t reg, uint32_t value);
void amdrdna_vk_pm4_set_user_config_indexed(struct amdrdna_vk_pm4 *pm4, uint32_t reg, uint32_t index, uint32_t value);
void amdrdna_vk_pm4_set_shader_indexed(struct amdrdna_vk_pm4 *pm4, uint32_t reg, uint32_t index, uint32_t value);
void amdrdna_vk_pm4_event(struct amdrdna_vk_pm4 *pm4, uint32_t event, uint32_t index);
void amdrdna_vk_pm4_event_fence(struct amdrdna_vk_pm4 *pm4, uint32_t event, uint32_t caches, uint64_t address, uint32_t value);
void amdrdna_vk_pm4_event_address(struct amdrdna_vk_pm4 *pm4, uint32_t event, uint32_t index, uint64_t address);
void amdrdna_vk_pm4_timestamp(struct amdrdna_vk_pm4 *pm4, uint64_t address);
void amdrdna_vk_pm4_wait_value(struct amdrdna_vk_pm4 *pm4, uint64_t address, uint32_t value);
void amdrdna_vk_pm4_cache_acquire(struct amdrdna_vk_pm4 *pm4, uint32_t caches);
void amdrdna_vk_pm4_fetcher_sync(struct amdrdna_vk_pm4 *pm4);
void amdrdna_vk_pm4_move(struct amdrdna_vk_pm4 *pm4, uint64_t source, uint64_t destination, uint32_t bytes);
void amdrdna_vk_pm4_fill(struct amdrdna_vk_pm4 *pm4, uint64_t destination, uint32_t value, uint32_t bytes);
void amdrdna_vk_pm4_draw(struct amdrdna_vk_pm4 *pm4, uint32_t vertex_count, uint32_t instance_count);
void amdrdna_vk_pm4_draw_indexed(struct amdrdna_vk_pm4 *pm4, uint64_t indices, uint32_t index_room, uint32_t index_count, uint32_t instance_count);
void amdrdna_vk_pm4_dispatch(struct amdrdna_vk_pm4 *pm4, uint32_t x, uint32_t y, uint32_t z);
void amdrdna_vk_pm4_pad(struct amdrdna_vk_pm4 *pm4, uint32_t multiple);

#endif /* DRIVERS_GPU_AMDRDNA_VK_AMDRDNA_VK_GFX10_H */
