/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transcoders and pipes of the display 12 and 13 code (see xd-pipe.h).
 */

#include "xd-pipe.h"
#include "xd-dp.h"
#include "xd-io.h"
#include "xd-platform.h"

#include "../intel/display-regs-xd.h"
#include "../intel/display-regs-xd9.h"

#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/*
 * How long a stopping transcoder may take to report itself off; how long
 * a pipe's scan line may take to start or stop moving, the time between
 * the two reads that tell, and the shortest and longest sleeps between
 * the tries.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (intel_wait_for_pipe_off), Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_vblank.c (pipe_scanline_is_moving,
 * wait_for_pipe_scanline_moving), Linux v6.8.12
 * drivers/gpu/drm/i915/i915_utils.h (wait_for).
 */
#define I915_XD_PIPE_OFF_US		100000U
#define I915_XD_PIPE_SCANLINE_US	100000U
#define I915_XD_PIPE_SCANLINE_GAP_US	5000U
#define I915_XD_PIPE_POLL_MIN_US	10U
#define I915_XD_PIPE_POLL_MAX_US	1000U

/*
 * The limits of a plane's buffer: the largest side, the pitch unit, the
 * page a surface starts on; the bytes of an XRGB8888 pixel.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_universal_plane.c
 * (skl_plane_max_width, skl_plane_stride_mult).
 */
#define I915_XD_PLANE_MAX_SIDE		8192U
#define I915_XD_PLANE_PITCH_UNIT	64U
#define I915_XD_PLANE_PAGE		0x1000U
#define I915_XD_PLANE_CPP		4U

/*
 * How long before a vertical blank an update that must land in one frame
 * stays away from it; how many times, 1 us apart, a scan line read as 0
 * is read again (the counter may read 0 just before the blank).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_crtc.h
 * (VBLANK_EVASION_TIME_US), intel_vblank.c (__intel_get_crtc_scanline).
 */
#define I915_XD_PIPE_EVADE_US		100U
#define I915_XD_PIPE_ZERO_REREADS	100U

/*
 * How long a display 9 transcoder may take to start or stop, and how long
 * a vertical blank may take to come (a 24 Hz mode with room).
 *
 * Values referenced: Intel Kaby Lake PRM Volume 12, Display Engine (the
 * transcoder enable and disable sequences).
 */
#define I915_XD9_TRANS_WAIT_US		100000U
#define I915_XD9_VBLANK_WAIT_MS		100U

static uint32_t i915_xd_pipe_pair(uint32_t lower, uint32_t upper);
static uint32_t i915_xd_pipe_vrr_word(const struct i915_xd_platform *platform, const struct i915_xd_vrr *vrr);
static int i915_xd_pipe_scanline_moves(struct i915_xd_io *io, unsigned pipe);
static uint32_t i915_xd_pipe_chicken(unsigned trans);

/*
 * Writes a transcoder's DisplayPort M/N values: the data M with the
 * transfer unit, the data N, the link M, and last the link N, which arms
 * the update of all four.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (intel_set_m_n, intel_cpu_transcoder_set_m1_n1).
 */
void
drv_i915_xd_pipe_m_n(
	struct i915_xd_io *io,
	unsigned trans,
	const struct i915_xd_dp_m_n *m_n)
{
	uint32_t data_m;

	/* The data M carries the transfer unit less one. */
	data_m = i915_xd_field_put(I915_XD_MN_TU_SIZE, m_n->tu - 1U) | (m_n->data_m & I915_XD_MN_VALUE);

	/* The four words, the link N last. */
	drv_i915_xd_io_write(io, I915_XD_TRANS_DATA_M1(trans), data_m);
	drv_i915_xd_io_write(io, I915_XD_TRANS_DATA_N1(trans), m_n->data_n);
	drv_i915_xd_io_write(io, I915_XD_TRANS_LINK_M1(trans), m_n->link_m);
	drv_i915_xd_io_write(io, I915_XD_TRANS_LINK_N1(trans), m_n->link_n);
}

/*
 * Writes a transcoder's timing: on display 13 the set context latency
 * (the lines from the end of the active area to the vertical blank
 * start, which the blank start word then no longer holds: it is written
 * as 1 to stand out), the vertical sync shift of an interlaced mode,
 * then the horizontal and the vertical totals, blanks and syncs (each
 * value less one).  An interlaced mode's vertical total and blank end
 * lose a line the hardware adds itself.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (intel_set_transcoder_timings).
 */
void
drv_i915_xd_pipe_timing(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned trans,
	const struct i915_xd_timing *timing)
{
	uint32_t vtotal;
	uint32_t vblank_start;
	uint32_t vblank_end;
	int shift;

	/* The vertical words of an interlaced mode, and its sync shift by half a line. */
	vtotal = timing->vtotal;
	vblank_start = timing->vblank_start;
	vblank_end = timing->vblank_end;
	shift = 0;
	if (timing->interlaced) {
		vtotal -= 1U;
		vblank_end -= 1U;
		shift = (int)timing->hsync_start - (int)(timing->htotal / 2U);
		if (shift < 0)
			shift += (int)timing->htotal;
	}

	/* Display 13 keeps the blank start in the context latency. */
	if (platform->display_ver >= 13U) {
		drv_i915_xd_io_write(io, I915_XD_TRANS_SET_CONTEXT_LATENCY(trans), vblank_start - timing->vdisplay);
		vblank_start = 1U;
	}

	/* The sync shift. */
	drv_i915_xd_io_write(io, I915_XD_TRANS_VSYNCSHIFT(trans), (uint32_t)shift);

	/* The horizontal words. */
	drv_i915_xd_io_write(io, I915_XD_TRANS_HTOTAL(trans), i915_xd_pipe_pair(timing->hdisplay - 1U, timing->htotal - 1U));
	drv_i915_xd_io_write(io, I915_XD_TRANS_HBLANK(trans), i915_xd_pipe_pair(timing->hblank_start - 1U, timing->hblank_end - 1U));
	drv_i915_xd_io_write(io, I915_XD_TRANS_HSYNC(trans), i915_xd_pipe_pair(timing->hsync_start - 1U, timing->hsync_end - 1U));

	/* The vertical words. */
	drv_i915_xd_io_write(io, I915_XD_TRANS_VTOTAL(trans), i915_xd_pipe_pair(timing->vdisplay - 1U, vtotal - 1U));
	drv_i915_xd_io_write(io, I915_XD_TRANS_VBLANK(trans), i915_xd_pipe_pair(vblank_start - 1U, vblank_end - 1U));
	drv_i915_xd_io_write(io, I915_XD_TRANS_VSYNC(trans), i915_xd_pipe_pair(timing->vsync_start - 1U, timing->vsync_end - 1U));
}

/*
 * Sets up a transcoder for a mode: the M/N values of a DisplayPort
 * stream, the timing, the variable refresh words (the vblank with delay
 * first; with a fixed refresh only the control word, cleared), the pixel
 * multiplier, the frame start delay, and the configuration word (enabled
 * only when it runs already), flushed with a read.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (hsw_configure_cpu_transcoder, hsw_set_frame_start_delay,
 * hsw_set_transconf), Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vrr.c
 * (intel_vrr_set_transcoder_timings, trans_vrr_ctl).
 */
