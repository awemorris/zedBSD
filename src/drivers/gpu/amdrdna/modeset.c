/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's own pipe programming, and stage A12: a mode set on the
 * firmware's TMDS output (DVI or HDMI) without the firmware's values.
 *
 * The firmware lit one pipe: a plane fetching a linear surface of 32 bits
 * a pixel, unscaled through the pixel processor and the blend unit, into
 * a timing generator and the output's encoder.  A mode set keeps that
 * chain and its surface; it rewrites what depends on the mode: the pixel
 * clock (the video BIOS's routine, on the first PHY PLL), the
 * timing generator's timing and global sync, the pixel processor's copy
 * of the blank and its sizes, the plane's viewport, the plane's request,
 * deadline and arbitration fields and the hub's watermarks (the display
 * plan, mode-plan.c, at the clocks the board runs), then the encoder's
 * stream and the transmitter (the video BIOS's routines).
 *
 * Stage A12 does it three times: the firmware's own mode with only the
 * plane's fields, the watermarks and the global sync replaced (the plan's
 * values instead of the firmware's), another mode the monitor lists, and
 * the firmware's mode again by the whole sequence; each time it checks
 * the frames, the underflow bits and the picture's CRC.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-sink.h"

/*
 * The video BIOS's routines (their places in the command list: the
 * encoder 4, the pixel clock 12, the transmitter 76), the encoder's stream
 * setup (15), the transmitter's off (0) and on (1), the modes DVI (2) and
 * HDMI (3), 8 bits a colour (2), four lanes, the first PHY PLL's number
 * (20), and the pixel clock routine's flags
 * (the crystal as the PLL's reference: 0).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atomfirmware.h
 * (struct atom_master_list_of_command_functions_v2_1: digxencodercontrol,
 * setpixelclock, dig1transmittercontrol; ATOM_ENCODER_CMD_STREAM_SETUP
 * 0x0F, ATOM_TRANSMITTER_ACTION_DISABLE 0, ATOM_TRANSMITTER_ACTION_ENABLE
 * 1, ATOM_ENCODER_MODE_DVI 2, ATOM_ENCODER_MODE_HDMI 3,
 * PANEL_8BIT_PER_COLOR 2, ATOM_COMBOPHY_PLL0 20,
 * PIXEL_CLOCK_V7_MISC_REF_DIV_SRC_XTALIN 0x00).
 */
#define MODESET_ENCODER_ROUTINE		0x04U
#define MODESET_CLOCK_ROUTINE		0x0cU
#define MODESET_TRANSMITTER_ROUTINE	0x4cU
#define MODESET_STREAM_SETUP		0x0fU
#define MODESET_TRANSMITTER_OFF		0U
#define MODESET_TRANSMITTER_ON		1U
#define MODESET_MODE_DVI		2U
#define MODESET_MODE_HDMI		3U
#define MODESET_EIGHT_BITS		2U
#define MODESET_LANES			4U
#define MODESET_FIRST_PHY_PLL		20U
#define MODESET_CLOCK_FLAGS		0x00U

/*
 * The timing generator's words: the totals, starts and ends in their low
 * and high 15 bits, the sync polarity (bit 0, set for a negative sync),
 * the control word's master enable (bit 0), its disable point (bits 9:8,
 * 3: at the end of the first line of the blank) and the master enable it
 * is in (bit 16), the clock control's busy bit
 * (16), the vertical timing generator's enable (bit 31), first line (bits
 * 30:16) and second front porch (bits 14:0), the blank word's show black
 * (bit 8) and the blank it is in (bit 0), the update lock (bit 0) and its
 * status (bit 8), the global sync's start line (bits 9:0), update width
 * (bits 25:16) and offset (bits 15:0), the input control's underflow clear
 * (bit 12), the CRC's on and continuous bits (0 and 4), and the 24-bit
 * frame count.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (OTG0_OTG_H_TOTAL, OTG0_OTG_H_BLANK_START_END, OTG0_OTG_H_SYNC_A,
 * OTG0_OTG_H_SYNC_A_CNTL, OTG0_OTG_V_TOTAL, OTG0_OTG_V_BLANK_START_END,
 * OTG0_OTG_V_SYNC_A, OTG0_OTG_V_SYNC_A_CNTL, OTG0_OTG_CONTROL,
 * OTG0_OTG_CLOCK_CONTROL, VTG0_CONTROL, OTG0_OTG_BLANK_CONTROL,
 * OTG0_OTG_MASTER_UPDATE_LOCK, OTG0_OTG_VSTARTUP_PARAM,
 * OTG0_OTG_VUPDATE_PARAM, OTG0_OTG_VREADY_PARAM,
 * ODM0_OPTC_INPUT_GLOBAL_CONTROL, OTG0_OTG_CRC_CNTL,
 * OTG0_OTG_STATUS_FRAME_COUNT).
 */
#define MODESET_LOW_FIELD		0x00007fffU
#define MODESET_HIGH_SHIFT		16U
#define MODESET_SYNC_NEGATIVE		0x00000001U
#define MODESET_MASTER_ENABLE		0x00000001U
#define MODESET_DISABLE_POINT		0x00000300U
#define MODESET_MASTER_STATE		0x00010000U
#define MODESET_TIMING_BUSY		0x00010000U
#define MODESET_VERTICAL_ENABLE		0x80000000U
#define MODESET_BLANK_DATA		0x00000100U
#define MODESET_BLANK_STATE		0x00000001U
#define MODESET_LOCK			0x00000001U
#define MODESET_LOCK_STATUS		0x00000100U
#define MODESET_START_FIELD		0x000003ffU
#define MODESET_UPDATE_WIDTH_FIELD	0x000003ffU
#define MODESET_OFFSET_FIELD		0x0000ffffU
#define MODESET_TIMING_UNDERFLOW_CLEAR	0x00001000U
#define MODESET_CRC_CONTINUOUS_ON	0x00000011U
#define MODESET_FRAME_FIELD		0x00ffffffU

/*
 * The pixel processor's and the plane's size words (width in bits 13:0,
 * height in bits 29:16, and the blank's start and end in the same
 * places), the plane control's underflow clear (bit 31) and its ready at
 * or after the sync (bit 8), the plane's debug word's "dv" mode off (bit
 * 8).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (DSCL0_OTG_H_BLANK, DSCL0_OTG_V_BLANK, DSCL0_RECOUT_SIZE, DSCL0_MPC_SIZE,
 * HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, HUBP0_DCHUBP_CNTL:
 * HUBP_UNDERFLOW_CLEAR, HUBP_VREADY_AT_OR_AFTER_VSYNC) and
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_vready_at_or_After_vsync: 1 << 8).
 */
#define MODESET_SIZE_FIELD		0x00003fffU
#define MODESET_PLANE_UNDERFLOW_CLEAR	0x80000000U
#define MODESET_PLANE_READY_AFTER	0x00000100U
#define MODESET_PLANE_DEBUG		0x00000100U

/*
 * The plane's request size word: swath height (bits 2:0), page table row
 * height (6:4), chunk (10:8), smallest chunk (12:11), meta chunk (17:16),
 * smallest meta chunk (19:18), page table group (22:20), meta page table
 * group (26:24); the expansion modes (data 1:0, cursor 3:2, meta 5:4,
 * page table 7:6); the levels (low 13:0, high 29:16); the global time
 * (the blank's time 23:0, the flip's level 31:28); a request's delivery
 * (22:0), its fixed level (27:24) and the ramp off (bit 28); the deadline
 * words' fields: the blank's end in cycles (12:0) and its line (30:16),
 * the next start (17:0), a line's cycles (20:0), the delay after the
 * scaler in cycles (12:0) and lines (18:16), the prefetch's ratio (21:0)
 * and lines (31:24), the cursor's and the rows' lines in the blank and in
 * a flip (4:0 and 13:8), the 23-bit cycle words, the 17-bit line words,
 * the line deliveries (luma 12:0, chroma 28:16), the reference over the
 * pixel clock (20:0) and the request limit (14:0).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (HUBP0_DCHUBP_REQ_SIZE_CONFIG, HUBPREQ0_DCN_EXPANSION_MODE,
 * HUBPREQ0_DCN_TTU_QOS_WM, HUBPREQ0_DCN_GLOBAL_TTU_CNTL,
 * HUBPREQ0_DCN_SURF0_TTU_CNTL0, HUBPREQ0_BLANK_OFFSET_0,
 * HUBPREQ0_BLANK_OFFSET_1, HUBPREQ0_DST_DIMENSIONS,
 * HUBPREQ0_DST_AFTER_SCALER, HUBPREQ0_PREFETCH_SETTINGS,
 * HUBPREQ0_VBLANK_PARAMETERS_0, HUBPREQ0_VBLANK_PARAMETERS_1,
 * HUBPREQ0_NOM_PARAMETERS_0, HUBPREQ0_PER_LINE_DELIVERY,
 * HUBPREQ0_REF_FREQ_TO_PIX_FREQ, HUBPREQ0_DST_Y_DELTA_DRQ_LIMIT).
 */
#define MODESET_SWATH_SHIFT		0U
#define MODESET_PTE_ROWS_SHIFT		4U
#define MODESET_CHUNK_SHIFT		8U
#define MODESET_LEAST_CHUNK_SHIFT	11U
#define MODESET_META_CHUNK_SHIFT	16U
#define MODESET_LEAST_META_SHIFT	18U
#define MODESET_PTE_GROUP_SHIFT		20U
#define MODESET_META_GROUP_SHIFT	24U
#define MODESET_DATA_EXPANSION_SHIFT	0U
#define MODESET_CURSOR_EXPANSION_SHIFT	2U
#define MODESET_META_EXPANSION_SHIFT	4U
#define MODESET_PTE_EXPANSION_SHIFT	6U
#define MODESET_LEVEL_FIELD		0x00003fffU
#define MODESET_TTU_TIME_FIELD		0x00ffffffU
#define MODESET_FLIP_LEVEL_SHIFT	28U
#define MODESET_DELIVERY_FIELD		0x007fffffU
#define MODESET_FIXED_LEVEL_SHIFT	24U
#define MODESET_CYCLES13_FIELD		0x00001fffU
#define MODESET_LINE15_FIELD		0x00007fffU
#define MODESET_NEXT_START_FIELD	0x0003ffffU
#define MODESET_CYCLES21_FIELD		0x001fffffU
#define MODESET_AFTER_LINES_FIELD	0x00000007U
#define MODESET_RATIO_FIELD		0x003fffffU
#define MODESET_PREFETCH_SHIFT		24U
#define MODESET_PREFETCH_FIELD		0x000000ffU
#define MODESET_VM_LINES_FIELD		0x0000001fU
#define MODESET_ROW_LINES_SHIFT		8U
#define MODESET_ROW_LINES_FIELD		0x0000003fU
#define MODESET_CYCLES23_FIELD		0x007fffffU
#define MODESET_LINES17_FIELD		0x0001ffffU

/*
 * The hub's watermark words take 21 bits of reference clock cycles; its
 * saturation level is 60 microseconds of them and its least outstanding
 * requests 180 (bits 24:16 of its word); the hub's timer divides the
 * crystal by its divider field (bits 3:0) when it is 2.  Without a
 * crystal from the video BIOS it is 100 MHz.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubbub/dcn10/dcn10_hubbub.c
 * (convert_and_clamp, 0x1fffff), display/dc/hubbub/dcn20/dcn20_hubbub.c
 * (hubbub2_program_watermarks: 60 * refclk_mhz, 180;
 * hubbub2_get_dchub_ref_freq), drivers/gpu/drm/amd/include/asic_reg/dcn/
 * dcn_2_0_0_sh_mask.h (DCHUBBUB_ARB_DF_REQ_OUTSTAND,
 * DCHUBBUB_GLOBAL_TIMER_CNTL) and display/dc/bios/bios_parser2.c (the
 * crystal from the display controller table).
 */
#define MODESET_MARK_FIELD		0x001fffffU
#define MODESET_SATURATION_US		60U
#define MODESET_LEAST_OUTSTANDING	180U
#define MODESET_OUTSTANDING_SHIFT	16U
#define MODESET_OUTSTANDING_FIELD	0x000001ffU
#define MODESET_TIMER_DIVIDER_FIELD	0x0000000fU
#define MODESET_CRYSTAL_KHZ		100000U
#define MODESET_MARK_SETS		4U

/*
 * The clock generator's dividers: the display clock's divider id (bits
 * 6:0) and the pipe clock's (bits 30:24), an id's divider in quarters (ids
 * 8 to 63: 8 + 1 per id, 64 to 95: 64 + 2 per id, 96 to 125: 128 + 4 per
 * id, from 126: 248 + 264 per id).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (DENTIST_DISPCLK_CNTL: DENTIST_DISPCLK_WDIVIDER, DENTIST_DPPCLK_WDIVIDER)
 * and drivers/gpu/drm/amd/display/dc/inc/hw/clk_mgr_internal.h
 * (DENTIST_BASE_DID_1 to _4, DENTIST_DIVIDER_RANGE_1_START to _4_STEP).
 */
#define MODESET_DISPLAY_DIVIDER_FIELD	0x0000007fU
#define MODESET_PIPE_DIVIDER_SHIFT	24U
#define MODESET_PIPE_DIVIDER_FIELD	0x0000007fU

/*
 * The display clock generator's VCO register (the clock block's fourth
 * window at 0x17200, word 0x0e) with its multiplier's integer (bits 8:0)
 * and fraction (bits 31:16) of the 100 MHz reference, and the VCO to
 * assume when it reads 0 (3.85 GHz).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/navi10_ip_offset.h (CLK_BASE__INST0_SEG3
 * 0x00017200), include/asic_reg/clk/clk_11_0_0_offset.h
 * (mmCLK3_0_CLK3_CLK_PLL_REQ 0x000e), clk_11_0_0_sh_mask.h
 * (CLK3_0_CLK3_CLK_PLL_REQ: FbMult_int, FbMult_frac) and
 * drivers/gpu/drm/amd/display/dc/clk_mgr/dcn20/dcn20_clk_mgr.c
 * (dcn20_clk_mgr_construct: 100000, 3850000).
 */
#define MODESET_VCO_REGISTER		(0x17200U + 0x000eU)
#define MODESET_VCO_WHOLE_FIELD		0x000001ffU
#define MODESET_VCO_FRACTION_SHIFT	16U
#define MODESET_VCO_REFERENCE_KHZ	100000U
#define MODESET_VCO_DEFAULT_KHZ		3850000U

/*
 * The video BIOS's clock routine (its place in the command list: 41), its
 * frequency command (0), and the clocks read through it: the data return
 * clock (PLL 0, clock 5) and the memory clock (PLL 1, clock 0).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atomfirmware.h
 * (struct atom_master_list_of_command_functions_v2_1: getsmuclockinfo;
 * GET_SMU_CLOCK_INFO_V3_1_GET_CLOCK_FREQ 0; SMU11_SYSPLL0_ID 0,
 * SMU11_SYSPLL0_DCEFCLK_ID 5, SMU11_SYSPLL1_0_ID 1,
 * SMU11_SYSPLL1_0_UCLKA_ID 0).
 */
#define MODESET_CLOCK_INFO_ROUTINE	41U
#define MODESET_CLOCK_FREQUENCY		0U
#define MODESET_RETURN_CLOCK_PLL	0U
#define MODESET_RETURN_CLOCK_ID		5U
#define MODESET_MEMORY_CLOCK_PLL	1U
#define MODESET_MEMORY_CLOCK_ID		0U

/*
 * The fabric clock from the memory clock: 1.08 times it, at least 506 MHz;
 * the memory's transfers a clock: 16.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/dcn20_fpu.c
 * (dcn20_update_bounding_box: min_dcfclk 506, the factor 1080 / 1000000
 * on the memory clock in kHz, dram_speed_mts = uclk * 16 / 1000).
 */
#define MODESET_FABRIC_PER_MILLE	1080U
#define MODESET_FABRIC_LEAST_MHZ	506U
#define MODESET_TRANSFERS		16U

/* How long the steps wait (in microseconds), the time the other mode is held, and the frames a look counts and needs. */
#define MODESET_LOOK_FRAMES		60U
#define MODESET_GOOD_FRAMES		30U
#define MODESET_STEP_US			500U
#define MODESET_WAIT_US			200000U
#define MODESET_DARK_US			300000U
#define MODESET_HOLD_US			3000000U

/* Two refreshes within half a hertz are the same one; the bytes of a pixel of the surface. */
#define MODESET_SAME_REFRESH_MILLIHZ	500U
#define MODESET_PIXEL_BYTES		4U

/* What a mode set works with: the output's mode word and the timing generator, plane and pixel processor. */
struct modeset_pipe {
	uint32_t mode;
	unsigned timing;
	unsigned plane;
	unsigned pixel;
	const struct amdrdna_sink *sink;
};

/* What a check of the picture found: the frames, the underflow bits and the CRC. */
struct modeset_check {
	uint32_t frames;
	uint32_t underflow;
	uint32_t timing_underflow;
	uint32_t red_green;
	uint32_t blue;
};

/*
 * The display words the log gets after a failed step of the mode set,
 * eight to a line: the clock controls, the hub arbiter and the vertical
 * timing generators, the plane, the pixel processor, the timing
 * generator's input and the timing generator (absolute dword indices of
 * the first pipe on Navi10).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_offset.h
 * (mmDENTIST_DISPCLK_CNTL to mmOTG0_PHYPLL_PIXEL_RATE_CNTL in window 1,
 * mmDCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A to mmVTG0_CONTROL, mmHUBP0_* and
 * mmHUBPREQ0_*, mmDSCL0_*, mmODM0_* and mmOTG0_* in window 2, with
 * navi10_ip_offset.h's display bases 0xc0 and 0x34c0).
 */
static const struct modeset_range {
	uint32_t first;
	uint32_t count;
} modeset_ranges[] = {
	{ 0x0124U, 0x0020U },
	{ 0x39c8U, 0x0028U },
	{ 0x3aa5U, 0x0090U },
	{ 0x41abU, 0x0020U },
	{ 0x4f8aU, 0x0008U },
	{ 0x4feaU, 0x0078U }
};

static int modeset_pipe(struct amdrdna_device *device, struct modeset_pipe *pipe);
static int modeset_input(struct amdrdna_device *device, const struct amdrdna_timing *timing, struct amdrdna_mode_plan_input *input);
static void modeset_listed(const struct amdrdna_sink *sink, struct amdrdna_timing *timing);
static bool modeset_good(const struct modeset_check *check);
static int modeset_bios_clock(struct amdrdna_device *device, uint32_t pll, uint32_t clock, uint32_t *khz);
static uint32_t modeset_divider(uint32_t id);
static void modeset_timing_now(struct amdrdna_device *device, const struct modeset_pipe *pipe, struct amdrdna_timing *timing);
static const struct amdrdna_timing *modeset_other(struct amdrdna_device *device, const struct modeset_pipe *pipe, const struct amdrdna_timing *now);
static int modeset_switch(struct amdrdna_device *device, const struct modeset_pipe *pipe, const struct amdrdna_timing *timing, const struct amdrdna_mode_plan *plan, uint32_t reference_khz);
static void modeset_timing(struct amdrdna_device *device, const struct modeset_pipe *pipe, const struct amdrdna_timing *timing);
static void modeset_sizes(struct amdrdna_device *device, const struct modeset_pipe *pipe, const struct amdrdna_timing *timing);
static void modeset_sync(struct amdrdna_device *device, const struct modeset_pipe *pipe, const struct amdrdna_timing *timing, const struct amdrdna_mode_plan *plan);
static void modeset_plane(struct amdrdna_device *device, const struct modeset_pipe *pipe, const struct amdrdna_mode_plan *plan);
static void modeset_marks(struct amdrdna_device *device, const struct amdrdna_mode_plan *plan, uint32_t reference_khz);
static int modeset_routine(struct amdrdna_device *device, unsigned routine, uint32_t *words, const char *what);
static int modeset_output(struct amdrdna_device *device, const struct modeset_pipe *pipe, uint32_t action, uint32_t pixel_10khz);
static void modeset_lock(struct amdrdna_device *device, const struct modeset_pipe *pipe, bool locked);
static void modeset_blank(struct amdrdna_device *device, const struct modeset_pipe *pipe, bool blank);
static bool modeset_frames(struct amdrdna_device *device, const struct modeset_pipe *pipe, unsigned frames);
static void modeset_look(struct amdrdna_device *device, const struct modeset_pipe *pipe, const struct amdrdna_timing *timing, unsigned frames, struct modeset_check *check);
static void modeset_note(struct amdrdna_device *device, const struct modeset_pipe *pipe, const char *step);
static void modeset_dump(struct amdrdna_device *device, const char *step);
static uint32_t modeset_read(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance);
static void modeset_write(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance, uint32_t value);

/*
 * Stage A12: replaces the firmware's plane fields, watermarks and global
 * sync with the plan's in the same mode, sets another mode the monitor
 * lists for three seconds, then sets the firmware's mode again by the
 * whole sequence.
 *
 * Returns 0; ENOENT before stage A10 read the output's path; ENOTSUP for
 * an output that is not TMDS; ERANGE when the plan refuses the firmware's
 * mode or no other mode fits; EIO when a clock reads 0, a step underflows
 * or the picture stops; or the error of a video BIOS routine.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hwss/dcn20/dcn20_hwseq.c
 * (dcn20_enable_stream_timing, dcn20_program_pipe, dcn20_update_dchubp_dpp)
 * and display/dc/link/link_dpms.c (the stream's off and on).
 */
int
amdrdna_a12_mode(
	struct amdrdna_device *device)
{
	struct modeset_pipe pipe;
	struct amdrdna_timing firmware;
	struct amdrdna_mode_plan_input input;
	struct amdrdna_mode_plan plan;
	struct modeset_check check;
	const struct amdrdna_timing *other;
	bool good;
	int error;
	int step;

	/* The path and the clocks. */
	amdrdna_mark("A12 begin");
	error = modeset_pipe(device, &pipe);
	if (error != 0)
		return error;
	modeset_timing_now(device, &pipe, &firmware);
	error = modeset_input(device, &firmware, &input);
	if (error != 0)
		return error;
	amdrdna_mark("A12 clocks reference=%ukHz return=%ukHz fabric=%ukHz display=%ukHz pipe=%ukHz memory=%uMT/s",
	    (unsigned)input.reference_khz,
	    (unsigned)input.return_khz,
	    (unsigned)input.fabric_khz,
	    (unsigned)input.display_khz,
	    (unsigned)input.pipe_khz,
	    (unsigned)input.memory_mts);

	/* The plan of the firmware's mode. */
	error = amdrdna_mode_plan(&input, &plan);
	if (error != 0) {
		amdrdna_mark("A12 fail: the plan refuses the firmware's %ux%u (%d)", firmware.width, firmware.height, error);
		return error;
	}

	/* One: the same mode, the plane's fields, the watermarks and the global sync from the plan, under the update lock. */
	modeset_lock(device, &pipe, true);
	modeset_plane(device, &pipe, &plan);
	modeset_marks(device, &plan, input.reference_khz);
	modeset_sync(device, &pipe, &firmware, &plan);
	modeset_lock(device, &pipe, false);
	modeset_look(device, &pipe, &firmware, MODESET_LOOK_FRAMES, &check);
	amdrdna_mark("A12 same %ux%u start=%u prefetch=%u/4 ttu=%u marks=%u/%u/%u/%u frames=%u underflow=%u/%u",
	    firmware.width,
	    firmware.height,
	    (unsigned)plan.start_lines,
	    (unsigned)plan.prefetch_quarters,
	    (unsigned)plan.vblank_ttu_cycles,
	    (unsigned)plan.urgent_ns,
	    (unsigned)plan.refresh_enter_exit_ns,
	    (unsigned)plan.refresh_exit_ns,
	    (unsigned)plan.switch_ns,
	    (unsigned)check.frames,
	    (unsigned)check.underflow,
	    (unsigned)check.timing_underflow);
	good = modeset_good(&check);
	if (!good)
		return EIO;

	/* Two: another mode the monitor lists and the plan fits. */
	other = modeset_other(device, &pipe, &firmware);
	if (other == NULL) {
		amdrdna_mark("A12 fail: no other mode of the monitor fits");
		return ERANGE;
	}
	input.h_total = (uint32_t)other->width + other->h_blanking;
	input.width = other->width;
	input.h_front = other->h_porch;
	input.v_total = (uint32_t)other->height + other->v_blanking;
	input.height = other->height;
	input.v_front = other->v_porch;
	input.pixel_khz = other->pixel_khz;
	error = amdrdna_mode_plan(&input, &plan);
	if (error != 0)
		return error;
	error = modeset_switch(device, &pipe, other, &plan, input.reference_khz);
	modeset_look(device, &pipe, other, MODESET_LOOK_FRAMES, &check);
	amdrdna_mark("A12 mode %ux%u@%u.%03u pclk=%ukHz start=%u frames=%u underflow=%u/%u crc=%08x/%08x error=%d",
	    other->width,
	    other->height,
	    (unsigned)(other->refresh_millihz / 1000U),
	    (unsigned)(other->refresh_millihz % 1000U),
	    (unsigned)other->pixel_khz,
	    (unsigned)plan.start_lines,
	    (unsigned)check.frames,
	    (unsigned)check.underflow,
	    (unsigned)check.timing_underflow,
	    (unsigned)check.red_green,
	    (unsigned)check.blue,
	    error);

	/* The words for a look at a failure. */
	good = modeset_good(&check);
	if (!good)
		modeset_dump(device, "other");

	/* The other mode held for the monitor. */
	kern_usleep_range(MODESET_HOLD_US, MODESET_HOLD_US + MODESET_STEP_US);

	/* Three: the firmware's mode again, by the whole sequence (also after a failure, to give the screen back). */
	input.h_total = (uint32_t)firmware.width + firmware.h_blanking;
	input.width = firmware.width;
	input.h_front = firmware.h_porch;
	input.v_total = (uint32_t)firmware.height + firmware.v_blanking;
	input.height = firmware.height;
	input.v_front = firmware.v_porch;
	input.pixel_khz = firmware.pixel_khz;
	good = modeset_good(&check);
	if (error == 0 && !good)
		error = EIO;
	step = amdrdna_mode_plan(&input, &plan);
	if (step != 0)
		return step;
	if (error != 0) {
		(void)modeset_switch(device, &pipe, &firmware, &plan, input.reference_khz);
		return error;
	}
	error = modeset_switch(device, &pipe, &firmware, &plan, input.reference_khz);
	modeset_look(device, &pipe, &firmware, MODESET_LOOK_FRAMES, &check);

	/* Reports a return that failed. */
	if (error != 0)
		return error;
	good = modeset_good(&check);
	if (!good) {
		amdrdna_mark("A12 fail: back at %ux%u frames=%u underflow=%u/%u", firmware.width, firmware.height,
		    (unsigned)check.frames, (unsigned)check.underflow, (unsigned)check.timing_underflow);
		return EIO;
	}

	/* Says how it went. */
	amdrdna_mark("A12 ok %ux%u and back to %ux%u@%u.%03u frames=%u underflow=0 crc=%08x/%08x",
	    other->width,
	    other->height,
	    firmware.width,
	    firmware.height,
	    (unsigned)(firmware.refresh_millihz / 1000U),
	    (unsigned)(firmware.refresh_millihz % 1000U),
	    (unsigned)check.frames,
	    (unsigned)check.red_green,
	    (unsigned)check.blue);

	/* Succeeded: both modes were shown and the firmware's is back. */
	return 0;
}

/*
 * Sets the timing generator's first CRC to cover the active area: both
 * windows from the top left to the width and height (the CRC takes their
 * union).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/core/dc.c (dc_stream_configure_crc) and
 * display/dc/optc/dcn10/dcn10_optc.c (optc1_configure_crc).
 */
void
amdrdna_timing_crc_window(
	struct amdrdna_device *device,
	unsigned timing,
	uint32_t width,
	uint32_t height)
{
	uint32_t across;
	uint32_t down;

	/* From 0 to the width and to the height. */
	across = (width & MODESET_LOW_FIELD) << MODESET_HIGH_SHIFT;
	down = (height & MODESET_LOW_FIELD) << MODESET_HIGH_SHIFT;
	modeset_write(device, AMDRDNA_DCN_TIMING_CRC_WINDOW_A_X, timing, across);
	modeset_write(device, AMDRDNA_DCN_TIMING_CRC_WINDOW_A_Y, timing, down);
	modeset_write(device, AMDRDNA_DCN_TIMING_CRC_WINDOW_B_X, timing, across);
	modeset_write(device, AMDRDNA_DCN_TIMING_CRC_WINDOW_B_Y, timing, down);
}

/*
 * Gives the clocks the board runs to a display plan's input: the hub's
 * reference (the crystal over the hub timer's divider), the display and
 * pipe clocks (the display clock generator's VCO over each one's
 * divider), the data return clock and the memory clock (the video BIOS's
 * clock routine, which reads them from the power controller's PLLs), the
 * fabric and the memory's transfers from the memory clock.  The timing
 * and the pitch are left as they are.  Returns 0, or the clock routine's
 * error.
 */
int
amdrdna_mode_clocks(
	struct amdrdna_device *device,
	struct amdrdna_mode_plan_input *input)
{
	uint32_t word;
	uint32_t vco_khz;
	uint32_t display_divider;
	uint32_t pipe_divider;
	uint32_t memory_khz;
	uint32_t fabric_mhz;
	uint32_t crystal_khz;
	int error;

	/* The hub's reference: the crystal, halved when the hub timer divides it by 2. */
	crystal_khz = device->vbios_info.display_reference_10khz * 10U;
	if (crystal_khz == 0U)
		crystal_khz = MODESET_CRYSTAL_KHZ;
	word = modeset_read(device, AMDRDNA_DCN_HUB_TIMER, 0U);
	input->reference_khz = crystal_khz;
	if ((word & MODESET_TIMER_DIVIDER_FIELD) == 2U)
		input->reference_khz = crystal_khz / 2U;

	/* The display clock generator's VCO: its multiplier (9 integer bits, 16 fraction bits) times 100 MHz. */
	word = amdrdna_read(device, MODESET_VCO_REGISTER);
	vco_khz = (word & MODESET_VCO_WHOLE_FIELD) * MODESET_VCO_REFERENCE_KHZ;
	vco_khz += (uint32_t)(((uint64_t)(word >> MODESET_VCO_FRACTION_SHIFT) * MODESET_VCO_REFERENCE_KHZ) >> 16);
	if (vco_khz == 0U)
		vco_khz = MODESET_VCO_DEFAULT_KHZ;

	/* The display and pipe clocks: the VCO over each divider (in quarters). */
	word = modeset_read(device, AMDRDNA_DCN_DISPLAY_CLOCK_DIVIDER, 0U);
	display_divider = modeset_divider(word & MODESET_DISPLAY_DIVIDER_FIELD);
	pipe_divider = modeset_divider((word >> MODESET_PIPE_DIVIDER_SHIFT) & MODESET_PIPE_DIVIDER_FIELD);
	input->display_khz = (uint32_t)((uint64_t)vco_khz * 4U / display_divider);
	input->pipe_khz = (uint32_t)((uint64_t)vco_khz * 4U / pipe_divider);

	/* The data return clock and the memory clock from the video BIOS. */
	error = modeset_bios_clock(device, MODESET_RETURN_CLOCK_PLL, MODESET_RETURN_CLOCK_ID, &input->return_khz);
	if (error != 0)
		return error;
	error = modeset_bios_clock(device, MODESET_MEMORY_CLOCK_PLL, MODESET_MEMORY_CLOCK_ID, &memory_khz);
	if (error != 0)
		return error;

	/* The fabric (1.08 times the memory clock, at least 506 MHz) and the memory's transfers. */
	fabric_mhz = (uint32_t)((uint64_t)memory_khz * MODESET_FABRIC_PER_MILLE / 1000000U);
	if (fabric_mhz < MODESET_FABRIC_LEAST_MHZ)
		fabric_mhz = MODESET_FABRIC_LEAST_MHZ;
	input->fabric_khz = fabric_mhz * 1000U;
	input->memory_mts = memory_khz * MODESET_TRANSFERS / 1000U;

	/* Succeeded: the clocks are in. */
	return 0;
}

/*
 * Gives a timing and a surface's pitch in pixels to a display plan's
 * input (the clocks are left as they are).
 */
void
amdrdna_mode_timing_input(
	const struct amdrdna_timing *timing,
	uint32_t pitch,
	struct amdrdna_mode_plan_input *input)
{
	/* The totals, the active area, the front porches and the pixel clock. */
	input->h_total = (uint32_t)timing->width + timing->h_blanking;
	input->width = timing->width;
	input->h_front = timing->h_porch;
	input->v_total = (uint32_t)timing->height + timing->v_blanking;
	input->height = timing->height;
	input->v_front = timing->v_porch;
	input->pixel_khz = timing->pixel_khz;

	/* The surface. */
	input->pitch = pitch;
}

/*
 * Reports the mode the display shows: the firmware's, read from the
 * timing generator at the first call (a mode the monitor lists with the
 * same totals gives its exact pixel clock), or the last one set.  The
 * caller holds the display's mode lock.
 *
 * Returns 0; ENOENT before stage A10 read the output's path; ENOTSUP for
 * an output that is not TMDS.
 */
int
amdrdna_mode_current(
	struct amdrdna_device *device,
	struct amdrdna_timing *timing)
{
	struct amdrdna_display *display;
	struct modeset_pipe pipe;
	int error;

	/* The lit TMDS output. */
	display = &device->display;
	error = modeset_pipe(device, &pipe);
	if (error != 0)
		return error;

	/* The firmware's mode, once: the timing generator's words, the listed pixel clock. */
	if (!display->mode_known) {
		modeset_timing_now(device, &pipe, &display->firmware_mode);
		modeset_listed(pipe.sink, &display->firmware_mode);
		display->mode = display->firmware_mode;
		display->mode_known = true;
	}

	/* Succeeded: the mode shown now. */
	*timing = display->mode;
	return 0;
}

/*
 * Sets a mode on the firmware's TMDS output by the whole sequence (the
 * picture dark for a third of a second) and makes it the display's mode;
 * the surface, its rows and the frames stay.  The caller holds the node's
 * display lock when the node exists, and the display's mode lock.
 *
 * Returns 0; ENOENT or ENOTSUP as amdrdna_mode_current() does; ERANGE for
 * a mode larger than the frames or one the plan refuses at the board's
 * clocks; EIO when the picture stops or underflows after the set; or the
 * error of a video BIOS routine.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hwss/dcn20/dcn20_hwseq.c
 * (dcn20_enable_stream_timing, dcn20_program_pipe) and
 * display/dc/link/link_dpms.c (the stream's off and on).
 */
int
amdrdna_mode_set(
	struct amdrdna_device *device,
	const struct amdrdna_timing *timing)
{
	struct amdrdna_display *display;
	struct amdrdna_timing now;
	struct amdrdna_mode_plan_input input;
	struct amdrdna_mode_plan plan;
	struct modeset_check check;
	struct modeset_pipe pipe;
	bool good;
	int error;

	/* The mode shown now, and the output. */
	display = &device->display;
	error = amdrdna_mode_current(device, &now);
	if (error != 0)
		return error;
	error = modeset_pipe(device, &pipe);
	if (error != 0)
		return error;

	/* A mode the frames hold: its rows in the stride, its lines in a frame. */
	if ((uint64_t)timing->width * MODESET_PIXEL_BYTES > display->stride ||
	    (uint64_t)display->stride * timing->height > display->frame_bytes) {
		amdrdna_mark("mode set refused: %ux%u is larger than the frames", timing->width, timing->height);
		return ERANGE;
	}

	/* The memory clock stays where it is while the mode changes; the plan at the board's clocks. */
	amdrdna_memory_display_hold(device);
	error = modeset_input(device, timing, &input);
	if (error != 0)
		return error;
	error = amdrdna_mode_plan(&input, &plan);
	if (error != 0) {
		amdrdna_mark("mode set refused: the plan refuses %ux%u at %ukHz (%d)", timing->width, timing->height,
		    (unsigned)timing->pixel_khz, error);
		return ERANGE;
	}

	/* The whole sequence, then a look at the picture. */
	error = modeset_switch(device, &pipe, timing, &plan, input.reference_khz);
	modeset_look(device, &pipe, timing, MODESET_GOOD_FRAMES, &check);

	/* The timing generator runs the new mode whatever happened; the output is lit. */
	display->mode = *timing;
	display->width = timing->width;
	display->height = timing->height;
	display->refresh_millihz = timing->refresh_millihz;
	display->output_dark = false;

	/* Reports a routine that failed, or a picture that stopped. */
	if (error != 0) {
		amdrdna_mark("mode set fail: %ux%u, a routine failed (%d)", timing->width, timing->height, error);
		return error;
	}
	good = modeset_good(&check);
	if (!good) {
		amdrdna_mark("mode set fail: %ux%u frames=%u underflow=%u/%u", timing->width, timing->height, (unsigned)check.frames,
		    (unsigned)check.underflow, (unsigned)check.timing_underflow);
		return EIO;
	}

	/* The display's part of the memory clock's levels again, for the new mode. */
	amdrdna_memory_display_start(device);

	/* Says how it went. */
	amdrdna_mark("mode set ok %ux%u@%u.%03u pclk=%ukHz (was %ux%u) start=%u frames=%u underflow=0 crc=%08x/%08x",
	    timing->width,
	    timing->height,
	    (unsigned)(timing->refresh_millihz / 1000U),
	    (unsigned)(timing->refresh_millihz % 1000U),
	    (unsigned)timing->pixel_khz,
	    now.width,
	    now.height,
	    (unsigned)plan.start_lines,
	    (unsigned)check.frames,
	    (unsigned)check.red_green,
	    (unsigned)check.blue);

	/* Succeeded: the mode is shown. */
	return 0;
}

/*
 * Works out the display plan of the mode the timing generator shows, at
 * the clocks the board runs (or those the display's part of the memory
 * clock's levels gives instead), and with program set writes the plane's
 * fields, the hub's watermarks and the global sync from it under the
 * update lock, then looks at the picture.  Needs only the pipe stage A6
 * took over.  The caller holds the display's mode lock or runs the
 * bring-up or the reset.
 *
 * Returns 0; ENOENT before the takeover; the plan's error; or EIO when
 * the picture underflowed or stopped after the writes.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hwss/dcn20/dcn20_hwseq.c
 * (dcn20_program_front_end_for_ctx with the same stream: the pipe's
 * deadlines and the hub's watermarks of the new bandwidth state).
 */
int
amdrdna_mode_replan(
	struct amdrdna_device *device,
	bool program,
	struct amdrdna_mode_plan *plan)
{
	struct modeset_pipe pipe;
	struct amdrdna_timing now;
	struct amdrdna_mode_plan_input input;
	struct modeset_check check;
	int error;

	/* The pipe the firmware lit and stage A6 took over. */
	if (!device->display.taken)
		return ENOENT;
	pipe = (struct modeset_pipe){ 0 };
	pipe.timing = device->gop.timing;
	pipe.plane = device->gop.plane;
	pipe.pixel = device->gop.plane;

	/* The plan of the mode shown. */
	modeset_timing_now(device, &pipe, &now);
	error = modeset_input(device, &now, &input);
	if (error != 0)
		return error;
	error = amdrdna_mode_plan(&input, plan);
	if (error != 0)
		return error;

	/* Only the plan was asked. */
	if (!program)
		return 0;

	/* The plane's fields, the watermarks and the global sync, under the update lock, then a look. */
	modeset_lock(device, &pipe, true);
	modeset_plane(device, &pipe, plan);
	modeset_marks(device, plan, input.reference_khz);
	modeset_sync(device, &pipe, &now, plan);
	modeset_lock(device, &pipe, false);
	modeset_look(device, &pipe, &now, MODESET_LOOK_FRAMES, &check);
	if (!modeset_good(&check)) {
		amdrdna_mark("mode replan fail: %ux%u frames=%u underflow=%u/%u", now.width, now.height, (unsigned)check.frames,
		    (unsigned)check.underflow, (unsigned)check.timing_underflow);
		return EIO;
	}

	/* Succeeded: the mode runs on the new plan. */
	return 0;
}

/*
 * Turns the output off, the picture blanked and the transmitter off (the
 * monitor went away), or on again in the mode shown, the encoder's stream
 * set up, the transmitter on and the picture back.  The timing generator
 * runs on either way, so frames and their events go on.  The caller holds
 * the display's mode lock.
 *
 * Returns 0; ENOENT or ENOTSUP as amdrdna_mode_current() does; or the
 * error of a video BIOS routine.
 */
int
amdrdna_mode_output(
	struct amdrdna_device *device,
	bool on)
{
	struct amdrdna_timing now;
	struct modeset_pipe pipe;
	uint32_t pixel_10khz;
	int error;

	/* The mode shown, and the output. */
	error = amdrdna_mode_current(device, &now);
	if (error != 0)
		return error;
	error = modeset_pipe(device, &pipe);
	if (error != 0)
		return error;
	pixel_10khz = (now.pixel_khz + 5U) / 10U;

	/* Off: the picture blanked, the transmitter off; the output counts as dark whatever the routine said. */
	if (!on) {
		modeset_blank(device, &pipe, true);
		error = modeset_output(device, &pipe, MODESET_TRANSMITTER_OFF, pixel_10khz);
		device->display.output_dark = true;
		if (error != 0)
			return error;
		return 0;
	}

	/* On: the stream and the transmitter, two frames for the monitor to lock, then the picture. */
	error = modeset_output(device, &pipe, MODESET_TRANSMITTER_ON, pixel_10khz);
	(void)modeset_frames(device, &pipe, 2U);
	modeset_blank(device, &pipe, false);
	if (error != 0)
		return error;
	device->display.output_dark = false;

	/* Succeeded: the output is lit again. */
	return 0;
}

/* Tells whether a look at the picture was good: 30 frames at least and no underflow. */
static bool
modeset_good(
	const struct modeset_check *check)
{
	/* Too few frames, or an underflow of the plane or the timing generator. */
	if (check->frames < MODESET_GOOD_FRAMES)
		return false;
	if (check->underflow != 0U)
		return false;
	if (check->timing_underflow != 0U)
		return false;

	/* Succeeded: the picture went on without underflow. */
	return true;
}

/*
 * Finds the pipe the firmware lit and its output's mode: a TMDS output
 * stage A10 read the path of.  Returns 0, ENOENT or ENOTSUP.
 */
static int
modeset_pipe(
	struct amdrdna_device *device,
	struct modeset_pipe *pipe)
{
	/* Stage A10's path of a lit output. */
	pipe->sink = amdrdna_sink_ready(device);
	if (pipe->sink == NULL || !device->gop.has_output) {
		amdrdna_mark("mode set fail: no output path (stage A10 first)");
		return ENOENT;
	}

	/* A TMDS output. */
	if (device->gop.output_kind == AMDRDNA_OUTPUT_DVI) {
		pipe->mode = MODESET_MODE_DVI;
	} else if (device->gop.output_kind == AMDRDNA_OUTPUT_HDMI) {
		pipe->mode = MODESET_MODE_HDMI;
	} else {
		amdrdna_mark("mode set note: output kind %u is not TMDS", device->gop.output_kind);
		return ENOTSUP;
	}

	/* The timing generator, the plane and its pixel processor (the plane's number). */
	pipe->timing = device->gop.timing;
	pipe->plane = device->gop.plane;
	pipe->pixel = device->gop.plane;

	/* Succeeded: the pipe is known. */
	return 0;
}

/*
 * Gives the plan's input for a timing on the firmware's surface (its
 * pitch) at the clocks the board runs.  Returns 0, or the clock routine's
 * error.
 */
static int
modeset_input(
	struct amdrdna_device *device,
	const struct amdrdna_timing *timing,
	struct amdrdna_mode_plan_input *input)
{
	int error;

	/* The mode and the surface (the plane's pitch word counts one less). */
	*input = (struct amdrdna_mode_plan_input){ 0 };
	amdrdna_mode_timing_input(timing, device->gop.pitch_word + 1U, input);

	/* The clocks, and those the display's part of the memory clock's levels works with instead. */
	error = amdrdna_mode_clocks(device, input);
	if (error != 0)
		return error;
	amdrdna_memory_plan_clocks(device, input);

	/* Succeeded: the input is complete. */
	return 0;
}

/*
 * Takes the pixel clock and the refresh of a mode the monitor lists with
 * the same active area, blanks and refresh (within half a hertz), which
 * are exact where the measured ones are rounded.
 */
static void
modeset_listed(
	const struct amdrdna_sink *sink,
	struct amdrdna_timing *timing)
{
	const struct amdrdna_timing *mode;
	unsigned at;

	/* Each listed mode, until one is the same. */
	for (at = 0U; at < sink->edid.mode_count; at++) {
		mode = &sink->edid.modes[at];
		if (mode->pixel_khz == 0U || mode->interlaced)
			continue;
		if (mode->width != timing->width || mode->height != timing->height)
			continue;
		if (mode->h_blanking != timing->h_blanking || mode->v_blanking != timing->v_blanking)
			continue;
		if (mode->refresh_millihz + MODESET_SAME_REFRESH_MILLIHZ < timing->refresh_millihz)
			continue;
		if (mode->refresh_millihz > timing->refresh_millihz + MODESET_SAME_REFRESH_MILLIHZ)
			continue;
		timing->pixel_khz = mode->pixel_khz;
		timing->refresh_millihz = mode->refresh_millihz;
		return;
	}
}

/*
 * Reads a clock of the power controller's PLLs by the video BIOS's clock
 * routine (the PLL, the clock, the frequency command, the divider from
 * the register; the frequency in Hz comes back in the first word).
 * Returns 0 or the routine's error; EIO for a clock that reads 0.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_atom_get_smu_clockinfo).
 */
static int
modeset_bios_clock(
	struct amdrdna_device *device,
	uint32_t pll,
	uint32_t clock,
	uint32_t *khz)
{
	uint32_t words[AMDRDNA_COMMAND_WORDS];
	unsigned index;
	int error;

	/* The parameters: the PLL, the clock, the frequency command and no divider. */
	for (index = 0U; index < AMDRDNA_COMMAND_WORDS; index++)
		words[index] = 0U;
	words[0] = pll | (clock << 8) | (MODESET_CLOCK_FREQUENCY << 16);
	error = modeset_routine(device, MODESET_CLOCK_INFO_ROUTINE, words, "clock info");
	if (error != 0)
		return error;

	/* The frequency in Hz, as kHz. */
	*khz = words[0] / 1000U;
	if (*khz == 0U)
		return EIO;

	/* Succeeded: the clock is read. */
	return 0;
}

/* Gives a clock generator divider id's divider in quarters. */
static uint32_t
modeset_divider(
	uint32_t id)
{
	/* The id within the ranges. */
	if (id < 0x08U)
		id = 0x08U;
	if (id > 0x7fU)
		id = 0x7fU;

	/* The range's start and step. */
	if (id < 0x40U)
		return 8U + (id - 0x08U);
	if (id < 0x60U)
		return 64U + 2U * (id - 0x40U);
	if (id < 0x7eU)
		return 128U + 4U * (id - 0x60U);

	/* Succeeded: the last range. */
	return 248U + 264U * (id - 0x7eU);
}

/* Reads the timing the timing generator runs: its totals, blanks, syncs and polarities, and the measured refresh. */
static void
modeset_timing_now(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	struct amdrdna_timing *timing)
{
	uint32_t h_total;
	uint32_t v_total;
	uint32_t h_blank;
	uint32_t v_blank;
	uint32_t h_start;
	uint32_t h_end;
	uint32_t v_start;
	uint32_t v_end;

	/* The words. */
	h_total = (modeset_read(device, AMDRDNA_DCN_TIMING_H_TOTAL, pipe->timing) & MODESET_LOW_FIELD) + 1U;
	v_total = (modeset_read(device, AMDRDNA_DCN_TIMING_V_TOTAL, pipe->timing) & MODESET_LOW_FIELD) + 1U;
	h_blank = modeset_read(device, AMDRDNA_DCN_TIMING_H_BLANK, pipe->timing);
	v_blank = modeset_read(device, AMDRDNA_DCN_TIMING_V_BLANK, pipe->timing);
	h_start = h_blank & MODESET_LOW_FIELD;
	h_end = (h_blank >> MODESET_HIGH_SHIFT) & MODESET_LOW_FIELD;
	v_start = v_blank & MODESET_LOW_FIELD;
	v_end = (v_blank >> MODESET_HIGH_SHIFT) & MODESET_LOW_FIELD;

	/* The timing: the active area between the blank's end and start, the front porch after it, the syncs from the start of the line. */
	*timing = (struct amdrdna_timing){ 0 };
	timing->width = (uint16_t)(h_start - h_end);
	timing->h_blanking = (uint16_t)(h_total - timing->width);
	timing->h_porch = (uint16_t)(h_total - h_start);
	timing->h_pulse = (uint16_t)((modeset_read(device, AMDRDNA_DCN_TIMING_H_SYNC, pipe->timing) >> MODESET_HIGH_SHIFT) &
	    MODESET_LOW_FIELD);
	timing->height = (uint16_t)(v_start - v_end);
	timing->v_blanking = (uint16_t)(v_total - timing->height);
	timing->v_porch = (uint16_t)(v_total - v_start);
	timing->v_pulse = (uint16_t)((modeset_read(device, AMDRDNA_DCN_TIMING_V_SYNC, pipe->timing) >> MODESET_HIGH_SHIFT) &
	    MODESET_LOW_FIELD);
	timing->h_positive = true;
	if ((modeset_read(device, AMDRDNA_DCN_TIMING_H_SYNC_CONTROL, pipe->timing) & MODESET_SYNC_NEGATIVE) != 0U)
		timing->h_positive = false;
	timing->v_positive = true;
	if ((modeset_read(device, AMDRDNA_DCN_TIMING_V_SYNC_CONTROL, pipe->timing) & MODESET_SYNC_NEGATIVE) != 0U)
		timing->v_positive = false;

	/* The pixel clock from the totals and the measured refresh, to 10 kHz. */
	timing->refresh_millihz = device->display.refresh_millihz;
	timing->pixel_khz = (uint32_t)(((uint64_t)h_total * v_total * device->display.refresh_millihz + 5000000U) / 10000000U) *
	    10U;
}

/*
 * Chooses the other mode: 1280x720 when the monitor lists it and the
 * plan fits, else the first listed mode of another size the plan fits
 * (of the modes whose syncs are known).
 * Returns NULL when none does.
 */
static const struct amdrdna_timing *
modeset_other(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	const struct amdrdna_timing *now)
{
	struct amdrdna_mode_plan_input input;
	struct amdrdna_mode_plan plan;
	const struct amdrdna_timing *mode;
	const struct amdrdna_timing *found;
	unsigned at;
	int error;

	/* The clocks once (the timing is replaced per mode). */
	error = modeset_input(device, now, &input);
	if (error != 0)
		return NULL;

	/* Each listed mode of another size, 1280x720 first. */
	found = NULL;
	for (at = 0U; at < pipe->sink->edid.mode_count; at++) {
		mode = &pipe->sink->edid.modes[at];
		if (mode->pixel_khz == 0U || mode->interlaced)
			continue;
		if (mode->h_pulse == 0U || mode->v_pulse == 0U)
			continue;
		if (mode->width == now->width && mode->height == now->height)
			continue;
		input.h_total = (uint32_t)mode->width + mode->h_blanking;
		input.width = mode->width;
		input.h_front = mode->h_porch;
		input.v_total = (uint32_t)mode->height + mode->v_blanking;
		input.height = mode->height;
		input.v_front = mode->v_porch;
		input.pixel_khz = mode->pixel_khz;
		error = amdrdna_mode_plan(&input, &plan);
		if (error != 0)
			continue;
		if (mode->width == 1280U && mode->height == 720U)
			return mode;
		if (found == NULL)
			found = mode;
	}

	/* Succeeded: the first that fits, or none. */
	return found;
}

/*
 * Sets a mode by the whole sequence: the picture blanked and the
 * transmitter off, the timing generator stopped, the pixel clock (the
 * first PHY PLL), the timing, the pixel processor's sizes, the plane's
 * viewport and fields, the watermarks and the global sync, the timing
 * generator started, then the encoder's stream and the transmitter, and
 * the picture shown.  Returns 0 or a routine's error (the rest still
 * runs, so the output comes back).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hwss/dcn20/dcn20_hwseq.c
 * (dcn20_enable_stream_timing), display/dc/optc/dcn10/dcn10_optc.c
 * (optc1_disable_crtc, optc1_program_timing, optc1_set_vtg_params),
 * display/dc/optc/dcn20/dcn20_optc.c (optc2_enable_crtc),
 * display/dc/dce/dce_clock_source.c (dcn20_program_pix_clk,
 * dce112_program_pix_clk) and display/dc/bios/command_table2.c
 * (set_pixel_clock_v7).
 */
static int
modeset_switch(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	const struct amdrdna_timing *timing,
	const struct amdrdna_mode_plan *plan,
	uint32_t reference_khz)
{
	struct amdrdna_pixel_clock_command clock;
	uint32_t words[AMDRDNA_COMMAND_WORDS];
	uint32_t word;
	uint32_t pixel_10khz;
	unsigned long irq;
	unsigned waited;
	int error;
	int step;

	/* The picture blanked and the transmitter off. */
	pixel_10khz = (timing->pixel_khz + 5U) / 10U;
	modeset_blank(device, pipe, true);
	error = modeset_output(device, pipe, MODESET_TRANSMITTER_OFF, pixel_10khz);

	/* The timing generator stopped at the end of the blank's first line, and its vertical generator. */
	word = modeset_read(device, AMDRDNA_DCN_TIMING_CONTROL, pipe->timing);
	word = (word & ~MODESET_MASTER_ENABLE) | MODESET_DISABLE_POINT;
	modeset_write(device, AMDRDNA_DCN_TIMING_CONTROL, pipe->timing, word);
	word = modeset_read(device, AMDRDNA_DCN_VERTICAL_GENERATOR, pipe->timing);
	modeset_write(device, AMDRDNA_DCN_VERTICAL_GENERATOR, pipe->timing, word & ~MODESET_VERTICAL_ENABLE);
	for (waited = 0U; waited < MODESET_WAIT_US; waited += MODESET_STEP_US) {
		word = modeset_read(device, AMDRDNA_DCN_TIMING_CLOCK_CONTROL, pipe->timing);
		if ((word & MODESET_TIMING_BUSY) == 0U)
			break;
		kern_usleep_range(MODESET_STEP_US, MODESET_STEP_US * 2U);
	}

	/*
	 * The pixel clock on the first PHY PLL, for this timing generator, the
	 * encoder and the mode: the reference driver's first free clock
	 * source.  The firmware clocks the timing generator from the output's
	 * own PHY's PLL, which the transmitter's off powers down with the PHY
	 * (the routine then never sees it lock); the first PLL runs apart from
	 * the transmitter.
	 */
	clock = (struct amdrdna_pixel_clock_command){ 0 };
	clock.pixel_100hz = timing->pixel_khz * 10U;
	clock.pll = (uint8_t)MODESET_FIRST_PHY_PLL;
	clock.encoder_object = (uint8_t)(pipe->sink->connector.encoder_object & 255U);
	clock.mode = (uint8_t)pipe->mode;
	clock.flags = MODESET_CLOCK_FLAGS;
	clock.generator = (uint8_t)pipe->timing;
	amdrdna_command_pixel_clock(words, &clock);
	step = modeset_routine(device, MODESET_CLOCK_ROUTINE, words, "pixel clock");
	if (error == 0)
		error = step;
	modeset_note(device, pipe, "clock");

	/* The timing, the sizes, the plane, the watermarks and the global sync. */
	modeset_timing(device, pipe, timing);
	modeset_sizes(device, pipe, timing);
	modeset_plane(device, pipe, plan);
	modeset_marks(device, plan, reference_khz);
	modeset_sync(device, pipe, timing, plan);

	/* The timing generator's frame counter starts again: the refresh count takes its next reading as a new base. */
	irq = spin_lock_irqsave(&device->display.lock);

	device->display.frame_valid = false;

	spin_unlock_irqrestore(&device->display.lock, irq);

	/* The timing generator started: its vertical generator first, then the master enable. */
	word = modeset_read(device, AMDRDNA_DCN_VERTICAL_GENERATOR, pipe->timing);
	modeset_write(device, AMDRDNA_DCN_VERTICAL_GENERATOR, pipe->timing, word | MODESET_VERTICAL_ENABLE);
	word = modeset_read(device, AMDRDNA_DCN_TIMING_CONTROL, pipe->timing);
	modeset_write(device, AMDRDNA_DCN_TIMING_CONTROL, pipe->timing, word | MODESET_DISABLE_POINT | MODESET_MASTER_ENABLE);
	for (waited = 0U; waited < MODESET_WAIT_US; waited += MODESET_STEP_US) {
		word = modeset_read(device, AMDRDNA_DCN_TIMING_CONTROL, pipe->timing);
		if ((word & MODESET_MASTER_STATE) != 0U)
			break;
		kern_usleep_range(MODESET_STEP_US, MODESET_STEP_US * 2U);
	}
	modeset_note(device, pipe, "started");

	/* The monitor without a signal a while, then the stream and the transmitter, two frames, and the picture. */
	kern_usleep_range(MODESET_DARK_US, MODESET_DARK_US + MODESET_STEP_US);
	(void)modeset_frames(device, pipe, 2U);
	step = modeset_output(device, pipe, MODESET_TRANSMITTER_ON, pixel_10khz);
	if (error == 0)
		error = step;
	(void)modeset_frames(device, pipe, 2U);
	modeset_blank(device, pipe, false);
	modeset_note(device, pipe, "shown");

	/* Reports a routine that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the mode is set. */
	return 0;
}

/*
 * Writes a timing to the stopped timing generator: the totals (less one),
 * the horizontal and vertical blanks (from the end of the front porch
 * back over the active area), the syncs from the start of the line and
 * their polarities, the total's limits, no interlace.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/optc/dcn10/dcn10_optc.c
 * (optc1_program_timing, optc1_set_vtotal_min_max).
 */
static void
modeset_timing(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	const struct amdrdna_timing *timing)
{
	uint32_t h_total;
	uint32_t v_total;
	uint32_t blank_start;
	uint32_t blank_end;
	uint32_t word;

	/* The horizontal timing. */
	h_total = (uint32_t)timing->width + timing->h_blanking;
	blank_start = h_total - timing->h_porch;
	blank_end = blank_start - timing->width;
	modeset_write(device, AMDRDNA_DCN_TIMING_H_TOTAL, pipe->timing, h_total - 1U);
	modeset_write(device, AMDRDNA_DCN_TIMING_H_SYNC, pipe->timing, (uint32_t)timing->h_pulse << MODESET_HIGH_SHIFT);
	modeset_write(device, AMDRDNA_DCN_TIMING_H_BLANK, pipe->timing, blank_start | (blank_end << MODESET_HIGH_SHIFT));
	word = modeset_read(device, AMDRDNA_DCN_TIMING_H_SYNC_CONTROL, pipe->timing) & ~MODESET_SYNC_NEGATIVE;
	if (!timing->h_positive)
		word |= MODESET_SYNC_NEGATIVE;
	modeset_write(device, AMDRDNA_DCN_TIMING_H_SYNC_CONTROL, pipe->timing, word);

	/* The vertical timing and the total's limits. */
	v_total = (uint32_t)timing->height + timing->v_blanking;
	blank_start = v_total - timing->v_porch;
	blank_end = blank_start - timing->height;
	modeset_write(device, AMDRDNA_DCN_TIMING_V_TOTAL, pipe->timing, v_total - 1U);
	modeset_write(device, AMDRDNA_DCN_TIMING_V_TOTAL_MAX, pipe->timing, v_total - 1U);
	modeset_write(device, AMDRDNA_DCN_TIMING_V_TOTAL_MIN, pipe->timing, v_total - 1U);
	modeset_write(device, AMDRDNA_DCN_TIMING_V_SYNC, pipe->timing, (uint32_t)timing->v_pulse << MODESET_HIGH_SHIFT);
	modeset_write(device, AMDRDNA_DCN_TIMING_V_BLANK, pipe->timing, blank_start | (blank_end << MODESET_HIGH_SHIFT));
	word = modeset_read(device, AMDRDNA_DCN_TIMING_V_SYNC_CONTROL, pipe->timing) & ~MODESET_SYNC_NEGATIVE;
	if (!timing->v_positive)
		word |= MODESET_SYNC_NEGATIVE;
	modeset_write(device, AMDRDNA_DCN_TIMING_V_SYNC_CONTROL, pipe->timing, word);

	/* Progressive (the start point stays as the firmware set it for the output). */
	modeset_write(device, AMDRDNA_DCN_TIMING_INTERLACE, pipe->timing, 0U);
}

/*
 * Writes the sizes of a mode to the pixel processor (its copy of the
 * blank, the rectangle it draws and the blend size) and the plane's
 * viewport (both, from the surface's top left).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dpp/dcn10/dcn10_dpp_dscl.c
 * (dpp1_dscl_set_recout, dpp1_dscl_set_scaler_manual_scale: the blend
 * size) and display/dc/hubp/dcn10/dcn10_hubp.c (hubp1_set_viewport).
 */
static void
modeset_sizes(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	const struct amdrdna_timing *timing)
{
	uint32_t h_total;
	uint32_t v_total;
	uint32_t blank_start;
	uint32_t size;

	/* The pixel processor's copy of the blanks. */
	h_total = (uint32_t)timing->width + timing->h_blanking;
	blank_start = h_total - timing->h_porch;
	modeset_write(device, AMDRDNA_DCN_SCALER_H_BLANK, pipe->pixel,
	    blank_start | ((blank_start - timing->width) << MODESET_HIGH_SHIFT));
	v_total = (uint32_t)timing->height + timing->v_blanking;
	blank_start = v_total - timing->v_porch;
	modeset_write(device, AMDRDNA_DCN_SCALER_V_BLANK, pipe->pixel,
	    blank_start | ((blank_start - timing->height) << MODESET_HIGH_SHIFT));

	/* The rectangle, the blend size and the viewports: the whole active area from the top left. */
	size = ((uint32_t)timing->width & MODESET_SIZE_FIELD) | (((uint32_t)timing->height & MODESET_SIZE_FIELD) << MODESET_HIGH_SHIFT);
	modeset_write(device, AMDRDNA_DCN_SCALER_OUTPUT_START, pipe->pixel, 0U);
	modeset_write(device, AMDRDNA_DCN_SCALER_OUTPUT_SIZE, pipe->pixel, size);
	modeset_write(device, AMDRDNA_DCN_SCALER_BLEND_SIZE, pipe->pixel, size);
	modeset_write(device, AMDRDNA_DCN_PLANE_VIEWPORT_START, pipe->plane, 0U);
	modeset_write(device, AMDRDNA_DCN_PLANE_VIEWPORT_SIZE, pipe->plane, size);
	modeset_write(device, AMDRDNA_DCN_PLANE_SECOND_VIEWPORT_START, pipe->plane, 0U);
	modeset_write(device, AMDRDNA_DCN_PLANE_SECOND_VIEWPORT_SIZE, pipe->plane, size);
}

/*
 * Writes the global sync (the start line, the update's offset and width,
 * the ready offset) and the vertical timing generator's first line and
 * second front porch.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/optc/dcn10/dcn10_optc.c
 * (optc1_program_global_sync, optc1_set_vtg_params).
 */
static void
modeset_sync(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	const struct amdrdna_timing *timing,
	const struct amdrdna_mode_plan *plan)
{
	uint32_t first_line;
	uint32_t blank_end;
	uint32_t porch;
	uint32_t word;

	/* The global sync. */
	modeset_write(device, AMDRDNA_DCN_TIMING_START_LINE, pipe->timing, plan->start_lines & MODESET_START_FIELD);
	modeset_write(device, AMDRDNA_DCN_TIMING_UPDATE, pipe->timing,
	    (plan->update_offset & MODESET_OFFSET_FIELD) | ((plan->update_width & MODESET_UPDATE_WIDTH_FIELD) << MODESET_HIGH_SHIFT));
	modeset_write(device, AMDRDNA_DCN_TIMING_READY, pipe->timing, plan->ready_offset & MODESET_OFFSET_FIELD);

	/* The first line is the blank's start; the second front porch is how far the start line reaches before the blank's end. */
	first_line = (uint32_t)timing->height + timing->v_blanking - timing->v_porch;
	blank_end = first_line - timing->height;
	porch = 0U;
	if (plan->start_lines > blank_end + 1U)
		porch = plan->start_lines - (blank_end + 1U);
	word = modeset_read(device, AMDRDNA_DCN_VERTICAL_GENERATOR, pipe->timing) & MODESET_VERTICAL_ENABLE;
	word |= (porch & MODESET_LINE15_FIELD) | ((first_line & MODESET_LINE15_FIELD) << MODESET_HIGH_SHIFT);
	modeset_write(device, AMDRDNA_DCN_VERTICAL_GENERATOR, pipe->timing, word);
}

/*
 * Writes the plan's request, deadline and arbitration fields to the
 * plane, and its ready at or after the sync.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_vready_at_or_After_vsync, hubp2_program_requestor,
 * hubp2_program_deadline, hubp2_setup_interdependent).
 */
static void
modeset_plane(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	const struct amdrdna_mode_plan *plan)
{
	uint32_t word;
	uint32_t delivery;

	/* The debug word's "dv" mode off, and ready at or after the sync. */
	modeset_write(device, AMDRDNA_DCN_PLANE_REQUEST_DEBUG, pipe->plane, MODESET_PLANE_DEBUG);
	word = modeset_read(device, AMDRDNA_DCN_PLANE_CONTROL, pipe->plane) & ~(MODESET_PLANE_READY_AFTER | MODESET_PLANE_UNDERFLOW_CLEAR);
	if (plan->ready_after_zero != 0U)
		word |= MODESET_PLANE_READY_AFTER;
	modeset_write(device, AMDRDNA_DCN_PLANE_CONTROL, pipe->plane, word);

	/* The requests: the expansion modes and the sizes (no second plane). */
	modeset_write(device, AMDRDNA_DCN_PLANE_EXPANSION, pipe->plane,
	    (plan->data_expansion << MODESET_DATA_EXPANSION_SHIFT) | (plan->cursor_expansion << MODESET_CURSOR_EXPANSION_SHIFT) |
	    (plan->meta_expansion << MODESET_META_EXPANSION_SHIFT) | (plan->pte_expansion << MODESET_PTE_EXPANSION_SHIFT));
	word = (plan->swath_log << MODESET_SWATH_SHIFT) | (plan->pte_rows_log << MODESET_PTE_ROWS_SHIFT);
	word |= (plan->chunk_log << MODESET_CHUNK_SHIFT) | (plan->least_chunk_log << MODESET_LEAST_CHUNK_SHIFT);
	word |= (plan->meta_chunk_log << MODESET_META_CHUNK_SHIFT) | (plan->least_meta_chunk_log << MODESET_LEAST_META_SHIFT);
	word |= (plan->pte_group_log << MODESET_PTE_GROUP_SHIFT) | (plan->meta_pte_group_log << MODESET_META_GROUP_SHIFT);
	modeset_write(device, AMDRDNA_DCN_PLANE_REQUEST_SIZE, pipe->plane, word);
	modeset_write(device, AMDRDNA_DCN_PLANE_REQUEST_SIZE_CHROMA, pipe->plane, 0U);

	/* The deadline: the blank, the next start, a line, the delay after the scaler, the reference over the pixel clock. */
	modeset_write(device, AMDRDNA_DCN_PLANE_BLANK_OFFSET, pipe->plane,
	    (plan->blank_end_cycles & MODESET_CYCLES13_FIELD) | ((plan->blank_end_line & MODESET_LINE15_FIELD) << MODESET_HIGH_SHIFT));
	modeset_write(device, AMDRDNA_DCN_PLANE_NEXT_START, pipe->plane, plan->next_start_quarters & MODESET_NEXT_START_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_LINE_CYCLES, pipe->plane, plan->line_cycles & MODESET_CYCLES21_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_AFTER_SCALER, pipe->plane,
	    (plan->after_scaler_cycles & MODESET_CYCLES13_FIELD) | ((plan->after_scaler_lines & MODESET_AFTER_LINES_FIELD) << MODESET_HIGH_SHIFT));
	modeset_write(device, AMDRDNA_DCN_PLANE_REFERENCE_RATIO, pipe->plane, plan->reference_to_pixel & MODESET_CYCLES21_FIELD);

	/* The page table and meta rows in the blank and in the active area (luma; chroma as luma for meta). */
	modeset_write(device, AMDRDNA_DCN_PLANE_VBLANK_PTE, pipe->plane, plan->pte_group_vblank_cycles & MODESET_CYCLES23_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_PTE_ROW_LINES, pipe->plane, plan->pte_row_quarters & MODESET_LINES17_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_PTE_GROUP, pipe->plane, plan->pte_group_cycles & MODESET_CYCLES23_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_META_ROW_LINES, pipe->plane, plan->meta_row_quarters & MODESET_LINES17_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_META_CHUNK, pipe->plane, plan->meta_chunk_cycles & MODESET_CYCLES23_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_VBLANK_PTE_CHROMA, pipe->plane, 0U);
	modeset_write(device, AMDRDNA_DCN_PLANE_PTE_ROW_LINES_CHROMA, pipe->plane, 0U);
	modeset_write(device, AMDRDNA_DCN_PLANE_PTE_GROUP_CHROMA, pipe->plane, 0U);
	modeset_write(device, AMDRDNA_DCN_PLANE_META_ROW_LINES_CHROMA, pipe->plane, plan->meta_row_quarters & MODESET_LINES17_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_META_CHUNK_CHROMA, pipe->plane, 0U);
	modeset_write(device, AMDRDNA_DCN_PLANE_LINE_DELIVERY, pipe->plane, plan->line_delivery_cycles & MODESET_CYCLES13_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_REQUEST_LIMIT, pipe->plane, plan->request_limit & MODESET_LINE15_FIELD);

	/* The arbitration: the levels and the luma, chroma and cursor deliveries with their fixed levels. */
	modeset_write(device, AMDRDNA_DCN_PLANE_LEVEL_MARKS, pipe->plane,
	    (plan->low_level_mark & MODESET_LEVEL_FIELD) | ((plan->high_level_mark & MODESET_LEVEL_FIELD) << MODESET_HIGH_SHIFT));
	delivery = plan->request_delivery_cycles & MODESET_DELIVERY_FIELD;
	modeset_write(device, AMDRDNA_DCN_PLANE_LUMA_TTU, pipe->plane, delivery | (plan->fixed_level << MODESET_FIXED_LEVEL_SHIFT));
	modeset_write(device, AMDRDNA_DCN_PLANE_CHROMA_TTU, pipe->plane, plan->fixed_level << MODESET_FIXED_LEVEL_SHIFT);
	delivery = plan->cursor_delivery_cycles & MODESET_DELIVERY_FIELD;
	modeset_write(device, AMDRDNA_DCN_PLANE_CURSOR_TTU, pipe->plane, delivery | (plan->fixed_level << MODESET_FIXED_LEVEL_SHIFT));
	modeset_write(device, AMDRDNA_DCN_PLANE_FLIP_PTE, pipe->plane, plan->pte_group_flip_cycles & MODESET_CYCLES23_FIELD);

	/* The prefetch: its lines and ratio, the rows' lines in the blank and in a flip, the meta chunks, the line delivery. */
	modeset_write(device, AMDRDNA_DCN_PLANE_PREFETCH, pipe->plane,
	    (plan->prefetch_ratio & MODESET_RATIO_FIELD) | ((plan->prefetch_quarters & MODESET_PREFETCH_FIELD) << MODESET_PREFETCH_SHIFT));
	modeset_write(device, AMDRDNA_DCN_PLANE_PREFETCH_CHROMA, pipe->plane, plan->prefetch_ratio_chroma & MODESET_RATIO_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_VBLANK_LINES, pipe->plane,
	    (plan->vm_vblank_quarters & MODESET_VM_LINES_FIELD) | ((plan->row_vblank_quarters & MODESET_ROW_LINES_FIELD) << MODESET_ROW_LINES_SHIFT));
	modeset_write(device, AMDRDNA_DCN_PLANE_FLIP_LINES, pipe->plane,
	    (plan->vm_flip_quarters & MODESET_VM_LINES_FIELD) | ((plan->row_flip_quarters & MODESET_ROW_LINES_FIELD) << MODESET_ROW_LINES_SHIFT));
	modeset_write(device, AMDRDNA_DCN_PLANE_VBLANK_META, pipe->plane, plan->meta_chunk_vblank_cycles & MODESET_CYCLES23_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_VBLANK_META_CHROMA, pipe->plane, plan->meta_chunk_vblank_cycles & MODESET_CYCLES23_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_FLIP_META, pipe->plane, plan->meta_chunk_flip_cycles & MODESET_CYCLES23_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_LINE_DELIVERY_PREFETCH, pipe->plane, plan->line_delivery_prefetch_cycles & MODESET_CYCLES13_FIELD);

	/* The prefetch's request deliveries, and the blank's time with the flip's level. */
	modeset_write(device, AMDRDNA_DCN_PLANE_LUMA_TTU_PREFETCH, pipe->plane, plan->request_delivery_prefetch_cycles & MODESET_DELIVERY_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_CHROMA_TTU_PREFETCH, pipe->plane, 0U);
	modeset_write(device, AMDRDNA_DCN_PLANE_CURSOR_TTU_PREFETCH, pipe->plane, plan->cursor_delivery_prefetch_cycles & MODESET_DELIVERY_FIELD);
	modeset_write(device, AMDRDNA_DCN_PLANE_SECOND_CURSOR_TTU_PREFETCH, pipe->plane, 0U);
	modeset_write(device, AMDRDNA_DCN_PLANE_GLOBAL_TTU, pipe->plane,
	    (plan->vblank_ttu_cycles & MODESET_TTU_TIME_FIELD) | (plan->flip_level << MODESET_FLIP_LEVEL_SHIFT));
}

/*
 * Writes the hub's four watermark sets alike (the board's clocks do not
 * change), in reference clock cycles, and its saturation level and least
 * outstanding requests.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubbub/dcn20/dcn20_hubbub.c
 * (hubbub2_program_watermarks) and display/dc/hubbub/dcn10/dcn10_hubbub.c
 * (hubbub1_program_urgent_watermarks, hubbub1_program_stutter_watermarks,
 * hubbub1_program_pstate_watermarks, convert_and_clamp).
 */
static void
modeset_marks(
	struct amdrdna_device *device,
	const struct amdrdna_mode_plan *plan,
	uint32_t reference_khz)
{
	uint32_t urgent;
	uint32_t trip;
	uint32_t enter;
	uint32_t leave;
	uint32_t change;
	uint32_t word;
	unsigned set;

	/* Each watermark in cycles of the reference, at most 21 bits. */
	urgent = (uint32_t)((uint64_t)plan->urgent_ns * reference_khz / 1000000U);
	trip = (uint32_t)((uint64_t)plan->trip_ns * reference_khz / 1000000U);
	enter = (uint32_t)((uint64_t)plan->refresh_enter_exit_ns * reference_khz / 1000000U);
	leave = (uint32_t)((uint64_t)plan->refresh_exit_ns * reference_khz / 1000000U);
	change = (uint32_t)((uint64_t)plan->switch_ns * reference_khz / 1000000U);
	if (urgent > MODESET_MARK_FIELD)
		urgent = MODESET_MARK_FIELD;
	if (trip > MODESET_MARK_FIELD)
		trip = MODESET_MARK_FIELD;
	if (enter > MODESET_MARK_FIELD)
		enter = MODESET_MARK_FIELD;
	if (leave > MODESET_MARK_FIELD)
		leave = MODESET_MARK_FIELD;
	if (change > MODESET_MARK_FIELD)
		change = MODESET_MARK_FIELD;

	/* The four sets alike. */
	for (set = 0U; set < MODESET_MARK_SETS; set++) {
		modeset_write(device, AMDRDNA_DCN_HUB_URGENT_MARK, set, urgent);
		modeset_write(device, AMDRDNA_DCN_HUB_TRIP_MARK, set, trip);
		modeset_write(device, AMDRDNA_DCN_HUB_ENTER_MARK, set, enter);
		modeset_write(device, AMDRDNA_DCN_HUB_EXIT_MARK, set, leave);
		modeset_write(device, AMDRDNA_DCN_HUB_SWITCH_MARK, set, change);
	}

	/* The saturation level (60 us of the reference) and the least outstanding requests. */
	modeset_write(device, AMDRDNA_DCN_HUB_SATURATION, 0U, MODESET_SATURATION_US * (reference_khz / 1000U));
	word = modeset_read(device, AMDRDNA_DCN_HUB_OUTSTANDING, 0U) & ~(MODESET_OUTSTANDING_FIELD << MODESET_OUTSTANDING_SHIFT);
	modeset_write(device, AMDRDNA_DCN_HUB_OUTSTANDING, 0U, word | (MODESET_LEAST_OUTSTANDING << MODESET_OUTSTANDING_SHIFT));
}

/* Runs one of the display routines; a failure is said with what it was. */
static int
modeset_routine(
	struct amdrdna_device *device,
	unsigned routine,
	uint32_t *words,
	const char *what)
{
	uint32_t steps;
	int error;

	/* The routine on the board's registers. */
	error = amdrdna_bios_run(device, routine, words, AMDRDNA_COMMAND_WORDS, &steps);
	if (error != 0) {
		amdrdna_mark("mode set note: the %s routine failed (%d)", what, error);
		return error;
	}

	/* Succeeded: the routine ran. */
	return 0;
}

/*
 * Turns the output off (the transmitter) or on (the encoder's stream set
 * up for the pixel clock, then the transmitter) by the video BIOS's
 * routines.  Returns 0 or the routine's error.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dio/dcn10/dcn10_link_encoder.c
 * (dcn10_link_encoder_disable_output, dcn10_link_encoder_enable_tmds_output),
 * display/dc/dio/dcn10/dcn10_stream_encoder.c
 * (enc1_stream_encoder_dvi_set_stream_attribute) and
 * display/dc/bios/command_table2.c (encoder_control_digx_v1_5,
 * transmitter_control_v1_6).
 */
static int
modeset_output(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	uint32_t action,
	uint32_t pixel_10khz)
{
	struct amdrdna_encoder_command encoder;
	struct amdrdna_transmitter_command transmitter;
	uint32_t words[AMDRDNA_COMMAND_WORDS];
	int error;

	/* The encoder's stream first when the output comes on. */
	if (action == MODESET_TRANSMITTER_ON) {
		encoder = (struct amdrdna_encoder_command){ 0 };
		encoder.dig = (uint8_t)device->gop.output_front;
		encoder.action = MODESET_STREAM_SETUP;
		encoder.mode = (uint8_t)pipe->mode;
		encoder.lanes = MODESET_LANES;
		encoder.pixel_10khz = pixel_10khz;
		if (pipe->mode == MODESET_MODE_HDMI)
			encoder.bits_per_colour = MODESET_EIGHT_BITS;
		amdrdna_command_encoder(words, &encoder);
		error = modeset_routine(device, MODESET_ENCODER_ROUTINE, words, "encoder setup");
		if (error != 0)
			return error;
	}

	/* The transmitter: off names the connector, on the lanes and the symbol clock. */
	transmitter = (struct amdrdna_transmitter_command){ 0 };
	transmitter.phy = (uint8_t)pipe->sink->connector.transmitter;
	transmitter.action = (uint8_t)action;
	transmitter.mode = (uint8_t)pipe->mode;
	transmitter.hotplug = (uint8_t)(device->gop.output_hotplug + 1U);
	if (action == MODESET_TRANSMITTER_ON) {
		transmitter.lanes = MODESET_LANES;
		transmitter.symbol_10khz = pixel_10khz;
	} else {
		transmitter.connector_id = (uint8_t)(pipe->sink->connector.connector_object & 255U);
	}
	amdrdna_command_transmitter(words, &transmitter);
	error = modeset_routine(device, MODESET_TRANSMITTER_ROUTINE, words, "transmitter");
	if (error != 0)
		return error;

	/* Succeeded: the output is off or on. */
	return 0;
}

/* Takes or lets go of the timing generator's update lock (the double-buffered words apply at once on the release). */
static void
modeset_lock(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	bool locked)
{
	uint32_t word;
	unsigned waited;

	/* The lock bit. */
	word = 0U;
	if (locked)
		word = MODESET_LOCK;
	modeset_write(device, AMDRDNA_DCN_TIMING_UPDATE_LOCK, pipe->timing, word);
	if (!locked)
		return;

	/* A lock waits for its status. */
	for (waited = 0U; waited < MODESET_WAIT_US; waited += MODESET_STEP_US) {
		word = modeset_read(device, AMDRDNA_DCN_TIMING_UPDATE_LOCK, pipe->timing);
		if ((word & MODESET_LOCK_STATUS) != 0U)
			return;
		kern_usleep_range(MODESET_STEP_US, MODESET_STEP_US * 2U);
	}
}

/* Blanks the timing generator's picture (black) or shows it, and waits for the generator to take it. */
static void
modeset_blank(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	bool blank)
{
	uint32_t word;
	uint32_t wanted;
	unsigned waited;

	/* The blank bit. */
	word = modeset_read(device, AMDRDNA_DCN_TIMING_BLANK, pipe->timing);
	wanted = 0U;
	if (blank) {
		word |= MODESET_BLANK_DATA;
		wanted = MODESET_BLANK_STATE;
	} else {
		word &= ~MODESET_BLANK_DATA;
	}
	modeset_write(device, AMDRDNA_DCN_TIMING_BLANK, pipe->timing, word);

	/* The generator's state at the next frame. */
	for (waited = 0U; waited < MODESET_WAIT_US; waited += MODESET_STEP_US) {
		word = modeset_read(device, AMDRDNA_DCN_TIMING_BLANK, pipe->timing);
		if ((word & MODESET_BLANK_STATE) == wanted)
			return;
		kern_usleep_range(MODESET_STEP_US, MODESET_STEP_US * 2U);
	}
}

/* Waits for a number of frames of the timing generator; false when they did not pass in time. */
static bool
modeset_frames(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	unsigned frames)
{
	uint32_t first;
	uint32_t now;
	unsigned waited;

	/* The frame counter until it moved on that far (a 24-bit counter). */
	first = modeset_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, pipe->timing);
	for (waited = 0U; waited < MODESET_WAIT_US * frames; waited += MODESET_STEP_US) {
		now = modeset_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, pipe->timing);
		if (((now - first) & MODESET_FRAME_FIELD) >= frames)
			return true;
		kern_usleep_range(MODESET_STEP_US, MODESET_STEP_US * 2U);
	}

	/* Succeeded: no; the frames did not pass. */
	return false;
}

