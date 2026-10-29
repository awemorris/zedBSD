/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC syntax elements of the software H.264 decoder (9.3.2, 9.3.3.1).
 */

#include "h264sw-cabac-se.h"
#include "h264sw-context.h"

#include <errno.h>
#include <stddef.h>

/* The ctxIdxOffset of each syntax element (Table 9-34) and the ctxIdx of the two elements that have one context. */
#define SE_OFFSET_MB_TYPE_I 3U
#define SE_OFFSET_SKIP_P 11U
#define SE_OFFSET_MB_TYPE_P_PREFIX 14U
#define SE_OFFSET_MB_TYPE_P_SUFFIX 17U
#define SE_OFFSET_SUB_MB_TYPE_P 21U
#define SE_OFFSET_SKIP_B 24U
#define SE_OFFSET_MB_TYPE_B_PREFIX 27U
#define SE_OFFSET_MB_TYPE_B_SUFFIX 32U
#define SE_OFFSET_SUB_MB_TYPE_B 36U
#define SE_OFFSET_MVD_X 40U
#define SE_OFFSET_MVD_Y 47U
#define SE_OFFSET_REF_IDX 54U
#define SE_OFFSET_QP_DELTA 60U
#define SE_OFFSET_CHROMA_MODE 64U
#define SE_CTX_PREV_INTRA_FLAG 68U
#define SE_CTX_REM_INTRA_MODE 69U
#define SE_OFFSET_CBP_LUMA 73U
#define SE_OFFSET_CBP_CHROMA 77U
#define SE_OFFSET_CODED_BLOCK_FLAG 85U
#define SE_OFFSET_SIGNIFICANT 105U
#define SE_OFFSET_LAST 166U
#define SE_OFFSET_ABS_LEVEL 227U
#define SE_OFFSET_TRANSFORM_8X8 399U
#define SE_OFFSET_SIGNIFICANT_8X8 402U
#define SE_OFFSET_LAST_8X8 417U
#define SE_OFFSET_ABS_LEVEL_8X8 426U

/* The most 1 bins the unary part of an Exp-Golomb suffix may have: no value of mvd or of a level needs as many. */
#define SE_EXP_GOLOMB_UNARY_MAX 24U

/* The prefix of coeff_abs_level_minus1 is TU with cMax 14 (uCoff of UEG0), and the prefix of an mvd component TU with cMax 9 (uCoff of UEG3). */
#define SE_LEVEL_UCOFF 14U
#define SE_MVD_UCOFF 9U

/* The largest mapped value of mb_qp_delta (Table 9-3) that stands for an allowed value: 52 is -26, and 51 is +26, which is above the range. */
#define SE_QP_DELTA_MAPPED_MAX 52U
#define SE_QP_DELTA_MAPPED_MAX_POSITIVE 49U

/* The range of a coefficient level that the transforms take (cij of 8.5.12.1 and 8.5.13.1 for 8 bits): the magnitude may be one larger for a negative level. */
#define SE_LEVEL_MAX 32767U
#define SE_LEVEL_MIN_ABS 32768U

/* The range of one mvd component in quarter luma samples (7.4.5.1: -8192 to 8191.75 luma samples): the magnitude may be one larger for a negative value. */
#define SE_MVD_MAX 32767U
#define SE_MVD_MIN_ABS 32768U

/* The bits of struct h264sw_mb's coded_flags that are not luma 4x4 blocks: the luma DC, the Cb DC (the Cr DC follows it), and the first Cb AC block (the other chroma AC blocks follow it). */
#define SE_FLAG_LUMA_DC 16U
#define SE_FLAG_CHROMA_DC 17U
#define SE_FLAG_CHROMA_AC 19U

/*
 * ctxIdxBlockCatOffset of coded_block_flag, significant_coeff_flag (and
 * last_significant_coeff_flag, which has the same offsets) and
 * coeff_abs_level_minus1 for ctxBlockCat 0 to 4, the blocks of 4:2:0
 * other than the 8x8 luma block (which has its own ctxIdxOffset and a
 * block category offset of 0).
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-40, ctxBlockCat 0 to 4 in columns.
 */
static const uint8_t se_cat_offset_coded_block_flag[5] = { 0, 4, 8, 12, 16 };
static const uint8_t se_cat_offset_significant[5] = { 0, 15, 29, 44, 47 };
static const uint8_t se_cat_offset_abs_level[5] = { 0, 10, 20, 30, 39 };

/*
 * ctxIdxInc of significant_coeff_flag (frame coded macroblocks) by
 * levelListIdx 0 to 62, for ctxBlockCat 5.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-43, the column of significant_coeff_flag for frame coded
 * macroblocks.
 */
static const uint8_t se_significant_inc_8x8[63] = {
	/* 0..15 */ 0, 1, 2, 3, 4, 5, 5, 4, 4, 3, 3, 4, 4, 4, 5, 5,
	/* 16..31 */ 4, 4, 4, 4, 3, 3, 6, 7, 7, 7, 8, 9, 10, 9, 8, 7,
	/* 32..47 */ 7, 6, 11, 12, 13, 11, 6, 7, 8, 9, 14, 10, 9, 8, 6, 11,
	/* 48..62 */ 12, 13, 11, 6, 9, 14, 10, 9, 11, 12, 13, 11, 14, 10, 12
};

/*
 * ctxIdxInc of last_significant_coeff_flag by levelListIdx 0 to 62, for
 * ctxBlockCat 5 (the same for frame and field coded macroblocks).
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-43, the column of last_significant_coeff_flag.
 */
static const uint8_t se_last_inc_8x8[63] = {
	/* 0..15 */ 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* 16..31 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* 32..47 */ 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
	/* 48..62 */ 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8
};

/*
 * The context of each bin of the bin string of an intra mb_type (Table
 * 9-36).  An I slice's mb_type and the suffix of a P or B slice's mb_type
 * use the same bin string with different ctxIdx (Tables 9-39 and 9-41).
 * Bin 1, the terminate bin, has no entry.
 */
struct se_intra_contexts {
	unsigned first;
	unsigned luma;
	unsigned chroma_nonzero;
	unsigned chroma_second;
	unsigned pred_high;
	unsigned pred_low;
};

/* Forward declarations of the helpers below. */
static unsigned se_mb_type_i_cond(const struct h264sw_mb *mb);
static unsigned se_mb_type_b_cond(const struct h264sw_mb *mb);
static unsigned se_ref_idx_cond(const struct h264sw_slice_ctx *ctx, unsigned list, unsigned blk, int upper);
static unsigned se_mvd_abs(const struct h264sw_slice_ctx *ctx, unsigned list, unsigned blk, int upper, unsigned component);
static unsigned se_cbp_chroma_cond(const struct h264sw_mb *mb, unsigned bin_idx);
static int se_mb_type_intra(struct h264sw_slice_ctx *ctx, const struct se_intra_contexts *contexts, unsigned *value);
static int se_mb_type_p(struct h264sw_slice_ctx *ctx, unsigned *value);
static int se_mb_type_b(struct h264sw_slice_ctx *ctx, unsigned *value);
static int se_exp_golomb_suffix(struct h264sw_slice_ctx *ctx, unsigned order, uint32_t *value);
static int se_residual_check(unsigned cat, unsigned blk, unsigned component, unsigned start, unsigned end, unsigned max_coeff);
static unsigned se_coded_block_flag_inc(const struct h264sw_slice_ctx *ctx, unsigned cat, unsigned blk, unsigned component);
static unsigned se_significance_inc(unsigned cat, unsigned level_list_idx, int last_flag);
static int se_abs_level(struct h264sw_slice_ctx *ctx, unsigned cat, unsigned eq1, unsigned gt1, uint32_t *abs_level);