void
drv_i915_xd_pipe_configure(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_trans_setup *setup)
{
	uint32_t conf;
	unsigned trans;

	/* The M/N values of a DisplayPort stream, and the timing. */
	trans = setup->trans;
	drv_i915_xd_io_step(io, "xd-pipe configure");
	if (setup->dp)
		drv_i915_xd_pipe_m_n(io, trans, &setup->m_n);
	drv_i915_xd_pipe_timing(io, platform, trans, &setup->timing);

	/* The vblank with the delay of the context latency (display 12 and 13), then the refresh words. */
	if (platform->display_ver >= 12U)
		drv_i915_xd_io_rmw(io, i915_xd_pipe_chicken(trans), 0U, I915_XD_CHICKEN_TRANS_VBLANK_DELAY);
	if (setup->vrr.flipline == 0U) {
		drv_i915_xd_io_write(io, I915_XD_TRANS_VRR_CTL(trans), 0U);
	} else {
		drv_i915_xd_io_write(io, I915_XD_TRANS_VRR_VMIN(trans), setup->vrr.vmin - 1U);
		drv_i915_xd_io_write(io, I915_XD_TRANS_VRR_VMAX(trans), setup->vrr.vmax - 1U);
		drv_i915_xd_io_write(io, I915_XD_TRANS_VRR_CTL(trans), i915_xd_pipe_vrr_word(platform, &setup->vrr));
		drv_i915_xd_io_write(io, I915_XD_TRANS_VRR_FLIPLINE(trans), setup->vrr.flipline - 1U);
	}

	/* The pixel multiplier and the frame start delay, each less one. */
	drv_i915_xd_io_write(io, I915_XD_TRANS_MULT(trans), setup->pixel_multiplier - 1U);
	drv_i915_xd_io_rmw(io, i915_xd_pipe_chicken(trans), I915_XD_CHICKEN_TRANS_FRAME_START,
		i915_xd_field_put(I915_XD_CHICKEN_TRANS_FRAME_START, setup->frame_start_delay - 1U));

	/* The configuration word: kept enabled when it runs, the interlace field. */
	conf = 0U;
	if (setup->running)
		conf |= I915_XD_TRANS_CONF_ENABLE;
	if (setup->timing.interlaced)
		conf |= i915_xd_field_put(I915_XD_TRANS_CONF_INTERLACE, I915_XD_TRANS_CONF_INTERLACED);
	drv_i915_xd_io_write(io, I915_XD_TRANS_CONF(trans), conf);
	drv_i915_xd_io_posting_read(io, I915_XD_TRANS_CONF(trans));
}

/*
 * Writes a pipe's source size: the size the planes are scaled from, the
 * width in the upper half and the height in the lower, each less one.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (intel_set_pipe_src_size).
 */
void
drv_i915_xd_pipe_source(
	struct i915_xd_io *io,
	unsigned pipe,
	uint32_t width,
	uint32_t height)
{
	/* The size word. */
	drv_i915_xd_io_write(io, I915_XD_PIPE_SRCSZ(pipe), i915_xd_pipe_pair(height - 1U, width - 1U));
}

/*
 * Writes a pipe's miscellaneous word: the colour depth of what it sends
 * (12 bits only on display 13; an unknown depth leaves the field at 8
 * bits), dithering, YUV output (4:2:0 in full blend), HDR precision, and
 * the rounding by truncation.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (bdw_set_pipe_misc).
 */
void
drv_i915_xd_pipe_misc(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned pipe,
	const struct i915_xd_pipe_misc *misc)
{
	uint32_t word;

	/* The colour depth. */
	word = 0U;
	switch (misc->bpp) {
	case 18U:
		word |= i915_xd_field_put(I915_XD_PIPE_MISC_BPC, I915_XD_PIPE_MISC_BPC_6);
		break;
	case 24U:
		word |= i915_xd_field_put(I915_XD_PIPE_MISC_BPC, I915_XD_PIPE_MISC_BPC_8);
		break;
	case 30U:
		word |= i915_xd_field_put(I915_XD_PIPE_MISC_BPC, I915_XD_PIPE_MISC_BPC_10);
		break;
	case 36U:
		if (platform->display_ver >= 13U)
			word |= i915_xd_field_put(I915_XD_PIPE_MISC_BPC, I915_XD_PIPE_MISC_BPC_12);
		break;
	default:
		break;
	}

	/* Dithering, of the spatial kind (type 0). */
	if (misc->dither)
		word |= I915_XD_PIPE_MISC_DITHER;

	/* YUV output, and 4:2:0 in full blend. */
	if (misc->output == I915_XD_OUTPUT_YCBCR420 || misc->output == I915_XD_OUTPUT_YCBCR444)
		word |= I915_XD_PIPE_MISC_OUTPUT_YUV;
	if (misc->output == I915_XD_OUTPUT_YCBCR420)
		word |= I915_XD_PIPE_MISC_YUV420 | I915_XD_PIPE_MISC_YUV420_FULL_BLEND;

	/* HDR precision, and the rounding by truncation. */
	if (misc->hdr)
		word |= I915_XD_PIPE_MISC_HDR_PRECISION;
	word |= I915_XD_PIPE_MISC_ROUNDING_TRUNC;

	/* The word. */
	drv_i915_xd_io_write(io, I915_XD_PIPE_MISC(pipe), word);
}

/*
 * Sets a pipe's chicken bits: per-pixel alpha 0 and 0xff bypass the
 * blending math, frame buffer pixels pass unmodified, and on display 13
 * the underrun recovery stays off.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (icl_set_pipe_chicken).
 */
void
drv_i915_xd_pipe_chicken(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned pipe)
{
	uint32_t word;

	/* The word with the pass-through bits. */
	word = drv_i915_xd_io_read(io, I915_XD_PIPE_CHICKEN(pipe));
	word |= I915_XD_PIPE_CHICKEN_ALPHA_BYPASS | I915_XD_PIPE_CHICKEN_FB_PASSTHRU;

	/* Display 13 keeps the underrun recovery off. */
	if (platform->display_ver >= 13U)
		word |= I915_XD_PIPE_CHICKEN_NO_UNDERRUN_RECOVERY;

	/* The word back. */
	drv_i915_xd_io_write(io, I915_XD_PIPE_CHICKEN(pipe), word);
}

/*
 * Writes a pipe's colour words: the bottom colour black (it is not the
 * client's to choose), the gamma mode and the CSC mode.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_color.c
 * (icl_color_commit_arm).
 */
void
drv_i915_xd_pipe_colors(
	struct i915_xd_io *io,
	unsigned pipe,
	uint32_t gamma_mode,
	uint32_t csc_mode)
{
	/* The three words. */
	drv_i915_xd_io_write(io, I915_XD_PIPE_BOTTOM_COLOR(pipe), 0U);
	drv_i915_xd_io_write(io, I915_XD_GAMMA_MODE(pipe), gamma_mode);
	drv_i915_xd_io_write(io, I915_XD_CSC_MODE(pipe), csc_mode);
}