/*
 * Looks at the picture for a number of frames: the underflow bits cleared
 * first, then the frames counted, the underflow bits read and the
 * picture's CRC taken over its active area.
 */
static void
modeset_look(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	const struct amdrdna_timing *timing,
	unsigned frames,
	struct modeset_check *check)
{
	uint32_t word;
	uint32_t first;
	uint32_t last;

	/* The underflow bits cleared: the plane's and the timing generator's input. */
	word = modeset_read(device, AMDRDNA_DCN_PLANE_CONTROL, pipe->plane);
	modeset_write(device, AMDRDNA_DCN_PLANE_CONTROL, pipe->plane, word | MODESET_PLANE_UNDERFLOW_CLEAR);
	word = modeset_read(device, AMDRDNA_DCN_COMBINER_INPUT, pipe->timing);
	modeset_write(device, AMDRDNA_DCN_COMBINER_INPUT, pipe->timing, word | MODESET_TIMING_UNDERFLOW_CLEAR);

	/* The frames. */
	first = modeset_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, pipe->timing);
	(void)modeset_frames(device, pipe, frames);
	last = modeset_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, pipe->timing);
	check->frames = (last - first) & MODESET_FRAME_FIELD;

	/* The underflow bits and the CRC (on, continuous, two frames). */
	check->underflow = amdrdna_display_underflow(device, &check->timing_underflow);
	modeset_write(device, AMDRDNA_DCN_TIMING_CRC_CONTROL, pipe->timing, 0U);
	amdrdna_timing_crc_window(device, pipe->timing, timing->width, timing->height);
	modeset_write(device, AMDRDNA_DCN_TIMING_CRC_CONTROL, pipe->timing, MODESET_CRC_CONTINUOUS_ON);
	(void)modeset_frames(device, pipe, 2U);
	check->red_green = modeset_read(device, AMDRDNA_DCN_TIMING_CRC_RED_GREEN, pipe->timing);
	check->blue = modeset_read(device, AMDRDNA_DCN_TIMING_CRC_BLUE, pipe->timing);
}

