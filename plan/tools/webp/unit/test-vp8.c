/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8.c: small key frames built with boolwriter.h from RFC
 * 6386 19.1 to 19.3 (frame tag, header, macroblock headers, tokens), whose
 * pixels are worked out by hand from 12, 14.3 and 14.4: the out-of-frame
 * edge values and corners, a WHT DC carried to every pixel, partitions
 * taken by rows, and frames cut short.
 */

#include "check.h"
#include "boolwriter.h"
#include "vp8.h"
#include "vp8-tables.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most bytes a test frame takes. */
#define FRAME_BYTES	131072U

/*
 * One macroblock of a test frame: skipped (no tokens) or with one Y2 DC
 * value, written with the Y2 context y2_context (the sum of the Y2
 * contexts of the macroblocks above and on the left that coded a Y2
 * block, 13.3); its modes.
 */
struct test_mb {
	int skip;
	int y2_value;
	unsigned y2_context;
	unsigned ymode;
	unsigned bmode;
	unsigned uvmode;
};

/* A frame being built: the first partition, the token partitions and the assembled bytes. */
struct test_frame {
	struct boolwriter first;
	struct boolwriter tokens[2];
	uint8_t bytes[FRAME_BYTES];
	size_t size;
};

static void frame_header(struct boolwriter *writer, unsigned partitions_log2, unsigned filter_level);
static void frame_mb_header(struct boolwriter *writer, const struct test_mb *mb);
static void frame_mb_tokens(struct boolwriter *writer, const struct test_mb *mb);
static int frame_build(struct test_frame *frame, unsigned width, unsigned height, unsigned partitions_log2,
	unsigned filter_level, const struct test_mb *mbs);
static int plane_is(const uint8_t *plane, ptrdiff_t stride, unsigned x, unsigned y, unsigned size, unsigned value);
static void test_flat(void);
static void test_edges(void);
static void test_residue(void);
static void test_partitions(void);
static void test_cut(void);

/*
 * Runs the tests of vp8.c.
 */
int
main(
	void)
{
	int status;

	/* Each group of frames in turn. */
	test_flat();
	test_edges();
	test_residue();
	test_partitions();
	test_cut();

	/* The summary line. */
	status = check_finish("vp8");
	return status;
}

/*
 * Writes a key frame's header (RFC 6386 19.2): no segmentation, the normal
 * filter at filter_level, sharpness 0, no deltas, 1 << partitions_log2
 * partitions, quantizer index 0 without deltas, no probability update, and
 * the skip flag enabled with probability 128.
 */
static void
frame_header(
	struct boolwriter *writer,
	unsigned partitions_log2,
	unsigned filter_level)
{
	unsigned i;
	unsigned j;
	unsigned k;
	unsigned l;

	/* color_space, clamping_type, segmentation_enabled, filter_type, loop_filter_level, sharpness_level. */
	boolwriter_literal(writer, 0U, 1U);
	boolwriter_literal(writer, 0U, 1U);
	boolwriter_literal(writer, 0U, 1U);
	boolwriter_literal(writer, 0U, 1U);
	boolwriter_literal(writer, filter_level, 6U);
	boolwriter_literal(writer, 0U, 3U);

	/* loop_filter_adj_enable, log2_nbr_of_dct_partitions, y_ac_qi and the five delta flags. */
	boolwriter_literal(writer, 0U, 1U);
	boolwriter_literal(writer, partitions_log2, 2U);
	boolwriter_literal(writer, 0U, 7U);
	for (i = 0; i < 5U; i++)
		boolwriter_literal(writer, 0U, 1U);

	/* refresh_entropy_probs, then no token probability updated (13.4). */
	boolwriter_literal(writer, 0U, 1U);
	for (i = 0; i < WEBPC_VP8_COEFF_TYPES; i++)
		for (j = 0; j < WEBPC_VP8_COEFF_BANDS; j++)
			for (k = 0; k < WEBPC_VP8_COEFF_CONTEXTS; k++)
				for (l = 0; l < WEBPC_VP8_COEFF_NODES; l++)
					boolwriter_put(writer, webpc_vp8_coeff_update[i][j][k][l], 0);

	/* mb_no_coeff_skip and prob_skip_false. */
	boolwriter_literal(writer, 1U, 1U);
	boolwriter_literal(writer, 128U, 8U);
}

