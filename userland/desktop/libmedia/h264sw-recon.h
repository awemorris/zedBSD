/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reconstruction of one macroblock in the software H.264 decoder:
 * from its parsed syntax (ctx->syntax) to its samples in the current
 * frame (before deblocking) and its motion and modes in its struct
 * h264sw_mb entry.  It brings together the intra prediction
 * (h264sw-intra.h), the motion vectors (h264sw-mvpred.h,
 * h264sw-direct.h), the inter prediction (8.4.2: h264sw-interp.h and
 * h264sw-weight.h) and the residual (8.5: h264sw-transform.h).
 */

#ifndef LIBMEDIA_H264SW_RECON_H
#define LIBMEDIA_H264SW_RECON_H

#include "h264sw.h"

struct h264sw_slice_ctx;

/*
 * Reconstructs the current macroblock (ctx->mb_addr, ctx->n, ctx->syntax).
 *
 * The macroblock is at luma location ( 16 * n.x, 16 * n.y ) of
 * ctx->picture->frame.  By kind:
 *
 * I_PCM (8.3.5): the samples are copied; motion intra (ref_idx -1 from
 * h264sw_mb_reset), nonzero 0xffff.
 *
 * Intra (I_4X4, I_8X8, I_16X16): for I_4X4 each block in luma4x4BlkIdx
 * order: its mode (h264sw_intra4x4_mode, stored in intra4x4_mode), its
 * edge, its prediction, then its residual added (8.5.1) before the next
 * block reads it as a neighbour; I_8X8 likewise per 8x8 block (8.5.3);
 * I_16X16 predicts the whole block, then the DC (8.5.10) and the AC
 * blocks (8.5.2).  The chroma (8.3.4, 8.5.4) is predicted with
 * intra_chroma_pred_mode and its DC and AC added.  motion.intra is 1,
 * ref_idx -1, mv 0.
 *
 * Inter: the motion first, partition by partition in mbPartIdx and
 * subMbPartIdx order, each setting its 4x4 blocks' bits in
 * ctx->decoded_4x4 so later partitions see it: P_Skip by
 * h264sw_mvpred_pskip, with ref_idx[0][0..3] 0 and ref_id[0][0..3] the
 * id of RefPicList0[ 0 ]'s frame (list 1 stays unused, -1); B_Skip and B_Direct_16x16 by
 * h264sw_direct_quadrant for quadrants 0..3; a B_Direct_8x8
 * sub-macroblock by h264sw_direct_quadrant; others mvLX = mvpLX + mvdLX
 * (8-172, 8-173, with mvp from h264sw_mvpred) for each list the
 * partition uses; the sum is computed in int and wrapped to -32768..32767
 * by explicit arithmetic before it is stored in int16_t (a conforming
 * stream stays in the range of 8.4.1; a broken one must not reach an
 * implementation-defined conversion).  ref_id is the id of RefPicListX[ refIdxLX ]'s frame.
 * Then the prediction of each partition (or each 4x4 block of a direct
 * quadrant without direct_8x8_inference_flag, whose vectors may
 * differ): for each list used, h264sw_interp_luma and, with the chroma
 * vector of 8.4.1.4 (equal to the luma vector for frames, read in
 * eighth chroma samples), h264sw_interp_chroma for Cb and Cr; then
 * h264sw_weights_of and h264sw_weights_apply write the prediction into
 * the frame.  Then the residual of each coded 4x4 or 8x8 luma block and
 * of the chroma blocks is added.
 *
 * Residual: QP'Y = QPY; QP'C from qPI = Clip3( -QpBdOffsetC, 51, QPY +
 * chroma offset ) by Table 8-15 (8.5.8), with chroma_qp_index_offset for
 * Cb and second_chroma_qp_index_offset for Cr.  The scaling list is
 * chosen by the macroblock's intra or inter mode and the component
 * (8.5.9).  nonzero gets the bit of each luma 4x4 block with a non-zero
 * level, and all four bits of an 8x8 transform block with any non-zero
 * level (struct h264sw_mb's rule, 8.7.2.1).  intra4x4_mode keeps the 2 of
 * h264sw_mb_reset for every kind but I_4X4 and I_8X8.
 *
 * Errors: an intra mode that needs unavailable samples, or a reference
 * marked missing in the lists, still produces samples (128, or the
 * stand-in frame's) and adds 1 to ctx->concealed; it is not returned.
 * Returns 0, or EINVAL when the macroblock cannot be built at all (a
 * direct prediction without a co-located frame).
 */
int h264sw_recon_mb(struct h264sw_slice_ctx *ctx);

#endif
