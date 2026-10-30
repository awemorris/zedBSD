/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fractional sample interpolation of the software H.264 decoder
 * (8.4.2.2).
 *
 * A block whose reference samples all lie inside the plane is read from
 * the plane directly; any other block first copies its samples, with the
 * positions clamped, into a window and is then read from the window the
 * same way.  The luma block is filtered a whole block at a time: the
 * half samples a position of Table 8-12 needs are made for the block,
 * then averaged when the position is a quarter one.
 */

#include "h264sw-interp.h"

#include <stddef.h>
#include <string.h>

/* The largest block a call predicts: a 16x16 luma block of a macroblock. */
#define INTERP_MAX_BLOCK 16U

/*
 * The reference samples a luma block reads: the six-tap filter reaches two
 * samples before the block and three after it (the samples A to U of
 * Figure 8-4 lie between -2 and +3 of G), so a block of N samples reads
 * N + 5 samples on a side.
 */
#define INTERP_MARGIN_BEFORE 2U
#define INTERP_MARGIN_AFTER 3U
#define INTERP_MARGIN_TOTAL 5U
#define INTERP_WINDOW_SIDE (INTERP_MAX_BLOCK + INTERP_MARGIN_TOTAL)

/* The fractional parts of a luma vector have two bits (8-225, 8-226) and of a chroma vector three (8-229, 8-230). */
#define INTERP_LUMA_FRAC_MASK 3U
#define INTERP_CHROMA_FRAC_MASK 7U

/* The largest sample value of an 8-bit picture, the upper bound of Clip1Y in 8-243, 8-244 and 8-247. */
#define INTERP_SAMPLE_MAX 255

/* Forward declarations of the static functions. */
static size_t interp_clamp_coordinate(int64_t value, unsigned size);
static int interp_clip_round_shift(int value, int add, unsigned shift);
static int interp_filter6(int s0, int s1, int s2, int s3, int s4, int s5);
static int interp_inside(unsigned plane_width, unsigned plane_height, int x, int y, unsigned width, unsigned height, unsigned before, unsigned after);
static void interp_fetch_window(const uint8_t *plane, size_t stride, unsigned plane_width, unsigned plane_height, int x, int y, unsigned width, unsigned height, uint8_t *window);
static void interp_block_full(const uint8_t *src, size_t src_stride, unsigned width, unsigned height, uint8_t *dst, size_t dst_stride);
static void interp_block_horizontal(const uint8_t *src, size_t src_stride, unsigned width, unsigned height, uint8_t *dst, size_t dst_stride);
static void interp_block_vertical(const uint8_t *src, size_t src_stride, unsigned width, unsigned height, uint8_t *dst, size_t dst_stride);
static void interp_block_centre(const uint8_t *src, size_t src_stride, unsigned width, unsigned height, uint8_t *dst, size_t dst_stride);
static void interp_block_average(const uint8_t *first, size_t first_stride, const uint8_t *second, size_t second_stride, unsigned width, unsigned height, uint8_t *dst, size_t dst_stride);
static void interp_luma_position(const uint8_t *src, size_t src_stride, unsigned position, unsigned width, unsigned height, uint8_t *out, size_t out_stride);
static void interp_chroma_inside(const uint8_t *plane, size_t stride, int x, int y, const int *weight, unsigned width, unsigned height, uint8_t *out, size_t out_stride);
static void interp_chroma_clamped(const uint8_t *plane, size_t stride, unsigned plane_width, unsigned plane_height, int x, int y, const int *weight, unsigned width, unsigned height, uint8_t *out, size_t out_stride);

/*
 * Predicts a block of luma samples from a reference plane (8.4.2.2.1).
 *
 * A call that cannot describe a block (a missing plane or output, an
 * empty plane, a block that is empty or larger than 16 by 16) writes
 * nothing.  The fractional parts are taken modulo 4.
 */