/*
 * Writes a DisplayPort stream's MSA word: a synchronous clock, the colour
 * depth (an unknown depth is sent as 6 bits), CEA RGB for limited range,
 * YCbCr 4:4:4, and the pointer to the VSC SDP.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_set_dp_msa).
 */
void
drv_i915_xd_pipe_msa(
	struct i915_xd_io *io,
	unsigned trans,
	const struct i915_xd_msa *msa)
{
	uint32_t word;

	/* The clock and the depth. */
	word = I915_XD_MSA_SYNC_CLOCK;
	switch (msa->bpp) {
	case 24U:
		word |= I915_XD_MSA_BPC_8;
		break;
	case 30U:
		word |= I915_XD_MSA_BPC_10;
		break;
	case 36U:
		word |= I915_XD_MSA_BPC_12;
		break;
	default:
		word |= I915_XD_MSA_BPC_6;
		break;
	}

	/* The colour format. */
	if (msa->cea_rgb)
		word |= I915_XD_MSA_CEA_RGB;
	if (msa->ycbcr444)
		word |= I915_XD_MSA_YCBCR_444_BT709;
	if (msa->vsc_sdp)
		word |= I915_XD_MSA_VSC_SDP;

	/* The word. */
	drv_i915_xd_io_write(io, I915_XD_TRANS_MSA_MISC(trans), word);
}

/*
 * Computes a transcoder's DDI function word with its enable: the DDI
 * (its index plus one), the colour depth (an unknown depth as 6 bits),
 * the sync polarities, and the mode: HDMI or DVI with scrambling and the
 * high character rate, DisplayPort multi-stream with its lanes and
 * master, or single stream with its lanes.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_transcoder_func_reg_val_get).
 */
uint32_t
drv_i915_xd_pipe_func_word(
	const struct i915_xd_trans_func *func)
{
	uint32_t word;
	uint32_t bpc;

	/* The enable and the DDI. */
	word = I915_XD_DDI_FUNC_ENABLE | i915_xd_field_put(I915_XD_DDI_FUNC_SELECT, func->ddi + 1U);

	/* The colour depth. */
	switch (func->bpp) {
	case 24U:
		bpc = I915_XD_DDI_FUNC_BPC_8;
		break;
	case 30U:
		bpc = I915_XD_DDI_FUNC_BPC_10;
		break;
	case 36U:
		bpc = I915_XD_DDI_FUNC_BPC_12;
		break;
	default:
		bpc = I915_XD_DDI_FUNC_BPC_6;
		break;
	}

	/* Puts the bits per colour into the word. */
	word |= i915_xd_field_put(I915_XD_DDI_FUNC_BPC, bpc);

	/* The sync polarities. */
	if (func->vsync_high)
		word |= I915_XD_DDI_FUNC_VSYNC_HIGH;
	if (func->hsync_high)
		word |= I915_XD_DDI_FUNC_HSYNC_HIGH;

	/* HDMI or DVI, with scrambling and the high character rate. */
	if (func->mode == I915_XD_FUNC_HDMI || func->mode == I915_XD_FUNC_DVI) {
		word |= i915_xd_field_put(I915_XD_DDI_FUNC_MODE, func->mode);
		if (func->scrambling)
			word |= I915_XD_DDI_FUNC_SCRAMBLING;
		if (func->high_tmds)
			word |= I915_XD_DDI_FUNC_HIGH_TMDS;
		return word;
	}

	/* DisplayPort: the stream mode, the lanes, and a multi-stream master. */
	word |= i915_xd_field_put(I915_XD_DDI_FUNC_MODE, func->mode);
	word |= i915_xd_field_put(I915_XD_DDI_FUNC_WIDTH, func->lanes - 1U);
	if (func->mode == I915_XD_FUNC_DP_MST)
		word |= i915_xd_field_put(I915_XD_DDI_FUNC_MST_MASTER, func->mst_master);

	/* Succeeded: the word. */
	return word;
}

/*
 * Enables a transcoder's DDI function: the second word (the port sync
 * master, or none) first, then the function word with its enable.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_enable_transcoder_func, bdw_trans_port_sync_master_select).
 */
void
drv_i915_xd_pipe_func_enable(
	struct i915_xd_io *io,
	unsigned trans,
	const struct i915_xd_trans_func *func)
{
	uint32_t second;
	uint32_t word;

	/* The port sync master selects its transcoder's index plus one. */
	second = 0U;
	if (func->sync_master != I915_XD_NO_TRANSCODER) {
		second = I915_XD_DDI_FUNC2_PORT_SYNC |
			 i915_xd_field_put(I915_XD_DDI_FUNC2_PORT_SYNC_MASTER, func->sync_master + 1U);
	}

	/* Writes the second word. */
	drv_i915_xd_io_write(io, I915_XD_TRANS_DDI_FUNC_CTL2(trans), second);

	/* The function. */
	word = drv_i915_xd_pipe_func_word(func);
	drv_i915_xd_io_write(io, I915_XD_TRANS_DDI_FUNC_CTL(trans), word);
}

/*
 * Computes a display 11 transcoder's DDI function word with its enable:
 * the word of drv_i915_xd_pipe_func_word() with the DDI in display 11's
 * select (the DDI index itself), and on the eDP transcoder the pipe it
 * takes its input from (pipe A always on, B or C switched).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (TRANS_DDI_SELECT_PORT, TRANS_DDI_EDP_INPUT_A_ON,
 * TRANS_DDI_EDP_INPUT_B_ONOFF, TRANS_DDI_EDP_INPUT_C_ONOFF).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_transcoder_func_reg_val_get).
 */
uint32_t
drv_i915_xd_pipe_func_word_11(
	unsigned trans,
	unsigned pipe,
	const struct i915_xd_trans_func *func)
{
	uint32_t word;
	uint32_t input;

	/* The word, its DDI moved into display 11's select. */
	word = drv_i915_xd_pipe_func_word(func);
	word &= ~I915_XD_DDI_FUNC_SELECT;
	word |= i915_xd_field_put(I915_XD_DDI_FUNC_SELECT_11, func->ddi);

	/* Only the eDP transcoder names its pipe. */
	if (trans != I915_XD_TRANS_EDP)
		return word;

	/* Pipe A keeps the panel on; pipes B and C switch it. */
	switch (pipe) {
	case 1U:
		input = I915_XD_DDI_FUNC_EDP_INPUT_B_ONOFF;
		break;
	case 2U:
		input = I915_XD_DDI_FUNC_EDP_INPUT_C_ONOFF;
		break;
	default:
		input = I915_XD_DDI_FUNC_EDP_INPUT_A_ON;
		break;
	}

	/* Puts the eDP input into the word. */
	word |= i915_xd_field_put(I915_XD_DDI_FUNC_EDP_INPUT, input);

	/* Succeeded: the word. */
	return word;
}

