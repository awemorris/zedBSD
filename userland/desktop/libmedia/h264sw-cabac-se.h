/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The syntax elements of the macroblock layer decoded with CABAC in the
 * software H.264 decoder: for each element its binarization (9.3.2), its
 * ctxIdxOffset and maxBinIdxCtx (Table 9-34), the context index
 * increments from the neighbours (9.3.3.1.1), from the bins before
 * (9.3.3.1.2, Table 9-39) and for the residual (9.3.3.1.3, Tables 9-40,
 * 9-42 and 9-43), and the residual block (residual_block_cabac,
 * 7.3.5.3.3).
 *
 * Each function reads one element of the current macroblock
 * (ctx->n.current) through ctx->cabac and returns its value.  The
 * neighbours' values come from the struct h264sw_mb entries; the
 * current macroblock's entry must already hold what the element's
 * context needs from the elements read before it in the same macroblock
 * (h264sw-macroblock.c writes them as it goes).  A broken bitstream
 * leaves ctx->cabac.engine.error set or gives a value out of range; each
 * function returns EINVAL for a value it can tell is out of range and
 * the caller checks ctx->cabac.engine.error after each macroblock.
 */

#ifndef LIBMEDIA_H264SW_CABAC_SE_H
#define LIBMEDIA_H264SW_CABAC_SE_H

#include "h264sw.h"

struct h264sw_slice_ctx;

/*
 * The block categories of the residual (ctxBlockCat, Table 9-42) used for
 * 4:2:0: Intra16x16DCLevel, Intra16x16ACLevel, a luma 4x4 block, the
 * chroma DC, a chroma AC block, a luma 8x8 block.
 */
#define H264SW_CAT_LUMA_DC 0U
#define H264SW_CAT_LUMA_AC 1U
#define H264SW_CAT_LUMA_4X4 2U
#define H264SW_CAT_CHROMA_DC 3U
#define H264SW_CAT_CHROMA_AC 4U
#define H264SW_CAT_LUMA_8X8 5U

/*
 * Decodes mb_skip_flag (ctxIdxOffset 11 for P, 24 for B; ctxIdxInc by
 * 9.3.3.1.1.1: condTermFlagN 0 when mbAddrN is not available or its
 * mb_skip_flag is 1).  Returns 0 or 1.
 */
unsigned h264sw_cabac_mb_skip(struct h264sw_slice_ctx *ctx);

/*
 * Decodes mb_type (9.3.2.5, Tables 9-36 and 9-37; ctxIdxOffset 3 for I
 * slices, 14 and 17 for P prefix and suffix, 27 and 32 for B; the first
 * bin's ctxIdxInc by 9.3.3.1.1.3, the others by Table 9-39 and
 * 9.3.3.1.2).
 *
 * slice_type is the current slice's; the value returned is mb_type as
 * the tables of 7.4.5 number it (for P and B slices the intra types are
 * returned as 5 + the I value and 23 + the I value).  The bin of I_PCM
 * (bin 1 of the I binarization) is decoded with vcodec_cabac_terminate.
 * Returns 0 and *mb_type, or EINVAL for a bin string that is not in the
 * table.
 */
int h264sw_cabac_mb_type(struct h264sw_slice_ctx *ctx, unsigned slice_type, unsigned *mb_type);

/* Decodes sub_mb_type (Table 9-38; ctxIdxOffset 21 for P, 36 for B, ctxIdxInc by Table 9-39 and 9.3.3.1.2).  Returns 0 and the value, or EINVAL. */
int h264sw_cabac_sub_mb_type(struct h264sw_slice_ctx *ctx, unsigned slice_type, unsigned *sub_mb_type);

/* Decodes transform_size_8x8_flag (ctxIdxOffset 399, ctxIdxInc by 9.3.3.1.1.10 from the neighbours' flags).  Returns 0 or 1. */
unsigned h264sw_cabac_transform_8x8(struct h264sw_slice_ctx *ctx);

/*
 * Decodes prev_intra4x4_pred_mode_flag or prev_intra8x8_pred_mode_flag
 * (ctxIdx 68) and, when it is 0, rem_intra4x4_pred_mode or
 * rem_intra8x8_pred_mode (three bins with ctxIdx 69, FL, least
 * significant bin first).  *flag and *rem receive them (*rem 0 when not
 * read).
 */
void h264sw_cabac_intra_mode(struct h264sw_slice_ctx *ctx, unsigned *flag, unsigned *rem);

/* Decodes intra_chroma_pred_mode (TU cMax 3, ctxIdxOffset 64, ctxIdxInc by 9.3.3.1.1.8 and Table 9-39).  Returns 0..3. */
unsigned h264sw_cabac_chroma_mode(struct h264sw_slice_ctx *ctx);

/*
 * Decodes ref_idx_l0 or ref_idx_l1 (U binarization, ctxIdxOffset 54;
 * the first bin's ctxIdxInc by 9.3.3.1.1.6, the others by Table 9-39).
 *
 * (x, y) is the partition's upper-left luma location in the macroblock;
 * the neighbours A and B are the partitions holding ( x - 1, y ) and
 * ( x, y - 1 ).  condTermFlagN is 0 when N is not available, is P_Skip
 * or B_Skip, intra, does not use the list, is a direct-predicted quadrant
 * (struct h264sw_mb's direct), or has refIdx 0; else 1.  The value is
 * bounded by the slice's num_ref_idx_active of the list: a value at or
 * above it returns EINVAL (the loop of unary bins stops there, so a
 * broken stream cannot loop without bound).
 */
