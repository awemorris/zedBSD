/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The words and tables of a reset of the GPU: the post routine's
 * parameters, the check of the routine's version, the pixel clock of a
 * running timing, the display registers a reset keeps and gives back in
 * the order it writes them, and the timing generator's switch words.
 * Nothing here touches the hardware, so the host tests run it as it is.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-reset.h"

/*
 * The post routine's parameters: the engine clock in 10 kHz in the low 24
 * bits of the first word with the engine's flags (0, a normal init) above
 * them, the memory clock in 10 kHz in the second word with the memory's
 * flags (0, the memory initialized in full, as after a reset) above them,
 * and sixteen reserved words of zero.  The routine's version must be 2.1
 * or a later 2.x.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atomfirmware.h
 * (struct asic_init_engine_parameters, struct asic_init_mem_parameters,
 * struct asic_init_ps_allocation_v2_1, enum atom_asic_init_engine_flag
 * b3NORMAL_ENGINE_INIT, enum atom_asic_init_mem_flag,
 * struct atom_common_table_header) and
 * drivers/gpu/drm/amd/amdgpu/amdgpu_atomfirmware.c
 * (amdgpu_atomfirmware_asic_init: frev 2, crev 1 or more; the memory flag
 * 0 for a reset of the frame buffer).
 */
#define POST_CLOCK_MASK			0x00ffffffU
#define POST_FORMAT_AT			2U
#define POST_CONTENT_AT			3U
#define POST_HEADER_BYTES		4U
#define POST_FORMAT			2U
#define POST_LEAST_CONTENT		1U

/*
 * The timing generator's control word (its master enable in bit 0, the
 * enable it is in now in bit 16) and its vertical generator's enable
 * (bit 31).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (OTG0_OTG_CONTROL: OTG_MASTER_EN, OTG_CURRENT_MASTER_EN_STATE;
 * VTG0_CONTROL: VTG0_ENABLE).
 */
#define TIMING_MASTER_ENABLE		0x00000001U
#define TIMING_MASTER_STATE		0x00010000U
#define VERTICAL_ENABLE			0x80000000U

/*
 * The named display registers a reset keeps, in the order it writes them
 * back: the hub's timer, arbitration and watermarks; the plane's clock,
 * surface, viewport and request, deadline and fetch words; the pixel
 * processor's control and scaler; the cursor; the blend unit and the
 * blend tree; the output processor's source; the timing generator's
 * words with its clocks (written stopped; the reset starts it after the
 * pixel clock); the encoder's front and back end's sources; and the
 * plane's control last, which lets the plane fetch.  The status, flip,
 * CRC, interrupt and I2C words are not kept: they say what happened, not
 * how the pipe is set up.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hwss/dcn20/dcn20_hwseq.c
 * (dcn20_enable_stream_timing, dcn20_program_pipe: the hub's watermarks,
 * the plane, the pixel processor, the blend tree, then the timing
 * generator started and the stream).
 */
