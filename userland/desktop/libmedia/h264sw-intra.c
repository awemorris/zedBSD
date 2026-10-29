/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The intra prediction of the software H.264 decoder (8.3).
 */

#include "h264sw-intra.h"
#include "h264sw-context.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* The value of every predicted sample of a block that cannot be predicted, and of a DC prediction without neighbours: 1 << ( BitDepth - 1 ) for 8 bits. */
#define INTRA_FLAT 128

/* The largest 8-bit sample value, the upper bound of Clip1 for a bit depth of 8. */
#define INTRA_SAMPLE_MAX 255

/* The highest Intra4x4PredMode and Intra8x8PredMode (Tables 8-2 and 8-3). */
#define INTRA_MODE_MAX 8U

/* The mode that a neighbour which cannot give one stands for: Intra_4x4_DC and Intra_8x8_DC, both 2 (8.3.1.1, 8.3.2.1). */
#define INTRA_MODE_DC 2U

/*
 * Where a block's neighbouring samples are, and which block it is.
 *
 * component is 0 for luma, 1 for Cb and 2 for Cr; mb_size is the size of
 * the component's macroblock (16 or 8); x and y are the upper-left
 * sample of the block inside the macroblock; size is the block's width
 * and height.  right is 1 when the size samples above and to the right
 * of the block are gathered as well.  order_size is 4 or 8 when a
 * neighbour inside the current macroblock that is a later block of that
 * size is not available (blk is the block's index), and 0 otherwise.
 * One instance lives for one gathering.
 */
struct intra_block {
	unsigned component;
	unsigned mb_size;
	int x;
	int y;
	unsigned size;
	unsigned right;
	unsigned order_size;
	unsigned blk;
};

/*
 * The parts of the edge each Intra_4x4 and Intra_8x8 mode shall use,
 * indexed by Intra4x4PredMode or Intra8x8PredMode.
 *
 * The top-right samples are not listed: they are substituted when absent,
 * so the top part stands for all of p[ x, -1 ].  The table is constant and
 * shared by every decoder.
 *
 * Values referenced: ITU-T H.264 (08/2021) 8.3.1.2.1 to 8.3.1.2.9 and
 * 8.3.2.2.2 to 8.3.2.2.10 (the sentence "This mode shall be used only
 * when the samples ... are marked as available" of each mode).
 */
static const uint8_t intra_nxn_needs[9] = {
	H264SW_EDGE_TOP,
	H264SW_EDGE_LEFT,
	0,
	H264SW_EDGE_TOP,
	H264SW_EDGE_TOP | H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT,
	H264SW_EDGE_TOP | H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT,
	H264SW_EDGE_TOP | H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT,
	H264SW_EDGE_TOP,
	H264SW_EDGE_LEFT
};

/*
 * The parts of the edge each Intra_16x16 mode shall use, indexed by
 * Intra16x16PredMode.  The table is constant and shared by every decoder.
 *
 * Values referenced: ITU-T H.264 (08/2021) 8.3.3.1 to 8.3.3.4 (the
 * sentence "This Intra_16x16 prediction mode shall be used only when the
 * samples ... are marked as available" of each mode).
 */
static const uint8_t intra_16x16_needs[4] = {
	H264SW_EDGE_TOP,
	H264SW_EDGE_LEFT,
	0,
	H264SW_EDGE_TOP | H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT
};

/*
 * The parts of the edge each chroma mode shall use, indexed by
 * intra_chroma_pred_mode.  The table is constant and shared by every
 * decoder.
 *
 * Values referenced: ITU-T H.264 (08/2021) 8.3.4.2 to 8.3.4.4 (the
 * sentence "This mode shall be used only when the samples ... are marked
 * as available" of each mode; the DC mode has none).
 */
static const uint8_t intra_chroma_needs[4] = {
	0,
	H264SW_EDGE_LEFT,
	H264SW_EDGE_TOP,
	H264SW_EDGE_TOP | H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT
};

/* Forward declarations of the static functions. */
static unsigned intra_neighbour_mode(const struct h264sw_mb *mb, unsigned index);
static unsigned intra_combine_mode(unsigned predicted, unsigned prev_flag, unsigned rem);
static int intra_constrained(const struct h264sw_slice_ctx *ctx);
static int intra_is_barred(const struct h264sw_slice_ctx *ctx, const struct h264sw_mb *mb);
static struct h264sw_mb *intra_find_mb(const struct h264sw_slice_ctx *ctx, unsigned mb_size, int xn, int yn, unsigned *xw, unsigned *yw);
static int intra_copy_run(const struct h264sw_slice_ctx *ctx, unsigned component, unsigned mb_size, int xn, int yn, int vertical, unsigned count, uint8_t *out);
static void intra_gather(const struct h264sw_slice_ctx *ctx, const struct intra_block *block, struct h264sw_intra_edge *edge);
static unsigned intra_load(const struct h264sw_intra_edge *edge, unsigned size, unsigned wide, uint8_t *top_row, uint8_t *left_col);
static void intra_fill_flat(uint8_t *pred, unsigned count);
static int intra_shift_floor(int value, unsigned shift);
static int intra_clip1(int value);
static void intra_filter_8x8(const uint8_t *top, const uint8_t *left, unsigned available, uint8_t *filtered_top, uint8_t *filtered_left);
static void intra_pred_vertical(unsigned size, const uint8_t *top, uint8_t *pred);
static void intra_pred_horizontal(unsigned size, const uint8_t *left, uint8_t *pred);
static void intra_pred_dc(unsigned size, const uint8_t *top, const uint8_t *left, unsigned available, uint8_t *pred);
static void intra_pred_diagonal_down_left(unsigned size, const uint8_t *top, uint8_t *pred);
static void intra_pred_diagonal_down_right(unsigned size, const uint8_t *top, const uint8_t *left, uint8_t *pred);
static void intra_pred_vertical_right(unsigned size, const uint8_t *top, const uint8_t *left, uint8_t *pred);
static void intra_pred_horizontal_down(unsigned size, const uint8_t *top, const uint8_t *left, uint8_t *pred);
static void intra_pred_vertical_left(unsigned size, const uint8_t *top, uint8_t *pred);
static void intra_pred_horizontal_up(unsigned size, const uint8_t *left, uint8_t *pred);
static void intra_pred_nxn(unsigned size, unsigned mode, const uint8_t *top, const uint8_t *left, unsigned available, uint8_t *pred);
static void intra_pred_plane(unsigned size, int scale, const uint8_t *top, const uint8_t *left, uint8_t *pred);
static void intra_pred_chroma_dc(const uint8_t *top, const uint8_t *left, unsigned available, uint8_t *pred);

/*
 * Derives Intra4x4PredMode of a block (8.3.1.1).
 *
 * A missing context or a block index of 16 or more gives 2 (DC), a mode
 * every block can use; a rem above 7 (it is a 3-bit syntax element) gives
 * a mode of at most 8.
 */
