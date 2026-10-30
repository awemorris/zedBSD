/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The intra mode decision of the software H.264 encoder: the best
 * Intra_16x16 mode, the best Intra_4x4 or Intra_8x8 mode of each block
 * (with the block's levels, because each block is predicted from the
 * reconstruction of the blocks before it), and the best chroma mode.
 *
 * The predictions, the edges and the predicted modes are the decoder's
 * (h264sw_intra_edge_luma, h264sw_intra_edge_chroma, h264sw_intra_pred*,
 * h264sw_intra4x4_mode, h264sw_intra8x8_mode), read from the frame being
 * reconstructed; the block reconstruction is the decoder's too
 * (h264sw_scale4x4 or h264sw_scale8x8 with the picture's levelscale,
 * h264sw_idct4x4 or h264sw_idct8x8, h264sw_add_residual).
 *
 * Costs (the encoder's choice): SATD of the prediction against the source
 * plus lambda_motion times the mode's bits: for a 4x4 or 8x8 block 1 bit
 * when the mode is the predicted one, else 4; for Intra_16x16 and chroma
 * the ue(v) length of the mode; and for the whole of Intra_4x4 or
 * Intra_8x8 6 bits more for the macroblock (their mb_type and the
 * per-block flags cost more than Intra_16x16's), so that flat areas
 * prefer Intra_16x16.  A mode that needs a sample that is not available
 * is not tried.
 */

#ifndef LIBMEDIA_H264ENC_INTRA_H
#define LIBMEDIA_H264ENC_INTRA_H

#include "h264enc-mbctx.h"

/* The extra bits charged to a whole Intra_4x4 or Intra_8x8 macroblock. */
#define H264ENC_INTRA_NXN_BITS 6U

/*
 * Chooses the Intra_16x16 mode of the current macroblock (ctx->slice->
 * mb_addr, its neighbours ctx->slice->n found).  Tries the available
 * modes in the order 0..3; ties keep the lower mode.  Returns the cost
 * and *mode.
 */
uint32_t h264enc_intra_16x16(struct h264enc_mbctx *ctx, unsigned *mode);

/*
 * Chooses the Intra_4x4 mode and levels of each block of the current
 * macroblock, in luma4x4BlkIdx order.
 *
 * Before it, the caller has reset the current entry (h264sw_mb_reset with
 * the slice number) and set its kind to H264SW_MB_I_4X4.  For each block:
 * the edge, each available mode 0..8 (ties keep the lower), the residual
 * of the best, h264enc_fdct4x4 and h264enc_quant4x4 (intra, ctx->qp, the
 * levels clamped to ctx->level_limit), and the block reconstructed into
 * the frame with those levels; the entry's intra4x4_mode[blk] is set to
 * the mode before the next block (the next block's predicted mode reads
 * it).  decision->intra_mode and decision->luma4x4 receive the modes and
 * levels.  The frame's samples of this macroblock are left as the blocks
 * were reconstructed (the coding reconstructs the macroblock again in
 * full).  Returns the cost (with H264ENC_INTRA_NXN_BITS).
 */
uint32_t h264enc_intra_4x4(struct h264enc_mbctx *ctx, struct h264enc_mb_decision *decision);

/*
 * The same for Intra_8x8 (High): kind H264SW_MB_I_8X8, transform_8x8 1
 * in the entry, the 8x8 edges and predictions (with the reference sample
 * filtering of 8.3.2.2.1), h264enc_fdct8x8 and h264enc_quant8x8, the
 * mode written to the four intra4x4_mode entries of each 8x8 block.
 * decision->intra_mode[0..3] and decision->luma8x8 receive them.
 */
uint32_t h264enc_intra_8x8(struct h264enc_mbctx *ctx, struct h264enc_mb_decision *decision);

/* Chooses intra_chroma_pred_mode (0..3, available ones, ties keep the lower) by the SATD of Cb plus Cr.  Returns the cost and *mode. */
uint32_t h264enc_intra_chroma(struct h264enc_mbctx *ctx, unsigned *mode);

/*
 * Decides an intra macroblock (every macroblock of I slices, and the
 * intra candidates of P and B slices): Intra_16x16; Intra_4x4 when
 * try_nxn; Intra_8x8 when try_nxn and the preset's intra8x8 and ctx->high;
 * the least cost wins (ties: 16x16, then 4x4, then 8x8); the chroma mode
 * is chosen once.  decision is filled for the winner (kind, the modes,
 * the levels for I_4X4 and I_8X8, transform 4x4 or 8x8, cost including
 * the chroma cost).  The frame's samples of this macroblock are left
 * undefined.  Returns the cost.
 */
uint32_t h264enc_intra_decide(struct h264enc_mbctx *ctx, int try_nxn, struct h264enc_mb_decision *decision);

#endif
