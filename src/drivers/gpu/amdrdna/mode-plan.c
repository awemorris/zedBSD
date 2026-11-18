/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display plan of a mode: how early the plane's hub must start
 * fetching before the active area, the watermarks the hub's arbiter keeps,
 * and the plane's request, deadline and arbitration fields, for one plane
 * on one pipe (a linear surface of 32 bits a pixel, shown unscaled, no
 * compression, no page tables, no ODM, not interlaced) at the clocks the
 * board runs.  It is pure arithmetic in 64-bit fixed point (32 fraction
 * bits) so the kernel needs no floating point, and the host test compares
 * it with the reference model built on the host.
 *
 * The reference model rounds through single precision in its minimum,
 * maximum, floor and ceiling helpers, and its ceiling counts a value
 * within 1/100000 above a step as that step; the plan keeps the ceiling's
 * rule (it changes results by whole steps) and computes the rest exactly,
 * so a field may differ from the model's by one in its last unit.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/* A real number: a signed 64-bit integer counting 1/2^32. */
typedef int64_t plan_number;

/* One, and the ceiling's slack (0.99999). */
#define PLAN_ONE			((plan_number)1 << 32)
#define PLAN_SLACK_OVER			99999
#define PLAN_SLACK_UNDER		100000

/*
 * The hub's sizes: the reorder buffer and the detile buffer in bytes, the
 * page table requests the buffer holds, the 64 KB page-directory requests,
 * the pipe's output buffer in pixels and the output processor's buffer in
 * lines, the pixel chunk in bytes, the line buffer in bits and its most
 * lines, the line buffer's bits a pixel at 16 bits a component, the pipe
 * clock's pixels a cycle into the scaler and from the scaler to the line
 * buffer, the delays in pipe clock cycles (the pipe, the line buffer only
 * scaler, the format converter, a cursor) and in display clock cycles,
 * the tile repeaters between the hub and the pipes, the display clock's
 * ramp margin and the clocks' spread in percent, the clock generator's
 * VCO in MHz.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/dcn20_fpu.c (dcn2_0_ip:
 * rob_buffer_size_kbytes 168, det_buffer_size_kbytes 164,
 * dpte_buffer_size_in_pte_reqs_luma 84, pde_proc_buffer_size_64k_reqs 48,
 * dpp_output_buffer_pixels 2560, opp_output_buffer_lines 1,
 * pixel_chunk_size_kbytes 8, line_buffer_size_bits 789504,
 * max_line_buffer_lines 12, max_dchub_pscl_bw_pix_per_clk 4,
 * max_pscl_lb_bw_pix_per_clk 2, dppclk_delay_subtotal 77,
 * dppclk_delay_scl_lb_only 16, dppclk_delay_cnvc_formatter 8,
 * dppclk_delay_cnvc_cursor 6, dispclk_delay_subtotal 87,
 * max_inter_dcn_tile_repeaters 8, dispclk_ramp_margin_percent 1;
 * dcn2_0_soc: dcn_downspread_percent 0.5, dispclk_dppclk_vco_speed_mhz
 * 3850) and dml/display_mode_vba.c (fetch_pipe_params: 48 bits for
 * dm_lb_16).
 */
#define PLAN_REORDER_BYTES		(168 * 1024)
#define PLAN_DETILE_BYTES		(164 * 1024)
#define PLAN_PTE_REQUESTS		84
#define PLAN_DIRECTORY_REQUESTS		48
#define PLAN_PIPE_BUFFER_PIXELS		2560
#define PLAN_OUTPUT_BUFFER_LINES	1
#define PLAN_PIXEL_CHUNK_BYTES		(8 * 1024)
#define PLAN_LINE_BUFFER_BITS		789504
#define PLAN_LINE_BUFFER_LINES		12
#define PLAN_LINE_BUFFER_PIXEL_BITS	48
#define PLAN_HUB_TO_SCALER_PIXELS	4
#define PLAN_SCALER_TO_BUFFER_PIXELS	2
#define PLAN_PIPE_DELAY			77
#define PLAN_SCALER_DELAY		16
#define PLAN_FORMAT_DELAY		8
#define PLAN_CURSOR_DELAY		6
#define PLAN_DISPLAY_DELAY		87
#define PLAN_REPEATERS			8
#define PLAN_RAMP_PERCENT		1
#define PLAN_SPREAD_PERCENT_TENTHS	5
#define PLAN_VCO_MHZ			3850

/*
 * The memory system: the urgent latency, the self-refresh exit and its
 * entry with the exit, the memory clock change's latency (in tenths of a
 * microsecond), the bytes a channel returns out of order, the share of
 * the ideal bandwidth the display gets after an urgent latency, the round
 * trip in return clock cycles, the channels and a channel's width in
 * bytes, the fabric's and the return bus's bytes a clock.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/dcn20_fpu.c (dcn2_0_soc:
 * urgent_latency_pixel_data_only_us 4.0, sr_exit_time_us 8.6,
 * sr_enter_plus_exit_time_us 10.9, dram_clock_change_latency_us 404.0,
 * urgent_out_of_order_return_per_channel_pixel_only_bytes 4096,
 * pct_ideal_dram_sdp_bw_after_urgent_pixel_only 40.0,
 * round_trip_ping_latency_dcfclk_cycles 131, num_chans 16,
 * dram_channel_width_bytes 2, fabric_datapath_to_dcn_data_return_bytes 64,
 * return_bus_width_bytes 64).
 */
#define PLAN_URGENT_TENTHS		40
#define PLAN_EXIT_TENTHS		86
#define PLAN_ENTER_EXIT_TENTHS		109
#define PLAN_SWITCH_TENTHS		4040
#define PLAN_OUT_OF_ORDER_BYTES		4096
#define PLAN_SHARE_PERCENT		40
#define PLAN_ROUND_TRIP_CYCLES		131
#define PLAN_CHANNELS			16
#define PLAN_CHANNEL_BYTES		2
#define PLAN_FABRIC_BYTES		64
#define PLAN_RETURN_BYTES		64

/*
 * The model's fixed steps: the first start line it tries, the extra round
 * trip cycles, the deep sleep floor of the return clock in MHz and its
 * bytes a cycle (the multiplier 1.1 as 11/10), the cycles the deadline
 * calculation takes on it (24) and the cycles the update and ready windows
 * take on it (14 and 20) and on the pipe clock (12, 10 and 150), the
 * margin in microseconds above which the memory clock may change in the
 * active area and the time added then, the cursor's width and bytes a
 * request, and the fields the deadline and arbitration registers take as
 * they are: the request chunk (8 KB), the smallest chunk (1 KB), the meta
 * chunk (2 KB), the smallest meta chunk (256 B), the page table groups (2
 * KB), the cursor's chunk adjustment, the request limit (off), the flip's
 * and the fixed priority levels.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/display_mode_vba_20v2.c
 * (dml20v2_DISPCLKDPPCLKDCFCLKDeepSleepPrefetchParametersWatermarksAndPerformanceCalculation:
 * VStartupLines 13, + 32 cycles, DCFCLKDeepSleep 8.0 and 1.1 / 64,
 * TCalc 24.0, the margin 60 and + 25; CalculatePrefetchSchedule: 14.0,
 * 12.0, 150.0, 20.0, 10.0) and dml/dcn20/display_rq_dlg_calc_20v2.c
 * (get_surf_rq_param: chunk_bytes 8192, min_chunk_bytes 1024,
 * meta_chunk_bytes 2048, min_meta_chunk_bytes 256, mpte_group_bytes 2048;
 * get_meta_and_pte_attr: dpte_group_bytes 2048;
 * dml20v2_rq_dlg_get_dlg_params: chunk_hdl_adjust_cur0 3,
 * dst_y_delta_drq_limit 0x7fff, qos_level_flip 14, qos_level_fixed_l 8;
 * calculate_ttu_cursor: 256-byte requests).
 */
