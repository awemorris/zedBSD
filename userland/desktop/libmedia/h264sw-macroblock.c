/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slice data and the macroblock layer syntax of the software H.264
 * decoder (7.3.4, 7.3.5).
 */

#include "h264sw-macroblock.h"
#include "h264sw-cabac-se.h"
#include "h264sw-cavlc.h"
#include "h264sw-context.h"
#include "h264sw-recon.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* The range of mb_qp_delta (7.4.5) for 8 bit samples. */
#define MB_QP_DELTA_MIN (-26)
#define MB_QP_DELTA_MAX 25

/* The range of a motion vector difference component in quarter luma samples (7.4.5.1). */
#define MB_MVD_MIN (-32768)
#define MB_MVD_MAX 32767

/* The largest absolute mvd a macroblock entry keeps for the CABAC context of mvd (9.3.3.1.1.7). */
#define MB_MVD_CAP 255

/* The samples of an I_PCM macroblock in 4:2:0: 256 luma and 2 * 64 chroma (7.3.5). */
#define MB_PCM_BYTES 384U

/* The largest value intra_chroma_pred_mode may have (7.4.5.1). */
#define MB_CHROMA_PRED_MAX 3U

/* The coded_block_pattern recorded for I_PCM: all luma bits and CodedBlockPatternChroma 2 (struct h264sw_mb). */
#define MB_CBP_PCM 0x2fU

/* The coded_flags recorded for I_PCM: every one of the 27 block flags (struct h264sw_mb). */
#define MB_CODED_ALL 0x07ffffffU

/* The direct bits of a macroblock whose four quadrants are all predicted in direct mode. */
#define MB_DIRECT_ALL 0x0fU

/* The bit of coded_flags for the Intra_16x16 luma DC, the chroma DC of Cb and the chroma AC blocks of Cb (struct h264sw_mb). */
#define MB_FLAG_LUMA_DC 16U
#define MB_FLAG_CHROMA_DC 17U
#define MB_FLAG_CHROMA_AC 19U

/* The index of the first Cb AC count in total_coeff, and the block number the CAVLC context uses for it (struct h264sw_mb, h264sw_cavlc_nc). */
#define MB_TOTAL_CHROMA_AC 16U

/* Forward declarations of the static functions. */
static int mb_uses_list(unsigned pred, unsigned list);
static void mb_partition_origin(unsigned index, unsigned width, unsigned height, unsigned base_x, unsigned base_y, unsigned span, unsigned *x, unsigned *y);
static void mb_store_ref_idx(struct h264sw_mb *mb, unsigned list, unsigned x, unsigned y, unsigned width, unsigned height, int value);
static void mb_store_mvd(struct h264sw_mb *mb, unsigned list, unsigned x, unsigned y, unsigned width, unsigned height, unsigned component, int value);
static void mb_syntax_reset(struct h264sw_mb_syntax *syntax);
static int mb_stream_error(const struct h264sw_slice_ctx *ctx);
static int mb_read_mb_type(struct h264sw_slice_ctx *ctx, unsigned *mb_type);
static int mb_read_sub_mb_type(struct h264sw_slice_ctx *ctx, unsigned *sub_mb_type);
static int mb_read_ref_idx(struct h264sw_slice_ctx *ctx, unsigned list, unsigned x, unsigned y, int *ref_idx);
static int mb_read_mvd(struct h264sw_slice_ctx *ctx, unsigned list, unsigned x, unsigned y, unsigned component, int *mvd);
static int mb_read_cbp(struct h264sw_slice_ctx *ctx, int intra, unsigned *cbp);
static int mb_read_qp_delta(struct h264sw_slice_ctx *ctx, int *qp_delta);
static unsigned mb_read_transform_8x8(struct h264sw_slice_ctx *ctx);
static int mb_parse_skipped(struct h264sw_slice_ctx *ctx);
static int mb_parse_pcm(struct h264sw_slice_ctx *ctx);
static int mb_pred_intra(struct h264sw_slice_ctx *ctx);
static int mb_pred_inter(struct h264sw_slice_ctx *ctx);
static int mb_sub_pred(struct h264sw_slice_ctx *ctx, int *no_sub_part_less_than_8x8);
static int mb_residual(struct h264sw_slice_ctx *ctx);
static int mb_residual_luma(struct h264sw_slice_ctx *ctx);
static int mb_residual_chroma(struct h264sw_slice_ctx *ctx);
static int mb_residual_block(struct h264sw_slice_ctx *ctx, unsigned cat, unsigned blk, unsigned component, unsigned max_coeff, int32_t *levels);
static int mb_slice_skip(struct h264sw_slice_ctx *ctx, int *skipped);
static int mb_slice_end(struct h264sw_slice_ctx *ctx, int skipped, int *more_data);

/*
 * Decodes the macroblocks of a slice (7.3.4).
 *
 * The specification is the comment of the declaration in
 * h264sw-macroblock.h.
 */
