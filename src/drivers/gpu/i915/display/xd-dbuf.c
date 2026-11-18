/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display buffer and the watermarks of the display 12 and 13 code
 * (see xd-dbuf.h).
 */

#include "xd-dbuf.h"
#include "xd-io.h"
#include "xd-pipe.h"
#include "xd-platform.h"

#include "../intel/display-regs-xd.h"
#include "../intel/display-regs-xd9.h"

#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The block of the display buffer the watermarks count in, the lines a
 * Y-tiled surface is fetched in, and the most lines a watermark holds on
 * display 12 and on display 13.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_compute_wm_params, skl_wm_max_lines).
 */
#define I915_XD_WM_BLOCK_BYTES		512U
#define I915_XD_WM_Y_LINES		4U
#define I915_XD_WM_MAX_LINES_12		31U
#define I915_XD_WM_MAX_LINES_13		255U

/*
 * The transition watermark: the fewest blocks on display 11 and later,
 * and the configurable amount above them; the cursor's fixed allocation
 * with one active pipe and with more, and the cursor's surface (256
 * pixels of 4 bytes).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_compute_transition_wm, skl_cursor_allocation).
 */
#define I915_XD_WM_TRANS_MIN		4U
#define I915_XD_WM_TRANS_AMOUNT		10U
#define I915_XD_WM_CURSOR_ONE_PIPE	32U
#define I915_XD_WM_CURSOR_PIPES		8U
#define I915_XD_WM_CURSOR_WIDTH		256U
#define I915_XD_WM_CURSOR_CPP		4U

/*
 * How long a slice's power takes to follow its request; the tracker
 * service ratio of the memory bus clock against the CD clock.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c (gen9_dbuf_slice_set),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (update_mbus_pre_enable, intel_dbuf_mdclk_cdclk_ratio_update).
 */
#define I915_XD_DBUF_POWER_US		10U
#define I915_XD_DBUF_MDCLK_RATIO	2U

/* A 16.16 fixed point value whose integer part is 1, and the largest one. */
#define I915_XD_FIXED_ONE		0x10000U
#define I915_XD_FIXED_MAX		0xffffffffU

/*
 * The parameters of a surface's watermarks: whether its layout is Y
 * tiled, its pixel rate and bytes per pixel, its bytes and blocks (16.16)
 * a line, the blocks of its Y tile minimum (16.16), and the pipe's line
 * time rounded up (us).
 */
/*
 * The memory's watermark latencies: the levels a display of version 13
 * has (the deeper two go to the SAGV watermark), the levels of an older
 * one, the read latency added to every level when level 0 reads as 0
 * (us, display 12 on and older), and the mailbox's wait (us).
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/skl_watermark.c (skl_read_wm_latency,
 * adjust_wm_latency); drivers/gpu/drm/i915/display/intel_display_device.h
 * (HAS_HW_SAGV_WM).
 */
#define I915_XD_MEMORY_LEVELS_13	6U
#define I915_XD_MEMORY_LEVELS		8U
#define I915_XD_MEMORY_READ_LATENCY_12	3U
#define I915_XD_MEMORY_READ_LATENCY	2U
#define I915_XD_MEMORY_MAILBOX_US	20500U

/*
 * The SAGV block time of a display that does not read it (us): display
 * 11's, and the older ones'; and the largest the watermarks can add.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/skl_watermark.c (intel_sagv_block_time,
 * intel_sagv_init).
 */
#define I915_XD_SAGV_BLOCK_US_11	10U
#define I915_XD_SAGV_BLOCK_US_OLD	30U
#define I915_XD_SAGV_BLOCK_US_MAX	0xffffU

struct i915_xd_wm_params {
	int y_tiled;
	uint32_t pixel_rate;
	unsigned cpp;
	uint32_t bytes_per_line;
	uint32_t blocks_per_line;
	uint32_t y_tile_minimum;
	uint32_t linetime_us;
};

/*
 * Display 9's watermarks: the most lines a level holds, and the largest
 * line time WM_LINETIME holds (in eighths of a microsecond).
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (PLANE_WM, WM_LINETIME).
 */
#define I915_XD9_WM_MAX_LINES		31U
#define I915_XD9_LINETIME_MAX		0x1ffU

/* The slice every display 9 pipe's share of the display buffer is in. */
#define I915_XD9_DBUF_SLICE		0x1U

static uint32_t i915_xd_fixed_div(uint32_t value, uint32_t divisor);
static void i915_xd_memory_levels(uint32_t word, uint16_t *latency);
static uint32_t i915_xd_fixed_mul(uint32_t value, uint32_t fixed);
static uint32_t i915_xd_fixed_round_up(uint32_t fixed);
static uint32_t i915_xd_div_round_up(uint64_t numerator, uint64_t denominator);
static const struct i915_xd_dbuf_rule *i915_xd_dbuf_rule(const struct i915_xd_platform *platform, uint8_t pipes, int joined, int match_joined);
static void i915_xd_wm_params(const struct i915_xd_wm_pipe *pipe, const struct i915_xd_wm_surface *surface, struct i915_xd_wm_params *params);
static void i915_xd_wm_level(const struct i915_xd_platform *platform, const struct i915_xd_wm_pipe *pipe, const struct i915_xd_wm_params *params, unsigned latency, int minimal_skip, struct i915_xd_wm_level *level);
static void i915_xd_wm_transition(const struct i915_xd_wm_pipe *pipe, const struct i915_xd_wm_params *params, const struct i915_xd_wm_level *wm0, struct i915_xd_wm_level *trans);
static void i915_xd_wm_fit(struct i915_xd_wm_level *level, const struct i915_xd_ddb *ddb);
static void i915_xd_wm_write_level(struct i915_xd_io *io, uint32_t offset, const struct i915_xd_wm_level *level);
static void i915_xd_ddb_write(struct i915_xd_io *io, uint32_t offset, const struct i915_xd_ddb *ddb);
static void i915_xd9_wm_level(const struct i915_xd_wm_pipe *pipe, const struct i915_xd_wm_params *params, unsigned latency, int above_zero, uint16_t floor_blocks, struct i915_xd_wm_level *level);
static void i915_xd9_wm_levels(const struct i915_xd_wm_pipe *pipe, const struct i915_xd_wm_params *params, struct i915_xd_wm_level *levels);
static int i915_xd9_dbuf_plan_pipe(const struct i915_xd_platform *platform, const uint16_t *latency, unsigned active, unsigned weight_before, unsigned weight_total, struct i915_xd9_pipe_plan *plan);

/*
 * Tells whether a set of active pipes joins the memory bus to its one
 * pipe: the first row of the platform's table for the set says.
 * Returns 1 or 0.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (adlp_check_mbus_joined, check_mbus_joined).
 */
uint8_t
drv_i915_xd_dbuf_joined(
	const struct i915_xd_platform *platform,
	uint8_t pipes)
{
	const struct i915_xd_dbuf_rule *rule;

	/* Display 12 never joins the bus. */
	if (platform->display_ver < 13U)
		return 0U;

	/* The first row of the set. */
	rule = i915_xd_dbuf_rule(platform, pipes, 0, 0);
	if (rule == NULL)
		return 0U;

	/* Succeeded: the row's answer. */
	return rule->joined;
}

/*
 * Tells which slices a pipe uses within a set of active pipes, with the
 * bus joined or not (0 when the table has no such row).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_compute_dbuf_slices, compute_dbuf_slices).
 */