static const struct amdrdna_reset_pipe_entry pipe_entries[] = {
	{ AMDRDNA_DCN_HUB_TIMER, AMDRDNA_RESET_ON_HUB, 0U },
	{ AMDRDNA_DCN_HUB_MARK_SELECT, AMDRDNA_RESET_ON_HUB, 0U },
	{ AMDRDNA_DCN_HUB_SATURATION, AMDRDNA_RESET_ON_HUB, 0U },
	{ AMDRDNA_DCN_HUB_OUTSTANDING, AMDRDNA_RESET_ON_HUB, 0U },
	{ AMDRDNA_DCN_HUB_URGENT_MARK, AMDRDNA_RESET_ON_SET, 0U },
	{ AMDRDNA_DCN_HUB_TRIP_MARK, AMDRDNA_RESET_ON_SET, 0U },
	{ AMDRDNA_DCN_HUB_ENTER_MARK, AMDRDNA_RESET_ON_SET, 0U },
	{ AMDRDNA_DCN_HUB_EXIT_MARK, AMDRDNA_RESET_ON_SET, 0U },
	{ AMDRDNA_DCN_HUB_SWITCH_MARK, AMDRDNA_RESET_ON_SET, 0U },
	{ AMDRDNA_DCN_HUB_URGENT_MARK, AMDRDNA_RESET_ON_SET, 1U },
	{ AMDRDNA_DCN_HUB_TRIP_MARK, AMDRDNA_RESET_ON_SET, 1U },
	{ AMDRDNA_DCN_HUB_ENTER_MARK, AMDRDNA_RESET_ON_SET, 1U },
	{ AMDRDNA_DCN_HUB_EXIT_MARK, AMDRDNA_RESET_ON_SET, 1U },
	{ AMDRDNA_DCN_HUB_SWITCH_MARK, AMDRDNA_RESET_ON_SET, 1U },
	{ AMDRDNA_DCN_HUB_URGENT_MARK, AMDRDNA_RESET_ON_SET, 2U },
	{ AMDRDNA_DCN_HUB_TRIP_MARK, AMDRDNA_RESET_ON_SET, 2U },
	{ AMDRDNA_DCN_HUB_ENTER_MARK, AMDRDNA_RESET_ON_SET, 2U },
	{ AMDRDNA_DCN_HUB_EXIT_MARK, AMDRDNA_RESET_ON_SET, 2U },
	{ AMDRDNA_DCN_HUB_SWITCH_MARK, AMDRDNA_RESET_ON_SET, 2U },
	{ AMDRDNA_DCN_HUB_URGENT_MARK, AMDRDNA_RESET_ON_SET, 3U },
	{ AMDRDNA_DCN_HUB_TRIP_MARK, AMDRDNA_RESET_ON_SET, 3U },
	{ AMDRDNA_DCN_HUB_ENTER_MARK, AMDRDNA_RESET_ON_SET, 3U },
	{ AMDRDNA_DCN_HUB_EXIT_MARK, AMDRDNA_RESET_ON_SET, 3U },
	{ AMDRDNA_DCN_HUB_SWITCH_MARK, AMDRDNA_RESET_ON_SET, 3U },
	{ AMDRDNA_DCN_PLANE_CLOCK, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_FORMAT, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_TILING, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_PITCH, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_SURFACE_CONTROL, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_SPACE_SELECT, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_FLIP_CONTROL, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_ADDRESS_HIGH, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_ADDRESS_LOW, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_VIEWPORT_START, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_VIEWPORT_SIZE, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_SECOND_VIEWPORT_START, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_SECOND_VIEWPORT_SIZE, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_COLOR_ROUTE, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_REQUEST_SIZE, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_REQUEST_SIZE_CHROMA, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_REQUEST_DEBUG, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_EXPANSION, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_LEVEL_MARKS, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_GLOBAL_TTU, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_LUMA_TTU, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_LUMA_TTU_PREFETCH, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_CHROMA_TTU, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_CHROMA_TTU_PREFETCH, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_CURSOR_TTU, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_CURSOR_TTU_PREFETCH, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_SECOND_CURSOR_TTU_PREFETCH, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_BLANK_OFFSET, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_NEXT_START, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_LINE_CYCLES, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_AFTER_SCALER, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_PREFETCH, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_PREFETCH_CHROMA, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_VBLANK_LINES, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_VBLANK_PTE, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_VBLANK_PTE_CHROMA, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_VBLANK_META, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_VBLANK_META_CHROMA, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_FLIP_LINES, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_FLIP_PTE, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_FLIP_META, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_PTE_ROW_LINES, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_PTE_GROUP, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_PTE_ROW_LINES_CHROMA, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_PTE_GROUP_CHROMA, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_META_ROW_LINES, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_META_CHUNK, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_META_ROW_LINES_CHROMA, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_META_CHUNK_CHROMA, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_LINE_DELIVERY_PREFETCH, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_LINE_DELIVERY, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_REFERENCE_RATIO, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_REQUEST_LIMIT, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PLANE_CURSOR_TIMING, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PIXEL_PROCESSOR_CONTROL, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_SCALER_MODE, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_SCALER_H_RATIO, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_SCALER_V_RATIO, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_SCALER_OUTPUT_START, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_SCALER_OUTPUT_SIZE, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_SCALER_AUTOMATIC, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_SCALER_H_BLANK, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_SCALER_V_BLANK, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_SCALER_BLEND_SIZE, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_PIXEL_CURSOR_SCALE, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_CURSOR_SIZE, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_CURSOR_ADDRESS_HIGH, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_CURSOR_ADDRESS_LOW, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_CURSOR_POSITION, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_CURSOR_HOT_SPOT, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_CURSOR_DESTINATION, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_CURSOR_CONTROL, AMDRDNA_RESET_ON_PLANE, 0U },
	{ AMDRDNA_DCN_BLEND_TOP, AMDRDNA_RESET_ON_BLEND, 0U },
	{ AMDRDNA_DCN_BLEND_BOTTOM, AMDRDNA_RESET_ON_BLEND, 0U },
	{ AMDRDNA_DCN_BLEND_OUTPUT, AMDRDNA_RESET_ON_BLEND, 0U },
	{ AMDRDNA_DCN_BLEND_CONTROL, AMDRDNA_RESET_ON_BLEND, 0U },
	{ AMDRDNA_DCN_BLEND_TREE_ROOT, AMDRDNA_RESET_ON_TREE, 0U },
	{ AMDRDNA_DCN_COMBINER_SOURCE, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_COMBINER_INPUT, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_HEAD_LEGACY_MODE, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_H_TOTAL, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_H_BLANK, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_H_SYNC, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_H_SYNC_CONTROL, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_V_TOTAL, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_V_TOTAL_MIN, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_V_TOTAL_MAX, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_V_BLANK, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_V_SYNC, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_V_SYNC_CONTROL, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_INTERLACE, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_START_LINE, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_UPDATE, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_READY, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_CLOCK_CONTROL, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_PIXEL_RATE_CONTROL, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_DP_CLOCK_PHASE, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_DP_CLOCK_MODULO, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_VERTICAL_GENERATOR, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_TIMING_CONTROL, AMDRDNA_RESET_ON_TIMING, 0U },
	{ AMDRDNA_DCN_ENCODER_FRONT_CONTROL, AMDRDNA_RESET_ON_FRONT, 0U },
	{ AMDRDNA_DCN_ENCODER_BACK_CONTROL, AMDRDNA_RESET_ON_BACK, 0U },
	{ AMDRDNA_DCN_PLANE_CONTROL, AMDRDNA_RESET_ON_PLANE, 0U }
};