#define PLAN_FIRST_START_LINES		13
#define PLAN_TRIP_EXTRA_CYCLES		32
#define PLAN_SLEEP_FLOOR_MHZ		8
#define PLAN_SLEEP_BYTES		64
#define PLAN_CALC_CYCLES		24
#define PLAN_UPDATE_SLEEP_CYCLES	14
#define PLAN_UPDATE_PIPE_CYCLES		12
#define PLAN_READY_PIPE_FLOOR		150
#define PLAN_READY_SLEEP_CYCLES		20
#define PLAN_READY_PIPE_CYCLES		10
#define PLAN_ACTIVE_SWITCH_US		60
#define PLAN_ACTIVE_SWITCH_ADDED_US	25
#define PLAN_CURSOR_PIXELS		256
#define PLAN_CURSOR_REQUEST_BYTES	256
#define PLAN_CHUNK_BYTES		8192
#define PLAN_LEAST_CHUNK_BYTES		1024
#define PLAN_META_CHUNK_BYTES		2048
#define PLAN_LEAST_META_CHUNK_BYTES	256
#define PLAN_PTE_GROUP_BYTES		2048
#define PLAN_CURSOR_CHUNK_ADJUST	3
#define PLAN_REQUEST_LIMIT		32767
#define PLAN_FLIP_LEVEL			14
#define PLAN_FIXED_LEVEL		8

/* The bytes of a pixel, of a 256-byte block's request and of a page. */
#define PLAN_PIXEL_BYTES		4
#define PLAN_BLOCK_BYTES		256
#define PLAN_PAGE_BYTES			4096

/*
 * The clocks a prefetch is worked out at (in MHz): the display clock, the
 * pipe clock and the return clock's deep sleep floor, and the deadline
 * calculation's time on it in microseconds.
 */
struct plan_view {
	plan_number display_mhz;
	plan_number pipe_mhz;
	plan_number sleep_mhz;
	plan_number calc_us;
};

/*
 * A prefetch: the time from the pipe's input to its output in pixels and
 * lines, the global sync's update offset, update width and ready offset
 * in pixels, the prefetch's lines (in quarters), the lines for the
 * cursor's and the rows' requests, and the ratio it reads the plane's
 * lines at.
 */
struct plan_fetch {
	plan_number after_x;
	plan_number after_y;
	uint32_t update_offset;
	plan_number update_width;
	plan_number ready_offset;
	plan_number prefetch_lines;
	plan_number vm_lines;
	plan_number row_lines;
	plan_number prefetch_ratio;
};

/*
 * What the plan works out between its steps: the clocks and the reference
 * in MHz, a line's time in microseconds, the bandwidths in bytes a
 * microsecond, the latencies and watermarks in microseconds, the lines
 * the detile buffer holds, the time from the pipe's input to its output
 * in pixels and lines, the prefetch's lines and ratio, the global sync's
 * pixels and the start line, the time to allow in the vertical blank, the
 * memory's use of the blank, and the request geometry.
 */
struct plan_work {
	plan_number pixel_mhz;
	plan_number pipe_mhz;
	plan_number display_mhz;
	plan_number return_mhz;
	plan_number reference_to_pixel;
	plan_number line_us;
	plan_number return_bw;
	plan_number read_bw;
	plan_number cursor_bw;
	plan_number extra_us;
	plan_number urgent_us;
	plan_number switch_us;
	plan_number exit_us;
	plan_number enter_exit_us;
	plan_number sleep_mhz;
	plan_number calc_us;
	plan_number buffer_lines;
	plan_number buffer_whole_lines;
	struct plan_fetch fetch;
	plan_number ttu_us;
	uint32_t start_lines;
	unsigned vblank_use;
	unsigned memory_switch;
	uint32_t swath_bound;
	uint32_t requests;
	uint32_t pte_rows;
	uint32_t pte_groups;
	uint32_t meta_chunks;
};

static plan_number number_from(int64_t whole);
static plan_number number_ratio(int64_t over, int64_t under);
static plan_number number_times(plan_number left, plan_number right);
static plan_number number_over(plan_number over, plan_number under);
static plan_number number_down(plan_number value, plan_number step);
static plan_number number_up(plan_number value, plan_number step);
static plan_number number_least(plan_number left, plan_number right);
static plan_number number_most(plan_number left, plan_number right);
static int64_t number_whole(plan_number value);
static unsigned whole_log2(uint64_t value);
static uint32_t multiple_above(uint32_t value, uint32_t step);
static int plan_clocks(const struct amdrdna_mode_plan_input *input, struct plan_work *work);
static void plan_rows(const struct amdrdna_mode_plan_input *input, struct plan_work *work);
static void plan_marks(const struct amdrdna_mode_plan_input *input, struct plan_work *work);
static int plan_choose(const struct amdrdna_mode_plan_input *input, struct plan_work *work);
static int plan_schedule(const struct amdrdna_mode_plan_input *input, struct plan_work *work);
static int plan_prefetch(const struct amdrdna_mode_plan_input *input, const struct plan_work *work, const struct plan_view *view, unsigned vblank_use, uint32_t start_lines, struct plan_fetch *fetch);
static void plan_switch(const struct amdrdna_mode_plan_input *input, struct plan_work *work);
static void plan_fields(const struct amdrdna_mode_plan_input *input, const struct plan_work *work, struct amdrdna_mode_plan *plan);
static plan_number plan_delivery(const struct plan_work *work, plan_number ratio, uint32_t pixel_width, uint32_t scaler_width, uint32_t parts);
static uint32_t plan_switch_tenths(const struct amdrdna_mode_plan_input *input);

/*
 * Works out the display plan of a mode at the board's clocks.
 *
 * Returns 0; EINVAL for a mode the plan does not cover (empty, a pitch
 * narrower than the width, a missing clock); ERANGE for a mode the clocks
 * or the hub cannot show (it needs a faster display or pipe clock, or no
 * start line leaves the prefetch enough time and bandwidth).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/dcn20_fpu.c
 * (dcn20_populate_dml_pipes_from_context, dcn20_calculate_wm,
 * dcn20_calculate_dlg_params), dml/dcn20/display_mode_vba_20v2.c
 * (dml20v2_recalculate and its steps) and
 * dml/dcn20/display_rq_dlg_calc_20v2.c (dml20v2_rq_dlg_get_rq_reg,
 * dml20v2_rq_dlg_get_dlg_reg), for the one-plane case.
 */
