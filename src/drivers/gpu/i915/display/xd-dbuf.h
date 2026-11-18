/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display buffer and the watermarks of the display 12 and 13 code
 * (xd-dbuf.c).
 *
 * The display buffer (DBUF) is split into slices; the platform's table
 * tells which slices each pipe of a set of active pipes uses, and on
 * display 13 whether one lone pipe joins the memory bus to all four.  A
 * pipe's share of its slices goes by the width of its mode against the
 * other pipes on the same slices; within it the cursor takes a fixed
 * part at the end, every plane its smallest allocation for the deepest
 * watermark level that fits, and what is left goes by the planes' data
 * rates.
 *
 * A plane's watermark of a level is how many blocks (and lines) the
 * plane must have buffered to ride out the memory latency of that level:
 * the larger of the bytes the plane fetches in the latency (method 1)
 * and the lines it crosses in it (method 2), as the memory latency, the
 * pixel rate and the line time decide.  The transition watermark, the
 * SAGV watermark (level 0 with the SAGV block time added) and its
 * transition follow from level 0.  Levels whose latency does not fit the
 * vertical blank, and levels the allocation cannot hold, are turned off;
 * a level turned off keeps the blocks and lines of the one below it.
 *
 * The writes: each plane's watermark words and its buffer range, the
 * slices' power (each one requested, then checked after 10 us), the
 * memory bus's joining and the slices' tracker service, and each pipe's
 * data box credits.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_DBUF_H
#define DRIVERS_GPU_I915_DISPLAY_XD_DBUF_H

#include <stdint.h>

#include "xd-io.h"
#include "xd-platform.h"

/* The most watermark levels a platform has. */
#define I915_XD_WM_LEVELS		8U

/* A minimal allocation that marks a level no allocation can hold. */
#define I915_XD_WM_TOO_BIG		0xffffU

/* The kinds of surface layout the watermarks know. */
#define I915_XD_WM_LINEAR		0U
#define I915_XD_WM_X_TILED		1U
#define I915_XD_WM_Y_TILED		2U

/*
 * One watermark level of a plane: whether it is on, whether the lines
 * are ignored, the blocks and lines it holds, and the smallest buffer
 * allocation that serves it.
 */
struct i915_xd_wm_level {
	uint8_t enable;
	uint8_t ignore_lines;
	uint16_t blocks;
	uint16_t lines;
	uint16_t min_ddb;
};

/*
 * The watermarks of one plane: each level, the transition, and the SAGV
 * level 0 and its transition.
 */
struct i915_xd_plane_wm {
	struct i915_xd_wm_level level[I915_XD_WM_LEVELS];
	struct i915_xd_wm_level trans;
	struct i915_xd_wm_level sagv;
	struct i915_xd_wm_level sagv_trans;
};

/*
 * What a pipe's watermarks are computed from: the platform's levels, the
 * memory latency of each level (us), whether isochronous priority
 * control is on, the SAGV block time (us, 0 for none), the pipe's pixel
 * rate (kHz) and line total.
 */
struct i915_xd_wm_pipe {
	unsigned levels;
	uint16_t latency_us[I915_XD_WM_LEVELS];
	int ipc;
	unsigned sagv_block_us;
	uint32_t pixel_rate_khz;
	uint32_t htotal;
};

/*
 * One plane's surface as the watermarks see it: its width in pixels,
 * its bytes per pixel, its layout, its pixel rate (kHz, after
 * downscaling), and whether it takes asynchronous flips with the minimal
 * level 0 only (display 13).
 */
struct i915_xd_wm_surface {
	uint32_t width;
	unsigned cpp;
	unsigned layout;
	uint32_t pixel_rate_khz;
	int minimal;
};

/* A range of display buffer blocks, end excluded (0..0: none). */
struct i915_xd_ddb {
	uint16_t start;
	uint16_t end;
};

/*
 * One display 9 pipe in a plan of the display buffer: what the plan takes
 * (whether the pipe is lit, its mode's width, total and pixel clock, and
 * its plane's width), and what it gives (whether the plan reached the
 * pipe, its share of the slice, the plane's and the cursor's parts, the
 * plane's levels and the line time in eighths of a microsecond).
 */