unsigned
h264sw_intra4x4_mode(
	const struct h264sw_slice_ctx *ctx,
	unsigned blk,
	unsigned prev_flag,
	unsigned rem)
{
	struct h264sw_mb *mb_a;
	struct h264sw_mb *mb_b;
	unsigned blk_a;
	unsigned blk_b;
	unsigned mode_a;
	unsigned mode_b;
	unsigned predicted;
	unsigned mode;
	int dc_predicted;
	int barred;

	/* Gives DC for a call that names no context or no block of a macroblock. */
	if (ctx == NULL)
		return INTRA_MODE_DC;
	if (blk >= 16U)
		return INTRA_MODE_DC;

	/* Finds the left (A) and upper (B) neighbouring 4x4 blocks (6.4.11.4, step 1). */
	mb_a = h264sw_mb_neighbour_4x4(&ctx->n, blk, 0, &blk_a);
	mb_b = h264sw_mb_neighbour_4x4(&ctx->n, blk, 1, &blk_b);

	/* Predicts DC when neighbour A is not available, or is inter coded while constrained_intra_pred_flag is 1 (step 2). */
	dc_predicted = 0;
	if (mb_a == NULL) {
		dc_predicted = 1;
	} else {
		barred = intra_is_barred(ctx, mb_a);
		if (barred != 0)
			dc_predicted = 1;
	}

	/* Predicts DC when neighbour B is not available, or is inter coded while constrained_intra_pred_flag is 1 (step 2). */
	if (mb_b == NULL) {
		dc_predicted = 1;
	} else {
		barred = intra_is_barred(ctx, mb_b);
		if (barred != 0)
			dc_predicted = 1;
	}

	/* Takes the mode of each neighbour, DC when the neighbours are not used (step 3). */
	mode_a = INTRA_MODE_DC;
	mode_b = INTRA_MODE_DC;
	if (dc_predicted == 0) {
		/* A neighbour coded in Intra_4x4 gives its block's mode; one coded in Intra_8x8 gives its 8x8 block's mode, which is kept in each of the four 4x4 entries. */
		mode_a = intra_neighbour_mode(mb_a, blk_a);
		mode_b = intra_neighbour_mode(mb_b, blk_b);
	}

	/* Predicts the mode as the smaller of the two (8-41). */
	predicted = mode_a;
	if (mode_b < predicted)
		predicted = mode_b;

	/* Applies the prediction flag and the remaining mode (8-41). */
	mode = intra_combine_mode(predicted, prev_flag, rem);

	/* Reports the mode of the block. */
	return mode;
}

/*
 * Derives Intra8x8PredMode of a block (8.3.2.1).
 *
 * A missing context or a block index of 4 or more gives 2 (DC); a rem
 * above 7 gives a mode of at most 8.
 */
unsigned
h264sw_intra8x8_mode(
	const struct h264sw_slice_ctx *ctx,
	unsigned blk,
	unsigned prev_flag,
	unsigned rem)
{
	struct h264sw_mb *mb_a;
	struct h264sw_mb *mb_b;
	unsigned blk_a;
	unsigned blk_b;
	unsigned index_a;
	unsigned index_b;
	unsigned mode_a;
	unsigned mode_b;
	unsigned predicted;
	unsigned mode;
	int dc_predicted;
	int barred;

	/* Gives DC for a call that names no context or no 8x8 block of a macroblock. */
	if (ctx == NULL)
		return INTRA_MODE_DC;
	if (blk >= 4U)
		return INTRA_MODE_DC;

	/* Finds the left (A) and upper (B) neighbouring 8x8 blocks (6.4.11.2, step 1). */
	mb_a = h264sw_mb_neighbour_8x8(&ctx->n, blk, 0, &blk_a);
	mb_b = h264sw_mb_neighbour_8x8(&ctx->n, blk, 1, &blk_b);

	/* Predicts DC when neighbour A is not available, or is inter coded while constrained_intra_pred_flag is 1 (step 2). */
	dc_predicted = 0;
	if (mb_a == NULL) {
		dc_predicted = 1;
	} else {
		barred = intra_is_barred(ctx, mb_a);
		if (barred != 0)
			dc_predicted = 1;
	}

	/* Predicts DC when neighbour B is not available, or is inter coded while constrained_intra_pred_flag is 1 (step 2). */
	if (mb_b == NULL) {
		dc_predicted = 1;
	} else {
		barred = intra_is_barred(ctx, mb_b);
		if (barred != 0)
			dc_predicted = 1;
	}

	/* Takes the mode of each neighbour, DC when the neighbours are not used (step 3). */
	mode_a = INTRA_MODE_DC;
	mode_b = INTRA_MODE_DC;
	if (dc_predicted == 0) {
		/*
		 * An Intra_8x8 neighbour gives the mode of its 8x8 block, which is kept in
		 * all four of its 4x4 entries; an Intra_4x4 neighbour gives the mode of
		 * its 4x4 block number 4 * luma8x8BlkIdxN + n, with n 1 for A and 2 for B
		 * (8-72, frames without MBAFF).
		 */
		index_a = blk_a * 4U;
		if (mb_a->kind == H264SW_MB_I_4X4)
			index_a = index_a + 1U;
		index_b = blk_b * 4U;
		if (mb_b->kind == H264SW_MB_I_4X4)
			index_b = index_b + 2U;
		mode_a = intra_neighbour_mode(mb_a, index_a);
		mode_b = intra_neighbour_mode(mb_b, index_b);
	}

	/* Predicts the mode as the smaller of the two (8-73). */
	predicted = mode_a;
	if (mode_b < predicted)
		predicted = mode_b;

	/* Applies the prediction flag and the remaining mode (8-73). */
	mode = intra_combine_mode(predicted, prev_flag, rem);

	/* Reports the mode of the block. */
	return mode;
}

/*
 * Gathers the neighbouring luma samples of a block of the current macroblock.
 *
 * A call with a missing context or edge, a size other than 4, 8 and 16, or
 * a block index its size does not have leaves nothing available.  The
 * block index of a 16x16 block is ignored.
 */
void
h264sw_intra_edge_luma(
	const struct h264sw_slice_ctx *ctx,
	unsigned size,
	unsigned blk,
	struct h264sw_intra_edge *edge)
{
	struct intra_block block;

	/* Ignores a call without an edge to fill. */
	if (edge == NULL)
		return;

	/* Starts with nothing available and every sample 0, so that no entry is left undefined. */
	memset(edge, 0, sizeof(*edge));

	/* Leaves nothing available for a call without a context. */
	if (ctx == NULL)
		return;

	/* Describes the block that the size and the index name, in luma samples. */
	memset(&block, 0, sizeof(block));
	block.component = 0;
	block.mb_size = 16U;
	block.size = size;
	if (size == 4U) {
		/* A 4x4 block has a top-right, which may lie in a later 4x4 block (6.4.3). */
		if (blk >= 16U)
			return;
		block.x = (int)h264sw_blk4x4_x[blk];
		block.y = (int)h264sw_blk4x4_y[blk];
		block.right = 1U;
		block.order_size = 4U;
		block.blk = blk;
	} else if (size == 8U) {
		/* An 8x8 block has a top-right, which may lie in a later 8x8 block (6.4.5). */
		if (blk >= 4U)
			return;
		block.x = (int)(blk % 2U) * 8;
		block.y = (int)(blk / 2U) * 8;
		block.right = 1U;
		block.order_size = 8U;
		block.blk = blk;
	} else if (size == 16U) {
		/* A 16x16 block is the whole macroblock and has no top-right. */
		block.x = 0;
		block.y = 0;
	} else {
		/* No other size of luma block is predicted. */
		return;
	}

	/* Gathers the left, top, top-left and top-right samples of the block. */
	intra_gather(ctx, &block, edge);
}

/*
 * Gathers the neighbouring samples of the current macroblock's 8x8 block of chroma component plane (1 Cb, 2 Cr), for 8.3.4, with the same rules.
 *
 * A call with a missing context or edge or a plane other than 1 and 2
 * leaves nothing available.
 */
void
h264sw_intra_edge_chroma(
	const struct h264sw_slice_ctx *ctx,
	unsigned plane,
	struct h264sw_intra_edge *edge)
{
	struct intra_block block;