int
amdrdna_mode_plan(
	const struct amdrdna_mode_plan_input *input,
	struct amdrdna_mode_plan *plan)
{
	struct plan_work work;
	int error;

	/* A mode the plan does not cover is refused. */
	*plan = (struct amdrdna_mode_plan){ 0 };
	work = (struct plan_work){ 0 };
	if (input->width == 0U || input->height == 0U || input->pixel_khz == 0U)
		return EINVAL;
	if (input->h_total <= input->width || input->v_total <= input->height)
		return EINVAL;
	if (input->pitch < input->width)
		return EINVAL;

	/* The clocks, and whether they are fast enough for the pixel clock. */
	error = plan_clocks(input, &work);
	if (error != 0)
		return error;

	/* The request geometry, then the bandwidths, latencies and watermarks. */
	plan_rows(input, &work);
	plan_marks(input, &work);

	/* The use of the vertical blank, then the start line at the board's clocks. */
	error = plan_choose(input, &work);
	if (error != 0)
		return error;
	error = plan_schedule(input, &work);
	if (error != 0)
		return error;

	/* Whether the memory clock may change, then the fields. */
	plan_switch(input, &work);
	plan_fields(input, &work, plan);

	/* Succeeded: the plan is complete. */
	return 0;
}

/* Makes a number from a whole number. */
static plan_number
number_from(
	int64_t whole)
{
	/* Succeeded: the whole number in units of 1/2^32. */
	return whole * PLAN_ONE;
}

/* Makes a number from a fraction of whole numbers. */
static plan_number
number_ratio(
	int64_t over,
	int64_t under)
{
	plan_number value;

	/* The division of the two as numbers. */
	value = number_over(number_from(over), number_from(under));

	/* Succeeded: the fraction. */
	return value;
}

/* Multiplies two numbers, the product's last bits cut. */
static plan_number
number_times(
	plan_number left,
	plan_number right)
{
	uint64_t left_size;
	uint64_t right_size;
	uint64_t high;
	uint64_t middle;
	uint64_t low;
	uint64_t size;
	bool negative;

	/* The sizes and the sign. */
	negative = false;
	left_size = (uint64_t)left;
	right_size = (uint64_t)right;
	if (left < 0) {
		negative = !negative;
		left_size = (uint64_t)0 - left_size;
	}
	if (right < 0) {
		negative = !negative;
		right_size = (uint64_t)0 - right_size;
	}

	/* The product of the 32-bit halves, shifted back by 32 bits. */
	high = (left_size >> 32) * (right_size >> 32);
	middle = (left_size >> 32) * (right_size & 0xffffffffU) + (left_size & 0xffffffffU) * (right_size >> 32);
	low = ((left_size & 0xffffffffU) * (right_size & 0xffffffffU)) >> 32;
	size = (high << 32) + middle + low;

	/* A negative product. */
	if (negative)
		return -(plan_number)size;

	/* Succeeded: the product. */
	return (plan_number)size;
}

/* Divides one number by another, the quotient's last bits cut (0 for a zero divisor). */
static plan_number
number_over(
	plan_number over,
	plan_number under)
{
	uint64_t over_size;
	uint64_t under_size;
	uint64_t whole;
	uint64_t rest;
	uint64_t fraction;
	unsigned bit;
	bool negative;

	/* A zero divisor gives zero. */
	if (under == 0)
		return 0;

	/* The sizes and the sign. */
	negative = false;
	over_size = (uint64_t)over;
	under_size = (uint64_t)under;
	if (over < 0) {
		negative = !negative;
		over_size = (uint64_t)0 - over_size;
	}
	if (under < 0) {
		negative = !negative;
		under_size = (uint64_t)0 - under_size;
	}

	/* The whole part, then 32 bits of fraction by long division of the rest. */
	whole = over_size / under_size;
	rest = over_size % under_size;
	fraction = 0U;
	for (bit = 0U; bit < 32U; bit++) {
		rest <<= 1;
		fraction <<= 1;
		if (rest >= under_size) {
			rest -= under_size;
			fraction |= 1U;
		}
	}

	/* A negative quotient. */
	if (negative)
		return -(plan_number)((whole << 32) | fraction);

	/* Succeeded: the quotient. */
	return (plan_number)((whole << 32) | fraction);
}

/* Cuts a number to the whole steps it holds, toward zero (the model's floor). */
static plan_number
number_down(
	plan_number value,
	plan_number step)
{
	plan_number steps;
	plan_number result;

	/* The steps, cut toward zero, times the step. */
	steps = number_over(value, step);
	result = number_times(number_from(number_whole(steps)), step);

	/* Succeeded: the value at a whole step. */
	return result;
}

/* Raises a number to the next whole step, a value just above a step counting as that step (the model's ceiling). */
static plan_number
number_up(
	plan_number value,
	plan_number step)
{
	plan_number steps;
	plan_number result;

	/* The steps with the slack added, cut toward zero, times the step. */
	steps = number_over(value, step) + number_ratio(PLAN_SLACK_OVER, PLAN_SLACK_UNDER);
	result = number_times(number_from(number_whole(steps)), step);

	/* Succeeded: the value at a whole step. */
	return result;
}

/* Gives the smaller of two numbers. */
static plan_number
number_least(
	plan_number left,
	plan_number right)
{
	/* The right one when it is smaller. */
	if (right < left)
		return right;

	/* Succeeded: the left one is not larger. */
	return left;
}

/* Gives the larger of two numbers. */
static plan_number
number_most(
	plan_number left,
	plan_number right)
{
	/* The right one when it is larger. */
	if (right > left)
		return right;

	/* Succeeded: the left one is not smaller. */
	return left;
}

/* Gives the whole part of a number, cut toward zero. */
static int64_t
number_whole(
	plan_number value)
{
	/* A negative number's size, cut, with the sign put back. */
	if (value < 0)
		return -(int64_t)(((uint64_t)0 - (uint64_t)value) >> 32);

	/* Succeeded: the whole part. */
	return (int64_t)((uint64_t)value >> 32);
}

/* Gives the power of two at or below a whole number (0 for 0 and 1). */
static unsigned
whole_log2(
	uint64_t value)
{
	unsigned power;

	/* Halves the value until one is left. */
	power = 0U;
	while (value > 1U) {
		value >>= 1;
		power++;
	}

	/* Succeeded: the power. */
	return power;
}

/* Raises a whole number to a multiple of a step (unchanged when it is one). */
static uint32_t
multiple_above(
	uint32_t value,
	uint32_t step)
{
	uint32_t rest;

	/* What is left above the last multiple. */
	rest = value % step;
	if (rest == 0U)
		return value;

	/* Succeeded: the next multiple. */
	return value + step - rest;
}

