/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The intra prediction of the lossy format.
 */

#include "vp8-predict.h"
#include "vp8-tables.h"

/*
 * The position of each edge pixel in the nine-plus-four pixel line the
 * subblock modes read.  The line runs from the bottom of the left column
 * up to the corner and then along the row above: the left column
 * bottom-up in 0..3, the corner in 4, the row above (eight pixels) in
 * 5..12.
 *
 * Values referenced: RFC 6386 (November 2011) section 12.3 (the array E
 * of subblock_intra_predict, extended with the four pixels above and to
 * the right).
 */
#define PREDICT_EDGE_LEFT_LAST		0
#define PREDICT_EDGE_CORNER		4
#define PREDICT_EDGE_ABOVE_FIRST	5
#define PREDICT_EDGE_SIZE		13

static void predict_fill(uint8_t *dst, unsigned size, unsigned value);
static void predict_dc(unsigned size, const struct webpc_vp8_edges *edges, uint8_t *dst, ptrdiff_t stride);
static void predict_tm(unsigned size, const struct webpc_vp8_edges *edges, uint8_t *dst, ptrdiff_t stride);
static int predict_avg2(int first, int second);
static int predict_avg3(int first, int second, int third);
static int predict_smooth2(const uint8_t *edge, int index);
static int predict_smooth3(const uint8_t *edge, int index);
static void predict_sub_dc(const uint8_t *edge, uint8_t pixels[4][4]);
static void predict_sub_tm(const uint8_t *edge, uint8_t pixels[4][4]);
static void predict_sub_ve(const uint8_t *edge, uint8_t pixels[4][4]);
static void predict_sub_he(const uint8_t *edge, uint8_t pixels[4][4]);
static void predict_sub_ld(const uint8_t *edge, uint8_t pixels[4][4]);
static void predict_sub_rd(const uint8_t *edge, uint8_t pixels[4][4]);
static void predict_sub_vr(const uint8_t *edge, uint8_t pixels[4][4]);
static void predict_sub_vl(const uint8_t *edge, uint8_t pixels[4][4]);
static void predict_sub_hd(const uint8_t *edge, uint8_t pixels[4][4]);
static void predict_sub_hu(const uint8_t *edge, uint8_t pixels[4][4]);

/*
 * Predicts a block of size by size (16 for luma, 8 for chroma) pixels into
 * dst (stride bytes a row) with mode WEBPC_VP8_DC_PRED, V_PRED, H_PRED or
 * TM_PRED (RFC 6386 12.2, 12.3).
 *
 * A size other than 8 or 16, or a mode other than the four, writes
 * nothing: the caller has checked both against the stream.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 12.2 and 12.3
 * (the four whole-block modes), 20.14 (the rounding of the DC mean).
 */
void
webpc_vp8_predict_block(
	unsigned mode,
	unsigned size,
	const struct webpc_vp8_edges *edges,
	uint8_t *dst,
	ptrdiff_t stride)
{
	unsigned row;
	unsigned column;
	uint8_t *row_start;

	/* Refuses a block size the format does not have. */
	if (size != 8U && size != 16U)
		return;

	/* The DC and TM modes need the neighbours' arithmetic. */
	if (mode == WEBPC_VP8_DC_PRED) {
		predict_dc(size, edges, dst, stride);
		return;
	}
	if (mode == WEBPC_VP8_TM_PRED) {
		predict_tm(size, edges, dst, stride);
		return;
	}

	/* V_PRED copies the row above into every row of the block. */
	if (mode == WEBPC_VP8_V_PRED) {
		for (row = 0; row < size; row++) {
			row_start = dst + (ptrdiff_t)row * stride;
			for (column = 0; column < size; column++) {
				row_start[column] = edges->above[column];
			}
		}
		return;
	}

	/* H_PRED copies the column on the left into every column of the block. */
	if (mode == WEBPC_VP8_H_PRED) {
		for (row = 0; row < size; row++) {
			predict_fill(dst + (ptrdiff_t)row * stride, size, edges->left[row]);
		}
		return;
	}

	/* Any other mode is not a whole-block mode; nothing is written. */
}

/*
 * Predicts one 4x4 luma subblock into dst (stride bytes a row) with mode
 * WEBPC_VP8_B_* (RFC 6386 12.3, its subblock code, and 20.14).
 *
 * A mode other than the ten subblock modes writes nothing.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3
 * (subblock_intra_predict, which section 20.14's predict.c agrees with in
 * every mode).
 */