void
h264sw_interp_luma(
	const uint8_t *plane,
	size_t stride,
	unsigned plane_width,
	unsigned plane_height,
	int x,
	int y,
	unsigned frac_x,
	unsigned frac_y,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	uint8_t window[INTERP_WINDOW_SIDE * INTERP_WINDOW_SIDE];
	const uint8_t *src;
	size_t src_stride;
	int inside;

	/* Refuses a block without a source or a destination. */
	if (plane == NULL)
		return;
	if (out == NULL)
		return;

	/* Refuses a reference plane without samples. */
	if (plane_width == 0U || plane_height == 0U)
		return;

	/* Refuses a block that is empty or larger than a macroblock. */
	if (width == 0U || height == 0U)
		return;
	if (width > INTERP_MAX_BLOCK || height > INTERP_MAX_BLOCK)
		return;

	/* Keeps the fractional parts to the quarter-sample positions of Table 8-12. */
	frac_x &= INTERP_LUMA_FRAC_MASK;
	frac_y &= INTERP_LUMA_FRAC_MASK;

	/*
	 * Points at the sample G of the block's first position: in the plane
	 * when every sample the filter reaches is inside it (no position needs
	 * the clamp of 8-239, 8-240), else in a window of the clamped samples.
	 */
	inside = interp_inside(plane_width, plane_height, x, y, width, height, INTERP_MARGIN_BEFORE, INTERP_MARGIN_AFTER);
	if (inside) {
		src = plane + (size_t)y * stride + (size_t)x;
		src_stride = stride;
	} else {
		interp_fetch_window(plane, stride, plane_width, plane_height, x, y, width, height, window);
		src = window + INTERP_MARGIN_BEFORE * INTERP_WINDOW_SIDE + INTERP_MARGIN_BEFORE;
		src_stride = INTERP_WINDOW_SIDE;
	}

	/* Derives the block's samples at the position xFracL, yFracL. */
	interp_luma_position(src, src_stride, frac_x * 4U + frac_y, width, height, out, out_stride);
}

/*
 * Predicts a block of chroma samples from a reference plane (8.4.2.2.2).
 *
 * A call that cannot describe a block (a missing plane or output, an
 * empty plane, a block that is empty or larger than 16 by 16) writes
 * nothing.  The fractional parts are taken modulo 8.
 */
void
h264sw_interp_chroma(
	const uint8_t *plane,
	size_t stride,
	unsigned plane_width,
	unsigned plane_height,
	int x,
	int y,
	unsigned frac_x,
	unsigned frac_y,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	int weight[4];
	int inside;

	/* Refuses a block without a source or a destination. */
	if (plane == NULL)
		return;
	if (out == NULL)
		return;

	/* Refuses a reference plane without samples. */
	if (plane_width == 0U || plane_height == 0U)
		return;

	/* Refuses a block that is empty or larger than a macroblock. */
	if (width == 0U || height == 0U)
		return;
	if (width > INTERP_MAX_BLOCK || height > INTERP_MAX_BLOCK)
		return;

	/* Keeps the fractional parts to the eighth-sample positions. */
	frac_x &= INTERP_CHROMA_FRAC_MASK;
	frac_y &= INTERP_CHROMA_FRAC_MASK;

	/* Derives the four bilinear weights of 8-270: each corner is weighted by the nearness to it in both directions. */
	weight[0] = (int)(8U - frac_x) * (int)(8U - frac_y);
	weight[1] = (int)frac_x * (int)(8U - frac_y);
	weight[2] = (int)(8U - frac_x) * (int)frac_y;
	weight[3] = (int)frac_x * (int)frac_y;

	/* Reads the plane directly when every A, B, C and D of the block is inside it, else clamps each position (8-262 to 8-269). */
	inside = interp_inside(plane_width, plane_height, x, y, width, height, 0U, 1U);
	if (inside) {
		interp_chroma_inside(plane, stride, x, y, weight, width, height, out, out_stride);
	} else {
		interp_chroma_clamped(plane, stride, plane_width, plane_height, x, y, weight, width, height, out, out_stride);
	}
}

/* Clamps a sample coordinate into 0..size-1 (Clip3 of 8-239, 8-240, 8-262 to 8-269); size is at least 1. */
static size_t
interp_clamp_coordinate(
	int64_t value,
	unsigned size)
{
	/* Clamps a coordinate before the plane to its first sample. */
	if (value < 0)
		return 0;

	/* Clamps a coordinate beyond the plane to its last sample. */
	if (value > (int64_t)size - 1)
		return (size_t)size - 1U;

	/* Keeps a coordinate inside the plane. */
	return (size_t)value;
}

/*
 * Rounds, shifts and clips an intermediate filter value to a sample (Clip1Y( ( v + add ) >> shift ), 8-243, 8-244, 8-247).
 *
 * A negative sum clips to 0 whatever its magnitude, so no negative value is shifted.
 */
