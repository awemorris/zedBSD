/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A model of a GHCI 1.0 GPU for the driver's host tests (ghci-model.h).
 *
 * The values come from amaterrace-gpu 20bf888 docs/ghci.md through the
 * driver's headers; the behaviour follows the same document, with the
 * open readings behind struct model_switches.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/gpu/ghci/ghci-cmd.h"
#include "drivers/gpu/ghci/ghci-regs.h"
#include "ghci-model.h"

/* The VERSION register of the model: GHCI 1.0. */
#define MODEL_VERSION			0x00010000U

/* The watchdog's cycles after a power-on (2^28). */
#define MODEL_WDT_DEFAULT		0x10000000U

/* The core clock of the model profiles: 100 MHz. */
#define MODEL_GCLK_HZ			100000000U

/* RESIDENT_WARPS: 63 registers per lane, 16 warps. */
#define MODEL_RESIDENT_WARPS		((63U << 16) | 16U)

/* The PCI identity the VEND block reports (the KC705 endpoint). */
#define MODEL_VENDOR_ID			0x10EEU
#define MODEL_DEVICE_ID			0x7024U

/* The host addresses the GPU reaches without MEM: below 2 GiB. */
#define MODEL_HOST_LIMIT		0x80000000U

/* The sessions PROT_CAPS reports. */
#define MODEL_PROT_SESSIONS		15U

/* The colour attachments FMT_LIMITS reports. */
#define MODEL_FMT_LIMITS		8U

/* The FMT features (bits 0 to 8) of the formats the model knows. */
#define MODEL_FMT_ALL			0x1FFU

/* The 2 MiB chunks of a 40-bit GPU address range, for the page table count. */
#define MODEL_CHUNKS_MAX		(1U << 19)

/* The 1 GiB regions of a 40-bit GPU address range. */
#define MODEL_REGIONS_MAX		(1U << 10)

/* The packets the processor parses for one doorbell before it gives up (a guard against a loop). */
#define MODEL_PACKETS_PER_RUN		100000U

/*
 * The display controller of the DISP2 profiles: its FourCC, its
 * capabilities (TMDS, BGRA8 and RGBA8, the cursor plane, two modes), and
 * the two modes (1280 x 720 at 74.25 MHz, 640 x 480 at 25.175 MHz): the
 * MODE registers' words in the order H0, H1, V0, V1, CLOCK, FLAGS.
 */
#define MODEL_DC_ID			0x3243444dU
#define MODEL_DC_CAPS			(GHCI_DC_CAPS_TMDS | GHCI_DC_CAPS_BGRA8 | GHCI_DC_CAPS_RGBA8 | GHCI_DC_CAPS_CURSOR | \
					    (2U << GHCI_DC_CAPS_MODES_SHIFT))
#define MODEL_DC_MODES			2U
#define MODEL_DC_STICKY			(GHCI_DC_ST_UNDERFLOW | GHCI_DC_ST_READ_ERROR | GHCI_DC_ST_RECORD_ERROR | GHCI_DC_ST_BAD_WRITE)
static const uint32_t display_modes[MODEL_DC_MODES][6] = {
	{ 1280U | (40U << 16), 110U | (220U << 16), 720U | (5U << 16), 5U | (20U << 16), 74250U, 3U | (1U << 4) },
	{ 640U | (96U << 16), 16U | (48U << 16), 480U | (2U << 16), 10U | (33U << 16), 25175U, 0U }
};

/* What the processor does after a packet: go on, wait at it, or stop. */
enum packet_result {
	PACKET_NEXT = 0,
	PACKET_WAIT,
	PACKET_STOP
};

/* The capability a command needs (section 7.2, the Guard column). */
enum guard {
	GUARD_NONE = 0,
	GUARD_WALKER,
	GUARD_SU,
	GUARD_INDIRECT,
	GUARD_MESH,
	GUARD_SECONDARY,
	GUARD_BLT,
	GUARD_DISP
};

/* How a command's length is checked: a fixed count, or one of the variable forms. */
enum length_rule {
	LENGTH_FIXED = 0,
	LENGTH_NOP,
	LENGTH_VERTEX_BUFFERS,
	LENGTH_LOAD_CONST,
	LENGTH_DISP_SET,
	LENGTH_DRAW_INDIRECT
};

/*
 * One row of the command table: the opcode, its length, how the length
 * is checked, its guard, and whether it is a draw or a dispatch (where a
 * test's injection at a draw fires).
 */
struct command_rule {
	uint32_t opcode;
	uint32_t dwords;
	enum length_rule length;
	enum guard guard;
	int draw;
};

/* VA_BITS_ENC of a 32-bit VA (VA_BITS = 30 + the field). */
#define MODEL_VA32			2U

/* The optional blocks a profile has unless its comment says otherwise. */
#define MODEL_OPTIONAL_FEATURE		(GHCI_FEATURE_CLIP_POLY | GHCI_FEATURE_BLT_SCALE)
#define MODEL_OPTIONAL_FMT0		(GHCI_FMT0_TU_BILINEAR | GHCI_FMT0_BLT_BLEND | GHCI_FMT0_BLT_TILE)

/*
 * The capabilities of one profile: the three capability registers and
 * the blocks of its extended capability list (bit (1 << ID)).
 */
struct profile {
	uint32_t device_caps;
	uint32_t feature;
	uint32_t fmt0;
	uint32_t blocks;
};

/*
 * The command set of GHCI 1.0.
 *
 * Values referenced: amaterrace-gpu 20bf888 docs/ghci.md
 * (section 7.2, the opcodes, sizes and guards).
 */
static const struct command_rule command_rules[] = {
	{ GHCI_OP_NOP, 1U, LENGTH_NOP, GUARD_NONE, 0 },
	{ GHCI_OP_VM_MAP, GHCI_DW_VM_MAP, LENGTH_FIXED, GUARD_WALKER, 0 },
	{ GHCI_OP_VM_UNMAP, GHCI_DW_VM_UNMAP, LENGTH_FIXED, GUARD_WALKER, 0 },
	{ GHCI_OP_SESSION_OPEN, GHCI_DW_SESSION_OPEN, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_SESSION_CLOSE, GHCI_DW_SESSION_CLOSE, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_BIND_SESSION, GHCI_DW_BIND_SESSION, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_BIND_PIPELINE, GHCI_DW_BIND_PIPELINE, LENGTH_FIXED, GUARD_SU, 0 },
	{ GHCI_OP_BIND_VERTEX_BUFFERS, 2U, LENGTH_VERTEX_BUFFERS, GUARD_SU, 0 },
	{ GHCI_OP_BIND_INDEX_BUFFER, GHCI_DW_BIND_INDEX_BUFFER, LENGTH_FIXED, GUARD_SU, 0 },
	{ GHCI_OP_BIND_DESCRIPTOR_TABLE, GHCI_DW_BIND_DESCRIPTOR_TABLE, LENGTH_FIXED, GUARD_SU, 0 },
	{ GHCI_OP_SET_VIEWPORT, GHCI_DW_SET_VIEWPORT, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_SET_SCISSOR, GHCI_DW_SET_SCISSOR, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_SET_DYNAMIC_STATE, GHCI_DW_SET_DYNAMIC_STATE, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_DRAW, GHCI_DW_DRAW, LENGTH_FIXED, GUARD_SU, 1 },
	{ GHCI_OP_DRAW_INDEXED, GHCI_DW_DRAW_INDEXED, LENGTH_FIXED, GUARD_SU, 1 },
	{ GHCI_OP_DRAW_INDIRECT, GHCI_DW_DRAW_INDIRECT, LENGTH_DRAW_INDIRECT, GUARD_INDIRECT, 1 },
	{ GHCI_OP_DRAW_MESH_TASKS, GHCI_DW_DRAW_MESH_TASKS, LENGTH_FIXED, GUARD_MESH, 1 },
	{ GHCI_OP_BEGIN_RENDER_PASS, GHCI_DW_BEGIN_RENDER_PASS, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_END_RENDER_PASS, GHCI_DW_END_RENDER_PASS, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_LOAD_CONST, 3U, LENGTH_LOAD_CONST, GUARD_SU, 0 },
	{ GHCI_OP_DISPATCH_INDIRECT, GHCI_DW_DISPATCH_INDIRECT, LENGTH_FIXED, GUARD_SU, 1 },
	{ GHCI_OP_DISPATCH, GHCI_DW_DISPATCH, LENGTH_FIXED, GUARD_SU, 1 },
	{ GHCI_OP_BARRIER, GHCI_DW_BARRIER, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_EXECUTE_COMMANDS, 4U, LENGTH_FIXED, GUARD_SECONDARY, 0 },
	{ GHCI_OP_FENCE, GHCI_DW_FENCE, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_WAIT_FENCE, GHCI_DW_WAIT_FENCE, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_WRITE_TIMESTAMP, GHCI_DW_WRITE_TIMESTAMP, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_BEGIN_QUERY, GHCI_DW_QUERY, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_END_QUERY, GHCI_DW_QUERY, LENGTH_FIXED, GUARD_NONE, 0 },
	{ GHCI_OP_COPY_BUFFER, GHCI_DW_COPY_BUFFER, LENGTH_FIXED, GUARD_BLT, 0 },
	{ GHCI_OP_COPY_IMAGE, GHCI_DW_COPY_IMAGE, LENGTH_FIXED, GUARD_BLT, 0 },
	{ GHCI_OP_BLIT, GHCI_DW_BLIT, LENGTH_FIXED, GUARD_BLT, 0 },
	{ GHCI_OP_FILL_BUFFER, GHCI_DW_FILL_BUFFER, LENGTH_FIXED, GUARD_BLT, 0 },
	{ GHCI_OP_RESOLVE_IMAGE, GHCI_DW_RESOLVE_IMAGE, LENGTH_FIXED, GUARD_BLT, 0 },
	{ GHCI_OP_COPY_BUFFER_TO_IMAGE, GHCI_DW_BUFFER_IMAGE, LENGTH_FIXED, GUARD_BLT, 0 },
	{ GHCI_OP_COPY_IMAGE_TO_BUFFER, GHCI_DW_BUFFER_IMAGE, LENGTH_FIXED, GUARD_BLT, 0 },
	{ GHCI_OP_DISP_SET, 3U, LENGTH_DISP_SET, GUARD_DISP, 0 },
	{ GHCI_OP_FLIP, GHCI_DW_FLIP, LENGTH_FIXED, GUARD_DISP, 0 }
};

/*
 * The profiles of design section 14, in the order of enum model_profile.
 * The KC705 values stand for am-ws003's default build; whether its board
 * is unified or discrete is not known, so it has both forms.  Every
 * profile but va30 and kc705-min has a 32-bit VA (VA_BITS_ENC 2), and
 * has the polygon clipper and the bilinear filter, and the blitter's
 * scaling, blending and tiling with the blitter, unless its comment
 * says otherwise.
 */
