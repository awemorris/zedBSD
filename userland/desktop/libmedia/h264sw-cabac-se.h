/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The syntax elements of the macroblock layer coded with CABAC in the
 * software H.264 codec: for each element its binarization (9.3.2), its
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
 *
 * The context selection (the h264sw_ctx_* functions) is shared with the
 * encoder, which codes bins with the same contexts.  These functions read
 * the state both sides keep alike: the slice context (its header, its
 * neighbours ctx->n, prev_mb_addr and prev_qp_delta_nonzero) and the
 * struct h264sw_mb entries.  They read only blocks and macroblocks that
 * precede the current element in coding order, so an encoder that has
 * filled the current macroblock's entry with the values the decoder's
 * parser will write gets the contexts the decoder will use.  Each of
 * them is pure (it changes nothing) and returns ctxIdx (ctxIdxOffset +
 * ctxIdxInc) unless its comment says ctxIdxInc.
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


/*
 * ctxIdx of mb_skip_flag (ctxIdxOffset 11 for P, 24 for B by
 * ctx->header->slice_type; ctxIdxInc 9.3.3.1.1.1 from A and B: each
 * available neighbour that is not skipped adds 1).  For a slice that is
 * neither P nor B the P value is returned (the element does not exist
 * there).
 */
unsigned h264sw_ctx_mb_skip(const struct h264sw_slice_ctx *ctx);

/*
 * ctxIdxInc of the first bin of mb_type (9.3.3.1.1.3) for the slice type:
 * for I slices condTermFlagN is 0 when N is not available or is I_NxN
 * (kind I_NXN, I_4X4 or I_8X8); for B slices, 0 when N is not available
 * or is B_Skip or B_Direct_16x16.  Returns condTermFlagA +
 * condTermFlagB, to be added to ctxIdxOffset 3 (I) or 27 (B prefix).  0
 * for any other slice type (the P prefix's first bin has a fixed
 * context, and the intra suffix of P and B slices too, Table 9-39).
 */
unsigned h264sw_ctx_mb_type_inc(const struct h264sw_slice_ctx *ctx, unsigned slice_type);

/* ctxIdx of transform_size_8x8_flag (399 + the number of available neighbours that use the 8x8 transform, 9.3.3.1.1.10). */
unsigned h264sw_ctx_transform_8x8(const struct h264sw_slice_ctx *ctx);

/*
 * ctxIdx of bin 0 of intra_chroma_pred_mode (64 + 9.3.3.1.1.8: each
 * available neighbour coded in an Intra mode other than I_PCM with a
 * chroma mode other than 0 adds 1); bins 1 and 2 use 64 + 3.
 */
unsigned h264sw_ctx_chroma_mode(const struct h264sw_slice_ctx *ctx);

/*
 * ctxIdx of bin 0 of ref_idx_lX for the partition whose upper-left luma
 * location is (x, y) in the current macroblock (54 + 9.3.3.1.1.6, the
 * rule of h264sw_cabac_ref_idx); bin 1 uses 54 + 4 and later bins 54 + 5
 * (Table 9-39).  list 0 or 1, x and y below 16.
 */
unsigned h264sw_ctx_ref_idx(const struct h264sw_slice_ctx *ctx, unsigned list, unsigned x, unsigned y);

/*
 * ctxIdx of bin 0 of the prefix of mvd_lX component comp (0 horizontal:
 * offset 40, 1 vertical: 47) for the (sub-)partition at (x, y), from
 * absMvdCompA + absMvdCompB (9.3.3.1.1.7: below 3 gives 0, 3..32 gives
 * 1, above 32 gives 2).  Bins 1..8 of the prefix use offset + 3, + 4,
 * + 5, + 6, + 6, + 6, + 6, + 6 (Table 9-39).  list and comp 0 or 1, x
 * and y below 16.
 */
unsigned h264sw_ctx_mvd(const struct h264sw_slice_ctx *ctx, unsigned list, unsigned x, unsigned y, unsigned comp);

/*
 * ctxIdx of prefix bin b8 (luma8x8BlkIdx 0..3) of coded_block_pattern (73
 * + 9.3.3.1.1.4).  luma_bits holds the bins of the current macroblock
 * already coded (bit b set for a coded 8x8 block b < b8); the
 * neighbouring macroblocks count by their cbp.
 */
unsigned h264sw_ctx_cbp_luma(const struct h264sw_slice_ctx *ctx, unsigned b8, unsigned luma_bits);

/* ctxIdx of suffix bin bin_idx (0 or 1) of coded_block_pattern (77 + 9.3.3.1.1.4 for chroma; bin 1 adds 4). */
unsigned h264sw_ctx_cbp_chroma(const struct h264sw_slice_ctx *ctx, unsigned bin_idx);

/*
 * ctxIdx of bin 0 of mb_qp_delta (60 + 9.3.3.1.1.5, from prev_mb_addr and
 * prev_qp_delta_nonzero); bin 1 uses 60 + 2, later bins 60 + 3.
 */
unsigned h264sw_ctx_qp_delta(const struct h264sw_slice_ctx *ctx);

/*
 * ctxIdx of coded_block_flag of a block (ctxIdxOffset 85 for ctxBlockCat
 * below 5, plus ctxIdxBlockCatOffset of Table 9-40, plus 9.3.3.1.1.9).
 * cat, blk and component as h264sw_cabac_residual_block; cat below 5
 * (an 8x8 block of 4:2:0 has no flag).
 */
unsigned h264sw_ctx_coded_block_flag(const struct h264sw_slice_ctx *ctx, unsigned cat, unsigned blk, unsigned component);

/*
 * ctxIdx of significant_coeff_flag (last 0) or last_significant_coeff_flag
 * (last 1) at scan position idx of a block of category cat, frame coded
 * (ctxIdxOffset 105 and 166 for cats 0..4, 402 and 417 for cat 5; Table
 * 9-40; 9.3.3.1.3: ctxIdxInc idx for cats other than 3 and 5,
 * Min( idx / NumC8x8, 2 ) for cat 3 with NumC8x8 1, Table 9-43 for cat
 * 5).  idx is below the category's maxNumCoeff - 1.
 */
unsigned h264sw_ctx_significance(unsigned cat, unsigned idx, int last);

/*
 * ctxIdx of bin bin_idx of the prefix of coeff_abs_level_minus1 of a
 * block of category cat (ctxIdxOffset 227 for cats 0..4, 426 for cat 5;
 * Table 9-40; 9.3.3.1.3 with numDecodAbsLevelEq1 eq1 and
 * numDecodAbsLevelGt1 gt1 counted over the levels of the block already
 * coded, in reverse scan order).  Every bin_idx above 0 has the same
 * context.
 */
unsigned h264sw_ctx_abs_level(unsigned cat, unsigned bin_idx, unsigned eq1, unsigned gt1);

#endif