/*
 * Decodes mb_skip_flag (ctxIdxOffset 11 for P, 24 for B; ctxIdxInc by 9.3.3.1.1.1: condTermFlagN 0 when mbAddrN is not available or its mb_skip_flag is 1).
 *
 * A slice that is neither a P nor a B slice has no mb_skip_flag: the
 * call sets error and returns 0.
 */
unsigned
h264sw_cabac_mb_skip(
	struct h264sw_slice_ctx *ctx)
{
	unsigned offset;
	unsigned inc;
	unsigned bin;

	/* Chooses the context range of the slice type. */
	if (ctx->header->slice_type == H264SW_SLICE_P) {
		offset = SE_OFFSET_SKIP_P;
	} else if (ctx->header->slice_type == H264SW_SLICE_B) {
		offset = SE_OFFSET_SKIP_B;
	} else {
		/* Any other slice type has no skipped macroblocks, so the caller made a mistake. */
		ctx->cabac.error = 1;
		return 0;
	}

	/* Adds one for each neighbour that is available and not skipped (9-7). */
	inc = 0;
	if (ctx->n.a != NULL) {
		if (ctx->n.a->kind != H264SW_MB_P_SKIP && ctx->n.a->kind != H264SW_MB_B_SKIP)
			inc++;
	}
	if (ctx->n.b != NULL) {
		if (ctx->n.b->kind != H264SW_MB_P_SKIP && ctx->n.b->kind != H264SW_MB_B_SKIP)
			inc++;
	}

	/* Decodes the flag, one bin of FL with cMax 1. */
	bin = h264sw_cabac_decision(&ctx->cabac, offset + inc);

	/* Reports the flag. */
	return bin;
}

/*
 * Decodes mb_type (9.3.2.5, Tables 9-36 and 9-37; ctxIdxOffset 3 for I slices, 14 and 17 for P prefix and suffix, 27 and 32 for B; the first bin's ctxIdxInc by 9.3.3.1.1.3, the others by Table 9-39 and 9.3.3.1.2).
 *
 * A slice type other than I, P and B (SP and SI are refused by the
 * decoder) returns EINVAL without reading, with *mb_type 0.
 */
int
h264sw_cabac_mb_type(
	struct h264sw_slice_ctx *ctx,
	unsigned slice_type,
	unsigned *mb_type)
{
	struct se_intra_contexts contexts;
	unsigned inc;
	unsigned value;
	int error;

	/* Clears the answer so that a refusal leaves a defined value. */
	*mb_type = 0;
	value = 0;
	error = 0;

	/* Decodes with the binarization and contexts of the slice type. */
	switch (slice_type) {
	case H264SW_SLICE_I:
		/* An I slice: Table 9-36 with ctxIdxOffset 3, whose first bin depends on the neighbours (9-9). */
		inc = 0;
		if (ctx->n.a != NULL)
			inc += se_mb_type_i_cond(ctx->n.a);
		if (ctx->n.b != NULL)
			inc += se_mb_type_i_cond(ctx->n.b);

		/* The ctxIdx of each bin of Table 9-36: bin 0 by the neighbours, bin 1 is the terminate bin, the others by Tables 9-39 and 9-41. */
		contexts.first = SE_OFFSET_MB_TYPE_I + inc;
		contexts.luma = SE_OFFSET_MB_TYPE_I + 3U;
		contexts.chroma_nonzero = SE_OFFSET_MB_TYPE_I + 4U;
		contexts.chroma_second = SE_OFFSET_MB_TYPE_I + 5U;
		contexts.pred_high = SE_OFFSET_MB_TYPE_I + 6U;
		contexts.pred_low = SE_OFFSET_MB_TYPE_I + 7U;

		/* Decodes the bin string of Table 9-36. */
		error = se_mb_type_intra(ctx, &contexts, &value);
		break;
	case H264SW_SLICE_P:
		/* A P slice: Table 9-37 for P, with an intra suffix of Table 9-36. */
		error = se_mb_type_p(ctx, &value);
		break;
	case H264SW_SLICE_B:
		/* A B slice: Table 9-37 for B, with an intra suffix of Table 9-36. */
		error = se_mb_type_b(ctx, &value);
		break;
	default:
		/* No other slice type is decoded. */
		error = EINVAL;
		break;
	}

	/* Reports why no value could be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: mb_type numbered as Tables 7-11, 7-13 and 7-14 do. */
	*mb_type = value;
	return 0;
}

/*
 * Decodes sub_mb_type (Table 9-38; ctxIdxOffset 21 for P, 36 for B, ctxIdxInc by Table 9-39 and 9.3.3.1.2).
 *
 * A slice type other than P and B returns EINVAL without reading, with
 * *sub_mb_type 0.
 */
int
h264sw_cabac_sub_mb_type(
	struct h264sw_slice_ctx *ctx,
	unsigned slice_type,
	unsigned *sub_mb_type)
{
	unsigned bin_0;
	unsigned bin_1;
	unsigned bin_2;
	unsigned bin_3;
	unsigned bin_4;
	unsigned bin_5;
	unsigned inc;

	/* Clears the answer so that a refusal leaves a defined value. */
	*sub_mb_type = 0;

	/* Decodes the P slice's sub_mb_type: bins 0 to 2 with ctxIdx 21, 22 and 23, the strings being 1, 00, 011 and 010. */
	if (slice_type == H264SW_SLICE_P) {
		bin_0 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_SUB_MB_TYPE_P);
		if (bin_0 != 0U) {
			/* The string 1 is P_L0_8x8. */
			*sub_mb_type = 0;
			return 0;
		}

		bin_1 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_SUB_MB_TYPE_P + 1U);
		if (bin_1 == 0U) {
			/* The string 00 is P_L0_8x4. */
			*sub_mb_type = 1;
			return 0;
		}

		bin_2 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_SUB_MB_TYPE_P + 2U);
		if (bin_2 != 0U) {
			/* The string 011 is P_L0_4x8. */
			*sub_mb_type = 2;
			return 0;
		}

		/* The string 010 is P_L0_4x4. */
		*sub_mb_type = 3;
		return 0;
	}

	/* Refuses a slice type that has no sub_mb_type with CABAC. */
	if (slice_type != H264SW_SLICE_B)
		return EINVAL;

	/* Decodes the B slice's sub_mb_type: bin 0 with ctxIdx 36, and 0 is B_Direct_8x8. */
	bin_0 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_SUB_MB_TYPE_B);
	if (bin_0 == 0U) {
		*sub_mb_type = 0;
		return 0;
	}

	/* Bin 1 uses ctxIdx 37; bin 2 uses 38 when bin 1 is not 0 and 39 when it is 0 (Table 9-41); the later bins use 39. */
	bin_1 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_SUB_MB_TYPE_B + 1U);
	inc = 3;
	if (bin_1 != 0U)
		inc = 2;
	bin_2 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_SUB_MB_TYPE_B + inc);

	/* The strings 100 and 101 are B_L0_8x8 and B_L1_8x8. */
	if (bin_1 == 0U) {
		*sub_mb_type = 1U + bin_2;
		return 0;
	}

	/* The strings 11000 to 11011 are the values 3 to 6: two more bins. */
	bin_3 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_SUB_MB_TYPE_B + 3U);
	if (bin_2 == 0U) {
		bin_4 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_SUB_MB_TYPE_B + 3U);
		*sub_mb_type = 3U + 2U * bin_3 + bin_4;
		return 0;
	}

	/* The strings 11110 and 11111 are the values 11 and 12. */
	if (bin_3 != 0U) {
		bin_4 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_SUB_MB_TYPE_B + 3U);
		*sub_mb_type = 11U + bin_4;
		return 0;
	}

	/* The strings 111000 to 111011 are the values 7 to 10: two more bins. */
	bin_4 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_SUB_MB_TYPE_B + 3U);
	bin_5 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_SUB_MB_TYPE_B + 3U);

	/* Succeeded: sub_mb_type 7 to 10. */
	*sub_mb_type = 7U + 2U * bin_4 + bin_5;
	return 0;
}