static const struct profile profiles[MODEL_PROFILES] = {
	/* kc705-uma */
	{
		GHCI_CAP_UMA | GHCI_CAP_HW_WALKER | GHCI_CAP_GSI1 | (8U << GHCI_CAP_PHYS_LANES_SHIFT) |
		    (1U << GHCI_CAP_NUM_SU_SHIFT) | GHCI_CAP_BLT | GHCI_CAP_DISP,
		MODEL_VA32 | GHCI_FEATURE_HW_CURSOR | (4U << GHCI_FEATURE_MAX_MRT_SHIFT) | (4U << GHCI_FEATURE_LDS_KB_SHIFT) |
		    MODEL_OPTIONAL_FEATURE,
		GHCI_FMT0_ETC2 | GHCI_FMT0_SRGB | (4U << GHCI_FMT0_MAX_COLOR_SHIFT) | MODEL_OPTIONAL_FMT0,
		(1U << GHCI_EXTCAP_ID_VEND) | (1U << GHCI_EXTCAP_ID_FMT) | (1U << GHCI_EXTCAP_ID_DISP2)
	},
	/* kc705-discrete */
	{
		GHCI_CAP_HW_WALKER | GHCI_CAP_GSI1 | (8U << GHCI_CAP_PHYS_LANES_SHIFT) |
		    (1U << GHCI_CAP_NUM_SU_SHIFT) | GHCI_CAP_BLT | GHCI_CAP_DISP,
		MODEL_VA32 | GHCI_FEATURE_HW_CURSOR | (4U << GHCI_FEATURE_MAX_MRT_SHIFT) | (4U << GHCI_FEATURE_LDS_KB_SHIFT) |
		    MODEL_OPTIONAL_FEATURE,
		GHCI_FMT0_ETC2 | GHCI_FMT0_SRGB | (4U << GHCI_FMT0_MAX_COLOR_SHIFT) | MODEL_OPTIONAL_FMT0,
		(1U << GHCI_EXTCAP_ID_VEND) | (1U << GHCI_EXTCAP_ID_FMT) | (1U << GHCI_EXTCAP_ID_DISP2) |
		    (1U << GHCI_EXTCAP_ID_MEM)
	},
	/* minimal: no blitter, nor its bits */
	{
		GHCI_CAP_HW_WALKER | GHCI_CAP_GSI1 | (8U << GHCI_CAP_PHYS_LANES_SHIFT) | (1U << GHCI_CAP_NUM_SU_SHIFT),
		MODEL_VA32 | GHCI_FEATURE_CLIP_POLY,
		GHCI_FMT0_TU_BILINEAR,
		0U
	},
	/* full */
	{
		GHCI_CAP_UMA | GHCI_CAP_IOMMU | GHCI_CAP_HW_WALKER | GHCI_CAP_INDIRECT_DRAW | GHCI_CAP_SECONDARY_RING |
		    GHCI_CAP_DUAL_SOURCE | GHCI_CAP_FP16 | GHCI_CAP_GSI1 | (32U << GHCI_CAP_PHYS_LANES_SHIFT) |
		    (2U << GHCI_CAP_NUM_SU_SHIFT) | GHCI_CAP_PROT | GHCI_CAP_TEX_FFILT | GHCI_CAP_ROP_PACK |
		    GHCI_CAP_BLT | GHCI_CAP_DISP | GHCI_CAP_MSAA,
		MODEL_VA32 | GHCI_FEATURE_DUAL_SOURCE | GHCI_FEATURE_COARSE_SCAN | GHCI_FEATURE_FAST_CLEAR |
		    GHCI_FEATURE_LOGIC_OP | GHCI_FEATURE_EARLY_Z | GHCI_FEATURE_HW_CURSOR | GHCI_FEATURE_COLOR_LUT |
		    GHCI_FEATURE_MRT_BLEND | (8U << GHCI_FEATURE_MAX_MRT_SHIFT) | (16U << GHCI_FEATURE_LDS_KB_SHIFT) |
		    GHCI_FEATURE_FP16 | GHCI_FEATURE_SUBGROUP | MODEL_OPTIONAL_FEATURE,
		GHCI_FMT0_BC_MASK | GHCI_FMT0_ETC2 | GHCI_FMT0_SRGB | GHCI_FMT0_FFILT_FP16 | GHCI_FMT0_FFILT_FP32 |
		    (8U << GHCI_FMT0_MAX_COLOR_SHIFT) | MODEL_OPTIONAL_FMT0,
		(1U << GHCI_EXTCAP_ID_VEND) | (1U << GHCI_EXTCAP_ID_PROT) | (1U << GHCI_EXTCAP_ID_FMT) |
		    (1U << GHCI_EXTCAP_ID_DISP2)
	},
	/* flat: no blitter, nor its bits */
	{
		GHCI_CAP_UMA | GHCI_CAP_GSI1 | (8U << GHCI_CAP_PHYS_LANES_SHIFT) | (1U << GHCI_CAP_NUM_SU_SHIFT),
		MODEL_VA32 | (4U << GHCI_FEATURE_MAX_MRT_SHIFT) | (4U << GHCI_FEATURE_LDS_KB_SHIFT) | GHCI_FEATURE_CLIP_POLY,
		(4U << GHCI_FMT0_MAX_COLOR_SHIFT) | GHCI_FMT0_TU_BILINEAR,
		0U
	},
	/* prot */
	{
		GHCI_CAP_UMA | GHCI_CAP_HW_WALKER | GHCI_CAP_GSI1 | (8U << GHCI_CAP_PHYS_LANES_SHIFT) |
		    (1U << GHCI_CAP_NUM_SU_SHIFT) | GHCI_CAP_BLT | GHCI_CAP_DISP | GHCI_CAP_PROT,
		MODEL_VA32 | GHCI_FEATURE_HW_CURSOR | (4U << GHCI_FEATURE_MAX_MRT_SHIFT) | (4U << GHCI_FEATURE_LDS_KB_SHIFT) |
		    MODEL_OPTIONAL_FEATURE,
		GHCI_FMT0_ETC2 | GHCI_FMT0_SRGB | (4U << GHCI_FMT0_MAX_COLOR_SHIFT) | MODEL_OPTIONAL_FMT0,
		(1U << GHCI_EXTCAP_ID_VEND) | (1U << GHCI_EXTCAP_ID_FMT) | (1U << GHCI_EXTCAP_ID_DISP2) |
		    (1U << GHCI_EXTCAP_ID_PROT)
	},
	/* no-blt: no blitter, nor its bits */
	{
		GHCI_CAP_UMA | GHCI_CAP_HW_WALKER | GHCI_CAP_GSI1 | (8U << GHCI_CAP_PHYS_LANES_SHIFT) |
		    (1U << GHCI_CAP_NUM_SU_SHIFT) | GHCI_CAP_DISP,
		MODEL_VA32 | GHCI_FEATURE_HW_CURSOR | (4U << GHCI_FEATURE_MAX_MRT_SHIFT) | (4U << GHCI_FEATURE_LDS_KB_SHIFT) |
		    GHCI_FEATURE_CLIP_POLY,
		GHCI_FMT0_ETC2 | GHCI_FMT0_SRGB | (4U << GHCI_FMT0_MAX_COLOR_SHIFT) | GHCI_FMT0_TU_BILINEAR,
		(1U << GHCI_EXTCAP_ID_VEND) | (1U << GHCI_EXTCAP_ID_FMT) | (1U << GHCI_EXTCAP_ID_DISP2)
	},
	/* no-su */
	{
		GHCI_CAP_UMA | GHCI_CAP_HW_WALKER | (8U << GHCI_CAP_PHYS_LANES_SHIFT) | GHCI_CAP_BLT | GHCI_CAP_DISP,
		MODEL_VA32 | GHCI_FEATURE_HW_CURSOR | MODEL_OPTIONAL_FEATURE,
		MODEL_OPTIONAL_FMT0,
		(1U << GHCI_EXTCAP_ID_VEND) | (1U << GHCI_EXTCAP_ID_DISP2)
	},
	/* discrete */
	{
		GHCI_CAP_HW_WALKER | GHCI_CAP_INDIRECT_DRAW | GHCI_CAP_DUAL_SOURCE | GHCI_CAP_FP16 | GHCI_CAP_GSI1 |
		    (32U << GHCI_CAP_PHYS_LANES_SHIFT) | (2U << GHCI_CAP_NUM_SU_SHIFT) | GHCI_CAP_TEX_FFILT |
		    GHCI_CAP_ROP_PACK | GHCI_CAP_BLT | GHCI_CAP_DISP | GHCI_CAP_MSAA,
		MODEL_VA32 | GHCI_FEATURE_DUAL_SOURCE | GHCI_FEATURE_HW_CURSOR | GHCI_FEATURE_MRT_BLEND |
		    (8U << GHCI_FEATURE_MAX_MRT_SHIFT) | (16U << GHCI_FEATURE_LDS_KB_SHIFT) | GHCI_FEATURE_FP16 |
		    MODEL_OPTIONAL_FEATURE,
		GHCI_FMT0_BC_MASK | GHCI_FMT0_ETC2 | GHCI_FMT0_SRGB | GHCI_FMT0_FFILT_FP16 | GHCI_FMT0_FFILT_FP32 |
		    (8U << GHCI_FMT0_MAX_COLOR_SHIFT) | MODEL_OPTIONAL_FMT0,
		(1U << GHCI_EXTCAP_ID_VEND) | (1U << GHCI_EXTCAP_ID_FMT) | (1U << GHCI_EXTCAP_ID_DISP2) |
		    (1U << GHCI_EXTCAP_ID_MEM)
	},
	/* mismatch: FP16 without its mirror, the dual-source mirror without CAP_DUAL_SOURCE, no blitter */
	{
		GHCI_CAP_UMA | GHCI_CAP_HW_WALKER | GHCI_CAP_GSI1 | (8U << GHCI_CAP_PHYS_LANES_SHIFT) |
		    (1U << GHCI_CAP_NUM_SU_SHIFT) | GHCI_CAP_FP16,
		MODEL_VA32 | GHCI_FEATURE_DUAL_SOURCE | (4U << GHCI_FEATURE_MAX_MRT_SHIFT) | GHCI_FEATURE_CLIP_POLY,
		(4U << GHCI_FMT0_MAX_COLOR_SHIFT) | GHCI_FMT0_TU_BILINEAR,
		(1U << GHCI_EXTCAP_ID_VEND)
	},
	/*
	 * kc705-min: the KC705's minimal build as the specification reads
	 * it (DEVICE_CAPS 0x40010480, EXT_CAP_FEATURE 0x00110000,
	 * EXT_CAP_FMT0 0x00001200): discrete and flat, four lanes, one
	 * colour attachment, 1 KiB of shared memory, a VA of 30 bits, sRGB,
	 * the display, and none of the blitter, the polygon clipper, the
	 * bilinear filter, PROT, MSAA and FP16.
	 */
	{
		GHCI_CAP_GSI1 | (4U << GHCI_CAP_PHYS_LANES_SHIFT) | (1U << GHCI_CAP_NUM_SU_SHIFT) | GHCI_CAP_DISP,
		(1U << GHCI_FEATURE_MAX_MRT_SHIFT) | (1U << GHCI_FEATURE_LDS_KB_SHIFT),
		GHCI_FMT0_SRGB | (1U << GHCI_FMT0_MAX_COLOR_SHIFT),
		(1U << GHCI_EXTCAP_ID_VEND) | (1U << GHCI_EXTCAP_ID_FMT) | (1U << GHCI_EXTCAP_ID_DISP2) |
		    (1U << GHCI_EXTCAP_ID_MEM)
	},
	/* va30: kc705-uma with a VA of 30 bits (VA_BITS_ENC 0) */
	{
		GHCI_CAP_UMA | GHCI_CAP_HW_WALKER | GHCI_CAP_GSI1 | (8U << GHCI_CAP_PHYS_LANES_SHIFT) |
		    (1U << GHCI_CAP_NUM_SU_SHIFT) | GHCI_CAP_BLT | GHCI_CAP_DISP,
		GHCI_FEATURE_HW_CURSOR | (4U << GHCI_FEATURE_MAX_MRT_SHIFT) | (4U << GHCI_FEATURE_LDS_KB_SHIFT) |
		    MODEL_OPTIONAL_FEATURE,
		GHCI_FMT0_ETC2 | GHCI_FMT0_SRGB | (4U << GHCI_FMT0_MAX_COLOR_SHIFT) | MODEL_OPTIONAL_FMT0,
		(1U << GHCI_EXTCAP_ID_VEND) | (1U << GHCI_EXTCAP_ID_FMT) | (1U << GHCI_EXTCAP_ID_DISP2)
	},
	/* blt-min: full with a blitter that neither scales, blends nor tiles, and no bilinear filter */
	{
		GHCI_CAP_UMA | GHCI_CAP_IOMMU | GHCI_CAP_HW_WALKER | GHCI_CAP_INDIRECT_DRAW | GHCI_CAP_SECONDARY_RING |
		    GHCI_CAP_DUAL_SOURCE | GHCI_CAP_FP16 | GHCI_CAP_GSI1 | (32U << GHCI_CAP_PHYS_LANES_SHIFT) |
		    (2U << GHCI_CAP_NUM_SU_SHIFT) | GHCI_CAP_PROT | GHCI_CAP_TEX_FFILT | GHCI_CAP_ROP_PACK |
		    GHCI_CAP_BLT | GHCI_CAP_DISP | GHCI_CAP_MSAA,
		MODEL_VA32 | GHCI_FEATURE_DUAL_SOURCE | GHCI_FEATURE_COARSE_SCAN | GHCI_FEATURE_FAST_CLEAR |
		    GHCI_FEATURE_LOGIC_OP | GHCI_FEATURE_EARLY_Z | GHCI_FEATURE_HW_CURSOR | GHCI_FEATURE_COLOR_LUT |
		    GHCI_FEATURE_MRT_BLEND | (8U << GHCI_FEATURE_MAX_MRT_SHIFT) | (16U << GHCI_FEATURE_LDS_KB_SHIFT) |
		    GHCI_FEATURE_FP16 | GHCI_FEATURE_SUBGROUP | GHCI_FEATURE_CLIP_POLY,
		GHCI_FMT0_BC_MASK | GHCI_FMT0_ETC2 | GHCI_FMT0_SRGB | GHCI_FMT0_FFILT_FP16 | GHCI_FMT0_FFILT_FP32 |
		    (8U << GHCI_FMT0_MAX_COLOR_SHIFT),
		(1U << GHCI_EXTCAP_ID_VEND) | (1U << GHCI_EXTCAP_ID_PROT) | (1U << GHCI_EXTCAP_ID_FMT) |
		    (1U << GHCI_EXTCAP_ID_DISP2)
	}
};

/*
 * The blocks of the extended capability list in the order the list
 * names them, and where each block's registers are (section 4.1).
 */
static const uint32_t block_ids[] = {
	GHCI_EXTCAP_ID_FMT,
	GHCI_EXTCAP_ID_VEND,
	GHCI_EXTCAP_ID_PROT,
	GHCI_EXTCAP_ID_MEM,
	GHCI_EXTCAP_ID_DISP2
};
static const uint32_t block_offsets[] = {
	MODEL_FMT_BLOCK,
	MODEL_VEND_BLOCK,
	MODEL_PROT_BLOCK,
	MODEL_MEM_BLOCK,
	MODEL_DISP2_BLOCK
};

/*
 * The model every test runs.  model_init fills it; the host and local
 * memories stay allocated from the first model_init to model_fini.
 */
struct model model;

/* Nonzero makes FMT know R8_UNORM and D24S8 and D32S8 too (ghci-model.h). */
int model_more_formats;

/*
 * The L0 tables each page table has built: one bit per 2 MiB chunk a
 * mapping has touched since the table's root was made.  VM_UNMAP leaves
 * the bits (the CP returns tables only at SESSION_CLOSE and at a reset,
 * d9252e4 docs/ghci.md section 11.6 and 5.2); closing a PROT session and
 * a reset clear them.  marks_saved holds one row while a map may be
 * rolled back, and region_bits is heap_count's scratch.
 */
