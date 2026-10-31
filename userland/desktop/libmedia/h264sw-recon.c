/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reconstruction of a macroblock in the software H.264 decoder.
 */

#include "h264sw-recon.h"
#include "h264sw-context.h"
#include "h264sw-interp.h"
#include "h264sw-intra.h"
#include "h264sw-mvpred.h"
#include "h264sw-tables.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* The decoded_4x4 value of a macroblock whose sixteen 4x4 blocks all have their motion. */
#define RECON_DECODED_ALL 0xffffU

/* The nonzero value of a macroblock whose sixteen luma 4x4 blocks all count as having coefficients. */
#define RECON_NONZERO_ALL 0xffffU

/* The largest QPY of an 8-bit stream (7.4.3). */
#define RECON_QP_MAX 51

/* The smallest and the largest value of an int16_t, and the number of values between them: a sum is wrapped into this range. */
#define RECON_VECTOR_MIN (-32768)
#define RECON_VECTOR_MAX 32767
#define RECON_VECTOR_SPAN 65536

/* The index in struct h264sw_levelscale's scale4x4 of the Intra Y, Intra Cb, Inter Y and Inter Cb lists (Table 7-2); the Cr list follows the Cb one. */
#define RECON_SCALE4_INTRA_Y 0U
#define RECON_SCALE4_INTRA_CB 1U
#define RECON_SCALE4_INTER_Y 3U
#define RECON_SCALE4_INTER_CB 4U

/* The index in struct h264sw_levelscale's scale8x8 of the Intra Y and Inter Y lists (Table 7-2). */
#define RECON_SCALE8_INTRA_Y 0U
#define RECON_SCALE8_INTER_Y 1U

/* Forward declarations of the static functions. */
static int recon_check(const struct h264sw_slice_ctx *ctx);
static int recon_floor_div(int value, int divisor);
static int recon_wrap_vector(int value);
static int recon_qp(const struct h264sw_mb *mb);
static int recon_chroma_qp(const struct h264sw_picture_ctx *picture, int qp, unsigned component);
static int recon_has_level(const int32_t *levels, unsigned count);
static int recon_uses_list(unsigned pred, unsigned list);
static uint8_t *recon_sample_at(const struct h264sw_frame *frame, unsigned plane, unsigned x, unsigned y);
static void recon_put_block(uint8_t *dst, size_t dst_stride, const uint8_t *src, unsigned size);
static void recon_residual4x4(const int32_t *levels, int dc_present, int32_t dc, const int32_t *scale, int qp, uint8_t *dst, size_t stride);
static void recon_residual8x8(const int32_t *levels, const int32_t *scale, int qp, uint8_t *dst, size_t stride);
static int recon_pcm(struct h264sw_slice_ctx *ctx);
static int recon_intra(struct h264sw_slice_ctx *ctx);
static void recon_intra4x4(struct h264sw_slice_ctx *ctx);
static void recon_intra8x8(struct h264sw_slice_ctx *ctx);
static void recon_intra16x16(struct h264sw_slice_ctx *ctx);
static void recon_intra_chroma(struct h264sw_slice_ctx *ctx);
static void recon_chroma_residual(struct h264sw_slice_ctx *ctx, int intra);
static void recon_luma_residual(struct h264sw_slice_ctx *ctx);
static int recon_inter(struct h264sw_slice_ctx *ctx);
static int recon_motion(struct h264sw_slice_ctx *ctx);
static int recon_motion_pskip(struct h264sw_slice_ctx *ctx);
static int recon_motion_direct(struct h264sw_slice_ctx *ctx);
static int recon_motion_parts(struct h264sw_slice_ctx *ctx);
static int recon_motion_sub(struct h264sw_slice_ctx *ctx);
static int recon_partition(struct h264sw_slice_ctx *ctx, unsigned pred, unsigned part, unsigned sub, unsigned x, unsigned y, unsigned width, unsigned height);
static int recon_vector(struct h264sw_slice_ctx *ctx, unsigned list, int ref_idx, const int16_t *mvd, unsigned x, unsigned y, unsigned width, unsigned height);
static void recon_mark_decoded(struct h264sw_slice_ctx *ctx, unsigned x, unsigned y, unsigned width, unsigned height);
static int recon_uniform(const struct h264sw_mb *mb, unsigned x, unsigned y, unsigned size);
static int recon_predict(struct h264sw_slice_ctx *ctx);
static int recon_predict_block(struct h264sw_slice_ctx *ctx, unsigned x, unsigned y, unsigned size);

/*
 * Reconstructs the current macroblock (ctx->mb_addr, ctx->n, ctx->syntax).
 *
 * The specification is the comment of the declaration in
 * h264sw-recon.h.
 */
int
h264sw_recon_mb(
	struct h264sw_slice_ctx *ctx)
{
	unsigned kind;
	unsigned concealed_before;
	int error;

	/* Refuses a context that has no picture, frame or macroblock to work on. */
	error = recon_check(ctx);
	if (error != 0)
		return error;

	/* Remembers how many macroblocks were concealed, so that this one adds at most one however many of its steps fall back. */
	concealed_before = ctx->concealed;

	/* Reconstructs the macroblock the way its kind says. */
	kind = ctx->syntax.type.kind;
	switch (kind) {
	case H264SW_MB_I_PCM:
		error = recon_pcm(ctx);
		break;
	case H264SW_MB_I_4X4:
	case H264SW_MB_I_8X8:
	case H264SW_MB_I_16X16:
		error = recon_intra(ctx);
		break;
	case H264SW_MB_P_SKIP:
	case H264SW_MB_P:
	case H264SW_MB_P_8X8:
	case H264SW_MB_P_8X8REF0:
	case H264SW_MB_B_SKIP:
	case H264SW_MB_B_DIRECT:
	case H264SW_MB_B:
	case H264SW_MB_B_8X8:
		error = recon_inter(ctx);
		break;
	default:
		/* I_NxN is resolved by the parser, so any other kind is a broken context. */
		error = EINVAL;
		break;
	}

	/* Reports a macroblock that cannot be built at all. */
	if (error != 0)
		return error;

	/* Counts the macroblock once if any of its steps used a stand-in. */
	if (ctx->concealed > concealed_before)
		ctx->concealed = concealed_before + 1U;