uint8_t
drv_i915_xd_dbuf_slices(
	const struct i915_xd_platform *platform,
	unsigned pipe,
	uint8_t pipes,
	int joined)
{
	const struct i915_xd_dbuf_rule *rule;

	/* The row of the set and the joining. */
	rule = i915_xd_dbuf_rule(platform, pipes, joined, 1);
	if (rule == NULL || pipe >= 4U)
		return 0U;

	/* Succeeded: the pipe's slices. */
	return rule->slices[pipe];
}

/*
 * Tells which slices are to be powered for the pipes' slices: slice 1
 * always (the display's start turns it on and it stays), and every slice
 * a pipe uses.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (intel_dbuf_enabled_slices).
 */
uint8_t
drv_i915_xd_dbuf_enabled(
	const uint8_t *pipe_slices,
	unsigned pipes)
{
	uint8_t slices;
	unsigned pipe;

	/* Slice 1, and each pipe's. */
	slices = 0x1U;
	for (pipe = 0U; pipe < pipes; pipe++)
		slices |= pipe_slices[pipe];

	/* Succeeded: the slices. */
	return slices;
}

/*
 * Finds the blocks a set of slices spans: from the start of its lowest
 * slice to the end of its highest (0..0 for no slice).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_ddb_entry_for_slices, intel_dbuf_slice_size).
 */
void
drv_i915_xd_dbuf_slice_range(
	const struct i915_xd_platform *platform,
	uint8_t slices,
	struct i915_xd_ddb *range)
{
	unsigned size;
	unsigned lowest;
	unsigned highest;

	/* No slice spans nothing. */
	range->start = 0U;
	range->end = 0U;
	if (slices == 0U)
		return;

	/* The size of one slice, the lowest and the highest slice. */
	size = platform->dbuf_blocks / platform->dbuf_slices;
	lowest = (unsigned)__builtin_ctz(slices);
	highest = 31U - (unsigned)__builtin_clz(slices);

	/* The span. */
	range->start = (uint16_t)(lowest * size);
	range->end = (uint16_t)((highest + 1U) * size);
}

/*
 * Tells where the allocations of a set of slices count from: the start of
 * slice 1 for slices 1 and 2, of slice 3 for slices 3 and 4 (each half of
 * the buffer numbers its blocks from its own start).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (mbus_ddb_offset).
 */
uint16_t
drv_i915_xd_dbuf_mbus_offset(
	const struct i915_xd_platform *platform,
	uint8_t slices)
{
	struct i915_xd_ddb range;
	uint8_t first;

	/* The first slice of the half. */
	first = slices;
	if ((slices & 0x3U) != 0U) {
		first = 0x1U;
	} else if ((slices & 0xcU) != 0U) {
		first = 0x4U;
	}

	/* Succeeded: where the half starts. */
	drv_i915_xd_dbuf_slice_range(platform, first, &range);
	return range.start;
}

/*
 * Gives a pipe its share of its slices: the span of the slices, divided
 * by the weights of the pipes before it and of itself against all pipes
 * on the same slices, counted from the start of the slices' half.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_crtc_allocate_ddb, intel_crtc_dbuf_weights).
 */
void
drv_i915_xd_dbuf_pipe_range(
	const struct i915_xd_platform *platform,
	uint8_t slices,
	unsigned weight_start,
	unsigned weight_end,
	unsigned weight_total,
	struct i915_xd_ddb *range)
{
	struct i915_xd_ddb span;
	uint16_t offset;
	uint32_t size;
	uint32_t start;
	uint32_t end;

	/* A pipe without weight has no share. */
	range->start = 0U;
	range->end = 0U;
	if (weight_end == weight_start || weight_total == 0U)
		return;

	/* The span of the slices and the half's start. */
	drv_i915_xd_dbuf_slice_range(platform, slices, &span);
	offset = drv_i915_xd_dbuf_mbus_offset(platform, slices);
	size = (uint32_t)(span.end - span.start);

	/* The share by weight. */
	start = size * weight_start / weight_total;
	end = size * weight_end / weight_total;
	range->start = (uint16_t)(span.start - offset + start);
	range->end = (uint16_t)(span.start - offset + end);
}

/*
 * Computes a plane's watermarks: each level of the platform from its
 * latency, the transition watermark (with isochronous priority control),
 * and on display 12 and 13 the SAGV level 0 (the block time added to
 * level 0's latency) and its transition.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_build_plane_wm_single, skl_compute_wm_levels, tgl_compute_sagv_wm).
 */
void
drv_i915_xd_wm_plane(
	const struct i915_xd_platform *platform,
	const struct i915_xd_wm_pipe *pipe,
	const struct i915_xd_wm_surface *surface,
	struct i915_xd_plane_wm *wm)
{
	struct i915_xd_wm_params params;
	unsigned index;
	unsigned latency;
	uint8_t *bytes;

	/* Starts from no watermark. */
	bytes = (uint8_t *)wm;
	for (index = 0U; index < sizeof(*wm); index++)
		bytes[index] = 0U;

	/* The levels. */
	i915_xd_wm_params(pipe, surface, &params);

	/* Display 9 has its own levels, and no transition or SAGV watermark. */
	if (platform->display_ver < 11U) {
		i915_xd9_wm_levels(pipe, &params, wm->level);
		return;
	}

	/* The levels of display 11 to 13. */
	for (index = 0U; index < pipe->levels && index < I915_XD_WM_LEVELS; index++)
		i915_xd_wm_level(platform, pipe, &params, pipe->latency_us[index], surface->minimal && index > 0U, &wm->level[index]);

	/* The transition. */
	i915_xd_wm_transition(pipe, &params, &wm->level[0], &wm->trans);

	/* The SAGV level 0 and its transition. */
	latency = 0U;
	if (pipe->sagv_block_us != 0U)
		latency = pipe->sagv_block_us + pipe->latency_us[0];
	i915_xd_wm_level(platform, pipe, &params, latency, 0, &wm->sagv);
	i915_xd_wm_transition(pipe, &params, &wm->sagv, &wm->sagv_trans);
}

/*
 * Computes the cursor's allocation: the smallest allocation of the
 * deepest level a 256-pixel cursor can have, but at least 32 blocks with
 * one active pipe and 8 with more.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_cursor_allocation).
 */
uint16_t
drv_i915_xd_wm_cursor_blocks(
	const struct i915_xd_platform *platform,
	const struct i915_xd_wm_pipe *pipe,
	unsigned active_pipes)
{
	struct i915_xd_wm_surface cursor;
	struct i915_xd_wm_params params;
	struct i915_xd_wm_level level;
	struct i915_xd_wm_level levels[I915_XD_WM_LEVELS];
	unsigned index;
	uint16_t needed;
	uint16_t fixed;

	/* The cursor's surface at the pipe's pixel rate. */
	cursor.width = I915_XD_WM_CURSOR_WIDTH;
	cursor.cpp = I915_XD_WM_CURSOR_CPP;
	cursor.layout = I915_XD_WM_LINEAR;
	cursor.pixel_rate_khz = pipe->pixel_rate_khz;
	cursor.minimal = 0;
	i915_xd_wm_params(pipe, &cursor, &params);

	/* Starts with no level. */
	needed = 0U;
	level.enable = 0U;
	level.ignore_lines = 0U;
	level.blocks = 0U;
	level.lines = 0U;
	level.min_ddb = 0U;
	for (index = 0U; index < I915_XD_WM_LEVELS; index++)
		levels[index] = level;

	/* The deepest level the cursor can have. */
	if (platform->display_ver < 11U) {
		/* Display 9's levels, each no smaller than the one below. */
		i915_xd9_wm_levels(pipe, &params, levels);
		for (index = 0U; index < pipe->levels && index < I915_XD_WM_LEVELS; index++) {
			if (levels[index].min_ddb == I915_XD_WM_TOO_BIG)
				break;
			needed = levels[index].min_ddb;
		}
	} else {
		/* The levels of display 11 to 13, one at a time. */
		for (index = 0U; index < pipe->levels && index < I915_XD_WM_LEVELS; index++) {
			i915_xd_wm_level(platform, pipe, &params, pipe->latency_us[index], 0, &level);
			if (level.min_ddb == I915_XD_WM_TOO_BIG)
				break;
			needed = level.min_ddb;
		}
	}

	/* The fixed part. */
	fixed = I915_XD_WM_CURSOR_PIPES;
	if (active_pipes == 1U)
		fixed = I915_XD_WM_CURSOR_ONE_PIPE;

	/* Succeeded: the larger of the two. */
	if (needed > fixed)
		return needed;
	return fixed;
}

