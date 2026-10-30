/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The macroblock decision of the software H.264 encoder for I and P
 * slices, and the speed presets.  (B slices: h264enc-mode-b.h.)
 *
 * Every candidate is costed as distortion (SATD of the prediction) +
 * lambda_motion * bits (h264enc-cost.h); the least cost wins, ties going
 * to the earlier candidate in the order listed.  None of this is the
 * specification's; it is fixed here so that the result is reproducible.
 *
 * The bits of the candidates: mb_type the ue(v) length of its value (7.4.5
 * numbering of the slice type), sub_mb_type likewise for each 8x8,
 * ref_idx the te(v) length (0 when num_ref_idx_active is 1), each vector
 * h264enc_mvd_bits( mv - mvp ) with mvp the decoder's prediction for that
 * partition given the partitions before it, the intra modes as
 * h264enc-intra.h says.  The same counts are used with CABAC.
 */

#ifndef LIBMEDIA_H264ENC_MODE_H
#define LIBMEDIA_H264ENC_MODE_H

#include "h264enc.h"
#include "h264enc-mbctx.h"

/*
 * The presets, indexed by H264ENC_SPEED_FAST - 1, _BALANCED - 1,
 * _QUALITY - 1 (me.lambda and me.mv_min/mv_max are 0 here: the encoder
 * sets them per picture):
 *
 *            refs range iter. large refine subpel points parts all-refs i4x4-always i8x8 tr-choice early-skip
 *  FAST       1    16    8      0     0      2      4     0      0        0          0       0        1
 *  BALANCED   2    32   16      1     0      2      4     1      0        1          1       1        1
 *  QUALITY    4    64   32      1     1      2      8     1      1        1          1       1        0
 */
extern const struct h264enc_preset h264enc_presets[3];

/*
 * Decides a macroblock of an I slice: h264enc_intra_decide with try_nxn
 * 1.  Fills *decision; returns its cost.
 */
uint32_t h264enc_mode_i(struct h264enc_mbctx *ctx, struct h264enc_mb_decision *decision);

/*
 * Decides a macroblock of a P slice.  The current entry is reset
 * (h264sw_mb_reset with the slice number) and the neighbours found before
 * the call; the decision may write the current entry's motion and the
 * frame's samples of this macroblock (the coding starts over).
 *
 *  1. Skip: mv_skip from h264sw_mvpred_pskip; its prediction from
 *     RefPicList0[ 0 ]'s planes.  With early_skip, the residual of luma
 *     (4x4 blocks, inter offset) and chroma (DC and AC, as h264enc-mb.h
 *     quantizes them) is quantized at ctx->qp; when every level is 0 the
 *     decision is P_Skip at once (cost: the SATD).  Without early_skip,
 *     P_Skip is a candidate with cost SATD (no bits).  A P_Skip decision
 *     is coded without residual (h264enc-mb.h).  The chroma prediction of
 *     the check is h264sw_interp_chroma from the reference frame with the
 *     chroma vector equal to the luma vector (8.4.1.4, frames).
 *  2. 16x16: for each ref_idx below Min( refs, num_ref_idx_active[0] )
 *     (refs the encoder's P reference count), h264enc_me_search with mvp
 *     from h264sw_mvpred for that ref_idx and candidates: mv_skip (ref 0
 *     only), the vectors of the neighbours A, B, C (D when C is not
 *     available) whose list 0 ref_idx equals this one, and the co-located
 *     macroblock's list 0 vector in RefPicList0[ 0 ]'s frame motion.  Cost
 *     + ref bits + mb_type bits.
 *  3. With partitions: 16x8, 8x16 and 8x8 (sub_mb_type 0 each), in that
 *     order, each partition in mbPartIdx order: the best 16x16 reference
 *     (or, with partition_all_refs, each reference), candidates the best
 *     16x16 vector of that reference and the vectors of the partitions
 *     already chosen; the partition's motion is written into the entry
 *     (and decoded_4x4) before the next partition's mvp.  P_8x8 with all
 *     four ref_idx 0 stays P_8x8 (P_8x8ref0 is never used).
 *  4. Intra: h264enc_intra_decide with try_nxn = intra4x4_always, or 1
 *     when the best inter cost so far is above the Intra_16x16 cost.
 *  5. The least cost.  transform: CHOOSE for inter kinds when
 *     ctx->transform_8x8_mode and transform_choice, else 4X4.
 *
 * Fills *decision; returns its cost.
 */
uint32_t h264enc_mode_p(struct h264enc_mbctx *ctx, struct h264enc_mb_decision *decision);

#endif