	/* Succeeded: the macroblock's samples, motion and modes are in place. */
	return 0;
}

/* Checks that the context holds a current macroblock inside a frame that has room for it. */
static int
recon_check(
	const struct h264sw_slice_ctx *ctx)
{
	const struct h264sw_picture_ctx *picture;
	const struct h264sw_frame *frame;
	unsigned plane;
	unsigned right;
	unsigned bottom;

	/* Refuses a missing context, picture or current macroblock entry. */
	if (ctx == NULL)
		return EINVAL;
	if (ctx->picture == NULL)
		return EINVAL;
	if (ctx->n.current == NULL)
		return EINVAL;

	/* Refuses a missing frame or a macroblock outside the picture. */
	picture = ctx->picture;
	frame = picture->frame;
	if (frame == NULL)
		return EINVAL;
	if (ctx->n.x >= picture->width_mbs)
		return EINVAL;
	if (ctx->n.y >= picture->height_mbs)
		return EINVAL;

	/* Refuses a frame that is smaller than the macroblock's last sample. */
	right = 16U * (ctx->n.x + 1U);
	bottom = 16U * (ctx->n.y + 1U);
	if (right > frame->width)
		return EINVAL;
	if (bottom > frame->height)
		return EINVAL;

	/* Refuses a frame whose planes are missing. */
	for (plane = 0; plane < 3U; plane++) {
		if (frame->plane[plane] == NULL)
			return EINVAL;
	}

	/* Refuses a frame whose rows overlap. */
	if (frame->stride[0] < frame->width)
		return EINVAL;
	if (frame->stride[1] < frame->width / 2U)
		return EINVAL;
	if (frame->stride[2] < frame->width / 2U)
		return EINVAL;

	/* Succeeded: the macroblock can be written. */
	return 0;
}

/* Divides rounding toward minus infinity, as a right shift of a negative value would in the clauses; the divisor is positive. */
static int
recon_floor_div(
	int value,
	int divisor)
{
	/* A non-negative value divides exactly as the language does. */
	if (value >= 0)
		return value / divisor;

	/* A negative value rounds down: negate, round up, negate again. */
	return -((-value + divisor - 1) / divisor);
}

/* Wraps a sum of a prediction and a difference, which lies within one span of the range, into -32768..32767 by arithmetic. */
static int
recon_wrap_vector(
	int value)
{
	/* A sum below the range comes back up by one span. */
	if (value < RECON_VECTOR_MIN)
		value += RECON_VECTOR_SPAN;

	/* A sum above the range comes back down by one span. */
	if (value > RECON_VECTOR_MAX)
		value -= RECON_VECTOR_SPAN;

	/* The wrapped sum fits an int16_t. */
	return value;
}

/* Gives QPY of the macroblock, held within 0..51 so that the scaling and the tables are indexed safely. */
static int
recon_qp(
	const struct h264sw_mb *mb)
{
	int qp;

	/* The entry keeps QPY in a byte; a broken value is brought to the largest QPY. */
	qp = (int)mb->qp;
	if (qp > RECON_QP_MAX)
		qp = RECON_QP_MAX;

	/* The quantization parameter of the luma blocks. */
	return qp;
}

/* Gives QP'C of a chroma component from QPY, by qPI of 8-309 to 8-312 and Table 8-15; component is 0 for Cb and 1 for Cr. */
static int
recon_chroma_qp(
	const struct h264sw_picture_ctx *picture,
	int qp,
	unsigned component)
{
	int qpi;

	/* qPI is QPY plus the component's offset, clipped to 0..51 (QpBdOffsetC is 0 for 8 bits). */
	qpi = qp + picture->chroma_qp_offset[component];
	if (qpi < 0)
		qpi = 0;
	if (qpi > RECON_QP_MAX)
		qpi = RECON_QP_MAX;

	/* The table gives QPC for qPI. */
	return (int)h264sw_chroma_qp[qpi];
}

/* Tells whether any of count levels is not zero. */
static int
recon_has_level(
	const int32_t *levels,
	unsigned count)
{
	unsigned index;

	/* Looks for a level that is not zero. */
	for (index = 0; index < count; index++) {
		if (levels[index] != 0)
			return 1;
	}

	/* Every level is zero. */
	return 0;
}

/* Tells whether a partition's prediction mode uses a reference list. */
static int
recon_uses_list(
	unsigned pred,
	unsigned list)
{
	/* Bi-prediction uses both lists. */
	if (pred == H264SW_PRED_BI)
		return 1;

	/* List 0 prediction uses list 0 only. */
	if (pred == H264SW_PRED_L0 && list == 0U)
		return 1;

	/* List 1 prediction uses list 1 only. */
	if (pred == H264SW_PRED_L1 && list == 1U)
		return 1;

	/* Direct prediction and no prediction have nothing to derive here. */
	return 0;
}

/* Gives the address of the sample (x, y) of a plane of a frame. */
static uint8_t *
recon_sample_at(
	const struct h264sw_frame *frame,
	unsigned plane,
	unsigned x,
	unsigned y)
{
	/* The row starts stride bytes after the one above. */
	return frame->plane[plane] + (size_t)y * frame->stride[plane] + x;
}

/* Copies a size by size block of samples into a plane. */
static void
recon_put_block(
	uint8_t *dst,
	size_t dst_stride,
	const uint8_t *src,
	unsigned size)
{
	unsigned row;

	/* Copies the block row by row; the source rows are size bytes apart. */
	for (row = 0; row < size; row++)
		memcpy(dst + (size_t)row * dst_stride, src + (size_t)row * size, size);
}

/*
 * Adds the residual of one 4x4 block to the samples already in the frame.
 *
 * levels are the block's 16 levels in scan order.  When dc_present, dc
 * is the block's DC already scaled by 8.5.10 or 8.5.11 and replaces the
 * first coefficient; otherwise the first level is scaled like the
 * others.  A block with nothing in it adds nothing.
 */
