/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The commands a GHCI 1.0 GPU reads from its ring and the records it
 * reads from memory: the pipeline state, the texture, sampler and buffer
 * descriptors and the render pass description.
 *
 * The encoders write words and bytes one at a time in little-endian
 * order; nothing here lays a structure over GPU memory, takes a lock or
 * touches the hardware, so a host test calls them exactly as the kernel
 * does.  Each command encoder returns the dwords it wrote, or 0 when its
 * arguments do not make a valid command.
 */

#ifndef DRIVERS_GPU_GHCI_CMD_H
#define DRIVERS_GPU_GHCI_CMD_H

#include <stdint.h>

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 7.1, the packet header; section 7.2, the opcodes).
 */
#define GHCI_HEADER_OPCODE_MASK		0xFFU
#define GHCI_HEADER_DWORDS_SHIFT	8U
#define GHCI_HEADER_DWORDS_MASK		0xFFU
#define GHCI_HEADER_FLAGS_SHIFT		16U
#define GHCI_PACKET_DWORDS_MAX		255U
#define GHCI_OP_NOP			0x00U
#define GHCI_OP_VM_MAP			0x10U
#define GHCI_OP_VM_UNMAP		0x11U
#define GHCI_OP_SESSION_OPEN		0x12U
#define GHCI_OP_SESSION_CLOSE		0x13U
#define GHCI_OP_BIND_SESSION		0x14U
#define GHCI_OP_QUERY_CAPS		0x15U
#define GHCI_OP_FAULT_LOG_SETUP		0x16U
#define GHCI_OP_VM_QUERY_PT_MEM		0x17U
#define GHCI_OP_MMU_ENABLE		0x18U
#define GHCI_OP_BIND_PIPELINE		0x20U
#define GHCI_OP_BIND_VERTEX_BUFFERS	0x21U
#define GHCI_OP_BIND_INDEX_BUFFER	0x22U
#define GHCI_OP_BIND_DESCRIPTOR_TABLE	0x23U
#define GHCI_OP_SET_VIEWPORT		0x24U
#define GHCI_OP_SET_SCISSOR		0x25U
#define GHCI_OP_SET_DYNAMIC_STATE	0x26U
#define GHCI_OP_DRAW			0x27U
#define GHCI_OP_DRAW_INDEXED		0x28U
#define GHCI_OP_DRAW_INDIRECT		0x29U
#define GHCI_OP_DRAW_MESH_TASKS		0x2AU
#define GHCI_OP_BEGIN_RENDER_PASS	0x2BU
#define GHCI_OP_END_RENDER_PASS		0x2CU
#define GHCI_OP_LOAD_CONST		0x2DU
#define GHCI_OP_DISPATCH_INDIRECT	0x2EU
#define GHCI_OP_DISPATCH		0x30U
#define GHCI_OP_BARRIER			0x31U
#define GHCI_OP_EXECUTE_COMMANDS	0x32U
#define GHCI_OP_FENCE			0x33U
#define GHCI_OP_WAIT_FENCE		0x34U
#define GHCI_OP_WRITE_TIMESTAMP		0x35U
#define GHCI_OP_BEGIN_QUERY		0x36U
#define GHCI_OP_END_QUERY		0x37U
#define GHCI_OP_WRITE_MEM		0x3CU
#define GHCI_OP_COPY_BUFFER		0x40U
#define GHCI_OP_COPY_IMAGE		0x41U
#define GHCI_OP_BLIT			0x42U
#define GHCI_OP_FILL_BUFFER		0x43U
#define GHCI_OP_RESOLVE_IMAGE		0x44U
#define GHCI_OP_COPY_BUFFER_TO_IMAGE	0x45U
#define GHCI_OP_COPY_IMAGE_TO_BUFFER	0x46U
#define GHCI_OP_DISP_SET		0x50U
#define GHCI_OP_FLIP			0x51U

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md (section 7.2,
 * the size of each command in dwords); lumiere-gpu 78d1b6b7 ghci.md
 * (section 7.2, CMD_VM_MAP 16, CMD_VM_UNMAP 12, CMD_SESSION_OPEN 6,
 * CMD_FAULT_LOG_SETUP 7, CMD_VM_QUERY_PT_MEM 5, CMD_MMU_ENABLE 6).
 */