/*
 * Finds the deepest watermark level whose latency fits the vertical
 * blank: the frame start delay, the latency's lines and the lines of
 * level 0 must not exceed the blank (level 0 counts no latency; a level
 * without latency is skipped).  Also tells whether the SAGV block time
 * does not fit.  Returns 0 with the level, or EINVAL when none fits.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_wm_check_vblank, skl_max_wm_level_for_vblank, skl_is_vblank_too_short).
 */
int
drv_i915_xd_wm_vblank_level(
	const struct i915_xd_wm_pipe *pipe,
	uint32_t clock_khz,
	uint32_t vblank_lines,
	unsigned frame_start_delay,
	unsigned wm0_lines,
	unsigned *level,
	int *sagv_too_short)
{
	unsigned index;
	uint32_t latency;
	uint32_t lines;

	/* The SAGV block time against the blank. */
	*sagv_too_short = 0;
	if (pipe->sagv_block_us != 0U && pipe->htotal != 0U) {
		lines = i915_xd_div_round_up((uint64_t)pipe->sagv_block_us * clock_khz, 1000ULL * pipe->htotal);
		if (frame_start_delay + lines + wm0_lines > vblank_lines)
			*sagv_too_short = 1;
	}

	/* The deepest level that fits. */
	for (index = pipe->levels; index > 0U; index--) {
		latency = pipe->latency_us[index - 1U];
		if (latency == 0U)
			continue;
		if (index == 1U)
			latency = 0U;
		lines = 1U;
		if (pipe->htotal != 0U)
			lines = i915_xd_div_round_up((uint64_t)latency * clock_khz, 1000ULL * pipe->htotal);
		if (frame_start_delay + lines + wm0_lines > vblank_lines)
			continue;

		/* Succeeded: the level. */
		*level = index - 1U;
		return 0;
	}

	/* No level fits. */
	return EINVAL;
}

/*
 * Turns off the levels deeper than the one the vertical blank allows
 * (they keep their blocks, and their allocation still counts), and the
 * SAGV watermarks when the SAGV block time does not fit the blank.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_wm_check_vblank).
 */
void
drv_i915_xd_wm_cut(
	struct i915_xd_plane_wm *wm,
	unsigned levels,
	unsigned deepest,
	int sagv_too_short)
{
	unsigned level;

	/* The deeper levels. */
	for (level = deepest + 1U; level < levels && level < I915_XD_WM_LEVELS; level++)
		wm->level[level].enable = 0U;

	/* The SAGV watermarks. */
	if (sagv_too_short) {
		wm->sagv.enable = 0U;
		wm->sagv_trans.enable = 0U;
	}
}

/*
 * Hands a pipe's share out: the cursor's part at the end, then the
 * plane's smallest allocation for the deepest level that fits (and the
 * rest by data rate), the levels the allocation cannot hold turned off
 * and given the blocks and lines of the level below, and the transition
 * and SAGV watermarks that do not fit turned off.  Returns 0, or EINVAL
 * when not even level 0 fits.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_crtc_allocate_plane_ddb, skl_allocate_plane_ddb, skl_check_wm_level,
 * skl_need_wm_copy_wa).
 */
int
drv_i915_xd_wm_allocate(
	const struct i915_xd_wm_pipe *pipe,
	const struct i915_xd_ddb *range,
	uint16_t cursor_blocks,
	struct i915_xd_plane_wm *wm,
	uint64_t data_rate,
	struct i915_xd_ddb *plane_ddb,
	struct i915_xd_ddb *cursor_ddb)
{
	uint32_t size;
	uint32_t extra;
	uint32_t blocks;
	unsigned level;
	unsigned found;
	int fits;

	/* Nothing to hand out. */
	plane_ddb->start = 0U;
	plane_ddb->end = 0U;
	cursor_ddb->start = 0U;
	cursor_ddb->end = 0U;
	size = (uint32_t)(range->end - range->start);
	if (size == 0U)
		return 0;

	/* The cursor's part at the end. */
	size -= cursor_blocks;
	cursor_ddb->start = (uint16_t)(range->end - cursor_blocks);
	cursor_ddb->end = range->end;

	/* The deepest level whose smallest allocation fits. */
	fits = 0;
	found = 0U;
	for (level = pipe->levels; level > 0U; level--) {
		blocks = wm->level[level - 1U].min_ddb;
		if (blocks <= size) {
			size -= blocks;
			found = level - 1U;
			fits = 1;
			break;
		}
	}

	/* Refuses a plane whose level never fits. */
	if (!fits)
		return EINVAL;

	/* The rest by data rate (all of it to the one plane). */
	if (data_rate == 0U)
		size = 0U;
	extra = 0U;
	if (data_rate != 0U) {
		extra = i915_xd_div_round_up((uint64_t)size * data_rate, data_rate);
		if (extra > size)
			extra = size;
	}

	/* The level's blocks with the plane's share of what is left. */
	blocks = wm->level[found].min_ddb + extra;
	if (blocks != 0U) {
		plane_ddb->start = range->start;
		plane_ddb->end = (uint16_t)(range->start + blocks);
	}

	/* The deeper levels the allocation cannot hold, given the level below. */
	for (level = found + 1U; level < pipe->levels && level < I915_XD_WM_LEVELS; level++) {
		i915_xd_wm_fit(&wm->level[level], plane_ddb);
		if (!wm->level[level].enable) {
			wm->level[level].blocks = wm->level[level - 1U].blocks;
			wm->level[level].lines = wm->level[level - 1U].lines;
			wm->level[level].ignore_lines = wm->level[level - 1U].ignore_lines;
		}
	}

	/* The transition and the SAGV watermarks. */
	i915_xd_wm_fit(&wm->trans, plane_ddb);
	i915_xd_wm_fit(&wm->sagv, plane_ddb);
	i915_xd_wm_fit(&wm->sagv_trans, plane_ddb);

	/* Succeeded: the allocation is handed out. */
	return 0;
}

/*
 * Writes a plane's watermarks and its allocation: each level (level 0
 * from the SAGV watermark when the pipe uses it), the transition, on
 * display 13 the hardware SAGV level and its transition, then the
 * plane's buffer range.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_write_plane_wm, skl_write_wm_level, skl_ddb_entry_write).
 */