static uint8_t table_marks[MODEL_TABLES][MODEL_CHUNKS_MAX / 8U];
static uint8_t marks_saved[MODEL_CHUNKS_MAX / 8U];
static uint8_t region_bits[MODEL_REGIONS_MAX / 8U];

static uint32_t window_word(uint32_t offset);
static void window_set(uint32_t offset, uint32_t value);
static void build_window(void);
static void power_on(void);
static void do_reset(void);
static void start_running(void);
static uint32_t status_value(int consume);
static uint32_t fmt_features(uint32_t format);
static int has_guard(enum guard guard);
static const struct command_rule *find_rule(uint32_t opcode);
static int length_ok(const struct command_rule *rule, const uint32_t *packet, uint32_t dwords);
static enum packet_result run_packet(const uint32_t *packet, uint32_t dwords);
static enum packet_result run_session_open(const uint32_t *packet);
static enum packet_result run_session_close(const uint32_t *packet);
static enum packet_result run_vm_map(const uint32_t *packet);
static enum packet_result run_vm_unmap(const uint32_t *packet);
static enum packet_result run_fence(const uint32_t *packet);
static enum packet_result run_wait_fence(const uint32_t *packet);
static void run_fill(const uint32_t *packet, uint32_t session);
static void run_copy(const uint32_t *packet, uint32_t session);
static int local_range(uint64_t phys, uint64_t bytes);
static void record(const uint32_t *packet, uint32_t dwords, uint32_t skipped);
static enum packet_result raise_fault(uint32_t kind, uint32_t unit, uint32_t session, uint64_t address);
static void irq_check(void);
static uint32_t table_of(uint32_t session);
static uint32_t root_bytes(void);
static uint64_t heap_count(void);
static void marks_add(uint32_t table, uint64_t va, uint64_t bytes);
static uint64_t heap_capacity(void);
static int table_overlaps(uint32_t table, uint64_t va, uint64_t bytes);
static void table_remove(uint32_t table, uint64_t va, uint64_t bytes, uint32_t session, int by_session);
static uint64_t get64(const uint32_t *words);
static int disp_register(uint32_t offset, uint32_t *reg);
static uint32_t disp_read(uint32_t reg);
static void disp_write(uint32_t reg, uint32_t value);
static void disp_scan_write(uint32_t reg, uint32_t value, int ring);
static enum packet_result run_disp_set(const uint32_t *packet, uint32_t dwords);
static enum packet_result run_flip(const uint32_t *packet, uint32_t dwords);
static void disp_take(unsigned plane, int cancelled);
static void disp_record(unsigned plane, uint32_t value, uint32_t record_pa);
static void disp_flush_due(void);
static void disp_event(uint32_t events);
static void disp_irq_update(void);
static void disp_reset(void);

/*
 * Builds the model of one profile, powered on and idle.
 */
void
model_init(
	enum model_profile profile,
	const struct model_switches *switches)
{
	uint8_t *host;
	uint8_t *local;

	/* Keeps the memories across models; the first model allocates them. */
	host = model.host;
	local = model.local;
	if (host == NULL)
		host = calloc(1, MODEL_HOST_BYTES);
	if (local == NULL)
		local = calloc(1, MODEL_LOCAL_BYTES);

	/* Starts from nothing, with the memories cleared. */
	memset(&model, 0, sizeof(model));
	model.host = host;
	model.local = local;
	if (model.host != NULL)
		memset(model.host, 0, MODEL_HOST_BYTES);
	if (model.local != NULL)
		memset(model.local, 0, MODEL_LOCAL_BYTES);

	/* Takes the profile and the readings of the specification. */
	model.profile = profile;
	if (switches != NULL)
		model.switches = *switches;

	/* Fills the register window and powers the device on. */
	build_window();
	power_on();
}

/*
 * Frees the model's memories.
 */
void
model_fini(void)
{
	/* Frees the memories the first model_init allocated. */
	free(model.host);
	free(model.local);
	model.host = NULL;
	model.local = NULL;
}

/*
 * Reads one register of the window, as the driver's MMIO read sees it.
 */