/* Decodes transform_size_8x8_flag (ctxIdxOffset 399, ctxIdxInc by 9.3.3.1.1.10 from the neighbours' flags).  Returns 0 or 1. */
unsigned
h264sw_cabac_transform_8x8(
	struct h264sw_slice_ctx *ctx)
{
	unsigned inc;
	unsigned bin;

	/* Adds one for each neighbour that is available and uses the 8x8 transform (9-20). */
	inc = 0;
	if (ctx->n.a != NULL) {
		if (ctx->n.a->transform_8x8 != 0U)
			inc++;
	}
	if (ctx->n.b != NULL) {
		if (ctx->n.b->transform_8x8 != 0U)
			inc++;
	}

	/* Decodes the flag, one bin of FL with cMax 1. */
	bin = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_TRANSFORM_8X8 + inc);

	/* Reports the flag. */
	return bin;
}

/*
 * Decodes prev_intra4x4_pred_mode_flag or prev_intra8x8_pred_mode_flag (ctxIdx 68) and, when it is 0, rem_intra4x4_pred_mode or rem_intra8x8_pred_mode (three bins with ctxIdx 69, FL, least significant bin first).
 *
 * *flag and *rem receive the flag and the remaining mode (0 when it is
 * not read).
 */
void
h264sw_cabac_intra_mode(
	struct h264sw_slice_ctx *ctx,
	unsigned *flag,
	unsigned *rem)
{
	unsigned bin_idx;
	unsigned bin;
	unsigned value;

	/* Decodes the flag, which says that the predicted mode is used. */
	*flag = h264sw_cabac_decision(&ctx->cabac, SE_CTX_PREV_INTRA_FLAG);
	*rem = 0;
	if (*flag != 0U)
		return;

	/* Decodes the three bins of FL with cMax 7, the least significant bin first, each with ctxIdx 69. */
	value = 0;
	for (bin_idx = 0; bin_idx < 3U; bin_idx++) {
		bin = h264sw_cabac_decision(&ctx->cabac, SE_CTX_REM_INTRA_MODE);
		value |= bin << bin_idx;
	}

	/* Reports the remaining mode. */
	*rem = value;
}

/* Decodes intra_chroma_pred_mode (TU cMax 3, ctxIdxOffset 64, ctxIdxInc by 9.3.3.1.1.8 and Table 9-39).  Returns 0..3. */
unsigned
h264sw_cabac_chroma_mode(
	struct h264sw_slice_ctx *ctx)
{
	unsigned inc;
	unsigned bin;

	/* Adds one for each neighbour that is available, coded in an Intra mode, not I_PCM, and has a chroma mode other than 0 (9-18). */
	inc = 0;
	if (ctx->n.a != NULL) {
		if (ctx->n.a->kind < H264SW_MB_P_SKIP &&
		    ctx->n.a->kind != H264SW_MB_I_PCM &&
		    ctx->n.a->chroma_pred_mode != 0U)
			inc++;
	}
	if (ctx->n.b != NULL) {
		if (ctx->n.b->kind < H264SW_MB_P_SKIP &&
		    ctx->n.b->kind != H264SW_MB_I_PCM &&
		    ctx->n.b->chroma_pred_mode != 0U)
			inc++;
	}

	/* Bin 0 with the neighbours' context: 0 is the value 0. */
	bin = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_CHROMA_MODE + inc);
	if (bin == 0U)
		return 0;

	/* Bin 1 with ctxIdx 67: 0 is the value 1. */
	bin = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_CHROMA_MODE + 3U);
	if (bin == 0U)
		return 1;

	/* Bin 2 with ctxIdx 67: 0 is the value 2, and 1 the value 3, which has no terminating 0 (TU with cMax 3). */
	bin = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_CHROMA_MODE + 3U);

	/* Succeeded: the value 2 or 3. */
	return 2U + bin;
}

/*
 * Decodes ref_idx_l0 or ref_idx_l1 (U binarization, ctxIdxOffset 54; the first bin's ctxIdxInc by 9.3.3.1.1.6, the others by Table 9-39).
 *
 * (x, y) is the partition's upper-left luma location in the macroblock;
 * the neighbours A and B are the partitions holding ( x - 1, y ) and
 * ( x, y - 1 ).  A list above 1, a location outside the macroblock and
 * a slice with no reference index in the list return EINVAL without
 * reading, with *ref_idx 0.
 */
int
h264sw_cabac_ref_idx(
	struct h264sw_slice_ctx *ctx,
	unsigned list,
	unsigned x,
	unsigned y,
	unsigned *ref_idx)
{
	unsigned blk;
	unsigned active;
	unsigned inc;
	unsigned bin;
	unsigned count;
	unsigned bin_ctx;

	/* Clears the answer so that a refusal leaves a defined value. */
	*ref_idx = 0;

	/* Refuses arguments the caller must not give. */
	if (list > 1U)
		return EINVAL;
	if (x >= 16U || y >= 16U)
		return EINVAL;

	/* Takes the number of reference indices the slice allows in the list, which bounds the value. */
	active = ctx->header->num_ref_idx_active[list];
	if (active == 0U)
		return EINVAL;

	/* Finds the 4x4 block at the partition's upper-left corner, whose neighbours are the partition's. */
	blk = h264sw_blk4x4_at(x, y);

	/* Forms the first bin's ctxIdxInc from the left and upper partitions (9-14). */
	inc = se_ref_idx_cond(ctx, list, blk, 0);
	inc += 2U * se_ref_idx_cond(ctx, list, blk, 1);

	/* Decodes the unary bins: ctxIdx 54 + inc for bin 0, 58 for bin 1 and 59 for the rest; each 1 is one more, and the first 0 ends the value. */
	count = 0;
	bin_ctx = SE_OFFSET_REF_IDX + inc;
	bin = h264sw_cabac_decision(&ctx->cabac, bin_ctx);
	while (bin != 0U) {
		count++;

		/* A value at or above the number of indices cannot be valid: stops reading. */
		if (count >= active)
			return EINVAL;

		bin_ctx = SE_OFFSET_REF_IDX + 5U;
		if (count == 1U)
			bin_ctx = SE_OFFSET_REF_IDX + 4U;
		bin = h264sw_cabac_decision(&ctx->cabac, bin_ctx);
	}

	/* Succeeded: the reference index. */
	*ref_idx = count;
	return 0;
}

/*
 * Decodes one component of mvd_l0 or mvd_l1 (UEG3 with signedValFlag 1 and uCoff 9, ctxIdxOffset 40 for the horizontal and 47 for the vertical component; the prefix's first bin's ctxIdxInc by 9.3.3.1.1.7 from absMvdCompA + absMvdCompB, the others by Table 9-39; suffix and sign bypass).
 *
 * (x, y) is the (sub-)partition's upper-left luma location; component 0
 * or 1.  A list or component above 1 and a location outside the
 * macroblock return EINVAL without reading, with *mvd 0.
 */