#define GHCI_DW_VM_MAP			16U
#define GHCI_DW_VM_UNMAP		12U
#define GHCI_DW_SESSION_OPEN		6U
#define GHCI_DW_SESSION_CLOSE		2U
#define GHCI_DW_BIND_SESSION		2U
#define GHCI_DW_FAULT_LOG_SETUP		7U
#define GHCI_DW_VM_QUERY_PT_MEM		5U
#define GHCI_DW_MMU_ENABLE		6U
#define GHCI_DW_QUERY_CAPS		5U
#define GHCI_DW_BIND_PIPELINE		3U
#define GHCI_DW_BIND_INDEX_BUFFER	4U
#define GHCI_DW_BIND_DESCRIPTOR_TABLE	4U
#define GHCI_DW_SET_VIEWPORT		7U
#define GHCI_DW_SET_SCISSOR		5U
#define GHCI_DW_SET_DYNAMIC_STATE	6U
#define GHCI_DW_DRAW			5U
#define GHCI_DW_DRAW_INDEXED		6U
#define GHCI_DW_DRAW_INDIRECT		7U
#define GHCI_DW_DRAW_INDIRECT_COUNT	8U
#define GHCI_DW_DRAW_MESH_TASKS		4U
#define GHCI_DW_BEGIN_RENDER_PASS	10U
#define GHCI_DW_END_RENDER_PASS		1U
#define GHCI_DW_DISPATCH_INDIRECT	6U
#define GHCI_DW_DISPATCH		4U
#define GHCI_DW_BARRIER			4U
#define GHCI_DW_FENCE			6U
#define GHCI_DW_WAIT_FENCE		5U
#define GHCI_DW_WRITE_TIMESTAMP		4U
#define GHCI_DW_QUERY			4U
#define GHCI_DW_WRITE_MEM		5U
#define GHCI_DW_COPY_BUFFER		8U
#define GHCI_DW_COPY_IMAGE		11U
#define GHCI_DW_BLIT			12U
#define GHCI_DW_FILL_BUFFER		5U
#define GHCI_DW_RESOLVE_IMAGE		11U
#define GHCI_DW_BUFFER_IMAGE		10U
#define GHCI_DW_FLIP			8U

/*
 * CMD_QUERY_CAPS: the address and the size of the buffer the capability
 * block is written to are whole 64-byte lines, at least one; the driver
 * asks for no more than the largest block, 1152 bytes (32 + 255 dwords,
 * in whole lines).
 *
 * Values referenced: lumiere-gpu 78d1b6b7 ghci.md (section 7.3,
 * CMD_QUERY_CAPS: five dwords, dst_pa on 64 bytes, dst_bytes >= 64 in
 * whole 64 bytes, min(dst_bytes, 4 * CAP_DWORDS) bytes written; section
 * 3.2.5, FMT_COUNT at most 255).
 */
#define GHCI_QUERY_CAPS_ALIGN		64U
#define GHCI_QUERY_CAPS_BYTES_MAX	1152U

/* The vertex buffer bindings, the constant bank and its words per load. */
#define GHCI_VERTEX_BINDINGS		16U
#define GHCI_CONST_WORDS		512U
#define GHCI_CONST_LOAD_MAX		32U

/* The register pairs of one DISP_SET: 1 + 2N dwords stay within a packet. */
#define GHCI_DISP_SET_PAIRS_MAX		127U

/*
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 7.3 and
 * section 11.2, CMD_VM_MAP flags [4:0], [31:5] reserved).
 */
#define GHCI_VM_FLAG_READ		(1U << 0)
#define GHCI_VM_FLAG_WRITE		(1U << 1)
#define GHCI_VM_FLAG_EXEC		(1U << 2)
#define GHCI_VM_FLAG_COHERENT		(1U << 3)
#define GHCI_VM_FLAG_WC			(1U << 4)
#define GHCI_VM_FLAGS_MASK		0x1FU

/*
 * The records the page-table commands write to host memory: CMD_VM_MAP's
 * completion (32 bytes on 32), CMD_VM_UNMAP's (a 16-byte header and
 * 16-byte entries of the intermediate tables it gave back, in a buffer
 * of whole 64 bytes, at least one), CMD_VM_QUERY_PT_MEM's sizes (64
 * bytes on 64), and the 32-byte fault records of the fault log ring
 * (on 32 bytes, a power of two of at least two of them).
 *
 * Values referenced: lumiere-gpu 78d1b6b7 ghci.md (section 7.3,
 * GhciVmMapComp: status, used_bytes, required_bytes, required_align;
 * GhciVmUnmapComp: status, freed_count, written_count, required_bytes and
 * {pa, bytes} entries; GhciVmPtMemInfo at 0x08 init_pt_bytes, 0x0C
 * init_pt_align, 0x10 table_page_bytes, 0x14 table_page_align;
 * CMD_FAULT_LOG_SETUP flags [0] ENABLE and [8] COMP_WRITE; section 10,
 * GhciFaultRecord: seq, fault_addr, timestamp, session_id,
 * fault_info_word).
 */