int
h264sw_slice_data(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_picture_ctx *picture;
	struct h264sw_mb *entry;
	unsigned addr;
	int skipped;
	int more_data;
	int error;

	/* Refuses a slice type that has no macroblock layer here. */
	if (ctx->header->slice_type != H264SW_SLICE_I &&
	    ctx->header->slice_type != H264SW_SLICE_P &&
	    ctx->header->slice_type != H264SW_SLICE_B)
		return ENOTSUP;

	/* Starts the slice: no macroblock before the first, no skip run being counted, and the first mb_qp_delta context seeing no previous macroblock. */
	picture = ctx->picture;
	ctx->prev_mb_addr = -1;
	ctx->prev_qp_delta_nonzero = 0;
	ctx->prev_skipped = 0;
	ctx->skip_run = -1;
	addr = ctx->header->first_mb;

	/* Decodes one macroblock per round until the slice says it has no more. */
	more_data = 1;
	while (more_data != 0) {
		/* Refuses a slice that goes on after the last macroblock of the picture. */
		if (addr >= picture->size_mbs)
			return EINVAL;

		/* Refuses an address that an earlier slice (or this one) already decoded; its entry is not touched. */
		entry = &picture->mbs[addr];
		if (entry->slice != -1)
			return EINVAL;

		/* Prepares the macroblock: its entry belongs to this slice, and the state that lasts one macroblock starts clean. */
		h264sw_mb_reset(entry, ctx->slice_num);
		ctx->mb_addr = addr;
		ctx->decoded_4x4 = 0;
		ctx->direct.spatial_ready = 0;

		/* Finds the available neighbours of the macroblock. */
		h264sw_mb_neighbours(picture->mbs, picture->width_mbs, addr, &ctx->n);

		/* Reads the skip indication: mb_skip_run for CAVLC, mb_skip_flag for CABAC, nothing in an I slice. */
		error = mb_slice_skip(ctx, &skipped);
		if (error != 0) {
			h264sw_mb_reset(entry, -1);
			return error;
		}

		/* Parses the macroblock into ctx->syntax and its entry. */
		error = h264sw_macroblock_parse(ctx, skipped);
		if (error != 0) {
			h264sw_mb_reset(entry, -1);
			return error;
		}

		/* Reconstructs the macroblock from its syntax. */
		error = h264sw_recon_mb(ctx);
		if (error != 0) {
			h264sw_mb_reset(entry, -1);
			return error;
		}

		/* Records the macroblock as decoded and as the previous one in decoding order. */
		picture->decoded_mbs++;
		ctx->prev_mb_addr = (int32_t)addr;
		ctx->prev_skipped = skipped;

		/* Finds out whether another macroblock follows (more_rbsp_data, or end_of_slice_flag). */
		error = mb_slice_end(ctx, skipped, &more_data);
		if (error != 0)
			return error;

		/* Moves to the next macroblock address; there are no slice groups. */
		addr++;
	}

	/* Succeeded: the slice's macroblocks are decoded. */
	return 0;
}

/*
 * Parses one macroblock (7.3.5 and below) into ctx->syntax.
 *
 * The specification is the comment of the declaration in
 * h264sw-macroblock.h.
 */
int
h264sw_macroblock_parse(
	struct h264sw_slice_ctx *ctx,
	int skipped)
{
	struct h264sw_mb *mb;
	struct h264sw_mb_syntax *syntax;
	const struct h264sw_pps *pps;
	unsigned slice_type;
	unsigned mb_type;
	unsigned cbp;
	unsigned flag;
	int no_sub_part_less_than_8x8;
	int qp_delta;
	int qp;
	int intra;
	int error;

	/* Takes the entry being written; without one there is nothing to parse into. */
	mb = ctx->n.current;
	if (mb == NULL)
		return EINVAL;

	/* Takes the pieces the syntax depends on. */
	syntax = &ctx->syntax;
	pps = ctx->picture->pps;
	slice_type = ctx->header->slice_type;

	/* Starts the syntax from its inferred values: no partitions read, every list unused, no motion vector difference, every level 0. */
	mb_syntax_reset(syntax);

	/* Handles a skipped macroblock, which carries no more syntax. */
	if (skipped != 0) {
		error = mb_parse_skipped(ctx);
		if (error != 0)
			return error;

		/* Succeeded: the inferred values are set. */
		return 0;
	}

	/* Reads mb_type and gives it its meaning; a value outside the slice type's table is an error. */
	error = mb_read_mb_type(ctx, &mb_type);
	if (error != 0)
		return error;
	syntax->mb_type = mb_type;
	error = h264sw_mb_type_of(slice_type, mb_type, &syntax->type);
	if (error != 0)
		return error;

	/* Records the kind at once: the contexts of the elements that follow read it, and an intra kind marks the motion record. */
	mb->kind = (uint8_t)syntax->type.kind;
	if (syntax->type.kind < H264SW_MB_P_SKIP)
		mb->motion.intra = 1;

	/* An I_PCM macroblock has no prediction, pattern, delta or residual: only its samples. */
	if (syntax->type.kind == H264SW_MB_I_PCM) {
		error = mb_parse_pcm(ctx);
		if (error != 0)
			return error;

		/* Succeeded: the samples are read. */
		return 0;
	}

	/* A B_Direct_16x16 macroblock predicts every quadrant in direct mode; the contexts of later elements need to know that. */
	if (syntax->type.kind == H264SW_MB_B_DIRECT)
		mb->direct = MB_DIRECT_ALL;

	/* Reads the prediction syntax: sub_mb_pred for the 8x8 kinds, the transform size and the modes of I_NxN, the modes of Intra_16x16, or mb_pred of the inter kinds. */
	no_sub_part_less_than_8x8 = 1;
	if (syntax->type.kind == H264SW_MB_P_8X8 ||
	    syntax->type.kind == H264SW_MB_P_8X8REF0 ||
	    syntax->type.kind == H264SW_MB_B_8X8) {
		error = mb_sub_pred(ctx, &no_sub_part_less_than_8x8);
		if (error != 0)
			return error;
	} else if (syntax->type.kind == H264SW_MB_I_NXN) {
		/* I_NxN is Intra_8x8 when transform_size_8x8_flag is sent and 1, else Intra_4x4; the entry and the syntax then carry the resolved kind. */
		syntax->type.kind = H264SW_MB_I_4X4;
		if (pps->transform_8x8_mode != 0) {
			flag = mb_read_transform_8x8(ctx);
			if (flag != 0U) {
				syntax->type.kind = H264SW_MB_I_8X8;
				mb->transform_8x8 = 1;
			}
		}
		mb->kind = (uint8_t)syntax->type.kind;

		error = mb_pred_intra(ctx);
		if (error != 0)
			return error;
	} else if (syntax->type.kind == H264SW_MB_I_16X16) {
		error = mb_pred_intra(ctx);
		if (error != 0)
			return error;
	} else {
		error = mb_pred_inter(ctx);
		if (error != 0)
			return error;
	}

	/* Takes the coded block pattern of an Intra_16x16 macroblock from its mb_type; the other kinds send it. */
	if (syntax->type.kind == H264SW_MB_I_16X16) {
		syntax->cbp_luma = syntax->type.cbp_luma;
		syntax->cbp_chroma = syntax->type.cbp_chroma;
		mb->cbp = (uint8_t)(syntax->cbp_luma | (syntax->cbp_chroma << 4));
	} else {
		/* Reads coded_block_pattern; CAVLC maps it through the column of the intra or the inter modes. */
		intra = 0;
		if (syntax->type.kind < H264SW_MB_P_SKIP)
			intra = 1;
		error = mb_read_cbp(ctx, intra, &cbp);
		if (error != 0)
			return error;
		syntax->cbp_luma = cbp & 15U;
		syntax->cbp_chroma = cbp >> 4;
		mb->cbp = (uint8_t)cbp;

		/* Reads transform_size_8x8_flag of an inter macroblock where 7.3.5 allows it: some luma coded, the PPS allows it, and no sub-macroblock splits below 8x8. */
		if (syntax->type.kind >= H264SW_MB_P_SKIP &&
		    syntax->cbp_luma > 0U &&
		    pps->transform_8x8_mode != 0 &&
		    no_sub_part_less_than_8x8 != 0) {
			/* B_Direct_16x16 also needs direct_8x8_inference_flag, because its motion is derived per 4x4 block otherwise. */
			if (syntax->type.kind != H264SW_MB_B_DIRECT ||
			    ctx->picture->sps->direct_8x8_inference != 0) {
				flag = mb_read_transform_8x8(ctx);
				if (flag != 0U)
					mb->transform_8x8 = 1;
			}
		}
	}

	/* Reads mb_qp_delta when there is residual or the type is Intra_16x16. */
	qp_delta = 0;
	if (syntax->cbp_luma > 0U ||
	    syntax->cbp_chroma > 0U ||
	    syntax->type.kind == H264SW_MB_I_16X16) {
		error = mb_read_qp_delta(ctx, &qp_delta);
		if (error != 0)
			return error;
	}

	/* Updates QPY by 7-37 and records it for the macroblock and for the one after; the next mb_qp_delta context sees whether this delta was non-zero. */
	qp = ctx->qp;
	if (qp < 0 || qp > 51)
		return EINVAL;
	qp = (qp + qp_delta + 52) % 52;
	syntax->qp_delta = qp_delta;
	mb->qp = (uint8_t)qp;
	ctx->qp = qp;
	ctx->prev_qp_delta_nonzero = 0;
	if (qp_delta != 0)
		ctx->prev_qp_delta_nonzero = 1;

	/* Reads the residual blocks when there are any. */
	error = mb_residual(ctx);
	if (error != 0)
		return error;

	/* Reports a read past the end of the data as an error. */
	error = mb_stream_error(ctx);
	if (error != 0)
		return error;

	/* Succeeded: the macroblock's syntax is in ctx->syntax and its entry. */
	return 0;
}