/*
 * Turns the clocks into numbers and checks that the display clock and the
 * pipe clock are fast enough for the pixel clock, with the spread and the
 * ramp margin, at the clock generator's steps.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/display_mode_vba_20v2.c
 * (dml20v2_DISPCLKDPPCLKDCFCLKDeepSleepPrefetchParametersWatermarksAndPerformanceCalculation,
 * the display and pipe clocks; RoundToDFSGranularityUp).
 */
static int
plan_clocks(
	const struct amdrdna_mode_plan_input *input,
	struct plan_work *work)
{
	plan_number spread;
	plan_number needed;
	plan_number divider;
	plan_number vco_steps;

	/* A clock the board did not give is refused. */
	if (input->reference_khz == 0U || input->return_khz == 0U || input->fabric_khz == 0U)
		return EINVAL;
	if (input->display_khz == 0U || input->pipe_khz == 0U || input->memory_mts == 0U)
		return EINVAL;

	/* The clocks in MHz, a line's time and the reference's cycles a pixel. */
	work->pixel_mhz = number_ratio(input->pixel_khz, 1000);
	work->pipe_mhz = number_ratio(input->pipe_khz, 1000);
	work->display_mhz = number_ratio(input->display_khz, 1000);
	work->return_mhz = number_ratio(input->return_khz, 1000);
	work->reference_to_pixel = number_ratio(input->reference_khz, input->pixel_khz);
	work->line_us = number_over(number_from(input->h_total), work->pixel_mhz);

	/* The display clock the pixels need with the spread and the margin, at a generator step. */
	spread = PLAN_ONE + number_ratio(PLAN_SPREAD_PERCENT_TENTHS, 1000);
	needed = number_times(number_times(work->pixel_mhz, spread), PLAN_ONE + number_ratio(PLAN_RAMP_PERCENT, 100));
	vco_steps = number_from(PLAN_VCO_MHZ * 4);
	divider = number_down(number_over(vco_steps, needed), PLAN_ONE);
	if (divider <= 0)
		return ERANGE;
	needed = number_over(vco_steps, divider);
	if (needed > work->display_mhz)
		return ERANGE;

	/* The pipe clock an unscaled pipe needs (one pixel a cycle) with the spread, at a generator step. */
	needed = number_times(work->pixel_mhz, spread);
	divider = number_down(number_over(vco_steps, needed), PLAN_ONE);
	if (divider <= 0)
		return ERANGE;
	needed = number_over(vco_steps, divider);
	if (needed > work->pipe_mhz)
		return ERANGE;

	/* Succeeded: the clocks are known and fast enough. */
	return 0;
}

/*
 * Works out the plane's request geometry: the swath's width rounded up to
 * whole 256-byte blocks and its requests, the lines a page table row
 * covers (a power of two the pitch and the page table buffer allow, at
 * most 128) and its groups, and the meta chunks a row takes.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/display_rq_dlg_calc_20v2.c
 * (get_meta_and_pte_attr, the linear, horizontal case).
 */
static void
plan_rows(
	const struct amdrdna_mode_plan_input *input,
	struct plan_work *work)
{
	uint32_t block_pixels;
	uint32_t page_request_pixels;
	uint64_t pixels_held;
	uint64_t directory_pixels;
	unsigned row_log;
	uint32_t row_width;
	uint32_t group_pixels;
	uint32_t meta_request_pixels;
	uint32_t meta_row_width;
	uint32_t meta_chunk_pixels;
	uint32_t least_meta_chunk_pixels;
	uint32_t meta_rest;
	plan_number groups;

	/* The swath: whole 256-byte blocks of one line, and a request a block. */
	block_pixels = PLAN_BLOCK_BYTES / PLAN_PIXEL_BYTES;
	work->swath_bound = multiple_above(input->width - 1U, block_pixels) + block_pixels;
	work->requests = work->swath_bound / block_pixels;

	/* A page table request covers 8 pages of one line; the rows the buffer holds at the pitch, as a power of two, at most 128. */
	page_request_pixels = 8U * PLAN_PAGE_BYTES / PLAN_PIXEL_BYTES;
	pixels_held = (uint64_t)PLAN_PTE_REQUESTS * page_request_pixels;
	directory_pixels = (uint64_t)64U * 1024U * PLAN_DIRECTORY_REQUESTS / PLAN_PIXEL_BYTES;
	if (directory_pixels < pixels_held)
		pixels_held = directory_pixels;
	row_log = whole_log2(pixels_held / input->pitch);
	if (row_log > 7U)
		row_log = 7U;
	work->pte_rows = 1U << row_log;

	/* The groups of a row: a group is 32 requests wide, and a row is the pitch's lines rounded to whole requests. */
	row_width = multiple_above(input->pitch * work->pte_rows - 1U, page_request_pixels) + page_request_pixels;
	group_pixels = (PLAN_PTE_GROUP_BYTES / 64U) * page_request_pixels;
	groups = number_up(number_ratio(row_width, group_pixels), PLAN_ONE);
	work->pte_groups = (uint32_t)number_whole(groups);

	/* The meta chunks of a row: a request is 8 by 8 blocks, a chunk 2 KB of them, one more past a short remainder. */
	meta_request_pixels = 64U * PLAN_BLOCK_BYTES / PLAN_PIXEL_BYTES / 8U;
	meta_row_width = multiple_above(input->width - 1U, meta_request_pixels) + meta_request_pixels;
	meta_chunk_pixels = PLAN_META_CHUNK_BYTES * PLAN_BLOCK_BYTES / PLAN_PIXEL_BYTES / 8U;
	least_meta_chunk_pixels = PLAN_LEAST_META_CHUNK_BYTES * PLAN_BLOCK_BYTES / PLAN_PIXEL_BYTES / 8U;
	meta_rest = meta_row_width % meta_chunk_pixels;
	work->meta_chunks = meta_row_width / meta_chunk_pixels + 2U;
	if (meta_rest <= 2U * least_meta_chunk_pixels - meta_request_pixels)
		work->meta_chunks = meta_row_width / meta_chunk_pixels + 1U;
}

/*
 * Works out the bandwidth the hub returns, the plane's and the cursor's
 * read bandwidth, the urgent extra latency and the watermarks (urgent,
 * memory clock change, self-refresh exit and entry), the return clock's
 * deep sleep floor, the deadline calculation's time and the lines the
 * detile buffer holds.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/display_mode_vba_20v2.c
 * (dml20v2_DISPCLKDPPCLKDCFCLKDeepSleepPrefetchParametersWatermarksAndPerformanceCalculation:
 * ReturnBW, TotalDataReadBandwidth, UrgentExtraLatency, the watermarks,
 * DCFCLKDeepSleep, TCalc, LinesInDETY; adjust_ReturnBW without
 * compression; dml20v2_ModeSupportAndSystemConfigurationFull:
 * FabricAndDRAMBandwidthPerState).
 */
static void
plan_marks(
	const struct amdrdna_mode_plan_input *input,
	struct plan_work *work)
{
	plan_number fabric_bw;
	plan_number bus_bw;
	plan_number trip_us;
	plan_number delivery_us;
	plan_number sleep_mhz;