/*
 * Enables a display 11 transcoder's DDI function, as
 * drv_i915_xd_pipe_func_enable() does with the display 11 word: the
 * second word (the port sync master, or none) first, then the function
 * word with its enable.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_enable_transcoder_func).
 */
void
drv_i915_xd_pipe_func_enable_11(
	struct i915_xd_io *io,
	unsigned trans,
	unsigned pipe,
	const struct i915_xd_trans_func *func)
{
	uint32_t second;
	uint32_t word;

	/* The port sync master selects its transcoder's index plus one. */
	second = 0U;
	if (func->sync_master != I915_XD_NO_TRANSCODER) {
		second = I915_XD_DDI_FUNC2_PORT_SYNC |
			 i915_xd_field_put(I915_XD_DDI_FUNC2_PORT_SYNC_MASTER, func->sync_master + 1U);
	}

	/* Writes the second word. */
	drv_i915_xd_io_write(io, I915_XD_TRANS_DDI_FUNC_CTL2(trans), second);

	/* The function. */
	word = drv_i915_xd_pipe_func_word_11(trans, pipe, func);
	drv_i915_xd_io_write(io, I915_XD_TRANS_DDI_FUNC_CTL(trans), word);
}

/*
 * Points a transcoder's clock at a DDI's: the DDI index plus one, in
 * display 11's field or in the later one.  Display 11's eDP transcoder
 * runs on DDI A's clock without a select and is left alone.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (TRANS_CLK_SEL_PORT, TGL_TRANS_CLK_SEL_PORT).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_enable_transcoder_clock).
 */
void
drv_i915_xd_pipe_clock_on(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned trans,
	unsigned ddi)
{
	uint32_t word;

	/* The eDP transcoder has no select. */
	if (trans == I915_XD_TRANS_EDP)
		return;

	/* The DDI index plus one in the platform's field. */
	word = i915_xd_field_put(I915_XD_TRANS_CLK_SEL_FIELD, ddi + 1U);
	if (platform->display_ver < 12U)
		word = i915_xd_field_put(I915_XD_TRANS_CLK_SEL_FIELD_11, ddi + 1U);
	drv_i915_xd_io_write(io, I915_XD_TRANS_CLK_SEL(trans), word);
}

/*
 * Takes a transcoder's clock off its DDI.  Display 11's eDP transcoder
 * has no select and is left alone.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_disable_transcoder_clock).
 */
void
drv_i915_xd_pipe_clock_off(
	struct i915_xd_io *io,
	unsigned trans)
{
	/* The eDP transcoder has no select. */
	if (trans == I915_XD_TRANS_EDP)
		return;

	/* No DDI's clock. */
	drv_i915_xd_io_write(io, I915_XD_TRANS_CLK_SEL(trans), I915_XD_TRANS_CLK_SEL_NONE);
}

/*
 * Writes a transcoder's DDI function without its enable, ahead of a
 * DisplayPort link's training.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_config_transcoder_func).
 */
void
drv_i915_xd_pipe_func_config(
	struct i915_xd_io *io,
	unsigned trans,
	const struct i915_xd_trans_func *func)
{
	uint32_t word;

	/* The function word, its enable cleared. */
	word = drv_i915_xd_pipe_func_word(func) & ~I915_XD_DDI_FUNC_ENABLE;
	drv_i915_xd_io_write(io, I915_XD_TRANS_DDI_FUNC_CTL(trans), word);
}

/*
 * Disables a transcoder's DDI function: the second word cleared, then the
 * function word without its enable and, unless the transcoder is a
 * multi-stream master, without its DDI and mode.  Returns the word as it
 * was read (the caller looks at its HDCP signalling).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_disable_transcoder_func).
 */
uint32_t
drv_i915_xd_pipe_func_disable(
	struct i915_xd_io *io,
	unsigned trans,
	int mst_master)
{
	uint32_t read;
	uint32_t word;

	/* The second word. */
	drv_i915_xd_io_write(io, I915_XD_TRANS_DDI_FUNC_CTL2(trans), 0U);

	/* The function word without its enable; a master keeps its DDI and mode. */
	read = drv_i915_xd_io_read(io, I915_XD_TRANS_DDI_FUNC_CTL(trans));
	word = read & ~I915_XD_DDI_FUNC_ENABLE;
	if (!mst_master)
		word &= ~(I915_XD_DDI_FUNC_SELECT | I915_XD_DDI_FUNC_MODE);
	drv_i915_xd_io_write(io, I915_XD_TRANS_DDI_FUNC_CTL(trans), word);

	/* Succeeded: the word as it was. */
	return read;
}

/*
 * Starts a transcoder whose planes are off: on display 13 the pipe
 * arbiter takes the programmed slots first; a transcoder that runs
 * already is left alone.  Returns 1 when it ran already, 0 when it was
 * started.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (intel_enable_transcoder).
 */
int
drv_i915_xd_pipe_start(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned trans)
{
	uint32_t conf;

	/* Display 13's arbiter slots (the pipe of the same index). */
	drv_i915_xd_io_step(io, "xd-pipe start");
	if (platform->display_ver == 13U)
		drv_i915_xd_io_rmw(io, I915_XD_PIPE_ARB_CTL(trans), 0U, I915_XD_PIPE_ARB_PROG_SLOTS);

	/* A transcoder that runs stays as it is. */
	conf = drv_i915_xd_io_read(io, I915_XD_TRANS_CONF(trans));
	if ((conf & I915_XD_TRANS_CONF_ENABLE) != 0U)
		return 1;

	/* The enable, flushed. */
	drv_i915_xd_io_write(io, I915_XD_TRANS_CONF(trans), conf | I915_XD_TRANS_CONF_ENABLE);
	drv_i915_xd_io_posting_read(io, I915_XD_TRANS_CONF(trans));

	/* Succeeded: the transcoder was started. */
	return 0;
}

/*
 * Stops a transcoder whose planes are off: its enable cleared, the FEC
 * stall released on display 12 and later (display 11 has no such bit),
 * and up to 100 ms for it to report itself off.  A transcoder that is off
 * is left alone (*was_running 0).  Returns the I915_XD_PIPE_LATE_ bits of
 * the wait that ran out.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (intel_disable_transcoder, intel_wait_for_pipe_off).
 */
unsigned
drv_i915_xd_pipe_stop(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned trans,
	int *was_running)
{
	uint32_t conf;
	uint32_t last;
	int error;

	/* A transcoder that is off stays as it is. */
	drv_i915_xd_io_step(io, "xd-pipe stop");
	*was_running = 0;
	conf = drv_i915_xd_io_read(io, I915_XD_TRANS_CONF(trans));
	if ((conf & I915_XD_TRANS_CONF_ENABLE) == 0U)
		return 0U;

	/* The enable cleared, then the FEC stall released where the transcoder has one. */
	*was_running = 1;
	drv_i915_xd_io_write(io, I915_XD_TRANS_CONF(trans), conf & ~I915_XD_TRANS_CONF_ENABLE);
	if (platform->display_ver >= 12U)
		drv_i915_xd_io_rmw(io, i915_xd_pipe_chicken(trans), I915_XD_CHICKEN_TRANS_FEC_STALL, 0U);

	/* The transcoder reports itself off. */
	error = drv_i915_xd_io_poll(io, I915_XD_TRANS_CONF(trans), I915_XD_TRANS_CONF_STATE, 0U, I915_XD_PIPE_OFF_US, &last);
	if (error != 0)
		return I915_XD_PIPE_LATE_OFF;

	/* Succeeded: the transcoder is off. */
	return 0U;
}