/*
 * Writes one macroblock's header (19.3): skip, the luma mode, for B_PRED
 * the same subblock mode 16 times (each with the context of the subblocks
 * above and on the left, which within the macroblock is that mode and at
 * the frame's top-left edges B_DC_PRED), and the chroma mode.  Only frames
 * whose B_PRED macroblock is the first of its row and column use B_PRED.
 */
static void
frame_mb_header(
	struct boolwriter *writer,
	const struct test_mb *mb)
{
	unsigned block;
	unsigned above;
	unsigned left;

	/* mb_skip_coeff, then the luma mode. */
	boolwriter_put(writer, 128U, mb->skip);
	boolwriter_tree(writer, webpc_vp8_kf_ymode_tree, 8U, webpc_vp8_kf_ymode_prob, 0U, (int)mb->ymode);

	/* The subblock modes of a B_PRED macroblock in the top-left of the frame. */
	if (mb->ymode == WEBPC_VP8_B_PRED) {
		for (block = 0; block < 16U; block++) {
			above = block < 4U ? WEBPC_VP8_B_DC_PRED : mb->bmode;
			left = block % 4U == 0U ? WEBPC_VP8_B_DC_PRED : mb->bmode;
			boolwriter_tree(writer, webpc_vp8_bmode_tree, 18U, webpc_vp8_kf_bmode_prob[above][left], 0U,
				(int)mb->bmode);
		}
	}

	/* The chroma mode. */
	boolwriter_tree(writer, webpc_vp8_uv_mode_tree, 6U, webpc_vp8_kf_uv_mode_prob, 0U, (int)mb->uvmode);
}

/*
 * Writes one macroblock's tokens with the default probabilities (13.2,
 * 13.3): a Y2 block whose only coefficient is y2_value at position 0, the
 * Y blocks (from position 1) and the chroma blocks all an immediate EOB.
 * The Y, U and V contexts are 0 (an immediate EOB leaves its context 0);
 * the Y2 context is the macroblock's y2_context.
 */
static void
frame_mb_tokens(
	struct boolwriter *writer,
	const struct test_mb *mb)
{
	const uint8_t *probs;
	unsigned block;

	/* Y2: the value at position 0 (band 0, its context), then EOB at position 1 (band 1, context 2 or 1). */
	probs = webpc_vp8_coeff_default[WEBPC_VP8_TYPE_Y2][0][mb->y2_context];
	boolwriter_tree(writer, webpc_vp8_coeff_tree, 22U, probs, 0U, mb->y2_value);
	boolwriter_put(writer, 128U, 0);
	probs = webpc_vp8_coeff_default[WEBPC_VP8_TYPE_Y2][webpc_vp8_coeff_bands[1]][mb->y2_value == (int)WEBPC_VP8_DCT_1 ? 1 : 2];
	boolwriter_tree(writer, webpc_vp8_coeff_tree, 22U, probs, 0U, (int)WEBPC_VP8_DCT_EOB);

	/* The 16 Y blocks from position 1, each an immediate EOB (band 1, context 0). */
	for (block = 0; block < 16U; block++) {
		probs = webpc_vp8_coeff_default[WEBPC_VP8_TYPE_Y_AFTER_Y2][webpc_vp8_coeff_bands[1]][0];
		boolwriter_tree(writer, webpc_vp8_coeff_tree, 22U, probs, 0U, (int)WEBPC_VP8_DCT_EOB);
	}

	/* The 8 chroma blocks, each an immediate EOB (band 0, context 0). */
	for (block = 0; block < 8U; block++) {
		probs = webpc_vp8_coeff_default[WEBPC_VP8_TYPE_CHROMA][0][0];
		boolwriter_tree(writer, webpc_vp8_coeff_tree, 22U, probs, 0U, (int)WEBPC_VP8_DCT_EOB);
	}
}

/*
 * Builds a whole key frame (19.1): the tag, start code and size, the first
 * partition, the partition sizes and the token partitions (row r's tokens
 * in partition r % count).  mbs lists the macroblocks in raster order.
 * Returns 0, or 1 when a writer overflowed.
 */