	/* The return bandwidth: the share the display gets of the bus or of the memory and fabric, whichever is less. */
	fabric_bw = number_least(number_from((int64_t)input->memory_mts * PLAN_CHANNELS * PLAN_CHANNEL_BYTES),
	    number_ratio((int64_t)input->fabric_khz * PLAN_FABRIC_BYTES, 1000));
	bus_bw = number_times(number_from(PLAN_RETURN_BYTES), work->return_mhz);
	work->return_bw = number_times(number_least(bus_bw, fabric_bw), number_ratio(PLAN_SHARE_PERCENT, 100));

	/* The plane reads its width of 4-byte pixels a line; the cursor 256 pixels a line. */
	work->read_bw = number_over(number_from((int64_t)input->width * PLAN_PIXEL_BYTES), work->line_us);
	work->cursor_bw = number_over(number_from(PLAN_CURSOR_PIXELS * PLAN_PIXEL_BYTES), work->line_us);

	/* The extra latency: the round trip, the out-of-order returns of all channels, and a pixel chunk. */
	trip_us = number_over(number_from(PLAN_ROUND_TRIP_CYCLES + PLAN_TRIP_EXTRA_CYCLES), work->return_mhz);
	trip_us += number_over(number_from(PLAN_OUT_OF_ORDER_BYTES * PLAN_CHANNELS), work->return_bw);
	work->extra_us = trip_us + number_over(number_from(PLAN_PIXEL_CHUNK_BYTES), work->return_bw);

	/* The watermarks: urgent, the memory clock change, and the self-refresh exit and entry. */
	work->urgent_us = number_ratio(PLAN_URGENT_TENTHS, 10) + work->extra_us;
	work->switch_us = number_ratio(plan_switch_tenths(input), 10) + work->urgent_us;
	work->enter_exit_us = number_ratio(PLAN_ENTER_EXIT_TENTHS, 10) + work->extra_us;

	/* The deep sleep floor: 1.1 times a line's bytes over 64 bytes a cycle in the line's delivery time, at least a 16th of the pixel clock. */
	delivery_us = number_over(number_from(input->width), work->pixel_mhz);
	sleep_mhz = number_over(number_ratio((int64_t)11 * input->width * PLAN_PIXEL_BYTES, 10 * PLAN_SLEEP_BYTES), delivery_us);
	sleep_mhz = number_most(sleep_mhz, number_over(work->pixel_mhz, number_from(16)));
	work->sleep_mhz = number_most(number_from(PLAN_SLEEP_FLOOR_MHZ), sleep_mhz);
	work->exit_us = number_ratio(PLAN_EXIT_TENTHS, 10) + work->extra_us + number_over(number_from(10), work->sleep_mhz);
	work->calc_us = number_over(number_from(PLAN_CALC_CYCLES), work->sleep_mhz);

	/* The lines the detile buffer holds, and its whole lines. */
	work->buffer_lines = number_ratio(PLAN_DETILE_BYTES / PLAN_PIXEL_BYTES, input->width);
	work->buffer_whole_lines = number_down(work->buffer_lines, PLAN_ONE);
}

/*
 * Chooses the use of the vertical blank the way the model's mode support
 * does: at the clocks the mode needs at least (the display clock with the
 * spread and the margin, the pipe clock with the spread, the return
 * clock's projected deep sleep floor) and the latest start line, the
 * memory clock change and the self-refresh first, then the self-refresh
 * alone; the prefetch must fit at one of them.
 *
 * Returns 0, or ERANGE when it fits at neither.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/display_mode_vba_20v2.c
 * (dml20v2_ModeSupportAndSystemConfigurationFull: the prefetch mode loop
 * with RequiredDISPCLK, RequiredDPPCLK, ProjectedDCFCLKDeepSleep,
 * TimeCalc and MaximumVStartup; display_mode_vba.c
 * CalculateMinAndMaxPrefetchMode for the default choice).
 */
static int
plan_choose(
	const struct amdrdna_mode_plan_input *input,
	struct plan_work *work)
{
	struct plan_view view;
	struct plan_fetch fetch;
	plan_number spread;
	uint32_t most_lines;
	unsigned vblank_use;
	int error;

	/* The least clocks the mode needs, and the deep sleep floor projected for them. */
	spread = PLAN_ONE + number_ratio(PLAN_SPREAD_PERCENT_TENTHS, 1000);
	view.display_mhz = number_times(number_times(work->pixel_mhz, spread), PLAN_ONE + number_ratio(PLAN_RAMP_PERCENT, 100));
	view.pipe_mhz = number_times(work->pixel_mhz, spread);
	view.sleep_mhz = number_most(number_over(work->pixel_mhz, number_from(16)),
	    number_times(number_ratio(11 * PLAN_PIXEL_BYTES, 10 * PLAN_SLEEP_BYTES), work->pixel_mhz));
	view.calc_us = number_over(number_from(PLAN_CALC_CYCLES), view.sleep_mhz);

	/* The latest start line: the blank less one line. */
	most_lines = input->v_total - input->height - 1U;
	if (most_lines == 0U)
		return ERANGE;

	/* The memory clock change with the self-refresh, then the self-refresh alone. */
	error = ERANGE;
	for (vblank_use = 0U; vblank_use < 2U; vblank_use++) {
		error = plan_prefetch(input, work, &view, vblank_use, most_lines, &fetch);
		if (error == 0)
			break;
	}

	/* Reports a mode the prefetch fits at neither use. */
	if (error != 0)
		return error;

	/* The use chosen. */
	work->vblank_use = vblank_use;

	/* Succeeded: the prefetch fits at this use. */
	return 0;
}

/*
 * Finds the first start line, from 13 up to the vertical blank less one
 * line, whose prefetch fits at the board's clocks for the use of the
 * blank chosen; the global sync's offsets and the prefetch come with it.
 *
 * Returns 0, or ERANGE when no start line fits.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/display_mode_vba_20v2.c
 * (dml20v2_DISPCLKDPPCLKDCFCLKDeepSleepPrefetchParametersWatermarksAndPerformanceCalculation,
 * the start line loop).
 */
static int
plan_schedule(
	const struct amdrdna_mode_plan_input *input,
	struct plan_work *work)
{
	struct plan_view view;
	struct plan_fetch fetch;
	uint32_t most_lines;
	uint32_t lines;
	uint32_t start_lines;
	int error;

	/* The board's clocks and the deep sleep floor worked out for them. */
	view.display_mhz = work->display_mhz;
	view.pipe_mhz = work->pipe_mhz;
	view.sleep_mhz = work->sleep_mhz;
	view.calc_us = work->calc_us;

	/* The start lines tried, from 13 up to the blank less one line, until the prefetch fits. */
	most_lines = input->v_total - input->height - 1U;
	error = ERANGE;
	start_lines = 0U;
	for (lines = PLAN_FIRST_START_LINES; lines <= most_lines || start_lines == 0U; lines++) {
		start_lines = lines;
		if (start_lines > most_lines)
			start_lines = most_lines;
		error = plan_prefetch(input, work, &view, work->vblank_use, start_lines, &fetch);
		if (error == 0)
			break;
	}