/*
 * Turns both scalers of a stopping pipe off and clears their windows,
 * used or not, so the next mode of the pipe starts unscaled.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/skl_scaler.c (skl_scaler_disable,
 * skl_detach_scaler).
 */
void
drv_i915_xd_pipe_scalers_off(
	struct i915_xd_io *io,
	unsigned pipe)
{
	unsigned scaler;

	/* Each scaler's control, then its window. */
	for (scaler = 0U; scaler < I915_XD_SCALERS; scaler++) {
		drv_i915_xd_io_write(io, I915_XD_PS_CTRL(pipe, scaler), 0U);
		drv_i915_xd_io_write(io, I915_XD_PS_WIN_POS(pipe, scaler), 0U);
		drv_i915_xd_io_write(io, I915_XD_PS_WIN_SZ(pipe, scaler), 0U);
	}
}

/*
 * Reads a pipe's frame counter, which moves at each vertical blank start
 * and starts again when the pipe is enabled.
 */
uint32_t
drv_i915_xd_pipe_frame(
	struct i915_xd_io *io,
	unsigned pipe)
{
	uint32_t frame;

	/* The counter. */
	frame = drv_i915_xd_io_read(io, I915_XD_PIPE_FRMCNT(pipe));

	/* Succeeded: the frame number. */
	return frame;
}

/*
 * Waits up to 100 ms for a pipe's scan line to start moving (moving
 * nonzero) or to stand still, trying with sleeps from 10 us that double
 * up to 1 ms.  Returns I915_XD_PIPE_LATE_SCANLINE when it did not.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vblank.c
 * (wait_for_pipe_scanline_moving).
 */
unsigned
drv_i915_xd_pipe_wait_scanline(
	struct i915_xd_io *io,
	unsigned pipe,
	int moving)
{
	unsigned left;
	unsigned sleep;
	int moves;
	int spent;

	/* Tries until the state comes, once more after the budget is spent. */
	left = I915_XD_PIPE_SCANLINE_US;
	sleep = I915_XD_PIPE_POLL_MIN_US;
	for (;;) {
		spent = left == 0U;
		moves = i915_xd_pipe_scanline_moves(io, pipe);
		if (moves == moving)
			return 0U;
		if (spent)
			break;

		/* The next sleep. */
		(void)drv_i915_xd_io_delay(io, sleep);
		if (sleep >= left) {
			left = 0U;
		} else {
			left -= sleep;
		}

		/* Doubles the sleep up to its longest. */
		if (sleep < I915_XD_PIPE_POLL_MAX_US)
			sleep <<= 1;
	}

	/* The scan line did not reach the state. */
	return I915_XD_PIPE_LATE_SCANLINE;
}

/*
 * Reads the line a pipe scans now: the counter, read again while it reads
 * 0 (up to 100 times 1 us apart), plus the counter's offset from the real
 * line, modulo the frame's lines.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vblank.c
 * (__intel_get_crtc_scanline).
 */
unsigned
drv_i915_xd_pipe_scanline(
	struct i915_xd_io *io,
	unsigned pipe,
	unsigned vtotal,
	unsigned offset)
{
	uint32_t line;
	uint32_t again;
	unsigned tries;

	/* The counter. */
	line = drv_i915_xd_io_read(io, I915_XD_PIPE_SCANLINE(pipe)) & I915_XD_PIPE_SCANLINE_LINE;

	/* A 0 may be the counter just before the blank: it is read again until it moves. */
	for (tries = 0U; line == 0U && tries < I915_XD_PIPE_ZERO_REREADS; tries++) {
		(void)drv_i915_xd_io_delay(io, 1U);
		again = drv_i915_xd_io_read(io, I915_XD_PIPE_SCANLINE(pipe)) & I915_XD_PIPE_SCANLINE_LINE;
		if (again != line) {
			line = again;
			break;
		}
	}

	/* A frame without lines has no line. */
	if (vtotal == 0U)
		return 0U;

	/* Succeeded: the line, with the counter's offset. */
	return (line + offset) % vtotal;
}

/*
 * Finds the lines an update that must land in one frame stays away from:
 * from 100 us before the vertical blank's start to the line before it.
 * A mode without a line length counts 100 us as one line.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vblank.c
 * (intel_crtc_vblank_evade_scanlines, intel_usecs_to_scanlines).
 */
void
drv_i915_xd_pipe_evade_window(
	uint32_t clock_khz,
	uint32_t htotal,
	uint32_t vblank_start,
	struct i915_xd_evade *evade)
{
	uint32_t lines;

	/* The lines 100 us take. */
	lines = 1U;
	if (htotal != 0U)
		lines = (I915_XD_PIPE_EVADE_US * clock_khz + 1000U * htotal - 1U) / (1000U * htotal);

	/* Succeeded: the window ends one line before the blank's start. */
	evade->vblank_start = (int)vblank_start;
	evade->min = (int)vblank_start - (int)lines;
	evade->max = (int)vblank_start - 1;
}

/*
 * Checks a buffer the first plane is to scan out: linear XRGB8888, 1 to
 * 8192 pixels each way, a pitch that holds a line and counts 64 bytes,
 * a surface on a page.  Returns 0 or EINVAL.
 */
int
drv_i915_xd_plane_check(
	uint32_t fourcc,
	uint64_t modifier,
	uint32_t width,
	uint32_t height,
	uint32_t pitch,
	uint32_t surf)
{
	/* The format and layout. */
	if (fourcc != I915_XD_FOURCC_XRGB8888)
		return EINVAL;
	if (modifier != I915_XD_MODIFIER_LINEAR)
		return EINVAL;

	/* The size. */
	if (width == 0U || height == 0U)
		return EINVAL;
	if (width > I915_XD_PLANE_MAX_SIDE || height > I915_XD_PLANE_MAX_SIDE)
		return EINVAL;

	/* The pitch and the surface. */
	if (pitch < width * I915_XD_PLANE_CPP)
		return EINVAL;
	if ((pitch % I915_XD_PLANE_PITCH_UNIT) != 0U)
		return EINVAL;
	if ((surf & (I915_XD_PLANE_PAGE - 1U)) != 0U)
		return EINVAL;

	/* Succeeded: the plane can scan the buffer out. */
	return 0;
}

/*
 * Fills the control words of a plane of linear XRGB8888: the enable and
 * the format, on display 13 one arbiter slot (a 4-byte pixel); the
 * colour control with the plane gamma off (the format has no alpha).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_universal_plane.c
 * (skl_plane_ctl, adlp_plane_ctl_arb_slots, glk_plane_color_ctl).
 */