static int
frame_build(
	struct test_frame *frame,
	unsigned width,
	unsigned height,
	unsigned partitions_log2,
	unsigned filter_level,
	const struct test_mb *mbs)
{
	unsigned mb_width;
	unsigned mb_height;
	unsigned count;
	unsigned index;
	unsigned partition;
	size_t first_size;
	size_t sizes[2];
	size_t at;
	uint32_t tag;

	/* The partitions' contents. */
	mb_width = (width + 15U) / 16U;
	mb_height = (height + 15U) / 16U;
	count = 1U << partitions_log2;
	boolwriter_init(&frame->first);
	boolwriter_init(&frame->tokens[0]);
	boolwriter_init(&frame->tokens[1]);
	frame_header(&frame->first, partitions_log2, filter_level);
	for (index = 0; index < mb_width * mb_height; index++) {
		frame_mb_header(&frame->first, &mbs[index]);
		if (!mbs[index].skip)
			frame_mb_tokens(&frame->tokens[(index / mb_width) % count], &mbs[index]);
	}
	first_size = boolwriter_finish(&frame->first);
	sizes[0] = boolwriter_finish(&frame->tokens[0]);
	sizes[1] = boolwriter_finish(&frame->tokens[1]);
	if (frame->first.overflow || frame->tokens[0].overflow || frame->tokens[1].overflow)
		return 1;

	/* The tag: key frame, version 0, shown, the first partition's size (19.1). */
	tag = (uint32_t)(1U << 4) | ((uint32_t)first_size << 5);
	frame->bytes[0] = (uint8_t)(tag & 0xffU);
	frame->bytes[1] = (uint8_t)((tag >> 8) & 0xffU);
	frame->bytes[2] = (uint8_t)((tag >> 16) & 0xffU);
	frame->bytes[3] = 0x9dU;
	frame->bytes[4] = 0x01U;
	frame->bytes[5] = 0x2aU;
	frame->bytes[6] = (uint8_t)(width & 0xffU);
	frame->bytes[7] = (uint8_t)(width >> 8);
	frame->bytes[8] = (uint8_t)(height & 0xffU);
	frame->bytes[9] = (uint8_t)(height >> 8);
	at = 10U;

	/* The first partition, the sizes of all partitions but the last, the partitions. */
	memcpy(frame->bytes + at, frame->first.bytes, first_size);
	at += first_size;
	for (partition = 0; partition + 1U < count; partition++) {
		frame->bytes[at] = (uint8_t)(sizes[partition] & 0xffU);
		frame->bytes[at + 1U] = (uint8_t)((sizes[partition] >> 8) & 0xffU);
		frame->bytes[at + 2U] = (uint8_t)((sizes[partition] >> 16) & 0xffU);
		at += 3U;
	}
	for (partition = 0; partition < count; partition++) {
		memcpy(frame->bytes + at, frame->tokens[partition].bytes, sizes[partition]);
		at += sizes[partition];
	}
	frame->size = at;
	return 0;
}

/* Tells whether every pixel of the size by size square at (x, y) of a plane is value. */
static int
plane_is(
	const uint8_t *plane,
	ptrdiff_t stride,
	unsigned x,
	unsigned y,
	unsigned size,
	unsigned value)
{
	unsigned row;
	unsigned column;

	for (row = 0; row < size; row++)
		for (column = 0; column < size; column++)
			if (plane[(ptrdiff_t)(y + row) * stride + (ptrdiff_t)(x + column)] != value)
				return 0;
	return 1;
}

/* A 16x16 frame, skipped, DC_PRED everywhere: no neighbour, so 128 in every plane (12.2, 12.3). */
static void
test_flat(void)
{
	static struct test_frame frame;
	struct webpc_vp8_picture picture;
	struct test_mb mbs[1];
	int error;

	/* The frame, then its planes. */
	memset(mbs, 0, sizeof(mbs));
	mbs[0].skip = 1;
	mbs[0].ymode = WEBPC_VP8_DC_PRED;
	mbs[0].uvmode = WEBPC_VP8_DC_PRED;
	CHECK(frame_build(&frame, 16U, 16U, 0U, 0U, mbs) == 0);
	error = webpc_vp8_decode(frame.bytes, frame.size, 0, &picture);
	CHECK(error == 0);
	if (error != 0)
		return;
	CHECK(picture.width == 16U && picture.height == 16U && picture.mb_width == 1U && picture.mb_height == 1U);
	CHECK(plane_is(picture.y, picture.y_stride, 0U, 0U, 16U, 128U));
	CHECK(plane_is(picture.u, picture.uv_stride, 0U, 0U, 8U, 128U));
	CHECK(plane_is(picture.v, picture.uv_stride, 0U, 0U, 8U, 128U));
	webpc_vp8_picture_release(&picture);
	CHECK(picture.memory == NULL);
}