	/* Reports a mode no start line fits. */
	if (error != 0)
		return error;

	/* The start line and its prefetch. */
	work->start_lines = start_lines;
	work->fetch = fetch;

	/* Succeeded: the prefetch fits. */
	return 0;
}

/*
 * Works out the prefetch for a start line at some clocks: the delay from
 * the pipe's input to its output, the global sync's offsets, the lines
 * left for the prefetch (the start line less the wait the use of the
 * blank needs, the calculation, the sync and the pipe's delay, or the
 * three lines it needs when fewer), the lines for the cursor's and the
 * rows' requests, and the ratio the prefetch reads the plane's two
 * prefetch lines at; then checks that it has two lines at least, a ratio
 * of 4 at most, and the bandwidth.
 *
 * Returns 0, or ERANGE when the prefetch does not fit.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/display_mode_vba_20v2.c
 * (CalculateTWait; CalculateDelayAfterScaler; CalculatePrefetchSchedule
 * without page tables and compression, with the cursor;
 * CalculatePrefetchSourceLines, unscaled and progressive: two lines; the
 * bandwidth and ratio checks of the start line loop).
 */
static int
plan_prefetch(
	const struct amdrdna_mode_plan_input *input,
	const struct plan_work *work,
	const struct plan_view *view,
	unsigned vblank_use,
	uint32_t start_lines,
	struct plan_fetch *fetch)
{
	plan_number wait_us;
	plan_number after_x;
	plan_number repeater_us;
	plan_number setup_us;
	plan_number fitting_lines;
	plan_number lines;
	plan_number meta_us;
	plan_number row_us;
	plan_number data_lines;
	plan_number ratio;
	plan_number pixel_bw;
	plan_number total_bw;
	plan_number quarter;
	uint32_t pipe_cycles;

	/* The wait: the memory clock change with an urgent latency, the self-refresh, or only the urgent latency. */
	wait_us = number_ratio(PLAN_URGENT_TENTHS, 10);
	if (vblank_use == 0U) {
		wait_us = number_most(number_ratio((int64_t)plan_switch_tenths(input) + PLAN_URGENT_TENTHS, 10),
		    number_most(number_ratio(PLAN_ENTER_EXIT_TENTHS, 10), wait_us));
	} else if (vblank_use == 1U) {
		wait_us = number_most(number_ratio(PLAN_ENTER_EXIT_TENTHS, 10), wait_us);
	}

	/* The delay through the pipe (with the cursor's) and the display clock's part, as lines and pixels. */
	pipe_cycles = PLAN_PIPE_DELAY + PLAN_SCALER_DELAY + PLAN_FORMAT_DELAY + PLAN_CURSOR_DELAY;
	after_x = number_over(number_times(number_from(pipe_cycles), work->pixel_mhz), view->pipe_mhz);
	after_x += number_over(number_times(number_from(PLAN_DISPLAY_DELAY), work->pixel_mhz), view->display_mhz);
	fetch->after_y = number_down(number_over(after_x, number_from(input->h_total)), PLAN_ONE);
	fetch->after_x = after_x - number_times(fetch->after_y, number_from(input->h_total));

	/* The global sync: the update a quarter line in, its width and the ready offset, in pixels. */
	fetch->update_offset = (uint32_t)number_whole(number_up(number_ratio(input->h_total, 4), PLAN_ONE));
	repeater_us = number_times(number_from(PLAN_REPEATERS),
	    number_over(number_from(2), view->pipe_mhz) + number_over(number_from(3), view->display_mhz));
	fetch->update_width = number_times(number_over(number_from(PLAN_UPDATE_SLEEP_CYCLES), view->sleep_mhz) +
	    number_over(number_from(PLAN_UPDATE_PIPE_CYCLES), view->pipe_mhz) + repeater_us, work->pixel_mhz);
	fetch->ready_offset = number_times(number_most(number_over(number_from(PLAN_READY_PIPE_FLOOR), view->pipe_mhz),
	    repeater_us + number_over(number_from(PLAN_READY_SLEEP_CYCLES), view->sleep_mhz) +
	    number_over(number_from(PLAN_READY_PIPE_CYCLES), view->pipe_mhz)), work->pixel_mhz);
	setup_us = number_over(number_from(fetch->update_offset) + fetch->update_width + fetch->ready_offset, work->pixel_mhz);

	/* The lines left before the active area, and the three the prefetch needs (a quarter, three quarters and two lines). */
	fitting_lines = number_from(start_lines) - number_over(view->calc_us + wait_us, work->line_us) -
	    number_over(setup_us, work->line_us) - (fetch->after_y + number_over(fetch->after_x, number_from(input->h_total)));
	lines = fitting_lines;
	if (3 * PLAN_ONE < fitting_lines)
		lines = 3 * PLAN_ONE;

	/* To whole quarters, rounding up past an eighth. */
	quarter = number_ratio(1, 4);
	lines = number_times(number_down(number_times(number_from(4), lines + number_ratio(1, 8)), PLAN_ONE), quarter);
	fetch->prefetch_lines = lines;
	if (lines <= PLAN_ONE)
		return ERANGE;

	/* The cursor's request a quarter line and the rows' the rest of a line, to whole quarters. */
	meta_us = number_over(work->line_us, number_from(4));
	row_us = work->line_us - meta_us;
	fetch->vm_lines = number_times(number_down(number_times(number_from(4),
	    number_over(meta_us, work->line_us) + number_ratio(1, 8)), PLAN_ONE), quarter);
	fetch->row_lines = number_times(number_down(number_times(number_from(4),
	    number_over(row_us, work->line_us) + number_ratio(1, 8)), PLAN_ONE), quarter);

	/* The lines left for the plane's two lines, and their ratio (at least 1). */
	data_lines = lines - (fetch->vm_lines + fetch->row_lines);
	if (data_lines <= 0)
		return ERANGE;
	ratio = number_over(number_from(2), data_lines);
	fetch->prefetch_ratio = number_most(ratio, PLAN_ONE);

	/* The prefetch needs two lines at least and a ratio of 4 at most. */
	if (lines < 2 * PLAN_ONE)
		return ERANGE;
	if (fetch->prefetch_ratio > 4 * PLAN_ONE)
		return ERANGE;

	/* The bandwidth: the cursor's and the larger of the plane's and the prefetch's must fit the return bandwidth. */
	pixel_bw = number_over(number_times(number_times(ratio, number_from(PLAN_PIXEL_BYTES)), number_from(input->width)),
	    work->line_us);
	total_bw = work->cursor_bw + number_most(work->read_bw, pixel_bw);
	if (total_bw > work->return_bw)
		return ERANGE;

	/* Succeeded: the prefetch fits. */
	return 0;
}

/*
 * Works out the time to allow in the vertical blank for the use of the
 * blank, and whether the memory clock may change: in the active area when
 * the line buffer, the detile buffer and the pipe's output buffers hide
 * its latency with 60 microseconds to spare (its watermark and the blank's
 * time then grow by 25), else in the blank when the blank allows it.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/display_mode_vba_20v2.c
 * (dml20v2_DISPCLKDPPCLKDCFCLKDeepSleepPrefetchParametersWatermarksAndPerformanceCalculation:
 * MinTTUVBlank, ActiveDRAMClockChangeLatencyMargin, DRAMClockChangeSupport).
 */