int
h264sw_cabac_mvd(
	struct h264sw_slice_ctx *ctx,
	unsigned list,
	unsigned x,
	unsigned y,
	unsigned component,
	int *mvd)
{
	unsigned blk;
	unsigned offset;
	unsigned sum;
	unsigned inc;
	unsigned bin;
	unsigned count;
	unsigned bin_inc;
	uint32_t suffix;
	uint32_t magnitude;
	int error;

	/* Clears the answer so that a refusal leaves a defined value. */
	*mvd = 0;

	/* Refuses arguments the caller must not give. */
	if (list > 1U || component > 1U)
		return EINVAL;
	if (x >= 16U || y >= 16U)
		return EINVAL;

	/* The horizontal component has ctxIdxOffset 40, the vertical one 47. */
	offset = SE_OFFSET_MVD_X;
	if (component == 1U)
		offset = SE_OFFSET_MVD_Y;

	/* Finds the 4x4 block at the partition's upper-left corner, whose neighbours are the partition's. */
	blk = h264sw_blk4x4_at(x, y);

	/* Sums the neighbours' absolute mvd components (absMvdCompA + absMvdCompB), each at most 255. */
	sum = se_mvd_abs(ctx, list, blk, 0, component);
	sum += se_mvd_abs(ctx, list, blk, 1, component);

	/* Forms the first bin's ctxIdxInc: a sum above 32 is 2, above 2 is 1, else 0. */
	if (sum > 32U) {
		inc = 2;
	} else if (sum > 2U) {
		inc = 1;
	} else {
		inc = 0;
	}

	/* Decodes bin 0 of the prefix: 0 is the value 0. */
	bin = h264sw_cabac_decision(&ctx->cabac, offset + inc);
	if (bin == 0U)
		return 0;

	/* Decodes the rest of the prefix, TU with cMax 9: bins 1 to 4 use the increments 3 to 6 and the later bins 6. */
	count = 1;
	while (count < SE_MVD_UCOFF) {
		bin_inc = 6;
		if (count < 4U)
			bin_inc = count + 2U;
		bin = h264sw_cabac_decision(&ctx->cabac, offset + bin_inc);
		if (bin == 0U)
			break;
		count++;
	}

	/* Reads the Exp-Golomb suffix of order 3 when the prefix was all ones; a shorter prefix is the whole magnitude. */
	magnitude = count;
	if (count == SE_MVD_UCOFF) {
		error = se_exp_golomb_suffix(ctx, 3U, &suffix);
		if (error != 0)
			return error;
		magnitude = SE_MVD_UCOFF + suffix;
	}

	/* Reads the sign, a bypass bin that is 1 for a negative value. */
	bin = h264sw_cabac_bypass(&ctx->cabac);

	/* Refuses a value outside the range of 7.4.5.1, which is one larger in magnitude for a negative value. */
	if (bin == 0U) {
		if (magnitude > SE_MVD_MAX)
			return EINVAL;
		*mvd = (int)magnitude;
	} else {
		if (magnitude > SE_MVD_MIN_ABS)
			return EINVAL;
		*mvd = -(int)magnitude;
	}

	/* Succeeded: the signed component. */
	return 0;
}

/*
 * Decodes coded_block_pattern (9.3.2.6: prefix of four FL bins, one per 8x8 luma block, ctxIdxOffset 73 with ctxIdxInc by 9.3.3.1.1.4 from the left and upper 8x8 blocks, which may be bins already decoded of this macroblock; suffix TU cMax 2 with ctxIdxOffset 77).
 *
 * Returns CodedBlockPatternLuma in bits 0..3 and CodedBlockPatternChroma
 * in bits 4..5.
 */
unsigned
h264sw_cabac_cbp(
	struct h264sw_slice_ctx *ctx)
{
	const struct h264sw_mb *mb_n;
	unsigned luma;
	unsigned chroma;
	unsigned blk;
	unsigned blk_n;
	unsigned side;
	unsigned cond[2];
	unsigned inc;
	unsigned bin;

	/* Decodes the four luma bins, bin k being bit k of CodedBlockPatternLuma. */
	luma = 0;
	for (blk = 0; blk < 4U; blk++) {
		/* Finds condTermFlagA (side 0) and condTermFlagB (side 1) from the left and upper 8x8 blocks (9.3.3.1.1.4). */
		for (side = 0; side < 2U; side++) {
			mb_n = h264sw_mb_neighbour_8x8(&ctx->n, blk, (int)side, &blk_n);

			/* A block of the current macroblock counts by the bin decoded for it; of another macroblock by its pattern, where I_PCM and a coded block do not count and a skipped macroblock does. */
			if (mb_n == NULL) {
				cond[side] = 0;
			} else if (mb_n == ctx->n.current) {
				cond[side] = 1;
				if (((luma >> blk_n) & 1U) != 0U)
					cond[side] = 0;
			} else if (mb_n->kind == H264SW_MB_I_PCM) {
				cond[side] = 0;
			} else if (mb_n->kind == H264SW_MB_P_SKIP || mb_n->kind == H264SW_MB_B_SKIP) {
				cond[side] = 1;
			} else if ((((unsigned)mb_n->cbp >> blk_n) & 1U) != 0U) {
				cond[side] = 0;
			} else {
				cond[side] = 1;
			}
		}

		/* Decodes the bin with ctxIdx 73 + condTermFlagA + 2 * condTermFlagB (9-10). */
		inc = cond[0] + 2U * cond[1];
		bin = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_CBP_LUMA + inc);
		luma |= bin << blk;
	}

	/* Decodes bin 0 of the chroma suffix, whose contexts come from the neighbouring macroblocks (9-11): 0 is CodedBlockPatternChroma 0. */
	inc = 0;
	if (ctx->n.a != NULL)
		inc += se_cbp_chroma_cond(ctx->n.a, 0);
	if (ctx->n.b != NULL)
		inc += 2U * se_cbp_chroma_cond(ctx->n.b, 0);
	bin = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_CBP_CHROMA + inc);
	if (bin == 0U)
		return luma;

	/* Decodes bin 1, with 4 added to the increment: 0 is 1 and 1 is 2 (TU with cMax 2). */
	inc = 4;
	if (ctx->n.a != NULL)
		inc += se_cbp_chroma_cond(ctx->n.a, 1);
	if (ctx->n.b != NULL)
		inc += 2U * se_cbp_chroma_cond(ctx->n.b, 1);
	bin = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_CBP_CHROMA + inc);
	chroma = 1U + bin;

	/* Succeeded: the luma bits and the chroma value in bits 4 and 5. */
	return luma | (chroma << 4);
}

/*
 * Decodes mb_qp_delta (9.3.2.7: the mapping of Table 9-3 over U bins, ctxIdxOffset 60; the first bin's ctxIdxInc by 9.3.3.1.1.5 from the previous macroblock in decoding order, ctx->prev_mb_addr and ctx->prev_qp_delta_nonzero).
 *
 * The U bins are bounded: a value outside -26..25 (7.4.5) returns
 * EINVAL, with *qp_delta 0.
 */