/* Tells whether a partition's prediction mode uses a reference list. */
static int
mb_uses_list(
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

	/* Direct or no prediction sends nothing for the list. */
	return 0;
}

/*
 * Finds the upper-left luma location of a partition from its index.
 *
 * The partitions of a region span samples wide are placed in raster
 * order, each width by height; base is the region's own origin.
 */
static void
mb_partition_origin(
	unsigned index,
	unsigned width,
	unsigned height,
	unsigned base_x,
	unsigned base_y,
	unsigned span,
	unsigned *x,
	unsigned *y)
{
	/* Places the partition on the row and the column its index gives. */
	*x = base_x + (index * width) % span;
	*y = base_y + ((index * width) / span) * height;
}

/* Records a reference index in every 8x8 quadrant a partition covers. */
static void
mb_store_ref_idx(
	struct h264sw_mb *mb,
	unsigned list,
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height,
	int value)
{
	unsigned quadrant;
	unsigned quadrant_x;
	unsigned quadrant_y;

	/* Visits the four quadrants and marks the ones inside the partition. */
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		quadrant_x = 8U * (quadrant & 1U);
		quadrant_y = 8U * (quadrant >> 1);

		/* A quadrant counts when its upper-left corner lies in the partition. */
		if (quadrant_x >= x && quadrant_x < x + width &&
		    quadrant_y >= y && quadrant_y < y + height)
			mb->motion.ref_idx[list][quadrant] = (int8_t)value;
	}
}

/* Records the absolute mvd component, capped, in every 4x4 block a partition covers. */
static void
mb_store_mvd(
	struct h264sw_mb *mb,
	unsigned list,
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height,
	unsigned component,
	int value)
{
	unsigned blk;
	unsigned block_x;
	unsigned block_y;
	int magnitude;

	/* The context of mvd reads the magnitude, which the entry keeps at most 255. */
	magnitude = value;
	if (magnitude < 0)
		magnitude = -magnitude;
	if (magnitude > MB_MVD_CAP)
		magnitude = MB_MVD_CAP;

	/* Visits the sixteen 4x4 blocks and marks the ones inside the partition. */
	for (blk = 0; blk < 16U; blk++) {
		block_x = h264sw_blk4x4_x[blk];
		block_y = h264sw_blk4x4_y[blk];

		/* A block counts when its upper-left corner lies in the partition. */
		if (block_x >= x && block_x < x + width &&
		    block_y >= y && block_y < y + height)
			mb->mvd[list][blk][component] = (uint8_t)magnitude;
	}
}

/* Gives the syntax of a macroblock its inferred values before anything is read. */
static void
mb_syntax_reset(
	struct h264sw_mb_syntax *syntax)
{
	unsigned list;
	unsigned part;

	/* Clears the types, the intra mode records and the motion vector differences. */
	syntax->mb_type = 0;
	memset(&syntax->type, 0, sizeof(syntax->type));
	memset(syntax->sub_mb_type, 0, sizeof(syntax->sub_mb_type));
	memset(syntax->sub, 0, sizeof(syntax->sub));
	memset(syntax->prev_intra_flag, 0, sizeof(syntax->prev_intra_flag));
	memset(syntax->rem_intra_mode, 0, sizeof(syntax->rem_intra_mode));
	memset(syntax->mvd, 0, sizeof(syntax->mvd));

	/* Marks every list of every partition as unused until a prediction mode says otherwise. */
	for (list = 0; list < 2U; list++) {
		for (part = 0; part < 4U; part++)
			syntax->ref_idx[list][part] = -1;
	}

	/* Starts with no residual pattern and no quantizer change. */
	syntax->cbp_luma = 0;
	syntax->cbp_chroma = 0;
	syntax->qp_delta = 0;

