/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transcoders and pipes of the display 12 and 13 code (xd-pipe.c).
 *
 * On these displays every transcoder belongs to the pipe of the same
 * index.  A transcoder makes the timing of a mode: its totals, blanks
 * and syncs, the M/N values of a DisplayPort stream, the variable
 * refresh words, the pixel multiplier, the frame start delay, and its
 * configuration word (interlace, enable).  On display 13 the vertical
 * blank start is not written as a timing but as the set context latency.
 * Its DDI function word ties it to a DDI with the output's mode (HDMI,
 * DVI, DisplayPort single or multi stream), colour depth, sync polarity
 * and lane count; a DisplayPort stream also carries its MSA word.
 *
 * The first plane of a pipe scans out a linear XRGB8888 buffer: its
 * words that do not arm an update (stride in 64-byte units, position,
 * size, the colour key and plane alpha, the offset, the auxiliary
 * distance, the chroma upsampler, the colour control) go first, the
 * pipe's watermarks after them (the caller's), then the control word and
 * the surface address, which arms the update at the next vertical blank.
 * The live surface tells which buffer the pipe scans.  Turning the plane
 * off writes the upsampler, then (after the watermarks) a zero control
 * word and surface.
 *
 * The pipe takes the planes' pixels: its source size, its miscellaneous
 * word (the colour depth of what it sends, dithering, YUV output, the
 * rounding), its chicken word, and the colour words that pass pixels
 * through (a black bottom colour, the gamma and CSC modes).
 *
 * Starting a transcoder enables it once the planes are off (on display
 * 13 the pipe arbiter takes the programmed slots first); stopping it
 * clears the enable, releases a DisplayPort FEC stall, and waits up to
 * 100 ms for the transcoder to report itself off.  The pipe's frame
 * counter and scan line tell whether it runs.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_PIPE_H
#define DRIVERS_GPU_I915_DISPLAY_XD_PIPE_H

#include <stdint.h>

#include "xd-dp.h"
#include "xd-io.h"
#include "xd-platform.h"

/* The modes of a transcoder's DDI function. */
#define I915_XD_FUNC_HDMI		0U
#define I915_XD_FUNC_DVI		1U
#define I915_XD_FUNC_DP_SST		2U
#define I915_XD_FUNC_DP_MST		3U

/* The pixel formats a pipe sends. */
#define I915_XD_OUTPUT_RGB		0U
#define I915_XD_OUTPUT_YCBCR444		1U
#define I915_XD_OUTPUT_YCBCR420		2U

/* No transcoder (a port sync or multi-stream master that is not there). */
#define I915_XD_NO_TRANSCODER		0xffU

/*
 * The one pixel format and layout the planes scan out: XRGB8888
 * ('XR24'), linear.
 */
#define I915_XD_FOURCC_XRGB8888		0x34325258U
#define I915_XD_MODIFIER_LINEAR		0ULL

/* The waits of a transcoder that ran out, as bits. */
#define I915_XD_PIPE_LATE_OFF		0x1U
#define I915_XD_PIPE_LATE_SCANLINE	0x2U

/*
 * A mode's timings as a transcoder takes them, in pixels and lines
 * counted from the start of the active area: the active size, the blank
 * start and end, the sync start and end, the total of each direction.
 */
struct i915_xd_timing {
	uint32_t hdisplay;
	uint32_t hblank_start;
	uint32_t hblank_end;
	uint32_t hsync_start;
	uint32_t hsync_end;
	uint32_t htotal;
	uint32_t vdisplay;
	uint32_t vblank_start;
	uint32_t vblank_end;
	uint32_t vsync_start;
	uint32_t vsync_end;
	uint32_t vtotal;
	int interlaced;
};

/*
 * The variable refresh of a transcoder: the lowest and highest vertical
 * totals, the flip line (0: the refresh is fixed), and the lines of
 * guardband (display 13) or of a full pipeline (display 12).
 */
struct i915_xd_vrr {
	uint32_t vmin;
	uint32_t vmax;
	uint32_t flipline;
	uint32_t guardband;
	uint32_t pipeline_full;
};