uint32_t
model_read(
	uint32_t offset)
{
	uint32_t value;
	uint32_t reg;
	int display;

	/* The display controller's block answers for itself. */
	display = disp_register(offset, &reg);
	if (display)
		return disp_read(reg);

	/* Picks the registers the device computes. */
	switch (offset) {
	case GHCI_REG_STATUS:
		value = status_value(1);
		break;
	case GHCI_REG_CTRL:
		/* RESET reads 1 until the reset ends; a hung reset never does. */
		value = model.ctrl;
		if (model.reset_hung) {
			value |= GHCI_CTRL_RESET;
		} else if (model.reset_reads_left != 0U) {
			value |= GHCI_CTRL_RESET;
			model.reset_reads_left--;
		}
		break;
	case GHCI_REG_FAULT_INFO:
		value = model.fault_info;
		break;
	case GHCI_REG_FAULT_ADDR_LO:
		value = (uint32_t)model.fault_address;
		break;
	case GHCI_REG_FAULT_ADDR_HI:
		value = (uint32_t)(model.fault_address >> 32);
		break;
	case GHCI_REG_RING_WPTR:
		value = model.ring_wptr;
		break;
	case GHCI_REG_RING_RPTR:
		value = model.ring_rptr;
		break;
	case GHCI_REG_IRQ_STATUS:
		value = model.irq_status;
		break;
	case GHCI_REG_IRQ_ENABLE:
		value = model.irq_enable;
		break;
	case GHCI_REG_SESSION_ACTIVE:
		value = model.session_active;
		break;
	case GHCI_REG_TIMESTAMP_LO:
		value = (uint32_t)model.ticks;
		break;
	case GHCI_REG_TIMESTAMP_HI:
		value = (uint32_t)(model.ticks >> 32);
		break;
	case GHCI_REG_DIAG_CP_PC:
		value = model.ring_rptr * 4U;
		break;
	case GHCI_REG_DIAG_CP_STATUS:
		value = (status_value(0) >> GHCI_STATUS_CP_STATE_SHIFT) & GHCI_STATUS_CP_STATE_MASK;
		break;
	case MODEL_FMT_BLOCK + 0x4U:
	case MODEL_FMT_BLOCK + 0x8U:
	case MODEL_FMT_BLOCK + 0xCU:
		/* The features of the format FMT_INDEX names, the same for every tiling. */
		value = fmt_features(window_word(MODEL_FMT_BLOCK));
		break;
	default:
		/* Every other register reads what the window holds; past the window, 0. */
		value = window_word(offset);
		break;
	}

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes one register of the window, as the driver's MMIO write does.
 */
void
model_write(
	uint32_t offset,
	uint32_t value)
{
	uint32_t old_ctrl;
	uint32_t reg;
	int display;

	/* The display controller's block takes its own writes. */
	display = disp_register(offset, &reg);
	if (display) {
		disp_write(reg, value);
		irq_check();
		return;
	}

	/* Picks what the register does when written. */
	switch (offset) {
	case GHCI_REG_MAGIC:
	case GHCI_REG_VERSION:
	case GHCI_REG_DEVICE_CAPS:
	case GHCI_REG_EXTCAP_PTR:
	case GHCI_REG_STATUS:
	case GHCI_REG_RING_RPTR:
	case GHCI_REG_RESIDENT_WARPS:
	case GHCI_REG_SESSION_ACTIVE:
	case GHCI_REG_TIMESTAMP_LO:
	case GHCI_REG_TIMESTAMP_HI:
	case GHCI_REG_GCLK_HZ:
	case GHCI_REG_NATIVE_ISA_ID:
	case GHCI_REG_DIAG_CP_PC:
	case GHCI_REG_DIAG_CP_STATUS:
	case GHCI_REG_EXT_CAP_FMT0:
	case GHCI_REG_EXT_CAP_FEATURE:
		/* A read-only register ignores the write. */
		break;
	case GHCI_REG_IRQ_STATUS:
		/* Write-1-to-clear; the display's summary stays while one of its events is pending. */
		model.irq_status &= ~value;
		disp_irq_update();
		break;
	case GHCI_REG_IRQ_ENABLE:
		/* A pending event raises the line as soon as it is enabled. */
		model.irq_enable = value;
		irq_check();
		break;
	case GHCI_REG_IRQ_SET:
		/* Only SOFT can be set by the host (section 3.4). */
		model.irq_status |= value & GHCI_IRQ_SOFT;
		irq_check();
		break;
	case GHCI_REG_FAULT_INFO:
		/* Writing back the value read clears the record and re-arms it (N05). */
		if (value == model.fault_info) {
			model.fault_info = 0U;
			model.fault_address = 0U;
		}
		break;
	case GHCI_REG_CTRL:
		/* A reset takes every state and ends the write. */
		if ((value & GHCI_CTRL_RESET) != 0U) {
			do_reset();
			break;
		}

		/* A faulted device takes nothing but a reset (section 5.2). */
		if (model.state == MODEL_FAULTED) {
			model.ctrl = value;
			break;
		}

		/* Keeps the bits, then moves the state on what changed. */
		old_ctrl = model.ctrl;
		model.ctrl = value;
		if ((value & GHCI_CTRL_RUN) != 0U && model.state == MODEL_IDLE)
			start_running();
		if ((value & GHCI_CTRL_RUN) == 0U && model.state == MODEL_RUNNING)
			model.state = MODEL_IDLE;
		if ((value & GHCI_CTRL_HALT) != 0U && (old_ctrl & GHCI_CTRL_HALT) == 0U && model.state == MODEL_RUNNING) {
			/* The model's packets are whole, so the halt is at once; STATUS may show it later. */
			model.state = MODEL_HALTED;
			model.halts++;
			model.halt_reads_left = model.switches.halt_reads;
			model.irq_status |= GHCI_IRQ_HALTED;
		}
		if ((value & GHCI_CTRL_HALT) == 0U && model.state == MODEL_HALTED) {
			/* Leaving HALTED goes back to RUNNING or IDLE by RUN. */
			model.state = MODEL_IDLE;
			if ((value & GHCI_CTRL_RUN) != 0U)
				model.state = MODEL_RUNNING;
		}

		/* Records due go out once the processor leaves HALTED; it runs what the doorbell left. */
		disp_flush_due();
		model_run();
		irq_check();
		break;
	case GHCI_REG_RING_BASE_LO:
	case GHCI_REG_RING_BASE_HI:
	case GHCI_REG_RING_SIZE:
	case GHCI_REG_CP_HEAP_BASE:
		/* These are written only while IDLE; otherwise the write is refused (kind 7). */
		if (model.state != MODEL_IDLE) {
			model.locked_writes++;
			raise_fault(GHCI_FAULT_KIND_LOCKED_REG, GHCI_UNIT_CP, 0U, offset);
			irq_check();
			break;
		}
		window_set(offset, value);
		break;
	case GHCI_REG_CP_HEAP_LIMIT:
		/* While not IDLE only a contiguous expansion is taken (N09). */
		if (model.state != MODEL_IDLE && value < window_word(offset)) {
			model.locked_writes++;
			raise_fault(GHCI_FAULT_KIND_LOCKED_REG, GHCI_UNIT_CP, 0U, offset);
			irq_check();
			break;
		}
		window_set(offset, value);
		break;
	case GHCI_REG_SCRATCH:
		/* A stuck window keeps what it read before. */
		if (!model.switches.scratch_stuck)
			window_set(offset, value);
		break;
	case GHCI_REG_RING_WPTR:
		/* The doorbell: the processor reads up to the new write index. */
		model.ring_wptr = value;
		model.doorbells++;
		model_run();
		irq_check();
		break;
	default:
		/* Every other register keeps what is written. */
		window_set(offset, value);
		break;
	}
}

/*
 * Lets the command processor read the ring up to RING_WPTR.
 */
void
model_run(void)
{
	enum packet_result result;
	uint8_t *ring_bytes;
	uint32_t *ring;
	uint64_t ring_base;
	uint32_t ring_dwords;
	uint32_t header;
	uint32_t dwords;
	uint32_t ready;
	unsigned packets;

	/* Only a running processor that the test does not hold reads the ring. */
	if (model.state != MODEL_RUNNING || model.hold)
		return;
	if (model.ring_rptr == model.ring_wptr)
		return;

	/* Without Bus Master the processor cannot fetch; the attempt is counted. */
	if (!model.bus_master) {
		model.dma_without_master++;
		return;
	}

	/* Finds the ring in the GPU's memory; a ring outside it is a bus error. */
	ring_base = ((uint64_t)window_word(GHCI_REG_RING_BASE_HI) << 32) | window_word(GHCI_REG_RING_BASE_LO);
	ring_dwords = window_word(GHCI_REG_RING_SIZE) / 4U;
	ring_bytes = model_dma(ring_base, (uint64_t)ring_dwords * 4U);
	if (ring_bytes == NULL || ring_dwords == 0U) {
		raise_fault(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, ring_base);
		return;
	}
	ring = (uint32_t *)(void *)ring_bytes;

	/* A write index past the ring is out of bounds (kind 2). */
	if (model.ring_wptr >= ring_dwords) {
		raise_fault(GHCI_FAULT_KIND_KICK_ABSENT, GHCI_UNIT_CP, 0U, model.ring_wptr);
		return;
	}

	/* Parses each packet between the read and the write index. */
	model.waiting_fence = 0;
	for (packets = 0U;
	     model.ring_rptr != model.ring_wptr && packets < MODEL_PACKETS_PER_RUN;
	     packets++) {
		/* Reads the header and the dwords the doorbell made ready. */
		header = ring[model.ring_rptr];
		dwords = (header >> GHCI_HEADER_DWORDS_SHIFT) & GHCI_HEADER_DWORDS_MASK;
		ready = (model.ring_wptr + ring_dwords - model.ring_rptr) % ring_dwords;

		/* A packet of no dwords, past the end of the ring or past the write index is a framing error. */
		if (dwords == 0U || model.ring_rptr + dwords > ring_dwords || dwords > ready) {
			raise_fault(GHCI_FAULT_KIND_BAD_REG, GHCI_UNIT_CP, model.bound_session, model.ring_rptr * 4U);
			break;
		}

		/* Runs the packet; a wait or a device fault leaves the read index on it. */
		result = run_packet(&ring[model.ring_rptr], dwords);
		if (result != PACKET_NEXT)
			break;

		/* Retires the packet; the end of the ring wraps to 0. */
		model.ring_rptr = (model.ring_rptr + dwords) % ring_dwords;
	}
}

/*
 * Advances the model's clock by one tick and lets a waiting processor go on.
 */
void
model_tick(void)
{
	/* One tick of TIMESTAMP and a frame of the display, then the ring again (a WAIT_FENCE or a FLIP may pass now). */
	model.ticks++;
	model_display_vsync();
	disp_flush_due();
	model_run();
	irq_check();
}

/*
 * Sets the PCI command register's Bus Master bit as the model sees it.
 */
void
model_set_bus_master(
	int enabled)
{
	/* Records the bit; a doorbell left waiting runs when it is set. */
	model.bus_master = enabled;
	model_run();
	irq_check();
}

/*
 * Sets the handler the model calls while its interrupt line is high.
 */
void
model_set_irq_handler(
	model_irq_handler handler,
	void *argument)
{
	/* Remembers the handler and its argument. */
	model.irq_handler = handler;
	model.irq_argument = argument;
}

/*
 * Reports the level of the interrupt line (section 3.4).
 */
int
model_irq_line(void)
{
	/* The line needs IRQ_GLOBAL and an enabled pending event. */
	if ((model.ctrl & GHCI_CTRL_IRQ_GLOBAL) == 0U)
		return 0;
	if ((model.irq_status & model.irq_enable) == 0U)
		return 0;

	/* Succeeded: the line is high. */
	return 1;
}

/*
 * Raises a fault now, or arms it for the next draw or dispatch of a session.
 */
void
model_inject(
	uint32_t kind,
	uint32_t unit,
	uint32_t session,
	uint64_t address,
	int at_draw)
{
	/* A fault at a draw waits for that draw. */
	if (at_draw) {
		model.injection.armed = 1;
		model.injection.at_draw = 1;
		model.injection.kind = kind;
		model.injection.unit = unit;
		model.injection.session = session;
		model.injection.address = address;
		return;
	}

	/* Raises it at once, and the line follows. */
	raise_fault(kind, unit, session, address);
	irq_check();
}

/*
 * Raises a watchdog expiry of a session's packet now (section 9.2).
 */
void
model_inject_watchdog(
	uint32_t session)
{
	/* The watchdog sets its own bit with the fault (kind 4). */
	model.irq_status |= GHCI_IRQ_WATCHDOG;
	raise_fault(GHCI_FAULT_KIND_WATCHDOG, GHCI_UNIT_CP, session, model.ring_rptr * 4U);
	irq_check();
}

/*
 * Holds the command processor (a hang without a fault) or lets it go.
 */
void
model_hold(
	int held)
{
	/* A released processor reads what was left. */
	model.hold = held;
	model_run();
	irq_check();
}

/*
 * Hands out a run of the model's host memory below a bus address.
 */
int
model_host_alloc(
	uint64_t bytes,
	uint64_t alignment,
	uint64_t max_address,
	uint64_t *bus)
{
	uint64_t start;

	/* Refuses an alignment that is not a power of two. */
	if (alignment == 0U || (alignment & (alignment - 1U)) != 0U)
		return 1;

	/* Takes the next aligned run, if it fits and the device can name it. */
	start = (model.host_next + alignment - 1U) & ~(alignment - 1U);
	if (start + bytes > MODEL_HOST_BYTES)
		return 1;
	if (MODEL_HOST_BASE + start + bytes - 1U > max_address)
		return 1;

	/* Never reuses a run, so a stale address of the driver shows. */
	model.host_next = start + bytes;
	*bus = MODEL_HOST_BASE + start;

	/* Succeeded: the run's bus address. */
	return 0;
}

/*
 * Finds the bytes the GPU reaches at a bus address.
 */
uint8_t *
model_dma(
	uint64_t bus,
	uint64_t bytes)
{
	/* Host memory. */
	if (bus >= MODEL_HOST_BASE && bus + bytes <= MODEL_HOST_BASE + MODEL_HOST_BYTES && model.host != NULL)
		return &model.host[bus - MODEL_HOST_BASE];

	/* Local memory, which only the discrete profiles have. */
	if ((window_word(GHCI_REG_DEVICE_CAPS) & GHCI_CAP_UMA) == 0U &&
	    (profiles[model.profile].blocks & (1U << GHCI_EXTCAP_ID_MEM)) != 0U &&
	    bus >= MODEL_LOCAL_BASE &&
	    bus + bytes <= MODEL_LOCAL_BASE + MODEL_LOCAL_BYTES &&
	    model.local != NULL)
		return &model.local[bus - MODEL_LOCAL_BASE];

	/* Nothing answers there. */
	return NULL;
}

/*
 * Reports the CP_HEAP bytes the page tables take now.
 */
uint64_t
model_heap_bytes(void)
{
	/* The tables of the live sessions, and what closed sessions left behind. */
	return model.heap_used;
}

/*
 * Finds the mapping that translates a session's GPU VA.
 */
const struct model_mapping *
model_lookup(
	uint32_t session,
	uint64_t va)
{
	const struct model_mapping *mapping;
	uint32_t table;
	uint32_t index;

	/* Picks the session's table; a session out of range has none. */
	if (session >= MODEL_SESSIONS)
		return NULL;
	table = table_of(session);

	/* Searches the table for a range that holds the address. */
	for (index = 0U; index < model.table_mappings[table]; index++) {
		/* A range holds the address when the address is inside it. */
		mapping = &model.tables[table][index];
		if (va >= mapping->va && va - mapping->va < mapping->bytes)
			return mapping;
	}

	/* Nothing maps the address. */
	return NULL;
}

/*
 * Counts the recorded commands of one opcode.
 */
unsigned
model_count_commands(
	uint32_t opcode)
{
	unsigned count;
	unsigned index;

	/* Counts every record of the opcode. */
	count = 0U;
	for (index = 0U; index < model.command_count; index++) {
		/* Only the commands of this opcode count. */
		if (model.commands[index].opcode == opcode)
			count++;
	}

	/* Succeeded: the count. */
	return count;
}

/*
 * Finds the last recorded command of one opcode.
 */
const struct model_command *
model_last_command(
	uint32_t opcode)
{
	unsigned index;

	/* Searches the records from the newest. */
	for (index = model.command_count; index > 0U; index--) {
		/* The newest record of the opcode is the answer. */
		if (model.commands[index - 1U].opcode == opcode)
			return &model.commands[index - 1U];
	}

	/* No command of the opcode ran. */
	return NULL;
}

/* Reads one word of the register window; past the window, 0. */
static uint32_t
window_word(
	uint32_t offset)
{
	/* Past the window nothing answers. */
	if (offset >= MODEL_WINDOW_BYTES)
		return 0U;

	/* Succeeded: the stored word. */
	return model.window[offset / 4U];
}

/* Stores one word of the register window; past the window the write is dropped. */
static void
window_set(
	uint32_t offset,
	uint32_t value)
{
	/* Past the window nothing is stored. */
	if (offset >= MODEL_WINDOW_BYTES)
		return;

	/* Stores the word. */
	model.window[offset / 4U] = value;
}

/* Fills the register window of the profile: the identity, capabilities, list and blocks. */
static void
build_window(void)
{
	const struct profile *profile;
	uint32_t entry;
	uint32_t index;
	uint32_t count;
	uint32_t found;
	uint32_t blocks;

	/*
	 * Values referenced: amaterrace-gpu 20bf888 docs/ghci.md
	 * (section 3.1, the identity registers; section 3.2, the
	 * capabilities; section 4.1, the list and its blocks).
	 */
	profile = &profiles[model.profile];
	window_set(GHCI_REG_MAGIC, GHCI_MAGIC_VALUE);
	window_set(GHCI_REG_VERSION, MODEL_VERSION);
	window_set(GHCI_REG_DEVICE_CAPS, profile->device_caps);
	window_set(GHCI_REG_EXT_CAP_FEATURE, profile->feature);
	window_set(GHCI_REG_EXT_CAP_FMT0, profile->fmt0);
	window_set(GHCI_REG_GCLK_HZ, MODEL_GCLK_HZ);
	window_set(GHCI_REG_NATIVE_ISA_ID, GHCI_NATIVE_ISA_GSI1);
	window_set(GHCI_REG_RESIDENT_WARPS, MODEL_RESIDENT_WARPS);
	window_set(GHCI_REG_WDT_CYCLES, MODEL_WDT_DEFAULT);

	/* Counts the blocks so that the last entry ends the list. */
	blocks = profile->blocks;
	count = 0U;
	for (index = 0U; index < sizeof(block_ids) / sizeof(block_ids[0]); index++) {
		/* Only the profile's blocks are listed. */
		if ((blocks & (1U << block_ids[index])) != 0U)
			count++;
	}

	/* A profile without blocks has no list. */
	if (count == 0U)
		return;
	window_set(GHCI_REG_EXTCAP_PTR, MODEL_EXTCAP_LIST);

	/* Writes one entry per block: ID, version 1, the distance to the next, the block's offset. */
	entry = MODEL_EXTCAP_LIST;
	found = 0U;
	for (index = 0U; index < sizeof(block_ids) / sizeof(block_ids[0]); index++) {
		/* Skips a block the profile does not have. */
		if ((blocks & (1U << block_ids[index])) == 0U)
			continue;

		/* The last entry has distance 0; the others point at the next entry, two dwords on. */
		found++;
		if (found == count)
			window_set(entry, block_ids[index] | (1U << 8));
		else
			window_set(entry, block_ids[index] | (1U << 8) | (2U << 16));
		window_set(entry + 4U, block_offsets[index]);
		entry += 8U;
	}

	/* VEND: the PCI identity and the clock. */
	if ((blocks & (1U << GHCI_EXTCAP_ID_VEND)) != 0U) {
		window_set(MODEL_VEND_BLOCK + GHCI_VEND_VENDOR_ID, MODEL_VENDOR_ID);
		window_set(MODEL_VEND_BLOCK + GHCI_VEND_DEVICE_ID, MODEL_DEVICE_ID);
		window_set(MODEL_VEND_BLOCK + 0x10U, MODEL_GCLK_HZ);
	}

	/* PROT: the sessions it protects. */
	if ((blocks & (1U << GHCI_EXTCAP_ID_PROT)) != 0U)
		window_set(MODEL_PROT_BLOCK + GHCI_PROT_CAPS, MODEL_PROT_SESSIONS);

	/* MEM: local memory, all of it in the window, and the host limit. */
	if ((blocks & (1U << GHCI_EXTCAP_ID_MEM)) != 0U) {
		window_set(MODEL_MEM_BLOCK + GHCI_MEM_LOCAL_BASE, (uint32_t)MODEL_LOCAL_BASE);
		window_set(MODEL_MEM_BLOCK + GHCI_MEM_LOCAL_SIZE, MODEL_LOCAL_BYTES);
		window_set(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_SIZE, MODEL_LOCAL_BYTES);
		window_set(MODEL_MEM_BLOCK + GHCI_MEM_HOST_LIMIT, MODEL_HOST_LIMIT);

		/* The window is open after a power-on reset (section 11.1). */
		window_set(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL, 1U);
	}

	/* FMT: the attachment limit. */
	if ((blocks & (1U << GHCI_EXTCAP_ID_FMT)) != 0U)
		window_set(MODEL_FMT_BLOCK + 0x10U, MODEL_FMT_LIMITS);

	/* DISP2: the controller's identity, two modes, the cursor plane, both formats, a sink connected. */
	if ((blocks & (1U << GHCI_EXTCAP_ID_DISP2)) != 0U) {
		window_set(MODEL_DISP2_BLOCK + GHCI_DC_ID, MODEL_DC_ID);
		window_set(MODEL_DISP2_BLOCK + GHCI_DC_VERSION, MODEL_VERSION);
		window_set(MODEL_DISP2_BLOCK + GHCI_DC_CAPS, MODEL_DC_CAPS);
		window_set(MODEL_DISP2_BLOCK + GHCI_DC_CONNECTED, 1U);
	}
}

/* Puts the device in its power-on state: idle, nothing open, the heap unset. */
static void
power_on(void)
{
	/* Power-on is a reset with the preserved registers at their defaults. */
	window_set(GHCI_REG_CP_HEAP_BASE, 0U);
	window_set(GHCI_REG_CP_HEAP_LIMIT, 0U);
	do_reset();
	model.resets = 0U;
	model.reset_reads_left = 0U;
	model.mem_ready_reads_left = 0U;
}

/* Runs CTRL.RESET: what it keeps and what it clears (section 5.2, N08). */
static void
do_reset(void)
{
	uint32_t table;

	/* Counts the reset; a hung reset never ends and reports AXI_STUCK. */
	model.resets++;
	if (model.switches.reset_stuck) {
		model.reset_hung = 1;
		return;
	}

	/* Clears the ring indices, interrupts, fault record and execution bits. */
	model.ring_wptr = 0U;
	model.ring_rptr = 0U;
	model.irq_status = 0U;
	model.fault_info = 0U;
	model.fault_address = 0U;
	model.ctrl = 0U;

	/* Clears the sessions and every page table; CP_HEAP is empty again. */
	model.session_active = 0U;
	model.session_faulted = 0U;
	model.bound_session = 0U;
	for (table = 0U; table < MODEL_TABLES; table++) {
		model.table_mappings[table] = 0U;
		model.table_root[table] = 0;
		memset(table_marks[table], 0, sizeof(table_marks[table]));
	}
	model.heap_used = 0U;
	model.heap_leaked = 0U;
	model.heap_ready = 0;

	/* Stops the processor; a held processor and an armed fault are gone too. */
	model.state = MODEL_IDLE;
	model.waiting_fence = 0;
	model.hold = 0;
	model.injection.armed = 0;

	/* CTRL.RESET and MEM_READY may take some reads to show the end. */
	model.reset_reads_left = model.switches.reset_reads;
	model.mem_ready_reads_left = model.switches.mem_ready_reads;

	/* The display controller is not reset; a record due goes out now that the processor is idle. */
	disp_flush_due();
}

/* Moves IDLE to RUNNING; the first run after a reset builds session 0's root (N07). */
static void
start_running(void)
{
	/* Runs. */
	model.state = MODEL_RUNNING;
	if (model.heap_ready)
		return;

	/* Builds session 0's root in CP_HEAP; without room session 0 faults and the device with it. */
	model.heap_ready = 1;
	model.table_root[0] = 1;
	model.session_active |= 1U;
	model.heap_used = heap_count();
	if (model.heap_used > heap_capacity()) {
		model.table_root[0] = 0;
		model.heap_used = 0U;
		raise_fault(GHCI_FAULT_KIND_MMU_OOM, GHCI_UNIT_CP, 0U, window_word(GHCI_REG_CP_HEAP_BASE));
	}
}

/*
 * Computes STATUS (section 3.3).  With consume, a read counts down the
 * reads before MEM_READY and HALTED show; DIAG_CP_STATUS does not.
 */
static uint32_t
status_value(
	int consume)
{
	uint32_t value;
	uint32_t cp_state;

	/* The memory controller is idle in the model between packets; the ring is empty when the indices meet. */
	value = GHCI_STATUS_MC_IDLE;
	if (model.ring_rptr == model.ring_wptr)
		value |= GHCI_STATUS_RING_EMPTY;

	/* MEM_READY may take some reads to show after a reset. */
	if (model.mem_ready_reads_left == 0U)
		value |= GHCI_STATUS_MEM_READY;
	else if (consume)
		model.mem_ready_reads_left--;

	/* The lifecycle: a halt may show late, while the device still reads as running. */
	cp_state = 0U;
	if (model.state == MODEL_RUNNING) {
		value |= GHCI_STATUS_RUNNING;
	} else if (model.state == MODEL_HALTED) {
		if (model.halt_reads_left != 0U) {
			if (consume)
				model.halt_reads_left--;
			value |= GHCI_STATUS_RUNNING;
		} else {
			value |= GHCI_STATUS_HALTED;
		}
	} else if (model.state == MODEL_FAULTED) {
		value |= GHCI_STATUS_FAULTED | GHCI_STATUS_QUIESCE;
		cp_state = 7U;
	}

	/* A processor that waits for a fence is in WAIT_FENCE. */
	if (model.waiting_fence && model.state == MODEL_RUNNING)
		cp_state = 4U;

	/* A hung reset reports a stuck interconnect. */
	if (model.reset_hung)
		value |= GHCI_STATUS_AXI_STUCK;

	/* Succeeded: the register. */
	return value | (cp_state << GHCI_STATUS_CP_STATE_SHIFT);
}

/* Answers FMT_OPTIMAL, FMT_LINEAR and FMT_BUFFER for a VkFormat. */
static uint32_t
fmt_features(
	uint32_t format)
{
	/* The model knows the common colour formats and one depth format. */
	switch (format) {
	case 37U:
	case 43U:
	case 44U:
	case 50U:
		return MODEL_FMT_ALL;
	case 126U:
		return 0x011U;
	default:
		break;
	}

	/* The formats the transfer test adds. */
	if (model_more_formats && format == 9U)
		return MODEL_FMT_ALL;
	if (model_more_formats && (format == 129U || format == 130U))
		return 0x011U;
	return 0U;
}

/* Reports whether the device has a command's capability. */
static int
has_guard(
	enum guard guard)
{
	uint32_t caps;

	/* Reads the capability the guard names. */
	caps = window_word(GHCI_REG_DEVICE_CAPS);
	switch (guard) {
	case GUARD_NONE:
		return 1;
	case GUARD_WALKER:
		return (caps & GHCI_CAP_HW_WALKER) != 0U;
	case GUARD_SU:
		return ((caps >> GHCI_CAP_NUM_SU_SHIFT) & 0xFFU) != 0U;
	case GUARD_INDIRECT:
		return (caps & GHCI_CAP_INDIRECT_DRAW) != 0U;
	case GUARD_MESH:
		return (caps & GHCI_CAP_MESH_SHADER) != 0U;
	case GUARD_SECONDARY:
		return (caps & GHCI_CAP_SECONDARY_RING) != 0U;
	case GUARD_BLT:
		return (caps & GHCI_CAP_BLT) != 0U;
	case GUARD_DISP:
		return (caps & GHCI_CAP_DISP) != 0U;
	default:
		return 0;
	}
}

/* Finds the row of an opcode in the command table. */
static const struct command_rule *
find_rule(
	uint32_t opcode)
{
	uint32_t index;

	/* Searches the table. */
	for (index = 0U; index < sizeof(command_rules) / sizeof(command_rules[0]); index++) {
		/* The row of the opcode is the answer. */
		if (command_rules[index].opcode == opcode)
			return &command_rules[index];
	}

	/* An opcode GHCI 1.0 does not define. */
	return NULL;
}

/* Reports whether a packet's length is one its opcode allows. */
static int
length_ok(
	const struct command_rule *rule,
	const uint32_t *packet,
	uint32_t dwords)
{
	uint32_t count;

	/* Picks the form of the opcode's length. */
	switch (rule->length) {
	case LENGTH_FIXED:
		return dwords == rule->dwords;
	case LENGTH_NOP:
		/* A NOP of any length 1 to 255. */
		return 1;
	case LENGTH_VERTEX_BUFFERS:
		/* 2 + 4N, N in dword 1 [31:16], at least one binding. */
		if (dwords < 2U)
			return 0;
		count = packet[1] >> 16;
		return count != 0U && dwords == 2U + 4U * count;
	case LENGTH_LOAD_CONST:
		/* 3 + N with at least one dword. */
		return dwords > 3U;
	case LENGTH_DISP_SET:
		/* 1 + 2N with at least one pair (N70). */
		return dwords >= 3U && (dwords & 1U) == 1U;
	case LENGTH_DRAW_INDIRECT:
		/* 7, or 8 with the count buffer. */
		return dwords == GHCI_DW_DRAW_INDIRECT || dwords == GHCI_DW_DRAW_INDIRECT_COUNT;
	default:
		return 0;
	}
}

/* Runs one packet whose framing is good. */
static enum packet_result
run_packet(
	const uint32_t *packet,
	uint32_t dwords)
{
	const struct command_rule *rule;
	uint32_t opcode;
	uint32_t session;
	int guarded;
	int fits;

	/* An opcode GHCI 1.0 does not define is kind 1. */
	opcode = packet[0] & GHCI_HEADER_OPCODE_MASK;
	rule = find_rule(opcode);
	if (rule == NULL)
		return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, model.bound_session, opcode);

	/* A length the opcode does not allow is a framing error (kind 3). */
	fits = length_ok(rule, packet, dwords);
	if (!fits)
		return raise_fault(GHCI_FAULT_KIND_BAD_REG, GHCI_UNIT_CP, model.bound_session, opcode);

	/* A command whose unit the device does not have is kind 2. */
	guarded = has_guard(rule->guard);
	if (!guarded)
		return raise_fault(GHCI_FAULT_KIND_KICK_ABSENT, GHCI_UNIT_CP, model.bound_session, opcode);

	/* Picks what the processor does with the command. */
	switch (opcode) {
	case GHCI_OP_NOP:
		return PACKET_NEXT;
	case GHCI_OP_SESSION_OPEN:
		record(packet, dwords, 0U);
		return run_session_open(packet);
	case GHCI_OP_SESSION_CLOSE:
		record(packet, dwords, 0U);
		return run_session_close(packet);
	case GHCI_OP_BIND_SESSION:
		/* Binding a session that is not open is kind 1. */
		record(packet, dwords, 0U);
		session = packet[1];
		if (session >= MODEL_SESSIONS || (model.session_active & (1U << session)) == 0U)
			return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, model.bound_session, session);
		model.bound_session = session;
		return PACKET_NEXT;
	case GHCI_OP_VM_MAP:
		record(packet, dwords, 0U);
		return run_vm_map(packet);
	case GHCI_OP_VM_UNMAP:
		record(packet, dwords, 0U);
		return run_vm_unmap(packet);
	case GHCI_OP_FENCE:
		record(packet, dwords, 0U);
		return run_fence(packet);
	case GHCI_OP_WAIT_FENCE:
		return run_wait_fence(packet);
	case GHCI_OP_DISP_SET:
		record(packet, dwords, 0U);
		return run_disp_set(packet, dwords);
	case GHCI_OP_FLIP:
		return run_flip(packet, dwords);
	default:
		break;
	}

	/* The work of a session that faulted is not done (PROT = 1, section 11.6). */
	session = model.bound_session;
	if ((model.session_faulted & (1U << session)) != 0U) {
		record(packet, dwords, 1U);
		return PACKET_NEXT;
	}

	/* A fault the test armed for this session's next draw fires here. */
	record(packet, dwords, 0U);
	if (rule->draw && model.injection.armed && model.injection.session == session) {
		model.injection.armed = 0;
		return raise_fault(model.injection.kind, model.injection.unit, session, model.injection.address);
	}

	/* A fill writes its pattern where the session maps it; a copy copies between its mappings. */
	if (opcode == GHCI_OP_FILL_BUFFER)
		run_fill(packet, session);
	if (opcode == GHCI_OP_COPY_BUFFER)
		run_copy(packet, session);

	/* Succeeded: the command is recorded. */
	return PACKET_NEXT;
}