void
drv_i915_xd_plane_words(
	const struct i915_xd_platform *platform,
	struct i915_xd_plane *plane)
{
	/* The control word. */
	plane->ctl = I915_XD_PLANE_CTL_ENABLE | i915_xd_field_put(I915_XD_PLANE_CTL_FORMAT, I915_XD_PLANE_CTL_FORMAT_XRGB8888);
	if (platform->display_ver == 13U)
		plane->ctl |= i915_xd_field_put(I915_XD_PLANE_CTL_ARB_SLOTS, 1U);

	/* The colour control. */
	plane->color_ctl = I915_XD_PLANE_COLOR_GAMMA_OFF;
}

/*
 * Writes the words of the first plane that do not arm an update: the
 * stride in 64-byte units, the position, the size (less one), the
 * colour key (none) with the plane alpha, the offset (0), the auxiliary
 * distance (no auxiliary surface), the chroma upsampler (off) and the
 * colour control.  The pipe's watermarks follow (the caller's).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_universal_plane.c
 * (icl_plane_update_noarm, skl_plane_keymsk, skl_plane_keymax).
 */
void
drv_i915_xd_plane_noarm(
	struct i915_xd_io *io,
	const struct i915_xd_plane *plane)
{
	uint32_t keymsk;
	unsigned pipe;

	/* The stride, the position and the size. */
	pipe = plane->pipe;
	drv_i915_xd_io_step(io, "xd-pipe plane");
	drv_i915_xd_io_write(io, I915_XD_PLANE_STRIDE(pipe), i915_xd_field_put(I915_XD_PLANE_STRIDE_UNITS, plane->pitch / I915_XD_PLANE_PITCH_UNIT));
	drv_i915_xd_io_write(io, I915_XD_PLANE_POS(pipe), i915_xd_pipe_pair(plane->x, plane->y));
	drv_i915_xd_io_write(io, I915_XD_PLANE_SIZE(pipe), i915_xd_pipe_pair(plane->width - 1U, plane->height - 1U));

	/* No colour key; a translucent plane enables the constant alpha. */
	keymsk = 0U;
	if (plane->alpha < 0xffU)
		keymsk |= I915_XD_PLANE_KEYMSK_ALPHA;
	drv_i915_xd_io_write(io, I915_XD_PLANE_KEYVAL(pipe), 0U);
	drv_i915_xd_io_write(io, I915_XD_PLANE_KEYMSK(pipe), keymsk);
	drv_i915_xd_io_write(io, I915_XD_PLANE_KEYMAX(pipe), i915_xd_field_put(I915_XD_PLANE_KEYMAX_ALPHA, plane->alpha));

	/* The offset, the auxiliary distance, the upsampler and the colour control. */
	drv_i915_xd_io_write(io, I915_XD_PLANE_OFFSET(pipe), 0U);
	drv_i915_xd_io_write(io, I915_XD_PLANE_AUX_DIST(pipe), 0U);
	drv_i915_xd_io_write(io, I915_XD_PLANE_CUS_CTL(pipe), 0U);
	drv_i915_xd_io_write(io, I915_XD_PLANE_COLOR_CTL(pipe), plane->color_ctl);
}

/*
 * Arms an update of the first plane: the control word just before the
 * surface address, whose write arms the update (a disabled plane's
 * control word arms by itself, so the two go together).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_universal_plane.c
 * (icl_plane_update_arm).
 */
void
drv_i915_xd_plane_arm(
	struct i915_xd_io *io,
	const struct i915_xd_plane *plane)
{
	/* The control word, then the surface. */
	drv_i915_xd_io_write(io, I915_XD_PLANE_CTL(plane->pipe), plane->ctl);
	drv_i915_xd_io_write(io, I915_XD_PLANE_SURF(plane->pipe), plane->surf);
}

/*
 * Starts turning the first plane off: its chroma upsampler.  The pipe's
 * watermarks follow (the caller's), then drv_i915_xd_plane_off_arm().
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_universal_plane.c
 * (icl_plane_disable_arm).
 */
void
drv_i915_xd_plane_off_noarm(
	struct i915_xd_io *io,
	unsigned pipe)
{
	/* The upsampler. */
	drv_i915_xd_io_step(io, "xd-pipe plane off");
	drv_i915_xd_io_write(io, I915_XD_PLANE_CUS_CTL(pipe), 0U);
}

/* Turns the first plane off: a zero control word, armed by a zero surface. */
void
drv_i915_xd_plane_off_arm(
	struct i915_xd_io *io,
	unsigned pipe)
{
	/* The control word, then the surface. */
	drv_i915_xd_io_write(io, I915_XD_PLANE_CTL(pipe), 0U);
	drv_i915_xd_io_write(io, I915_XD_PLANE_SURF(pipe), 0U);
}

/* Reads the surface the first plane scans out now. */
uint32_t
drv_i915_xd_plane_live(
	struct i915_xd_io *io,
	unsigned pipe)
{
	uint32_t live;

	/* The live surface. */
	live = drv_i915_xd_io_read(io, I915_XD_PLANE_SURFLIVE(pipe));

	/* Succeeded: its address. */
	return live;
}

/*
 * Sets a display 9 pipe and its transcoder up for a mode, still off: the
 * pipe's source size and 8 bits a colour, a DisplayPort link's M/N values
 * (m_n NULL for HDMI), the progressive timing, no pixel multiplier, and
 * the transcoder's configuration cleared and pushed out.
 */
void
drv_i915_xd9_pipe_configure(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned pipe,
	const struct i915_display_mode *mode,
	const struct i915_xd_dp_m_n *m_n)
{
	struct i915_xd_timing timing;

	/* The pipe's source and depth. */
	drv_i915_xd_pipe_source(io, pipe, mode->hdisplay, mode->vdisplay);
	drv_i915_xd_io_write(io, I915_XD9_PIPE_MISC(pipe), I915_XD9_PIPE_MISC_BPC_8);

	/* A DisplayPort link's M/N values. */
	if (m_n != NULL)
		drv_i915_xd_pipe_m_n(io, pipe, m_n);

	/* The timing: the blanks from the end of the active area to the total. */
	timing.hdisplay = mode->hdisplay;
	timing.hblank_start = mode->hdisplay;
	timing.hblank_end = mode->htotal;
	timing.hsync_start = mode->hsync_start;
	timing.hsync_end = mode->hsync_end;
	timing.htotal = mode->htotal;
	timing.vdisplay = mode->vdisplay;
	timing.vblank_start = mode->vdisplay;
	timing.vblank_end = mode->vtotal;
	timing.vsync_start = mode->vsync_start;
	timing.vsync_end = mode->vsync_end;
	timing.vtotal = mode->vtotal;
	timing.interlaced = 0;
	drv_i915_xd_pipe_timing(io, platform, pipe, &timing);

	/* Succeeded: no multiplier, the configuration clear. */
	drv_i915_xd_io_write(io, I915_XD9_TRANS_MULT(pipe), 0U);
	drv_i915_xd_io_write(io, I915_XD9_TRANSCONF(pipe), 0U);
	drv_i915_xd_io_posting_read(io, I915_XD9_TRANSCONF(pipe));
}