#define GHCI_MAP_COMP_BYTES		32U
#define GHCI_MAP_OK			0U
#define GHCI_MAP_NEED_PT_MEM		1U
#define GHCI_MAP_COLLISION		2U
#define GHCI_MAP_INVALID_SESSION	3U
#define GHCI_MAP_COMP_STATUS		0x00U
#define GHCI_MAP_COMP_USED		0x04U
#define GHCI_MAP_COMP_REQUIRED		0x08U
#define GHCI_MAP_COMP_ALIGN		0x0CU
#define GHCI_UNMAP_COMP_ALIGN		64U
#define GHCI_UNMAP_OK			0U
#define GHCI_UNMAP_NEED_BUF		1U
#define GHCI_UNMAP_INVALID_SESSION	2U
#define GHCI_UNMAP_COMP_STATUS		0x00U
#define GHCI_UNMAP_COMP_FREED		0x04U
#define GHCI_UNMAP_COMP_WRITTEN		0x08U
#define GHCI_UNMAP_COMP_REQUIRED	0x0CU
#define GHCI_UNMAP_COMP_ENTRIES		0x10U
#define GHCI_UNMAP_ENTRY_BYTES		16U
#define GHCI_PT_INFO_BYTES		64U
#define GHCI_PT_INFO_INIT_BYTES		0x08U
#define GHCI_PT_INFO_INIT_ALIGN		0x0CU
#define GHCI_PT_INFO_TABLE_BYTES	0x10U
#define GHCI_PT_INFO_TABLE_ALIGN	0x14U
#define GHCI_FAULT_LOG_ENABLE		(1U << 0)
#define GHCI_FAULT_RECORD_BYTES		32U
#define GHCI_FAULT_RECORD_SEQ		0x00U
#define GHCI_FAULT_RECORD_ADDRESS	0x08U
#define GHCI_FAULT_RECORD_SESSION	0x18U
#define GHCI_FAULT_RECORD_INFO		0x1CU

/*
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md
 * (section 7.3, CMD_FENCE flags, CMD_BARRIER stages (VF 0x1 to MS
 * 0x400) and flags, CMD_DRAW_INDIRECT (header flags[0] indexed, [1] the
 * 8-dword form with a count buffer), CMD_WRITE_TIMESTAMP,
 * CMD_BEGIN/END_QUERY, CMD_COPY_BUFFER, CMD_BEGIN_RENDER_PASS,
 * CMD_DISP_SET, CMD_FLIP).
 */
#define GHCI_FENCE_FLAG_IRQ		(1U << 0)
#define GHCI_FENCE_FLAG_MEM_WRITE	(1U << 1)
#define GHCI_FENCE_FLAG_HOST_FLUSH	(1U << 2)
#define GHCI_STAGE_VF			(1U << 0)
#define GHCI_STAGE_VS			(1U << 1)
#define GHCI_STAGE_FS			(1U << 2)
#define GHCI_STAGE_CS			(1U << 3)
#define GHCI_STAGE_ROP			(1U << 4)
#define GHCI_STAGE_BLT			(1U << 5)
#define GHCI_STAGE_TCS			(1U << 6)
#define GHCI_STAGE_TES			(1U << 7)
#define GHCI_STAGE_GS			(1U << 8)
#define GHCI_STAGE_TS			(1U << 9)
#define GHCI_STAGE_MS			(1U << 10)
#define GHCI_BARRIER_WAIT_IDLE		(1U << 0)
#define GHCI_BARRIER_INVAL_ICACHE	(1U << 1)
#define GHCI_BARRIER_INVAL_TEX		(1U << 2)
#define GHCI_BARRIER_FLUSH_L1_DATA	(1U << 3)
#define GHCI_BARRIER_INVAL_L1_DATA	(1U << 4)
#define GHCI_BARRIER_INVAL_DESCRIPTOR	(1U << 5)
#define GHCI_BARRIER_FLUSH_L2		(1U << 6)
#define GHCI_DRAW_INDIRECT_INDEXED	(1U << 0)
#define GHCI_DRAW_INDIRECT_COUNT	(1U << 1)
#define GHCI_TIMESTAMP_HOST		(1U << 0)
#define GHCI_QUERY_INDEX_MASK		0x00FFFFFFU
#define GHCI_QUERY_PRECISE		(1U << 24)
#define GHCI_QUERY_AVAILABILITY		(1U << 25)
#define GHCI_COPY_ASYNC			(1U << 0)
#define GHCI_COPY_FILL_PATTERN		(1U << 1)
#define GHCI_COPY_FLUSH_SRC		(1U << 2)
#define GHCI_COPY_FLUSH_DST		(1U << 3)
#define GHCI_CLEAR_COLOR_SHIFT		0U
#define GHCI_CLEAR_DEPTH		(1U << 8)
#define GHCI_CLEAR_STENCIL		(1U << 9)
#define GHCI_DISP_SET_WAIT_IDLE		(1U << 0)
#define GHCI_FLIP_VSYNC_WAIT		(1U << 0)
#define GHCI_FLIP_IMMEDIATE		(1U << 1)
#define GHCI_FLIP_PLANE_CURSOR		(1U << 2)
#define GHCI_FLIP_RECORD_ENABLE		(1U << 3)
#define GHCI_STRIDE_FROM_PSO		0xFFFFFFFFU