static void
plan_switch(
	const struct amdrdna_mode_plan_input *input,
	struct plan_work *work)
{
	plan_number line_buffer_us;
	plan_number output_lines;
	plan_number output_us;
	plan_number buffer_us;
	plan_number margin_us;
	plan_number other_us;
	int64_t buffer_lines;

	/* The blank's time: the watermarks the use of the blank needs, and the calculation. */
	work->ttu_us = work->urgent_us;
	if (work->vblank_use == 0U) {
		work->ttu_us = number_most(work->switch_us, number_most(work->enter_exit_us, work->urgent_us));
	} else if (work->vblank_use == 1U) {
		work->ttu_us = number_most(work->enter_exit_us, work->urgent_us);
	}
	work->ttu_us += work->calc_us;

	/* The line buffer's lines (at most 12) as time. */
	buffer_lines = PLAN_LINE_BUFFER_BITS / PLAN_LINE_BUFFER_PIXEL_BITS / input->width;
	if (buffer_lines > PLAN_LINE_BUFFER_LINES)
		buffer_lines = PLAN_LINE_BUFFER_LINES;
	line_buffer_us = number_times(number_from(buffer_lines), work->line_us);

	/* The pipe's output buffer (a line, half of one, or part of one) and the output processor's line as time. */
	output_lines = PLAN_ONE;
	if (input->width > 2U * PLAN_PIPE_BUFFER_PIXELS) {
		output_lines = number_ratio(PLAN_PIPE_BUFFER_PIXELS, input->width);
	} else if (input->width > PLAN_PIPE_BUFFER_PIXELS) {
		output_lines = number_ratio(1, 2);
	}
	output_us = number_times(work->line_us, output_lines + number_from(PLAN_OUTPUT_BUFFER_LINES));

	/* The detile buffer's whole lines, and the part line, as time. */
	buffer_us = number_times(work->buffer_whole_lines, work->line_us);
	buffer_us += number_times(work->buffer_lines - work->buffer_whole_lines, work->line_us);

	/* The margin, and where the memory clock may change. */
	margin_us = output_us + line_buffer_us + buffer_us - work->switch_us;
	other_us = number_most(work->enter_exit_us, work->urgent_us);
	if (margin_us > PLAN_ACTIVE_SWITCH_US * PLAN_ONE) {
		/* In the active area; the watermark and the blank's time grow. */
		work->switch_us += PLAN_ACTIVE_SWITCH_ADDED_US * PLAN_ONE;
		if (work->vblank_use == 0U && work->switch_us > other_us)
			work->ttu_us += PLAN_ACTIVE_SWITCH_ADDED_US * PLAN_ONE;
		work->memory_switch = 2U;
	} else if (work->vblank_use == 0U) {
		/* In the vertical blank. */
		work->memory_switch = 1U;
	} else {
		/* Never. */
		work->memory_switch = 0U;
	}
}

/*
 * Puts the plan into the registers' terms: the global sync, the
 * watermarks in ns, the request fields, and the deadline and arbitration
 * fields (in reference clock cycles, quarter lines and ratios with 19
 * fraction bits, as the registers take them).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/display_rq_dlg_calc_20v2.c
 * (extract_rq_sizing_regs, extract_rq_regs, dml20v2_rq_dlg_get_dlg_params,
 * calculate_ttu_cursor) and dml/dcn20/dcn20_fpu.c (dcn20_calculate_wm,
 * the watermarks in ns).
 */
static void
plan_fields(
	const struct amdrdna_mode_plan_input *input,
	const struct plan_work *work,
	struct amdrdna_mode_plan *plan)
{
	plan_number ratio;
	plan_number line_cycles;
	plan_number blank_lines;
	plan_number ready_line;
	plan_number thousand;
	uint32_t h_blank_end;
	uint32_t v_blank_start;
	uint32_t after_x;
	uint32_t sync_pixels;

	/* The use of the blank, the memory clock's change and the global sync (the widths cut to whole pixels). */
	ratio = work->reference_to_pixel;
	plan->vblank_use = work->vblank_use;
	plan->memory_switch = work->memory_switch;
	plan->start_lines = work->start_lines;
	plan->update_offset = work->fetch.update_offset;
	plan->update_width = (uint32_t)number_whole(work->fetch.update_width);
	plan->ready_offset = (uint32_t)number_whole(work->fetch.ready_offset);

	/* The watermarks in ns. */
	thousand = number_from(1000);
	plan->urgent_ns = (uint32_t)number_whole(number_times(work->urgent_us, thousand));
	plan->switch_ns = (uint32_t)number_whole(number_times(work->switch_us, thousand));
	plan->refresh_exit_ns = (uint32_t)number_whole(number_times(work->exit_us, thousand));
	plan->refresh_enter_exit_ns = (uint32_t)number_whole(number_times(work->enter_exit_us, thousand));
	plan->trip_ns = PLAN_URGENT_TENTHS * 100U;

	/* The request fields: the chunk sizes and groups as powers of two over their units, the page table rows over 8 lines. */
	plan->chunk_log = whole_log2(PLAN_CHUNK_BYTES) - 10U;
	plan->least_chunk_log = whole_log2(PLAN_LEAST_CHUNK_BYTES) - 8U + 1U;
	plan->meta_chunk_log = whole_log2(PLAN_META_CHUNK_BYTES) - 10U;
	plan->least_meta_chunk_log = whole_log2(PLAN_LEAST_META_CHUNK_BYTES) - 6U + 1U;
	plan->pte_group_log = whole_log2(PLAN_PTE_GROUP_BYTES) - 6U;
	plan->meta_pte_group_log = whole_log2(PLAN_PTE_GROUP_BYTES) - 6U;
	plan->pte_rows_log = whole_log2(work->pte_rows) - 3U;
	plan->swath_log = 0U;
	plan->data_expansion = 2U;
	plan->pte_expansion = 1U;
	plan->meta_expansion = 1U;
	plan->cursor_expansion = 1U;

	/* The timing in reference cycles: the reference a pixel, a line, the end of the horizontal blank. */
	h_blank_end = input->h_total - input->h_front - input->width;
	v_blank_start = input->v_total - input->v_front;
	plan->reference_to_pixel = (uint32_t)number_whole(number_times(ratio, number_from(1 << 19)));
	line_cycles = number_times(ratio, number_from(input->h_total));
	plan->line_cycles = (uint32_t)number_whole(number_times(line_cycles, number_from(1 << 8)));
	plan->blank_end_line = v_blank_start - input->height;
	plan->blank_end_cycles = (uint32_t)number_whole(number_times(number_from(h_blank_end), ratio));

	/* The next start: the blank's start and the blank's time, in quarter lines. */
	blank_lines = number_over(number_times(work->ttu_us, work->pixel_mhz), number_from(input->h_total));
	plan->next_start_quarters = (uint32_t)number_whole(number_times(number_from(v_blank_start) + blank_lines, number_from(4)));

