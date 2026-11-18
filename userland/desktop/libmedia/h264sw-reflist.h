/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reference picture lists of a slice of the software H.264 decoder
 * (8.2.4): the picture numbers (8.2.4.1), the initial lists of P and B
 * slices (8.2.4.2.1, 8.2.4.2.3, with 8.2.4.2.5 not applying to frames),
 * and their modification (8.2.4.3).
 */

#ifndef LIBMEDIA_H264SW_REFLIST_H
#define LIBMEDIA_H264SW_REFLIST_H

#include "h264sw.h"
#include "h264sw-dpb.h"
#include "h264sw-frame.h"
#include "h264sw-slice.h"

/*
 * One entry of a reference picture list.
 *
 * frame is the reference frame, borrowed from the DPB (the DPB keeps it
 * while the slice is decoded: marking happens after the picture).
 * long_term is 1 for a long-term reference.  poc is PicOrderCnt of the
 * frame (8-1).  pic_num is PicNum (short-term, 8-28) or LongTermPicNum
 * (8-29).  missing is 1 when the position had no picture (the list was
 * shorter than num_ref_idx_lX_active, or a modification named a picture
 * the DPB lacks): frame then points to a stand-in (below) so that the
 * prediction reads samples, and the decoder counts the picture as
 * concealed.
 */
struct h264sw_ref {
	struct h264sw_frame *frame;
	int long_term;
	int32_t poc;
	int32_t pic_num;
	int missing;
};

/*
 * The lists of one slice: RefPicList0 and RefPicList1, count[n] entries
 * each (num_ref_idx_active of the header; 0 for an unused list).
 */
struct h264sw_reflist {
	unsigned count[2];
	struct h264sw_ref list[2][H264SW_MAX_LIST];
};

/*
 * Builds the reference picture lists of a P or B slice (8.2.4).
 *
 * dpb holds the references as marked before the current picture;
 * header is the slice's header; frame_num is the current picture's
 * frame_num, poc its PicOrderCnt and poc_type the active SPS's
 * pic_order_cnt_type.  Computes FrameNumWrap (8-27) and
 * the picture numbers, builds the initial lists: for P, the short-term
 * frames by descending PicNum then the long-term frames by ascending
 * LongTermPicNum (8.2.4.2.1); for B, list 0 the short-term frames with
 * POC below the current one by descending POC, then those above by
 * ascending POC, then the long-term frames by ascending LongTermPicNum,
 * list 1 the same with the two short-term halves swapped, and when list
 * 1 has more than one entry and equals list 0, its first two entries
 * swapped (8.2.4.2.3).  Non-existing frames are in the initial lists
 * like the others, except in a B slice with poc_type 0, where they are
 * left out of both initial lists (8.2.4.2.3: their PicOrderCnt is not
 * defined then).  Each list is cut to num_ref_idx_active (8.2.4.2) and then
 * modified (8.2.4.3.1 for idc 0 and 1 with picNumPred, picNumNoWrap and
 * picNum of 8-34 to 8-37; 8.2.4.3.2 for idc 2), shifting the entries as
 * the clauses say.
 *
 * Positions left without a picture are marked missing and given a
 * stand-in: the first entry of the same list that is not missing, else
 * of the other list, else any frame in the DPB.  When the DPB has no
 * frame at all (a P slice at the start of a stream that begins without
 * an IDR picture), returns EINVAL: the slice cannot be predicted.
 *
 * For I slices both counts are 0 and nothing else is done.  Returns 0
 * and *lists; EINVAL as above.
 */
int h264sw_reflist_build(const struct h264sw_dpb *dpb, const struct h264sw_slice_header *header, uint32_t frame_num, uint32_t max_frame_num, int32_t poc, unsigned poc_type, struct h264sw_reflist *lists);

/*
 * Finds the lowest index of list 0 whose frame is the frame of a given id (8.4.1.2.3, refIdxL0).
 *
 * Returns that index, or -1 when no entry of list 0 holds it.
 */
int h264sw_reflist_find(const struct h264sw_reflist *lists, uint32_t frame_id);

#endif