/*
 * CMD_WRITE_MEM's flags: when the write happens (at the top of the pipe,
 * after the processor's own earlier writes, or at the bottom, after all
 * earlier work), whether it writes 64 bits, and whether the GPU's caches
 * are flushed first (the bottom only).
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 7.3,
 * CMD_WRITE_MEM: 5 dwords, flags [1:0] stage 0 TOP and 1 BOTTOM, [2]
 * WRITE_64, [3] FLUSH_CACHES, TOP with FLUSH_CACHES is INVALID_OP, dst_va
 * 4-byte aligned or 8-byte aligned for WRITE_64).
 */
#define GHCI_WRITE_MEM_TOP		0U
#define GHCI_WRITE_MEM_BOTTOM		1U
#define GHCI_WRITE_MEM_64		(1U << 2)
#define GHCI_WRITE_MEM_FLUSH		(1U << 3)

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 7.3, CMD_SET_DYNAMIC_STATE state IDs).
 */
#define GHCI_DYNAMIC_DEPTH_BIAS		0x01U
#define GHCI_DYNAMIC_BLEND_CONSTANTS	0x02U
#define GHCI_DYNAMIC_STENCIL_PARAMS	0x03U
#define GHCI_DYNAMIC_LINE_WIDTH		0x04U
#define GHCI_DYNAMIC_BLEND_ATTACHMENT_0	0x07U
#define GHCI_DYNAMIC_BORDER_COLOR_TABLE	0x10U

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 8.2, 8.3 and 8.4, the sizes of the records in memory).
 */
#define GHCI_PSO_BYTES			128U
#define GHCI_TEXTURE_DESC_BYTES		32U
#define GHCI_SAMPLER_DESC_BYTES		16U
#define GHCI_BUFFER_DESC_BYTES		16U
#define GHCI_RENDER_PASS_BYTES		192U
#define GHCI_COLOR_ATTACHMENTS		8U
#define GHCI_VERTEX_ATTRIBUTES		16U

/*
 * The GhciFormat numbers every format field of a record or a packet
 * carries: 0 for none (an unused attribute, an untyped buffer), 0xFF for
 * no format; the formats themselves are the executor's table's.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 8.5.1, the
 * 8-bit GhciFormat namespace: 0x00 GHCI_FMT_NONE, 0xFF GHCI_FMT_INVALID).
 */
#define GHCI_FORMAT_NONE		0x00U
#define GHCI_FORMAT_INVALID		0xFFU

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 8.2, the byte offsets of the pipeline state).
 */
#define GHCI_PSO_VS_CODE		0x00U
#define GHCI_PSO_FS_CODE		0x08U
#define GHCI_PSO_CS_CODE		0x10U
#define GHCI_PSO_MS_CODE		0x18U
#define GHCI_PSO_FLAGS			0x20U
#define GHCI_PSO_RASTER			0x24U
#define GHCI_PSO_DEPTH_STENCIL		0x28U
#define GHCI_PSO_BLEND			0x2CU
#define GHCI_PSO_STRIDES		0x30U
#define GHCI_PSO_LIMITS			0x34U
#define GHCI_PSO_LOCAL_SIZE		0x38U
#define GHCI_PSO_LDS_FLAGS		0x3CU
#define GHCI_PSO_ATTRIBUTES		0x40U