void
webpc_vp8_predict_subblock(
	unsigned mode,
	const uint8_t *above,
	const uint8_t *left,
	uint8_t *dst,
	ptrdiff_t stride)
{
	uint8_t edge[PREDICT_EDGE_SIZE];
	uint8_t pixels[4][4];
	uint8_t *row_start;
	int index;
	int row;
	int column;

	/* Refuses a mode the format does not have before any pixel is read. */
	if (mode >= WEBPC_VP8_BMODES)
		return;

	/*
	 * Gathers the edge into one line: the left column bottom-up, the
	 * corner, then the eight pixels of the row above.
	 */
	for (index = 0; index < 4; index++) {
		edge[PREDICT_EDGE_LEFT_LAST + index] = left[3 - index];
	}
	edge[PREDICT_EDGE_CORNER] = above[-1];
	for (index = 0; index < 8; index++) {
		edge[PREDICT_EDGE_ABOVE_FIRST + index] = above[index];
	}

	/* Computes the 4x4 pixels of the mode. */
	switch (mode) {
	case WEBPC_VP8_B_DC_PRED:
		predict_sub_dc(edge, pixels);
		break;
	case WEBPC_VP8_B_TM_PRED:
		predict_sub_tm(edge, pixels);
		break;
	case WEBPC_VP8_B_VE_PRED:
		predict_sub_ve(edge, pixels);
		break;
	case WEBPC_VP8_B_HE_PRED:
		predict_sub_he(edge, pixels);
		break;
	case WEBPC_VP8_B_LD_PRED:
		predict_sub_ld(edge, pixels);
		break;
	case WEBPC_VP8_B_RD_PRED:
		predict_sub_rd(edge, pixels);
		break;
	case WEBPC_VP8_B_VR_PRED:
		predict_sub_vr(edge, pixels);
		break;
	case WEBPC_VP8_B_VL_PRED:
		predict_sub_vl(edge, pixels);
		break;
	case WEBPC_VP8_B_HD_PRED:
		predict_sub_hd(edge, pixels);
		break;
	default:
		predict_sub_hu(edge, pixels);
		break;
	}

	/* Stores the four rows at the destination's stride. */
	for (row = 0; row < 4; row++) {
		row_start = dst + (ptrdiff_t)row * stride;
		for (column = 0; column < 4; column++) {
			row_start[column] = pixels[row][column];
		}
	}
}

/*
 * Fills size pixels of one row with a value.
 */
static void
predict_fill(
	uint8_t *dst,
	unsigned size,
	unsigned value)
{
	unsigned column;

	/* Writes the value across the row. */
	for (column = 0; column < size; column++) {
		dst[column] = (uint8_t)value;
	}
}

/*
 * Predicts a whole block with DC_PRED: the rounded mean of the edges that
 * exist, or 128 with neither.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 12.2 (which
 * edges the mean takes and the case of neither) and 20.14 (predict_dc_nxn:
 * (sum + half) >> log2 of the number of pixels).
 */
static void
predict_dc(
	unsigned size,
	const struct webpc_vp8_edges *edges,
	uint8_t *dst,
	ptrdiff_t stride)
{
	unsigned sum;
	unsigned shift;
	unsigned value;
	unsigned index;
	unsigned row;

	/* The shift is the base 2 logarithm of the edge's pixel count. */
	shift = 3U;
	if (size == 16U)
		shift = 4U;

	/* Adds the row above when the block has a macroblock above. */
	sum = 0U;
	if (edges->have_above != 0) {
		for (index = 0; index < size; index++) {
			sum += edges->above[index];
		}
	}

	/* Adds the column on the left when the block has a macroblock there. */
	if (edges->have_left != 0) {
		for (index = 0; index < size; index++) {
			sum += edges->left[index];
		}
	}

	/*
	 * Takes the mean: with both edges there are twice the pixels, so one
	 * more bit; with neither there is no mean and the value is 128.
	 */
	if (edges->have_above != 0 && edges->have_left != 0) {
		value = (sum + (1U << shift)) >> (shift + 1U);
	} else if (edges->have_above != 0 || edges->have_left != 0) {
		value = (sum + (1U << (shift - 1U))) >> shift;
	} else {
		value = 128U;
	}

	/* Fills the block with the one value. */
	for (row = 0; row < size; row++) {
		predict_fill(dst + (ptrdiff_t)row * stride, size, value);
	}
}

/*
 * Predicts a whole block with TM_PRED: left[row] + above[column] - corner
 * clamped to a pixel, with the out-of-frame edge values as they are.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.2 (TMpred).
 */