static void
recon_residual4x4(
	const int32_t *levels,
	int dc_present,
	int32_t dc,
	const int32_t *scale,
	int qp,
	uint8_t *dst,
	size_t stride)
{
	int32_t block[16];
	int any;

	/* Finds out whether the block holds any level or a DC. */
	any = recon_has_level(levels, 16U);
	if (any == 0 && dc_present != 0 && dc != 0)
		any = 1;

	/* A block without coefficients has a residual of zero (8.5.12). */
	if (any == 0)
		return;

	/* Places the levels in raster order (8.5.6). */
	h264sw_unscan4x4(levels, block);

	/* The DC of an Intra_16x16 or chroma block comes from its own transform (8.5.10, 8.5.11). */
	if (dc_present != 0)
		block[0] = dc;

	/* Scales the levels, transforms them and adds the residual to the prediction (8.5.12, 8.5.14). */
	h264sw_scale4x4(block, scale, qp, dc_present);
	h264sw_idct4x4(block);
	h264sw_add_residual(dst, stride, block, 4U, dst, stride);
}

/* Adds the residual of one 8x8 block of 64 levels in scan order to the samples already in the frame (8.5.7, 8.5.13, 8.5.14). */
static void
recon_residual8x8(
	const int32_t *levels,
	const int32_t *scale,
	int qp,
	uint8_t *dst,
	size_t stride)
{
	int32_t block[64];

	/* Places the levels in raster order (8.5.7). */
	h264sw_unscan8x8(levels, block);

	/* Scales the levels, transforms them and adds the residual to the prediction (8.5.13, 8.5.14). */
	h264sw_scale8x8(block, scale, qp);
	h264sw_idct8x8(block);
	h264sw_add_residual(dst, stride, block, 8U, dst, stride);
}

/* Reconstructs an I_PCM macroblock by copying its samples (8.3.5). */
static int
recon_pcm(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb *mb;
	const struct h264sw_frame *frame;
	const uint8_t *pcm;
	uint8_t *dst;
	unsigned row;
	unsigned plane;
	unsigned size;
	unsigned offset;

	/* Takes the entry being written, the frame and the samples the parser read. */
	mb = ctx->n.current;
	frame = ctx->picture->frame;
	pcm = ctx->syntax.pcm;

	/* Copies the 256 luma samples, then the 64 samples of Cb and of Cr, each in raster order. */
	offset = 0;
	for (plane = 0; plane < 3U; plane++) {
		size = 16U;
		if (plane != 0U)
			size = 8U;
		dst = recon_sample_at(frame, plane, size * ctx->n.x, size * ctx->n.y);
		for (row = 0; row < size; row++)
			memcpy(dst + (size_t)row * frame->stride[plane], pcm + offset + (size_t)row * size, size);
		offset += size * size;
	}

	/* Records the motion of an intra macroblock, and that every block counts as having coefficients for the deblocking filter. */
	mb->motion.intra = 1;
	mb->nonzero = (uint16_t)RECON_NONZERO_ALL;

	/* Succeeded: the samples are in the frame. */
	return 0;
}

/* Reconstructs an Intra_4x4, Intra_8x8 or Intra_16x16 macroblock: luma first, then the two chroma blocks (8.3, 8.5). */
static int
recon_intra(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb *mb;

	/* Takes the entry being written. */
	mb = ctx->n.current;

	/* An intra macroblock uses no reference list. */
	mb->motion.intra = 1;

	/* Predicts and reconstructs the luma by the macroblock's intra kind. */
	if (ctx->syntax.type.kind == H264SW_MB_I_4X4) {
		recon_intra4x4(ctx);
	} else if (ctx->syntax.type.kind == H264SW_MB_I_8X8) {
		recon_intra8x8(ctx);
	} else {
		recon_intra16x16(ctx);
	}

	/* Predicts the chroma with intra_chroma_pred_mode and adds its residual. */
	recon_intra_chroma(ctx);

	/* Succeeded: the macroblock is in the frame. */
	return 0;
}

/* Predicts and reconstructs the sixteen 4x4 blocks of an Intra_4x4 macroblock in luma4x4BlkIdx order (8.3.1, 8.5.1). */
static void
recon_intra4x4(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb *mb;
	const struct h264sw_mb_syntax *syntax;
	const struct h264sw_frame *frame;
	struct h264sw_intra_edge edge;
	const int32_t *scale;
	uint8_t pred[16];
	uint8_t *dst;
	unsigned blk;
	unsigned mode;
	int qp;
	int has_level;
	int error;

	/* Takes the entry being written, the syntax and the frame. */
	mb = ctx->n.current;
	syntax = &ctx->syntax;
	frame = ctx->picture->frame;

	/* Takes QPY and the scaling of the Intra Y list. */
	qp = recon_qp(mb);
	scale = ctx->picture->levelscale.scale4x4[RECON_SCALE4_INTRA_Y][qp % 6];

	/* Reconstructs each block before the next, because a later block reads the samples of an earlier one as its edge. */
	for (blk = 0; blk < 16U; blk++) {
		/* Derives the block's mode and keeps it for the blocks and macroblocks after it (8.3.1.1). */
		mode = h264sw_intra4x4_mode(ctx, blk, syntax->prev_intra_flag[blk], syntax->rem_intra_mode[blk]);
		mb->intra4x4_mode[blk] = (uint8_t)mode;

		/* Gathers the block's edge from the frame and predicts the block. */
		h264sw_intra_edge_luma(ctx, 4U, blk, &edge);
		error = h264sw_intra_pred4x4(mode, &edge, pred);

		/* A mode whose samples are not available left a flat prediction; the stream is broken. */
		if (error != 0)
			ctx->concealed++;

		/* Puts the prediction into the frame. */
		dst = recon_sample_at(frame, 0, 16U * ctx->n.x + h264sw_blk4x4_x[blk], 16U * ctx->n.y + h264sw_blk4x4_y[blk]);
		recon_put_block(dst, frame->stride[0], pred, 4U);

		/* Adds the residual, and notes the block as having coefficients for the deblocking filter. */
		has_level = recon_has_level(syntax->luma[blk], 16U);
		if (has_level != 0) {
			recon_residual4x4(syntax->luma[blk], 0, 0, scale, qp, dst, frame->stride[0]);
			mb->nonzero = (uint16_t)(mb->nonzero | (1U << blk));
		}
	}
}