/*
 * Runs CMD_FILL_BUFFER (section 7.3): the pattern in every word of the
 * range, when one mapping of the session holds all of it.  A range the
 * session does not map is only recorded (the model checks the driver's
 * packets, not a client's addresses).
 */
static void
run_fill(
	const uint32_t *packet,
	uint32_t session)
{
	const struct model_mapping *mapping;
	uint64_t va;
	uint64_t bytes;
	uint64_t index;
	uint8_t *memory;
	uint32_t pattern;

	/* The range and the pattern. */
	va = get64(&packet[1]);
	bytes = packet[3];
	pattern = packet[4];

	/* One mapping holds the whole range. */
	mapping = model_lookup(session, va);
	if (mapping == NULL || bytes == 0U || va - mapping->va + bytes > mapping->bytes)
		return;

	/* The bytes the mapping reaches. */
	memory = model_dma(mapping->phys + (va - mapping->va), bytes);
	if (memory == NULL)
		return;

	/* The pattern, word by word. */
	for (index = 0U; index + 4U <= bytes; index += 4U)
		memcpy(&memory[index], &pattern, 4U);
}

/*
 * Runs CMD_COPY_BUFFER (section 7.3) without its fill form: the bytes
 * from the source range to the destination range, when one mapping of
 * the session holds each.  Other copies are only recorded.
 */
static void
run_copy(
	const uint32_t *packet,
	uint32_t session)
{
	const struct model_mapping *source;
	const struct model_mapping *target;
	uint64_t from;
	uint64_t to;
	uint64_t bytes;
	uint8_t *source_memory;
	uint8_t *target_memory;

	/* The ranges; a fill pattern is not a copy. */
	from = get64(&packet[1]);
	to = get64(&packet[3]);
	bytes = packet[5];
	if ((packet[6] & GHCI_COPY_FILL_PATTERN) != 0U || bytes == 0U)
		return;

	/* One mapping holds each range. */
	source = model_lookup(session, from);
	target = model_lookup(session, to);
	if (source == NULL || target == NULL ||
	    from - source->va + bytes > source->bytes || to - target->va + bytes > target->bytes)
		return;

	/* The bytes the mappings reach. */
	source_memory = model_dma(source->phys + (from - source->va), bytes);
	target_memory = model_dma(target->phys + (to - target->va), bytes);
	if (source_memory == NULL || target_memory == NULL)
		return;
	memmove(target_memory, source_memory, bytes);
}

