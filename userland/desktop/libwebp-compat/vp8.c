/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lossy decoder: one key frame into its planes.  Every macroblock is
 * read and reconstructed in raster order into planes that cover whole
 * macroblocks; the loop filter then runs over the whole frame, so that
 * intra prediction always sees unfiltered pixels.
 */

#include "vp8.h"

#include "bool.h"
#include "vp8-filter.h"
#include "vp8-header.h"
#include "vp8-idct.h"
#include "vp8-modes.h"
#include "vp8-predict.h"
#include "vp8-tables.h"
#include "vp8-tokens.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The value of the row above the frame and of the column left of it (RFC 6386 12.2, 12.3). */
#define VP8_ABOVE_OUTSIDE	127U
#define VP8_LEFT_OUTSIDE	129U

/*
 * What the loop filter needs of one macroblock once the frame is
 * reconstructed: its segment, whether it is B_PRED, and whether its
 * subblock edges are filtered (B_PRED, or any block coded).
 */
struct vp8_mb_filter {
	uint8_t segment;
	uint8_t b_pred;
	uint8_t inner;
};

/*
 * The state of one frame's decoding: the header, the partitions'
 * decoders, the picture being built, and the contexts the macroblocks
 * pass along (above per macroblock column, left for the current row).
 * It lives on the stack of webpc_vp8_decode; the arrays it points to are
 * allocated there and freed before it returns.
 */
struct vp8_frame {
	struct webpc_vp8_header header;
	struct webpc_bool first;
	struct webpc_bool tokens[WEBPC_VP8_PARTITIONS_MAX];
	struct webpc_vp8_picture *picture;
	uint8_t *above_bmodes;
	uint8_t *above_contexts;
	uint8_t left_bmodes[4];
	uint8_t left_contexts[WEBPC_VP8_CONTEXTS];
	struct vp8_mb_filter *filters;
};

static int vp8_allocate(struct vp8_frame *frame);
static void vp8_release_contexts(struct vp8_frame *frame);
static int vp8_decode_rows(struct vp8_frame *frame);
static void vp8_macroblock(struct vp8_frame *frame, unsigned mb_x, unsigned mb_y, const struct webpc_vp8_modes *modes,
	struct webpc_vp8_residue *residue);
static void vp8_edges(const uint8_t *plane, ptrdiff_t stride, unsigned size, unsigned x, unsigned y,
	struct webpc_vp8_edges *edges);
static void vp8_above_right(const struct vp8_frame *frame, unsigned mb_x, unsigned mb_y, uint8_t right[4]);
static void vp8_luma_subblocks(const struct webpc_vp8_modes *modes, const struct webpc_vp8_edges *edges,
	const uint8_t right[4], const struct webpc_vp8_residue *residue, uint8_t *dst, ptrdiff_t stride);
static void vp8_add_residue(const int16_t coeffs[16], uint8_t *dst, ptrdiff_t stride);
static void vp8_filter_frame(struct vp8_frame *frame);

/*
 * Decodes a key frame (data from its frame tag, size bytes) into a new
 * picture.
 */
int
webpc_vp8_decode(
	const uint8_t *data,
	size_t size,
	int bypass_filtering,
	struct webpc_vp8_picture *picture)
{
	struct vp8_frame frame;
	unsigned index;
	int error;

	/* Nothing is handed out until the decoding has succeeded. */
	memset(picture, 0, sizeof(*picture));
	memset(&frame, 0, sizeof(frame));
	frame.picture = picture;

	/* The frame tag and size, then the rest of the header from the first partition. */
	error = webpc_vp8_read_tag(data, size, &frame.header);
	if (error != 0)
		return error;
	error = webpc_vp8_read_header(data, size, &frame.header, &frame.first);
	if (error != 0)
		return error;

	/* One decoder per token partition (an empty one is ended from the start). */
	for (index = 0; index < frame.header.partition_count; index++)
		webpc_bool_init(&frame.tokens[index], frame.header.partitions[index], frame.header.partition_sizes[index]);

	/* The planes and the contexts. */
	error = vp8_allocate(&frame);
	if (error != 0) {
		vp8_release_contexts(&frame);
		webpc_vp8_picture_release(picture);
		return error;
	}

	/* Every macroblock, reconstructed in raster order. */
	error = vp8_decode_rows(&frame);
	if (error != 0) {
		vp8_release_contexts(&frame);
		webpc_vp8_picture_release(picture);
		return error;
	}