int
h264sw_cabac_qp_delta(
	struct h264sw_slice_ctx *ctx,
	int *qp_delta)
{
	const struct h264sw_mb *prev;
	unsigned inc;
	unsigned bin;
	unsigned bin_ctx;
	unsigned mapped;

	/* Clears the answer so that a refusal leaves a defined value. */
	*qp_delta = 0;

	/* Forms the first bin's ctxIdxInc from the macroblock before this one in decoding order (9.3.3.1.1.5); a first macroblock of the slice has none. */
	inc = 0;
	if (ctx->prev_mb_addr >= 0 && (uint32_t)ctx->prev_mb_addr < ctx->picture->size_mbs) {
		prev = &ctx->picture->mbs[ctx->prev_mb_addr];

		/* It counts only when that macroblock had an mb_qp_delta other than 0: not a skipped one, not I_PCM, and not one without residual that is not Intra_16x16. */
		inc = 1;
		if (prev->kind == H264SW_MB_P_SKIP || prev->kind == H264SW_MB_B_SKIP) {
			inc = 0;
		} else if (prev->kind == H264SW_MB_I_PCM) {
			inc = 0;
		} else if (prev->kind != H264SW_MB_I_16X16 && prev->cbp == 0U) {
			inc = 0;
		} else if (ctx->prev_qp_delta_nonzero == 0) {
			inc = 0;
		}
	}

	/* Decodes the unary bins of the mapped value: ctxIdx 60 + inc for bin 0, 62 for bin 1 and 63 for the rest; the first 0 ends the value. */
	mapped = 0;
	bin_ctx = SE_OFFSET_QP_DELTA + inc;
	bin = h264sw_cabac_decision(&ctx->cabac, bin_ctx);
	while (bin != 0U) {
		mapped++;

		/* No allowed value has a mapped value above 52: stops reading. */
		if (mapped > SE_QP_DELTA_MAPPED_MAX)
			return EINVAL;

		bin_ctx = SE_OFFSET_QP_DELTA + 3U;
		if (mapped == 1U)
			bin_ctx = SE_OFFSET_QP_DELTA + 2U;
		bin = h264sw_cabac_decision(&ctx->cabac, bin_ctx);
	}

	/* Maps back by Table 9-3: an odd number k is the positive value (k + 1) / 2 and an even one the negative value -k / 2. */
	if ((mapped % 2U) == 1U) {
		/* The mapped value 51 is +26, which is above the allowed range. */
		if (mapped > SE_QP_DELTA_MAPPED_MAX_POSITIVE)
			return EINVAL;
		*qp_delta = (int)((mapped + 1U) / 2U);
	} else {
		*qp_delta = -(int)(mapped / 2U);
	}

	/* Succeeded: the value within -26..25. */
	return 0;
}

/* Decodes end_of_slice_flag (ctxIdx 276, DecodeTerminate).  Returns 0 or 1. */
unsigned
h264sw_cabac_end_of_slice(
	struct h264sw_slice_ctx *ctx)
{
	unsigned bin;

	/* Decodes the bin with the termination procedure. */
	bin = h264sw_cabac_terminate(&ctx->cabac);

	/* Reports the flag. */
	return bin;
}

/*
 * Decodes one residual block (residual_block_cabac, 7.3.5.3.3).
 *
 * cat is ctxBlockCat (H264SW_CAT_*); blk the block's index (luma4x4BlkIdx
 * for cats 1 and 2, luma8x8BlkIdx for cat 5, chroma4x4BlkIdx for cat 4,
 * 0 for cats 0 and 3); component 0 luma, 1 Cb, 2 Cr; start and end
 * startIdx and endIdx; max_coeff maxNumCoeff (16, 15, 16, 4, 15, 64).
 *
 * A ctxBlockCat above 5, a block index or component the category does
 * not have (the Cb and Cr of cats 3 and 4 are components 1 and 2, the
 * other cats are component 0), a max_coeff that is not the category's,
 * and start above end or end at max_coeff or more return EINVAL without
 * reading and without touching levels.  A level out of range returns
 * EINVAL with levels holding what was read until it, the rest zero.
 */
int
h264sw_cabac_residual_block(
	struct h264sw_slice_ctx *ctx,
	unsigned cat,
	unsigned blk,
	unsigned component,
	unsigned start,
	unsigned end,
	unsigned max_coeff,
	int32_t *levels,
	unsigned *coded)
{
	unsigned positions[64];
	unsigned count;
	unsigned index;
	unsigned position;
	unsigned num_coeff;
	unsigned inc;
	unsigned sig_offset;
	unsigned last_offset;
	unsigned flag;
	unsigned last_seen;
	unsigned eq1;
	unsigned gt1;
	unsigned sign;
	uint32_t abs_level;
	int error;

	/* Refuses arguments that do not describe a block of 4:2:0. */
	*coded = 0;
	error = se_residual_check(cat, blk, component, start, end, max_coeff);
	if (error != 0)
		return error;

	/* Zeroes every level, so that the positions with no coefficient are 0. */
	for (index = 0; index < max_coeff; index++)
		levels[index] = 0;

	/* Reads coded_block_flag unless the block is an 8x8 luma block, whose flag is inferred to be 1 (7.4.5.3.3). */
	flag = 1;
	if (cat != H264SW_CAT_LUMA_8X8) {
		inc = se_coded_block_flag_inc(ctx, cat, blk, component);
		flag = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_CODED_BLOCK_FLAG + se_cat_offset_coded_block_flag[cat] + inc);
	}

	/* A block with no coefficient has all levels 0. */
	if (flag == 0U)
		return 0;
	*coded = 1;

	/* Takes the contexts of the significance map, which are by ctxBlockCat. */
	if (cat == H264SW_CAT_LUMA_8X8) {
		sig_offset = SE_OFFSET_SIGNIFICANT_8X8;
		last_offset = SE_OFFSET_LAST_8X8;
	} else {
		sig_offset = SE_OFFSET_SIGNIFICANT + se_cat_offset_significant[cat];
		last_offset = SE_OFFSET_LAST + se_cat_offset_significant[cat];
	}

	/* Reads the significance map: for each position before the last one a significant_coeff_flag and, for a significant one, a last_significant_coeff_flag that ends the map. */
	count = 0;
	last_seen = 0;
	num_coeff = end + 1U;
	index = start;
	while (index + 1U < num_coeff) {
		inc = se_significance_inc(cat, index, 0);
		flag = h264sw_cabac_decision(&ctx->cabac, sig_offset + inc);

		/* A significant position is noted, and its last flag says whether it is the final one. */
		if (flag != 0U) {
			positions[count] = index;
			count++;

			inc = se_significance_inc(cat, index, 1);
			flag = h264sw_cabac_decision(&ctx->cabac, last_offset + inc);
			if (flag != 0U) {
				last_seen = 1;
				num_coeff = index + 1U;
			}
		}

		index++;
	}

	/* The position before numCoeff, which no last flag ended, is significant without a flag (7.3.5.3.3). */
	if (last_seen == 0U) {
		positions[count] = num_coeff - 1U;
		count++;
	}

	/* Reads the levels in reverse scan order: each is coeff_abs_level_minus1 and then coeff_sign_flag. */
	eq1 = 0;
	gt1 = 0;
	for (index = count; index > 0U; index--) {
		position = positions[index - 1U];

		/* Reads the absolute value of the level, which is coeff_abs_level_minus1 + 1. */
		error = se_abs_level(ctx, cat, eq1, gt1, &abs_level);
		if (error != 0)
			return error;

		/* Counts the level for the contexts of the levels after it. */
		if (abs_level == 1U) {
			eq1++;
		} else {
			gt1++;
		}

		/* Reads the sign, a bypass bin that is 1 for a negative level. */
		sign = h264sw_cabac_bypass(&ctx->cabac);

		/* Refuses a level outside -2^15 .. 2^15 - 1, which is one larger in magnitude for a negative level. */
		if (sign == 0U) {
			if (abs_level > SE_LEVEL_MAX)
				return EINVAL;
			levels[position] = (int32_t)abs_level;
		} else {
			if (abs_level > SE_LEVEL_MIN_ABS)
				return EINVAL;
			levels[position] = -(int32_t)abs_level;
		}
	}

	/* Succeeded: the block's levels in scan order. */
	return 0;
}

/* Gives condTermFlagN of mb_type in an I slice for an available neighbour: 0 when it is I_NxN (9.3.3.1.1.3). */
static unsigned
se_mb_type_i_cond(
	const struct h264sw_mb *mb)
{
	/* I_NxN, in any of the forms the decoder keeps it in. */
	if (mb->kind == H264SW_MB_I_NXN)
		return 0;
	if (mb->kind == H264SW_MB_I_4X4)
		return 0;
	if (mb->kind == H264SW_MB_I_8X8)
		return 0;

	/* Any other type counts. */
	return 1;
}

