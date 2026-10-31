/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inter prediction of one prediction unit in the software H.265
 * decoder (8.5.3): its motion from merge mode (h265sw-merge.h) or from
 * the motion vector predictor and the difference (h265sw-amvp.h,
 * 8.5.3.2.1), written to the picture context at once (the next
 * prediction unit of the same coding unit reads it), and its prediction
 * samples (8.5.3.3): the reference picture of each used list (8.5.3.3.2),
 * the luma and chroma interpolation and the default or explicit weighting
 * (h265sw-interp.h), written into the current frame.  The residual is
 * added afterwards by h265sw-recon.h.
 */

#ifndef LIBMEDIA_H265SW_INTER_H
#define LIBMEDIA_H265SW_INTER_H

#include "h265sw.h"
#include "h265sw-ctu.h"
#include "h265sw-mvctx.h"
#include "h265sw-ps.h"
#include "h265sw-reflist.h"
#include "h265sw-slice.h"
#include "vcodec-frame.h"

/*
 * What the inter prediction of a slice reads and writes: frame the
 * current picture's samples (written); mv the motion derivations'
 * context (its pic is written: the motion entries); lists the slice's
 * reference picture lists (the frames to predict from); header the
 * slice segment header (slice_type, mvd_l1_zero, the weights); pps
 * (weighted_pred_flag, weighted_bipred_flag).
 */
struct h265sw_inter_ctx {
	struct vcodec_frame *frame;
	const struct h265sw_mvctx *mv;
	const struct h265sw_reflists *lists;
	const struct h265sw_slice_header *header;
	const struct h265sw_pps *pps;
};

/*
 * Predicts prediction unit pu_index of an inter (or skipped) coding unit.
 *
 * Motion: with merge_flag (always for a skipped CU),
 * h265sw_merge_derive with merge_idx; else for each list the
 * inter_pred_idc uses, ref_idx checked below lists->num[X] (EINVAL
 * otherwise), h265sw_amvp_predict with mvp_flag and h265sw_amvp_add with
 * the CU's MvdLX.  The result (struct h265sw_mv_field) is written to
 * every 4x4 entry of the prediction block (h265sw_pic_fill_mv) and to
 * *motion when motion is not NULL.
 *
 * Samples (8.5.3.3): for each used list, the frame RefPicListX[ refIdxLX
 * ] (never NULL: missing references were replaced), luma by
 * h265sw_interp_luma at ( xPb + ( mv[0] >> 2 ), yPb + ( mv[1] >> 2 ) )
 * with the fractions mv & 3, chroma by h265sw_interp_chroma at half the
 * position with mvC = mv (8-210, 8-211 for 4:2:0) and the fractions mvC
 * & 7; then explicit weighting (h265sw_weight_explicit with the slice's
 * weights of refIdxL0 and refIdxL1) when weighted_pred_flag is 1 for a P
 * slice or weighted_bipred_flag is 1 for a B slice, else
 * h265sw_weight_default.  The intermediate blocks (at most 64x64 int16_t
 * per list and component) are on the stack or in a scratch the
 * implementation keeps within its call.
 *
 * Returns 0; EINVAL for a reference index out of range.  Every sample
 * write stays inside the frame.
 */
int h265sw_inter_predict(const struct h265sw_inter_ctx *ctx, const struct h265sw_cu *cu, unsigned pu_index, struct h265sw_mv_field *motion);

#endif