/*
 * Writes a display 9 transcoder's DDI function: on, its DDI, 8 bits a
 * colour, the mode's sync polarities, and HDMI or single-stream
 * DisplayPort with its lanes.
 */
void
drv_i915_xd9_pipe_func(
	struct i915_xd_io *io,
	unsigned trans,
	unsigned ddi,
	int hdmi,
	unsigned lanes,
	const struct i915_display_mode *mode)
{
	uint32_t word;

	/* On, the DDI, 8 bits a colour. */
	word = I915_XD9_DDI_FUNC_ENABLE | (ddi << I915_XD9_DDI_FUNC_DDI_SHIFT) | I915_XD9_DDI_FUNC_BPC_8;

	/* The sync polarities. */
	if (mode->vsync_positive)
		word |= I915_XD9_DDI_FUNC_PVSYNC;
	if (mode->hsync_positive)
		word |= I915_XD9_DDI_FUNC_PHSYNC;

	/* HDMI, or DisplayPort with its lanes. */
	if (hdmi) {
		word |= I915_XD9_DDI_FUNC_MODE_HDMI << I915_XD9_DDI_FUNC_MODE_SHIFT;
	} else {
		word |= I915_XD9_DDI_FUNC_MODE_DP_SST << I915_XD9_DDI_FUNC_MODE_SHIFT;
		word |= (lanes - 1U) << I915_XD9_DDI_FUNC_WIDTH_SHIFT;
	}

	/* Succeeded: the word. */
	drv_i915_xd_io_write(io, I915_XD9_TRANS_DDI_FUNC(trans), word);
}

/*
 * Starts a display 9 transcoder and waits for it to run.  Returns 0, or
 * ETIMEDOUT.
 */
int
drv_i915_xd9_pipe_start(
	struct i915_xd_io *io,
	unsigned trans)
{
	int error;

	/* The enable, pushed out. */
	(void)drv_i915_xd_io_rmw(io, I915_XD9_TRANSCONF(trans), 0U, I915_XD9_TRANSCONF_ENABLE);
	drv_i915_xd_io_posting_read(io, I915_XD9_TRANSCONF(trans));

	/* The state. */
	error = drv_i915_xd_io_poll(io, I915_XD9_TRANSCONF(trans), I915_XD9_TRANSCONF_STATE, I915_XD9_TRANSCONF_STATE, I915_XD9_TRANS_WAIT_US, NULL);
	if (error != 0)
		return ETIMEDOUT;

	/* Succeeded: the transcoder runs. */
	return 0;
}

/*
 * Stops a display 9 transcoder, waits for it to stop, and turns its DDI
 * function off with its DDI.
 */
void
drv_i915_xd9_pipe_stop(
	struct i915_xd_io *io,
	unsigned trans)
{
	/* The enable cleared, and the state. */
	(void)drv_i915_xd_io_rmw(io, I915_XD9_TRANSCONF(trans), I915_XD9_TRANSCONF_ENABLE, 0U);
	(void)drv_i915_xd_io_poll(io, I915_XD9_TRANSCONF(trans), I915_XD9_TRANSCONF_STATE, 0U, I915_XD9_TRANS_WAIT_US, NULL);

	/* Succeeded: the function and its DDI off. */
	(void)drv_i915_xd_io_rmw(io, I915_XD9_TRANS_DDI_FUNC(trans), I915_XD9_DDI_FUNC_ENABLE | I915_XD9_DDI_FUNC_DDI_MASK, 0U);
}

/*
 * Shows a linear XRGB8888 surface on a display 9 pipe's first plane
 * (either byte order, the plane gamma off): the stride, the position, the
 * size and the offset, then the control, and the surface last, whose
 * write arms them for the next vertical blank.
 */
void
drv_i915_xd9_plane_on(
	struct i915_xd_io *io,
	unsigned pipe,
	uint32_t surf,
	uint32_t stride_units,
	uint32_t width,
	uint32_t height,
	int rgbx)
{
	uint32_t control;

	/* The control. */
	control = I915_XD9_PLANE_CTL_ENABLE | I915_XD9_PLANE_CTL_GAMMA_OFF | I915_XD9_PLANE_CTL_XRGB_8888;
	if (rgbx)
		control |= I915_XD9_PLANE_CTL_RGBX;

	/* The layout. */
	drv_i915_xd_io_write(io, I915_XD9_PLANE_STRIDE(pipe), stride_units);
	drv_i915_xd_io_write(io, I915_XD9_PLANE_POS(pipe), 0U);
	drv_i915_xd_io_write(io, I915_XD9_PLANE_SIZE(pipe), ((height - 1U) << I915_XD9_PLANE_SIZE_HEIGHT_SHIFT) | (width - 1U));
	drv_i915_xd_io_write(io, I915_XD9_PLANE_OFFSET(pipe), 0U);

	/* Succeeded: the control, then the surface that arms them. */
	drv_i915_xd_io_write(io, I915_XD9_PLANE_CTL(pipe), control);
	drv_i915_xd_io_write(io, I915_XD9_PLANE_SURF(pipe), surf);
	drv_i915_xd_io_posting_read(io, I915_XD9_PLANE_SURF(pipe));
}

/*
 * Turns a display 9 pipe's first plane off: the control, then the surface
 * that arms it.
 */
void
drv_i915_xd9_plane_off(
	struct i915_xd_io *io,
	unsigned pipe)
{
	/* Succeeded: the control and the surface. */
	drv_i915_xd_io_write(io, I915_XD9_PLANE_CTL(pipe), 0U);
	drv_i915_xd_io_write(io, I915_XD9_PLANE_SURF(pipe), 0U);
	drv_i915_xd_io_posting_read(io, I915_XD9_PLANE_SURF(pipe));
}

/*
 * Waits for the next vertical blank of a running display 9 pipe: its
 * frame counter moves.  Returns 0, or ETIMEDOUT when the pipe does not
 * run.
 */
int
drv_i915_xd9_pipe_wait_vblank(
	struct i915_xd_io *io,
	unsigned pipe)
{
	uint32_t first;
	uint32_t now;
	unsigned waited;

	/* The counter now, then again every millisecond. */
	first = drv_i915_xd_io_read(io, I915_XD9_PIPE_FRMCOUNT(pipe));
	for (waited = 0U; waited < I915_XD9_VBLANK_WAIT_MS; waited++) {
		(void)drv_i915_xd_io_delay(io, 1000U);
		now = drv_i915_xd_io_read(io, I915_XD9_PIPE_FRMCOUNT(pipe));
		if (now != first)
			return 0;
	}

	/* Succeeded in learning that the pipe does not run. */
	return ETIMEDOUT;
}

/*
 * Points a display 9 pipe's first plane at a surface: the stride first
 * when it differs from the plane's, then the surface, whose write arms
 * both for the next vertical blank.
 */