/*
 * A 32x32 frame of four skipped macroblocks whose values come from the
 * out-of-frame edges and corners (12.2, 12.3):
 *   (0,0) H_PRED: left of the frame, 129;
 *   (1,0) TM_PRED: left 129 + above 127 - corner 127 (top row) = 129
 *         (a corner of 129 would give 127);
 *   (0,1) TM_PRED: left 129 + above 129 - corner 129 (left column below
 *         the top row) = 129 (a corner of 127 would give 131);
 *   (1,1) B_PRED with B_VE_PRED: each pixel (P + 2A + B + 2) >> 2 of the
 *         row above, all 129, so 129 (the above-right of the right column
 *         is the rightmost macroblock's row -1 column 15, 129 too).
 * The frame is 30x30 so the right and bottom macroblocks are partly
 * outside the image.
 */
static void
test_edges(void)
{
	static struct test_frame frame;
	struct webpc_vp8_picture picture;
	struct test_mb mbs[4];
	int error;

	/* The four macroblocks. */
	memset(mbs, 0, sizeof(mbs));
	mbs[0].skip = 1;
	mbs[0].ymode = WEBPC_VP8_H_PRED;
	mbs[0].uvmode = WEBPC_VP8_H_PRED;
	mbs[1].skip = 1;
	mbs[1].ymode = WEBPC_VP8_TM_PRED;
	mbs[1].uvmode = WEBPC_VP8_TM_PRED;
	mbs[2].skip = 1;
	mbs[2].ymode = WEBPC_VP8_TM_PRED;
	mbs[2].uvmode = WEBPC_VP8_TM_PRED;
	mbs[3].skip = 1;
	mbs[3].ymode = WEBPC_VP8_V_PRED;
	mbs[3].uvmode = WEBPC_VP8_DC_PRED;
	CHECK(frame_build(&frame, 30U, 30U, 0U, 0U, mbs) == 0);
	error = webpc_vp8_decode(frame.bytes, frame.size, 0, &picture);
	CHECK(error == 0);
	if (error != 0)
		return;
	CHECK(picture.width == 30U && picture.height == 30U && picture.mb_width == 2U && picture.mb_height == 2U);
	CHECK(plane_is(picture.y, picture.y_stride, 0U, 0U, 16U, 129U));
	CHECK(plane_is(picture.y, picture.y_stride, 16U, 0U, 16U, 129U));
	CHECK(plane_is(picture.y, picture.y_stride, 0U, 16U, 16U, 129U));
	CHECK(plane_is(picture.y, picture.y_stride, 16U, 16U, 16U, 129U));
	CHECK(plane_is(picture.u, picture.uv_stride, 0U, 0U, 8U, 129U));
	CHECK(plane_is(picture.u, picture.uv_stride, 8U, 0U, 8U, 129U));
	CHECK(plane_is(picture.u, picture.uv_stride, 0U, 8U, 8U, 129U));
	CHECK(plane_is(picture.v, picture.uv_stride, 8U, 8U, 8U, 129U));
	webpc_vp8_picture_release(&picture);

	/*
	 * A B_PRED macroblock at the top-left with B_VE_PRED: the first row of
	 * subblocks has 127 above (corner and above-right too), so 127.  Below,
	 * the first column's corner is the left edge's 129: its first pixel is
	 * (129 + 2 * 127 + 127 + 2) >> 2 = 128 (then (129 + 256 + 127 + 2) >> 2 =
	 * 128 again under it), every other pixel (127 + 254 + 127 + 2) >> 2 = 127
	 * or, next to the 128, (128 + 254 + 127 + 2) >> 2 = 127.  Chroma DC 128.
	 */
	memset(mbs, 0, sizeof(mbs));
	mbs[0].skip = 1;
	mbs[0].ymode = WEBPC_VP8_B_PRED;
	mbs[0].bmode = WEBPC_VP8_B_VE_PRED;
	mbs[0].uvmode = WEBPC_VP8_DC_PRED;
	CHECK(frame_build(&frame, 16U, 16U, 0U, 0U, mbs) == 0);
	error = webpc_vp8_decode(frame.bytes, frame.size, 0, &picture);
	CHECK(error == 0);
	if (error != 0)
		return;
	CHECK(plane_is(picture.y, picture.y_stride, 0U, 0U, 4U, 127U));
	CHECK(plane_is(picture.y, picture.y_stride, 4U, 0U, 4U, 127U));
	CHECK(plane_is(picture.y, picture.y_stride, 12U, 12U, 4U, 127U));
	CHECK(picture.y[4 * picture.y_stride] == 128U);
	CHECK(picture.y[15 * picture.y_stride] == 128U);
	CHECK(picture.y[4 * picture.y_stride + 1] == 127U);
	CHECK(picture.y[3 * picture.y_stride] == 127U);
	CHECK(plane_is(picture.u, picture.uv_stride, 0U, 0U, 8U, 128U));
	webpc_vp8_picture_release(&picture);

	/*
	 * B_HE_PRED at the top-left: the left column is 129 and the corner
	 * 127; the first column of subblocks gets (P + 2 L0 + L1 + 2) >> 2 =
	 * (127 + 258 + 129 + 2) >> 2 = 129 on its first row of each subblock
	 * (and 129 below); the next columns take the column reconstructed on
	 * their left, all 129 with its corner from the row above (127 on the
	 * top row: the first row of subblocks is again 129).
	 */
	mbs[0].bmode = WEBPC_VP8_B_HE_PRED;
	CHECK(frame_build(&frame, 16U, 16U, 0U, 0U, mbs) == 0);
	error = webpc_vp8_decode(frame.bytes, frame.size, 0, &picture);
	CHECK(error == 0);
	if (error != 0)
		return;
	CHECK(plane_is(picture.y, picture.y_stride, 0U, 0U, 16U, 129U));
	webpc_vp8_picture_release(&picture);
}