void
drv_i915_xd_wm_write_plane(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned pipe,
	unsigned levels,
	const struct i915_xd_plane_wm *wm,
	int use_sagv,
	const struct i915_xd_ddb *ddb)
{
	const struct i915_xd_wm_level *level;
	unsigned index;

	/* Each level. */
	for (index = 0U; index < levels && index < I915_XD_WM_LEVELS; index++) {
		level = &wm->level[index];
		if (index == 0U && use_sagv)
			level = &wm->sagv;
		i915_xd_wm_write_level(io, I915_XD_PLANE_WM(pipe, index), level);
	}

	/* The transition. */
	level = &wm->trans;
	if (use_sagv)
		level = &wm->sagv_trans;
	i915_xd_wm_write_level(io, I915_XD_PLANE_WM_TRANS(pipe), level);

	/* The hardware SAGV levels. */
	if (platform->hw_sagv_wm) {
		i915_xd_wm_write_level(io, I915_XD_PLANE_WM_SAGV(pipe), &wm->sagv);
		i915_xd_wm_write_level(io, I915_XD_PLANE_WM_SAGV_TRANS(pipe), &wm->sagv_trans);
	}

	/* The buffer range. */
	i915_xd_ddb_write(io, I915_XD_PLANE_BUF_CFG(pipe), ddb);
}

/*
 * Writes a display 9 pipe's first plane's eight levels, its transition
 * (unused, 0) and its ranges, the cursor's levels (it stays off) and its
 * range, and the line time.  A plan of zeroes frees the pipe.  The
 * planes' surface writes arm them.
 */
void
drv_i915_xd9_wm_write_pipe(
	struct i915_xd_io *io,
	unsigned pipe,
	const struct i915_xd9_pipe_plan *plan)
{
	struct i915_xd_wm_level off;
	unsigned index;

	/* The plane's levels, its transition, its range and no NV12 range. */
	kern_memset(&off, 0, sizeof(off));
	for (index = 0U; index < I915_XD_WM_LEVELS; index++)
		i915_xd_wm_write_level(io, I915_XD_PLANE_WM(pipe, index), &plan->wm.level[index]);
	i915_xd_wm_write_level(io, I915_XD_PLANE_WM_TRANS(pipe), &off);
	i915_xd_ddb_write(io, I915_XD_PLANE_BUF_CFG(pipe), &plan->plane_ddb);
	drv_i915_xd_io_write(io, I915_XD9_PLANE_NV12_BUF_CFG(pipe), 0U);

	/* The cursor's levels, off, and its range. */
	for (index = 0U; index < I915_XD_WM_LEVELS; index++)
		i915_xd_wm_write_level(io, I915_XD_CUR_WM(pipe, index), &off);
	i915_xd_wm_write_level(io, I915_XD9_CUR_WM_TRANS(pipe), &off);
	i915_xd_ddb_write(io, I915_XD_CUR_BUF_CFG(pipe), &plan->cursor_ddb);

	/* Succeeded: the line time. */
	drv_i915_xd_io_write(io, I915_XD_WM_LINETIME(pipe), plan->linetime & I915_XD9_WM_LINETIME_MASK);
}

/*
 * Moves a lit display 9 pipe to its plan and waits for the vertical
 * blank that takes it: the first plane's and the cursor's watermarks and
 * ranges, planes 2 and 3 (an off one gives its range up), and every
 * plane's and the cursor's surface written again with its own value.
 */
void
drv_i915_xd9_wm_arm_pipe(
	struct i915_xd_io *io,
	unsigned pipe,
	const struct i915_xd9_pipe_plan *plan)
{
	uint32_t word;
	unsigned plane;

	/* The first plane's and the cursor's part. */
	drv_i915_xd9_wm_write_pipe(io, pipe, plan);

	/* Planes 2 and 3. */
	for (plane = 1U; plane <= 2U; plane++) {
		word = drv_i915_xd_io_read(io, I915_XD9_PLANE_N_CTL(pipe, plane));
		if ((word & I915_XD9_PLANE_CTL_ENABLE) == 0U)
			drv_i915_xd_io_write(io, I915_XD9_PLANE_N_BUF_CFG(pipe, plane), 0U);
		word = drv_i915_xd_io_read(io, I915_XD9_PLANE_N_SURF(pipe, plane));
		drv_i915_xd_io_write(io, I915_XD9_PLANE_N_SURF(pipe, plane), word);
	}

	/* The cursor, then the first plane. */
	word = drv_i915_xd_io_read(io, I915_XD9_CURBASE(pipe));
	drv_i915_xd_io_write(io, I915_XD9_CURBASE(pipe), word);
	word = drv_i915_xd_io_read(io, I915_XD9_PLANE_SURF(pipe));
	drv_i915_xd_io_write(io, I915_XD9_PLANE_SURF(pipe), word);

	/* Succeeded once the vertical blank has taken them. */
	(void)drv_i915_xd9_pipe_wait_vblank(io, pipe);
}

/*
 * Moves every lit display 9 pipe but one (except) to its plan, one
 * vertical blank each, in ascending order of pipes when the ranges
 * shrink and descending when they grow, so that no two lit pipes' ranges
 * overlap between the steps.
 */
void
drv_i915_xd9_wm_arm_pipes(
	struct i915_xd_io *io,
	const struct i915_xd9_pipe_plan *plans,
	unsigned count,
	int ascending,
	unsigned except)
{
	unsigned step;
	unsigned pipe;

	/* Each pipe in the order. */
	for (step = 0U; step < count; step++) {
		pipe = step;
		if (!ascending)
			pipe = count - 1U - step;
		if (pipe == except || !plans[pipe].active)
			continue;
		drv_i915_xd9_wm_arm_pipe(io, pipe, &plans[pipe]);
	}
}

/*
 * Powers the platform's slices as asked: each slice's request set or
 * cleared, the write flushed, 10 us, and its state read back.  Returns
 * the slices whose state did not follow.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (gen9_dbuf_slices_update, gen9_dbuf_slice_set).
 */
uint8_t
drv_i915_xd_dbuf_power(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	uint8_t slices)
{
	static const uint32_t controls[4] = {
		I915_XD_DBUF_CTL_S1, I915_XD_DBUF_CTL_S2, I915_XD_DBUF_CTL_S3, I915_XD_DBUF_CTL_S4
	};
	uint32_t request;
	uint32_t state;
	uint8_t late;
	unsigned slice;
	int wanted;

	/* Each slice the platform has. */
	drv_i915_xd_io_step(io, "xd-dbuf power");
	late = 0U;
	for (slice = 0U; slice < platform->dbuf_slices && slice < 4U; slice++) {
		wanted = (slices & (1U << slice)) != 0U;
		request = 0U;
		if (wanted)
			request = I915_XD_DBUF_POWER_REQUEST;
		drv_i915_xd_io_rmw(io, controls[slice], I915_XD_DBUF_POWER_REQUEST, request);
		drv_i915_xd_io_posting_read(io, controls[slice]);
		(void)drv_i915_xd_io_delay(io, I915_XD_DBUF_POWER_US);

		/* The state follows the request. */
		state = drv_i915_xd_io_read(io, controls[slice]) & I915_XD_DBUF_POWER_STATE;
		if ((state != 0U) != (wanted != 0))
			late |= (uint8_t)(1U << slice);
	}

	/* Succeeded: the slices that did not follow. */
	return late;
}

/*
 * Joins the memory bus to one pipe, or splits it: on display 13 the bus
 * control's hashing mode and join (no pipe selected), then each slice's
 * tracker service from the memory bus clock ratio (doubled when joined).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (update_mbus_pre_enable, intel_dbuf_mdclk_cdclk_ratio_update).
 */
