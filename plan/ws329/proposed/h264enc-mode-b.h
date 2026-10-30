/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The macroblock decision of the software H.264 encoder for B slices.
 * B pictures are never references; list 0 holds the past references and
 * list 1 the next anchor (one entry); direct prediction is spatial
 * (direct_spatial_mv_pred_flag 1) with direct_8x8_inference_flag 1.
 * Costs and ties as h264enc-mode.h.  The prediction of a bi-predicted
 * block is the default weighted average ( p0 + p1 + 1 ) >> 1 (8-273) of
 * the two lists' predictions from h264enc_refplanes.
 */

#ifndef LIBMEDIA_H264ENC_MODE_B_H
#define LIBMEDIA_H264ENC_MODE_B_H

#include "h264enc-mbctx.h"

/*
 * Decides a macroblock of a B slice.  The current entry is reset and the
 * neighbours found before the call; the decision may write the entry's
 * motion and the frame's samples of the macroblock.
 *
 *  1. Direct: h264sw_direct_quadrant for quadrants 0..3 (the motion in the
 *     entry; each quadrant's 4x4 bits set in decoded_4x4 after it, as
 *     the decoder's reconstruction does), the prediction of each quadrant
 *     from its lists (luma from h264enc_refplanes, chroma by
 *     h264sw_interp_chroma, two lists averaged).  With
 *     early_skip, when the quantized residual (luma and chroma, inter
 *     offset) has no level, the decision is B_Skip at once.  Otherwise
 *     B_Direct_16x16 is a candidate with cost SATD + mb_type bits (and,
 *     without early_skip, B_Skip with cost SATD; a B_Skip decision is
 *     coded without residual).
 *  2. 16x16 L0: each ref_idx below Min( refs, num_ref_idx_active[0] ) as
 *     P's 16x16 (candidates: mvp, the direct vector of list 0 when the
 *     direct reference index is this one, the neighbours' vectors).  16x16
 *     L1: ref_idx 0, the same way.  16x16 Bi: the best L0 and the best L1
 *     together, cost SATD of the average + both vectors' and references'
 *     bits (no joint search).  mb_type bits each.
 *  3. With partitions: 16x8 and 8x16, each partition the best of L0, L1
 *     and Bi (searched per partition with the 16x16 results as
 *     candidates); mb_type from the pair of predictions (Table 7-14); 8x8
 *     with each sub-macroblock the best of direct (B_Direct_8x8), L0, L1
 *     and Bi (B_L0_8x8, B_L1_8x8, B_Bi_8x8; never smaller sub-partitions,
 *     which also keeps MaxMvsPer2Mb and MinLumaBiPredSize of A.3.3).
 *     Partitions are decided in order with their motion written into the
 *     entry before the next one's vector prediction, direct quadrants by
 *     h264sw_direct_quadrant in their turn.
 *  4. Intra as h264enc_mode_p's step 4.
 *  5. The least cost; transform as h264enc_mode_p.
 *
 * Fills *decision; returns its cost.
 */
uint32_t h264enc_mode_b(struct h264enc_mbctx *ctx, struct h264enc_mb_decision *decision);

#endif