/* Predicts and reconstructs the four 8x8 blocks of an Intra_8x8 macroblock in luma8x8BlkIdx order (8.3.2, 8.5.3). */
static void
recon_intra8x8(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb *mb;
	const struct h264sw_mb_syntax *syntax;
	const struct h264sw_frame *frame;
	struct h264sw_intra_edge edge;
	const int32_t *scale;
	uint8_t pred[64];
	uint8_t *dst;
	unsigned blk;
	unsigned part;
	unsigned mode;
	int qp;
	int has_level;
	int error;

	/* Takes the entry being written, the syntax and the frame. */
	mb = ctx->n.current;
	syntax = &ctx->syntax;
	frame = ctx->picture->frame;

	/* Takes QPY and the scaling of the Intra Y 8x8 list. */
	qp = recon_qp(mb);
	scale = ctx->picture->levelscale.scale8x8[RECON_SCALE8_INTRA_Y][qp % 6];

	/* Reconstructs each block before the next, because a later block reads the samples of an earlier one as its edge. */
	for (blk = 0; blk < 4U; blk++) {
		/* Derives the block's mode and keeps it in each of the block's four 4x4 entries (8.3.2.1). */
		mode = h264sw_intra8x8_mode(ctx, blk, syntax->prev_intra_flag[blk], syntax->rem_intra_mode[blk]);
		for (part = 0; part < 4U; part++)
			mb->intra4x4_mode[4U * blk + part] = (uint8_t)mode;

		/* Gathers the block's edge from the frame and predicts the block. */
		h264sw_intra_edge_luma(ctx, 8U, blk, &edge);
		error = h264sw_intra_pred8x8(mode, &edge, pred);

		/* A mode whose samples are not available left a flat prediction; the stream is broken. */
		if (error != 0)
			ctx->concealed++;

		/* Puts the prediction into the frame. */
		dst = recon_sample_at(frame, 0, 16U * ctx->n.x + 8U * (blk % 2U), 16U * ctx->n.y + 8U * (blk / 2U));
		recon_put_block(dst, frame->stride[0], pred, 8U);

		/* Adds the residual, and notes the four 4x4 blocks of the 8x8 block as having coefficients. */
		has_level = recon_has_level(syntax->luma8x8[blk], 64U);
		if (has_level != 0) {
			recon_residual8x8(syntax->luma8x8[blk], scale, qp, dst, frame->stride[0]);
			mb->nonzero = (uint16_t)(mb->nonzero | (0xfU << (4U * blk)));
		}
	}
}

/* Predicts the whole luma block of an Intra_16x16 macroblock and adds its DC and AC residual (8.3.3, 8.5.2). */
static void
recon_intra16x16(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb *mb;
	const struct h264sw_mb_syntax *syntax;
	const struct h264sw_frame *frame;
	struct h264sw_intra_edge edge;
	const int32_t *scale;
	int32_t dc[16];
	uint8_t pred[256];
	uint8_t *origin;
	uint8_t *dst;
	unsigned blk;
	unsigned block_x;
	unsigned block_y;
	int qp;
	int has_dc;
	int has_ac;
	int error;

	/* Takes the entry being written, the syntax and the frame. */
	mb = ctx->n.current;
	syntax = &ctx->syntax;
	frame = ctx->picture->frame;

	/* Gathers the macroblock's edge from the frame and predicts the whole block. */
	h264sw_intra_edge_luma(ctx, 16U, 0U, &edge);
	error = h264sw_intra_pred16x16(syntax->type.intra16x16_mode, &edge, pred);

	/* A mode whose samples are not available left a flat prediction; the stream is broken. */
	if (error != 0)
		ctx->concealed++;

	/* Puts the prediction into the frame. */
	origin = recon_sample_at(frame, 0, 16U * ctx->n.x, 16U * ctx->n.y);
	recon_put_block(origin, frame->stride[0], pred, 16U);

	/* Takes QPY and the scaling of the Intra Y list. */
	qp = recon_qp(mb);
	scale = ctx->picture->levelscale.scale4x4[RECON_SCALE4_INTRA_Y][qp % 6];

	/* Places the sixteen DC levels in a 4x4 matrix (8.5.6) and transforms and scales them (8.5.10). */
	h264sw_unscan4x4(syntax->luma_dc, dc);
	h264sw_luma_dc(dc, scale[0], qp);

	/* A DC level that is not zero counts for every block's boundary strength. */
	has_dc = recon_has_level(syntax->luma_dc, 16U);
	if (has_dc != 0)
		mb->nonzero = (uint16_t)RECON_NONZERO_ALL;

	/* Adds each block's residual: the DC from the matrix at the block's row and column, and the AC levels the block carries (8.5.2). */
	for (blk = 0; blk < 16U; blk++) {
		block_x = h264sw_blk4x4_x[blk];
		block_y = h264sw_blk4x4_y[blk];
		dst = origin + (size_t)block_y * frame->stride[0] + block_x;
		recon_residual4x4(syntax->luma[blk], 1, dc[4U * (block_y / 4U) + block_x / 4U], scale, qp, dst, frame->stride[0]);

		/* Notes the block as having coefficients when its own AC levels are not zero. */
		has_ac = recon_has_level(syntax->luma[blk], 16U);
		if (has_ac != 0)
			mb->nonzero = (uint16_t)(mb->nonzero | (1U << blk));
	}
}

/* Predicts the Cb and Cr blocks of an intra macroblock with intra_chroma_pred_mode (8.3.4) and adds their residual (8.5.4). */
static void
recon_intra_chroma(
	struct h264sw_slice_ctx *ctx)
{
	const struct h264sw_frame *frame;
	struct h264sw_intra_edge edge;
	uint8_t pred[64];
	uint8_t *dst;
	unsigned plane;
	int error;

	/* Takes the frame the prediction is written into. */
	frame = ctx->picture->frame;

	/* Predicts each chroma component from its own edge. */
	for (plane = 1; plane < 3U; plane++) {
		/* Gathers the component's edge from the frame and predicts the 8x8 block. */
		h264sw_intra_edge_chroma(ctx, plane, &edge);
		error = h264sw_intra_pred_chroma(ctx->n.current->chroma_pred_mode, &edge, pred);

		/* A mode whose samples are not available left a flat prediction; the stream is broken. */
		if (error != 0)
			ctx->concealed++;

		/* Puts the prediction into the frame. */
		dst = recon_sample_at(frame, plane, 8U * ctx->n.x, 8U * ctx->n.y);
		recon_put_block(dst, frame->stride[plane], pred, 8U);
	}