static void
predict_tm(
	unsigned size,
	const struct webpc_vp8_edges *edges,
	uint8_t *dst,
	ptrdiff_t stride)
{
	uint8_t *row_start;
	unsigned row;
	unsigned column;
	int value;

	/* Predicts each row from its left pixel and the row above. */
	for (row = 0; row < size; row++) {
		row_start = dst + (ptrdiff_t)row * stride;
		for (column = 0; column < size; column++) {
			value = (int)edges->left[row] + (int)edges->above[column] - (int)edges->corner;
			row_start[column] = (uint8_t)webpc_clamp255(value);
		}
	}
}

/*
 * Averages two pixels, rounding half up.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (avg2).
 */
static int
predict_avg2(
	int first,
	int second)
{
	/* Reports the rounded mean. */
	return (first + second + 1) >> 1;
}

/*
 * Averages three pixels with the middle one counted twice, rounding half
 * up.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (avg3).
 */
static int
predict_avg3(
	int first,
	int second,
	int third)
{
	/* Reports the weighted mean. */
	return (first + second + second + third + 2) >> 2;
}

/*
 * Averages the edge pixel at index and the next one: the pixel half a step
 * past index.
 */
static int
predict_smooth2(
	const uint8_t *edge,
	int index)
{
	int result;

	/* Averages the two neighbours along the edge line. */
	result = predict_avg2(edge[index], edge[index + 1]);

	/* Reports the synthetic pixel between them. */
	return result;
}

/*
 * Smooths the edge pixel at index with its two neighbours on the edge
 * line.
 */
static int
predict_smooth3(
	const uint8_t *edge,
	int index)
{
	int result;

	/* Weights the pixel by the one before and the one after. */
	result = predict_avg3(edge[index - 1], edge[index], edge[index + 1]);

	/* Reports the smoothed pixel. */
	return result;
}

/*
 * B_DC_PRED: the four pixels above and the four on the left, 8 in all,
 * averaged as (sum + 4) >> 3, whatever the neighbours are.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (B_DC_PRED).
 */
static void
predict_sub_dc(
	const uint8_t *edge,
	uint8_t pixels[4][4])
{
	int sum;
	int index;
	int row;
	int column;

	/* Sums the left column (edge 0..3) and the four above (edge 5..8). */
	sum = 0;
	for (index = 0; index < 4; index++) {
		sum += edge[PREDICT_EDGE_LEFT_LAST + index];
		sum += edge[PREDICT_EDGE_ABOVE_FIRST + index];
	}

	/* Rounds the mean of eight pixels. */
	sum = (sum + 4) >> 3;

	/* Fills the subblock with it. */
	for (row = 0; row < 4; row++) {
		for (column = 0; column < 4; column++) {
			pixels[row][column] = (uint8_t)sum;
		}
	}
}

/*
 * B_TM_PRED: left[row] + above[column] - corner clamped to a pixel.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (B_TM_PRED).
 */
static void
predict_sub_tm(
	const uint8_t *edge,
	uint8_t pixels[4][4])
{
	int value;
	int row;
	int column;

	/* Each pixel is its row's left pixel plus the column's above pixel minus the corner. */
	for (row = 0; row < 4; row++) {
		for (column = 0; column < 4; column++) {
			value = (int)edge[3 - row] + (int)edge[PREDICT_EDGE_ABOVE_FIRST + column];
			value -= (int)edge[PREDICT_EDGE_CORNER];
			pixels[row][column] = (uint8_t)webpc_clamp255(value);
		}
	}
}

/*
 * B_VE_PRED: every row the row above smoothed with its neighbours (the
 * corner before it and the above-right pixel after it).
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (B_VE_PRED).
 */
static void
predict_sub_ve(
	const uint8_t *edge,
	uint8_t pixels[4][4])
{
	int row;
	int column;

	/* Smooths each column's above pixel and repeats it down the column. */
	for (column = 0; column < 4; column++) {
		for (row = 0; row < 4; row++) {
			pixels[row][column] = (uint8_t)predict_smooth3(edge, PREDICT_EDGE_ABOVE_FIRST + column);
		}
	}
}

/*
 * B_HE_PRED: every column the column on the left smoothed with its
 * neighbours (the corner above the top pixel; the bottom pixel counted
 * again below the last).
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (B_HE_PRED).
 */