/* Gives condTermFlagN of mb_type in a B slice for an available neighbour: 0 when it is B_Skip or B_Direct_16x16 (9.3.3.1.1.3). */
static unsigned
se_mb_type_b_cond(
	const struct h264sw_mb *mb)
{
	/* B_Skip. */
	if (mb->kind == H264SW_MB_B_SKIP)
		return 0;

	/* B_Direct_16x16. */
	if (mb->kind == H264SW_MB_B_DIRECT)
		return 0;

	/* Any other type counts. */
	return 1;
}

/*
 * Gives condTermFlagN of ref_idx for the partition whose upper-left 4x4 block is blk (9.3.3.1.1.6).
 *
 * upper is 0 for the left partition (A) and 1 for the upper one (B).
 */
static unsigned
se_ref_idx_cond(
	const struct h264sw_slice_ctx *ctx,
	unsigned list,
	unsigned blk,
	int upper)
{
	const struct h264sw_mb *mb_n;
	unsigned blk_n;
	unsigned quadrant;

	/* Finds the macroblock and the 4x4 block next to the partition. */
	mb_n = h264sw_mb_neighbour_4x4(&ctx->n, blk, upper, &blk_n);

	/* A neighbour that is not available does not count. */
	if (mb_n == NULL)
		return 0;

	/* A skipped macroblock has no ref_idx. */
	if (mb_n->kind == H264SW_MB_P_SKIP || mb_n->kind == H264SW_MB_B_SKIP)
		return 0;

	/* An intra macroblock has no ref_idx. */
	if (mb_n->kind < H264SW_MB_P_SKIP)
		return 0;

	/* B_Direct_16x16 predicts every quadrant in direct mode. */
	if (mb_n->kind == H264SW_MB_B_DIRECT)
		return 0;

	/* A quadrant predicted in direct mode (a B_Direct_8x8 sub-macroblock) has no ref_idx of its own. */
	quadrant = blk_n >> 2;
	if ((((unsigned)mb_n->direct >> quadrant) & 1U) != 0U)
		return 0;

	/* A list the partition does not use has a reference index of -1, and 0 is the value that does not count. */
	if (mb_n->motion.ref_idx[list][quadrant] <= 0)
		return 0;

	/* Succeeded: the neighbouring partition uses the list with a reference index above 0. */
	return 1;
}

/*
 * Gives absMvdCompN of mvd for the (sub-)partition whose upper-left 4x4 block is blk (9.3.3.1.1.7).
 *
 * upper is 0 for the left neighbour (A) and 1 for the upper one (B).
 */
static unsigned
se_mvd_abs(
	const struct h264sw_slice_ctx *ctx,
	unsigned list,
	unsigned blk,
	int upper,
	unsigned component)
{
	const struct h264sw_mb *mb_n;
	unsigned blk_n;
	unsigned quadrant;

	/* Finds the macroblock and the 4x4 block next to the partition. */
	mb_n = h264sw_mb_neighbour_4x4(&ctx->n, blk, upper, &blk_n);

	/* A neighbour that is not available counts as 0. */
	if (mb_n == NULL)
		return 0;

	/* A skipped macroblock sent no mvd. */
	if (mb_n->kind == H264SW_MB_P_SKIP || mb_n->kind == H264SW_MB_B_SKIP)
		return 0;

	/* An intra macroblock sent no mvd. */
	if (mb_n->kind < H264SW_MB_P_SKIP)
		return 0;

	/* A direct-predicted block sent no mvd. */
	if (mb_n->kind == H264SW_MB_B_DIRECT)
		return 0;
	quadrant = blk_n >> 2;
	if ((((unsigned)mb_n->direct >> quadrant) & 1U) != 0U)
		return 0;

	/* Succeeded: the absolute mvd of the neighbouring 4x4 block, which is 0 where the list was not used. */
	return mb_n->mvd[list][blk_n][component];
}

/* Gives condTermFlagN of the chroma part of coded_block_pattern for an available neighbour, for bin 0 or bin 1 (9.3.3.1.1.4). */
static unsigned
se_cbp_chroma_cond(
	const struct h264sw_mb *mb,
	unsigned bin_idx)
{
	unsigned chroma;

	/* An I_PCM macroblock counts as coded. */
	if (mb->kind == H264SW_MB_I_PCM)
		return 1;

	/* A skipped macroblock has no residual. */
	if (mb->kind == H264SW_MB_P_SKIP || mb->kind == H264SW_MB_B_SKIP)
		return 0;

	/* Bin 0 asks whether the neighbour has any chroma residual, bin 1 whether it has chroma AC. */
	chroma = ((unsigned)mb->cbp >> 4) & 3U;
	if (bin_idx == 0U) {
		if (chroma == 0U)
			return 0;
	} else {
		if (chroma != 2U)
			return 0;
	}

	/* Succeeded: the neighbour's chroma pattern is set as the bin asks. */
	return 1;
}

/*
 * Decodes the bin string of Table 9-36 with the given contexts and gives the mb_type of an I slice (0 to 25).
 *
 * Bin 0 is 0 for I_NxN; bin 1 is the terminate bin, 1 for I_PCM; bin 2
 * says whether the luma AC is coded (CodedBlockPatternLuma 15); bin 3
 * whether chroma is coded, and then bin 4 whether it is 1 or 2; the last
 * two bins are Intra16x16PredMode, most significant first.
 */
static int
se_mb_type_intra(
	struct h264sw_slice_ctx *ctx,
	const struct se_intra_contexts *contexts,
	unsigned *value)
{
	unsigned bin;
	unsigned luma;
	unsigned chroma;
	unsigned pred;

	/* Bin 0: 0 is I_NxN. */
	bin = h264sw_cabac_decision(&ctx->cabac, contexts->first);
	if (bin == 0U) {
		*value = 0;
		return 0;
	}

	/* Bin 1, the terminate bin: 1 is I_PCM. */
	bin = h264sw_cabac_terminate(&ctx->cabac);
	if (bin != 0U) {
		*value = 25;
		return 0;
	}

	/* Bin 2: whether the luma blocks have AC coefficients (CodedBlockPatternLuma 15). */
	luma = h264sw_cabac_decision(&ctx->cabac, contexts->luma);

	/* Bin 3: 0 is no chroma residual. */
	chroma = h264sw_cabac_decision(&ctx->cabac, contexts->chroma_nonzero);

	/* A chroma residual is 1 (DC only) or 2 (DC and AC) by one more bin. */
	if (chroma != 0U) {
		bin = h264sw_cabac_decision(&ctx->cabac, contexts->chroma_second);
		chroma = 1U + bin;
	}

	/* The two bins of Intra16x16PredMode, most significant first. */
	pred = h264sw_cabac_decision(&ctx->cabac, contexts->pred_high);
	pred = 2U * pred;
	bin = h264sw_cabac_decision(&ctx->cabac, contexts->pred_low);
	pred += bin;

	/* Succeeded: mb_type is 1 + Intra16x16PredMode + 4 * CodedBlockPatternChroma + 12 * (luma 15 ? 1 : 0) (Table 7-11). */
	*value = 1U + pred + 4U * chroma + 12U * luma;
	return 0;
}