	/* Ignores a call without an edge to fill. */
	if (edge == NULL)
		return;

	/* Starts with nothing available and every sample 0, so that no entry is left undefined. */
	memset(edge, 0, sizeof(*edge));

	/* Leaves nothing available for a call without a context or a plane the 4:2:0 format does not have. */
	if (ctx == NULL)
		return;
	if (plane != 1U && plane != 2U)
		return;

	/* Describes the 8x8 chroma block of the macroblock, which has no top-right. */
	memset(&block, 0, sizeof(block));
	block.component = plane;
	block.mb_size = 8U;
	block.x = 0;
	block.y = 0;
	block.size = 8U;

	/* Gathers the left, top and top-left samples of the block. */
	intra_gather(ctx, &block, edge);
}

/*
 * Predicts a 4x4 luma block (8.3.1.2.1 to 8.3.1.2.9).
 *
 * A mode above 8, a missing edge or a mode whose samples the edge lacks
 * gives EINVAL and a block of 128; a missing pred gives EINVAL.
 */
int
h264sw_intra_pred4x4(
	unsigned mode,
	const struct h264sw_intra_edge *edge,
	uint8_t *pred)
{
	uint8_t top_row[9];
	uint8_t left_col[5];
	unsigned available;
	unsigned needs;

	/* Refuses a call without a place to put the prediction. */
	if (pred == NULL)
		return EINVAL;

	/* Refuses a mode that Table 8-2 does not have, with a block of 128. */
	if (mode > INTRA_MODE_MAX) {
		intra_fill_flat(pred, 16U);
		return EINVAL;
	}

	/* Refuses a call without an edge, with a block of 128. */
	if (edge == NULL) {
		intra_fill_flat(pred, 16U);
		return EINVAL;
	}

	/* Copies the available samples, with the top-right samples substituted when absent (8.3.1.2). */
	available = intra_load(edge, 4U, 1U, top_row, left_col);

	/* Refuses a mode that needs samples the edge does not have, with a block of 128. */
	needs = intra_nxn_needs[mode];
	if ((available & needs) != needs) {
		intra_fill_flat(pred, 16U);
		return EINVAL;
	}

	/* Predicts the block; the arrays are read from index 1 so that index -1 is p[ -1, -1 ]. */
	intra_pred_nxn(4U, mode, top_row + 1, left_col + 1, available, pred);

	/* Succeeded: pred holds the 4x4 prediction. */
	return 0;
}

/*
 * Predicts an 8x8 luma block (8.3.2.2): first the reference samples are filtered (8.3.2.2.1, which also substitutes the unavailable top-right samples), then the mode's equations (8.3.2.2.2 to 8.3.2.2.10).
 *
 * Errors are those of h264sw_intra_pred4x4, with a block of 64 samples
 * of 128.
 */
int
h264sw_intra_pred8x8(
	unsigned mode,
	const struct h264sw_intra_edge *edge,
	uint8_t *pred)
{
	uint8_t top_row[17];
	uint8_t left_col[9];
	uint8_t filtered_top_row[17];
	uint8_t filtered_left_col[9];
	unsigned available;
	unsigned needs;

	/* Refuses a call without a place to put the prediction. */
	if (pred == NULL)
		return EINVAL;

	/* Refuses a mode that Table 8-3 does not have, with a block of 128. */
	if (mode > INTRA_MODE_MAX) {
		intra_fill_flat(pred, 64U);
		return EINVAL;
	}

	/* Refuses a call without an edge, with a block of 128. */
	if (edge == NULL) {
		intra_fill_flat(pred, 64U);
		return EINVAL;
	}

	/* Copies the available samples, with the top-right samples substituted when absent (8.3.2.2). */
	available = intra_load(edge, 8U, 1U, top_row, left_col);

	/* Refuses a mode that needs samples the edge does not have, with a block of 128. */
	needs = intra_nxn_needs[mode];
	if ((available & needs) != needs) {
		intra_fill_flat(pred, 64U);
		return EINVAL;
	}

	/* Filters the reference samples (8.3.2.2.1). */
	intra_filter_8x8(top_row + 1, left_col + 1, available, filtered_top_row + 1, filtered_left_col + 1);

	/* Predicts the block from the filtered samples; the arrays are read from index 1 so that index -1 is p'[ -1, -1 ]. */
	intra_pred_nxn(8U, mode, filtered_top_row + 1, filtered_left_col + 1, available, pred);

	/* Succeeded: pred holds the 8x8 prediction. */
	return 0;
}

/*
 * Predicts a 16x16 luma block (8.3.3.1 to 8.3.3.4); pred receives 256 samples.
 *
 * Errors are those of h264sw_intra_pred4x4, with a block of 256 samples
 * of 128.
 */
int
h264sw_intra_pred16x16(
	unsigned mode,
	const struct h264sw_intra_edge *edge,
	uint8_t *pred)
{
	uint8_t top_row[17];
	uint8_t left_col[17];
	unsigned available;
	unsigned needs;

	/* Refuses a call without a place to put the prediction. */
	if (pred == NULL)
		return EINVAL;

	/* Refuses a mode that Table 8-4 does not have, with a block of 128. */
	if (mode > H264SW_I16_PLANE) {
		intra_fill_flat(pred, 256U);
		return EINVAL;
	}

	/* Refuses a call without an edge, with a block of 128. */
	if (edge == NULL) {
		intra_fill_flat(pred, 256U);
		return EINVAL;
	}

	/* Copies the available samples. */
	available = intra_load(edge, 16U, 0U, top_row, left_col);

	/* Refuses a mode that needs samples the edge does not have, with a block of 128. */
	needs = intra_16x16_needs[mode];
	if ((available & needs) != needs) {
		intra_fill_flat(pred, 256U);
		return EINVAL;
	}

	/* Predicts the block by the mode; the arrays are read from index 1 so that index -1 is p[ -1, -1 ]. */
	if (mode == H264SW_I16_VERTICAL) {
		/* Copies the row above into every row (8-116). */
		intra_pred_vertical(16U, top_row + 1, pred);
	} else if (mode == H264SW_I16_HORIZONTAL) {
		/* Copies the left column into every column (8-117). */
		intra_pred_horizontal(16U, left_col + 1, pred);
	} else if (mode == H264SW_I16_DC) {
		/* Averages the neighbours that are available (8-118 to 8-121). */
		intra_pred_dc(16U, top_row + 1, left_col + 1, available, pred);
	} else {
		/* Fits a plane through the neighbours, with the scale 5 of 8-124 and 8-125 (8-122 to 8-127). */
		intra_pred_plane(16U, 5, top_row + 1, left_col + 1, pred);
	}

	/* Succeeded: pred holds the 16x16 prediction. */
	return 0;
}

/*
 * Predicts an 8x8 chroma block of 4:2:0 (8.3.4.1 to 8.3.4.4).
 *
 * Errors are those of h264sw_intra_pred4x4, with a block of 64 samples
 * of 128.
 */