/*
 * The pipeline state with the extended stages: dw8's flags[16] makes the
 * record 256 bytes, the 128 of the pipeline state and the tessellation
 * and geometry shader stages behind them; the topology PATCH_LIST, the
 * geometry shader's input and output topologies, the tessellation
 * domain and spacing; and the limits of their fields.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 8.2,
 * GhciPipelineStateExt: dw32-37 the TCS, TES and GS code, dw38
 * tess_state, dw39 gs_state, dw40 the registers, dw41-44 the geometry
 * shader's spill ring, dw45-63 reserved; the topology 7 PATCH_LIST;
 * section 7.3, CMD_BIND_PIPELINE, a 256-byte record on 64 bytes;
 * section 12.10.4, max_output_vertices x output_stride_words at most
 * 1024, a patch at most 16256 bytes from 0x220 on).
 */
#define GHCI_PSO_EXT_BYTES		256U
#define GHCI_PSO_ALIGN			64U
#define GHCI_PSO_FLAG_EXT_STAGES	(1U << 16)
#define GHCI_PSO_TCS_CODE		0x80U
#define GHCI_PSO_TES_CODE		0x88U
#define GHCI_PSO_GS_CODE		0x90U
#define GHCI_PSO_TESS_STATE		0x98U
#define GHCI_PSO_GS_STATE		0x9CU
#define GHCI_PSO_LIMITS_EXT		0xA0U
#define GHCI_PSO_SPILL_VA		0xA4U
#define GHCI_PSO_SPILL_BYTES		0xACU
#define GHCI_PSO_SPILL_STRIDE		0xB0U
#define GHCI_TOPOLOGY_PATCH_LIST	7U
#define GHCI_GS_INPUT_POINTS		0U
#define GHCI_GS_INPUT_LINES		1U
#define GHCI_GS_INPUT_LINE_STRIP	2U
#define GHCI_GS_INPUT_TRIANGLES		3U
#define GHCI_GS_INPUT_TRIANGLE_STRIP	4U
#define GHCI_GS_INPUT_RESERVED		5U
#define GHCI_GS_INPUT_LINES_ADJ		6U
#define GHCI_GS_INPUT_LINE_STRIP_ADJ	7U
#define GHCI_GS_INPUT_TRIANGLES_ADJ	8U
#define GHCI_GS_INPUT_TRIANGLE_STRIP_ADJ 9U
#define GHCI_GS_OUTPUT_POINTS		0U
#define GHCI_GS_OUTPUT_LINE_STRIP	1U
#define GHCI_GS_OUTPUT_TRIANGLE_STRIP	2U
#define GHCI_GS_VERTICES_MAX		256U
#define GHCI_GS_INVOCATIONS_MAX		32U
#define GHCI_GS_STREAMS_MAX		4U
#define GHCI_GS_STRIDE_WORDS_MAX	127U
#define GHCI_GS_OUTPUT_WORDS_MAX	1024U
#define GHCI_TESS_DOMAIN_TRIANGLE	0U
#define GHCI_TESS_DOMAIN_QUAD		1U
#define GHCI_TESS_DOMAIN_ISOLINE	2U
#define GHCI_TESS_SPACING_EQUAL		0U
#define GHCI_TESS_SPACING_FRACTIONAL_EVEN 1U
#define GHCI_TESS_SPACING_FRACTIONAL_ODD 2U
#define GHCI_TESS_POINTS_MAX		32U
#define GHCI_TESS_CP_STRIDE_MAX		127U
#define GHCI_TESS_PATCH_STRIDE_MAX	127U
#define GHCI_TESS_PATCH_UNIT		128U
#define GHCI_TESS_PATCH_CP_FIRST	0x220U
#define GHCI_TESS_PATCH_BYTES_MAX	16256U

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 8.3, the byte offsets of the descriptors).
 */
#define GHCI_TEX_BASE			0x00U
#define GHCI_TEX_WIDTH			0x08U
#define GHCI_TEX_HEIGHT			0x0AU
#define GHCI_TEX_DEPTH			0x0CU
#define GHCI_TEX_FORMAT			0x0EU
#define GHCI_TEX_MIP_LEVELS		0x0FU
#define GHCI_TEX_ROW_PITCH		0x10U
#define GHCI_TEX_SLICE_PITCH		0x14U
#define GHCI_TEX_SAMPLES		0x18U
#define GHCI_TEX_DIMENSION		0x19U
#define GHCI_TEX_FLAGS			0x1AU
#define GHCI_TEX_SWIZZLE		0x1CU
#define GHCI_TEX_FLAG_TILED		(1U << 0)
#define GHCI_TEX_FLAG_SRGB		(1U << 1)
#define GHCI_SAMPLER_LOD_BIAS		0x08U
#define GHCI_SAMPLER_UNNORMALIZED	0x0FU
#define GHCI_BUFFER_BASE		0x00U
#define GHCI_BUFFER_SIZE		0x08U
#define GHCI_BUFFER_STRIDE		0x0CU
#define GHCI_BUFFER_FORMAT		0x0EU
#define GHCI_BUFFER_FLAGS		0x0FU

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 8.4, the render pass description).
 */