/* Decodes the mb_type of a P slice (Table 9-37): the four inter types and, after a prefix of 1, an intra type from Table 9-36 shifted by 5. */
static int
se_mb_type_p(
	struct h264sw_slice_ctx *ctx,
	unsigned *value)
{
	struct se_intra_contexts contexts;
	unsigned bin_0;
	unsigned bin_1;
	unsigned bin_2;
	unsigned inc;
	unsigned intra;
	int error;

	/* Bin 0 of the prefix, with ctxIdx 14: 1 is an intra type. */
	bin_0 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_MB_TYPE_P_PREFIX);
	if (bin_0 != 0U) {
		/* The suffix is Table 9-36 with ctxIdxOffset 17: bin 0 with ctxIdx 17, the terminate bin, then 18, 19, 19 or 20, and 20. */
		contexts.first = SE_OFFSET_MB_TYPE_P_SUFFIX;
		contexts.luma = SE_OFFSET_MB_TYPE_P_SUFFIX + 1U;
		contexts.chroma_nonzero = SE_OFFSET_MB_TYPE_P_SUFFIX + 2U;
		contexts.chroma_second = SE_OFFSET_MB_TYPE_P_SUFFIX + 2U;
		contexts.pred_high = SE_OFFSET_MB_TYPE_P_SUFFIX + 3U;
		contexts.pred_low = SE_OFFSET_MB_TYPE_P_SUFFIX + 3U;

		/* Decodes the suffix, then numbers it as an mb_type of a P slice. */
		error = se_mb_type_intra(ctx, &contexts, &intra);
		if (error != 0)
			return error;
		*value = 5U + intra;
		return 0;
	}

	/* Bin 1 with ctxIdx 15, and bin 2 with ctxIdx 16 when bin 1 is not 1 and 17 when it is (Table 9-41). */
	bin_1 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_MB_TYPE_P_PREFIX + 1U);
	inc = 3;
	if (bin_1 != 1U)
		inc = 2;
	bin_2 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_MB_TYPE_P_PREFIX + inc);

	/* The strings 000, 011, 010 and 001 are P_L0_16x16, P_L0_L0_16x8, P_L0_L0_8x16 and P_8x8. */
	if (bin_1 == 0U) {
		*value = 0;
		if (bin_2 != 0U)
			*value = 3;
	} else {
		*value = 2;
		if (bin_2 != 0U)
			*value = 1;
	}

	/* Succeeded: mb_type 0 to 3. */
	return 0;
}

/* Decodes the mb_type of a B slice (Table 9-37): the inter types and, after the prefix 111101, an intra type from Table 9-36 shifted by 23. */
static int
se_mb_type_b(
	struct h264sw_slice_ctx *ctx,
	unsigned *value)
{
	struct se_intra_contexts contexts;
	unsigned inc;
	unsigned bin_0;
	unsigned bin_1;
	unsigned bin_2;
	unsigned bin_3;
	unsigned bin_4;
	unsigned bin_5;
	unsigned bin_6;
	unsigned intra;
	int error;

	/* Forms the first bin's ctxIdxInc from the neighbours (9-9). */
	inc = 0;
	if (ctx->n.a != NULL)
		inc += se_mb_type_b_cond(ctx->n.a);
	if (ctx->n.b != NULL)
		inc += se_mb_type_b_cond(ctx->n.b);

	/* Bin 0: 0 is B_Direct_16x16. */
	bin_0 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_MB_TYPE_B_PREFIX + inc);
	if (bin_0 == 0U) {
		*value = 0;
		return 0;
	}

	/* Bin 1 with ctxIdx 27 + 3. */
	bin_1 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_MB_TYPE_B_PREFIX + 3U);

	/* Bin 2 uses ctxIdx 27 + 4 when bin 1 is not 0 and 27 + 5 when it is 0 (Table 9-41). */
	inc = 5;
	if (bin_1 != 0U)
		inc = 4;
	bin_2 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_MB_TYPE_B_PREFIX + inc);

	/* The strings 100 and 101 are B_L0_16x16 and B_L1_16x16. */
	if (bin_1 == 0U) {
		*value = 1U + bin_2;
		return 0;
	}

	/* Bins 3 to 5 use ctxIdx 27 + 5, like every later bin; every string has at least these six bins. */
	bin_3 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_MB_TYPE_B_PREFIX + 5U);
	bin_4 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_MB_TYPE_B_PREFIX + 5U);
	bin_5 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_MB_TYPE_B_PREFIX + 5U);

	/* The strings 110xxx are the values 3 to 10. */
	if (bin_2 == 0U) {
		*value = 3U + 4U * bin_3 + 2U * bin_4 + bin_5;
		return 0;
	}

	/* The strings 1110xxx are the values 12 to 19: bin 6 is the last bit of the value. */
	if (bin_3 == 0U) {
		bin_6 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_MB_TYPE_B_PREFIX + 5U);
		*value = 12U + 4U * bin_4 + 2U * bin_5 + bin_6;
		return 0;
	}

	/* The strings 111100x are the values 20 and 21. */
	if (bin_4 == 0U && bin_5 == 0U) {
		bin_6 = h264sw_cabac_decision(&ctx->cabac, SE_OFFSET_MB_TYPE_B_PREFIX + 5U);
		*value = 20U + bin_6;
		return 0;
	}

	/* The strings 111110 and 111111 are B_L1_L0_8x16 (11) and B_8x8 (22). */
	if (bin_4 != 0U) {
		*value = 11;
		if (bin_5 != 0U)
			*value = 22;
		return 0;
	}

	/* The prefix 111101 is an intra type: the suffix is Table 9-36 with ctxIdxOffset 32. */
	contexts.first = SE_OFFSET_MB_TYPE_B_SUFFIX;
	contexts.luma = SE_OFFSET_MB_TYPE_B_SUFFIX + 1U;
	contexts.chroma_nonzero = SE_OFFSET_MB_TYPE_B_SUFFIX + 2U;
	contexts.chroma_second = SE_OFFSET_MB_TYPE_B_SUFFIX + 2U;
	contexts.pred_high = SE_OFFSET_MB_TYPE_B_SUFFIX + 3U;
	contexts.pred_low = SE_OFFSET_MB_TYPE_B_SUFFIX + 3U;

	/* Decodes the suffix, then numbers it as an mb_type of a B slice. */
	error = se_mb_type_intra(ctx, &contexts, &intra);
	if (error != 0)
		return error;
	*value = 23U + intra;

	/* Succeeded: mb_type 23 to 48. */
	return 0;
}

/*
 * Decodes the Exp-Golomb suffix of order k of a UEGk binarization with bypass bins (9-6) and gives its value.
 *
 * The unary part is bounded: more than 24 bins of 1 returns EINVAL.
 */
static int
se_exp_golomb_suffix(
	struct h264sw_slice_ctx *ctx,
	unsigned order,
	uint32_t *value)
{
	uint32_t suffix;
	unsigned ones;
	unsigned bin;

	/* Reads the unary part: each 1 adds 2^k to the value and raises k by one; the first 0 ends it. */
	suffix = 0;
	ones = 0;
	bin = h264sw_cabac_bypass(&ctx->cabac);
	while (bin != 0U) {
		ones++;

		/* No value in range needs this many bins: stops reading. */
		if (ones > SE_EXP_GOLOMB_UNARY_MAX)
			return EINVAL;

		suffix += (uint32_t)1 << order;
		order++;
		bin = h264sw_cabac_bypass(&ctx->cabac);
	}

	/* Reads the k bits that follow, most significant first. */
	while (order > 0U) {
		order--;
		bin = h264sw_cabac_bypass(&ctx->cabac);
		suffix += (uint32_t)bin << order;
	}

	/* Succeeded: the suffix value. */
	*value = suffix;
	return 0;
}