int
h264sw_intra_pred_chroma(
	unsigned mode,
	const struct h264sw_intra_edge *edge,
	uint8_t *pred)
{
	uint8_t top_row[9];
	uint8_t left_col[9];
	unsigned available;
	unsigned needs;

	/* Refuses a call without a place to put the prediction. */
	if (pred == NULL)
		return EINVAL;

	/* Refuses a mode that Table 8-5 does not have, with a block of 128. */
	if (mode > H264SW_ICHROMA_PLANE) {
		intra_fill_flat(pred, 64U);
		return EINVAL;
	}

	/* Refuses a call without an edge, with a block of 128. */
	if (edge == NULL) {
		intra_fill_flat(pred, 64U);
		return EINVAL;
	}

	/* Copies the available samples. */
	available = intra_load(edge, 8U, 0U, top_row, left_col);

	/* Refuses a mode that needs samples the edge does not have, with a block of 128. */
	needs = intra_chroma_needs[mode];
	if ((available & needs) != needs) {
		intra_fill_flat(pred, 64U);
		return EINVAL;
	}

	/* Predicts the block by the mode; the arrays are read from index 1 so that index -1 is p[ -1, -1 ]. */
	if (mode == H264SW_ICHROMA_DC) {
		/* Averages the neighbours each 4x4 block prefers (8-132 to 8-141). */
		intra_pred_chroma_dc(top_row + 1, left_col + 1, available, pred);
	} else if (mode == H264SW_ICHROMA_HORIZONTAL) {
		/* Copies the left column into every column (8-142). */
		intra_pred_horizontal(8U, left_col + 1, pred);
	} else if (mode == H264SW_ICHROMA_VERTICAL) {
		/* Copies the row above into every row (8-143). */
		intra_pred_vertical(8U, top_row + 1, pred);
	} else {
		/* Fits a plane through the neighbours, with the scale 34 of 8-146 and 8-147 for 4:2:0 (8-144 to 8-149). */
		intra_pred_plane(8U, 34, top_row + 1, left_col + 1, pred);
	}

	/* Succeeded: pred holds the 8x8 chroma prediction. */
	return 0;
}

/*
 * Gives the Intra4x4PredMode that a neighbouring macroblock keeps at an index.
 *
 * Only a macroblock coded in Intra_4x4 or Intra_8x8 has a mode; any other
 * kind, an index of 16 or more and a stored value above 8 give DC.
 */
static unsigned
intra_neighbour_mode(
	const struct h264sw_mb *mb,
	unsigned index)
{
	unsigned mode;

	/* Gives DC for a macroblock that is not coded in Intra_4x4 or Intra_8x8 (8.3.1.1 step 3). */
	if (mb->kind != H264SW_MB_I_4X4 && mb->kind != H264SW_MB_I_8X8)
		return INTRA_MODE_DC;

	/* Gives DC for an index no macroblock has. */
	if (index >= 16U)
		return INTRA_MODE_DC;

	/* Gives DC for a stored value that is not a mode. */
	mode = mb->intra4x4_mode[index];
	if (mode > INTRA_MODE_MAX)
		return INTRA_MODE_DC;

	/* Reports the stored mode of the neighbouring block. */
	return mode;
}

/*
 * Turns the predicted mode, the prediction flag and the remaining mode into the mode of the block.
 *
 * The result never exceeds 8, which a rem of 8 or more would give.
 */
static unsigned
intra_combine_mode(
	unsigned predicted,
	unsigned prev_flag,
	unsigned rem)
{
	unsigned mode;

	/* Uses the predicted mode when the stream says so (8-41, 8-73). */
	if (prev_flag != 0U)
		return predicted;

	/* Skips the predicted mode among the remaining ones: a rem below it is the mode itself, any other is one above (8-41, 8-73). */
	mode = rem + 1U;
	if (rem < predicted)
		mode = rem;

	/* Keeps the mode inside Table 8-2 whatever rem was. */
	if (mode > INTRA_MODE_MAX)
		mode = INTRA_MODE_MAX;

	/* Reports the mode of the block. */
	return mode;
}

/* Tells whether the picture's constrained_intra_pred_flag is 1; a picture without parameters is read as 0. */
static int
intra_constrained(
	const struct h264sw_slice_ctx *ctx)
{
	/* A slice without a picture or a picture parameter set has no flag. */
	if (ctx->picture == NULL)
		return 0;
	if (ctx->picture->pps == NULL)
		return 0;

	/* Reports the flag of the active picture parameter set. */
	if (ctx->picture->pps->constrained_intra_pred != 0)
		return 1;

	/* The flag is 0. */
	return 0;
}

/*
 * Tells whether the macroblock's samples and modes are not used for Intra prediction.
 *
 * That is so for a macroblock coded in an Inter prediction mode while
 * constrained_intra_pred_flag is 1 (8.3.1.1, 8.3.1.2, 8.3.2.1, 8.3.2.2,
 * 8.3.3, 8.3.4).
 */
static int
intra_is_barred(
	const struct h264sw_slice_ctx *ctx,
	const struct h264sw_mb *mb)
{
	int constrained;

	/* Takes the flag of the picture. */
	constrained = intra_constrained(ctx);

	/* Uses every macroblock when the flag is 0. */
	if (constrained == 0)
		return 0;

	/* Bars a macroblock coded in an Inter prediction mode: every kind from P_Skip on. */
	if (mb->kind >= H264SW_MB_P_SKIP)
		return 1;

	/* An Intra macroblock is used. */
	return 0;
}

/*
 * Finds the macroblock holding a neighbouring location when it may be used for Intra prediction.
 *
 * (xn, yn) is relative to the current macroblock in samples of a
 * component whose macroblock is mb_size by mb_size.  Returns the macroblock
 * and the location inside it (6.4.12), or NULL when it is not available
 * or is barred (see intra_is_barred).
 */
static struct h264sw_mb *
intra_find_mb(
	const struct h264sw_slice_ctx *ctx,
	unsigned mb_size,
	int xn,
	int yn,
	unsigned *xw,
	unsigned *yw)
{
	struct h264sw_mb *found;
	int barred;

	/* Finds the macroblock that holds the location (6.4.12.1, Table 6-3). */
	found = h264sw_mb_locate(&ctx->n, xn, yn, mb_size, mb_size, xw, yw);
	if (found == NULL)
		return NULL;

	/* Refuses a macroblock coded in an Inter prediction mode when constrained_intra_pred_flag is 1. */
	barred = intra_is_barred(ctx, found);
	if (barred != 0)
		return NULL;

	/* Succeeded: the macroblock may be used. */
	return found;
}

/*
 * Copies a run of constructed samples of the current picture, a row or a column, into out.
 *
 * The run starts at (xn, yn) relative to the current macroblock and goes
 * to the right, or down when vertical is 1.  Returns 1 when all count
 * samples lie inside the component's plane and were copied, 0 otherwise.
 */
static int
intra_copy_run(
	const struct h264sw_slice_ctx *ctx,
	unsigned component,
	unsigned mb_size,
	int xn,
	int yn,
	int vertical,
	unsigned count,
	uint8_t *out)
{
	const struct h264sw_frame *frame;
	const uint8_t *plane;
	int64_t plane_width;
	int64_t plane_height;
	int64_t x0;
	int64_t y0;
	int64_t last_x;
	int64_t last_y;
	size_t stride;
	unsigned i;

	/* Refuses a picture that has no frame or no plane to read. */
	if (ctx->picture == NULL)
		return 0;
	frame = ctx->picture->frame;
	if (frame == NULL)
		return 0;
	if (component > 2U)
		return 0;
	plane = frame->plane[component];
	if (plane == NULL)
		return 0;

	/* Takes the size of the component's plane: the chroma planes are half of the luma plane (4:2:0). */
	plane_width = (int64_t)frame->width;
	plane_height = (int64_t)frame->height;
	if (component != 0U) {
		plane_width = plane_width / 2;
		plane_height = plane_height / 2;
	}

	/* Takes the distance between rows of the plane. */
	stride = frame->stride[component];

	/* Places the first sample of the run in the picture: the macroblock's position plus the location (8-45, 8-77, 8-115, 8-131 for frames without MBAFF). */
	x0 = (int64_t)ctx->n.x * (int64_t)mb_size + (int64_t)xn;
	y0 = (int64_t)ctx->n.y * (int64_t)mb_size + (int64_t)yn;

	/* Places the last sample of the run. */
	last_x = x0;
	last_y = y0;
	if (vertical != 0)
		last_y = y0 + (int64_t)count - 1;
	else
		last_x = x0 + (int64_t)count - 1;

	/* Refuses a run that leaves the plane. */
	if (x0 < 0 || y0 < 0)
		return 0;
	if (last_x >= plane_width || last_y >= plane_height)
		return 0;

	/* Copies the samples of the run one by one. */
	for (i = 0; i < count; i++) {
		if (vertical != 0)
			out[i] = plane[(size_t)(y0 + (int64_t)i) * stride + (size_t)x0];
		else
			out[i] = plane[(size_t)y0 * stride + (size_t)(x0 + (int64_t)i)];
	}

	/* Succeeded: the run is in out. */
	return 1;
}