	/* Adds the DC and AC residual of both components. */
	recon_chroma_residual(ctx, 1);
}

/* Adds the chroma residual of both components to the prediction already in the frame (8.5.4, 8.5.8, 8.5.11). */
static void
recon_chroma_residual(
	struct h264sw_slice_ctx *ctx,
	int intra)
{
	const struct h264sw_mb_syntax *syntax;
	const struct h264sw_frame *frame;
	const int32_t *scale;
	int32_t dc[4];
	uint8_t *origin;
	uint8_t *dst;
	unsigned component;
	unsigned blk;
	unsigned list;
	unsigned index;
	int luma_qp;
	int qp;
	int any;

	/* Takes the syntax and the frame. */
	syntax = &ctx->syntax;
	frame = ctx->picture->frame;

	/* Each component has its own quantization parameter, scaling list and DC block. */
	for (component = 0; component < 2U; component++) {
		/* Skips a component with no level at all: its residual is zero. */
		any = recon_has_level(syntax->chroma_dc[component], 4U);
		for (blk = 0; blk < 4U; blk++) {
			if (any != 0)
				break;
			any = recon_has_level(syntax->chroma_ac[component][blk], 16U);
		}

		/* A component whose levels are all zero adds nothing to the prediction. */
		if (any == 0)
			continue;

		/* Finds QP'C (8.5.8) and the scaling of the component's list: Intra or Inter, Cb then Cr. */
		luma_qp = recon_qp(ctx->n.current);
		qp = recon_chroma_qp(ctx->picture, luma_qp, component);
		list = RECON_SCALE4_INTER_CB + component;
		if (intra != 0)
			list = RECON_SCALE4_INTRA_CB + component;
		scale = ctx->picture->levelscale.scale4x4[list][qp % 6];

		/* Transforms and scales the four DC levels, a 2x2 matrix in raster order (8.5.11). */
		for (index = 0; index < 4U; index++)
			dc[index] = syntax->chroma_dc[component][index];
		h264sw_chroma_dc(dc, scale[0], qp);

		/* Adds each 4x4 block's residual: the DC of the same index and the AC levels the block carries (8.5.4). */
		origin = recon_sample_at(frame, 1U + component, 8U * ctx->n.x, 8U * ctx->n.y);
		for (blk = 0; blk < 4U; blk++) {
			dst = origin + (size_t)(4U * (blk / 2U)) * frame->stride[1U + component] + 4U * (blk % 2U);
			recon_residual4x4(syntax->chroma_ac[component][blk], 1, dc[blk], scale, qp, dst, frame->stride[1U + component]);
		}
	}
}

/* Adds the luma residual of an inter macroblock to its prediction, with 4x4 or 8x8 transform blocks (8.5.1, 8.5.3). */
static void
recon_luma_residual(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb *mb;
	const struct h264sw_mb_syntax *syntax;
	const struct h264sw_frame *frame;
	const int32_t *scale;
	uint8_t *dst;
	unsigned blk;
	int qp;
	int has_level;

	/* Takes the entry being written, the syntax, the frame and QPY. */
	mb = ctx->n.current;
	syntax = &ctx->syntax;
	frame = ctx->picture->frame;
	qp = recon_qp(mb);

	/* An 8x8 transform has one block of 64 levels per quadrant. */
	if (mb->transform_8x8 != 0) {
		scale = ctx->picture->levelscale.scale8x8[RECON_SCALE8_INTER_Y][qp % 6];

		/* Adds the residual of each quadrant that has levels, and notes its four 4x4 blocks. */
		for (blk = 0; blk < 4U; blk++) {
			has_level = recon_has_level(syntax->luma8x8[blk], 64U);
			if (has_level == 0)
				continue;
			dst = recon_sample_at(frame, 0, 16U * ctx->n.x + 8U * (blk % 2U), 16U * ctx->n.y + 8U * (blk / 2U));
			recon_residual8x8(syntax->luma8x8[blk], scale, qp, dst, frame->stride[0]);
			mb->nonzero = (uint16_t)(mb->nonzero | (0xfU << (4U * blk)));
		}

		/* Succeeded: the 8x8 blocks are added. */
		return;
	}

	/* A 4x4 transform has one block of 16 levels per 4x4 block. */
	scale = ctx->picture->levelscale.scale4x4[RECON_SCALE4_INTER_Y][qp % 6];

	/* Adds the residual of each block that has levels, and notes the block. */
	for (blk = 0; blk < 16U; blk++) {
		has_level = recon_has_level(syntax->luma[blk], 16U);
		if (has_level == 0)
			continue;
		dst = recon_sample_at(frame, 0, 16U * ctx->n.x + h264sw_blk4x4_x[blk], 16U * ctx->n.y + h264sw_blk4x4_y[blk]);
		recon_residual4x4(syntax->luma[blk], 0, 0, scale, qp, dst, frame->stride[0]);
		mb->nonzero = (uint16_t)(mb->nonzero | (1U << blk));
	}
}

/* Reconstructs an inter macroblock: its motion, then the prediction, then the residual (8.4, 8.5). */
static int
recon_inter(
	struct h264sw_slice_ctx *ctx)
{
	int error;

	/* Derives the motion vectors and reference indices of every partition. */
	error = recon_motion(ctx);
	if (error != 0)
		return error;

	/* Predicts the samples from the reference frames with the weights of the slice. */
	error = recon_predict(ctx);
	if (error != 0)
		return error;

	/* Adds the luma residual. */
	recon_luma_residual(ctx);

	/* Adds the chroma residual. */
	recon_chroma_residual(ctx, 0);

	/* Succeeded: the macroblock is in the frame. */
	return 0;
}

/* Derives the motion of the macroblock by its kind and marks all its 4x4 blocks as derived (8.4.1). */
static int
recon_motion(
	struct h264sw_slice_ctx *ctx)
{
	unsigned kind;
	int error;

	/* Takes the kind the derivation is picked by. */
	kind = ctx->syntax.type.kind;

	/* Picks the derivation the macroblock's kind needs. */
	if (kind == H264SW_MB_P_SKIP) {
		error = recon_motion_pskip(ctx);
	} else if (kind == H264SW_MB_B_SKIP || kind == H264SW_MB_B_DIRECT) {
		error = recon_motion_direct(ctx);
	} else if (kind == H264SW_MB_P || kind == H264SW_MB_B) {
		error = recon_motion_parts(ctx);
	} else {
		error = recon_motion_sub(ctx);
	}