static void
predict_sub_he(
	const uint8_t *edge,
	uint8_t pixels[4][4])
{
	int value;
	int row;
	int column;

	/*
	 * Smooths the upper three rows with the left pixels around each; row
	 * r's left pixel is edge[3 - r].
	 */
	for (row = 0; row < 3; row++) {
		value = predict_smooth3(edge, 3 - row);
		for (column = 0; column < 4; column++) {
			pixels[row][column] = (uint8_t)value;
		}
	}

	/* The bottom row has no pixel below it, so its own pixel stands in for it. */
	value = predict_avg3(edge[1], edge[0], edge[0]);
	for (column = 0; column < 4; column++) {
		pixels[3][column] = (uint8_t)value;
	}
}

/*
 * B_LD_PRED: each anti-diagonal one smoothed pixel of the row above, from
 * above[1] for the corner to above[6]; the last pixel counts above[7]
 * twice since above[8] does not exist.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (B_LD_PRED).
 */
static void
predict_sub_ld(
	const uint8_t *edge,
	uint8_t pixels[4][4])
{
	int value;
	int row;
	int column;

	/* Each pixel takes the smoothed above pixel at its row plus column plus 1. */
	for (row = 0; row < 4; row++) {
		for (column = 0; column < 4; column++) {
			if (row + column < 6) {
				/* The three pixels are all on the edge line. */
				value = predict_smooth3(edge, PREDICT_EDGE_ABOVE_FIRST + row + column + 1);
			} else {
				/* The last pixel's right neighbour does not exist, so above[7] repeats. */
				value = predict_avg3(edge[PREDICT_EDGE_ABOVE_FIRST + 6],
				    edge[PREDICT_EDGE_ABOVE_FIRST + 7], edge[PREDICT_EDGE_ABOVE_FIRST + 7]);
			}
			pixels[row][column] = (uint8_t)value;
		}
	}
}

/*
 * B_RD_PRED: each diagonal down to the right one smoothed pixel of the
 * edge line, from the bottom of the left column through the corner to
 * above[2].
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (B_RD_PRED).
 */
static void
predict_sub_rd(
	const uint8_t *edge,
	uint8_t pixels[4][4])
{
	int row;
	int column;

	/* The diagonal of (row, column) is centred on edge[4 - row + column]. */
	for (row = 0; row < 4; row++) {
		for (column = 0; column < 4; column++) {
			pixels[row][column] = (uint8_t)predict_smooth3(edge, PREDICT_EDGE_CORNER - row + column);
		}
	}
}

/*
 * B_VR_PRED: lines of slope 2 down to the right; the even rows are
 * averages of two edge pixels, the odd rows smoothed three, and the lower
 * left pixels step down the left column.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (B_VR_PRED).
 */
static void
predict_sub_vr(
	const uint8_t *edge,
	uint8_t pixels[4][4])
{
	/* The bottom two rows' first pixels come from the left column. */
	pixels[3][0] = (uint8_t)predict_smooth3(edge, 2);
	pixels[2][0] = (uint8_t)predict_smooth3(edge, 3);

	/* The pixels centred on the corner. */
	pixels[3][1] = (uint8_t)predict_smooth3(edge, 4);
	pixels[1][0] = pixels[3][1];
	pixels[2][1] = (uint8_t)predict_smooth2(edge, 4);
	pixels[0][0] = pixels[2][1];

	/* The pixels centred on above[0]. */
	pixels[3][2] = (uint8_t)predict_smooth3(edge, 5);
	pixels[1][1] = pixels[3][2];
	pixels[2][2] = (uint8_t)predict_smooth2(edge, 5);
	pixels[0][1] = pixels[2][2];

	/* The pixels centred on above[1]. */
	pixels[3][3] = (uint8_t)predict_smooth3(edge, 6);
	pixels[1][2] = pixels[3][3];
	pixels[2][3] = (uint8_t)predict_smooth2(edge, 6);
	pixels[0][2] = pixels[2][3];

	/* The last column's top two pixels, centred on above[2]. */
	pixels[1][3] = (uint8_t)predict_smooth3(edge, 7);
	pixels[0][3] = (uint8_t)predict_smooth2(edge, 7);
}

/*
 * B_VL_PRED: lines of slope 2 down to the left from the row above and
 * the above-right pixels; the last two pixels of the bottom rows do not
 * follow the pattern.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (B_VL_PRED).
 */