static int
interp_clip_round_shift(
	int value,
	int add,
	unsigned shift)
{
	int sum;
	int scaled;

	/* Clips a negative sum to the lowest sample value. */
	sum = value + add;
	if (sum < 0)
		return 0;

	/* Clips a sum above the highest sample value. */
	scaled = sum >> shift;
	if (scaled > INTERP_SAMPLE_MAX)
		return INTERP_SAMPLE_MAX;

	/* Keeps the shifted sum, which is already a sample value. */
	return scaled;
}

/* Applies the six-tap filter with the taps ( 1, -5, 20, 20, -5, 1 ) of 8.4.2.2.1 (8-241, 8-242) to six values. */
static int
interp_filter6(
	int s0,
	int s1,
	int s2,
	int s3,
	int s4,
	int s5)
{
	int sum;

	/* Weights the two samples nearest the half-sample position by 20, the next two by -5 and the outer two by 1. */
	sum = s0 - 5 * s1 + 20 * s2 + 20 * s3 - 5 * s4 + s5;

	/* Reports the unrounded filter value. */
	return sum;
}

/*
 * Tells whether a block's reference samples lie inside the plane: the
 * columns x - before .. x + width - 1 + after and the rows y - before ..
 * y + height - 1 + after.  Returns 1 when they do, so no position needs a
 * clamp, else 0.
 */
static int
interp_inside(
	unsigned plane_width,
	unsigned plane_height,
	int x,
	int y,
	unsigned width,
	unsigned height,
	unsigned before,
	unsigned after)
{
	int64_t first_x;
	int64_t first_y;
	int64_t last_x;
	int64_t last_y;

	/* The reach of the block in each direction, in 64 bits so that no vector overflows it. */
	first_x = (int64_t)x - (int64_t)before;
	first_y = (int64_t)y - (int64_t)before;
	last_x = (int64_t)x + (int64_t)width - 1 + (int64_t)after;
	last_y = (int64_t)y + (int64_t)height - 1 + (int64_t)after;

	/* A reach before the first column or row is outside. */
	if (first_x < 0 || first_y < 0)
		return 0;

	/* A reach beyond the last column or row is outside. */
	if (last_x > (int64_t)plane_width - 1 || last_y > (int64_t)plane_height - 1)
		return 0;

	/* Every sample the block reads is inside the plane. */
	return 1;
}

/*
 * Reads the reference samples around a block into a window.
 *
 * window holds width + 5 by height + 5 samples with rows of
 * INTERP_WINDOW_SIDE bytes; window[ r ][ c ] is the reference sample at
 * ( x - 2 + c, y - 2 + r ) clamped to the plane, so the samples A to U of
 * Figure 8-4 for every output position are inside it.  No read leaves
 * the plane.
 */
static void
interp_fetch_window(
	const uint8_t *plane,
	size_t stride,
	unsigned plane_width,
	unsigned plane_height,
	int x,
	int y,
	unsigned width,
	unsigned height,
	uint8_t *window)
{
	size_t source_x[INTERP_WINDOW_SIDE];
	const uint8_t *line;
	unsigned row;
	unsigned col;
	size_t source_y;

	/* Clamps each column of Table 8-11's offsets to the plane once for all rows (8-239). */
	for (col = 0; col < width + INTERP_MARGIN_TOTAL; col++)
		source_x[col] = interp_clamp_coordinate((int64_t)x - (int64_t)INTERP_MARGIN_BEFORE + (int64_t)col, plane_width);

	/* Copies each window row from the clamped reference row. */
	for (row = 0; row < height + INTERP_MARGIN_TOTAL; row++) {
		/* Clamps the row to the plane (8-240). */
		source_y = interp_clamp_coordinate((int64_t)y - (int64_t)INTERP_MARGIN_BEFORE + (int64_t)row, plane_height);
		line = plane + source_y * stride;

		/* Copies each sample of the row from its clamped column. */
		for (col = 0; col < width + INTERP_MARGIN_TOTAL; col++)
			window[row * INTERP_WINDOW_SIDE + col] = line[source_x[col]];
	}
}