void
drv_i915_xd9_plane_flip(
	struct i915_xd_io *io,
	unsigned pipe,
	uint32_t surf,
	uint32_t stride)
{
	uint32_t current;

	/* The stride, only when it changes. */
	current = drv_i915_xd_io_read(io, I915_XD9_PLANE_STRIDE(pipe));
	if (current != stride)
		drv_i915_xd_io_write(io, I915_XD9_PLANE_STRIDE(pipe), stride);

	/* Succeeded: the surface, which arms them. */
	drv_i915_xd_io_write(io, I915_XD9_PLANE_SURF(pipe), surf);
	drv_i915_xd_io_posting_read(io, I915_XD9_PLANE_SURF(pipe));
}

/* Reports the surface a display 9 pipe's first plane scans out now. */
uint32_t
drv_i915_xd9_plane_surf_live(
	struct i915_xd_io *io,
	unsigned pipe)
{
	uint32_t live;

	/* The live surface's word. */
	live = drv_i915_xd_io_read(io, I915_XD9_PLANE_SURFLIVE(pipe));

	/* Succeeded: its address. */
	return live & I915_XD9_PLANE_SURF_ADDRESS;
}

/* Reports a fault of a display 9 pipe's first plane that is pending in the pipe's interrupts (0 for none). */
uint32_t
drv_i915_xd9_plane_fault(
	struct i915_xd_io *io,
	unsigned pipe)
{
	uint32_t iir;

	/* The pipe's interrupt identity. */
	iir = drv_i915_xd_io_read(io, I915_XD9_DE_PIPE_IIR(pipe));

	/* Succeeded: its fault bit. */
	return iir & I915_XD9_DE_PIPE_FAULT;
}

/*
 * Clears a display 9 pipe's pending flip done and plane fault, twice
 * since the identity register can hold two of each.
 */
void
drv_i915_xd9_flip_irq_clear(
	struct i915_xd_io *io,
	unsigned pipe)
{
	/* Succeeded: both writes. */
	drv_i915_xd_io_write(io, I915_XD9_DE_PIPE_IIR(pipe), I915_XD9_FLIP_IRQS);
	drv_i915_xd_io_write(io, I915_XD9_DE_PIPE_IIR(pipe), I915_XD9_FLIP_IRQS);
}

/* Enables, then unmasks, a display 9 pipe's flip done and plane fault; the pipe's other sources stay as they are. */
void
drv_i915_xd9_flip_irq_unmask(
	struct i915_xd_io *io,
	unsigned pipe)
{
	/* The enable, then the mask, pushed out. */
	(void)drv_i915_xd_io_rmw(io, I915_XD9_DE_PIPE_IER(pipe), 0U, I915_XD9_FLIP_IRQS);
	(void)drv_i915_xd_io_rmw(io, I915_XD9_DE_PIPE_IMR(pipe), I915_XD9_FLIP_IRQS, 0U);

	/* Succeeded: the writes reached the device. */
	drv_i915_xd_io_posting_read(io, I915_XD9_DE_PIPE_IMR(pipe));
}

/* Masks, then disables, a display 9 pipe's flip done and plane fault. */
void
drv_i915_xd9_flip_irq_mask(
	struct i915_xd_io *io,
	unsigned pipe)
{
	/* The mask, pushed out. */
	(void)drv_i915_xd_io_rmw(io, I915_XD9_DE_PIPE_IMR(pipe), 0U, I915_XD9_FLIP_IRQS);
	drv_i915_xd_io_posting_read(io, I915_XD9_DE_PIPE_IMR(pipe));

	/* Succeeded: the enable off. */
	(void)drv_i915_xd_io_rmw(io, I915_XD9_DE_PIPE_IER(pipe), I915_XD9_FLIP_IRQS, 0U);
}

/*
 * Acknowledges what a display 9 pipe's interrupts report when the master
 * control names the pipe, and reports it (0 for nothing).
 */
uint32_t
drv_i915_xd9_flip_irq_ack(
	struct i915_xd_io *io,
	unsigned pipe,
	uint32_t master_ctl)
{
	uint32_t iir;

	/* Not the pipe's. */
	if ((master_ctl & I915_XD9_MASTER_DE_PIPE(pipe)) == 0U)
		return 0U;

	/* Nothing reported. */
	iir = drv_i915_xd_io_read(io, I915_XD9_DE_PIPE_IIR(pipe));
	if (iir == 0U)
		return 0U;

	/* Succeeded: what was seen, acknowledged. */
	drv_i915_xd_io_write(io, I915_XD9_DE_PIPE_IIR(pipe), iir);
	return iir;
}

/* Packs two values into a register's lower and upper halves. */
static uint32_t
i915_xd_pipe_pair(
	uint32_t lower,
	uint32_t upper)
{
	/* Succeeded: the word. */
	return i915_xd_field_put(I915_XD_TIMING_LOWER, lower) | i915_xd_field_put(I915_XD_TIMING_UPPER, upper);
}

/*
 * Computes a variable refresh control word: the ignored maximum shift and
 * the flip line enable, with the guardband on display 13, or the full
 * pipeline lines and their override on display 12.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vrr.c
 * (trans_vrr_ctl).
 */
static uint32_t
i915_xd_pipe_vrr_word(
	const struct i915_xd_platform *platform,
	const struct i915_xd_vrr *vrr)
{
	uint32_t word;

	/* The bits both versions set. */
	word = I915_XD_VRR_IGNORE_MAX_SHIFT | I915_XD_VRR_FLIP_LINE;

	/* Display 13: the guardband. */
	if (platform->display_ver >= 13U)
		return word | i915_xd_field_put(I915_XD_VRR_GUARDBAND, vrr->guardband);

	/* Succeeded: display 12's full pipeline lines. */
	return word | i915_xd_field_put(I915_XD_VRR_PIPELINE_FULL, vrr->pipeline_full) | I915_XD_VRR_PIPELINE_FULL_OVERRIDE;
}

/* Tells whether a pipe's scan line moves across 5 ms.  Returns 1 or 0. */
static int
i915_xd_pipe_scanline_moves(
	struct i915_xd_io *io,
	unsigned pipe)
{
	uint32_t first;
	uint32_t second;

	/* Two reads 5 ms apart. */
	first = drv_i915_xd_io_read(io, I915_XD_PIPE_SCANLINE(pipe)) & I915_XD_PIPE_SCANLINE_LINE;
	(void)drv_i915_xd_io_delay(io, I915_XD_PIPE_SCANLINE_GAP_US);
	second = drv_i915_xd_io_read(io, I915_XD_PIPE_SCANLINE(pipe)) & I915_XD_PIPE_SCANLINE_LINE;

	/* The line stood still. */
	if (first == second)
		return 0;

	/* Succeeded: the line moved. */
	return 1;
}

/*
 * Reports a transcoder's chicken word: display 11's eDP transcoder has
 * its own, the others follow their index.
 */
static uint32_t
i915_xd_pipe_chicken(
	unsigned trans)
{
	/* The eDP transcoder's word lies apart. */
	if (trans == I915_XD_TRANS_EDP)
		return I915_XD_CHICKEN_TRANS_EDP;

	/* Succeeded: the word of the index. */
	return I915_XD_CHICKEN_TRANS(trans);
}