	/* Reports a macroblock whose motion cannot be derived. */
	if (error != 0)
		return error;

	/* Every 4x4 block now has its motion. */
	ctx->decoded_4x4 = RECON_DECODED_ALL;

	/* Succeeded: the motion is in the macroblock's entry. */
	return 0;
}

/* Derives the motion of a P_Skip macroblock: reference index 0 of list 0 and the vector of 8.4.1.1 for all sixteen blocks. */
static int
recon_motion_pskip(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_motion *motion;
	const struct h264sw_frame *frame;
	int16_t mv[2];
	unsigned blk;
	unsigned quadrant;

	/* Takes the motion record of the macroblock being written. */
	motion = &ctx->n.current->motion;

	/* Refuses a slice whose list 0 has no first entry to predict from. */
	if (ctx->lists.count[0] == 0U)
		return EINVAL;
	frame = ctx->lists.list[0][0].frame;
	if (frame == NULL)
		return EINVAL;

	/* Derives the vector from the neighbours A and B (8.4.1.1). */
	h264sw_mvpred_pskip(ctx, mv);

	/* Gives all sixteen blocks the vector. */
	for (blk = 0; blk < 16U; blk++) {
		motion->mv[0][blk][0] = mv[0];
		motion->mv[0][blk][1] = mv[1];
	}

	/* Gives every quadrant reference index 0 and the id of the first frame of list 0; list 1 stays unused. */
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		motion->ref_idx[0][quadrant] = 0;
		motion->ref_id[0][quadrant] = frame->id;
	}

	/* Succeeded: the skipped macroblock's motion is derived. */
	return 0;
}

/* Derives the motion of a B_Skip or B_Direct_16x16 macroblock by the direct prediction of each of its four quadrants (8.4.1.2). */
static int
recon_motion_direct(
	struct h264sw_slice_ctx *ctx)
{
	unsigned quadrant;
	int error;

	/* Derives the quadrants in increasing order, as the direct prediction requires. */
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		error = h264sw_direct_quadrant(ctx, quadrant);
		if (error != 0)
			return error;
	}

	/* Succeeded: all four quadrants are derived. */
	return 0;
}

/* Derives the motion of a macroblock of one or two macroblock partitions (16x16, 16x8 or 8x16) in mbPartIdx order (8.4.1). */
static int
recon_motion_parts(
	struct h264sw_slice_ctx *ctx)
{
	const struct h264sw_mb_syntax *syntax;
	unsigned part;
	unsigned x;
	unsigned y;
	unsigned width;
	unsigned height;
	int error;

	/* Takes the syntax and the size of the partitions. */
	syntax = &ctx->syntax;
	width = syntax->type.part_width;
	height = syntax->type.part_height;

	/* Refuses a type that does not describe one or two partitions inside the macroblock. */
	if (syntax->type.parts == 0U || syntax->type.parts > 2U)
		return EINVAL;
	if (width == 0U || width > 16U)
		return EINVAL;
	if (height == 0U || height > 16U)
		return EINVAL;

	/* Derives each partition; their indices place them in raster order. */
	for (part = 0; part < syntax->type.parts; part++) {
		x = (part * width) % 16U;
		y = ((part * width) / 16U) * height;
		error = recon_partition(ctx, syntax->type.pred[part], part, 0U, x, y, width, height);
		if (error != 0)
			return error;
	}

	/* Succeeded: both partitions are derived. */
	return 0;
}

/* Derives the motion of a macroblock of four sub-macroblocks, partition by partition in mbPartIdx and subMbPartIdx order (8.4.1). */
static int
recon_motion_sub(
	struct h264sw_slice_ctx *ctx)
{
	const struct h264sw_mb_syntax *syntax;
	const struct h264sw_sub_mb_type *sub;
	unsigned quadrant;
	unsigned part;
	unsigned x;
	unsigned y;
	int error;

	/* Takes the syntax. */
	syntax = &ctx->syntax;

	/* Derives each sub-macroblock in order, so that the later ones see the motion of the earlier ones. */
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		sub = &syntax->sub[quadrant];

		/* A direct sub-macroblock takes its motion from the direct prediction of its quadrant (8.4.1.2). */
		if (sub->pred == H264SW_PRED_DIRECT) {
			error = h264sw_direct_quadrant(ctx, quadrant);
			if (error != 0)
				return error;

			/* Marks the quadrant's blocks as having their motion for the sub-macroblocks after it. */
			recon_mark_decoded(ctx, 8U * (quadrant % 2U), 8U * (quadrant / 2U), 8U, 8U);
			continue;
		}

		/* Refuses a sub-macroblock type that does not describe 1, 2 or 4 partitions inside the 8x8 block. */
		if (sub->parts == 0U || sub->parts > 4U)
			return EINVAL;
		if (sub->part_width != 4U && sub->part_width != 8U)
			return EINVAL;
		if (sub->part_height != 4U && sub->part_height != 8U)
			return EINVAL;

