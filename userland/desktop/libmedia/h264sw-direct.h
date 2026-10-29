/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The direct prediction of B slices of the software H.264 decoder
 * (8.4.1.2): the co-located block (8.4.1.2.1, frames only: colPic is
 * RefPicList1[ 0 ], mbAddrCol is CurrMbAddr, Table 8-6 and Table 8-8 for
 * PicCodingStruct( CurrPic ) FRM), the spatial mode (8.4.1.2.2) and the
 * temporal mode (8.4.1.2.3), for B_Skip, B_Direct_16x16 and the
 * B_Direct_8x8 sub-macroblocks.
 */

#ifndef LIBMEDIA_H264SW_DIRECT_H
#define LIBMEDIA_H264SW_DIRECT_H

#include "h264sw.h"
#include "h264sw-frame.h"
#include "h264sw-reflist.h"

struct h264sw_slice_ctx;

/*
 * What a B slice's direct prediction prepares once.
 *
 * col is the frame of RefPicList1[ 0 ] (colPic) and col_long_term
 * whether that entry is a long-term reference (spatial mode's colZeroFlag
 * needs it short-term).  For the temporal mode: dist_scale[i] is
 * DistScaleFactor for refIdxL0 i (8-197, 8-198 with tb and td of 8-201
 * and 8-202 between the current picture, RefPicList0[ i ] and
 * RefPicList1[ 0 ]), and long_term0[i] whether RefPicList0[ i ] is
 * long-term or its td is 0 (then mvL0 = mvCol and mvL1 = 0, 8-195, 8-196).
 *
 * For the spatial mode of the current macroblock, spatial_ready is 1
 * once its refIdxL0, refIdxL1 and the predicted vectors (taken for the
 * whole macroblock as 16x16, 8.4.1.2.2) are derived; they are kept in
 * spatial_ref and spatial_mv for the macroblock's other direct
 * sub-macroblocks; direct_zero is the macroblock's
 * directZeroPredictionFlag (8-190).  h264sw_slice_data clears
 * spatial_ready before each macroblock.
 */
struct h264sw_direct {
	const struct h264sw_frame *col;
	int col_long_term;
	int32_t dist_scale[H264SW_MAX_LIST];
	uint8_t long_term0[H264SW_MAX_LIST];
	int spatial_ready;
	int spatial_ref[2];
	int16_t spatial_mv[2][2];
	int direct_zero;
};

/*
 * Prepares the direct prediction of a B slice.
 *
 * lists are the slice's lists (list 1 has at least one entry in a B
 * slice); poc is the current picture's PicOrderCnt.  Computes col,
 * col_long_term and, for every refIdxL0, dist_scale (tb = Clip3( -128,
 * 127, DiffPicOrderCnt( currPicOrField, pic0 ) ) (8-201), td = Clip3(
 * -128, 127, DiffPicOrderCnt( pic1, pic0 ) ) (8-202), tx = ( 16384 +
 * Abs( td / 2 ) ) / td (8-197), DistScaleFactor = Clip3( -1024, 1023, (
 * tb * tx + 32 ) >> 6 ) (8-198), with pic0 RefPicList0[ i ] and pic1
 * RefPicList1[ 0 ]).  When RefPicList0[ i ] is long-term or td is 0
 * (DiffPicOrderCnt( pic1, pic0 ) equal to 0), DistScaleFactor is not
 * computed (no division by 0): the temporal mode uses mvL0 = mvCol and
 * mvL1 = 0 for that index (8-195, 8-196), recorded by long_term0[i] 1.
 */
void h264sw_direct_setup(struct h264sw_direct *direct, const struct h264sw_reflist *lists, int32_t poc);

/*
 * Derives the motion of one 8x8 quadrant of the current macroblock in direct mode (8.4.1.2).
 *
 * quadrant is mbPartIdx 0..3; the caller calls it for each direct
 * quadrant in increasing order (all four for B_Skip and B_Direct_16x16,
 * after the other sub-macroblocks before it for B_8x8), so that the
 * motion vector prediction of later partitions sees it.  The mode is
 * direct_spatial_mv_pred_flag of the slice header.
 *
 * Writes, in the current macroblock's motion (ctx->n.current->motion),
 * ref_idx[0..1][quadrant], ref_id[0..1][quadrant] (the frames of
 * RefPicListX[ refIdx ]), and mv[0..1][blk] of the quadrant's four 4x4
 * blocks.  With direct_8x8_inference_flag 1 the co-located vectors of
 * the corner 4x4 blocks (luma4x4BlkIdx 0, 5, 10, 15 of the co-located
 * macroblock, by quadrant) are used for the whole quadrant (Table 8-8,
 * luma4x4BlkIdx = 5 * mbPartIdx); else each 4x4 block uses its own
 * (4 * mbPartIdx + subMbPartIdx).  The co-located block gives mvCol and
 * refIdxCol from its list 0 motion when that list was used, else from
 * list 1; an intra co-located macroblock gives mvCol 0 and refIdxCol -1
 * (8.4.1.2.1).
 *
 * Spatial (8.4.1.2.2): refIdxL0 and refIdxL1 are MinPositive of the
 * neighbours A, B, C of the macroblock (8.4.1.3.2 for a 16x16
 * partition), both -1 meaning refIdxL0 = refIdxL1 = 0 with zero vectors
 * (directZeroPredictionFlag); the vectors are the 16x16 prediction
 * (8.4.1.3) per list, set to 0 for a 4x4 block (or quadrant) whose
 * colZeroFlag is 1 (RefPicList1[ 0 ] short-term, refIdxCol 0, both
 * components of mvCol within -1..1) when that list's refIdx is 0, and
 * for a list whose refIdx is below 0 the list is unused.
 *
 * Temporal (8.4.1.2.3): refIdxL0 = the lowest index of list 0 holding
 * the frame refIdxCol named in colPic (h264sw_reflist_find with the
 * co-located ref_id; 0 when refIdxCol is below 0; when the frame is in
 * no entry the stream is broken: refIdxL0 0 is used and the macroblock
 * counted as concealed), refIdxL1 = 0, mvL0 = ( DistScaleFactor * mvCol
 * + 128 ) >> 8, mvL1 = mvL0 - mvCol (8-191, 8-192, 8-199, 8-200),
 * computed in int32_t and clamped to -32768..32767 before they are
 * stored (a conforming stream stays inside).
 *
 * Sets the quadrant's bit in ctx->n.current->direct.  Returns 0, or
 * EINVAL when the derivation cannot proceed (no co-located frame).
 */
int h264sw_direct_quadrant(struct h264sw_slice_ctx *ctx, unsigned quadrant);

#endif
