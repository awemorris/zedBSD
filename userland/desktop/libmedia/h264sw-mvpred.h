/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The motion vector prediction of the software H.264 decoder: the
 * neighbouring partitions (6.4.11.7), the prediction of a partition's
 * vector from them (8.4.1.3 with the median of 8.4.1.3.1 and the
 * neighbours' motion of 8.4.1.3.2), and the motion of P_Skip (8.4.1.1).
 */

#ifndef LIBMEDIA_H264SW_MVPRED_H
#define LIBMEDIA_H264SW_MVPRED_H

#include "h264sw.h"

struct h264sw_slice_ctx;

/*
 * Predicts the motion vector of a partition (8.4.1.3).
 *
 * The partition is the rectangle of the current macroblock at (x, y),
 * width by height luma samples (a macroblock partition or a
 * sub-macroblock partition); list is 0 or 1 and ref_idx its refIdxLX.
 *
 * The neighbours A, B, C and D are the partitions holding the luma
 * locations ( x - 1, y ), ( x, y - 1 ), ( x + width, y - 1 ) and
 * ( x - 1, y - 1 ) (6.4.11.7 with h264sw_mb_locate): a location in the
 * current macroblock is available only when its 4x4 block's motion is
 * already derived in this macroblock (ctx->decoded_4x4 has its bit; a
 * partition later in decoding order is "not available"), and C is
 * replaced by D when C is not available.  For each neighbour,
 * 8.4.1.3.2 gives refIdxLXN and mvLXN: -1 and 0 when it is not
 * available, intra, or does not use list X (predFlagLX 0).
 *
 * Then: for a 16x8 partition, the upper one (y 0) takes mvLXB when
 * refIdxLXB equals ref_idx and the lower one mvLXA when refIdxLXA equals
 * it; for an 8x16 partition, the left one takes mvLXA and the right one
 * mvLXC under the same condition (8.4.1.3, 8-203 to 8-206); otherwise
 * the median of 8.4.1.3.1: when B and C are both not available and A is,
 * A's values stand for B's and C's; when exactly one of refIdxLXA,
 * refIdxLXB, refIdxLXC equals ref_idx that one's vector; else the
 * component-wise median of the three.
 *
 * mvp receives the prediction, in quarter luma samples.
 */
void h264sw_mvpred(const struct h264sw_slice_ctx *ctx, unsigned list, unsigned x, unsigned y, unsigned width, unsigned height, int ref_idx, int16_t *mvp);

/*
 * Derives the motion vector of a P_Skip macroblock (8.4.1.1).
 *
 * refIdxL0 is 0.  mv receives 0 when mbAddrA or mbAddrB is not available,
 * or refIdxL0A is 0 with mvL0A 0, or refIdxL0B is 0 with mvL0B 0 (A and
 * B the neighbours of the macroblock as a 16x16 partition, 8.4.1.3.2);
 * otherwise the 16x16 prediction of h264sw_mvpred with ref_idx 0.
 */
void h264sw_mvpred_pskip(const struct h264sw_slice_ctx *ctx, int16_t *mv);

#endif