/*
 * Gathers the neighbouring samples of a block into an edge that is already cleared.
 *
 * Each of the left, top, top-left and top-right parts is marked available
 * when the macroblock holding it is available and not barred (see
 * intra_find_mb), and, for the top-right part of a block that has an
 * order, when the block holding it is not a later block of the current
 * macroblock (8.3.1.2: blocks 3 and 11 of the 4x4 blocks; the other blocks
 * whose top-right lies right of the macroblock below its top row are
 * refused by Table 6-3).
 */
static void
intra_gather(
	const struct h264sw_slice_ctx *ctx,
	const struct intra_block *block,
	struct h264sw_intra_edge *edge)
{
	struct h264sw_mb *found;
	unsigned xw;
	unsigned yw;
	unsigned block_at;
	unsigned later;
	int copied;

	/* Gathers the left column p[ -1, 0..N-1 ] (8.3.1.2, 8.3.2.2, 8.3.3, 8.3.4). */
	found = intra_find_mb(ctx, block->mb_size, block->x - 1, block->y, &xw, &yw);
	if (found != NULL) {
		/* Reads the column from the picture. */
		copied = intra_copy_run(ctx, block->component, block->mb_size, block->x - 1, block->y, 1, block->size, edge->left);
		if (copied != 0)
			edge->available = edge->available | H264SW_EDGE_LEFT;
	}

	/* Gathers the top row p[ 0..N-1, -1 ]. */
	found = intra_find_mb(ctx, block->mb_size, block->x, block->y - 1, &xw, &yw);
	if (found != NULL) {
		/* Reads the row from the picture. */
		copied = intra_copy_run(ctx, block->component, block->mb_size, block->x, block->y - 1, 0, block->size, edge->top);
		if (copied != 0)
			edge->available = edge->available | H264SW_EDGE_TOP;
	}

	/* Gathers the top-left sample p[ -1, -1 ]. */
	found = intra_find_mb(ctx, block->mb_size, block->x - 1, block->y - 1, &xw, &yw);
	if (found != NULL) {
		/* Reads the sample from the picture. */
		copied = intra_copy_run(ctx, block->component, block->mb_size, block->x - 1, block->y - 1, 0, 1U, &edge->top_left);
		if (copied != 0)
			edge->available = edge->available | H264SW_EDGE_TOP_LEFT;
	}

	/* Leaves out the top-right row for a block that has none. */
	if (block->right == 0U)
		return;

	/* Finds the macroblock holding the top-right row p[ N..2N-1, -1 ]. */
	found = intra_find_mb(ctx, block->mb_size, block->x + (int)block->size, block->y - 1, &xw, &yw);
	if (found == NULL)
		return;

	/* Finds out whether the top-right row lies in a block of the current macroblock that is decoded later. */
	later = 0;
	if (found == ctx->n.current) {
		if (block->order_size == 4U) {
			/* A 4x4 block is later when its luma4x4BlkIdx is higher (6.4.3). */
			block_at = h264sw_blk4x4_at(xw, yw);
			if (block_at > block->blk)
				later = 1;
		} else if (block->order_size == 8U) {
			/* An 8x8 block is later when its luma8x8BlkIdx is higher (6.4.5). */
			block_at = 2U * (yw / 8U) + (xw / 8U);
			if (block_at > block->blk)
				later = 1;
		}
	}

	/* Refuses a top-right row that is not decoded yet. */
	if (later != 0U)
		return;

	/* Gathers the top-right row, to the right of the top row. */
	copied = intra_copy_run(ctx, block->component, block->mb_size, block->x + (int)block->size, block->y - 1, 0, block->size, edge->top + block->size);
	if (copied != 0)
		edge->available = edge->available | H264SW_EDGE_TOP_RIGHT;
}

/*
 * Copies an edge into two arrays that have p[ -1, -1 ] in front, and says which parts are available.
 *
 * top_row[0] and left_col[0] are the top-left sample, top_row[1 + x] is
 * p[ x, -1 ] and left_col[1 + y] is p[ -1, y ], so a caller passes
 * top_row + 1 and left_col + 1 and reads index -1 as p[ -1, -1 ].  size is
 * the block's width and height; wide is 1 for a block that has a
 * top-right (4x4, 8x8): the top row has 2 * size samples, and when the
 * top is available but the top-right is not, the last top sample stands
 * for the top-right ones (8.3.1.2, 8.3.2.2).  A part that is not available
 * is 0.  The result keeps H264SW_EDGE_TOP, _LEFT and _TOP_LEFT; with
 * wide, the top bit stands for all of p[ 0..2N-1, -1 ].
 */
static unsigned
intra_load(
	const struct h264sw_intra_edge *edge,
	unsigned size,
	unsigned wide,
	uint8_t *top_row,
	uint8_t *left_col)
{
	unsigned available;
	unsigned top_count;
	unsigned i;

	/* Clears both arrays, so that a part that is not available reads as 0. */
	top_count = size;
	if (wide != 0U)
		top_count = 2U * size;
	memset(top_row, 0, top_count + 1U);
	memset(left_col, 0, size + 1U);

	/* Keeps the parts that the predictors ask about. */
	available = edge->available & (H264SW_EDGE_TOP | H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT);

	/* Copies the top-left sample into the front of both arrays. */
	if ((available & H264SW_EDGE_TOP_LEFT) != 0U) {
		top_row[0] = edge->top_left;
		left_col[0] = edge->top_left;
	}

	/* Copies the samples above the block. */
	if ((available & H264SW_EDGE_TOP) != 0U) {
		for (i = 0; i < size; i++)
			top_row[1U + i] = edge->top[i];
	}

	/* Copies the samples above and to the right of the block, or substitutes the last sample above the block for them when only those are absent. */
	if (wide != 0U && (available & H264SW_EDGE_TOP) != 0U) {
		for (i = size; i < 2U * size; i++) {
			if ((edge->available & H264SW_EDGE_TOP_RIGHT) != 0U)
				top_row[1U + i] = edge->top[i];
			else
				top_row[1U + i] = edge->top[size - 1U];
		}
	}

	/* Copies the left column. */
	if ((available & H264SW_EDGE_LEFT) != 0U) {
		for (i = 0; i < size; i++)
			left_col[1U + i] = edge->left[i];
	}

	/* Reports the parts that hold samples. */
	return available;
}

