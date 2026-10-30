/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The picture order count of the software H.265 decoder (8.3.1) and the
 * order counts of a picture's reference picture set (the first part of
 * 8.3.2: the five lists PocStCurrBefore, PocStCurrAfter, PocStFoll,
 * PocLtCurr and PocLtFoll, 8-5).  Which pictures of the decoded picture
 * buffer those order counts name is h265sw-dpb.h's part.
 */

#ifndef LIBMEDIA_H265SW_POC_H
#define LIBMEDIA_H265SW_POC_H

#include "h265sw.h"
#include "h265sw-slice.h"

/*
 * What 8.3.1 carries from picture to picture: prev_lsb and prev_msb are
 * slice_pic_order_cnt_lsb and PicOrderCntMsb of prevTid0Pic (the
 * previous picture in decoding order with TemporalId 0 that is not a
 * RASL, RADL or sub-layer non-reference picture); 0 and 0 at the start.
 */
struct h265sw_poc_state {
	uint32_t prev_lsb;
	int32_t prev_msb;
};

/*
 * The order counts of a picture's reference picture set (8-5).
 *
 * st_before[0..num_st_before), st_after, st_foll: PicOrderCntVal +
 * DeltaPocS0 or DeltaPocS1 of the short-term entries, split by used
 * flag; lt_curr and lt_foll: the long-term entries' pocLt with
 * lt_curr_msb and lt_foll_msb CurrDeltaPocMsbPresentFlag and
 * FollDeltaPocMsbPresentFlag (1: pocLt is a full order count; 0: only
 * its slice_pic_order_cnt_lsb bits identify the picture).  Every count is
 * at most H265SW_MAX_RPS; the sums of order counts are computed in
 * int64_t and an order count outside int32_t is EINVAL.
 */
struct h265sw_rps_pocs {
	unsigned num_st_before;
	unsigned num_st_after;
	unsigned num_st_foll;
	unsigned num_lt_curr;
	unsigned num_lt_foll;
	int32_t st_before[H265SW_MAX_RPS];
	int32_t st_after[H265SW_MAX_RPS];
	int32_t st_foll[H265SW_MAX_RPS];
	int32_t lt_curr[H265SW_MAX_RPS];
	int32_t lt_foll[H265SW_MAX_RPS];
	uint8_t lt_curr_msb[H265SW_MAX_RPS];
	uint8_t lt_foll_msb[H265SW_MAX_RPS];
};

/* Resets the state (the start of a stream). */
void h265sw_poc_init(struct h265sw_poc_state *state);

/*
 * Derives PicOrderCntVal of a picture (8.3.1, 8-1 and 8-2).
 *
 * header is the picture's first slice segment header; irap_no_rasl is 1
 * for an IRAP picture with NoRaslOutputFlag 1 (PicOrderCntMsb 0);
 * log2_max_poc_lsb from the SPS.  The Msb arithmetic is in int64_t.
 * Returns 0 and *poc; EINVAL when PicOrderCntVal leaves -2^31..2^31 - 1.
 * The state is not changed (h265sw_poc_finish does that).
 */
int h265sw_poc_compute(const struct h265sw_poc_state *state, const struct h265sw_slice_header *header, int irap_no_rasl, unsigned log2_max_poc_lsb, int32_t *poc);

/*
 * Records a decoded picture as prevTid0Pic when it qualifies (TemporalId
 * 0 and not RASL, RADL or SLNR, by its nal_type): prev_lsb its
 * slice_pic_order_cnt_lsb, prev_msb poc - lsb.
 */
void h265sw_poc_finish(struct h265sw_poc_state *state, const struct h265sw_slice_header *header, int32_t poc);

/*
 * Derives the order counts of the reference picture set (8.3.2, 8-5).
 *
 * header carries st_rps and the long-term entries; poc is the current
 * PicOrderCntVal; log2_max_poc_lsb from the SPS (MaxPicOrderCntLsb).
 * For an IDR picture every list is empty.  The long-term pocLt with
 * delta_poc_msb_present_flag is PicOrderCntVal - DeltaPocMsbCycleLt *
 * MaxPicOrderCntLsb - ( PicOrderCntVal & ( MaxPicOrderCntLsb - 1 ) ) +
 * PocLsbLt, in int64_t.  Returns 0 and *out; EINVAL when an order count
 * leaves int32_t.
 */
int h265sw_poc_rps(const struct h265sw_slice_header *header, int32_t poc, unsigned log2_max_poc_lsb, struct h265sw_rps_pocs *out);

#endif