/*
 * What a transcoder is set up with: its index, the mode's timings, the
 * M/N values of a DisplayPort stream (dp nonzero), the variable refresh,
 * the pixel multiplier and the frame start delay (both at least 1), and
 * whether it runs already (a change without a modeset keeps it on).
 */
struct i915_xd_trans_setup {
	unsigned trans;
	struct i915_xd_timing timing;
	int dp;
	struct i915_xd_dp_m_n m_n;
	struct i915_xd_vrr vrr;
	unsigned pixel_multiplier;
	unsigned frame_start_delay;
	int running;
};

/*
 * A transcoder's DDI function: the DDI, the mode, the bits per pixel,
 * the sync polarities, the lanes (DisplayPort), HDMI scrambling and the
 * high TMDS character rate, the multi-stream master transcoder, and the
 * port sync master transcoder (I915_XD_NO_TRANSCODER: none).
 */
struct i915_xd_trans_func {
	unsigned ddi;
	unsigned mode;
	unsigned bpp;
	int vsync_high;
	int hsync_high;
	unsigned lanes;
	int scrambling;
	int high_tmds;
	unsigned mst_master;
	unsigned sync_master;
};

/*
 * A DisplayPort stream's MSA word: the bits per pixel, limited range RGB
 * (signalled as CEA RGB), YCbCr 4:4:4, and whether the colour format is
 * in the VSC SDP instead.
 */
struct i915_xd_msa {
	unsigned bpp;
	int cea_rgb;
	int ycbcr444;
	int vsc_sdp;
};

/*
 * A pipe's miscellaneous word: the bits per pixel it sends, dithering,
 * the pixel format, and HDR precision (only HDR planes are active).
 */
struct i915_xd_pipe_misc {
	unsigned bpp;
	int dither;
	unsigned output;
	int hdr;
};

/*
 * The first plane of a pipe: the source size and pitch of its buffer,
 * its position on the pipe, the buffer's GGTT address, the plane alpha
 * (0xff opaque), and the control words drv_i915_xd_plane_words() fills.
 */
struct i915_xd_plane {
	unsigned pipe;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t x;
	uint32_t y;
	uint32_t surf;
	uint8_t alpha;
	uint32_t ctl;
	uint32_t color_ctl;
};

/*
 * The lines an update that must land in one frame stays away from: from
 * min to max, the lines just before the vertical blank's start.
 */
struct i915_xd_evade {
	int min;
	int max;
	int vblank_start;
};

/*
 * The scan line counter's offset from the real line: one behind, two
 * behind on an HDMI output.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vblank.c
 * (intel_crtc_scanline_offset).
 */
#define I915_XD_SCANLINE_OFFSET		1U
#define I915_XD_SCANLINE_OFFSET_HDMI	2U

