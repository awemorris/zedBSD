/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The coding of one macroblock in the software H.264 encoder: from a
 * decision (h264enc-mbctx.h) to the macroblock's syntax in
 * ctx->slice->syntax, its struct h264sw_mb entry as the decoder will hold
 * it, and its reconstruction in the frame, made by the decoder's
 * h264sw_recon_mb.
 *
 * The coding reconstructs twice.  First with the motion and the modes of
 * the decision and no residual (the luma levels of Intra_4x4 and
 * Intra_8x8 excepted, which the decision computed block by block), which
 * leaves the prediction in the frame; the residual source - prediction is
 * then transformed and quantized; then the entry is put back as before
 * the first pass and the macroblock reconstructed again with the levels.
 * So the samples the encoder keeps are, by construction, the decoder's.
 */

#ifndef LIBMEDIA_H264ENC_MB_H
#define LIBMEDIA_H264ENC_MB_H

#include "h264enc-mbctx.h"

/*
 * Codes the current macroblock (ctx->slice->mb_addr; ctx->slice->n found
 * by the caller with h264sw_mb_neighbours) as decision says.
 *
 * Steps:
 *  1. Mirrors the decoder's per-macroblock start (h264sw_slice_data):
 *     h264sw_mb_reset( entry, slice_num ), decoded_4x4 0,
 *     direct.spatial_ready 0.  Then at once, before anything reads the
 *     entry: kind (the decision's, I_NXN never: I_4X4 or I_8X8),
 *     transform_8x8 (1 for I_8X8; for inter kinds the decision's when it
 *     is fixed), qp (ctx->qp, the QPY the reconstruction uses), and
 *     motion.intra (1 for the intra kinds).  The predicted intra modes of
 *     step 2 read the kind of the current macroblock for the blocks inside
 *     it (a kind other than I_4X4 and I_8X8 gives DC), and the first
 *     reconstruction reads qp.
 *  2. The syntax: mb_type and its meaning (h264sw_mb_type_of, or
 *     h264sw_mb_skip_type), sub_mb_type and their meanings, ref_idx (0
 *     where not sent but used, -1 for unused lists).  Motion vector
 *     differences: partition by partition in mbPartIdx order, mvd = mv -
 *     mvp with mvp from h264sw_mvpred, writing each partition's motion
 *     into the entry and its 4x4 bits into decoded_4x4 before the next
 *     (as h264sw_recon_mb will), direct quadrants derived in their turn
 *     with h264sw_direct_quadrant and their 4x4 bits set in decoded_4x4
 *     after each (the decoder's reconstruction marks them so); then the entry's motion and
 *     decoded_4x4 are reset for step 3 (h264sw_recon_mb then derives
 *     mvp + mvd, which is the decision's vector again).  Intra
 *     modes: for each block in order, the predicted mode
 *     (h264sw_intra4x4_mode or h264sw_intra8x8_mode with prev flag 1),
 *     prev_intra_flag 1 when it equals the decision's mode, else
 *     rem_intra_mode ( mode < predicted ? mode : mode - 1 ); the entry's
 *     intra4x4_mode written before the next block.  intra_chroma_pred_mode.
 *  3. First reconstruction (h264sw_recon_mb) with cbp 0 and no levels
 *     (Intra_4x4 and Intra_8x8: the decision's luma levels and their cbp
 *     bits): the frame holds the prediction (luma and chroma; for the NxN
 *     kinds the reconstructed luma).
 *  4. The residual.  The skip kinds (P_SKIP, B_SKIP) have none: cbp 0,
 *     no levels, whatever the source (the decision took them for that).
 *     Luma (not NxN): per 4x4 block, or per 8x8 block for
 *     the 8x8 transform; Intra_16x16: the AC of each block (first 1) and
 *     the DC through h264enc_luma_dc_transform and h264enc_quant_luma_dc.
 *     Chroma: per component, the four blocks' AC (first 1) and the DC
 *     through h264enc_chroma_dc_transform and h264enc_quant_chroma_dc
 *     with QP'C (Table 8-15 by h264sw_chroma_qp with the PPS offsets).
 *     The intra rounding offset for intra kinds, the inter one otherwise.
 *     Levels clamped to ctx->level_limit.  With decision->transform
 *     H264ENC_TRANSFORM_CHOOSE (inter kinds, High), both transforms are
 *     quantized and the one with the smaller SSD( source, reconstruction
 *     ) + ( ( lambda_mode * bits ) >> 8 ) is kept, bits from
 *     h264enc_cavlc_block_bits with nC 0 for every block (an estimate for
 *     both entropy coders); ties keep 4x4.
 *  5. The coded block pattern: luma bit b when any level of 8x8 block b
 *     is non-zero; chroma 2 when any AC level is non-zero, else 1 when any
 *     DC level is, else 0.  Intra_16x16: CodedBlockPatternLuma 15 when any
 *     AC level is non-zero, else 0, and mb_type 1 + predMode + 4 *
 *     CodedBlockPatternChroma + 12 * ( luma 15 ) (Table 7-11; I slice
 *     numbering, + 5 in P and + 23 in B slices).  An inter macroblock
 *     with cbp 0: P (16x16, ref_idx
 *     0, mv equal to h264sw_mvpred_pskip's) becomes P_Skip; B_Direct_16x16
 *     becomes B_Skip.  The transform flag is cleared when cbp's luma part
 *     is 0 (it is not sent then, 7.3.5).
 *  6. mb_qp_delta: ctx->qp - ctx->slice->qp when the macroblock sends it
 *     (cbp above 0 or Intra_16x16), else 0 and QPY carried.
 *  7. The entry's parser fields, as h264sw_macroblock_parse writes them
 *     for the same syntax: slice, kind, cbp, transform_8x8, qp,
 *     chroma_pred_mode, direct, and by the entropy coding as the parser
 *     does (h264sw_macroblock_parse): with CAVLC total_coeff (TotalCoeff
 *     of each block) and coded_flags 0, with CABAC coded_flags and
 *     total_coeff 0 (I_PCM: both, total_coeff 16 and every flag); mvd
 *     (absolute, capped at 255), motion.intra, and the sent or inferred
 *     ref_idx.
 *  8. The entry put back as after step 7 and the second reconstruction
 *     with the levels (decoded_4x4 0, spatial_ready 0 again).
 *  9. ctx->slice->qp updated as the decoder would (7-37), prev_mb_addr
 *     and prev_qp_delta_nonzero left for the writer (h264enc-mbwrite.h
 *     updates them after writing, as the decoder does after parsing).
 *
 * Returns 0; EINVAL for a decision the slice cannot carry (a kind of
 * another slice type, a ref_idx beyond num_ref_idx_active, a vector
 * outside -32768..32767) or when h264sw_recon_mb fails.
 */
int h264enc_mb_code(struct h264enc_mbctx *ctx, const struct h264enc_mb_decision *decision);

/*
 * Codes the current macroblock as I_PCM: the 384 source samples (raster:
 * 256 Y, 64 Cb, 64 Cr) in ctx->slice->syntax.pcm, each 0 made 1 when
 * ctx->pcm_nonzero (A.2.1, A.2.2: pcm_sample_luma and pcm_sample_chroma
 * not 0 in those profiles; the reconstruction uses the samples sent), mb_type 25 (+ 5 in P,
 * + 23 in B slices), the entry as the parser writes it for I_PCM (qp 0,
 * cbp 0x2f, total_coeff 16, coded_flags all set, intra), and the
 * reconstruction (the samples themselves).  QPY is carried.  For a
 * macroblock whose coding would pass H264ENC_MB_MAX_BITS.  Returns 0 or
 * EINVAL.
 */
int h264enc_mb_code_pcm(struct h264enc_mbctx *ctx);

#endif