/* Copies the full samples G of a block (Table 8-12, xFracL and yFracL 0). */
static void
interp_block_full(
	const uint8_t *src,
	size_t src_stride,
	unsigned width,
	unsigned height,
	uint8_t *dst,
	size_t dst_stride)
{
	unsigned row;

	/* Copies each row. */
	for (row = 0; row < height; row++)
		memcpy(dst + (size_t)row * dst_stride, src + (size_t)row * src_stride, width);
}

/*
 * Makes the horizontal half samples b of a block (b1 of 8-241 rounded by
 * 8-243): for each position, E F G H I J of Figure 8-4 filtered.  Called
 * one row lower it makes the samples s (8-248).
 */
static void
interp_block_horizontal(
	const uint8_t *src,
	size_t src_stride,
	unsigned width,
	unsigned height,
	uint8_t *dst,
	size_t dst_stride)
{
	const uint8_t *line;
	const uint8_t *g;
	uint8_t *out;
	unsigned row;
	unsigned col;
	int raw;

	/* Filters each row. */
	for (row = 0; row < height; row++) {
		line = src + (size_t)row * src_stride;
		out = dst + (size_t)row * dst_stride;

		/* Filters the six samples from two before to three after each position, and rounds and clips. */
		for (col = 0; col < width; col++) {
			g = line + col;
			raw = interp_filter6(g[-2], g[-1], g[0], g[1], g[2], g[3]);
			out[col] = (uint8_t)interp_clip_round_shift(raw, 16, 5);
		}
	}
}

/*
 * Makes the vertical half samples h of a block (h1 of 8-242 rounded by
 * 8-244): for each position, A C G M R T of Figure 8-4 filtered.  Called
 * one column to the right it makes the samples m (8-249).
 */
static void
interp_block_vertical(
	const uint8_t *src,
	size_t src_stride,
	unsigned width,
	unsigned height,
	uint8_t *dst,
	size_t dst_stride)
{
	const uint8_t *line;
	const uint8_t *g;
	uint8_t *out;
	ptrdiff_t step;
	unsigned row;
	unsigned col;
	int raw;

	/* Filters each row of positions; step moves one row. */
	step = (ptrdiff_t)src_stride;
	for (row = 0; row < height; row++) {
		line = src + (size_t)row * src_stride;
		out = dst + (size_t)row * dst_stride;

		/* Filters the six samples from two rows above to three rows below each position, and rounds and clips. */
		for (col = 0; col < width; col++) {
			g = line + col;
			raw = interp_filter6(g[-2 * step], g[-step], g[0], g[step], g[2 * step], g[3 * step]);
			out[col] = (uint8_t)interp_clip_round_shift(raw, 16, 5);
		}
	}
}

/*
 * Makes the centre half samples j of a block (8-245, 8-247).
 *
 * The vertical intermediate values (h1 of 8-242, unrounded) of each row
 * are made once for the columns two to the left through three to the
 * right of the row's positions; j1 of each position is the six-tap filter
 * of six of them (cc, dd, h1, m1, ee, ff), rounded with 512 and a shift
 * of 10.  The horizontal form of 8-246 gives the same value.
 */
static void
interp_block_centre(
	const uint8_t *src,
	size_t src_stride,
	unsigned width,
	unsigned height,
	uint8_t *dst,
	size_t dst_stride)
{
	int column[INTERP_WINDOW_SIDE];
	const uint8_t *line;
	const uint8_t *g;
	uint8_t *out;
	ptrdiff_t step;
	unsigned row;
	unsigned col;
	int raw;

	/* Filters each row of positions; step moves one row. */
	step = (ptrdiff_t)src_stride;
	for (row = 0; row < height; row++) {
		/* The row's first intermediate value is the column two to the left of its first position. */
		line = src + (size_t)row * src_stride - INTERP_MARGIN_BEFORE;
		out = dst + (size_t)row * dst_stride;

		/* Derives the vertical intermediate values of the row's width + 5 columns. */
		for (col = 0; col < width + INTERP_MARGIN_TOTAL; col++) {
			g = line + col;
			column[col] = interp_filter6(g[-2 * step], g[-step], g[0], g[step], g[2 * step], g[3 * step]);
		}

		/* Filters six intermediate values around each position horizontally, and rounds and clips. */
		for (col = 0; col < width; col++) {
			raw = interp_filter6(column[col], column[col + 1U], column[col + 2U], column[col + 3U], column[col + 4U], column[col + 5U]);
			out[col] = (uint8_t)interp_clip_round_shift(raw, 512, 10);
		}
	}
}