/* Fills a prediction of count samples with 128, the block given for a mode that cannot be used. */
static void
intra_fill_flat(
	uint8_t *pred,
	unsigned count)
{
	unsigned i;

	/* Sets every sample of the block. */
	for (i = 0; i < count; i++)
		pred[i] = (uint8_t)INTRA_FLAT;
}

/*
 * Shifts an integer right by shift bits as a division rounded down.
 *
 * The operator >> of the specification means an arithmetic shift, which
 * for a negative value C leaves to the implementation; this computes the
 * same result for every value.
 */
static int
intra_shift_floor(
	int value,
	unsigned shift)
{
	int rounding;

	/* Shifts a value that is not negative directly. */
	if (value >= 0)
		return value >> shift;

	/* Shifts a negative value through its magnitude, rounding the magnitude up so that the result rounds down. */
	rounding = (1 << shift) - 1;
	return -((-value + rounding) >> shift);
}

/* Clips a value to the range of an 8-bit sample (Clip1 with a bit depth of 8). */
static int
intra_clip1(
	int value)
{
	/* Raises a value below the range to the lowest sample. */
	if (value < 0)
		return 0;

	/* Lowers a value above the range to the highest sample. */
	if (value > INTRA_SAMPLE_MAX)
		return INTRA_SAMPLE_MAX;

	/* The value is a sample already. */
	return value;
}

/*
 * Filters the reference samples of an Intra_8x8 block (8.3.2.2.1).
 *
 * top and left are as for intra_pred_nxn with size 8 (index -1 is p[ -1,
 * -1 ]); the top row has 16 samples and available says which parts hold
 * samples.  filtered_top and filtered_left receive p' in the same layout.
 * A part that is not available is 0.
 */
static void
intra_filter_8x8(
	const uint8_t *top,
	const uint8_t *left,
	unsigned available,
	uint8_t *filtered_top,
	uint8_t *filtered_left)
{
	int x;
	int y;
	int value;
	int have_top;
	int have_left;
	int have_corner;

	/* Starts the filtered top row, corner included, from 0. */
	for (x = -1; x < 16; x++)
		filtered_top[x] = 0;

	/* Starts the filtered left column, corner included, from 0. */
	for (y = -1; y < 8; y++)
		filtered_left[y] = 0;

	/* Notes which parts are available. */
	have_top = 0;
	have_left = 0;
	have_corner = 0;
	if ((available & H264SW_EDGE_TOP) != 0U)
		have_top = 1;
	if ((available & H264SW_EDGE_LEFT) != 0U)
		have_left = 1;
	if ((available & H264SW_EDGE_TOP_LEFT) != 0U)
		have_corner = 1;

	/* Filters the top row when all of p[ 0..15, -1 ] are available (the top-right samples having been substituted when absent). */
	if (have_top != 0) {
		/* Filters the first sample with the corner when it is available (8-78), without it otherwise (8-79). */
		if (have_corner != 0)
			value = (top[-1] + 2 * top[0] + top[1] + 2) >> 2;
		else
			value = (3 * top[0] + top[1] + 2) >> 2;
		filtered_top[0] = (uint8_t)value;

		/* Filters the samples that have two neighbours in the row (8-80). */
		for (x = 1; x <= 14; x++) {
			value = (top[x - 1] + 2 * top[x] + top[x + 1] + 2) >> 2;
			filtered_top[x] = (uint8_t)value;
		}

		/* Filters the last sample, which has one neighbour (8-81). */
		value = (top[14] + 3 * top[15] + 2) >> 2;
		filtered_top[15] = (uint8_t)value;
	}

	/* Filters the corner sample when it is available, by the neighbours it has. */
	if (have_corner != 0) {
		if (have_top == 0 && have_left == 0) {
			/* With neither neighbour the corner is not filtered (8.3.2.2.1, after 8-83). */
			value = top[-1];
		} else if (have_left == 0) {
			/* With only the top row the corner leans on p[ 0, -1 ] (8-82). */
			value = (3 * top[-1] + top[0] + 2) >> 2;
		} else if (have_top == 0) {
			/* With only the left column the corner leans on p[ -1, 0 ] (8-83). */
			value = (3 * top[-1] + left[0] + 2) >> 2;
		} else {
			/* With both neighbours the corner is filtered between them (8-84). */
			value = (top[0] + 2 * top[-1] + left[0] + 2) >> 2;
		}
		filtered_top[-1] = (uint8_t)value;
		filtered_left[-1] = (uint8_t)value;
	}

	/* Filters the left column when all of p[ -1, 0..7 ] are available. */
	if (have_left != 0) {
		/* Filters the first sample with the corner when it is available (8-85), without it otherwise (8-86). */
		if (have_corner != 0)
			value = (left[-1] + 2 * left[0] + left[1] + 2) >> 2;
		else
			value = (3 * left[0] + left[1] + 2) >> 2;
		filtered_left[0] = (uint8_t)value;

		/* Filters the samples that have two neighbours in the column (8-87). */
		for (y = 1; y <= 6; y++) {
			value = (left[y - 1] + 2 * left[y] + left[y + 1] + 2) >> 2;
			filtered_left[y] = (uint8_t)value;
		}

		/* Filters the last sample, which has one neighbour (8-88). */
		value = (left[6] + 3 * left[7] + 2) >> 2;
		filtered_left[7] = (uint8_t)value;
	}
}

/* Predicts a block by copying the row above into every row (8-46, 8-89, 8-116, 8-143). */
static void
intra_pred_vertical(
	unsigned size,
	const uint8_t *top,
	uint8_t *pred)
{
	unsigned x;
	unsigned y;

	/* Gives every row of the block the row above it. */
	for (y = 0; y < size; y++) {
		for (x = 0; x < size; x++)
			pred[y * size + x] = top[x];
	}
}

/* Predicts a block by copying the left column into every column (8-47, 8-90, 8-117, 8-142). */
static void
intra_pred_horizontal(
	unsigned size,
	const uint8_t *left,
	uint8_t *pred)
{
	unsigned x;
	unsigned y;

	/* Gives every column of the block the column to its left. */
	for (y = 0; y < size; y++) {
		for (x = 0; x < size; x++)
			pred[y * size + x] = left[y];
	}
}

/*
 * Predicts a block of 4, 8 or 16 samples a side by the mean of the available neighbours.
 *
 * Both the top row and the left column give a mean of 2 * size samples
 * (8-48, 8-91, 8-118); one of them gives a mean of size samples (8-49,
 * 8-50, 8-92, 8-93, 8-119, 8-120); none gives 128 (8-51, 8-94, 8-121).
 */
static void
intra_pred_dc(
	unsigned size,
	const uint8_t *top,
	const uint8_t *left,
	unsigned available,
	uint8_t *pred)
{
	unsigned i;
	unsigned count;
	unsigned bits;
	int sum;
	int dc;

	/* Takes the number of bits that divide by size: 2 for 4, 3 for 8 and 4 for 16. */
	bits = 2;
	if (size == 8U)
		bits = 3;
	if (size == 16U)
		bits = 4;

	/* Adds up the samples above the block when they are available. */
	sum = 0;
	count = 0;
	if ((available & H264SW_EDGE_TOP) != 0U) {
		for (i = 0; i < size; i++)
			sum = sum + (int)top[i];
		count = count + 1U;
	}

	/* Adds up the samples to the left of the block when they are available. */
	if ((available & H264SW_EDGE_LEFT) != 0U) {
		for (i = 0; i < size; i++)
			sum = sum + (int)left[i];
		count = count + 1U;
	}

	/* Takes the mean with the rounding of the equation that matches how many sides are available. */
	if (count == 2U) {
		/* Both sides: ( sum + size ) >> ( bits + 1 ). */
		dc = (sum + (int)size) >> (bits + 1U);
	} else if (count == 1U) {
		/* One side: ( sum + size / 2 ) >> bits. */
		dc = (sum + (int)(size / 2U)) >> bits;
	} else {
		/* No side: the middle of the sample range. */
		dc = INTRA_FLAT;
	}

	/* Gives every sample of the block that value. */
	for (i = 0; i < size * size; i++)
		pred[i] = (uint8_t)dc;
}