void
drv_i915_xd_dbuf_mbus(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	int joined)
{
	static const uint32_t controls[4] = {
		I915_XD_DBUF_CTL_S1, I915_XD_DBUF_CTL_S2, I915_XD_DBUF_CTL_S3, I915_XD_DBUF_CTL_S4
	};
	uint32_t word;
	unsigned ratio;
	unsigned slice;
	uint32_t service;

	/* Display 12 has no joining. */
	if (platform->display_ver < 13U)
		return;

	/* The hashing mode and the join. */
	drv_i915_xd_io_step(io, "xd-dbuf mbus");
	word = i915_xd_field_put(I915_XD_MBUS_CTL_JOIN_PIPE, I915_XD_MBUS_CTL_JOIN_PIPE_NONE);
	if (joined)
		word |= I915_XD_MBUS_CTL_HASH_1X4 | I915_XD_MBUS_CTL_JOIN;
	drv_i915_xd_io_rmw(io, I915_XD_MBUS_CTL, I915_XD_MBUS_CTL_HASH_1X4 | I915_XD_MBUS_CTL_JOIN | I915_XD_MBUS_CTL_JOIN_PIPE, word);

	/* Each slice's tracker service. */
	ratio = I915_XD_DBUF_MDCLK_RATIO;
	if (joined)
		ratio *= 2U;
	service = i915_xd_field_put(I915_XD_DBUF_MIN_TRACKER_STATE_SERVICE, ratio - 1U);
	for (slice = 0U; slice < platform->dbuf_slices && slice < 4U; slice++)
		drv_i915_xd_io_rmw(io, controls[slice], I915_XD_DBUF_MIN_TRACKER_STATE_SERVICE, service);
}

/*
 * Computes a pipe's data box credits: the A credit (joined or not), the
 * BW and B credits, and the regulated back-to-back transactions.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (intel_mbus_dbox_update).
 */
uint32_t
drv_i915_xd_dbuf_dbox_word(
	const struct i915_xd_platform *platform,
	int joined)
{
	const struct i915_xd_dbox_credit *credit;
	uint32_t word;
	uint32_t a;

	/* The regulated back-to-back transactions (display 12 on; Ice Lake has a maximum of 0 and none). */
	credit = platform->dbox;
	word = 0U;
	if (credit->b2b_max != 0U) {
		word = i915_xd_field_put(I915_XD_DBOX_B2B_MAX, credit->b2b_max) |
		       i915_xd_field_put(I915_XD_DBOX_B2B_DELAY, credit->b2b_delay) |
		       I915_XD_DBOX_B2B_REGULATE;
	}

	/* The A credit, the BW and the B credits. */
	a = credit->a_split;
	if (joined)
		a = credit->a_joined;
	word |= i915_xd_field_put(I915_XD_DBOX_A_CREDIT, a);
	word |= i915_xd_field_put(I915_XD_DBOX_BW_CREDIT, credit->bw);
	word |= i915_xd_field_put(I915_XD_DBOX_B_CREDIT, credit->b);

	/* Succeeded: the word. */
	return word;
}

/*
 * Reads what the memory's firmware reports for the watermarks: the
 * latency of each level and the SAGV block time.
 *
 * The SAGV block time is read first, then the latencies, which come
 * from the mailbox in two words of four levels.  A
 * level that reads 0 us turns itself and every deeper level off; when
 * level 0 reads 0 us the read latency is added to every level left on;
 * dimm_adjust adds 1 us to level 0 (the 16 GB DIMM workaround).  The
 * SAGV block time is read from the mailbox on display 12 on; it is 0
 * when the display does not control the SAGV (sagv_controlled zero) or
 * the mailbox reports more than the watermarks can add.  Returns 0, or
 * the mailbox's error of the latencies (memory->valid stays 0; the block
 * time is still read).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/skl_watermark.c (skl_read_wm_latency,
 * adjust_wm_latency, intel_sagv_block_time, intel_sagv_init).
 */
int
drv_i915_xd_memory_read(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	int sagv_controlled,
	int dimm_adjust,
	struct i915_xd_memory *memory)
{
	uint32_t data0;
	uint32_t data1;
	unsigned read_latency;
	unsigned level;
	unsigned deeper;
	unsigned on;
	int latency_error;
	int error;

	/* Starts with nothing read. */
	kern_memset(memory, 0, sizeof(*memory));

	/* The levels of the display. */
	memory->levels = I915_XD_MEMORY_LEVELS;
	if (platform->display_ver >= 13U)
		memory->levels = I915_XD_MEMORY_LEVELS_13;

	/* The SAGV block time: read on display 12 on, fixed before. */
	memory->sagv_block_us = I915_XD_SAGV_BLOCK_US_OLD;
	if (platform->display_ver == 11U)
		memory->sagv_block_us = I915_XD_SAGV_BLOCK_US_11;

	/* Display 12 and later read the latencies from the firmware's mailbox. */
	if (platform->display_ver >= 12U) {
		data0 = 0U;
		data1 = 0U;
		error = drv_i915_xd_io_mailbox(io, I915_XD_MAILBOX_SAGV_BLOCK_TIME, &data0, &data1, I915_XD_MEMORY_MAILBOX_US);
		memory->sagv_block_us = data0;
		if (error != 0) {
			memory->sagv_block_us = 0U;
			memory->sagv_error = error;
		}
	}

	/* A block time the watermarks cannot add blocks nothing. */
	if (memory->sagv_block_us > I915_XD_SAGV_BLOCK_US_MAX)
		memory->sagv_block_us = 0U;

	/* Nor does an SAGV the display does not control. */
	if (!sagv_controlled)
		memory->sagv_block_us = 0U;

	/* Levels 0 to 3, then 4 to 7, one byte each. */
	data0 = 0U;
	data1 = 0U;
	latency_error = drv_i915_xd_io_mailbox(io, I915_XD_MAILBOX_READ_LATENCY, &data0, &data1, I915_XD_MEMORY_MAILBOX_US);
	if (latency_error == 0) {
		i915_xd_memory_levels(data0, &memory->latency[0]);
		data0 = 1U;
		data1 = 0U;
		latency_error = drv_i915_xd_io_mailbox(io, I915_XD_MAILBOX_READ_LATENCY, &data0, &data1, I915_XD_MEMORY_MAILBOX_US);
	}

	/* The latencies were read whole. */
	if (latency_error == 0) {
		i915_xd_memory_levels(data0, &memory->latency[4]);
		memory->valid = 1;
	}

	/* A level that reads 0 us turns itself and every deeper level of the display off. */
	on = memory->levels;
	for (level = 1U; memory->valid && level < memory->levels; level++) {
		if (memory->latency[level] != 0U)
			continue;

		/* A level with no latency disables it and every deeper one. */
		for (deeper = level; deeper < memory->levels; deeper++)
			memory->latency[deeper] = 0U;
		on = level;
		break;
	}

	/* Level 0 that reads 0 us: the read latency is added to every level left on. */
	read_latency = I915_XD_MEMORY_READ_LATENCY;
	if (platform->display_ver >= 12U)
		read_latency = I915_XD_MEMORY_READ_LATENCY_12;

	/* Adds the read latency to every enabled level when level 0 reads zero. */
	if (memory->valid && memory->latency[0] == 0U) {
		for (level = 0U; level < on; level++)
			memory->latency[level] = (uint16_t)(memory->latency[level] + read_latency);
	}

	/* The 16 GB DIMM workaround. */
	if (memory->valid && dimm_adjust)
		memory->latency[0] = (uint16_t)(memory->latency[0] + 1U);