/* Reports whether a range of physical addresses lies in the profile's local memory: 1 when it does. */
static int
local_range(
	uint64_t phys,
	uint64_t bytes)
{
	/* Only a profile with MEM has local memory. */
	if ((profiles[model.profile].blocks & (1U << GHCI_EXTCAP_ID_MEM)) == 0U)
		return 0;

	/* The range lies wholly in it. */
	if (phys < MODEL_LOCAL_BASE || bytes > MODEL_LOCAL_BYTES || phys - MODEL_LOCAL_BASE > MODEL_LOCAL_BYTES - bytes)
		return 0;

	/* Succeeded: the range is local memory. */
	return 1;
}

/* Runs CMD_SESSION_OPEN (section 11.6). */
static enum packet_result
run_session_open(
	const uint32_t *packet)
{
	uint32_t session;

	/* Session 0 is always open, and there are 16 sessions. */
	session = packet[1];
	if (session == 0U || session >= MODEL_SESSIONS)
		return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, 0U, session);

	/* Without the walker only session 0 exists (the flat device). */
	if ((window_word(GHCI_REG_DEVICE_CAPS) & GHCI_CAP_HW_WALKER) == 0U)
		return raise_fault(GHCI_FAULT_KIND_KICK_ABSENT, GHCI_UNIT_CP, 0U, session);

	/* Opening an open session is taken as an error. */
	if ((model.session_active & (1U << session)) != 0U)
		return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, 0U, session);

	/* With PROT the session has its own root in CP_HEAP; without room the session faults (MMU_OOM). */
	if ((window_word(GHCI_REG_DEVICE_CAPS) & GHCI_CAP_PROT) != 0U) {
		model.table_root[session] = 1;
		model.table_mappings[session] = 0U;
		memset(table_marks[session], 0, sizeof(table_marks[session]));
		model.heap_used = heap_count() + model.heap_leaked;
		if (model.heap_used > heap_capacity()) {
			model.table_root[session] = 0;
			model.heap_used = heap_count() + model.heap_leaked;
			return raise_fault(GHCI_FAULT_KIND_MMU_OOM, GHCI_UNIT_CP, session, session);
		}
	}

	/* Succeeded: the session is open. */
	model.session_active |= 1U << session;
	model.session_faulted &= ~(1U << session);
	return PACKET_NEXT;
}

/* Runs CMD_SESSION_CLOSE (section 11.6, N48, N49). */
static enum packet_result
run_session_close(
	const uint32_t *packet)
{
	uint64_t before;
	uint32_t session;

	/* Session 0 cannot be closed. */
	session = packet[1];
	if (session == 0U || session >= MODEL_SESSIONS)
		return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, 0U, session);

	/* Closing a closed session is a no-op (N49). */
	if ((model.session_active & (1U << session)) == 0U && (model.session_faulted & (1U << session)) == 0U)
		return PACKET_NEXT;

	/* With PROT the session's tables go back to CP_HEAP, unless the switch keeps them. */
	if ((window_word(GHCI_REG_DEVICE_CAPS) & GHCI_CAP_PROT) != 0U) {
		before = heap_count();
		model.table_root[session] = 0;
		model.table_mappings[session] = 0U;
		memset(table_marks[session], 0, sizeof(table_marks[session]));
		if (model.switches.close_keeps_heap)
			model.heap_leaked += before - heap_count();
	} else if (model.switches.prot0_close_clears) {
		/* Without PROT the shared PTEs stay (N49), unless the switch clears them. */
		table_remove(0U, 0U, UINT64_MAX, session, 1);
	}

	/* Succeeded: the session is closed. */
	model.heap_used = heap_count() + model.heap_leaked;
	model.session_active &= ~(1U << session);
	model.session_faulted &= ~(1U << session);
	return PACKET_NEXT;
}

/* Runs CMD_VM_MAP (section 7.3, section 11.2). */
static enum packet_result
run_vm_map(
	const uint32_t *packet)
{
	struct model_mapping *mapping;
	uint64_t va_limit;
	uint64_t va;
	uint64_t phys;
	uint64_t bytes;
	uint32_t session;
	uint32_t table;
	uint32_t va_bits;

	/* Reads the session and the range. */
	session = packet[1];
	va = get64(&packet[4]);
	phys = get64(&packet[6]);
	bytes = get64(&packet[8]);

	/* A session out of range, or one not open, is kind 1. */
	if (session >= MODEL_SESSIONS || (model.session_active & (1U << session)) == 0U)
		return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, session, va);

	/* A range not on pages, empty, or with the reserved word set is kind 1. */
	if (bytes == 0U || ((va | phys | bytes) & (GHCI_PAGE_BYTES - 1U)) != 0U || packet[3] != 0U)
		return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, session, va);

	/* A range past the GPU's VA is kind 1. */
	va_bits = GHCI_FEATURE_VA_BITS_BASE + (window_word(GHCI_REG_EXT_CAP_FEATURE) & GHCI_FEATURE_VA_BITS_MASK);
	va_limit = 1ULL << va_bits;
	if (va >= va_limit || bytes > va_limit - va)
		return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, session, va);

	/*
	 * VM_FLAG_LOCAL names local memory and only local memory (section
	 * 11.3, HOST_LIMIT's routing): a local address without the flag, or
	 * the flag on an address outside local memory, is a violation of the
	 * mapping's flags (kind 10).
	 */
	if ((packet[2] & GHCI_VM_FLAG_LOCAL) != 0U && !local_range(phys, bytes))
		return raise_fault(GHCI_FAULT_KIND_PERM_FAULT, GHCI_UNIT_CP, session, va);
	if ((packet[2] & GHCI_VM_FLAG_LOCAL) == 0U && local_range(phys, bytes))
		return raise_fault(GHCI_FAULT_KIND_PERM_FAULT, GHCI_UNIT_CP, session, va);

	/* A range that overlaps a mapping of the same table is a collision (kind 10). */
	table = table_of(session);
	if (table_overlaps(table, va, bytes))
		return raise_fault(GHCI_FAULT_KIND_PERM_FAULT, GHCI_UNIT_CP, session, va);

	/* A table that is full is the model's limit, not the device's: taken as MMU_OOM. */
	if (model.table_mappings[table] >= MODEL_MAPPINGS)
		return raise_fault(GHCI_FAULT_KIND_MMU_OOM, GHCI_UNIT_CP, session, va);

	/* Adds the mapping. */
	mapping = &model.tables[table][model.table_mappings[table]];
	mapping->va = va;
	mapping->bytes = bytes;
	mapping->phys = phys;
	mapping->flags = packet[2];
	mapping->session = session;
	model.table_mappings[table]++;

	/* Marks the L0 tables the map builds, keeping the row for a roll-back. */
	memcpy(marks_saved, table_marks[table], sizeof(marks_saved));
	marks_add(table, va, bytes);

	/* Counts the tables; without room the map is rolled back and faults (N09). */
	model.heap_used = heap_count() + model.heap_leaked;
	if (model.heap_used > heap_capacity()) {
		model.table_mappings[table]--;
		memcpy(table_marks[table], marks_saved, sizeof(marks_saved));
		model.heap_used = heap_count() + model.heap_leaked;
		return raise_fault(GHCI_FAULT_KIND_MMU_OOM, GHCI_UNIT_CP, session, va);
	}

	/* Succeeded: the range is mapped. */
	return PACKET_NEXT;
}

/* Runs CMD_VM_UNMAP (section 7.3, N27, N48, N49). */
static enum packet_result
run_vm_unmap(
	const uint32_t *packet)
{
	uint64_t va;
	uint64_t bytes;
	uint32_t session;

	/* Reads the session and the range. */
	session = packet[1];
	va = get64(&packet[4]);
	bytes = get64(&packet[6]);

	/* A session out of range is kind 1. */
	if (session >= MODEL_SESSIONS)
		return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, session, va);

	/* A closed session is a no-op (N49), unless the switch makes it kind 1. */
	if ((model.session_active & (1U << session)) == 0U) {
		if (model.switches.unmap_closed_faults)
			return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, session, va);
		return PACKET_NEXT;
	}

	/* A range not on pages is kind 1. */
	if (bytes == 0U || ((va | bytes) & (GHCI_PAGE_BYTES - 1U)) != 0U)
		return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, session, va);

	/* Removes what the range covers; the L0 tables stay until SESSION_CLOSE or a reset. */
	table_remove(table_of(session), va, bytes, session, 0);

	/* Succeeded: the range is unmapped. */
	return PACKET_NEXT;
}

/* Runs CMD_FENCE (section 7.3, section 9.1). */
static enum packet_result
run_fence(
	const uint32_t *packet)
{
	uint8_t *word;
	uint64_t address;
	uint64_t sequence;
	uint32_t flags;

	/* Reads the address, sequence and flags. */
	address = get64(&packet[1]);
	sequence = get64(&packet[3]);
	flags = packet[5];

	/* Writes the 64-bit sequence; an address nothing answers is a bus error. */
	if ((flags & GHCI_FENCE_FLAG_MEM_WRITE) != 0U) {
		word = model_dma(address, 8U);
		if (word == NULL || (address & 7U) != 0U)
			return raise_fault(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, model.bound_session, address);
		memcpy(word, &sequence, 8U);
	}

	/* Raises FENCE when asked. */
	if ((flags & GHCI_FENCE_FLAG_IRQ) != 0U)
		model.irq_status |= GHCI_IRQ_FENCE;

	/* Succeeded: the fence is written. */
	return PACKET_NEXT;
}

/* Runs CMD_WAIT_FENCE: waits at the packet until the 64-bit word reaches the value. */
static enum packet_result
run_wait_fence(
	const uint32_t *packet)
{
	uint8_t *word;
	uint64_t address;
	uint64_t wanted;
	uint64_t value;

	/* Reads the word; an address nothing answers is a bus error. */
	address = get64(&packet[1]);
	wanted = get64(&packet[3]);
	word = model_dma(address, 8U);
	if (word == NULL)
		return raise_fault(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, model.bound_session, address);
	memcpy(&value, word, 8U);

	/* Waits while the word is behind. */
	if (value < wanted) {
		model.waiting_fence = 1;
		return PACKET_WAIT;
	}

	/* Succeeded: the condition passed. */
	record(packet, GHCI_DW_WAIT_FENCE, 0U);
	return PACKET_NEXT;
}

/* Records a command the processor took. */
static void
record(
	const uint32_t *packet,
	uint32_t dwords,
	uint32_t skipped)
{
	struct model_command *command;
	uint32_t index;

	/* Counts a command past the record's room as dropped. */
	if (model.command_count >= MODEL_COMMANDS) {
		model.commands_dropped++;
		return;
	}

	/* Keeps the header's fields, the bound session and the leading dwords. */
	command = &model.commands[model.command_count];
	memset(command, 0, sizeof(*command));
	command->opcode = packet[0] & GHCI_HEADER_OPCODE_MASK;
	command->flags = packet[0] >> GHCI_HEADER_FLAGS_SHIFT;
	command->dwords = dwords;
	command->session = model.bound_session;
	command->skipped = skipped;
	for (index = 0U; index < dwords && index < MODEL_COMMAND_WORDS; index++)
		command->words[index] = packet[index];
	model.command_count++;
}

/*
 * Raises a fault: the first one is latched in FAULT_INFO, IRQ_FAULT is set,
 * and either the session or the whole device stops.
 *
 * A client session's fault stops only that session when the device has
 * PROT, and an MMU_OOM of a client session does so on every device
 * (section 11.2, N09); every other fault stops the device (FAULTED).
 */
static enum packet_result
raise_fault(
	uint32_t kind,
	uint32_t unit,
	uint32_t session,
	uint64_t address)
{
	int session_only;

	/* Latches the first fault until the driver clears it (N05). */
	model.faults++;
	if (model.fault_info == 0U) {
		model.fault_info = kind | (unit << GHCI_FAULT_UNIT_SHIFT) | ((session & 0xFFU) << GHCI_FAULT_SESSION_SHIFT);
		model.fault_address = address;
	}
	model.irq_status |= GHCI_IRQ_FAULT;

	/* Decides whether the fault stays in its session. */
	session_only = 0;
	if (session != 0U && session < MODEL_SESSIONS) {
		if ((window_word(GHCI_REG_DEVICE_CAPS) & GHCI_CAP_PROT) != 0U)
			session_only = 1;
		else if (kind == GHCI_FAULT_KIND_MMU_OOM)
			session_only = 1;
	}

	/* A session fault leaves SESSION_ACTIVE without the session; the processor goes on. */
	if (session_only) {
		model.session_active &= ~(1U << session);
		model.session_faulted |= 1U << session;
		return PACKET_NEXT;
	}

	/* A device fault stops the processor at the packet. */
	model.state = MODEL_FAULTED;
	return PACKET_STOP;
}

/* Calls the interrupt handler while the line is high, once per event. */
static void
irq_check(void)
{
	int high;

	/* Nothing to do while the line is low, without a handler, or inside the handler. */
	high = model_irq_line();
	if (!high || model.irq_handler == NULL || model.in_handler)
		return;

	/* Calls the handler once. */
	model.in_handler = 1;
	model.irq_calls++;
	model.irq_handler(model.irq_argument);
	model.in_handler = 0;

	/* A level line still high after the handler would call it again and again. */
	high = model_irq_line();
	if (high)
		model.irq_left_high++;
}