/*
 * One macroblock with a Y2 DC of 4 (DCT_4) at quantizer index 0: the Y2 DC
 * factor is dc_q[0] * 2 = 8 (14.1), so the WHT input is 32 and every Y DC
 * is (32 + 3) >> 3 = 4 (14.3); each Y block's inverse DCT adds (4 + 4) >> 3
 * = 1 (14.4), so luma is 128 + 1 = 129 with DC_PRED; chroma stays 128.
 * With DCT_CAT1 + extra (the value 5: 40, 5, 1) luma is 129 too, with a
 * value of 1 (8 -> 1 -> 0) it is 128.
 */
static void
test_residue(void)
{
	static struct test_frame frame;
	struct webpc_vp8_picture picture;
	struct test_mb mbs[1];
	int error;

	/* DCT_4. */
	memset(mbs, 0, sizeof(mbs));
	mbs[0].ymode = WEBPC_VP8_DC_PRED;
	mbs[0].uvmode = WEBPC_VP8_DC_PRED;
	mbs[0].y2_value = (int)WEBPC_VP8_DCT_4;
	CHECK(frame_build(&frame, 16U, 16U, 0U, 0U, mbs) == 0);
	error = webpc_vp8_decode(frame.bytes, frame.size, 0, &picture);
	CHECK(error == 0);
	if (error != 0)
		return;
	CHECK(plane_is(picture.y, picture.y_stride, 0U, 0U, 16U, 129U));
	CHECK(plane_is(picture.u, picture.uv_stride, 0U, 0U, 8U, 128U));
	webpc_vp8_picture_release(&picture);

	/* DCT_1: the DC is too small to reach the pixels. */
	mbs[0].y2_value = (int)WEBPC_VP8_DCT_1;
	CHECK(frame_build(&frame, 16U, 16U, 0U, 0U, mbs) == 0);
	error = webpc_vp8_decode(frame.bytes, frame.size, 0, &picture);
	CHECK(error == 0);
	if (error != 0)
		return;
	CHECK(plane_is(picture.y, picture.y_stride, 0U, 0U, 16U, 128U));
	webpc_vp8_picture_release(&picture);

	/* The loop filter on a flat frame changes nothing (every difference across an edge is 0). */
	mbs[0].y2_value = (int)WEBPC_VP8_DCT_4;
	CHECK(frame_build(&frame, 16U, 16U, 0U, 40U, mbs) == 0);
	error = webpc_vp8_decode(frame.bytes, frame.size, 0, &picture);
	CHECK(error == 0);
	if (error != 0)
		return;
	CHECK(plane_is(picture.y, picture.y_stride, 0U, 0U, 16U, 129U));
	webpc_vp8_picture_release(&picture);
}

/*
 * A 16x32 frame (two rows) with 2 partitions (9.5): row 0's tokens are in
 * partition 0 and row 1's in partition 1.  Row 0 has DCT_4 (128 + 1 =
 * 129); row 1 has DCT_1 with DC_PRED from the 129 row above alone, (16 *
 * 129 + 8) >> 4 = 129, plus 0.  Then row 1 has DCT_4: 129 + 1 = 130.  Rows
 * read from the wrong partitions would not decode.
 */