	/*
	 * The loop filter over the whole frame, unless the caller bypasses it
	 * or the frame's level is 0 (then nothing is filtered, whatever the
	 * segments say; RFC 6386 15).
	 */
	if (bypass_filtering == 0 && frame.header.filter.level != 0)
		vp8_filter_frame(&frame);

	/* Succeeded: the picture holds the frame; the contexts are done with. */
	vp8_release_contexts(&frame);
	return 0;
}

/*
 * Frees a picture's planes and clears it; a cleared picture is ignored.
 */
void
webpc_vp8_picture_release(
	struct webpc_vp8_picture *picture)
{
	/* The one allocation the planes are in, and nothing left pointing at it. */
	free(picture->memory);
	memset(picture, 0, sizeof(*picture));
}

/* Allocates the planes (whole macroblocks) and the contexts of a frame whose header is read. */
static int
vp8_allocate(
	struct vp8_frame *frame)
{
	struct webpc_vp8_picture *picture;
	size_t luma_size;
	size_t chroma_size;
	size_t count;
	uint8_t *memory;

	/* The size in macroblocks; the header has checked width and height are 1..16383. */
	picture = frame->picture;
	picture->width = frame->header.width;
	picture->height = frame->header.height;
	picture->mb_width = (picture->width + 15U) / 16U;
	picture->mb_height = (picture->height + 15U) / 16U;
	picture->y_stride = (ptrdiff_t)picture->mb_width * 16;
	picture->uv_stride = (ptrdiff_t)picture->mb_width * 8;

	/* The three planes in one allocation: luma, then U, then V. */
	luma_size = (size_t)picture->mb_width * 16U * (size_t)picture->mb_height * 16U;
	chroma_size = luma_size / 4U;
	memory = malloc(luma_size + 2U * chroma_size);
	if (memory == NULL)
		return ENOMEM;
	picture->memory = memory;
	picture->y = memory;
	picture->u = memory + luma_size;
	picture->v = memory + luma_size + chroma_size;

	/* The above contexts: subblock modes start as B_DC_PRED, token contexts as empty (RFC 6386 11.3, 13.3). */
	count = (size_t)picture->mb_width;
	frame->above_bmodes = malloc(count * 4U);
	if (frame->above_bmodes == NULL)
		return ENOMEM;
	memset(frame->above_bmodes, (int)WEBPC_VP8_B_DC_PRED, count * 4U);

	frame->above_contexts = calloc(count, WEBPC_VP8_CONTEXTS);
	if (frame->above_contexts == NULL)
		return ENOMEM;

	/* What the loop filter needs of each macroblock. */
	frame->filters = calloc(count * (size_t)picture->mb_height, sizeof(*frame->filters));
	if (frame->filters == NULL)
		return ENOMEM;

	/* Succeeded: the frame can be decoded. */
	return 0;
}

/* Frees a frame's contexts (not its picture). */
static void
vp8_release_contexts(
	struct vp8_frame *frame)
{
	/* Each array, and nothing left pointing at it. */
	free(frame->above_bmodes);
	free(frame->above_contexts);
	free(frame->filters);
	frame->above_bmodes = NULL;
	frame->above_contexts = NULL;
	frame->filters = NULL;
}

/*
 * Reads and reconstructs every macroblock in raster order; ENOBUFS when a
 * partition runs past its end.
 */
static int
vp8_decode_rows(
	struct vp8_frame *frame)
{
	struct webpc_vp8_residue residue;
	struct webpc_vp8_modes modes;
	struct webpc_bool *tokens;
	struct vp8_mb_filter *filter;
	uint8_t *above_contexts;
	unsigned mb_x;
	unsigned mb_y;
	unsigned mb_width;
	int has_y2;

	mb_width = frame->picture->mb_width;