	/* The latencies could not be read. */
	if (latency_error != 0)
		return latency_error;

	/* Succeeded: the latencies and the block time are read. */
	return 0;
}

/*
 * Plans the display buffer and the watermarks of display 9's lit pipes,
 * each with one linear 32-bit plane and its cursor (which stays off).
 *
 * The one slice is shared by the lit pipes in the proportion of their
 * widths.  At the end of each pipe's share the cursor keeps what its
 * deepest level needs (at least 32 blocks alone, 8 beside other pipes);
 * the plane takes the rest at the deepest level both it and the cursor
 * fit at, and loses every deeper level its share cannot hold.  The line
 * time is set beside them.  Returns 0, or ENOSPC when a pipe's share
 * cannot hold its cursor, or its plane at level 0; the pipes planned up
 * to that point keep what they were given, and their computed field is
 * set.
 *
 * Procedure referenced: Intel Kaby Lake PRM Volume 12, Display Engine
 * (the display buffer allocation and the watermark calculation).
 */
int
drv_i915_xd9_dbuf_plan(
	const struct i915_xd_platform *platform,
	const uint16_t *latency,
	struct i915_xd9_pipe_plan *pipes,
	unsigned count)
{
	unsigned weight_total;
	unsigned weight_before;
	unsigned active;
	unsigned pipe;
	int error;

	/* The lit pipes and the sum of their widths. */
	weight_total = 0U;
	active = 0U;
	for (pipe = 0U; pipe < count; pipe++) {
		if (!pipes[pipe].active)
			continue;
		weight_total += pipes[pipe].hdisplay;
		active++;
	}

	/* Each lit pipe in order, its share starting where the last one's ended. */
	weight_before = 0U;
	for (pipe = 0U; pipe < count; pipe++) {
		if (!pipes[pipe].active)
			continue;

		/* The pipe's share, its watermarks and its line time. */
		error = i915_xd9_dbuf_plan_pipe(platform, latency, active, weight_before, weight_total, &pipes[pipe]);
		if (error != 0)
			return error;
		weight_before += pipes[pipe].hdisplay;
	}

	/* Succeeded: every lit pipe fits. */
	return 0;
}

/* Divides into 16.16 fixed point, rounding up. */
static uint32_t
i915_xd_fixed_div(
	uint32_t value,
	uint32_t divisor)
{
	/* Succeeded: the quotient. */
	return (uint32_t)i915_xd_div_round_up((uint64_t)value << 16, divisor);
}

/* Multiplies a whole number by a 16.16 value, keeping 16.16 (cut to 32 bits). */
static uint32_t
i915_xd_fixed_mul(
	uint32_t value,
	uint32_t fixed)
{
	/* Succeeded: the product. */
	return (uint32_t)((uint64_t)value * fixed);
}

/* Rounds a 16.16 value up to a whole number. */
static uint32_t
i915_xd_fixed_round_up(
	uint32_t fixed)
{
	/* Succeeded: the whole number. */
	return (uint32_t)i915_xd_div_round_up(fixed, I915_XD_FIXED_ONE);
}

/* Divides, rounding up. */
static uint32_t
i915_xd_div_round_up(
	uint64_t numerator,
	uint64_t denominator)
{
	/* Succeeded: the quotient. */
	return (uint32_t)((numerator + denominator - 1U) / denominator);
}

/*
 * Finds the first row of the platform's table for a set of active pipes
 * (with the joining given when match_joined is nonzero).
 */
static const struct i915_xd_dbuf_rule *
i915_xd_dbuf_rule(
	const struct i915_xd_platform *platform,
	uint8_t pipes,
	int joined,
	int match_joined)
{
	unsigned index;

	/* Looks the set up. */
	for (index = 0U; index < platform->ndbuf; index++) {
		if (platform->dbuf[index].pipes != pipes)
			continue;
		if (match_joined && (platform->dbuf[index].joined != 0U) != (joined != 0))
			continue;
		return &platform->dbuf[index];
	}

	/* Succeeded: the table has no such row. */
	return NULL;
}

/*
 * Fills the parameters of a surface's watermarks: blocks of 512 bytes,
 * the bytes a line, the blocks a line (16.16: the bytes a line in blocks
 * plus one; a Y-tiled surface by its four-line fetch), the Y tile minimum
 * (four lines), and the pipe's line time rounded up.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_compute_wm_params, intel_get_linetime_us).
 */
static void
i915_xd_wm_params(
	const struct i915_xd_wm_pipe *pipe,
	const struct i915_xd_wm_surface *surface,
	struct i915_xd_wm_params *params)
{
	uint32_t blocks;
	uint32_t linetime;

	/* The surface. */
	params->y_tiled = surface->layout == I915_XD_WM_Y_TILED;
	params->pixel_rate = surface->pixel_rate_khz;
	params->cpp = surface->cpp;
	params->bytes_per_line = surface->width * surface->cpp;

	/* The blocks a line. */
	if (params->y_tiled) {
		blocks = i915_xd_div_round_up((uint64_t)params->bytes_per_line * I915_XD_WM_Y_LINES, I915_XD_WM_BLOCK_BYTES) + 1U;
		params->blocks_per_line = i915_xd_fixed_div(blocks, I915_XD_WM_Y_LINES);
	} else {
		blocks = i915_xd_div_round_up(params->bytes_per_line, I915_XD_WM_BLOCK_BYTES) + 1U;
		params->blocks_per_line = blocks << 16;
	}

	/* The fewest blocks a Y-tiled plane needs. */
	params->y_tile_minimum = i915_xd_fixed_mul(I915_XD_WM_Y_LINES, params->blocks_per_line);

	/* The pipe's line time. */
	linetime = 0U;
	if (pipe->pixel_rate_khz != 0U)
		linetime = i915_xd_fixed_div(pipe->htotal * 1000U, pipe->pixel_rate_khz);
	params->linetime_us = i915_xd_fixed_round_up(linetime);
}

/*
 * Computes one watermark level from its latency: method 1 (the bytes the
 * plane fetches in the latency, in blocks, plus one) or method 2 (the
 * lines it crosses in the latency, in blocks a line), the blocks (plus
 * one, at least a line's), the lines, and the smallest allocation (a
 * tenth more than the blocks, or whole Y fetches).  A latency of 0, a
 * level skipped for the minimal level 0, or more lines than a watermark
 * holds mark the level as too big for any allocation.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_compute_plane_wm, skl_wm_method1, skl_wm_method2).
 */
static void
i915_xd_wm_level(
	const struct i915_xd_platform *platform,
	const struct i915_xd_wm_pipe *pipe,
	const struct i915_xd_wm_params *params,
	unsigned latency,
	int minimal_skip,
	struct i915_xd_wm_level *level)
{
	uint32_t method1;
	uint32_t method2;
	uint32_t selected;
	uint32_t blocks;
	uint32_t line_blocks;
	uint32_t lines;
	uint32_t extra;
	uint32_t min_ddb;
	uint32_t max_lines;
	uint32_t crossed;

	/* No latency, or a level the minimal level 0 skips. */
	if (latency == 0U || minimal_skip) {
		level->min_ddb = I915_XD_WM_TOO_BIG;
		return;
	}

	/* Method 1, plus one block. */
	method1 = i915_xd_fixed_div(latency * params->pixel_rate * params->cpp, 1000U * I915_XD_WM_BLOCK_BYTES);
	method1 += I915_XD_FIXED_ONE;