struct i915_xd9_pipe_plan {
	int active;
	uint32_t hdisplay;
	uint32_t htotal;
	uint32_t pixel_khz;
	uint32_t plane_width;
	int computed;
	struct i915_xd_ddb pipe_ddb;
	struct i915_xd_ddb plane_ddb;
	struct i915_xd_ddb cursor_ddb;
	struct i915_xd_plane_wm wm;
	uint32_t linetime;
};

void drv_i915_xd9_wm_write_pipe(struct i915_xd_io *io, unsigned pipe, const struct i915_xd9_pipe_plan *plan);
void drv_i915_xd9_wm_arm_pipe(struct i915_xd_io *io, unsigned pipe, const struct i915_xd9_pipe_plan *plan);
void drv_i915_xd9_wm_arm_pipes(struct i915_xd_io *io, const struct i915_xd9_pipe_plan *plans, unsigned count, int ascending, unsigned except);
int drv_i915_xd9_dbuf_plan(const struct i915_xd_platform *platform, const uint16_t *latency, struct i915_xd9_pipe_plan *pipes, unsigned count);
uint8_t drv_i915_xd_dbuf_joined(const struct i915_xd_platform *platform, uint8_t pipes);
uint8_t drv_i915_xd_dbuf_slices(const struct i915_xd_platform *platform, unsigned pipe, uint8_t pipes, int joined);
uint8_t drv_i915_xd_dbuf_enabled(const uint8_t *pipe_slices, unsigned pipes);
/*
 * What the memory's firmware reports for the watermarks: each level's
 * latency (us, 0 for a level that is off), how many levels the display
 * has (a level that is off counts), whether the latencies were read, the SAGV block time (us, 0
 * for none) and the mailbox's error of the block time (0 for none).
 */
struct i915_xd_memory {
	uint16_t latency[8];
	unsigned levels;
	int valid;
	uint32_t sagv_block_us;
	int sagv_error;
};

int drv_i915_xd_memory_read(struct i915_xd_io *io, const struct i915_xd_platform *platform, int sagv_controlled, int dimm_adjust, struct i915_xd_memory *memory);
void drv_i915_xd_dbuf_slice_range(const struct i915_xd_platform *platform, uint8_t slices, struct i915_xd_ddb *range);
uint16_t drv_i915_xd_dbuf_mbus_offset(const struct i915_xd_platform *platform, uint8_t slices);
void drv_i915_xd_dbuf_pipe_range(const struct i915_xd_platform *platform, uint8_t slices, unsigned weight_start, unsigned weight_end, unsigned weight_total, struct i915_xd_ddb *range);
void drv_i915_xd_wm_plane(const struct i915_xd_platform *platform, const struct i915_xd_wm_pipe *pipe, const struct i915_xd_wm_surface *surface, struct i915_xd_plane_wm *wm);
uint16_t drv_i915_xd_wm_cursor_blocks(const struct i915_xd_platform *platform, const struct i915_xd_wm_pipe *pipe, unsigned active_pipes);
int drv_i915_xd_wm_vblank_level(const struct i915_xd_wm_pipe *pipe, uint32_t clock_khz, uint32_t vblank_lines, unsigned frame_start_delay, unsigned wm0_lines, unsigned *level, int *sagv_too_short);
void drv_i915_xd_wm_cut(struct i915_xd_plane_wm *wm, unsigned levels, unsigned deepest, int sagv_too_short);
int drv_i915_xd_wm_allocate(const struct i915_xd_wm_pipe *pipe, const struct i915_xd_ddb *range, uint16_t cursor_blocks, struct i915_xd_plane_wm *wm, uint64_t data_rate, struct i915_xd_ddb *plane_ddb, struct i915_xd_ddb *cursor_ddb);
void drv_i915_xd_wm_write_plane(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned pipe, unsigned levels, const struct i915_xd_plane_wm *wm, int use_sagv, const struct i915_xd_ddb *ddb);
uint8_t drv_i915_xd_dbuf_power(struct i915_xd_io *io, const struct i915_xd_platform *platform, uint8_t slices);
void drv_i915_xd_dbuf_mbus(struct i915_xd_io *io, const struct i915_xd_platform *platform, int joined);
uint32_t drv_i915_xd_dbuf_dbox_word(const struct i915_xd_platform *platform, int joined);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_DBUF_H */