	/* Each row of macroblocks, its tokens from partition mb_y % partition_count (RFC 6386 9.5). */
	for (mb_y = 0; mb_y < frame->picture->mb_height; mb_y++) {
		tokens = &frame->tokens[mb_y % frame->header.partition_count];

		/* A row starts with left contexts as left of the frame. */
		memset(frame->left_bmodes, (int)WEBPC_VP8_B_DC_PRED, sizeof(frame->left_bmodes));
		memset(frame->left_contexts, 0, sizeof(frame->left_contexts));

		/* Each macroblock: header, residue, reconstruction. */
		for (mb_x = 0; mb_x < mb_width; mb_x++) {
			webpc_vp8_read_modes(&frame->first, &frame->header, &frame->above_bmodes[(size_t)mb_x * 4U],
				frame->left_bmodes, &modes);
			has_y2 = 1;
			if (modes.ymode == WEBPC_VP8_B_PRED)
				has_y2 = 0;
			above_contexts = &frame->above_contexts[(size_t)mb_x * WEBPC_VP8_CONTEXTS];

			/* A skipped macroblock codes nothing; its contexts are reset as RFC 6386 20.16 does. */
			if (modes.skip) {
				memset(&residue, 0, sizeof(residue));
				webpc_vp8_skip_residue(has_y2, frame->left_contexts, above_contexts);
			} else {
				webpc_vp8_read_residue(tokens, &frame->header, &frame->header.dequant[modes.segment], has_y2,
					frame->left_contexts, above_contexts, &residue);
			}

			/* A token partition read past its end fails the frame (as libwebp fails it). */
			if (webpc_bool_ended(tokens))
				return ENOBUFS;

			/* The pixels, and what the loop filter will need. */
			vp8_macroblock(frame, mb_x, mb_y, &modes, &residue);
			filter = &frame->filters[(size_t)mb_y * mb_width + mb_x];
			filter->segment = (uint8_t)modes.segment;
			filter->b_pred = (uint8_t)(has_y2 ? 0 : 1);

			/* The subblock edges are filtered for B_PRED and for a macroblock with any block coded (RFC 6386 15.1). */
			filter->inner = 0;
			if (has_y2 == 0)
				filter->inner = 1;
			if ((residue.coded & 0x80000000UL) != 0U)
				filter->inner = 1;
		}

		/* The first partition read past its end within the row fails the frame. */
		if (webpc_bool_ended(&frame->first))
			return ENOBUFS;
	}

	/* Succeeded: every macroblock is reconstructed. */
	return 0;
}

/* Reconstructs one macroblock: the WHT, the luma and chroma predictions and their residue. */
static void
vp8_macroblock(
	struct vp8_frame *frame,
	unsigned mb_x,
	unsigned mb_y,
	const struct webpc_vp8_modes *modes,
	struct webpc_vp8_residue *residue)
{
	struct webpc_vp8_picture *picture;
	struct webpc_vp8_edges edges;
	int16_t dc[16];
	uint8_t right[4];
	uint8_t *y;
	uint8_t *u;
	uint8_t *v;
	unsigned block;

	picture = frame->picture;
	y = picture->y + (ptrdiff_t)mb_y * 16 * picture->y_stride + (ptrdiff_t)mb_x * 16;
	u = picture->u + (ptrdiff_t)mb_y * 8 * picture->uv_stride + (ptrdiff_t)mb_x * 8;
	v = picture->v + (ptrdiff_t)mb_y * 8 * picture->uv_stride + (ptrdiff_t)mb_x * 8;

	/* A macroblock with Y2 gets the DC of its Y blocks from the WHT (RFC 6386 14.3). */
	if (modes->ymode != WEBPC_VP8_B_PRED) {
		webpc_vp8_iwht(residue->coeffs[WEBPC_VP8_BLOCK_Y2], dc);
		for (block = 0; block < 16U; block++)
			residue->coeffs[WEBPC_VP8_BLOCK_Y + block][0] = dc[block];
	}

	/* Luma: one 16x16 prediction and its 16 residue blocks, or 16 subblocks each predicted then summed. */
	vp8_edges(picture->y, picture->y_stride, 16U, mb_x * 16U, mb_y * 16U, &edges);
	if (modes->ymode != WEBPC_VP8_B_PRED) {
		webpc_vp8_predict_block(modes->ymode, 16U, &edges, y, picture->y_stride);
		for (block = 0; block < 16U; block++)
			vp8_add_residue(residue->coeffs[WEBPC_VP8_BLOCK_Y + block],
				y + (ptrdiff_t)(block / 4U) * 4 * picture->y_stride + (ptrdiff_t)(block % 4U) * 4,
				picture->y_stride);
	} else {
		vp8_above_right(frame, mb_x, mb_y, right);
		vp8_luma_subblocks(modes, &edges, right, residue, y, picture->y_stride);
	}