static void
predict_sub_vl(
	const uint8_t *edge,
	uint8_t pixels[4][4])
{
	/* The first column and its diagonal neighbours, from above[0] and above[1]. */
	pixels[0][0] = (uint8_t)predict_smooth2(edge, 5);
	pixels[1][0] = (uint8_t)predict_smooth3(edge, 6);
	pixels[2][0] = (uint8_t)predict_smooth2(edge, 6);
	pixels[0][1] = pixels[2][0];

	/* The pixels from above[2]. */
	pixels[1][1] = (uint8_t)predict_smooth3(edge, 7);
	pixels[3][0] = pixels[1][1];
	pixels[2][1] = (uint8_t)predict_smooth2(edge, 7);
	pixels[0][2] = pixels[2][1];

	/* The pixels from above[3]. */
	pixels[3][1] = (uint8_t)predict_smooth3(edge, 8);
	pixels[1][2] = pixels[3][1];
	pixels[2][2] = (uint8_t)predict_smooth2(edge, 8);
	pixels[0][3] = pixels[2][2];

	/* The pixels from above[4], the first above-right pixel. */
	pixels[3][2] = (uint8_t)predict_smooth3(edge, 9);
	pixels[1][3] = pixels[3][2];

	/* The last two pixels step on to above[5] and above[6] without a half step between. */
	pixels[2][3] = (uint8_t)predict_smooth3(edge, 10);
	pixels[3][3] = (uint8_t)predict_smooth3(edge, 11);
}

/*
 * B_HD_PRED: lines of slope 1/2 down to the right from the left column,
 * the corner and the row above.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (B_HD_PRED;
 * the reference decoder of 20.14 computes the same pixels).
 */
static void
predict_sub_hd(
	const uint8_t *edge,
	uint8_t pixels[4][4])
{
	/* The bottom row's first two pixels, from the bottom of the left column. */
	pixels[3][0] = (uint8_t)predict_smooth2(edge, 0);
	pixels[3][1] = (uint8_t)predict_smooth3(edge, 1);

	/* The pixels between the left column's bottom two pixels. */
	pixels[2][0] = (uint8_t)predict_smooth2(edge, 1);
	pixels[3][2] = pixels[2][0];
	pixels[2][1] = (uint8_t)predict_smooth3(edge, 2);
	pixels[3][3] = pixels[2][1];

	/* The pixels between the left column's top two pixels. */
	pixels[2][2] = (uint8_t)predict_smooth2(edge, 2);
	pixels[1][0] = pixels[2][2];
	pixels[2][3] = (uint8_t)predict_smooth3(edge, 3);
	pixels[1][1] = pixels[2][3];

	/* The pixels between the top of the left column and the corner. */
	pixels[1][2] = (uint8_t)predict_smooth2(edge, 3);
	pixels[0][0] = pixels[1][2];
	pixels[1][3] = (uint8_t)predict_smooth3(edge, 4);
	pixels[0][1] = pixels[1][3];

	/* The top row's last two pixels, from the row above. */
	pixels[0][2] = (uint8_t)predict_smooth3(edge, 5);
	pixels[0][3] = (uint8_t)predict_smooth3(edge, 6);
}

/*
 * B_HU_PRED: lines of slope 1/2 up to the right, from the left column
 * only; the lower right pixels are all the bottom left pixel.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 12.3 (B_HU_PRED).
 */
static void
predict_sub_hu(
	const uint8_t *edge,
	uint8_t pixels[4][4])
{
	int column;

	/* The upper left pixels, between and across the left column's pixels (edge 3 is the top one). */
	pixels[0][0] = (uint8_t)predict_avg2(edge[3], edge[2]);
	pixels[0][1] = (uint8_t)predict_avg3(edge[3], edge[2], edge[1]);
	pixels[0][2] = (uint8_t)predict_avg2(edge[2], edge[1]);
	pixels[1][0] = pixels[0][2];
	pixels[0][3] = (uint8_t)predict_avg3(edge[2], edge[1], edge[0]);
	pixels[1][1] = pixels[0][3];

	/* The pixels near the bottom of the column; the one below the last pixel is the last pixel again. */
	pixels[1][2] = (uint8_t)predict_avg2(edge[1], edge[0]);
	pixels[2][0] = pixels[1][2];
	pixels[1][3] = (uint8_t)predict_avg3(edge[1], edge[0], edge[0]);
	pixels[2][1] = pixels[1][3];

	/* No edge pixel lies on the remaining diagonals, so each is the bottom left pixel. */
	pixels[2][2] = edge[0];
	pixels[2][3] = edge[0];
	for (column = 0; column < 4; column++) {
		pixels[3][column] = edge[0];
	}
}