/* Averages two blocks with upward rounding, the form of 8-250 to 8-261. */
static void
interp_block_average(
	const uint8_t *first,
	size_t first_stride,
	const uint8_t *second,
	size_t second_stride,
	unsigned width,
	unsigned height,
	uint8_t *dst,
	size_t dst_stride)
{
	const uint8_t *a;
	const uint8_t *b;
	uint8_t *out;
	unsigned row;
	unsigned col;

	/* Averages each row. */
	for (row = 0; row < height; row++) {
		a = first + (size_t)row * first_stride;
		b = second + (size_t)row * second_stride;
		out = dst + (size_t)row * dst_stride;

		/* Adds one before the halving so that a half rounds up. */
		for (col = 0; col < width; col++)
			out[col] = (uint8_t)(((unsigned)a[col] + (unsigned)b[col] + 1U) >> 1);
	}
}

/*
 * Derives the predicted samples of a block at one fractional position (Table 8-12).
 *
 * src points at G of the block's first position and reaches two samples
 * before and three after the block in both directions.  position is
 * xFracL * 4 + yFracL.  H is the sample to the right of G and M the
 * sample below it; s is the b of the row below and m the h of the column
 * to the right.
 */
static void
interp_luma_position(
	const uint8_t *src,
	size_t src_stride,
	unsigned position,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	uint8_t first[INTERP_MAX_BLOCK * INTERP_MAX_BLOCK];
	uint8_t second[INTERP_MAX_BLOCK * INTERP_MAX_BLOCK];

	/*
	 * Selects the sample of the position by Table 8-12, indexed by xFracL
	 * and yFracL; the quarter positions average two of G, H, M and the
	 * half samples b, h, j, m, s with upward rounding (8-250 to 8-261).
	 *
	 * Values referenced: ITU-T H.264 (08/2021) Table 8-12.
	 *
	 *   xFracL  0 0 0 0 1 1 1 1 2 2 2 2 3 3 3 3
	 *   yFracL  0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3
	 *   sample  G d h n a e i p b f j q c g k r
	 */
	switch (position) {
	case 1:
		/* d = ( G + h + 1 ) >> 1 (8-252). */
		interp_block_vertical(src, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_average(src, src_stride, first, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	case 2:
		/* h: the vertical half position (8-244). */
		interp_block_vertical(src, src_stride, width, height, out, out_stride);
		break;
	case 3:
		/* n = ( M + h + 1 ) >> 1 (8-253). */
		interp_block_vertical(src, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_average(src + src_stride, src_stride, first, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	case 4:
		/* a = ( G + b + 1 ) >> 1 (8-250). */
		interp_block_horizontal(src, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_average(src, src_stride, first, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	case 5:
		/* e = ( b + h + 1 ) >> 1 (8-258). */
		interp_block_horizontal(src, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_vertical(src, src_stride, width, height, second, INTERP_MAX_BLOCK);
		interp_block_average(first, INTERP_MAX_BLOCK, second, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	case 6:
		/* i = ( h + j + 1 ) >> 1 (8-255). */
		interp_block_vertical(src, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_centre(src, src_stride, width, height, second, INTERP_MAX_BLOCK);
		interp_block_average(first, INTERP_MAX_BLOCK, second, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	case 7:
		/* p = ( h + s + 1 ) >> 1 (8-260). */
		interp_block_vertical(src, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_horizontal(src + src_stride, src_stride, width, height, second, INTERP_MAX_BLOCK);
		interp_block_average(first, INTERP_MAX_BLOCK, second, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	case 8:
		/* b: the horizontal half position (8-243). */
		interp_block_horizontal(src, src_stride, width, height, out, out_stride);
		break;
	case 9:
		/* f = ( b + j + 1 ) >> 1 (8-254). */
		interp_block_horizontal(src, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_centre(src, src_stride, width, height, second, INTERP_MAX_BLOCK);
		interp_block_average(first, INTERP_MAX_BLOCK, second, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	case 10:
		/* j: the centre half position (8-247). */
		interp_block_centre(src, src_stride, width, height, out, out_stride);
		break;
	case 11:
		/* q = ( j + s + 1 ) >> 1 (8-257). */
		interp_block_centre(src, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_horizontal(src + src_stride, src_stride, width, height, second, INTERP_MAX_BLOCK);
		interp_block_average(first, INTERP_MAX_BLOCK, second, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	case 12:
		/* c = ( H + b + 1 ) >> 1 (8-251). */
		interp_block_horizontal(src, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_average(src + 1, src_stride, first, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	case 13:
		/* g = ( b + m + 1 ) >> 1 (8-259). */
		interp_block_horizontal(src, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_vertical(src + 1, src_stride, width, height, second, INTERP_MAX_BLOCK);
		interp_block_average(first, INTERP_MAX_BLOCK, second, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	case 14:
		/* k = ( j + m + 1 ) >> 1 (8-256). */
		interp_block_centre(src, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_vertical(src + 1, src_stride, width, height, second, INTERP_MAX_BLOCK);
		interp_block_average(first, INTERP_MAX_BLOCK, second, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	case 15:
		/* r = ( m + s + 1 ) >> 1 (8-261). */
		interp_block_vertical(src + 1, src_stride, width, height, first, INTERP_MAX_BLOCK);
		interp_block_horizontal(src + src_stride, src_stride, width, height, second, INTERP_MAX_BLOCK);
		interp_block_average(first, INTERP_MAX_BLOCK, second, INTERP_MAX_BLOCK, width, height, out, out_stride);
		break;
	default:
		/* G (position 0): the integer position is copied. */
		interp_block_full(src, src_stride, width, height, out, out_stride);
		break;
	}
}

/*
 * Predicts a chroma block whose samples A, B, C and D all lie inside the plane (8-270 without a clamp).
 *
 * weight holds the weights of A, B, C and D; the sum is at most 255 * 64
 * + 32, so it needs no clip.
 */
static void
interp_chroma_inside(
	const uint8_t *plane,
	size_t stride,
	int x,
	int y,
	const int *weight,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	const uint8_t *upper;
	const uint8_t *lower;
	uint8_t *dst;
	unsigned row;
	unsigned col;
	int sum;

	/* Combines each row with the row below it. */
	for (row = 0; row < height; row++) {
		upper = plane + ((size_t)y + row) * stride + (size_t)x;
		lower = upper + stride;
		dst = out + (size_t)row * out_stride;

		/* Combines A, B, C and D of Figure 8-5 around each position with rounding by 8-270. */
		for (col = 0; col < width; col++) {
			sum = weight[0] * upper[col] + weight[1] * upper[col + 1U] + weight[2] * lower[col] + weight[3] * lower[col + 1U] + 32;
			dst[col] = (uint8_t)(sum >> 6);
		}
	}
}

/*
 * Predicts a chroma block with each of A, B, C and D at its clamped position (8-262 to 8-270).
 *
 * weight holds the weights of A, B, C and D.
 */
static void
interp_chroma_clamped(
	const uint8_t *plane,
	size_t stride,
	unsigned plane_width,
	unsigned plane_height,
	int x,
	int y,
	const int *weight,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	unsigned row;
	unsigned col;
	size_t x_a;
	size_t x_b;
	size_t y_a;
	size_t y_c;
	int sum;

	/* Derives each predicted sample from the four reference samples around it. */
	for (row = 0; row < height; row++) {
		/* Clamps the rows of A and B, and of C and D, to the plane (8-266 to 8-269). */
		y_a = interp_clamp_coordinate((int64_t)y + (int64_t)row, plane_height);
		y_c = interp_clamp_coordinate((int64_t)y + (int64_t)row + 1, plane_height);

		/* Derives the row's samples. */
		for (col = 0; col < width; col++) {
			/* Clamps the columns of A and C, and of B and D, to the plane (8-262 to 8-265). */
			x_a = interp_clamp_coordinate((int64_t)x + (int64_t)col, plane_width);
			x_b = interp_clamp_coordinate((int64_t)x + (int64_t)col + 1, plane_width);

			/* Combines A, B, C and D of Figure 8-5 with rounding by 8-270; the sum is at most 255 * 64 + 32, so it needs no clip. */
			sum = weight[0] * plane[y_a * stride + x_a] + weight[1] * plane[y_a * stride + x_b] +
			    weight[2] * plane[y_c * stride + x_a] + weight[3] * plane[y_c * stride + x_b] + 32;
			out[(size_t)row * out_stride + col] = (uint8_t)(sum >> 6);
		}
	}
}
