/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The temporal motion vector prediction of the software H.265 decoder:
 * the motion a decoded picture keeps for later pictures, the temporal
 * candidate of a prediction block (8.5.3.2.8, 8.5.3.2.9) and the scaling
 * of a motion vector by order count distances (8-205 to 8-209, also
 * used by the spatial candidates of 8.5.3.2.7).
 *
 * A picture keeps its motion at the granularity the collocated
 * candidates read it: one record per 16x16 luma block, taken from the 4x4
 * block at the 16x16 block's top-left corner (8.5.3.2.8 reads the block
 * covering ( ( x >> 4 ) << 4, ( y >> 4 ) << 4 )).  The record carries the
 * order count and the long-term marking of each reference as they were
 * when the picture was decoded (LongTermRefPic and DiffPicOrderCnt of
 * 8.5.3.2.9 ask for them), so the collocated picture's own reference
 * lists need not be kept.
 */

#ifndef LIBMEDIA_H265SW_TMVP_H
#define LIBMEDIA_H265SW_TMVP_H

#include "h265sw.h"
#include "h265sw-pic.h"
#include "vcodec-frame.h"

/* The flags of struct h265sw_col_motion. */
#define H265SW_COL_L0 0x01U /* predFlagL0 */
#define H265SW_COL_L1 0x02U /* predFlagL1 */
#define H265SW_COL_LONG_L0 0x04U /* the L0 reference was a long-term reference picture */
#define H265SW_COL_LONG_L1 0x08U /* the L1 reference was a long-term reference picture */

/*
 * The motion of one 16x16 block of a decoded picture: flags
 * H265SW_COL_* (none for an intra block or a generated picture), mv[X]
 * mvLX, ref_poc[X] PicOrderCntVal of RefPicListX[ refIdxLX ] of the slice
 * that contained the block.
 */
struct h265sw_col_motion {
	int16_t mv[2][2];
	int32_t ref_poc[2];
	uint8_t flags;
};

/*
 * Returns the side data size of a frame of width by height luma samples:
 * Ceil( width / 16 ) * Ceil( height / 16 ) records.  The pool's side_init
 * is h265sw_tmvp_side_init.
 */
size_t h265sw_tmvp_side_bytes(unsigned width, unsigned height);

/* Makes every record of a new frame intra (flags 0): the vcodec_side_init_fn of the pool. */
void h265sw_tmvp_side_init(void *side, size_t side_bytes);

/*
 * Copies the motion of the decoded picture into its frame at the end of
 * the picture: for each 16x16 block the 4x4 entry at its top-left corner
 * of pic->mv, with ref_poc and the long-term flags from the segment of
 * the CTB that contains it (pic->segments).  A block of a CTB that was
 * not decoded is intra.
 */
void h265sw_tmvp_store(const struct h265sw_pic *pic, struct vcodec_frame *frame);

/*
 * Scales a motion vector component by the distances (8-205 to 8-209):
 * td = Clip3( -128, 127, col_diff ), tb = Clip3( -128, 127, curr_diff )
 * (the differences given in int64_t, so DiffPicOrderCnt of any two
 * order counts fits), tx = ( 16384 + ( Abs( td ) >> 1 ) ) / td,
 * distScaleFactor = Clip3( -4096, 4095, ( tb * tx + 32 ) >> 6 ), result
 * Clip3( -32768, 32767, Sign( dsf * mv ) * ( ( Abs( dsf * mv ) + 127 ) >>
 * 8 ) ).  Division truncates toward zero (5.7 "/").  td 0 cannot occur
 * in a conforming stream; it returns mv unscaled.
 */
int16_t h265sw_tmvp_scale(int16_t mv, int64_t col_diff, int64_t curr_diff);

/*
 * The inputs of the temporal candidate of one prediction block: col the
 * collocated picture's frame (its side data, its poc), cur_poc the
 * current PicOrderCntVal, ctb_log2, width and height of the picture,
 * no_backward_pred and collocated_from_l0 of the slice.
 */
struct h265sw_tmvp_ctx {
	const struct vcodec_frame *col;
	int32_t cur_poc;
	unsigned ctb_log2;
	unsigned width;
	unsigned height;
	int no_backward_pred;
	int collocated_from_l0;
};

/*
 * Derives mvLXCol and availableFlagLXCol (8.5.3.2.8, 8.5.3.2.9) for the
 * prediction block ( x_pb, y_pb, w, h ), list X (0 or 1) and the
 * reference refIdxLX whose order count is ref_poc and long-term marking
 * ref_long.
 *
 * The bottom-right candidate ( xPb + nPbW, yPb + nPbH ) when it lies in
 * the same CTB row and inside the picture (8-198, 8-199), else or when
 * unavailable the centre ( xPb + ( nPbW >> 1 ), yPb + ( nPbH >> 1 ) )
 * (8-200, 8-201); each read at its 16x16 block.  The choice of mvCol by
 * the predFlags, NoBackwardPredFlag and collocated_from_l0_flag; the
 * long-term check; mvCol as is when the reference is long-term or the
 * distances are equal (8-204), else scaled (h265sw_tmvp_scale).
 *
 * Writes mv[0..1] (0 when unavailable); returns availableFlagLXCol.
 * ctx->col NULL gives 0 (slice_temporal_mvp_enabled_flag 0).
 */
int h265sw_tmvp_candidate(const struct h265sw_tmvp_ctx *ctx, int x_pb, int y_pb, unsigned w, unsigned h, unsigned list, int32_t ref_poc, int ref_long, int16_t mv[2]);

#endif