	/* Chroma: U and V alike, each one 8x8 prediction and four residue blocks. */
	vp8_edges(picture->u, picture->uv_stride, 8U, mb_x * 8U, mb_y * 8U, &edges);
	webpc_vp8_predict_block(modes->uvmode, 8U, &edges, u, picture->uv_stride);
	vp8_edges(picture->v, picture->uv_stride, 8U, mb_x * 8U, mb_y * 8U, &edges);
	webpc_vp8_predict_block(modes->uvmode, 8U, &edges, v, picture->uv_stride);
	for (block = 0; block < 4U; block++) {
		vp8_add_residue(residue->coeffs[WEBPC_VP8_BLOCK_U + block],
			u + (ptrdiff_t)(block / 2U) * 4 * picture->uv_stride + (ptrdiff_t)(block % 2U) * 4, picture->uv_stride);
		vp8_add_residue(residue->coeffs[WEBPC_VP8_BLOCK_V + block],
			v + (ptrdiff_t)(block / 2U) * 4 * picture->uv_stride + (ptrdiff_t)(block % 2U) * 4, picture->uv_stride);
	}
}

/*
 * Gathers the edges of the size by size block at (x, y) of a plane from
 * its unfiltered pixels, with the out-of-frame values: 127 above the
 * frame (the corner too on the top row), 129 left of it (the corner too
 * in the left column below the top row) (RFC 6386 12.2, 12.3).
 */
static void
vp8_edges(
	const uint8_t *plane,
	ptrdiff_t stride,
	unsigned size,
	unsigned x,
	unsigned y,
	struct webpc_vp8_edges *edges)
{
	const uint8_t *above;
	unsigned index;

	/* The row above: the frame's, or 127 above the frame. */
	edges->have_above = 0;
	if (y > 0U) {
		edges->have_above = 1;
		above = plane + (ptrdiff_t)(y - 1U) * stride + (ptrdiff_t)x;
		memcpy(edges->above, above, size);
	} else {
		memset(edges->above, (int)VP8_ABOVE_OUTSIDE, size);
	}

	/* The column on the left: the frame's, or 129 left of the frame. */
	edges->have_left = 0;
	if (x > 0U)
		edges->have_left = 1;
	for (index = 0; index < size; index++) {
		if (x > 0U)
			edges->left[index] = plane[(ptrdiff_t)(y + index) * stride + (ptrdiff_t)x - 1];
		else
			edges->left[index] = (uint8_t)VP8_LEFT_OUTSIDE;
	}

	/* The corner: 127 on the top row, 129 in the left column below it, else the frame's pixel. */
	if (y == 0U)
		edges->corner = (uint8_t)VP8_ABOVE_OUTSIDE;
	else if (x == 0U)
		edges->corner = (uint8_t)VP8_LEFT_OUTSIDE;
	else
		edges->corner = plane[(ptrdiff_t)(y - 1U) * stride + (ptrdiff_t)x - 1];
}

/*
 * Gathers the 4 pixels above and to the right of a macroblock, which the
 * right column of its subblocks uses (RFC 6386 12.3): row -1 columns 16 to
 * 19; for the rightmost macroblock below the top row, row -1 column 15
 * four times; on the top row, 127.
 */
static void
vp8_above_right(
	const struct vp8_frame *frame,
	unsigned mb_x,
	unsigned mb_y,
	uint8_t right[4])
{
	const struct webpc_vp8_picture *picture;
	const uint8_t *row;

	picture = frame->picture;

	/* Above the frame. */
	if (mb_y == 0U) {
		memset(right, (int)VP8_ABOVE_OUTSIDE, 4U);
		return;
	}

	/* The row above, from this macroblock's first column. */
	row = picture->y + (ptrdiff_t)(mb_y * 16U - 1U) * picture->y_stride + (ptrdiff_t)mb_x * 16;

	/* The rightmost macroblock repeats the last pixel above it; the others take the next macroblock's. */
	if (mb_x + 1U >= picture->mb_width)
		memset(right, row[15], 4U);
	else
		memcpy(right, row + 16, 4U);
}

/*
 * Predicts and sums the 16 luma subblocks of a B_PRED macroblock in raster
 * order, each from the macroblock's edges and the subblocks already
 * reconstructed (RFC 6386 12.3).
 */