/*
 * The display registers the named table lacks, written back before the
 * named ones: the hub's view of the frame buffer and of the system
 * aperture (the plane fetches through it), the hub's memory-state
 * arbitration, the clock gates' disable words, the pipe clock's divider
 * control and the plane's divider, the timing generator's pixel rate
 * from the PHY's PLL, and the output processor's pipe control (its clock).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_offset.h
 * (mmDCN_VM_FB_LOCATION_BASE 0x0493 to mmDCN_VM_AGP_BASE 0x0498 and
 * mmDCHUBBUB_ARB_DRAM_STATE_CNTL 0x0508 in window 2,
 * mmDCCG_GATE_DISABLE_CNTL 0x0074, mmDCCG_GATE_DISABLE_CNTL2 0x007c,
 * mmDPPCLK_DTO_CTRL 0x00b6, mmDPPCLK0_DTO_PARAM 0x0099 (one apart),
 * mmOTG0_PHYPLL_PIXEL_RATE_CNTL 0x0083 (four apart) in window 1,
 * mmOPP_PIPE0_OPP_PIPE_CONTROL 0x188c (0x5a apart) in window 2).
 */
static const struct amdrdna_reset_extra_entry extra_entries[] = {
	{ 2U, 0x0493U, 0x0000U, AMDRDNA_RESET_ON_HUB },
	{ 2U, 0x0494U, 0x0000U, AMDRDNA_RESET_ON_HUB },
	{ 2U, 0x0495U, 0x0000U, AMDRDNA_RESET_ON_HUB },
	{ 2U, 0x0496U, 0x0000U, AMDRDNA_RESET_ON_HUB },
	{ 2U, 0x0497U, 0x0000U, AMDRDNA_RESET_ON_HUB },
	{ 2U, 0x0498U, 0x0000U, AMDRDNA_RESET_ON_HUB },
	{ 2U, 0x0508U, 0x0000U, AMDRDNA_RESET_ON_HUB },
	{ 1U, 0x0074U, 0x0000U, AMDRDNA_RESET_ON_HUB },
	{ 1U, 0x007cU, 0x0000U, AMDRDNA_RESET_ON_HUB },
	{ 1U, 0x00b6U, 0x0000U, AMDRDNA_RESET_ON_HUB },
	{ 1U, 0x0099U, 0x0001U, AMDRDNA_RESET_ON_PLANE },
	{ 1U, 0x0083U, 0x0004U, AMDRDNA_RESET_ON_TIMING },
	{ 2U, 0x188cU, 0x005aU, AMDRDNA_RESET_ON_PROCESSOR }
};

_Static_assert(sizeof(pipe_entries) / sizeof(pipe_entries[0]) <= AMDRDNA_RESET_PIPE_WORDS, "the kept display words fit");
_Static_assert(sizeof(extra_entries) / sizeof(extra_entries[0]) <= AMDRDNA_RESET_EXTRA_WORDS, "the other display words fit");