	/* Method 2. */
	crossed = i915_xd_div_round_up(latency * params->pixel_rate, pipe->htotal * 1000U);
	method2 = i915_xd_fixed_mul(crossed, params->blocks_per_line);

	/* The method of the layout and of the line time. */
	if (params->y_tiled) {
		selected = method2;
		if (params->y_tile_minimum > selected)
			selected = params->y_tile_minimum;
	} else if (params->cpp * pipe->htotal / I915_XD_WM_BLOCK_BYTES < 1U &&
		   params->bytes_per_line / I915_XD_WM_BLOCK_BYTES < 1U) {
		selected = method2;
	} else if (latency >= params->linetime_us) {
		selected = method2;
	} else {
		selected = method1;
	}

	/* The blocks, at least a line's, and the lines. */
	blocks = i915_xd_fixed_round_up(selected) + 1U;
	line_blocks = i915_xd_fixed_round_up(params->blocks_per_line);
	if (line_blocks > blocks)
		blocks = line_blocks;
	lines = i915_xd_div_round_up(selected, params->blocks_per_line);

	/* The smallest allocation: whole Y fetches, or a tenth more than the blocks. */
	if (params->y_tiled) {
		extra = I915_XD_WM_Y_LINES * 2U - lines % I915_XD_WM_Y_LINES;
		if (lines % I915_XD_WM_Y_LINES == 0U)
			extra = I915_XD_WM_Y_LINES;
		min_ddb = i915_xd_div_round_up((uint64_t)(lines + extra) * params->blocks_per_line, I915_XD_FIXED_ONE);
	} else {
		min_ddb = blocks + i915_xd_div_round_up(blocks, 10U);
	}

	/* More lines than a watermark holds. */
	max_lines = I915_XD_WM_MAX_LINES_12;
	if (platform->display_ver >= 13U)
		max_lines = I915_XD_WM_MAX_LINES_13;
	if (lines > max_lines) {
		level->min_ddb = I915_XD_WM_TOO_BIG;
		return;
	}

	/* Succeeded: the level. */
	if (blocks > min_ddb)
		min_ddb = blocks;
	level->blocks = (uint16_t)blocks;
	level->lines = (uint16_t)lines;
	level->min_ddb = (uint16_t)(min_ddb + 1U);
	level->enable = 1U;
}

/*
 * Computes a transition watermark from its level 0, when isochronous
 * priority control is on: level 0's blocks less one (at least two Y
 * fetches' worth on a Y-tiled surface) plus 4 and the configured 10,
 * plus one.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_compute_transition_wm).
 */
static void
i915_xd_wm_transition(
	const struct i915_xd_wm_pipe *pipe,
	const struct i915_xd_wm_params *params,
	const struct i915_xd_wm_level *wm0,
	struct i915_xd_wm_level *trans)
{
	uint16_t wm0_blocks;
	uint16_t y_minimum;
	uint16_t blocks;

	/* Without the priority control there is no transition watermark. */
	if (!pipe->ipc)
		return;

	/* Level 0's blocks less one, at least two Y fetches. */
	wm0_blocks = (uint16_t)(wm0->blocks - 1U);
	blocks = wm0_blocks;
	if (params->y_tiled) {
		y_minimum = (uint16_t)i915_xd_div_round_up((uint64_t)2U * params->y_tile_minimum, I915_XD_FIXED_ONE);
		if (y_minimum > blocks)
			blocks = y_minimum;
	}

	/* The transition level adds its minimum and its amount to the blocks. */
	blocks = (uint16_t)(blocks + I915_XD_WM_TRANS_MIN + I915_XD_WM_TRANS_AMOUNT + 1U);

	/* Succeeded: the transition. */
	trans->blocks = blocks;
	trans->min_ddb = wm0->min_ddb;
	if ((uint16_t)(blocks + 1U) > trans->min_ddb)
		trans->min_ddb = (uint16_t)(blocks + 1U);
	trans->enable = 1U;
}

/* Turns a watermark off when the allocation cannot hold it. */
static void
i915_xd_wm_fit(
	struct i915_xd_wm_level *level,
	const struct i915_xd_ddb *ddb)
{
	/* A watermark the allocation holds stays. */
	if (level->min_ddb <= (uint16_t)(ddb->end - ddb->start))
		return;

	/* The watermark goes. */
	level->enable = 0U;
	level->ignore_lines = 0U;
	level->blocks = 0U;
	level->lines = 0U;
	level->min_ddb = 0U;
}

/* Writes one watermark word: the enable, the ignore lines bit, the lines and the blocks. */
static void
i915_xd_wm_write_level(
	struct i915_xd_io *io,
	uint32_t offset,
	const struct i915_xd_wm_level *level)
{
	uint32_t word;

	/* The word. */
	word = i915_xd_field_put(I915_XD_WM_BLOCKS, level->blocks) | i915_xd_field_put(I915_XD_WM_LINES, level->lines);
	if (level->enable)
		word |= I915_XD_WM_ENABLE;
	if (level->ignore_lines)
		word |= I915_XD_WM_IGNORE_LINES;
	drv_i915_xd_io_write(io, offset, word);
}

/* Writes a buffer range word: the last block and the first, or 0 for none. */
static void
i915_xd_ddb_write(
	struct i915_xd_io *io,
	uint32_t offset,
	const struct i915_xd_ddb *ddb)
{
	uint32_t word;

	/* The word. */
	word = 0U;
	if (ddb->end != 0U)
		word = i915_xd_field_put(I915_XD_BUF_CFG_END, ddb->end - 1U) | i915_xd_field_put(I915_XD_BUF_CFG_START, ddb->start);
	drv_i915_xd_io_write(io, offset, word);
}

/* Unpacks four levels' latencies (us), one byte each from the lowest, out of a mailbox word. */
static void
i915_xd_memory_levels(
	uint32_t word,
	uint16_t *latency)
{
	unsigned index;

	/* Each byte, level by level. */
	for (index = 0U; index < 4U; index++)
		latency[index] = (uint16_t)((word >> (I915_XD_MAILBOX_LATENCY_LEVEL_BITS * index)) & I915_XD_MAILBOX_LATENCY_LEVEL_MASK);
}

/*
 * Computes one display 9 watermark level from its latency.
 *
 * Method 1 is the plane's fetch over the latency in blocks; method 2 the
 * lines it crosses in the latency, in blocks a line.  A line shorter
 * than a block takes method 2; a latency of a line time or more the
 * smaller of the two; a shorter one method 1.  The blocks are the method
 * rounded up plus one; above level 0 they are at least a line's blocks,
 * then one more, and never fewer than the level below's.  Level 0 has no
 * lines.  A level of more lines than the register holds, or without a
 * latency, is too big for any allocation; the allocation a level needs
 * is one more than its blocks.
 *
 * Procedure referenced: Intel Kaby Lake PRM Volume 12, Display Engine
 * (the watermark calculation methods).
 */
static void
i915_xd9_wm_level(
	const struct i915_xd_wm_pipe *pipe,
	const struct i915_xd_wm_params *params,
	unsigned latency,
	int above_zero,
	uint16_t floor_blocks,
	struct i915_xd_wm_level *level)
{
	uint32_t method1;
	uint32_t method2;
	uint32_t selected;
	uint32_t crossed;
	uint32_t blocks;
	uint32_t line_blocks;
	uint32_t lines;

	/* A level without a latency is never used. */
	if (latency == 0U) {
		level->min_ddb = I915_XD_WM_TOO_BIG;
		return;
	}