	/* Ready after the line counter's zero when the start line less the sync falls in the blank's end. */
	sync_pixels = plan->ready_offset + plan->update_width + plan->update_offset;
	ready_line = number_from(work->start_lines) - number_ratio(sync_pixels, input->h_total);
	plan->ready_after_zero = 0U;
	if (ready_line <= (plan_number)plan->blank_end_line * PLAN_ONE)
		plan->ready_after_zero = 1U;

	/* The delay after the scaler, the prefetch and the cursor's and rows' requests in the blank. */
	after_x = (uint32_t)number_whole(work->fetch.after_x);
	plan->after_scaler_lines = (uint32_t)number_whole(work->fetch.after_y);
	plan->after_scaler_cycles = (uint32_t)number_whole(number_times(number_from(after_x), ratio));
	plan->prefetch_quarters = (uint32_t)number_whole(number_times(work->fetch.prefetch_lines, number_from(4)));
	plan->vm_vblank_quarters = (uint32_t)number_whole(number_times(work->fetch.vm_lines, number_from(4)));
	plan->row_vblank_quarters = (uint32_t)number_whole(number_times(work->fetch.row_lines, number_from(4)));
	plan->vm_flip_quarters = 0U;
	plan->row_flip_quarters = 0U;
	plan->prefetch_ratio = (uint32_t)number_whole(number_times(work->fetch.prefetch_ratio, number_from(1 << 19)));
	plan->prefetch_ratio_chroma = 1U << 19;

	/* The rows' requests in the blank and in a flip, per page table group and meta chunk. */
	plan->pte_group_vblank_cycles = (uint32_t)number_whole(number_over(number_times(work->fetch.row_lines, line_cycles),
	    number_from(work->pte_groups)));
	plan->meta_chunk_vblank_cycles = (uint32_t)number_whole(number_over(number_times(work->fetch.row_lines, line_cycles),
	    number_from(work->meta_chunks)));
	plan->pte_group_flip_cycles = 0U;
	plan->meta_chunk_flip_cycles = 0U;

	/* The rows in the active area: their lines in quarters and their groups' and chunks' cycles (at most 2^23 - 1). */
	plan->pte_row_quarters = work->pte_rows * 4U;
	plan->meta_row_quarters = 8U * 4U;
	plan->pte_group_cycles = (uint32_t)number_whole(number_over(number_times(number_from(work->pte_rows), line_cycles),
	    number_from(work->pte_groups)));
	if (plan->pte_group_cycles >= (1U << 23))
		plan->pte_group_cycles = (1U << 23) - 1U;
	plan->meta_chunk_cycles = (uint32_t)number_whole(number_over(number_times(number_from(8), line_cycles),
	    number_from(work->meta_chunks)));
	if (plan->meta_chunk_cycles >= (1U << 23))
		plan->meta_chunk_cycles = (1U << 23) - 1U;

	/* A line's delivery in the prefetch and in the active area. */
	plan->line_delivery_prefetch_cycles = (uint32_t)number_whole(plan_delivery(work, work->fetch.prefetch_ratio,
	    input->width, work->swath_bound, 1U));
	plan->line_delivery_cycles = (uint32_t)number_whole(plan_delivery(work, PLAN_ONE, input->width,
	    work->swath_bound, 1U));
	plan->cursor_chunk_adjust = PLAN_CURSOR_CHUNK_ADJUST;
	plan->request_limit = PLAN_REQUEST_LIMIT;

	/* A request's delivery in the prefetch and in the active area, in 1/1024 cycles. */
	plan->request_delivery_prefetch_cycles = (uint32_t)number_whole(number_times(plan_delivery(work,
	    work->fetch.prefetch_ratio, input->width, input->width, work->requests), number_from(1024)));
	plan->request_delivery_cycles = (uint32_t)number_whole(number_times(plan_delivery(work, PLAN_ONE, input->width,
	    input->width, work->requests), number_from(1024)));

	/* The cursor's: 256 pixels in four 256-byte requests. */
	plan->cursor_delivery_prefetch_cycles = (uint32_t)number_whole(number_times(plan_delivery(work,
	    work->fetch.prefetch_ratio, PLAN_CURSOR_PIXELS, PLAN_CURSOR_PIXELS,
	    PLAN_CURSOR_PIXELS * PLAN_PIXEL_BYTES / PLAN_CURSOR_REQUEST_BYTES), number_from(1024)));
	plan->cursor_delivery_cycles = (uint32_t)number_whole(number_times(plan_delivery(work, PLAN_ONE,
	    PLAN_CURSOR_PIXELS, PLAN_CURSOR_PIXELS, PLAN_CURSOR_PIXELS * PLAN_PIXEL_BYTES / PLAN_CURSOR_REQUEST_BYTES),
	    number_from(1024)));

	/* The arbitration: the levels and the blank's time in reference cycles. */
	plan->low_level_mark = 0U;
	plan->high_level_mark = (uint32_t)number_whole(number_times(number_from(4), line_cycles));
	plan->flip_level = PLAN_FLIP_LEVEL;
	plan->fixed_level = PLAN_FIXED_LEVEL;
	plan->vblank_ttu_cycles = (uint32_t)number_whole(number_times(work->ttu_us,
	    number_ratio(input->reference_khz, 1000)));
}

/*
 * Gives the reference cycles to deliver a width in some parts: the shown
 * width at the pixel rate when the ratio is 1 at most, else the fetched
 * width at the scaler's two pixels a pipe clock cycle.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/display_rq_dlg_calc_20v2.c
 * (get_refcyc_per_delivery; calculate_ttu_cursor), unscaled, one pipe.
 */
static plan_number
plan_delivery(
	const struct plan_work *work,
	plan_number ratio,
	uint32_t pixel_width,
	uint32_t scaler_width,
	uint32_t parts)
{
	plan_number reference_mhz;
	plan_number cycles;

	/* The reference clock in MHz. */
	reference_mhz = number_times(work->reference_to_pixel, work->pixel_mhz);

	/* At the scaler's rate when the prefetch reads faster than the pixels go, else at the pixel rate. */
	if (ratio > PLAN_ONE) {
		cycles = number_over(number_times(reference_mhz, number_from(scaler_width)), number_times(number_from(2), work->pipe_mhz));
	} else {
		cycles = number_times(work->reference_to_pixel, number_from(pixel_width));
	}
	cycles = number_over(cycles, number_from(parts));

	/* Succeeded: the cycles a part. */
	return cycles;
}

/*
 * The memory clock change's latency the plan works with, in tenths of a
 * microsecond: the input's, or the latency of a change between the memory
 * clock's levels when the input gives none.
 */
static uint32_t
plan_switch_tenths(
	const struct amdrdna_mode_plan_input *input)
{
	/* The input's latency. */
	if (input->switch_tenths != 0U)
		return input->switch_tenths;

	/* The change between levels. */
	return PLAN_SWITCH_TENTHS;
}