void drv_i915_xd9_pipe_configure(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned pipe, const struct i915_display_mode *mode, const struct i915_xd_dp_m_n *m_n);
void drv_i915_xd9_pipe_func(struct i915_xd_io *io, unsigned trans, unsigned ddi, int hdmi, unsigned lanes, const struct i915_display_mode *mode);
int drv_i915_xd9_pipe_start(struct i915_xd_io *io, unsigned trans);
void drv_i915_xd9_pipe_stop(struct i915_xd_io *io, unsigned trans);
void drv_i915_xd9_plane_on(struct i915_xd_io *io, unsigned pipe, uint32_t surf, uint32_t stride_units, uint32_t width, uint32_t height, int rgbx);
void drv_i915_xd9_plane_off(struct i915_xd_io *io, unsigned pipe);
int drv_i915_xd9_pipe_wait_vblank(struct i915_xd_io *io, unsigned pipe);
void drv_i915_xd9_plane_flip(struct i915_xd_io *io, unsigned pipe, uint32_t surf, uint32_t stride);
uint32_t drv_i915_xd9_plane_surf_live(struct i915_xd_io *io, unsigned pipe);
uint32_t drv_i915_xd9_plane_fault(struct i915_xd_io *io, unsigned pipe);
void drv_i915_xd9_flip_irq_clear(struct i915_xd_io *io, unsigned pipe);
void drv_i915_xd9_flip_irq_unmask(struct i915_xd_io *io, unsigned pipe);
void drv_i915_xd9_flip_irq_mask(struct i915_xd_io *io, unsigned pipe);
uint32_t drv_i915_xd9_flip_irq_ack(struct i915_xd_io *io, unsigned pipe, uint32_t master_ctl);
void drv_i915_xd_pipe_m_n(struct i915_xd_io *io, unsigned trans, const struct i915_xd_dp_m_n *m_n);
void drv_i915_xd_pipe_timing(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned trans, const struct i915_xd_timing *timing);
void drv_i915_xd_pipe_configure(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_trans_setup *setup);
void drv_i915_xd_pipe_source(struct i915_xd_io *io, unsigned pipe, uint32_t width, uint32_t height);
void drv_i915_xd_pipe_misc(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned pipe, const struct i915_xd_pipe_misc *misc);
void drv_i915_xd_pipe_chicken(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned pipe);
void drv_i915_xd_pipe_colors(struct i915_xd_io *io, unsigned pipe, uint32_t gamma_mode, uint32_t csc_mode);
void drv_i915_xd_pipe_msa(struct i915_xd_io *io, unsigned trans, const struct i915_xd_msa *msa);
uint32_t drv_i915_xd_pipe_func_word(const struct i915_xd_trans_func *func);
void drv_i915_xd_pipe_func_enable(struct i915_xd_io *io, unsigned trans, const struct i915_xd_trans_func *func);
void drv_i915_xd_pipe_func_config(struct i915_xd_io *io, unsigned trans, const struct i915_xd_trans_func *func);
uint32_t drv_i915_xd_pipe_func_word_11(unsigned trans, unsigned pipe, const struct i915_xd_trans_func *func);
void drv_i915_xd_pipe_func_enable_11(struct i915_xd_io *io, unsigned trans, unsigned pipe, const struct i915_xd_trans_func *func);
void drv_i915_xd_pipe_clock_on(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned trans, unsigned ddi);
void drv_i915_xd_pipe_clock_off(struct i915_xd_io *io, unsigned trans);
uint32_t drv_i915_xd_pipe_func_disable(struct i915_xd_io *io, unsigned trans, int mst_master);
int drv_i915_xd_pipe_start(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned trans);
unsigned drv_i915_xd_pipe_stop(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned trans, int *was_running);
void drv_i915_xd_pipe_scalers_off(struct i915_xd_io *io, unsigned pipe);
uint32_t drv_i915_xd_pipe_frame(struct i915_xd_io *io, unsigned pipe);
unsigned drv_i915_xd_pipe_wait_scanline(struct i915_xd_io *io, unsigned pipe, int moving);
unsigned drv_i915_xd_pipe_scanline(struct i915_xd_io *io, unsigned pipe, unsigned vtotal, unsigned offset);
void drv_i915_xd_pipe_evade_window(uint32_t clock_khz, uint32_t htotal, uint32_t vblank_start, struct i915_xd_evade *evade);
int drv_i915_xd_plane_check(uint32_t fourcc, uint64_t modifier, uint32_t width, uint32_t height, uint32_t pitch, uint32_t surf);
void drv_i915_xd_plane_words(const struct i915_xd_platform *platform, struct i915_xd_plane *plane);
void drv_i915_xd_plane_noarm(struct i915_xd_io *io, const struct i915_xd_plane *plane);
void drv_i915_xd_plane_arm(struct i915_xd_io *io, const struct i915_xd_plane *plane);
void drv_i915_xd_plane_off_noarm(struct i915_xd_io *io, unsigned pipe);
void drv_i915_xd_plane_off_arm(struct i915_xd_io *io, unsigned pipe);
uint32_t drv_i915_xd_plane_live(struct i915_xd_io *io, unsigned pipe);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_PIPE_H */