	/* Method 1: the bytes fetched over the latency, in blocks. */
	method1 = i915_xd_fixed_div(latency * params->pixel_rate * params->cpp, 1000U * I915_XD_WM_BLOCK_BYTES);

	/* Method 2: the lines crossed over the latency, in blocks. */
	crossed = i915_xd_div_round_up(latency * params->pixel_rate, pipe->htotal * 1000U);
	method2 = i915_xd_fixed_mul(crossed, params->blocks_per_line);

	/* The method the line's size and the latency choose. */
	if (params->cpp * pipe->htotal / I915_XD_WM_BLOCK_BYTES < 1U &&
	    params->bytes_per_line / I915_XD_WM_BLOCK_BYTES < 1U) {
		selected = method2;
	} else if (latency >= params->linetime_us) {
		selected = method1;
		if (method2 < method1)
			selected = method2;
	} else {
		selected = method1;
	}

	/* The blocks and the lines. */
	blocks = i915_xd_fixed_round_up(selected) + 1U;
	lines = i915_xd_div_round_up(selected, params->blocks_per_line);

	/* Above level 0: a line's blocks at least, one more, and no fewer than the level below; level 0 has no lines. */
	if (above_zero) {
		line_blocks = i915_xd_fixed_round_up(params->blocks_per_line);
		if (blocks < line_blocks)
			blocks = line_blocks;
		blocks++;
		if (blocks < floor_blocks)
			blocks = floor_blocks;
	} else {
		lines = 0U;
	}

	/* More lines than the register holds. */
	if (lines > I915_XD9_WM_MAX_LINES) {
		level->min_ddb = I915_XD_WM_TOO_BIG;
		return;
	}

	/* Succeeded: the level and the allocation it needs. */
	level->blocks = (uint16_t)blocks;
	level->lines = (uint16_t)lines;
	level->min_ddb = (uint16_t)(blocks + 1U);
	level->enable = 1U;
}

/*
 * Computes the eight display 9 levels of a surface, each deeper level no
 * smaller than the last one in use below it.
 */
static void
i915_xd9_wm_levels(
	const struct i915_xd_wm_pipe *pipe,
	const struct i915_xd_wm_params *params,
	struct i915_xd_wm_level *levels)
{
	uint16_t floor_blocks;
	unsigned index;

	/* Each level from its latency, the floor rising with every level in use. */
	floor_blocks = 0U;
	for (index = 0U; index < pipe->levels && index < I915_XD_WM_LEVELS; index++) {
		i915_xd9_wm_level(pipe, params, pipe->latency_us[index], index > 0U, floor_blocks, &levels[index]);
		if (levels[index].enable)
			floor_blocks = levels[index].blocks;
	}
}

/*
 * Plans one lit display 9 pipe: its share of the slice, the plane's and
 * the cursor's levels and parts, and its line time.  Returns 0, or
 * ENOSPC when the share holds neither the cursor nor the plane's level 0.
 */
static int
i915_xd9_dbuf_plan_pipe(
	const struct i915_xd_platform *platform,
	const uint16_t *latency,
	unsigned active,
	unsigned weight_before,
	unsigned weight_total,
	struct i915_xd9_pipe_plan *plan)
{
	struct i915_xd_wm_pipe timing;
	struct i915_xd_wm_surface surface;
	struct i915_xd_wm_params params;
	struct i915_xd_plane_wm cursor;
	struct i915_xd_wm_level *level;
	uint16_t cursor_blocks;
	uint32_t size;
	uint32_t plane_size;
	unsigned index;
	int chosen;

	/* The pipe's share of the slice by its width. */
	plan->computed = 1;
	drv_i915_xd_dbuf_pipe_range(platform, I915_XD9_DBUF_SLICE, weight_before, weight_before + plan->hdisplay, weight_total, &plan->pipe_ddb);
	size = (uint32_t)(plan->pipe_ddb.end - plan->pipe_ddb.start);

	/* The pipe's timing for the watermarks: eight levels, no priority control, no SAGV. */
	timing.levels = I915_XD_WM_LEVELS;
	for (index = 0U; index < I915_XD_WM_LEVELS; index++)
		timing.latency_us[index] = latency[index];
	timing.ipc = 0;
	timing.sagv_block_us = 0U;
	timing.pixel_rate_khz = plan->pixel_khz;
	timing.htotal = plan->htotal;

	/* The plane's levels: a linear surface of four bytes a pixel. */
	surface.width = plan->plane_width;
	surface.cpp = 4U;
	surface.layout = I915_XD_WM_LINEAR;
	surface.pixel_rate_khz = plan->pixel_khz;
	surface.minimal = 0;
	drv_i915_xd_wm_plane(platform, &timing, &surface, &plan->wm);

	/* The cursor's levels, and the blocks it keeps. */
	surface.width = I915_XD_WM_CURSOR_WIDTH;
	surface.cpp = I915_XD_WM_CURSOR_CPP;
	i915_xd_wm_params(&timing, &surface, &params);
	for (index = 0U; index < I915_XD_WM_LEVELS; index++) {
		cursor.level[index].enable = 0U;
		cursor.level[index].ignore_lines = 0U;
		cursor.level[index].blocks = 0U;
		cursor.level[index].lines = 0U;
		cursor.level[index].min_ddb = 0U;
	}
	i915_xd9_wm_levels(&timing, &params, cursor.level);
	cursor_blocks = drv_i915_xd_wm_cursor_blocks(platform, &timing, active);

	/* A share the cursor fills leaves nothing for the plane. */
	if (cursor_blocks >= size)
		return ENOSPC;
	plan->cursor_ddb.start = (uint16_t)(plan->pipe_ddb.end - cursor_blocks);
	plan->cursor_ddb.end = plan->pipe_ddb.end;

	/* The deepest level at which both the cursor and the plane fit. */
	plane_size = size - cursor_blocks;
	chosen = -1;
	for (index = I915_XD_WM_LEVELS; index > 0U; index--) {
		if (cursor.level[index - 1U].min_ddb > cursor_blocks)
			continue;
		if (plan->wm.level[index - 1U].min_ddb > plane_size)
			continue;
		chosen = (int)(index - 1U);
		break;
	}

	/* A plane that fits at no level does not fit. */
	if (chosen < 0)
		return ENOSPC;

	/* The plane takes the rest of the share. */
	plan->plane_ddb.start = plan->pipe_ddb.start;
	plan->plane_ddb.end = (uint16_t)(plan->pipe_ddb.start + plane_size);

	/* Deeper levels the share cannot hold, and levels too big for any, are off. */
	for (index = 0U; index < I915_XD_WM_LEVELS; index++) {
		level = &plan->wm.level[index];
		if ((int)index > chosen && level->min_ddb > plane_size) {
			level->enable = 0U;
			level->blocks = 0U;
			level->lines = 0U;
			level->min_ddb = 0U;
		}
		if (level->min_ddb == I915_XD_WM_TOO_BIG) {
			level->enable = 0U;
			level->blocks = 0U;
			level->lines = 0U;
			level->min_ddb = 0U;
		}
	}

	/* The line time in eighths of a microsecond, rounded up, as far as the register goes. */
	plan->linetime = (plan->htotal * 1000U * 8U + plan->pixel_khz - 1U) / plan->pixel_khz;
	if (plan->linetime > I915_XD9_LINETIME_MAX)
		plan->linetime = I915_XD9_LINETIME_MAX;

	/* Succeeded: the pipe fits. */
	return 0;
}