/* Picks the page table of a session: its own with PROT, the shared one without. */
static uint32_t
table_of(
	uint32_t session)
{
	/* With PROT every session has its own table. */
	if ((window_word(GHCI_REG_DEVICE_CAPS) & GHCI_CAP_PROT) != 0U)
		return session;

	/* Without PROT every session shares table 0. */
	return 0U;
}

/* The bytes of a root directory for the device's VA width (section 11.2). */
static uint32_t
root_bytes(void)
{
	uint32_t va_bits;

	/* 40-bit VA has an 8 KiB root; 32-bit VA a 16 KiB one. */
	va_bits = GHCI_FEATURE_VA_BITS_BASE + (window_word(GHCI_REG_EXT_CAP_FEATURE) & GHCI_FEATURE_VA_BITS_MASK);
	if (va_bits > 32U)
		return GHCI_ROOT_BYTES_VA40;

	/* Succeeded: the 32-bit root. */
	return GHCI_ROOT_BYTES_VA32;
}

/*
 * Counts the CP_HEAP bytes of every table with a root: the root, a 4 KiB
 * L1 table per 1 GiB region (40-bit VA) and a 4 KiB L0 table per 2 MiB
 * chunk a mapping has touched since the root was made (section 11.2, N64).
 */
static uint64_t
heap_count(void)
{
	uint64_t total;
	uint32_t table;
	uint32_t index;
	uint32_t va_bits;
	uint32_t chunks;
	uint32_t regions;

	/* Counts each table that has a root. */
	va_bits = GHCI_FEATURE_VA_BITS_BASE + (window_word(GHCI_REG_EXT_CAP_FEATURE) & GHCI_FEATURE_VA_BITS_MASK);
	total = 0U;
	for (table = 0U; table < MODEL_TABLES; table++) {
		/* A table without a root takes nothing. */
		if (!model.table_root[table])
			continue;

		/* Counts the table's L0 tables and the 1 GiB regions they lie in. */
		memset(region_bits, 0, sizeof(region_bits));
		chunks = 0U;
		for (index = 0U; index < MODEL_CHUNKS_MAX; index++) {
			/* One L0 table per marked chunk, and its region. */
			if ((table_marks[table][index / 8U] & (1U << (index % 8U))) == 0U)
				continue;
			chunks++;
			region_bits[(index >> 9) / 8U] |= (uint8_t)(1U << ((index >> 9) % 8U));
		}
		regions = 0U;
		for (index = 0U; index < MODEL_REGIONS_MAX; index++) {
			/* One L1 table per marked region. */
			if ((region_bits[index / 8U] & (1U << (index % 8U))) != 0U)
				regions++;
		}

		/* The root, the L1 tables of a 40-bit VA, and the L0 tables. */
		total += root_bytes();
		if (va_bits > 32U)
			total += (uint64_t)regions * GHCI_TABLE_BYTES;
		total += (uint64_t)chunks * GHCI_TABLE_BYTES;
	}

	/* Succeeded: the bytes in use. */
	return total;
}

/* Marks the 2 MiB chunks a range touches in a table's L0 bitmap. */
static void
marks_add(
	uint32_t table,
	uint64_t va,
	uint64_t bytes)
{
	uint64_t chunk;
	uint64_t last;

	/* Marks each chunk from the first to the last the range touches. */
	last = (va + bytes - 1U) >> 21;
	for (chunk = va >> 21; chunk <= last && chunk < MODEL_CHUNKS_MAX; chunk++)
		table_marks[table][chunk / 8U] |= (uint8_t)(1U << (chunk % 8U));
}

/* The bytes CP_HEAP_BASE and CP_HEAP_LIMIT give; 0 when the heap is not set. */
static uint64_t
heap_capacity(void)
{
	uint32_t base;
	uint32_t limit;

	/* A heap not set, or with its limit below its base, holds nothing. */
	base = window_word(GHCI_REG_CP_HEAP_BASE);
	limit = window_word(GHCI_REG_CP_HEAP_LIMIT);
	if (base == 0U || limit <= base)
		return 0U;

	/* Succeeded: the heap's bytes. */
	return (uint64_t)limit - base;
}

/* Reports whether a range overlaps a mapping of a table. */
static int
table_overlaps(
	uint32_t table,
	uint64_t va,
	uint64_t bytes)
{
	const struct model_mapping *mapping;
	uint32_t index;

	/* Compares with each mapping of the table. */
	for (index = 0U; index < model.table_mappings[table]; index++) {
		/* Two ranges overlap when each starts before the other ends. */
		mapping = &model.tables[table][index];
		if (va < mapping->va + mapping->bytes && mapping->va < va + bytes)
			return 1;
	}

	/* No overlap. */
	return 0;
}

/*
 * Removes what a range covers from a table: whole mappings go, and a
 * mapping the range cuts keeps its parts outside it.  With by_session,
 * every mapping of the session goes whatever the range.
 */
static void
table_remove(
	uint32_t table,
	uint64_t va,
	uint64_t bytes,
	uint32_t session,
	int by_session)
{
	struct model_mapping *mapping;
	struct model_mapping tail;
	uint64_t end;
	uint64_t mapping_end;
	uint32_t index;

	/* Walks the table; a removed entry is replaced by the last one. */
	end = va + bytes;
	if (bytes > UINT64_MAX - va)
		end = UINT64_MAX;
	index = 0U;
	while (index < model.table_mappings[table]) {
		/* A mapping of another session, or outside the range, stays. */
		mapping = &model.tables[table][index];
		mapping_end = mapping->va + mapping->bytes;
		if (by_session) {
			if (mapping->session != session) {
				index++;
				continue;
			}
		} else if (mapping_end <= va || mapping->va >= end) {
			index++;
			continue;
		}

		/* A mapping inside the range, or the session's, goes. */
		if (by_session || (mapping->va >= va && mapping_end <= end)) {
			model.table_mappings[table]--;
			*mapping = model.tables[table][model.table_mappings[table]];
			continue;
		}

		/* A mapping cut in the middle keeps both ends, the tail as a new entry. */
		if (mapping->va < va && mapping_end > end && model.table_mappings[table] < MODEL_MAPPINGS) {
			tail = *mapping;
			tail.phys += end - tail.va;
			tail.va = end;
			tail.bytes = mapping_end - end;
			model.tables[table][model.table_mappings[table]] = tail;
			model.table_mappings[table]++;
			mapping->bytes = va - mapping->va;
			index++;
			continue;
		}

		/* A mapping cut at one end keeps the other end. */
		if (mapping->va < va) {
			mapping->bytes = va - mapping->va;
		} else {
			mapping->phys += end - mapping->va;
			mapping->bytes = mapping_end - end;
			mapping->va = end;
		}
		index++;
	}
}

/* Reads a 64-bit value from two dwords, low first. */
static uint64_t
get64(
	const uint32_t *words)
{
	/* The low dword, then the high one. */
	return (uint64_t)words[0] | ((uint64_t)words[1] << 32);
}

/*
 * Lets one frame of the display pass, as a vertical sync does while the
 * controller is lit: FRAME_COUNT moves, VSYNC is raised, plane 0 takes its
 * pending image and plane 1 its pending cursor (section 13.2).
 */
void
model_display_vsync(void)
{
	uint32_t ctrl;

	/* Only a profile with the controller, lit, has frames. */
	if ((profiles[model.profile].blocks & (1U << GHCI_EXTCAP_ID_DISP2)) == 0U)
		return;
	ctrl = window_word(MODEL_DISP2_BLOCK + GHCI_DC_CTRL);
	if ((ctrl & GHCI_DC_CTRL_ENABLE) == 0U)
		return;

	/* The frame, and the pending flips taken at its start. */
	window_set(MODEL_DISP2_BLOCK + GHCI_DC_FRAME_COUNT, window_word(MODEL_DISP2_BLOCK + GHCI_DC_FRAME_COUNT) + 1U);
	disp_event(GHCI_DC_IRQ_VSYNC);
	if (model.display.pending[0])
		disp_take(0U, 0);
	if (model.display.pending[1])
		disp_take(1U, 0);
}

/* Sets sticky error bits of STATUS as a scan-out read or a record write that failed does, with DISP_ERROR. */
void
model_display_error(
	uint32_t sticky)
{
	/* The bits, the event, and the line. */
	model.display.sticky |= sticky & MODEL_DC_STICKY;
	disp_event(GHCI_DC_IRQ_ERROR);
	irq_check();
}

/* Changes CONNECTED as the sink's line does, raising HOTPLUG when it changes. */
void
model_display_sense(
	int connected)
{
	uint32_t value;

	/* The new level, and the event when it moved. */
	value = connected ? 1U : 0U;
	if (window_word(MODEL_DISP2_BLOCK + GHCI_DC_CONNECTED) == value)
		return;
	window_set(MODEL_DISP2_BLOCK + GHCI_DC_CONNECTED, value);
	disp_event(GHCI_DC_IRQ_HOTPLUG);
	irq_check();
}

/* Reports whether a register offset lies in the DISP2 block of the profile, and its offset in the block. */
static int
disp_register(
	uint32_t offset,
	uint32_t *reg)
{
	/* A profile without the controller has no block. */
	if ((profiles[model.profile].blocks & (1U << GHCI_EXTCAP_ID_DISP2)) == 0U)
		return 0;
	if (offset < MODEL_DISP2_BLOCK || offset >= MODEL_DISP2_BLOCK + GHCI_DC_WINDOW)
		return 0;

	/* Succeeded: the register's offset in the block. */
	*reg = offset - MODEL_DISP2_BLOCK;
	return 1;
}

/* Reads one register of the controller: STATUS, the events and the MODE registers are computed. */
static uint32_t
disp_read(
	uint32_t reg)
{
	uint32_t value;
	uint32_t ctrl;
	uint32_t index;

	/* Picks the registers the controller computes. */
	switch (reg) {
	case GHCI_DC_STATUS:
		/* Running or idle, the sticky errors, the connection, what is pending; a test may set more. */
		ctrl = window_word(MODEL_DISP2_BLOCK + GHCI_DC_CTRL);
		value = model.display.sticky;
		if ((ctrl & GHCI_DC_CTRL_ENABLE) != 0U)
			value |= GHCI_DC_ST_RUNNING;
		else
			value |= GHCI_DC_ST_IDLE;
		if ((window_word(MODEL_DISP2_BLOCK + GHCI_DC_CONNECTED) & 1U) != 0U)
			value |= GHCI_DC_ST_CONNECTED;
		if (model.display.pending[0])
			value |= GHCI_DC_ST_PENDING0;
		if (model.display.pending[1])
			value |= GHCI_DC_ST_PENDING1;
		if (model.display.due[0] || model.display.due[1])
			value |= GHCI_DC_ST_RECORD;
		value |= window_word(MODEL_DISP2_BLOCK + GHCI_DC_STATUS) & (GHCI_DC_ST_PENDING0 | GHCI_DC_ST_PENDING1 | GHCI_DC_ST_RECORD);
		return value;
	case GHCI_DC_IRQ_STATUS:
		return model.display.irq_status;
	case GHCI_DC_IRQ_ENABLE:
		return model.display.irq_enable;
	case GHCI_DC_MODE_H0:
	case GHCI_DC_MODE_H1:
	case GHCI_DC_MODE_V0:
	case GHCI_DC_MODE_V1:
	case GHCI_DC_MODE_CLOCK:
	case GHCI_DC_MODE_FLAGS:
		/* The mode MODE_INDEX selects; past the table, 0. */
		index = window_word(MODEL_DISP2_BLOCK + GHCI_DC_MODE_INDEX);
		if (index >= MODEL_DC_MODES)
			return 0U;
		return display_modes[index][(reg - GHCI_DC_MODE_H0) / 4U];
	case GHCI_DC_I2C_STATUS:
		/* The I2C master is never busy and no device acknowledges. */
		return 0x2U;
	default:
		break;
	}

	/* Succeeded: every other register reads what it holds. */
	return window_word(MODEL_DISP2_BLOCK + reg);
}

/* Writes one register of the controller from the host, by the kind of the register (section 13.2). */
static void
disp_write(
	uint32_t reg,
	uint32_t value)
{
	uint32_t connected;

	/* Picks the register's kind. */
	switch (reg) {
	case GHCI_DC_CTRL:
	case GHCI_DC_FB_NEXT:
	case GHCI_DC_FB_STRIDE:
	case GHCI_DC_FB_SIZE:
	case GHCI_DC_H_ACTIVE:
	case GHCI_DC_H_FP:
	case GHCI_DC_H_SYNC:
	case GHCI_DC_H_BP:
	case GHCI_DC_V_ACTIVE:
	case GHCI_DC_V_FP:
	case GHCI_DC_V_SYNC:
	case GHCI_DC_V_BP:
	case GHCI_DC_SYNC_POL:
	case GHCI_DC_PCLK_SEL:
		/* S: the host writes it while the processor is idle, halted or faulted; otherwise BAD_WRITE. */
		if (model.state == MODEL_RUNNING) {
			model.display.bad_writes++;
			model.display.sticky |= GHCI_DC_ST_BAD_WRITE;
			disp_event(GHCI_DC_IRQ_ERROR);
			return;
		}
		disp_scan_write(reg, value, 0);
		return;
	case GHCI_DC_STATUS:
		/* W: the sticky bits, write 1 to clear. */
		model.display.sticky &= ~(value & MODEL_DC_STICKY);
		return;
	case GHCI_DC_IRQ_STATUS:
		/* W: the events, write 1 to clear. */
		model.display.irq_status &= ~value;
		return;
	case GHCI_DC_IRQ_ENABLE:
		/* The events that raise IRQ_STATUS[3]. */
		model.display.irq_enable = value & GHCI_DC_IRQ_ALL;
		disp_irq_update();
		return;
	case GHCI_DC_CONNECTED:
		/* L: a write that changes it raises HOTPLUG; a controller that senses the sink keeps its own. */
		if ((MODEL_DC_CAPS & GHCI_DC_CAPS_SENSE) != 0U)
			return;
		connected = window_word(MODEL_DISP2_BLOCK + reg);
		window_set(MODEL_DISP2_BLOCK + reg, value & 1U);
		if ((value & 1U) != connected)
			disp_event(GHCI_DC_IRQ_HOTPLUG);
		return;
	case GHCI_DC_MODE_INDEX:
	case GHCI_DC_CURSOR_POS:
	case GHCI_DC_CURSOR_CTRL:
	case GHCI_DC_I2C_CTRL:
	case GHCI_DC_I2C_DATA:
		/* I, C and L: the host writes them in every state. */
		window_set(MODEL_DISP2_BLOCK + reg, value);
		return;
	default:
		/* R: a write is ignored. */
		return;
	}
}