/*
 * Predicts a block along the down-left diagonal (8-52, 8-53, 8-95, 8-96).
 *
 * top has 2 * size samples.
 */
static void
intra_pred_diagonal_down_left(
	unsigned size,
	const uint8_t *top,
	uint8_t *pred)
{
	int n;
	int x;
	int y;
	int value;

	/* Fills the block from the top row, including the samples to the right of it. */
	n = (int)size;
	for (y = 0; y < n; y++) {
		for (x = 0; x < n; x++) {
			if (x == n - 1 && y == n - 1) {
				/* The last sample leans on the last two samples of the top row (8-52, 8-95). */
				value = (top[2 * n - 2] + 3 * top[2 * n - 1] + 2) >> 2;
			} else {
				/* The others take the three samples along the diagonal (8-53, 8-96). */
				value = (top[x + y] + 2 * top[x + y + 1] + top[x + y + 2] + 2) >> 2;
			}
			pred[y * n + x] = (uint8_t)value;
		}
	}
}

/*
 * Predicts a block along the down-right diagonal (8-54 to 8-56, 8-97 to 8-99).
 *
 * top and left have index -1 as the corner sample.
 */
static void
intra_pred_diagonal_down_right(
	unsigned size,
	const uint8_t *top,
	const uint8_t *left,
	uint8_t *pred)
{
	int n;
	int x;
	int y;
	int value;

	/* Fills the block from the corner, the top row and the left column. */
	n = (int)size;
	for (y = 0; y < n; y++) {
		for (x = 0; x < n; x++) {
			if (x > y) {
				/* Above the diagonal the samples come from the top row (8-54, 8-97). */
				value = (top[x - y - 2] + 2 * top[x - y - 1] + top[x - y] + 2) >> 2;
			} else if (x < y) {
				/* Below the diagonal the samples come from the left column (8-55, 8-98). */
				value = (left[y - x - 2] + 2 * left[y - x - 1] + left[y - x] + 2) >> 2;
			} else {
				/* On the diagonal the samples lean on the corner (8-56, 8-99). */
				value = (top[0] + 2 * top[-1] + left[0] + 2) >> 2;
			}
			pred[y * n + x] = (uint8_t)value;
		}
	}
}

/*
 * Predicts a block along the vertical-right direction (8-57 to 8-60, 8-100 to 8-103).
 *
 * top and left have index -1 as the corner sample.  The cases of zVR below
 * -1 are written with 2 * x, which is 0 in the column 0 of a 4x4 block,
 * the only place those cases occur there (8-60 has no x).
 */
static void
intra_pred_vertical_right(
	unsigned size,
	const uint8_t *top,
	const uint8_t *left,
	uint8_t *pred)
{
	int n;
	int x;
	int y;
	int z;
	int i;
	int value;

	/* Fills the block by the position along the direction, zVR = 2 * x - y. */
	n = (int)size;
	for (y = 0; y < n; y++) {
		for (x = 0; x < n; x++) {
			z = 2 * x - y;
			i = x - (y >> 1);
			if (z >= 0 && (z % 2) == 0) {
				/* An even zVR is a half-sample position between two top samples (8-57, 8-100). */
				value = (top[i - 1] + top[i] + 1) >> 1;
			} else if (z >= 0) {
				/* An odd positive zVR is a sample between three top samples (8-58, 8-101). */
				value = (top[i - 2] + 2 * top[i - 1] + top[i] + 2) >> 2;
			} else if (z == -1) {
				/* A zVR of -1 is the sample next to the corner (8-59, 8-102). */
				value = (left[0] + 2 * left[-1] + top[0] + 2) >> 2;
			} else {
				/* A lower zVR comes from the left column (8-60, 8-103). */
				value = (left[y - 2 * x - 1] + 2 * left[y - 2 * x - 2] + left[y - 2 * x - 3] + 2) >> 2;
			}
			pred[y * n + x] = (uint8_t)value;
		}
	}
}

/*
 * Predicts a block along the horizontal-down direction (8-61 to 8-64, 8-104 to 8-107).
 *
 * top and left have index -1 as the corner sample.  The cases of zHD below
 * -1 are written with 2 * y, which is 0 in the row 0 of a 4x4 block, the
 * only place those cases occur there (8-64 has no y).
 */
static void
intra_pred_horizontal_down(
	unsigned size,
	const uint8_t *top,
	const uint8_t *left,
	uint8_t *pred)
{
	int n;
	int x;
	int y;
	int z;
	int i;
	int value;

	/* Fills the block by the position along the direction, zHD = 2 * y - x. */
	n = (int)size;
	for (y = 0; y < n; y++) {
		for (x = 0; x < n; x++) {
			z = 2 * y - x;
			i = y - (x >> 1);
			if (z >= 0 && (z % 2) == 0) {
				/* An even zHD is a half-sample position between two left samples (8-61, 8-104). */
				value = (left[i - 1] + left[i] + 1) >> 1;
			} else if (z >= 0) {
				/* An odd positive zHD is a sample between three left samples (8-62, 8-105). */
				value = (left[i - 2] + 2 * left[i - 1] + left[i] + 2) >> 2;
			} else if (z == -1) {
				/* A zHD of -1 is the sample next to the corner (8-63, 8-106). */
				value = (left[0] + 2 * left[-1] + top[0] + 2) >> 2;
			} else {
				/* A lower zHD comes from the top row (8-64, 8-107). */
				value = (top[x - 2 * y - 1] + 2 * top[x - 2 * y - 2] + top[x - 2 * y - 3] + 2) >> 2;
			}
			pred[y * n + x] = (uint8_t)value;
		}
	}
}

/*
 * Predicts a block along the vertical-left direction (8-65, 8-66, 8-108, 8-109).
 *
 * top has 2 * size samples.
 */
static void
intra_pred_vertical_left(
	unsigned size,
	const uint8_t *top,
	uint8_t *pred)
{
	int n;
	int x;
	int y;
	int i;
	int value;

	/* Fills the block row by row; each pair of rows moves one sample to the right. */
	n = (int)size;
	for (y = 0; y < n; y++) {
		for (x = 0; x < n; x++) {
			i = x + (y >> 1);
			if ((y % 2) == 0) {
				/* An even row is a half-sample position between two top samples (8-65, 8-108). */
				value = (top[i] + top[i + 1] + 1) >> 1;
			} else {
				/* An odd row is a sample between three top samples (8-66, 8-109). */
				value = (top[i] + 2 * top[i + 1] + top[i + 2] + 2) >> 2;
			}
			pred[y * n + x] = (uint8_t)value;
		}
	}
}