/* Checks the arguments of h264sw_cabac_residual_block against what a block of 4:2:0 has; returns EINVAL for ones that do not describe a block. */
static int
se_residual_check(
	unsigned cat,
	unsigned blk,
	unsigned component,
	unsigned start,
	unsigned end,
	unsigned max_coeff)
{
	/* The category decides the number of coefficients, the block indices and the components (Table 9-42). */
	switch (cat) {
	case H264SW_CAT_LUMA_DC:
		if (max_coeff != 16U || blk != 0U || component != 0U)
			return EINVAL;
		break;
	case H264SW_CAT_LUMA_AC:
		if (max_coeff != 15U || blk >= 16U || component != 0U)
			return EINVAL;
		break;
	case H264SW_CAT_LUMA_4X4:
		if (max_coeff != 16U || blk >= 16U || component != 0U)
			return EINVAL;
		break;
	case H264SW_CAT_CHROMA_DC:
		if (max_coeff != 4U || blk != 0U || component < 1U || component > 2U)
			return EINVAL;
		break;
	case H264SW_CAT_CHROMA_AC:
		if (max_coeff != 15U || blk >= 4U || component < 1U || component > 2U)
			return EINVAL;
		break;
	case H264SW_CAT_LUMA_8X8:
		if (max_coeff != 64U || blk >= 4U || component != 0U)
			return EINVAL;
		break;
	default:
		/* A category of 4:4:4 or one that does not exist. */
		return EINVAL;
	}

	/* The positions to read lie inside the block. */
	if (start > end || end >= max_coeff)
		return EINVAL;

	/* Succeeded: the arguments describe a block. */
	return 0;
}

/* Gives ctxIdxInc of coded_block_flag (9.3.3.1.1.9) from the coded_block_flag of the blocks to the left (A) and above (B), which struct h264sw_mb keeps in coded_flags. */
static unsigned
se_coded_block_flag_inc(
	const struct h264sw_slice_ctx *ctx,
	unsigned cat,
	unsigned blk,
	unsigned component)
{
	const struct h264sw_mb *mb_n;
	unsigned blk_n;
	unsigned bit;
	unsigned side;
	unsigned cond[2];
	unsigned intra;

	/* An unavailable neighbour counts as 1 for a current macroblock coded in an Intra mode and 0 for an Inter one. */
	intra = 0;
	if (ctx->n.current->kind < H264SW_MB_P_SKIP)
		intra = 1;

	/* Finds condTermFlagA (side 0) and condTermFlagB (side 1). */
	for (side = 0; side < 2U; side++) {
		mb_n = NULL;
		bit = 0;
		blk_n = 0;

		/* Finds the neighbouring macroblock and the bit of coded_flags of the neighbouring block of the same category. */
		switch (cat) {
		case H264SW_CAT_LUMA_DC:
			mb_n = ctx->n.a;
			if (side == 1U)
				mb_n = ctx->n.b;
			bit = SE_FLAG_LUMA_DC;
			break;
		case H264SW_CAT_LUMA_AC:
		case H264SW_CAT_LUMA_4X4:
			mb_n = h264sw_mb_neighbour_4x4(&ctx->n, blk, (int)side, &blk_n);
			bit = blk_n;
			break;
		case H264SW_CAT_CHROMA_DC:
			mb_n = ctx->n.a;
			if (side == 1U)
				mb_n = ctx->n.b;
			bit = SE_FLAG_CHROMA_DC + (component - 1U);
			break;
		default:
			/* The chroma AC blocks, whose bits are 19 to 22 for Cb and 23 to 26 for Cr. */
			mb_n = h264sw_mb_neighbour_chroma(&ctx->n, blk, (int)side, &blk_n);
			bit = SE_FLAG_CHROMA_AC + 4U * (component - 1U) + blk_n;
			break;
		}

		/* Decides the flag by the neighbour: none, I_PCM (all its blocks count as coded), a skipped macroblock (none coded), or the flag of its block. */
		if (mb_n == NULL) {
			cond[side] = intra;
		} else if (mb_n->kind == H264SW_MB_I_PCM) {
			cond[side] = 1;
		} else if (mb_n->kind == H264SW_MB_P_SKIP || mb_n->kind == H264SW_MB_B_SKIP) {
			cond[side] = 0;
		} else {
			cond[side] = (mb_n->coded_flags >> bit) & 1U;
		}
	}

	/* Succeeded: ctxIdxInc by 9-19. */
	return cond[0] + 2U * cond[1];
}

/*
 * Gives ctxIdxInc of significant_coeff_flag (last_flag 0) or last_significant_coeff_flag (last_flag 1) for the position levelListIdx of a block of category cat (9.3.3.1.3).
 *
 * levelListIdx is at most 62 for cat 5: the caller reads positions below
 * max_coeff - 1.
 */
static unsigned
se_significance_inc(
	unsigned cat,
	unsigned level_list_idx,
	int last_flag)
{
	/* An 8x8 block takes the increment from Table 9-43 (frame coded). */
	if (cat == H264SW_CAT_LUMA_8X8) {
		if (last_flag)
			return se_last_inc_8x8[level_list_idx];
		return se_significant_inc_8x8[level_list_idx];
	}

	/* The chroma DC of 4:2:0 is Min( levelListIdx / NumC8x8, 2 ) with NumC8x8 equal to 1 (9-22). */
	if (cat == H264SW_CAT_CHROMA_DC) {
		if (level_list_idx > 2U)
			return 2;
		return level_list_idx;
	}

	/* The other categories use the position itself (9-21). */
	return level_list_idx;
}

/*
 * Decodes coeff_abs_level_minus1 (UEG0, uCoff 14) and gives the absolute value of the level, which is one more.
 *
 * eq1 and gt1 are numDecodAbsLevelEq1 and numDecodAbsLevelGt1 of the
 * levels read before in the block.  The unary part of the Exp-Golomb
 * suffix is bounded: more than 24 bins returns EINVAL.
 */
static int
se_abs_level(
	struct h264sw_slice_ctx *ctx,
	unsigned cat,
	unsigned eq1,
	unsigned gt1,
	uint32_t *abs_level)
{
	unsigned offset;
	unsigned first_inc;
	unsigned later_inc;
	unsigned limit;
	unsigned bin;
	unsigned ones;
	uint32_t suffix;
	int error;

	/* Takes the first context of the category's coeff_abs_level_minus1. */
	if (cat == H264SW_CAT_LUMA_8X8) {
		offset = SE_OFFSET_ABS_LEVEL_8X8;
	} else {
		offset = SE_OFFSET_ABS_LEVEL + se_cat_offset_abs_level[cat];
	}

	/* Forms ctxIdxInc of bin 0 (9-23): 0 once a level above 1 was read, else 1 + the number of levels equal to 1, at most 4. */
	first_inc = 0;
	if (gt1 == 0U) {
		first_inc = 1U + eq1;
		if (first_inc > 4U)
			first_inc = 4;
	}

	/* Forms ctxIdxInc of the later bins (9-24): 5 + the number of levels above 1, at most 4, or at most 3 for the chroma DC. */
	limit = 4;
	if (cat == H264SW_CAT_CHROMA_DC)
		limit = 3;
	later_inc = gt1;
	if (later_inc > limit)
		later_inc = limit;
	later_inc += 5U;

	/* Bin 0: 0 is coeff_abs_level_minus1 equal to 0, a level of absolute value 1. */
	bin = h264sw_cabac_decision(&ctx->cabac, offset + first_inc);
	if (bin == 0U) {
		*abs_level = 1;
		return 0;
	}

	/* Reads the rest of the prefix, TU with cMax 14: each 1 adds one and the first 0 ends the value. */
	ones = 1;
	while (ones < SE_LEVEL_UCOFF) {
		bin = h264sw_cabac_decision(&ctx->cabac, offset + later_inc);
		if (bin == 0U)
			break;
		ones++;
	}

	/* A prefix shorter than 14 is the whole value. */
	if (ones < SE_LEVEL_UCOFF) {
		*abs_level = ones + 1U;
		return 0;
	}

	/* A prefix of fourteen 1 bins has an Exp-Golomb suffix of order 0. */
	error = se_exp_golomb_suffix(ctx, 0U, &suffix);
	if (error != 0)
		return error;

	/* Succeeded: coeff_abs_level_minus1 is 14 + the suffix, and the level's magnitude one more. */
	*abs_level = SE_LEVEL_UCOFF + suffix + 1U;
	return 0;
}