/* Puts the first pipe's display words in the log, eight to a line, after a step that failed. */
static void
modeset_dump(
	struct amdrdna_device *device,
	const char *step)
{
	uint32_t values[8];
	uint32_t at;
	size_t range;
	unsigned slot;

	/* Each range, eight words a line. */
	for (range = 0U; range < sizeof(modeset_ranges) / sizeof(modeset_ranges[0]); range++) {
		for (at = 0U; at < modeset_ranges[range].count; at += 8U) {
			for (slot = 0U; slot < 8U; slot++)
				values[slot] = amdrdna_read(device, modeset_ranges[range].first + at + slot);
			amdrdna_mark("A12 dump %s 0x%05x: %08x %08x %08x %08x %08x %08x %08x %08x",
			    step,
			    (unsigned)(modeset_ranges[range].first + at),
			    (unsigned)values[0], (unsigned)values[1], (unsigned)values[2], (unsigned)values[3],
			    (unsigned)values[4], (unsigned)values[5], (unsigned)values[6], (unsigned)values[7]);
		}
	}
}

/* Notes the timing generator's state after a step of the mode set: its control and clock words, its pixel clock's sources and its frame and position. */
static void
modeset_note(
	struct amdrdna_device *device,
	const struct modeset_pipe *pipe,
	const char *step)
{
	/* One line of the words. */
	amdrdna_mark("A12 step %s: control=0x%08x clock=0x%08x rate=0x%08x phy=0x%08x frame=%u position=0x%08x",
	    step,
	    (unsigned)modeset_read(device, AMDRDNA_DCN_TIMING_CONTROL, pipe->timing),
	    (unsigned)modeset_read(device, AMDRDNA_DCN_TIMING_CLOCK_CONTROL, pipe->timing),
	    (unsigned)modeset_read(device, AMDRDNA_DCN_PIXEL_RATE_CONTROL, pipe->timing),
	    (unsigned)modeset_read(device, AMDRDNA_DCN_PIXEL_RATE_SOURCE, pipe->timing),
	    (unsigned)(modeset_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, pipe->timing) & MODESET_FRAME_FIELD),
	    (unsigned)modeset_read(device, AMDRDNA_DCN_TIMING_POSITION, pipe->timing));
}

/* Reads a display register (0 when the display block lacks it). */
static uint32_t
modeset_read(
	struct amdrdna_device *device,
	enum amdrdna_dcn_register which,
	unsigned instance)
{
	uint32_t index;
	uint32_t value;

	/* Its index, then the read. */
	index = amdrdna_dcn_index(&device->blocks.blocks[AMDRDNA_BLOCK_DCN], which, instance);
	if (index == 0U)
		return 0U;
	value = amdrdna_read(device, index);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes a display register (nothing when the display block lacks it). */
static void
modeset_write(
	struct amdrdna_device *device,
	enum amdrdna_dcn_register which,
	unsigned instance,
	uint32_t value)
{
	uint32_t index;

	/* Its index, then the write. */
	index = amdrdna_dcn_index(&device->blocks.blocks[AMDRDNA_BLOCK_DCN], which, instance);
	if (index == 0U)
		return;
	amdrdna_write(device, index, value);
}
