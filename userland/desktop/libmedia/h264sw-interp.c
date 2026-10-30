/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fractional sample interpolation of the software H.264 decoder
 * (8.4.2.2).
 */

#include "h264sw-interp.h"

#include <stddef.h>

/* The largest block a call predicts: a 16x16 luma block of a macroblock. */
#define INTERP_MAX_BLOCK 16U

/*
 * The reference samples a luma block reads: the six-tap filter reaches two
 * samples before the block and three after it (the samples A to U of
 * Figure 8-4 lie between -2 and +3 of G), so a block of N samples reads
 * N + 5 samples on a side.
 */
#define INTERP_MARGIN_BEFORE 2U
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
static int interp_average(int first, int second);
static void interp_fetch_window(const uint8_t *plane, size_t stride, unsigned plane_width, unsigned plane_height, int x, int y, unsigned width, unsigned height, uint8_t *window);
static int interp_full(const uint8_t *window, unsigned row, unsigned col);
static int interp_half_horizontal_raw(const uint8_t *window, unsigned row, unsigned col);
static int interp_half_vertical_raw(const uint8_t *window, unsigned row, unsigned col);
static int interp_half_horizontal(const uint8_t *window, unsigned row, unsigned col);
static int interp_half_vertical(const uint8_t *window, unsigned row, unsigned col);
static int interp_half_centre(const uint8_t *window, unsigned row, unsigned col);
static int interp_luma_sample(const uint8_t *window, unsigned row, unsigned col, unsigned frac_x, unsigned frac_y);

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
	unsigned row;
	unsigned col;
	int sample;

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

	/* Reads every reference sample the block can need, with the positions clamped to the plane (8-239, 8-240). */
	interp_fetch_window(plane, stride, plane_width, plane_height, x, y, width, height, window);

	/* Derives each predicted sample from the window; the sample G of a position sits two rows and two columns inside it. */
	for (row = 0; row < height; row++) {
		for (col = 0; col < width; col++) {
			sample = interp_luma_sample(window, row + INTERP_MARGIN_BEFORE, col + INTERP_MARGIN_BEFORE, frac_x, frac_y);
			out[(size_t)row * out_stride + col] = (uint8_t)sample;
		}
	}
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
	unsigned row;
	unsigned col;
	size_t x_a;
	size_t x_b;
	size_t y_a;
	size_t y_c;
	int sample_a;
	int sample_b;
	int sample_c;
	int sample_d;
	int weight_a;
	int weight_b;
	int weight_c;
	int weight_d;
	int sum;

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
	weight_a = (int)(8U - frac_x) * (int)(8U - frac_y);
	weight_b = (int)frac_x * (int)(8U - frac_y);
	weight_c = (int)(8U - frac_x) * (int)frac_y;
	weight_d = (int)frac_x * (int)frac_y;

	/* Derives each predicted sample from the four reference samples around it. */
	for (row = 0; row < height; row++) {
		for (col = 0; col < width; col++) {
			/* Clamps the columns of A and C, and of B and D, to the plane (8-262 to 8-265). */
			x_a = interp_clamp_coordinate((int64_t)x + (int64_t)col, plane_width);
			x_b = interp_clamp_coordinate((int64_t)x + (int64_t)col + 1, plane_width);

			/* Clamps the rows of A and B, and of C and D, to the plane (8-266 to 8-269). */
			y_a = interp_clamp_coordinate((int64_t)y + (int64_t)row, plane_height);
			y_c = interp_clamp_coordinate((int64_t)y + (int64_t)row + 1, plane_height);

			/* Reads the four full-sample neighbours A, B, C and D of Figure 8-5. */
			sample_a = plane[y_a * stride + x_a];
			sample_b = plane[y_a * stride + x_b];
			sample_c = plane[y_c * stride + x_a];
			sample_d = plane[y_c * stride + x_b];

			/* Combines them with rounding by 8-270; the sum is at most 255 * 64 + 32, so it needs no clip. */
			sum = weight_a * sample_a + weight_b * sample_b + weight_c * sample_c + weight_d * sample_d + 32;
			out[(size_t)row * out_stride + col] = (uint8_t)(sum >> 6);
		}
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

/* Averages two samples with upward rounding, the form of 8-250 to 8-261. */
static int
interp_average(
	int first,
	int second)
{
	/* Adds one before the halving so that a half rounds up. */
	return (first + second + 1) >> 1;
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
	unsigned row;
	unsigned col;
	size_t source_x;
	size_t source_y;

	/* Copies each window row from the clamped reference row. */
	for (row = 0; row < height + INTERP_MARGIN_TOTAL; row++) {
		/* Clamps the row of Table 8-11's offsets to the plane (8-240). */
		source_y = interp_clamp_coordinate((int64_t)y - (int64_t)INTERP_MARGIN_BEFORE + (int64_t)row, plane_height);

		/* Copies each sample of the row from the clamped column (8-239). */
		for (col = 0; col < width + INTERP_MARGIN_TOTAL; col++) {
			source_x = interp_clamp_coordinate((int64_t)x - (int64_t)INTERP_MARGIN_BEFORE + (int64_t)col, plane_width);
			window[row * INTERP_WINDOW_SIDE + col] = plane[source_y * stride + source_x];
		}
	}
}

/* Reads the full-sample value at a window position. */
static int
interp_full(
	const uint8_t *window,
	unsigned row,
	unsigned col)
{
	/* Reports the window sample. */
	return window[row * INTERP_WINDOW_SIDE + col];
}

/*
 * Filters the row of a window position horizontally (b1 of 8-241; aa, bb, gg, s1 and hh in the same manner).
 *
 * The result is for the half sample to the right of the full sample at ( row, col ): E F G H I J of Figure 8-4 for G.
 */
static int
interp_half_horizontal_raw(
	const uint8_t *window,
	unsigned row,
	unsigned col)
{
	const uint8_t *line;
	int filtered;

	/* Filters the six samples of the row from two before to three after the position. */
	line = window + row * INTERP_WINDOW_SIDE + col;
	filtered = interp_filter6(line[-2], line[-1], line[0], line[1], line[2], line[3]);

	/* Reports the unrounded value b1. */
	return filtered;
}

/*
 * Filters the column of a window position vertically (h1 of 8-242; cc, dd, ee, m1 and ff in the same manner).
 *
 * The result is for the half sample below the full sample at ( row, col ): A C G M R T of Figure 8-4 for G.
 */
static int
interp_half_vertical_raw(
	const uint8_t *window,
	unsigned row,
	unsigned col)
{
	const uint8_t *line;
	int filtered;

	/* Filters the six samples of the column from two before to three after the position. */
	line = window + row * INTERP_WINDOW_SIDE + col;
	filtered = interp_filter6(
		line[-2 * (int)INTERP_WINDOW_SIDE],
		line[-1 * (int)INTERP_WINDOW_SIDE],
		line[0],
		line[1 * (int)INTERP_WINDOW_SIDE],
		line[2 * (int)INTERP_WINDOW_SIDE],
		line[3 * (int)INTERP_WINDOW_SIDE]);

	/* Reports the unrounded value h1. */
	return filtered;
}

/* Gives the half sample b to the right of the position, or s when called for the row below (8-243, 8-248). */
static int
interp_half_horizontal(
	const uint8_t *window,
	unsigned row,
	unsigned col)
{
	int raw;
	int rounded;

	/* Rounds the horizontal filter value to a sample. */
	raw = interp_half_horizontal_raw(window, row, col);
	rounded = interp_clip_round_shift(raw, 16, 5);

	/* Reports the half sample. */
	return rounded;
}

/* Gives the half sample h below the position, or m when called for the column to the right (8-244, 8-249). */
static int
interp_half_vertical(
	const uint8_t *window,
	unsigned row,
	unsigned col)
{
	int raw;
	int rounded;

	/* Rounds the vertical filter value to a sample. */
	raw = interp_half_vertical_raw(window, row, col);
	rounded = interp_clip_round_shift(raw, 16, 5);

	/* Reports the half sample. */
	return rounded;
}

/*
 * Gives the centre half sample j of the position (8-245, 8-247).
 *
 * j1 is the six-tap filter of the vertical intermediate values of the
 * columns two to the left through three to the right (cc, dd, h1, m1,
 * ee, ff); the horizontal form of 8-246 gives the same value.
 */
static int
interp_half_centre(
	const uint8_t *window,
	unsigned row,
	unsigned col)
{
	int cc;
	int dd;
	int h1;
	int m1;
	int ee;
	int ff;
	int j1;
	int rounded;

	/* Derives the vertical intermediate values of the six columns around the centre. */
	cc = interp_half_vertical_raw(window, row, col - 2U);
	dd = interp_half_vertical_raw(window, row, col - 1U);
	h1 = interp_half_vertical_raw(window, row, col);
	m1 = interp_half_vertical_raw(window, row, col + 1U);
	ee = interp_half_vertical_raw(window, row, col + 2U);
	ff = interp_half_vertical_raw(window, row, col + 3U);

	/* Filters them (8-245) and rounds with 512 and a shift of 10 (8-247). */
	j1 = interp_filter6(cc, dd, h1, m1, ee, ff);
	rounded = interp_clip_round_shift(j1, 512, 10);

	/* Reports the centre half sample. */
	return rounded;
}

/*
 * Gives the predicted luma sample for the fractional position of a full sample G (Table 8-12).
 *
 * ( row, col ) is the window position of G.
 */
static int
interp_luma_sample(
	const uint8_t *window,
	unsigned row,
	unsigned col,
	unsigned frac_x,
	unsigned frac_y)
{
	int g;
	int neighbour;
	int b;
	int h;
	int j;
	int s;
	int m;
	int value;

	/* Starts from the full-sample position G, the value of position ( 0, 0 ). */
	g = interp_full(window, row, col);
	value = g;

	/*
	 * Selects the sample of the position by Table 8-12, indexed by xFracL
	 * and yFracL; the quarter positions average two of G, H, M and the
	 * half samples b, h, j, m, s with upward rounding (8-250 to 8-261).
	 * H is the sample to the right of G and M the sample below it.
	 *
	 * Values referenced: ITU-T H.264 (08/2021) Table 8-12.
	 *
	 *   xFracL  0 0 0 0 1 1 1 1 2 2 2 2 3 3 3 3
	 *   yFracL  0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3
	 *   sample  G d h n a e i p b f j q c g k r
	 */
	switch (frac_x * 4U + frac_y) {
	case 0:
		/* G: the integer position is copied. */
		value = g;
		break;
	case 1:
		/* d = ( G + h + 1 ) >> 1 (8-252). */
		h = interp_half_vertical(window, row, col);
		value = interp_average(g, h);
		break;
	case 2:
		/* h: the vertical half position (8-244). */
		value = interp_half_vertical(window, row, col);
		break;
	case 3:
		/* n = ( M + h + 1 ) >> 1 (8-253). */
		neighbour = interp_full(window, row + 1U, col);
		h = interp_half_vertical(window, row, col);
		value = interp_average(neighbour, h);
		break;
	case 4:
		/* a = ( G + b + 1 ) >> 1 (8-250). */
		b = interp_half_horizontal(window, row, col);
		value = interp_average(g, b);
		break;
	case 5:
		/* e = ( b + h + 1 ) >> 1 (8-258). */
		b = interp_half_horizontal(window, row, col);
		h = interp_half_vertical(window, row, col);
		value = interp_average(b, h);
		break;
	case 6:
		/* i = ( h + j + 1 ) >> 1 (8-255). */
		h = interp_half_vertical(window, row, col);
		j = interp_half_centre(window, row, col);
		value = interp_average(h, j);
		break;
	case 7:
		/* p = ( h + s + 1 ) >> 1 (8-260), s being the horizontal half sample of the row below. */
		h = interp_half_vertical(window, row, col);
		s = interp_half_horizontal(window, row + 1U, col);
		value = interp_average(h, s);
		break;
	case 8:
		/* b: the horizontal half position (8-243). */
		value = interp_half_horizontal(window, row, col);
		break;
	case 9:
		/* f = ( b + j + 1 ) >> 1 (8-254). */
		b = interp_half_horizontal(window, row, col);
		j = interp_half_centre(window, row, col);
		value = interp_average(b, j);
		break;
	case 10:
		/* j: the centre half position (8-247). */
		value = interp_half_centre(window, row, col);
		break;
	case 11:
		/* q = ( j + s + 1 ) >> 1 (8-257). */
		j = interp_half_centre(window, row, col);
		s = interp_half_horizontal(window, row + 1U, col);
		value = interp_average(j, s);
		break;
	case 12:
		/* c = ( H + b + 1 ) >> 1 (8-251). */
		neighbour = interp_full(window, row, col + 1U);
		b = interp_half_horizontal(window, row, col);
		value = interp_average(neighbour, b);
		break;
	case 13:
		/* g = ( b + m + 1 ) >> 1 (8-259), m being the vertical half sample of the column to the right. */
		b = interp_half_horizontal(window, row, col);
		m = interp_half_vertical(window, row, col + 1U);
		value = interp_average(b, m);
		break;
	case 14:
		/* k = ( j + m + 1 ) >> 1 (8-256). */
		j = interp_half_centre(window, row, col);
		m = interp_half_vertical(window, row, col + 1U);
		value = interp_average(j, m);
		break;
	case 15:
		/* r = ( m + s + 1 ) >> 1 (8-261). */
		m = interp_half_vertical(window, row, col + 1U);
		s = interp_half_horizontal(window, row + 1U, col);
		value = interp_average(m, s);
		break;
	default:
		/* Cannot happen, the fractional parts having been masked to two bits; G stands for it. */
		value = g;
		break;
	}

	/* Reports the predicted sample. */
	return value;
}