static void
vp8_luma_subblocks(
	const struct webpc_vp8_modes *modes,
	const struct webpc_vp8_edges *edges,
	const uint8_t right[4],
	const struct webpc_vp8_residue *residue,
	uint8_t *dst,
	ptrdiff_t stride)
{
	uint8_t above[9];
	uint8_t left[4];
	uint8_t *sub;
	unsigned block;
	unsigned bx;
	unsigned by;
	unsigned index;

	for (block = 0; block < 16U; block++) {
		bx = block % 4U;
		by = block / 4U;
		sub = dst + (ptrdiff_t)by * 4 * stride + (ptrdiff_t)bx * 4;

		/* The 4 pixels above: the macroblock's edge on the first row of subblocks, else the row just reconstructed. */
		for (index = 0; index < 4U; index++) {
			if (by == 0U)
				above[1U + index] = edges->above[bx * 4U + index];
			else
				above[1U + index] = sub[(ptrdiff_t)index - stride];
		}

		/* The 4 above-right: the macroblock's above-right for the right column on every row (12.3). */
		for (index = 0; index < 4U; index++) {
			if (bx == 3U)
				above[5U + index] = right[index];
			else if (by == 0U)
				above[5U + index] = edges->above[bx * 4U + 4U + index];
			else
				above[5U + index] = sub[(ptrdiff_t)(4U + index) - stride];
		}

		/* The column on the left: the macroblock's edge in the first column, else the subblock reconstructed there. */
		for (index = 0; index < 4U; index++) {
			if (bx == 0U)
				left[index] = edges->left[by * 4U + index];
			else
				left[index] = sub[(ptrdiff_t)index * stride - 1];
		}

		/* The corner, from the edges where the subblock touches them. */
		if (bx == 0U && by == 0U)
			above[0] = edges->corner;
		else if (by == 0U)
			above[0] = edges->above[bx * 4U - 1U];
		else if (bx == 0U)
			above[0] = edges->left[by * 4U - 1U];
		else
			above[0] = sub[-stride - 1];

		/* The prediction, then the subblock's residue on it. */
		webpc_vp8_predict_subblock(modes->bmodes[block], above + 1, left, sub, stride);
		vp8_add_residue(residue->coeffs[WEBPC_VP8_BLOCK_Y + block], sub, stride);
	}
}

/*
 * Adds one block's residue to its prediction; a block whose 16
 * coefficients are all 0 adds nothing and is skipped.
 */
static void
vp8_add_residue(
	const int16_t coeffs[16],
	uint8_t *dst,
	ptrdiff_t stride)
{
	unsigned index;

	/* Looks for a coefficient that is not 0. */
	for (index = 0; index < 16U; index++) {
		if (coeffs[index] != 0)
			break;
	}

	/* All zero: the prediction stays as it is. */
	if (index == 16U)
		return;

	/* The inverse DCT, summed with the prediction. */
	webpc_vp8_idct_add(coeffs, dst, stride);
}

/* Runs the loop filter over every macroblock in raster order (RFC 6386 15.1). */
static void
vp8_filter_frame(
	struct vp8_frame *frame)
{
	struct webpc_vp8_filter_params params;
	const struct vp8_mb_filter *filter;
	struct webpc_vp8_picture *picture;
	unsigned mb_x;
	unsigned mb_y;

	picture = frame->picture;

	/* Each macroblock with its own controls. */
	for (mb_y = 0; mb_y < picture->mb_height; mb_y++) {
		for (mb_x = 0; mb_x < picture->mb_width; mb_x++) {
			filter = &frame->filters[(size_t)mb_y * picture->mb_width + mb_x];
			webpc_vp8_filter_params(&frame->header, filter->segment, filter->b_pred, &params);
			webpc_vp8_filter_macroblock(picture->y + (ptrdiff_t)mb_y * 16 * picture->y_stride + (ptrdiff_t)mb_x * 16,
				picture->u + (ptrdiff_t)mb_y * 8 * picture->uv_stride + (ptrdiff_t)mb_x * 8,
				picture->v + (ptrdiff_t)mb_y * 8 * picture->uv_stride + (ptrdiff_t)mb_x * 8,
				picture->y_stride, picture->uv_stride, mb_x, mb_y, &params, frame->header.filter.simple,
				filter->inner);
		}
	}
}