#define GHCI_RP_ATTACHMENT_BYTES	16U
#define GHCI_RP_DEPTH_STENCIL		0x80U
#define GHCI_RP_RESOLVE			0x90U
#define GHCI_RP_WIDTH			0xA0U
#define GHCI_RP_HEIGHT			0xA2U
#define GHCI_RP_LAYERS			0xA4U
#define GHCI_RP_SAMPLES			0xA6U
#define GHCI_RP_CLEAR_TABLE		0xA8U

/* One vertex buffer binding of BIND_VERTEX_BUFFERS. */
struct ghci_vertex_binding {
	uint64_t va;
	uint32_t bytes;
	uint32_t stride;
};

/* One image slice of a transfer: where it is and how its rows lie. */
struct ghci_slice {
	uint64_t va;
	uint32_t pitch;
	uint32_t x;
	uint32_t y;
	uint32_t tiled;
};

/*
 * One transfer between two image slices or a slice and a buffer.
 *
 * COPY_IMAGE, RESOLVE_IMAGE and the buffer-image copies use src_max as
 * the extent; BLIT uses src_min to src_max and dst_min to dst_max.
 */
struct ghci_transfer {
	struct ghci_slice src;
	struct ghci_slice dst;
	uint32_t width;
	uint32_t height;
	uint32_t src_x1;
	uint32_t src_y1;
	uint32_t dst_x1;
	uint32_t dst_y1;
	uint32_t format;
	uint32_t dst_format;
	uint32_t filter;
	uint32_t resolve_mode;
	uint32_t samples;
};

/* One packed vertex attribute of the pipeline state. */
struct ghci_attribute {
	uint32_t format;
	uint32_t binding;
	uint32_t per_instance;
	uint32_t offset;
};

/*
 * The pipeline state, field by field, before it is packed into the
 * 128 bytes the GPU reads.  A graphics state has vs_code; a compute
 * state has cs_code.
 */
struct ghci_pso {
	uint64_t vs_code;
	uint64_t fs_code;
	uint64_t cs_code;
	uint64_t ms_code;
	uint32_t topology;
	uint32_t primitive_restart;
	uint32_t depth_clamp;
	uint32_t logic_op_enable;
	uint32_t logic_op;
	uint32_t alpha_to_coverage;
	uint32_t sample_shading;
	uint32_t sample_mask;
	uint32_t cull_mode;
	uint32_t front_cw;
	uint32_t polygon_mode;
	uint32_t depth_bias_enable;
	uint32_t line_width_fx4;
	uint32_t depth_test;
	uint32_t depth_write;
	uint32_t depth_op;
	uint32_t stencil_test;
	uint32_t front_stencil[4];
	uint32_t back_stencil[4];
	uint32_t separate_back;
	uint32_t blend_enable;
	uint32_t src_color;
	uint32_t dst_color;
	uint32_t color_op;
	uint32_t src_alpha;
	uint32_t dst_alpha;
	uint32_t alpha_op;
	uint32_t write_mask;
	uint32_t strides[4];
	uint32_t regs_vs;
	uint32_t regs_fs;
	uint32_t regs_cs;
	uint32_t varyings;
	uint32_t local_size[3];
	uint32_t lds_bytes;
	uint32_t fs_centroid;
	uint32_t fs_depth_export;
	uint32_t fs_discard;
	struct ghci_attribute attributes[GHCI_VERTEX_ATTRIBUTES];
	uint32_t attribute_count;
};

/*
 * The extended stages of a pipeline state, field by field, before they
 * are packed behind its 128 bytes.  A stage whose code is 0 is absent:
 * tessellation has both tcs_code and tes_code, a geometry shader
 * gs_code.  The geometry shader's spill ring is spill_va (0: none).
 */