	/* Clears every level, so a block the pattern leaves out is zero as 7.3.5.3 says. */
	memset(syntax->luma_dc, 0, sizeof(syntax->luma_dc));
	memset(syntax->luma, 0, sizeof(syntax->luma));
	memset(syntax->luma8x8, 0, sizeof(syntax->luma8x8));
	memset(syntax->chroma_dc, 0, sizeof(syntax->chroma_dc));
	memset(syntax->chroma_ac, 0, sizeof(syntax->chroma_ac));
}

/* Reports a read past the end of the data as EINVAL, whichever entropy coder the slice uses. */
static int
mb_stream_error(
	const struct h264sw_slice_ctx *ctx)
{
	/* CAVLC keeps its sticky error in the bit reader. */
	if (ctx->cabac_mode == 0 && ctx->bits.error != 0)
		return EINVAL;

	/* CABAC keeps it in the arithmetic decoder. */
	if (ctx->cabac_mode != 0 && ctx->cabac.error != 0)
		return EINVAL;

	/* Succeeded: no read failed. */
	return 0;
}

/* Reads mb_type: ue(v) for CAVLC, the binarization of 9.3.2.5 for CABAC. */
static int
mb_read_mb_type(
	struct h264sw_slice_ctx *ctx,
	unsigned *mb_type)
{
	uint32_t code;
	int error;

	/* Decodes the CABAC bin string. */
	if (ctx->cabac_mode != 0) {
		error = h264sw_cabac_mb_type(ctx, ctx->header->slice_type, mb_type);
		if (error != 0)
			return error;

		/* Succeeded: the value of the table. */
		return 0;
	}

	/* Reads the CAVLC code number. */
	code = h264sw_bits_ue(&ctx->bits);
	if (ctx->bits.error != 0)
		return EINVAL;

	/* Succeeded: the range is checked by the caller against the slice type's table. */
	*mb_type = code;
	return 0;
}

/* Reads sub_mb_type: ue(v) for CAVLC, Table 9-38 for CABAC. */
static int
mb_read_sub_mb_type(
	struct h264sw_slice_ctx *ctx,
	unsigned *sub_mb_type)
{
	uint32_t code;
	int error;

	/* Decodes the CABAC bin string. */
	if (ctx->cabac_mode != 0) {
		error = h264sw_cabac_sub_mb_type(ctx, ctx->header->slice_type, sub_mb_type);
		if (error != 0)
			return error;

		/* Succeeded: the value of the table. */
		return 0;
	}

	/* Reads the CAVLC code number. */
	code = h264sw_bits_ue(&ctx->bits);
	if (ctx->bits.error != 0)
		return EINVAL;

	/* Succeeded: the range is checked by the caller against the slice type's table. */
	*sub_mb_type = code;
	return 0;
}

/*
 * Reads ref_idx_lX of the partition at (x, y), or gives 0 when the slice
 * has only one reference index in the list and the syntax sends none.
 */
static int
mb_read_ref_idx(
	struct h264sw_slice_ctx *ctx,
	unsigned list,
	unsigned x,
	unsigned y,
	int *ref_idx)
{
	unsigned active;
	unsigned value;
	uint32_t code;
	int error;

	/* Without a second reference index, the index is not sent and is 0 (7.3.5.1). */
	active = ctx->header->num_ref_idx_active[list];
	if (active <= 1U) {
		*ref_idx = 0;
		return 0;
	}

	/* Decodes the CABAC unary code, which is bounded by the number of indices. */
	if (ctx->cabac_mode != 0) {
		error = h264sw_cabac_ref_idx(ctx, list, x, y, &value);
		if (error != 0)
			return error;
		*ref_idx = (int)value;

		/* Succeeded: the index. */
		return 0;
	}

	/* Reads the CAVLC te(v) whose largest value is the last index. */
	code = h264sw_bits_te(&ctx->bits, active - 1U);
	if (ctx->bits.error != 0)
		return EINVAL;

	/* Refuses an index past the end of the list. */
	if (code >= active)
		return EINVAL;

	/* Succeeded: the index. */
	*ref_idx = (int)code;
	return 0;
}

/* Reads one component of mvd_lX of the (sub-)partition at (x, y): se(v) for CAVLC. */
static int
mb_read_mvd(
	struct h264sw_slice_ctx *ctx,
	unsigned list,
	unsigned x,
	unsigned y,
	unsigned component,
	int *mvd)
{
	int32_t value;
	int error;

	/* Decodes the CABAC code, which keeps the value in range itself. */
	if (ctx->cabac_mode != 0) {
		error = h264sw_cabac_mvd(ctx, list, x, y, component, mvd);
		if (error != 0)
			return error;

		/* Succeeded: the component. */
		return 0;
	}

	/* Reads the CAVLC signed Exp-Golomb code. */
	value = h264sw_bits_se(&ctx->bits);
	if (ctx->bits.error != 0)
		return EINVAL;

	/* Refuses a value that does not fit the 16 bits the syntax keeps (se(v) can say more). */
	if (value < MB_MVD_MIN || value > MB_MVD_MAX)
		return EINVAL;

	/* Succeeded: the component. */
	*mvd = (int)value;
	return 0;
}

/* Reads coded_block_pattern: me(v) through Table 9-4 for CAVLC, the prefix and suffix of 9.3.2.6 for CABAC. */
static int
mb_read_cbp(
	struct h264sw_slice_ctx *ctx,
	int intra,
	unsigned *cbp)
{
	uint32_t code;
	int error;

	/* Decodes the CABAC bins. */
	if (ctx->cabac_mode != 0) {
		*cbp = h264sw_cabac_cbp(ctx);

		/* Succeeded: luma in bits 0..3, chroma in bits 4..5. */
		return 0;
	}

	/* Reads the CAVLC code number. */
	code = h264sw_bits_ue(&ctx->bits);
	if (ctx->bits.error != 0)
		return EINVAL;

	/* Maps it through the column of the intra or the inter modes; a code above 47 is refused. */
	error = h264sw_cavlc_cbp(code, intra, cbp);
	if (error != 0)
		return error;

	/* Succeeded: luma in bits 0..3, chroma in bits 4..5. */
	return 0;
}

