/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The picture order count of the software H.264 decoder (8.2.1): the
 * three types of 8.2.1.1, 8.2.1.2 and 8.2.1.3 for frames, and the state
 * carried from one picture to the next.
 */

#ifndef LIBMEDIA_H264SW_POC_H
#define LIBMEDIA_H264SW_POC_H

#include "h264sw.h"
#include "h264sw-ps.h"
#include "h264sw-slice.h"

/*
 * What the order count of a picture needs from the pictures before it.
 *
 * prev_msb and prev_lsb are prevPicOrderCntMsb and prevPicOrderCntLsb of
 * type 0 (8.2.1.1): taken from the previous reference picture, or 0 and
 * the reset TopFieldOrderCnt after a picture with operation 5.
 * prev_frame_num and prev_frame_num_offset are prevFrameNum and
 * prevFrameNumOffset of types 1 and 2 (8.2.1.2, 8.2.1.3), prev_mmco5
 * whether the previous picture had operation 5.  prev_ref_frame_num is
 * PrevRefFrameNum (7.4.3), which the gap test of frame_num and the
 * decoding of gaps (8.2.5.2) use.
 *
 * Owned by the decoder; reset by an IDR picture as the clauses say.
 */
struct h264sw_poc_state {
	int32_t prev_msb;
	int32_t prev_lsb;
	uint32_t prev_frame_num;
	int64_t prev_frame_num_offset;
	int prev_mmco5;
	uint32_t prev_ref_frame_num;
};

/*
 * The order counts of one frame: TopFieldOrderCnt, BottomFieldOrderCnt,
 * their minimum (PicOrderCnt of a frame, 8-1), and FrameNumOffset
 * (types 1 and 2; 0 for type 0).
 */
struct h264sw_poc {
	int32_t top;
	int32_t bottom;
	int32_t poc;
	int64_t frame_num_offset;
};

/* Clears the state as before the first picture of a stream. */
void h264sw_poc_init(struct h264sw_poc_state *state);

/*
 * Derives the order counts of a frame from its first slice's header (8.2.1).
 *
 * sps is the active SPS and header the picture's first slice header.
 * Type 0 (8.2.1.1): PicOrderCntMsb from prev_msb, prev_lsb and
 * pic_order_cnt_lsb with MaxPicOrderCntLsb (8-3); TopFieldOrderCnt =
 * PicOrderCntMsb + pic_order_cnt_lsb, BottomFieldOrderCnt = Top +
 * delta_pic_order_cnt_bottom.  Type 1 (8.2.1.2): FrameNumOffset, then
 * absFrameNum, picOrderCntCycleCnt, frameNumInPicOrderCntCycle and
 * expectedPicOrderCnt (8-6 to 8-10).  Type 2 (8.2.1.3): tempPicOrderCnt
 * from FrameNumOffset and frame_num (8-12).  For an IDR picture the
 * previous values are taken as 0 as each clause says.
 *
 * Does not change state: h264sw_poc_finish does, after the picture.
 * Arithmetic is done in 64 bits; a result outside int32_t (a stream
 * that breaks the range of 8.2.1) returns EINVAL.  Returns 0 and *poc.
 */
int h264sw_poc_compute(const struct h264sw_poc_state *state, const struct h264sw_sps *sps, const struct h264sw_slice_header *header, struct h264sw_poc *poc);

/*
 * Carries the state past a decoded picture, and resets its counts when it had operation 5 (8.2.1).
 *
 * header is the picture's first slice header and poc what
 * h264sw_poc_compute gave.  When the picture had operation 5: poc is
 * changed as 8.2.1 says (tempPicOrderCnt = PicOrderCnt( CurrPic ); Top
 * and Bottom minus it), frame_num is taken as 0 for prevFrameNum and
 * prevFrameNumOffset as 0, and for type 0 prev_msb is 0 and prev_lsb is
 * the new TopFieldOrderCnt (8.2.1.1).  Otherwise, for a reference picture
 * (nal_ref_idc above 0), type 0 keeps its PicOrderCntMsb and
 * pic_order_cnt_lsb.  For every picture prev_frame_num and
 * prev_frame_num_offset take its frame_num and FrameNumOffset;
 * prev_ref_frame_num takes frame_num for a reference picture (0 after
 * operation 5).
 */
void h264sw_poc_finish(struct h264sw_poc_state *state, const struct h264sw_sps *sps, const struct h264sw_slice_header *header, struct h264sw_poc *poc);

#endif