/*
 * Fills the post routine's parameters with the boot engine and memory
 * clocks.
 */
void
amdrdna_reset_post_words(
	uint32_t *words,
	uint32_t engine_10khz,
	uint32_t memory_10khz)
{
	unsigned index;

	/* Zeros, the reserved words among them. */
	for (index = 0U; index < AMDRDNA_RESET_POST_WORDS; index++)
		words[index] = 0U;

	/* The clocks, their flags zero: a normal init of the engine and the whole memory. */
	words[0] = engine_10khz & POST_CLOCK_MASK;
	words[1] = memory_10khz & POST_CLOCK_MASK;
}

/*
 * Checks the post routine's version in its header at routine (an offset
 * into image).
 *
 * Returns 0 for version 2.1 or a later 2.x, ENOENT when the image has no
 * such routine, and ENOTSUP for another version.
 */
int
amdrdna_reset_post_format(
	const uint8_t *image,
	size_t length,
	uint16_t routine)
{
	uint8_t format;
	uint8_t content;

	/* A routine the image lacks, or a header past its end. */
	if (routine == 0U)
		return ENOENT;
	if ((size_t)routine + POST_HEADER_BYTES > length)
		return ENOENT;

	/* The header's format and content revisions. */
	format = image[routine + POST_FORMAT_AT];
	content = image[routine + POST_CONTENT_AT];
	if (format != POST_FORMAT)
		return ENOTSUP;
	if (content < POST_LEAST_CONTENT)
		return ENOTSUP;

	/* Succeeded: the routine takes the parameters this file packs. */
	return 0;
}

/*
 * Works out the pixel clock in kHz of a timing from its totals and its
 * refresh in thousandths of a hertz, rounded to the nearest kHz.
 */
uint32_t
amdrdna_reset_pixel_khz(
	uint32_t h_total,
	uint32_t v_total,
	uint32_t refresh_millihz)
{
	uint64_t pixels;
	uint64_t khz;

	/* The pixels of a frame times the frames a second, in kHz. */
	pixels = (uint64_t)h_total * v_total;
	khz = (pixels * refresh_millihz + 500000U) / 1000000U;

	/* Succeeded: the clock. */
	return (uint32_t)khz;
}

/* Reports how many named display registers a reset keeps. */
unsigned
amdrdna_reset_pipe_count(void)
{
	/* The table's length. */
	return (unsigned)(sizeof(pipe_entries) / sizeof(pipe_entries[0]));
}

/* Reports one named display register a reset keeps, in write order; NULL past the last. */
const struct amdrdna_reset_pipe_entry *
amdrdna_reset_pipe_entry(
	unsigned index)
{
	/* An index past the table names nothing. */
	if (index >= amdrdna_reset_pipe_count())
		return NULL;

	/* Succeeded: the entry. */
	return &pipe_entries[index];
}

/* Reports how many other display registers a reset keeps. */
unsigned
amdrdna_reset_extra_count(void)
{
	/* The table's length. */
	return (unsigned)(sizeof(extra_entries) / sizeof(extra_entries[0]));
}

/* Reports one other display register a reset keeps; NULL past the last. */
const struct amdrdna_reset_extra_entry *
amdrdna_reset_extra_entry(
	unsigned index)
{
	/* An index past the table names nothing. */
	if (index >= amdrdna_reset_extra_count())
		return NULL;

	/* Succeeded: the entry. */
	return &extra_entries[index];
}

/* Gives a timing generator's control word with its master enable off, for the write before the start. */
uint32_t
amdrdna_reset_timing_stopped(
	uint32_t timing_control)
{
	/* The other fields as they were. */
	return timing_control & ~TIMING_MASTER_ENABLE;
}

/* Gives a vertical generator's control word with its enable on or off. */
uint32_t
amdrdna_reset_vertical_word(
	uint32_t vertical,
	bool on)
{
	/* The enable bit as asked, the rest as it was. */
	if (on)
		return vertical | VERTICAL_ENABLE;

	/* Off. */
	return vertical & ~VERTICAL_ENABLE;
}

/* Tells whether a timing generator's control word says it runs (its master enable is in effect). */
bool
amdrdna_reset_timing_running(
	uint32_t timing_control)
{
	/* The state bit, not the enable that was asked for. */
	if ((timing_control & TIMING_MASTER_STATE) != 0U)
		return true;

	/* Succeeded: it does not run. */
	return false;
}