/* Reads mb_qp_delta: se(v) for CAVLC, 9.3.2.7 for CABAC; a value outside -26..25 is refused. */
static int
mb_read_qp_delta(
	struct h264sw_slice_ctx *ctx,
	int *qp_delta)
{
	int32_t value;
	int error;

	/* Decodes the CABAC code, which keeps the value in range itself. */
	if (ctx->cabac_mode != 0) {
		error = h264sw_cabac_qp_delta(ctx, qp_delta);
		if (error != 0)
			return error;

		/* Succeeded: the delta. */
		return 0;
	}

	/* Reads the CAVLC signed Exp-Golomb code. */
	value = h264sw_bits_se(&ctx->bits);
	if (ctx->bits.error != 0)
		return EINVAL;

	/* Refuses a delta outside the range of 7.4.5. */
	if (value < MB_QP_DELTA_MIN || value > MB_QP_DELTA_MAX)
		return EINVAL;

	/* Succeeded: the delta. */
	*qp_delta = (int)value;
	return 0;
}

/* Reads transform_size_8x8_flag: u(1) for CAVLC, ctxIdx 399 and up for CABAC. */
static unsigned
mb_read_transform_8x8(
	struct h264sw_slice_ctx *ctx)
{
	unsigned flag;

	/* Decodes the CABAC bin. */
	if (ctx->cabac_mode != 0) {
		flag = h264sw_cabac_transform_8x8(ctx);

		/* Succeeded: the flag. */
		return flag;
	}

	/* Reads the CAVLC bit; a failed read gives 0 and sets the sticky error of the reader. */
	flag = h264sw_bits_u1(&ctx->bits);

	/* Succeeded: the flag. */
	return flag;
}

/*
 * Sets the inferred values of a skipped macroblock: P_Skip or B_Skip, no
 * pattern, no quantizer change, and the quantizer of the macroblock before.
 */
static int
mb_parse_skipped(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb *mb;
	unsigned slice_type;

	/* Only P and B slices have skipped macroblocks. */
	slice_type = ctx->header->slice_type;
	if (slice_type != H264SW_SLICE_P && slice_type != H264SW_SLICE_B)
		return EINVAL;

	/* Describes the skipped type. */
	mb = ctx->n.current;
	h264sw_mb_skip_type(slice_type, &ctx->syntax.type);

	/* Records it in the entry; B_Skip predicts all four quadrants in direct mode. */
	mb->kind = (uint8_t)ctx->syntax.type.kind;
	if (ctx->syntax.type.kind == H264SW_MB_B_SKIP)
		mb->direct = MB_DIRECT_ALL;

	/* QPY carries over from the macroblock before (mb_qp_delta is inferred 0), and the next mb_qp_delta context sees no delta. */
	mb->qp = (uint8_t)ctx->qp;
	ctx->prev_qp_delta_nonzero = 0;

	/* Succeeded: the macroblock's inferred values are set. */
	return 0;
}

/*
 * Reads the samples of an I_PCM macroblock (7.3.5) and records what the
 * neighbours' contexts expect of one.
 */
static int
mb_parse_pcm(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb *mb;
	size_t bits_left;
	unsigned skip_bits;
	unsigned index;
	int error;

	mb = ctx->n.current;

	/* Reads the samples with the CABAC engine, which finds the byte position itself and restarts afterwards. */
	if (ctx->cabac_mode != 0) {
		error = h264sw_cabac_pcm(&ctx->cabac, ctx->syntax.pcm, MB_PCM_BYTES);
		if (error != 0)
			return error;
	} else {
		/* Skips the pcm_alignment_zero_bit bits up to the byte boundary. */
		skip_bits = (unsigned)(ctx->bits.position % 8U);
		if (skip_bits != 0U)
			h264sw_bits_skip(&ctx->bits, 8U - skip_bits);

		/* Refuses a macroblock whose samples run past the end of the data. */
		bits_left = h264sw_bits_left(&ctx->bits);
		if (bits_left < (size_t)MB_PCM_BYTES * 8U)
			return EINVAL;

		/* Reads the 256 luma samples and the 2 * 64 chroma samples. */
		for (index = 0; index < MB_PCM_BYTES; index++)
			ctx->syntax.pcm[index] = (uint8_t)h264sw_bits_u(&ctx->bits, 8U);
	}

	/* An I_PCM macroblock counts as fully coded for the contexts, has QPY 0 for the deblocking filter, and leaves QPY of the slice as it was. */
	mb->cbp = MB_CBP_PCM;
	mb->qp = 0;
	mb->coded_flags = MB_CODED_ALL;
	memset(mb->total_coeff, 16, sizeof(mb->total_coeff));
	ctx->prev_qp_delta_nonzero = 0;

	/* Reports a read past the end of the data as an error. */
	error = mb_stream_error(ctx);
	if (error != 0)
		return error;

	/* Succeeded: the samples are in ctx->syntax.pcm. */
	return 0;
}

/* Reads the intra part of mb_pred (7.3.5.1): the pred mode flags of Intra_4x4 or Intra_8x8 and intra_chroma_pred_mode. */
static int
mb_pred_intra(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb *mb;
	struct h264sw_mb_syntax *syntax;
	unsigned count;
	unsigned index;
	unsigned flag;
	unsigned rem;
	unsigned chroma_mode;
	uint32_t code;
	int error;

	mb = ctx->n.current;
	syntax = &ctx->syntax;

	/* Intra_4x4 sends 16 modes, Intra_8x8 four, Intra_16x16 none (its mode is in mb_type). */
	count = 0;
	if (syntax->type.kind == H264SW_MB_I_4X4)
		count = 16;
	if (syntax->type.kind == H264SW_MB_I_8X8)
		count = 4;

	/* Reads prev_intra_pred_mode_flag and, when it is 0, rem_intra_pred_mode for each block. */
	for (index = 0; index < count; index++) {
		rem = 0;
		if (ctx->cabac_mode != 0) {
			h264sw_cabac_intra_mode(ctx, &flag, &rem);
		} else {
			flag = h264sw_bits_u1(&ctx->bits);
			if (flag == 0U)
				rem = h264sw_bits_u(&ctx->bits, 3U);
		}
		syntax->prev_intra_flag[index] = (uint8_t)flag;
		syntax->rem_intra_mode[index] = (uint8_t)rem;
	}

	/* Reads intra_chroma_pred_mode (ChromaArrayType 1): ue(v) for CAVLC, at most 3. */
	if (ctx->cabac_mode != 0) {
		chroma_mode = h264sw_cabac_chroma_mode(ctx);
	} else {
		code = h264sw_bits_ue(&ctx->bits);
		if (ctx->bits.error != 0)
			return EINVAL;
		if (code > MB_CHROMA_PRED_MAX)
			return EINVAL;
		chroma_mode = code;
	}
	mb->chroma_pred_mode = (uint8_t)chroma_mode;

	/* Reports a read past the end of the data as an error. */
	error = mb_stream_error(ctx);
	if (error != 0)
		return error;

	/* Succeeded: the modes are in the syntax. */
	return 0;
}