static void
test_partitions(void)
{
	static struct test_frame frame;
	struct webpc_vp8_picture picture;
	struct test_mb mbs[2];
	int error;

	/* Row 0 DCT_4 with DC_PRED (129), row 1 DCT_1 with V_PRED (129 from the row above, + 0). */
	memset(mbs, 0, sizeof(mbs));
	mbs[0].ymode = WEBPC_VP8_DC_PRED;
	mbs[0].uvmode = WEBPC_VP8_DC_PRED;
	mbs[0].y2_value = (int)WEBPC_VP8_DCT_4;
	mbs[1].ymode = WEBPC_VP8_DC_PRED;
	mbs[1].uvmode = WEBPC_VP8_DC_PRED;
	mbs[1].y2_value = (int)WEBPC_VP8_DCT_1;
	mbs[1].y2_context = 1U;
	CHECK(frame_build(&frame, 16U, 32U, 1U, 0U, mbs) == 0);
	error = webpc_vp8_decode(frame.bytes, frame.size, 0, &picture);
	CHECK(error == 0);
	if (error != 0)
		return;

	/* The top row: DC 128 + 1.  The bottom: DC from the 129 row above only, (16 * 129 + 8) >> 4 = 129, + 0. */
	CHECK(plane_is(picture.y, picture.y_stride, 0U, 0U, 16U, 129U));
	CHECK(plane_is(picture.y, picture.y_stride, 0U, 16U, 16U, 129U));
	webpc_vp8_picture_release(&picture);

	/* Row 1 DCT_4 too: 129 above + 1 = 130 at the bottom. */
	mbs[1].y2_value = (int)WEBPC_VP8_DCT_4;
	CHECK(frame_build(&frame, 16U, 32U, 1U, 0U, mbs) == 0);
	error = webpc_vp8_decode(frame.bytes, frame.size, 0, &picture);
	CHECK(error == 0);
	if (error != 0)
		return;
	CHECK(plane_is(picture.y, picture.y_stride, 0U, 16U, 16U, 130U));
	webpc_vp8_picture_release(&picture);
}

/* Frames cut short, and random bytes after a valid tag, fail without reading outside the data. */
static void
test_cut(void)
{
	static struct test_frame frame;
	struct webpc_vp8_picture picture;
	struct test_mb mbs[4];
	unsigned long state;
	uint8_t *copy;
	size_t cut;
	size_t index;
	unsigned round;
	int error;
	int failures;

	/* A frame with tokens in every macroblock. */
	memset(mbs, 0, sizeof(mbs));
	for (index = 0; index < 4U; index++) {
		mbs[index].ymode = WEBPC_VP8_DC_PRED;
		mbs[index].uvmode = WEBPC_VP8_DC_PRED;
		mbs[index].y2_value = (int)WEBPC_VP8_DCT_4;
		mbs[index].y2_context = (index % 2U) + (index / 2U);
	}
	CHECK(frame_build(&frame, 32U, 32U, 0U, 20U, mbs) == 0);
	error = webpc_vp8_decode(frame.bytes, frame.size, 0, &picture);
	CHECK(error == 0);
	webpc_vp8_picture_release(&picture);

	/* Every cut, in a copy of exactly its size: never 0 (ENOBUFS or EINVAL), never outside the copy. */
	failures = 0;
	for (cut = 0; cut < frame.size; cut++) {
		copy = malloc(cut > 0U ? cut : 1U);
		if (copy == NULL)
			return;
		memcpy(copy, frame.bytes, cut);
		error = webpc_vp8_decode(copy, cut, 0, &picture);
		if (error == 0) {
			failures++;
			webpc_vp8_picture_release(&picture);
		}
		CHECK(picture.memory == NULL || error == 0);
		free(copy);
	}
	CHECK(failures == 0);

	/* Cut inside the token partition (the tag and the first partition whole): ENOBUFS. */
	error = webpc_vp8_decode(frame.bytes, frame.size - 3U, 0, &picture);
	CHECK(error == ENOBUFS);

	/* Random bytes after a valid tag and header start: some decode, none crash. */
	state = 99UL;
	for (round = 0; round < 300U; round++) {
		copy = malloc(frame.size);
		if (copy == NULL)
			return;
		memcpy(copy, frame.bytes, frame.size);
		for (index = 10U + (size_t)(round % 7U); index < frame.size; index++)
			copy[index] = (uint8_t)(check_random(&state) >> 8);
		error = webpc_vp8_decode(copy, frame.size, round % 2U, &picture);
		if (error == 0)
			webpc_vp8_picture_release(&picture);
		free(copy);
	}
	CHECK(1);
}