struct ghci_pso_ext {
	uint64_t tcs_code;
	uint64_t tes_code;
	uint64_t gs_code;
	uint32_t input_points;
	uint32_t output_points;
	uint32_t domain;
	uint32_t spacing;
	uint32_t clockwise;
	uint32_t point_mode;
	uint32_t cp_stride_words;
	uint32_t patch_stride_128b;
	uint32_t gs_input;
	uint32_t gs_output;
	uint32_t gs_vertices;
	uint32_t gs_invocations;
	uint32_t gs_streams;
	uint32_t gs_stride_words;
	uint32_t regs_tcs;
	uint32_t regs_tes;
	uint32_t regs_gs;
	uint64_t spill_va;
	uint32_t spill_bytes;
	uint32_t spill_stride;
};

/* A texture descriptor, field by field. */
struct ghci_texture_desc {
	uint64_t base;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t format;
	uint32_t mip_levels;
	uint32_t row_pitch;
	uint32_t slice_pitch;
	uint32_t samples;
	uint32_t dimension;
	uint32_t flags;
	uint32_t swizzle;
};

/* A sampler descriptor, field by field. */
struct ghci_sampler_desc {
	uint32_t min_filter;
	uint32_t mag_filter;
	uint32_t mipmap_mode;
	uint32_t address[3];
	uint32_t compare_enable;
	uint32_t compare_op;
	int32_t lod_bias_fx8;
	uint32_t min_lod_fx4;
	uint32_t max_lod_fx4;
	uint32_t max_anisotropy;
	uint32_t border_type;
	uint32_t border_index;
	uint32_t unnormalized;
};

/* A buffer descriptor, field by field. */
struct ghci_buffer_desc {
	uint64_t base;
	uint32_t bytes;
	uint32_t stride;
	uint32_t format;
	uint32_t flags;
};

/* One attachment of a render pass description. */
struct ghci_rp_attachment {
	uint64_t va;
	uint32_t row_pitch;
	uint32_t format;
	uint32_t ops;
};

/* A render pass description, field by field. */
struct ghci_render_pass_desc {
	struct ghci_rp_attachment colors[GHCI_COLOR_ATTACHMENTS];
	struct ghci_rp_attachment depth_stencil;
	struct ghci_rp_attachment resolve;
	uint32_t width;
	uint32_t height;
	uint32_t layers;
	uint32_t samples;
	uint64_t clear_table;
};

/*
 * What one CMD_VM_MAP says: the session, the access flags, the GPU
 * address, the bus or local address and the bytes of the pages, the
 * memory given for intermediate page tables (none when pt_bytes is 0),
 * and where the completion record is written.
 */
struct ghci_map_args {
	uint32_t session;
	uint32_t flags;
	uint64_t va;
	uint64_t bus;
	uint64_t bytes;
	uint64_t pt;
	uint32_t pt_bytes;
	uint64_t comp;
};