/* Reads the inter part of mb_pred (7.3.5.1) for a macroblock of one or two partitions: all ref_idx, then all mvd. */
static int
mb_pred_inter(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb *mb;
	struct h264sw_mb_syntax *syntax;
	unsigned list;
	unsigned part;
	unsigned parts;
	unsigned width;
	unsigned height;
	unsigned component;
	unsigned x[2];
	unsigned y[2];
	int uses;
	int ref_idx;
	int mvd;
	int error;

	mb = ctx->n.current;
	syntax = &ctx->syntax;

	/* B_Direct_16x16 has no mb_pred syntax: its motion is derived. */
	if (syntax->type.kind == H264SW_MB_B_DIRECT)
		return 0;

	/* Places the one or two partitions in the macroblock. */
	parts = syntax->type.parts;
	width = syntax->type.part_width;
	height = syntax->type.part_height;
	if (parts == 0U || parts > 2U)
		return EINVAL;
	for (part = 0; part < parts; part++)
		mb_partition_origin(part, width, height, 0U, 0U, 16U, &x[part], &y[part]);

	/* Reads ref_idx_l0 of every partition that uses list 0, then ref_idx_l1 of every one that uses list 1; each is recorded as soon as it is read because the next one's context reads it. */
	for (list = 0; list < 2U; list++) {
		for (part = 0; part < parts; part++) {
			/* A partition that does not use the list keeps ref_idx -1. */
			uses = mb_uses_list(syntax->type.pred[part], list);
			if (uses == 0)
				continue;

			error = mb_read_ref_idx(ctx, list, x[part], y[part], &ref_idx);
			if (error != 0)
				return error;
			syntax->ref_idx[list][part] = (int8_t)ref_idx;
			mb_store_ref_idx(mb, list, x[part], y[part], width, height, ref_idx);
		}
	}

	/* Reads mvd_l0 of every partition that uses list 0, then mvd_l1 of every one that uses list 1, both components each. */
	for (list = 0; list < 2U; list++) {
		for (part = 0; part < parts; part++) {
			/* A partition that does not use the list sends no difference. */
			uses = mb_uses_list(syntax->type.pred[part], list);
			if (uses == 0)
				continue;

			for (component = 0; component < 2U; component++) {
				error = mb_read_mvd(ctx, list, x[part], y[part], component, &mvd);
				if (error != 0)
					return error;
				syntax->mvd[list][part][0][component] = (int16_t)mvd;
				mb_store_mvd(mb, list, x[part], y[part], width, height, component, mvd);
			}
		}
	}

	/* Reports a read past the end of the data as an error. */
	error = mb_stream_error(ctx);
	if (error != 0)
		return error;

	/* Succeeded: the reference indices and differences are in the syntax. */
	return 0;
}

/*
 * Reads sub_mb_pred (7.3.5.2) of a macroblock with four sub-macroblocks
 * and finds noSubMbPartSizeLessThan8x8Flag.
 */
static int
mb_sub_pred(
	struct h264sw_slice_ctx *ctx,
	int *no_sub_part_less_than_8x8)
{
	struct h264sw_mb *mb;
	struct h264sw_mb_syntax *syntax;
	unsigned slice_type;
	unsigned quadrant;
	unsigned list;
	unsigned sub;
	unsigned component;
	unsigned base_x;
	unsigned base_y;
	unsigned x;
	unsigned y;
	unsigned sub_mb_type;
	int uses;
	int ref_idx;
	int mvd;
	int error;

	mb = ctx->n.current;
	syntax = &ctx->syntax;
	slice_type = ctx->header->slice_type;

	/* Reads the four sub_mb_type values, and notes the quadrants predicted in direct mode and whether any sub-macroblock splits below 8x8. */
	*no_sub_part_less_than_8x8 = 1;
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		error = mb_read_sub_mb_type(ctx, &sub_mb_type);
		if (error != 0)
			return error;
		error = h264sw_sub_mb_type_of(slice_type, sub_mb_type, &syntax->sub[quadrant]);
		if (error != 0)
			return error;
		syntax->sub_mb_type[quadrant] = sub_mb_type;

		/* B_Direct_8x8 splits below 8x8 unless direct_8x8_inference_flag is set; the others split by their part count. */
		if (syntax->sub[quadrant].pred == H264SW_PRED_DIRECT) {
			mb->direct = (uint8_t)(mb->direct | (1U << quadrant));
			if (ctx->picture->sps->direct_8x8_inference == 0)
				*no_sub_part_less_than_8x8 = 0;
		} else if (syntax->sub[quadrant].parts > 1U) {
			*no_sub_part_less_than_8x8 = 0;
		}
	}

	/* Reads ref_idx_l0 of the sub-macroblocks that use list 0, then ref_idx_l1; P_8x8ref0 sends none and uses index 0. */
	for (list = 0; list < 2U; list++) {
		for (quadrant = 0; quadrant < 4U; quadrant++) {
			/* A sub-macroblock predicted in direct mode or not using the list keeps -1. */
			uses = mb_uses_list(syntax->sub[quadrant].pred, list);
			if (uses == 0)
				continue;

			base_x = 8U * (quadrant & 1U);
			base_y = 8U * (quadrant >> 1);
			ref_idx = 0;
			if (syntax->type.kind != H264SW_MB_P_8X8REF0) {
				error = mb_read_ref_idx(ctx, list, base_x, base_y, &ref_idx);
				if (error != 0)
					return error;
			}
			syntax->ref_idx[list][quadrant] = (int8_t)ref_idx;
			mb_store_ref_idx(mb, list, base_x, base_y, 8U, 8U, ref_idx);
		}
	}

	/* Reads mvd_l0 of every sub-macroblock partition that uses list 0, then mvd_l1. */
	for (list = 0; list < 2U; list++) {
		for (quadrant = 0; quadrant < 4U; quadrant++) {
			/* A sub-macroblock predicted in direct mode or not using the list sends no difference. */
			uses = mb_uses_list(syntax->sub[quadrant].pred, list);
			if (uses == 0)
				continue;

			base_x = 8U * (quadrant & 1U);
			base_y = 8U * (quadrant >> 1);
			for (sub = 0; sub < syntax->sub[quadrant].parts; sub++) {
				mb_partition_origin(sub, syntax->sub[quadrant].part_width, syntax->sub[quadrant].part_height, base_x, base_y, 8U, &x, &y);
				for (component = 0; component < 2U; component++) {
					error = mb_read_mvd(ctx, list, x, y, component, &mvd);
					if (error != 0)
						return error;
					syntax->mvd[list][quadrant][sub][component] = (int16_t)mvd;
					mb_store_mvd(mb, list, x, y, syntax->sub[quadrant].part_width, syntax->sub[quadrant].part_height, component, mvd);
				}
			}
		}
	}

	/* Reports a read past the end of the data as an error. */
	error = mb_stream_error(ctx);
	if (error != 0)
		return error;

	/* Succeeded: the sub-macroblock types, reference indices and differences are in the syntax. */
	return 0;
}