		/* Derives each sub-macroblock partition, placed in raster order inside the 8x8 block. */
		for (part = 0; part < sub->parts; part++) {
			x = 8U * (quadrant % 2U) + (part * sub->part_width) % 8U;
			y = 8U * (quadrant / 2U) + ((part * sub->part_width) / 8U) * sub->part_height;
			error = recon_partition(ctx, sub->pred, quadrant, part, x, y, sub->part_width, sub->part_height);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: all four sub-macroblocks are derived. */
	return 0;
}

/*
 * Derives the vectors of one partition for each list its prediction mode uses.
 *
 * pred is the partition's prediction mode; part is its mbPartIdx and sub
 * its subMbPartIdx (0 for a macroblock partition), which select its
 * reference indices and motion vector differences in the syntax.  (x, y)
 * is its upper-left luma sample inside the macroblock.
 */
static int
recon_partition(
	struct h264sw_slice_ctx *ctx,
	unsigned pred,
	unsigned part,
	unsigned sub,
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height)
{
	const struct h264sw_mb_syntax *syntax;
	unsigned list;
	int uses;
	int used;
	int error;

	/* Takes the syntax. */
	syntax = &ctx->syntax;

	/* Refuses a partition that is not made of whole 4x4 blocks inside the macroblock, whatever the type says. */
	if (width == 0U || height == 0U)
		return EINVAL;
	if (width % 4U != 0U || height % 4U != 0U)
		return EINVAL;
	if (x % 4U != 0U || y % 4U != 0U)
		return EINVAL;
	if (x + width > 16U || y + height > 16U)
		return EINVAL;
	if (part >= 4U || sub >= 4U)
		return EINVAL;

	/* Derives the list 0 vector, then the list 1 vector, of the lists the partition uses. */
	used = 0;
	for (list = 0; list < 2U; list++) {
		uses = recon_uses_list(pred, list);
		if (uses == 0)
			continue;
		used = 1;
		error = recon_vector(ctx, list, syntax->ref_idx[list][part], syntax->mvd[list][part][sub], x, y, width, height);
		if (error != 0)
			return error;
	}

	/* Refuses a partition that predicts from no list: it has no motion to give. */
	if (used == 0)
		return EINVAL;

	/* The partition's blocks now have their motion for the partitions after it. */
	recon_mark_decoded(ctx, x, y, width, height);

	/* Succeeded: the partition's vectors are derived. */
	return 0;
}

/* Derives the vector and records the reference of one partition for one list: mvLX = mvpLX + mvdLX (8-172, 8-173). */
static int
recon_vector(
	struct h264sw_slice_ctx *ctx,
	unsigned list,
	int ref_idx,
	const int16_t *mvd,
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height)
{
	struct h264sw_motion *motion;
	const struct h264sw_frame *frame;
	int16_t mvp[2];
	int mv_x;
	int mv_y;
	unsigned block_x;
	unsigned block_y;
	unsigned blk;
	unsigned quadrant;

	/* Takes the motion record of the macroblock being written. */
	motion = &ctx->n.current->motion;

	/* Refuses a reference index outside the list or naming no frame. */
	if (ref_idx < 0)
		return EINVAL;
	if ((unsigned)ref_idx >= ctx->lists.count[list])
		return EINVAL;
	frame = ctx->lists.list[list][ref_idx].frame;
	if (frame == NULL)
		return EINVAL;

	/* Predicts the vector from the neighbouring partitions (8.4.1.3). */
	h264sw_mvpred(ctx, list, x, y, width, height, ref_idx, mvp);

	/* Adds the difference in int and wraps the sum into the range of int16_t by arithmetic. */
	mv_x = recon_wrap_vector((int)mvp[0] + (int)mvd[0]);
	mv_y = recon_wrap_vector((int)mvp[1] + (int)mvd[1]);

	/* Gives every 4x4 block of the partition the vector, and its quadrant the reference index and the id of the frame. */
	for (block_y = y; block_y < y + height; block_y += 4U) {
		for (block_x = x; block_x < x + width; block_x += 4U) {
			blk = h264sw_blk4x4_at(block_x, block_y);
			motion->mv[list][blk][0] = (int16_t)mv_x;
			motion->mv[list][blk][1] = (int16_t)mv_y;
			quadrant = 2U * (block_y / 8U) + block_x / 8U;
			motion->ref_idx[list][quadrant] = (int8_t)ref_idx;
			motion->ref_id[list][quadrant] = frame->id;
		}
	}

	/* Succeeded: the partition's vector is stored. */
	return 0;
}

/* Marks the 4x4 blocks of a rectangle as having their motion derived, so that the partitions after it may use them as neighbours. */
static void
recon_mark_decoded(
	struct h264sw_slice_ctx *ctx,
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height)
{
	unsigned block_x;
	unsigned block_y;
	unsigned blk;

	/* Visits each 4x4 block of the rectangle. */
	for (block_y = y; block_y < y + height; block_y += 4U) {
		for (block_x = x; block_x < x + width; block_x += 4U) {
			blk = h264sw_blk4x4_at(block_x, block_y);
			ctx->decoded_4x4 |= (uint32_t)1U << blk;
		}
	}
}

/* Tells whether all 4x4 blocks of a size by size square of the macroblock have the same vectors and reference indices, so one prediction covers them. */
static int
recon_uniform(
	const struct h264sw_mb *mb,
	unsigned x,
	unsigned y,
	unsigned size)
{
	unsigned first;
	unsigned first_quadrant;
	unsigned blk;
	unsigned quadrant;
	unsigned list;
	unsigned block_x;
	unsigned block_y;

	/* Finds the first block of the square and its quadrant, which the others are compared with. */
	first = h264sw_blk4x4_at(x, y);
	first_quadrant = 2U * (y / 8U) + x / 8U;

	/* Compares each block of the square with the first. */
	for (block_y = y; block_y < y + size; block_y += 4U) {
		for (block_x = x; block_x < x + size; block_x += 4U) {
			blk = h264sw_blk4x4_at(block_x, block_y);
			quadrant = 2U * (block_y / 8U) + block_x / 8U;
			for (list = 0; list < 2U; list++) {
				if (mb->motion.mv[list][blk][0] != mb->motion.mv[list][first][0])
					return 0;
				if (mb->motion.mv[list][blk][1] != mb->motion.mv[list][first][1])
					return 0;
				if (mb->motion.ref_idx[list][quadrant] != mb->motion.ref_idx[list][first_quadrant])
					return 0;
			}
		}
	}

	/* One prediction covers the whole square. */
	return 1;
}

/* Predicts the macroblock's samples from its motion, in the largest squares that share one motion (8.4.2). */
static int
recon_predict(
	struct h264sw_slice_ctx *ctx)
{
	const struct h264sw_mb *mb;
	unsigned quadrant;
	unsigned part;
	unsigned x;
	unsigned y;
	int uniform;
	int error;

	/* Takes the entry being read. */
	mb = ctx->n.current;

	/* A macroblock that moves as one is predicted as one 16x16 block. */
	uniform = recon_uniform(mb, 0U, 0U, 16U);
	if (uniform != 0) {
		error = recon_predict_block(ctx, 0U, 0U, 16U);
		if (error != 0)
			return error;

		/* Succeeded: the macroblock is predicted. */
		return 0;
	}