uint32_t ghci_cmd_header(uint32_t opcode, uint32_t dwords, uint32_t flags);
uint32_t ghci_cmd_nop(uint32_t *out, uint32_t dwords);
uint32_t ghci_cmd_vm_map(uint32_t *out, const struct ghci_map_args *args);
uint32_t ghci_cmd_vm_unmap(uint32_t *out, uint32_t session, uint64_t va, uint64_t bytes, uint64_t comp, uint32_t comp_bytes);
uint32_t ghci_cmd_session_open(uint32_t *out, uint32_t session, uint32_t flags, uint64_t root, uint32_t root_bytes);
uint32_t ghci_cmd_fault_log_setup(uint32_t *out, uint64_t ring, uint32_t entries);
uint32_t ghci_cmd_vm_query_pt_mem(uint32_t *out, uint64_t bus, uint32_t bytes);
uint32_t ghci_cmd_mmu_enable(uint32_t *out, uint64_t root, uint32_t root_bytes);
uint32_t ghci_cmd_session_close(uint32_t *out, uint32_t session);
uint32_t ghci_cmd_bind_session(uint32_t *out, uint32_t session);
uint32_t ghci_cmd_query_caps(uint32_t *out, uint64_t bus, uint32_t bytes);
uint32_t ghci_cmd_bind_pipeline(uint32_t *out, uint64_t pso_va);
uint32_t ghci_cmd_bind_vertex_buffers(uint32_t *out, uint32_t first, uint32_t count, const struct ghci_vertex_binding *bindings);
uint32_t ghci_cmd_bind_index_buffer(uint32_t *out, uint64_t va, uint32_t index_32bit);
uint32_t ghci_cmd_bind_descriptor_table(uint32_t *out, uint32_t set, uint64_t table_va, uint32_t count);
uint32_t ghci_cmd_set_viewport(uint32_t *out, const uint32_t *float_bits);
uint32_t ghci_cmd_set_scissor(uint32_t *out, int32_t x, int32_t y, uint32_t width, uint32_t height);
uint32_t ghci_cmd_set_dynamic_state(uint32_t *out, uint32_t state_id, const uint32_t *payload);
uint32_t ghci_cmd_draw(uint32_t *out, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance);
uint32_t ghci_cmd_draw_indexed(uint32_t *out, uint32_t indices, uint32_t instances, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance);
uint32_t ghci_cmd_draw_indirect(uint32_t *out, uint32_t indexed, uint64_t va, uint32_t offset, uint32_t draws, uint32_t stride);
uint32_t ghci_cmd_draw_indirect_count(uint32_t *out, uint32_t indexed, uint64_t va, uint32_t offset, uint32_t max_draws, uint32_t stride, uint64_t count_va);
uint32_t ghci_cmd_draw_mesh_tasks(uint32_t *out, uint32_t x, uint32_t y, uint32_t z);
uint32_t ghci_cmd_begin_render_pass(uint32_t *out, uint64_t desc_va, const uint32_t *area, uint32_t color_clear, uint32_t depth_clear, uint32_t stencil_clear, uint32_t flags);
uint32_t ghci_cmd_end_render_pass(uint32_t *out);
uint32_t ghci_cmd_load_const(uint32_t *out, uint32_t offset, uint32_t count, const uint32_t *data);
uint32_t ghci_cmd_dispatch_indirect(uint32_t *out, uint64_t va, uint32_t offset);
uint32_t ghci_cmd_dispatch(uint32_t *out, uint32_t x, uint32_t y, uint32_t z);
uint32_t ghci_cmd_barrier(uint32_t *out, uint32_t src_stages, uint32_t dst_stages, uint32_t flags);
uint32_t ghci_cmd_fence(uint32_t *out, uint64_t bus, uint64_t seq, uint32_t flags);
uint32_t ghci_cmd_wait_fence(uint32_t *out, uint64_t bus, uint64_t value);
uint32_t ghci_cmd_write_timestamp(uint32_t *out, uint32_t host, uint64_t address, uint32_t stage);
uint32_t ghci_cmd_write_mem(uint32_t *out, uint32_t flags, uint64_t va, uint64_t value);
uint32_t ghci_cmd_query(uint32_t *out, uint32_t end, uint64_t pool_va, uint32_t index, uint32_t flags);
uint32_t ghci_cmd_copy_buffer(uint32_t *out, uint64_t src_va, uint64_t dst_va, uint32_t bytes, uint32_t flags, uint32_t fill);
uint32_t ghci_cmd_copy_image(uint32_t *out, const struct ghci_transfer *transfer);
uint32_t ghci_cmd_blit(uint32_t *out, const struct ghci_transfer *transfer);
uint32_t ghci_cmd_fill_buffer(uint32_t *out, uint64_t dst_va, uint32_t bytes, uint32_t pattern);
uint32_t ghci_cmd_resolve_image(uint32_t *out, const struct ghci_transfer *transfer);
uint32_t ghci_cmd_copy_buffer_to_image(uint32_t *out, const struct ghci_transfer *transfer);
uint32_t ghci_cmd_copy_image_to_buffer(uint32_t *out, const struct ghci_transfer *transfer);
uint32_t ghci_cmd_disp_set(uint32_t *out, uint32_t wait_idle, uint32_t pairs, const uint32_t *offsets, const uint32_t *values);
uint32_t ghci_cmd_flip(uint32_t *out, uint32_t display, uint32_t base_phys, uint32_t flags, uint32_t value, uint32_t record_phys);
int ghci_pso_encode(uint8_t *out, const struct ghci_pso *pso);
int ghci_pso_ext_encode(uint8_t *out, const struct ghci_pso *pso, const struct ghci_pso_ext *ext);
int ghci_texture_desc_encode(uint8_t *out, const struct ghci_texture_desc *desc);
int ghci_sampler_desc_encode(uint8_t *out, const struct ghci_sampler_desc *desc);
int ghci_buffer_desc_encode(uint8_t *out, const struct ghci_buffer_desc *desc);
int ghci_render_pass_desc_encode(uint8_t *out, const struct ghci_render_pass_desc *desc);
int ghci_fence64_passed(uint64_t memory, uint64_t value);

#endif