/* Reads residual( 0, 15 ) (7.3.5.3) when the macroblock has any residual. */
static int
mb_residual(
	struct h264sw_slice_ctx *ctx)
{
	int error;

	/* Reads nothing when the pattern is empty and the type is not Intra_16x16 (which always has its DC block). */
	if (ctx->syntax.cbp_luma == 0U &&
	    ctx->syntax.cbp_chroma == 0U &&
	    ctx->syntax.type.kind != H264SW_MB_I_16X16)
		return 0;

	/* Reads the luma blocks. */
	error = mb_residual_luma(ctx);
	if (error != 0)
		return error;

	/* Reads the chroma blocks of 4:2:0. */
	error = mb_residual_chroma(ctx);
	if (error != 0)
		return error;

	/* Succeeded: the levels are in the syntax. */
	return 0;
}

/* Reads residual_luma (7.3.5.3.1). */
static int
mb_residual_luma(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb *mb;
	struct h264sw_mb_syntax *syntax;
	unsigned quadrant;
	unsigned part;
	unsigned blk;
	unsigned index;
	int32_t list[16];
	int intra16x16;
	int error;

	mb = ctx->n.current;
	syntax = &ctx->syntax;
	intra16x16 = 0;
	if (syntax->type.kind == H264SW_MB_I_16X16)
		intra16x16 = 1;

	/* Reads the Intra_16x16 DC block first. */
	if (intra16x16 != 0) {
		error = mb_residual_block(ctx, H264SW_CAT_LUMA_DC, 0U, 0U, 16U, syntax->luma_dc);
		if (error != 0)
			return error;
	}

	/* Reads the four 8x8 luma blocks in order. */
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		/* A quadrant whose pattern bit is clear has all its levels 0, which the syntax already holds. */
		if (((syntax->cbp_luma >> quadrant) & 1U) == 0U)
			continue;

		/* An 8x8 transform with CABAC is one block of 64 levels, coded without coded_block_flag; its four 4x4 flags are the inferred flag. */
		if (mb->transform_8x8 != 0 && ctx->cabac_mode != 0) {
			error = mb_residual_block(ctx, H264SW_CAT_LUMA_8X8, quadrant, 0U, 64U, syntax->luma8x8[quadrant]);
			if (error != 0)
				return error;
			continue;
		}

		/* Otherwise reads four 4x4 blocks of the quadrant. */
		for (part = 0; part < 4U; part++) {
			blk = quadrant * 4U + part;

			/* An Intra_16x16 AC block has 15 levels placed at 1..15; the DC position stays 0. */
			if (intra16x16 != 0) {
				error = mb_residual_block(ctx, H264SW_CAT_LUMA_AC, blk, 0U, 15U, &syntax->luma[blk][1]);
				if (error != 0)
					return error;
				continue;
			}

			/* An 8x8 transform with CAVLC interleaves the four lists into the 64 levels: list number part supplies level 4 * i + part (7.3.5.3.1). */
			if (mb->transform_8x8 != 0) {
				error = mb_residual_block(ctx, H264SW_CAT_LUMA_4X4, blk, 0U, 16U, list);
				if (error != 0)
					return error;
				for (index = 0; index < 16U; index++)
					syntax->luma8x8[quadrant][4U * index + part] = list[index];
				continue;
			}

			/* A 4x4 block has 16 levels. */
			error = mb_residual_block(ctx, H264SW_CAT_LUMA_4X4, blk, 0U, 16U, syntax->luma[blk]);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the luma levels are in the syntax. */
	return 0;
}

/* Reads the chroma part of residual (7.3.5.3) for 4:2:0: both DC blocks, then the AC blocks of Cb and of Cr. */
static int
mb_residual_chroma(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb_syntax *syntax;
	unsigned component;
	unsigned blk;
	int error;

	syntax = &ctx->syntax;

	/* Reads the chroma DC block of Cb then Cr when any chroma is coded. */
	if ((syntax->cbp_chroma & 3U) != 0U) {
		for (component = 0; component < 2U; component++) {
			error = mb_residual_block(ctx, H264SW_CAT_CHROMA_DC, 0U, component + 1U, 4U, syntax->chroma_dc[component]);
			if (error != 0)
				return error;
		}
	}

	/* Reads the four AC blocks of Cb then of Cr when the chroma AC is coded; the 15 levels go to positions 1..15. */
	if ((syntax->cbp_chroma & 2U) != 0U) {
		for (component = 0; component < 2U; component++) {
			for (blk = 0; blk < 4U; blk++) {
				error = mb_residual_block(ctx, H264SW_CAT_CHROMA_AC, blk, component + 1U, 15U, &syntax->chroma_ac[component][blk][1]);
				if (error != 0)
					return error;
			}
		}
	}

	/* Succeeded: the chroma levels are in the syntax. */
	return 0;
}

/*
 * Reads one residual block with the slice's entropy coder and records
 * what the contexts of the blocks after it read: TotalCoeff for CAVLC,
 * coded_block_flag for CABAC.
 */
static int
mb_residual_block(
	struct h264sw_slice_ctx *ctx,
	unsigned cat,
	unsigned blk,
	unsigned component,
	unsigned max_coeff,
	int32_t *levels)
{
	struct h264sw_mb *mb;
	unsigned total_coeff;
	unsigned coded;
	unsigned bit;
	unsigned index;
	unsigned nc_block;
	int nc;
	int error;

	mb = ctx->n.current;

	/* Reads the block with CABAC, then records its coded_block_flag in the bit of its category. */
	if (ctx->cabac_mode != 0) {
		coded = 0;
		error = h264sw_cabac_residual_block(ctx, cat, blk, component, 0U, max_coeff - 1U, max_coeff, levels, &coded);
		if (error != 0)
			return error;

		/* Finds the first bit of coded_flags the block owns (struct h264sw_mb). */
		bit = blk;
		if (cat == H264SW_CAT_LUMA_DC)
			bit = MB_FLAG_LUMA_DC;
		if (cat == H264SW_CAT_CHROMA_DC)
			bit = MB_FLAG_CHROMA_DC + (component - 1U);
		if (cat == H264SW_CAT_CHROMA_AC)
			bit = MB_FLAG_CHROMA_AC + 4U * (component - 1U) + blk;

		/* An 8x8 block sets all four bits of its quadrant; the others set their own. */
		if (coded != 0U) {
			if (cat == H264SW_CAT_LUMA_8X8) {
				for (index = 0; index < 4U; index++)
					mb->coded_flags |= (uint32_t)1U << (4U * blk + index);
			} else {
				mb->coded_flags |= (uint32_t)1U << bit;
			}
		}

		/* Succeeded: the levels and the flag are recorded. */
		return 0;
	}

	/* Finds nC from the counts of the neighbouring blocks (9.2.1); the chroma DC has its own column, the Intra_16x16 DC block uses block 0's neighbours. */
	nc = H264SW_NC_CHROMA_DC;
	if (cat == H264SW_CAT_LUMA_DC) {
		nc_block = 0;
		nc = h264sw_cavlc_nc(ctx, nc_block);
	}
	if (cat == H264SW_CAT_LUMA_AC || cat == H264SW_CAT_LUMA_4X4) {
		nc_block = blk;
		nc = h264sw_cavlc_nc(ctx, nc_block);
	}
	if (cat == H264SW_CAT_CHROMA_AC) {
		nc_block = MB_TOTAL_CHROMA_AC + 4U * (component - 1U) + blk;
		nc = h264sw_cavlc_nc(ctx, nc_block);
	}

	/* Reads the block with CAVLC. */
	total_coeff = 0;
	error = h264sw_cavlc_residual_block(&ctx->bits, nc, 0U, max_coeff - 1U, max_coeff, levels, &total_coeff);
	if (error != 0)
		return error;

	/* Records TotalCoeff where the next blocks' nC reads it; the DC blocks have no entry. */
	if (cat == H264SW_CAT_LUMA_AC || cat == H264SW_CAT_LUMA_4X4)
		mb->total_coeff[blk] = (uint8_t)total_coeff;
	if (cat == H264SW_CAT_CHROMA_AC)
		mb->total_coeff[MB_TOTAL_CHROMA_AC + 4U * (component - 1U) + blk] = (uint8_t)total_coeff;

	/* Succeeded: the levels and the count are recorded. */
	return 0;
}

/*
 * Reads the skip indication of the macroblock about to be decoded
 * (7.3.4): mb_skip_run for CAVLC when none is being counted, mb_skip_flag
 * for CABAC, nothing in an I slice.
 */
static int
mb_slice_skip(
	struct h264sw_slice_ctx *ctx,
	int *skipped)
{
	struct h264sw_picture_ctx *picture;
	uint32_t run;
	unsigned flag;

	picture = ctx->picture;
	*skipped = 0;

	/* An I slice has no skipped macroblocks. */
	if (ctx->header->slice_type == H264SW_SLICE_I)
		return 0;

	/* CABAC sends one mb_skip_flag per macroblock. */
	if (ctx->cabac_mode != 0) {
		flag = h264sw_cabac_mb_skip(ctx);
		if (ctx->cabac.error != 0)
			return EINVAL;
		*skipped = (int)flag;

		/* Succeeded: the flag. */
		return 0;
	}

	/* CAVLC reads mb_skip_run when no run is being counted and no coded macroblock is due; the run may not pass the last macroblock of the picture. */
	if (ctx->skip_run < 0) {
		run = h264sw_bits_ue(&ctx->bits);
		if (ctx->bits.error != 0)
			return EINVAL;
		if (run > picture->size_mbs - ctx->mb_addr)
			return EINVAL;
		ctx->skip_run = (int32_t)run;
	}

	/* A macroblock of the run is skipped; with the run used up, the next macroblock is a coded one. */
	if (ctx->skip_run > 0) {
		*skipped = 1;
		ctx->skip_run--;
	}

	/* Succeeded: this macroblock is skipped or coded. */
	return 0;
}

/*
 * Finds out whether the slice has another macroblock after the one just
 * decoded: more_rbsp_data() for CAVLC (after the last of a skip run, and
 * after a coded macroblock), end_of_slice_flag for CABAC.
 */
static int
mb_slice_end(
	struct h264sw_slice_ctx *ctx,
	int skipped,
	int *more_data)
{
	unsigned end;
	int more;

	/* CABAC sends end_of_slice_flag after every macroblock, skipped or not. */
	if (ctx->cabac_mode != 0) {
		end = h264sw_cabac_end_of_slice(ctx);
		if (ctx->cabac.error != 0)
			return EINVAL;
		*more_data = 1;
		if (end != 0U)
			*more_data = 0;

		/* Succeeded: the flag decided. */
		return 0;
	}

	/* CAVLC with the skip run still counting: the skipped macroblocks that follow are certain. */
	*more_data = 1;
	if (skipped != 0 && ctx->skip_run > 0)
		return 0;

	/* A coded macroblock ends the round: the next round reads a new mb_skip_run. */
	if (skipped == 0)
		ctx->skip_run = -1;

	/* Asks whether the RBSP has more data after the last skipped macroblock of a run or after a coded one. */
	more = h264sw_bits_more_rbsp_data(&ctx->bits);
	if (more == 0)
		*more_data = 0;

	/* Succeeded: more_rbsp_data() decided. */
	return 0;
}