/*
 * Writes a scan-out register as the host (ring 0) or CMD_DISP_SET (ring 1)
 * does: CTRL's RESET resets the controller, ENABLE falling cancels every
 * pending flip, the timing and the image's rows and size take a write only
 * while ENABLE is 0, and FB_NEXT is the host's (a FLIP's for the ring).
 */
static void
disp_scan_write(
	uint32_t reg,
	uint32_t value,
	int ring)
{
	uint32_t ctrl;
	uint32_t old_format;
	uint32_t new_format;

	/* The cursor's registers are the ring's as well. */
	ctrl = window_word(MODEL_DISP2_BLOCK + GHCI_DC_CTRL);
	if (reg == GHCI_DC_CURSOR_POS || reg == GHCI_DC_CURSOR_CTRL) {
		window_set(MODEL_DISP2_BLOCK + reg, value);
		return;
	}

	/* CTRL: the reset, a format change while lit refused, ENABLE's edges. */
	if (reg == GHCI_DC_CTRL) {
		if ((value & GHCI_DC_CTRL_RESET) != 0U) {
			disp_reset();
			return;
		}
		old_format = (ctrl >> GHCI_DC_CTRL_FORMAT_SHIFT) & GHCI_DC_CTRL_FORMAT_MASK;
		new_format = (value >> GHCI_DC_CTRL_FORMAT_SHIFT) & GHCI_DC_CTRL_FORMAT_MASK;
		if ((ctrl & GHCI_DC_CTRL_ENABLE) != 0U && (value & GHCI_DC_CTRL_ENABLE) != 0U && old_format != new_format) {
			model.display.bad_writes++;
			model.display.sticky |= GHCI_DC_ST_BAD_WRITE;
			disp_event(GHCI_DC_IRQ_ERROR);
			value = (value & ~(GHCI_DC_CTRL_FORMAT_MASK << GHCI_DC_CTRL_FORMAT_SHIFT)) | (old_format << GHCI_DC_CTRL_FORMAT_SHIFT);
		}
		window_set(MODEL_DISP2_BLOCK + reg, value & ~GHCI_DC_CTRL_RESET);
		if ((ctrl & GHCI_DC_CTRL_ENABLE) != 0U && (value & GHCI_DC_CTRL_ENABLE) == 0U) {
			if (model.display.pending[0])
				disp_take(0U, 1);
			if (model.display.pending[1])
				disp_take(1U, 1);
		}
		if ((ctrl & GHCI_DC_CTRL_ENABLE) == 0U && (value & GHCI_DC_CTRL_ENABLE) != 0U)
			model.display.sticky &= ~GHCI_DC_ST_UNDERFLOW;
		return;
	}

	/* FB_NEXT: the host's alone; a pending flip is replaced (cancelled), and taken at once while dark. */
	if (reg == GHCI_DC_FB_NEXT) {
		if (ring) {
			model.display.bad_writes++;
			model.display.sticky |= GHCI_DC_ST_BAD_WRITE;
			disp_event(GHCI_DC_IRQ_ERROR);
			return;
		}
		if (model.display.pending[0])
			disp_take(0U, 1);
		window_set(MODEL_DISP2_BLOCK + reg, value);
		if ((ctrl & GHCI_DC_CTRL_ENABLE) == 0U) {
			window_set(MODEL_DISP2_BLOCK + GHCI_DC_FB_BASE, value);
			return;
		}
		model.display.pending[0] = 1;
		model.display.pending_base[0] = value;
		model.display.pending_value[0] = 0U;
		model.display.pending_record[0] = 0U;
		return;
	}

	/* The timing, the rows and the size: only while dark. */
	if ((ctrl & GHCI_DC_CTRL_ENABLE) != 0U) {
		model.display.bad_writes++;
		model.display.sticky |= GHCI_DC_ST_BAD_WRITE;
		disp_event(GHCI_DC_IRQ_ERROR);
		return;
	}
	window_set(MODEL_DISP2_BLOCK + reg, value);
}

/* Runs CMD_DISP_SET (section 7.3): each pair as the ring writes it, then the wait for the idle. */
static enum packet_result
run_disp_set(
	const uint32_t *packet,
	uint32_t dwords)
{
	uint32_t pairs;
	uint32_t index;
	uint32_t reg;

	/* Each pair: an offset in the block and its value; offsets outside the ring's registers are refused. */
	pairs = (dwords - 1U) / 2U;
	for (index = 0U; index < pairs; index++) {
		reg = packet[1U + 2U * index];
		if (reg >= GHCI_DC_WINDOW || (reg > GHCI_DC_PCLK_SEL && reg != GHCI_DC_CURSOR_POS && reg != GHCI_DC_CURSOR_CTRL) ||
		    reg < GHCI_DC_CTRL || reg == GHCI_DC_STATUS || reg == GHCI_DC_IRQ_STATUS || reg == GHCI_DC_IRQ_ENABLE ||
		    reg == GHCI_DC_FB_BASE || reg == GHCI_DC_FRAME_COUNT) {
			model.display.bad_writes++;
			model.display.sticky |= GHCI_DC_ST_BAD_WRITE;
			disp_event(GHCI_DC_IRQ_ERROR);
			continue;
		}
		disp_scan_write(reg, packet[2U + 2U * index], 1);
	}

	/* A wait for the idle while the controller stays lit would never end; the model counts it. */
	if ((packet[0] >> GHCI_HEADER_FLAGS_SHIFT) & GHCI_DISP_SET_WAIT_IDLE) {
		if ((window_word(MODEL_DISP2_BLOCK + GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) != 0U)
			model.display.idle_waits_lit++;
	}

	/* Succeeded: the registers are written. */
	return PACKET_NEXT;
}

/*
 * Runs CMD_FLIP (section 7.3): it waits while its plane has a pending
 * flip; plane 0 is taken at once while dark or with IMMEDIATE and at the
 * next vsync otherwise; plane 1 is taken at the next frame, or at once as
 * cancelled while dark.
 */
static enum packet_result
run_flip(
	const uint32_t *packet,
	uint32_t dwords)
{
	unsigned plane;
	uint32_t flags;
	uint32_t ctrl;

	/* Only display 0, and 32-bit addresses. */
	if (packet[1] != 0U || packet[3] != 0U || packet[7] != 0U) {
		record(packet, dwords, 0U);
		return raise_fault(GHCI_FAULT_KIND_INVALID_OP, GHCI_UNIT_CP, model.bound_session, 1U);
	}

	/* A plane with a pending flip makes the FLIP wait for it to be taken. */
	flags = packet[4];
	plane = 0U;
	if ((flags & GHCI_FLIP_PLANE_CURSOR) != 0U)
		plane = 1U;
	if (model.display.pending[plane]) {
		model.display.flip_waits++;
		return PACKET_WAIT;
	}
	record(packet, dwords, 0U);

	/* The flip pending, with its value and its record. */
	model.display.pending[plane] = 1;
	model.display.pending_base[plane] = packet[2];
	model.display.pending_value[plane] = packet[5];
	model.display.pending_record[plane] = 0U;
	if ((flags & GHCI_FLIP_RECORD_ENABLE) != 0U)
		model.display.pending_record[plane] = packet[6];

	/* Taken now while dark (plane 1 as cancelled) or for IMMEDIATE; at the next frame otherwise. */
	ctrl = window_word(MODEL_DISP2_BLOCK + GHCI_DC_CTRL);
	if ((ctrl & GHCI_DC_CTRL_ENABLE) == 0U)
		disp_take(plane, plane == 1U);
	else if (plane == 0U && (flags & GHCI_FLIP_IMMEDIATE) != 0U)
		disp_take(0U, 0);

	/* Succeeded: the flip is pending or taken. */
	return PACKET_NEXT;
}

/*
 * Takes a plane's pending flip, shown or cancelled: the image becomes the
 * plane's, TAKENn gets the value (bit 31 when cancelled), and the record
 * is written (or due while the processor cannot write it) before the
 * plane's completion event.
 */
static void
disp_take(
	unsigned plane,
	int cancelled)
{
	uint32_t value;

	/* The image, unless cancelled. */
	model.display.pending[plane] = 0;
	if (!cancelled) {
		if (plane == 0U)
			window_set(MODEL_DISP2_BLOCK + GHCI_DC_FB_BASE, model.display.pending_base[0]);
		else
			window_set(MODEL_DISP2_BLOCK + GHCI_DC_CURSOR_BASE, model.display.pending_base[1]);
		model.display.takes[plane]++;
	} else {
		model.display.cancels[plane]++;
	}

	/* The value in TAKEN0 or TAKEN1. */
	value = model.display.pending_value[plane];
	if (cancelled)
		value |= GHCI_DC_TAKEN_CANCELLED;
	window_set(MODEL_DISP2_BLOCK + GHCI_DC_TAKEN0 + 4U * plane, value);

	/* The record and the event. */
	disp_record(plane, value, model.display.pending_record[plane]);
}

/*
 * Writes a flip's record {value, FRAME_COUNT, timestamp} and raises the
 * plane's event; a record due while the processor is halted or faulted
 * waits until it leaves those states (STATUS[31]).
 */
static void
disp_record(
	unsigned plane,
	uint32_t value,
	uint32_t record_pa)
{
	uint32_t words[4];
	uint8_t *target;

	/* Without a record, the event only. */
	if (record_pa != 0U) {
		/* Due while the processor cannot write. */
		if (model.state == MODEL_HALTED || model.state == MODEL_FAULTED) {
			model.display.due[plane] = 1;
			model.display.due_value[plane] = value;
			model.display.due_record[plane] = record_pa;
			return;
		}

		/* The 16 bytes; a record nobody answers is a record error, with DISP_ERROR. */
		words[0] = value;
		words[1] = window_word(MODEL_DISP2_BLOCK + GHCI_DC_FRAME_COUNT);
		words[2] = (uint32_t)model.ticks;
		words[3] = (uint32_t)(model.ticks >> 32);
		target = model_dma(record_pa, sizeof(words));
		if (target == NULL) {
			model.display.sticky |= GHCI_DC_ST_RECORD_ERROR;
			disp_event(GHCI_DC_IRQ_ERROR);
		} else {
			memcpy(target, words, sizeof(words));
			model.display.records++;
		}
	}

	/* The plane's completion. */
	if (plane == 0U)
		disp_event(GHCI_DC_IRQ_FLIP_DONE);
	else
		disp_event(GHCI_DC_IRQ_CURSOR_DONE);
}

/* Writes the records due once the processor is idle or running again. */
static void
disp_flush_due(void)
{
	unsigned plane;

	/* Only a processor that left HALTED and FAULTED writes them. */
	if (model.state != MODEL_IDLE && model.state != MODEL_RUNNING)
		return;

	/* Each plane's record due. */
	for (plane = 0U; plane < 2U; plane++) {
		if (!model.display.due[plane])
			continue;
		model.display.due[plane] = 0;
		disp_record(plane, model.display.due_value[plane], model.display.due_record[plane]);
	}
}

/* Raises display events in DISP_IRQ_STATUS and their summary in IRQ_STATUS[3]. */
static void
disp_event(
	uint32_t events)
{
	model.display.irq_status |= events;
	disp_irq_update();
}

/* Raises IRQ_STATUS[3] while an enabled display event is pending (N21). */
static void
disp_irq_update(void)
{
	if ((model.display.irq_status & model.display.irq_enable) != 0U)
		model.irq_status |= GHCI_IRQ_DISPLAY;
}

/* The controller's own reset: dark, every pending flip cancelled, FRAME_COUNT 0, a sink connected. */
static void
disp_reset(void)
{
	/* The pending flips are taken as cancelled. */
	model.display.resets++;
	window_set(MODEL_DISP2_BLOCK + GHCI_DC_CTRL, 0U);
	if (model.display.pending[0])
		disp_take(0U, 1);
	if (model.display.pending[1])
		disp_take(1U, 1);

	/* The state a reset leaves. */
	model.display.sticky = 0U;
	window_set(MODEL_DISP2_BLOCK + GHCI_DC_FRAME_COUNT, 0U);
	window_set(MODEL_DISP2_BLOCK + GHCI_DC_CONNECTED, 1U);
}