int h264sw_cabac_ref_idx(struct h264sw_slice_ctx *ctx, unsigned list, unsigned x, unsigned y, unsigned *ref_idx);

/*
 * Decodes one component of mvd_l0 or mvd_l1 (UEG3 with signedValFlag 1
 * and uCoff 9, ctxIdxOffset 40 for the horizontal and 47 for the vertical
 * component; the prefix's first bin's ctxIdxInc by 9.3.3.1.1.7 from
 * absMvdCompA + absMvdCompB, the others by Table 9-39; suffix and sign
 * bypass).
 *
 * (x, y) is the (sub-)partition's upper-left luma location; component 0
 * or 1.  absMvdCompN comes from struct h264sw_mb's mvd of the
 * neighbouring 4x4 block (0 where no mvd of that list was sent).  The
 * unary part of the Exp-Golomb suffix is bounded: more than 24 bins
 * returns EINVAL (no value in range needs as many).  The value is
 * checked against the range of 7.4.5.1 (-8192 to 8191.75 luma samples,
 * -32768..32767 in quarter samples): outside it returns EINVAL.
 */
int h264sw_cabac_mvd(struct h264sw_slice_ctx *ctx, unsigned list, unsigned x, unsigned y, unsigned component, int *mvd);

/*
 * Decodes coded_block_pattern (9.3.2.6: prefix of four FL bins, one per
 * 8x8 luma block, ctxIdxOffset 73 with ctxIdxInc by 9.3.3.1.1.4 from the
 * left and upper 8x8 blocks, which may be bins already decoded of this
 * macroblock; suffix TU cMax 2 with ctxIdxOffset 77).  Returns
 * CodedBlockPatternLuma in bits 0..3 and CodedBlockPatternChroma in bits
 * 4..5.
 */
unsigned h264sw_cabac_cbp(struct h264sw_slice_ctx *ctx);

/*
 * Decodes mb_qp_delta (9.3.2.7: the mapping of Table 9-3 over U bins,
 * ctxIdxOffset 60; the first bin's ctxIdxInc by 9.3.3.1.1.5 from the
 * previous macroblock in decoding order, ctx->prev_mb_addr and
 * ctx->prev_qp_delta_nonzero).  The U bins are bounded: a value outside
 * -26..25 (7.4.5) returns EINVAL.
 */
int h264sw_cabac_qp_delta(struct h264sw_slice_ctx *ctx, int *qp_delta);

/* Decodes end_of_slice_flag (ctxIdx 276, DecodeTerminate).  Returns 0 or 1. */
unsigned h264sw_cabac_end_of_slice(struct h264sw_slice_ctx *ctx);

/*
 * Decodes one residual block (residual_block_cabac, 7.3.5.3.3).
 *
 * cat is ctxBlockCat (H264SW_CAT_*); blk the block's index (luma4x4BlkIdx
 * for cats 1 and 2, luma8x8BlkIdx for cat 5, chroma4x4BlkIdx for cat 4,
 * 0 for cats 0 and 3); component 0 luma, 1 Cb, 2 Cr; start and end
 * startIdx and endIdx; max_coeff maxNumCoeff (16, 15, 16, 4, 15, 64).
 *
 * coded_block_flag is read for every cat except 5 (for 4:2:0 an 8x8 block
 * has none and is inferred 1, 7.4.5.3.3), with ctxIdxInc by 9.3.3.1.1.9
 * from the neighbouring blocks' flags (struct h264sw_mb's coded_flags:
 * unavailable neighbours count as 1 for an intra current macroblock and
 * 0 for an inter one; skipped ones 0; I_PCM 1; an 8x8-transform
 * neighbour's 4x4 entries carry its 8x8 block's flag).  Then the
 * significance map (significant_coeff_flag and last_significant_coeff_flag
 * with ctxIdxInc from the scan position, Table 9-43 for cat 5 frame
 * coded), the levels (coeff_abs_level_minus1 as UEG0 with uCoff 14,
 * ctxIdxInc by 9.3.3.1.3 from numDecodAbsLevelGt1 and
 * numDecodAbsLevelEq1, suffix bypass) and coeff_sign_flag (bypass), in
 * reverse scan order.
 *
 * levels receives coeffLevel[ 0..max_coeff - 1 ] in scan order (0 where
 * none); *coded receives coded_block_flag (1 for cat 5) and the caller
 * records it in the current macroblock's coded_flags.  Returns 0, or
 * EINVAL for a level outside -2^15 .. 2^15 - 1 or an Exp-Golomb suffix
 * whose unary part has more than 24 bins.
 */
int h264sw_cabac_residual_block(struct h264sw_slice_ctx *ctx, unsigned cat, unsigned blk, unsigned component, unsigned start, unsigned end, unsigned max_coeff, int32_t *levels, unsigned *coded);

#endif