/* Predicts a block along the horizontal-up direction (8-67 to 8-70, 8-110 to 8-113). */
static void
intra_pred_horizontal_up(
	unsigned size,
	const uint8_t *left,
	uint8_t *pred)
{
	int n;
	int x;
	int y;
	int z;
	int i;
	int value;

	/* Fills the block by the position along the direction, zHU = x + 2 * y; the last position with a formula of its own is 2 * size - 3. */
	n = (int)size;
	for (y = 0; y < n; y++) {
		for (x = 0; x < n; x++) {
			z = x + 2 * y;
			i = y + (x >> 1);
			if (z < 2 * n - 3 && (z % 2) == 0) {
				/* An even zHU is a half-sample position between two left samples (8-67, 8-110). */
				value = (left[i] + left[i + 1] + 1) >> 1;
			} else if (z < 2 * n - 3) {
				/* An odd zHU is a sample between three left samples (8-68, 8-111). */
				value = (left[i] + 2 * left[i + 1] + left[i + 2] + 2) >> 2;
			} else if (z == 2 * n - 3) {
				/* The position next to the last leans on the last two left samples (8-69, 8-112). */
				value = (left[n - 2] + 3 * left[n - 1] + 2) >> 2;
			} else {
				/* Beyond it the samples repeat the last left sample (8-70, 8-113). */
				value = left[n - 1];
			}
			pred[y * n + x] = (uint8_t)value;
		}
	}
}

/*
 * Predicts a 4x4 or 8x8 block by one of the nine directional modes.
 *
 * top and left have index -1 as the corner sample; the top row has 2 *
 * size samples.  The caller has checked that the samples the mode needs
 * are available.  For size 8 the samples are the filtered ones.
 */
static void
intra_pred_nxn(
	unsigned size,
	unsigned mode,
	const uint8_t *top,
	const uint8_t *left,
	unsigned available,
	uint8_t *pred)
{
	/* Chooses the equations of the mode (Tables 8-2 and 8-3). */
	switch (mode) {
	case H264SW_I4_VERTICAL:
		intra_pred_vertical(size, top, pred);
		break;
	case H264SW_I4_HORIZONTAL:
		intra_pred_horizontal(size, left, pred);
		break;
	case H264SW_I4_DC:
		intra_pred_dc(size, top, left, available, pred);
		break;
	case H264SW_I4_DIAGONAL_DOWN_LEFT:
		intra_pred_diagonal_down_left(size, top, pred);
		break;
	case H264SW_I4_DIAGONAL_DOWN_RIGHT:
		intra_pred_diagonal_down_right(size, top, left, pred);
		break;
	case H264SW_I4_VERTICAL_RIGHT:
		intra_pred_vertical_right(size, top, left, pred);
		break;
	case H264SW_I4_HORIZONTAL_DOWN:
		intra_pred_horizontal_down(size, top, left, pred);
		break;
	case H264SW_I4_VERTICAL_LEFT:
		intra_pred_vertical_left(size, top, pred);
		break;
	default:
		/* The last mode, horizontal up; the callers refuse a mode above 8. */
		intra_pred_horizontal_up(size, left, pred);
		break;
	}
}

/*
 * Predicts a 16x16 luma block or an 8x8 chroma block by the plane through the neighbours (8-122 to 8-127, 8-144 to 8-149).
 *
 * size is 16 or 8; scale is the multiplier of H and V in b and c: 5 for the
 * luma block (8-124, 8-125) and 34 for the 4:2:0 chroma block (8-146,
 * 8-147).  top and left have index -1 as the corner sample.
 */
static void
intra_pred_plane(
	unsigned size,
	int scale,
	const uint8_t *top,
	const uint8_t *left,
	uint8_t *pred)
{
	int half;
	int i;
	int x;
	int y;
	int h_sum;
	int v_sum;
	int a;
	int b;
	int c;
	int value;

	/* Sums the weighted differences across the centre of the row and of the column (8-126, 8-127, 8-148, 8-149). */
	half = (int)(size / 2U);
	h_sum = 0;
	v_sum = 0;
	for (i = 0; i < half; i++) {
		h_sum = h_sum + (i + 1) * ((int)top[half + i] - (int)top[half - 2 - i]);
		v_sum = v_sum + (i + 1) * ((int)left[half + i] - (int)left[half - 2 - i]);
	}

	/* Takes the offset and the two gradients (8-123 to 8-125, 8-145 to 8-147). */
	a = 16 * ((int)left[size - 1U] + (int)top[size - 1U]);
	b = intra_shift_floor(scale * h_sum + 32, 6U);
	c = intra_shift_floor(scale * v_sum + 32, 6U);

	/* Evaluates the plane at each sample and clips it to the sample range (8-122, 8-144). */
	for (y = 0; y < (int)size; y++) {
		for (x = 0; x < (int)size; x++) {
			value = a + b * (x - (half - 1)) + c * (y - (half - 1)) + 16;
			value = intra_shift_floor(value, 5U);
			pred[y * (int)size + x] = (uint8_t)intra_clip1(value);
		}
	}
}

/*
 * Predicts an 8x8 chroma block of 4:2:0 by DC, one 4x4 block at a time (8-132 to 8-141).
 *
 * top and left have 8 samples each.  The blocks at the upper left and the
 * lower right use both neighbours, then the left, then the top; the block
 * at the upper right prefers the top; the block at the lower left prefers
 * the left.
 */
static void
intra_pred_chroma_dc(
	const uint8_t *top,
	const uint8_t *left,
	unsigned available,
	uint8_t *pred)
{
	unsigned blk;
	unsigned x_origin;
	unsigned y_origin;
	unsigned i;
	unsigned x;
	unsigned y;
	int top_sum;
	int left_sum;
	int have_top;
	int have_left;
	int dc;

	/* Notes which sides are available. */
	have_top = 0;
	have_left = 0;
	if ((available & H264SW_EDGE_TOP) != 0U)
		have_top = 1;
	if ((available & H264SW_EDGE_LEFT) != 0U)
		have_left = 1;

	/* Predicts each of the four chroma 4x4 blocks (chroma4x4BlkIdx 0..3, raster order, 6.4.7). */
	for (blk = 0; blk < 4U; blk++) {
		/* Places the block and sums the four neighbours above it and the four to its left. */
		x_origin = (blk % 2U) * 4U;
		y_origin = (blk / 2U) * 4U;
		top_sum = 0;
		left_sum = 0;
		for (i = 0; i < 4U; i++) {
			top_sum = top_sum + (int)top[x_origin + i];
			left_sum = left_sum + (int)left[y_origin + i];
		}

		/* Takes the mean the block's position prefers. */
		if (x_origin == y_origin) {
			/* The upper-left and lower-right blocks use both sides, else the left, else the top (8-132 to 8-135). */
			if (have_top != 0 && have_left != 0)
				dc = (top_sum + left_sum + 4) >> 3;
			else if (have_left != 0)
				dc = (left_sum + 2) >> 2;
			else if (have_top != 0)
				dc = (top_sum + 2) >> 2;
			else
				dc = INTRA_FLAT;
		} else if (y_origin == 0U) {
			/* The upper-right block prefers the top, else the left (8-136 to 8-138). */
			if (have_top != 0)
				dc = (top_sum + 2) >> 2;
			else if (have_left != 0)
				dc = (left_sum + 2) >> 2;
			else
				dc = INTRA_FLAT;
		} else {
			/* The lower-left block prefers the left, else the top (8-139 to 8-141). */
			if (have_left != 0)
				dc = (left_sum + 2) >> 2;
			else if (have_top != 0)
				dc = (top_sum + 2) >> 2;
			else
				dc = INTRA_FLAT;
		}

		/* Gives the four rows of four samples of the block that value. */
		for (y = 0; y < 4U; y++) {
			for (x = 0; x < 4U; x++)
				pred[(y_origin + y) * 8U + x_origin + x] = (uint8_t)dc;
		}
	}
}