	/* Otherwise predicts each 8x8 quadrant that moves as one as one block, and the others block by block. */
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		x = 8U * (quadrant % 2U);
		y = 8U * (quadrant / 2U);
		uniform = recon_uniform(mb, x, y, 8U);
		if (uniform != 0) {
			error = recon_predict_block(ctx, x, y, 8U);
			if (error != 0)
				return error;
			continue;
		}

		/* Predicts the quadrant block by block when its blocks move differently. */
		for (part = 0; part < 4U; part++) {
			error = recon_predict_block(ctx, x + 4U * (part % 2U), y + 4U * (part / 2U), 4U);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the macroblock is predicted. */
	return 0;
}

/*
 * Predicts one square block of the macroblock from its reference frames (8.4.2.1 to 8.4.2.3).
 *
 * (x, y) is the block's upper-left luma sample inside the macroblock
 * and size its width and height (16, 8 or 4).  The block's reference
 * indices come from the quadrant it lies in and its vectors from its
 * first 4x4 block.
 */
static int
recon_predict_block(
	struct h264sw_slice_ctx *ctx,
	unsigned x,
	unsigned y,
	unsigned size)
{
	const struct h264sw_mb *mb;
	const struct h264sw_frame *frame;
	const struct h264sw_ref *ref;
	struct h264sw_weight_factors factors;
	uint8_t luma[2][256];
	uint8_t chroma[2][2][64];
	const uint8_t *prediction[2];
	uint8_t *dst;
	unsigned quadrant;
	unsigned blk;
	unsigned list;
	unsigned component;
	unsigned chroma_size;
	int ref_idx[2];
	int mv_x;
	int mv_y;
	int whole_x;
	int whole_y;
	int plane_x;
	int plane_y;

	/* Takes the entry being read, the frame, the block's quadrant and first 4x4 block, and the chroma block size. */
	mb = ctx->n.current;
	frame = ctx->picture->frame;
	quadrant = 2U * (y / 8U) + x / 8U;
	blk = h264sw_blk4x4_at(x, y);
	chroma_size = size / 2U;

	/* Takes the reference index of each list from the block's quadrant. */
	ref_idx[0] = mb->motion.ref_idx[0][quadrant];
	ref_idx[1] = mb->motion.ref_idx[1][quadrant];

	/* Refuses a block that predicts from no list. */
	if (ref_idx[0] < 0 && ref_idx[1] < 0)
		return EINVAL;

	/* Predicts the block from each list it uses: luma at quarter samples, Cb and Cr at eighth samples. */
	prediction[0] = NULL;
	prediction[1] = NULL;
	for (list = 0; list < 2U; list++) {
		if (ref_idx[list] < 0)
			continue;

		/* Refuses an index outside the list, a missing frame and a frame of another size. */
		if ((unsigned)ref_idx[list] >= ctx->lists.count[list])
			return EINVAL;
		ref = &ctx->lists.list[list][ref_idx[list]];
		if (ref->frame == NULL)
			return EINVAL;
		if (ref->frame->plane[0] == NULL)
			return EINVAL;
		if (ref->frame->plane[1] == NULL)
			return EINVAL;
		if (ref->frame->plane[2] == NULL)
			return EINVAL;
		if (ref->frame->width != frame->width)
			return EINVAL;
		if (ref->frame->height != frame->height)
			return EINVAL;

		/* A position the lists could not fill is predicted from a stand-in, and the macroblock counts as concealed. */
		if (ref->missing != 0)
			ctx->concealed++;

		/* Splits the luma vector into its whole-sample part and its quarter-sample fraction, rounding down (8-223 to 8-226). */
		mv_x = mb->motion.mv[list][blk][0];
		mv_y = mb->motion.mv[list][blk][1];
		whole_x = recon_floor_div(mv_x, 4);
		whole_y = recon_floor_div(mv_y, 4);
		plane_x = (int)(16U * ctx->n.x + x) + whole_x;
		plane_y = (int)(16U * ctx->n.y + y) + whole_y;
		h264sw_interp_luma(ref->frame->plane[0], ref->frame->stride[0], ref->frame->width, ref->frame->height, plane_x, plane_y, (unsigned)(mv_x - 4 * whole_x), (unsigned)(mv_y - 4 * whole_y), size, size, luma[list], size);
		prediction[list] = luma[list];

		/* The chroma vector of a frame is the luma vector in eighth chroma samples (8.4.1.4); splits it likewise (8-227 to 8-230). */
		whole_x = recon_floor_div(mv_x, 8);
		whole_y = recon_floor_div(mv_y, 8);
		plane_x = (int)(8U * ctx->n.x + x / 2U) + whole_x;
		plane_y = (int)(8U * ctx->n.y + y / 2U) + whole_y;
		for (component = 0; component < 2U; component++)
			h264sw_interp_chroma(ref->frame->plane[1U + component], ref->frame->stride[1U + component], ref->frame->width / 2U, ref->frame->height / 2U, plane_x, plane_y, (unsigned)(mv_x - 8 * whole_x), (unsigned)(mv_y - 8 * whole_y), chroma_size, chroma_size, chroma[list][component], chroma_size);
	}

	/* Weights the luma predictions and writes the block into the frame (8.4.2.3). */
	h264sw_weights_of(&ctx->weights, ref_idx[0], ref_idx[1], 0U, &factors);
	dst = recon_sample_at(frame, 0, 16U * ctx->n.x + x, 16U * ctx->n.y + y);
	h264sw_weights_apply(&factors, prediction[0], prediction[1], size, size, size, dst, frame->stride[0]);

	/* Weights the Cb and Cr predictions likewise with the chroma factors. */
	for (component = 0; component < 2U; component++) {
		h264sw_weights_of(&ctx->weights, ref_idx[0], ref_idx[1], 1U + component, &factors);
		prediction[0] = NULL;
		prediction[1] = NULL;
		if (ref_idx[0] >= 0)
			prediction[0] = chroma[0][component];
		if (ref_idx[1] >= 0)
			prediction[1] = chroma[1][component];
		dst = recon_sample_at(frame, 1U + component, 8U * ctx->n.x + x / 2U, 8U * ctx->n.y + y / 2U);
		h264sw_weights_apply(&factors, prediction[0], prediction[1], chroma_size, chroma_size, chroma_size, dst, frame->stride[1U + component]);
	}

	/* Succeeded: the block is predicted. */
	return 0;
}
