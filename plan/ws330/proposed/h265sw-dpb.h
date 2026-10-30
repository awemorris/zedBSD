/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoded picture buffer of the software H.265 decoder: the marking
 * of the reference picture set (the second part of 8.3.2), the
 * generation of unavailable reference pictures (8.3.3), and the output
 * order DPB of C.5.2 (removal and "bumping" before a picture, C.5.2.2;
 * storage and additional bumping after it, C.5.2.3; the bumping process,
 * C.5.2.4).
 *
 * The steps for one picture, in this order: h265sw_dpb_apply_rps (8.3.2),
 * h265sw_dpb_generate (8.3.3, and stand-ins for missing references),
 * h265sw_dpb_before_picture (C.5.2.2), the decoding, h265sw_dpb_store
 * (C.5.2.3).  Frames output by bumping are appended to a struct
 * vcodec_output_queue in output order.
 *
 * The frames are struct vcodec_frame whose side data is the motion
 * h265sw-tmvp.h defines (struct h265sw_col_motion per 16x16 block).
 */

#ifndef LIBMEDIA_H265SW_DPB_H
#define LIBMEDIA_H265SW_DPB_H

#include "h265sw.h"
#include "h265sw-poc.h"
#include "vcodec-frame.h"
#include "vcodec-output.h"

/* The marking of an entry (8.3.2). */
#define H265SW_REF_UNUSED 0U
#define H265SW_REF_SHORT 1U
#define H265SW_REF_LONG 2U

/*
 * One picture storage buffer.
 *
 * frame NULL for an empty buffer (the entry holds one reference
 * otherwise); reference H265SW_REF_*; output_needed 1 while "needed for
 * output"; latency PicLatencyCount; poc PicOrderCntVal (also in the
 * frame, kept here for the marking); generated 1 for a picture made by
 * 8.3.3 or as a stand-in (never output).
 */
struct h265sw_dpb_entry {
	struct vcodec_frame *frame;
	unsigned reference;
	int output_needed;
	uint32_t latency;
	int32_t poc;
	int generated;
};

/*
 * The decoded picture buffer.
 *
 * size is sps_max_dec_pic_buffering_minus1[ HighestTid ] + 1 (1..16),
 * reorder sps_max_num_reorder_pics[ HighestTid ], max_latency
 * SpsMaxLatencyPictures[ HighestTid ] (7-9: reorder +
 * sps_max_latency_increase_plus1 - 1) with latency_limited
 * sps_max_latency_increase_plus1 != 0.  HighestTid is the highest
 * sub-layer (the decoder decodes all of them).  entry[0..size) are the
 * buffers; pictures counts the non-empty ones.
 */
struct h265sw_dpb {
	unsigned size;
	unsigned reorder;
	uint64_t max_latency;
	int latency_limited;
	struct h265sw_dpb_entry entry[H265SW_MAX_DPB];
	unsigned pictures;
};

/*
 * The reference picture set of the current picture as entries of the
 * DPB (8.3.2): RefPicSetStCurrBefore, StCurrAfter, StFoll, LtCurr and
 * LtFoll, each an index into dpb->entry or -1 for "no reference
 * picture", in the order of the matching lists of struct
 * h265sw_rps_pocs.
 */
struct h265sw_ref_sets {
	unsigned num_st_before;
	unsigned num_st_after;
	unsigned num_st_foll;
	unsigned num_lt_curr;
	unsigned num_lt_foll;
	int st_before[H265SW_MAX_RPS];
	int st_after[H265SW_MAX_RPS];
	int st_foll[H265SW_MAX_RPS];
	int lt_curr[H265SW_MAX_RPS];
	int lt_foll[H265SW_MAX_RPS];
};

/* Makes an empty DPB with the sequence's parameters (sizes as struct h265sw_dpb says; size 1..16). */
void h265sw_dpb_init(struct h265sw_dpb *dpb, unsigned size, unsigned reorder, uint32_t max_latency_increase_plus1);

/*
 * Applies the reference picture set of the current picture (8.3.2).
 *
 * pocs from h265sw_poc_rps; irap_no_rasl 1 marks every reference
 * picture "unused for reference" first; log2_max_poc_lsb from the SPS.
 * The long-term entries are found first (a reference picture whose
 * PicOrderCntVal, or with the msb flag 0 whose PicOrderCntVal & (
 * MaxPicOrderCntLsb - 1 ), equals the entry), then the short-term ones
 * among the short-term reference pictures (PicOrderCntVal equal).
 * Pictures of LtCurr and LtFoll are marked "used for long-term
 * reference"; every reference picture in none of the five sets is
 * marked "unused for reference".  Generated pictures take part like the
 * others.  Writes *sets.  Cannot fail (a missing picture is -1).
 */
void h265sw_dpb_apply_rps(struct h265sw_dpb *dpb, const struct h265sw_rps_pocs *pocs, int irap_no_rasl, unsigned log2_max_poc_lsb, struct h265sw_ref_sets *sets);

/*
 * Fills the "no reference picture" entries of sets with generated
 * pictures (8.3.3.2: every sample 1 << ( BitDepth - 1 ), motion all
 * intra, PicOutputFlag 0, PicOrderCntVal from the entry, marked short- or
 * long-term by its set).
 *
 * With foll_only 1 (a BLA picture or a CRA picture with NoRaslOutputFlag
 * 1, 8.3.3.1) only StFoll and LtFoll entries are generated, as the clause
 * says; with 0 the Curr entries too, as stand-ins for references a broken
 * or cut stream lost (the decoder counts them as concealed).  A
 * generated picture takes an empty buffer; when none is empty it is not
 * made and the entry stays -1.
 *
 * pool gives the frames (the pool's side_init makes their motion
 * intra).  Returns the number generated, or -ENOMEM when the pool fails
 * (the entries made so far stay).
 */
int h265sw_dpb_generate(struct h265sw_dpb *dpb, struct h265sw_ref_sets *sets, const struct h265sw_rps_pocs *pocs, int foll_only, struct vcodec_frame_pool *pool);

/*
 * The removal and bumping before the current picture is decoded
 * (C.5.2.2).
 *
 * irap_no_rasl and first_picture (picture 0 of the stream, or the first
 * after a reset) select the IRAP branch; no_output_of_prior_pics is
 * NoOutputOfPriorPicsFlag already derived by the caller (1 for a CRA
 * picture, else no_output_of_prior_pics_flag).  Bumped frames go to
 * output in order.  Then, when the DPB is still full (every buffer holds
 * a reference picture not needed for output, which only a broken stream
 * leaves), the entry with the smallest PicOrderCntVal that is in no Curr
 * set of sets is emptied so the current picture has a buffer.
 *
 * Returns 0; ENOBUFS when output is full (the caller takes the output
 * first; nothing was emptied that the retry would not empty again);
 * EINVAL when no buffer can be freed (every buffer is in a Curr set).
 */
int h265sw_dpb_before_picture(struct h265sw_dpb *dpb, const struct h265sw_ref_sets *sets, int irap_no_rasl, int first_picture, int no_output_of_prior_pics, struct vcodec_output_queue *output);

/*
 * Stores the decoded current picture (C.5.2.3): PicLatencyCount + 1 for
 * each picture needed for output that follows it in output order (when
 * pic_output is 1); the frame in an empty buffer with a new reference,
 * marked "used for short-term reference" (8.1.3 step 4), needed for
 * output when pic_output is 1 with latency 0; then the additional
 * bumping into output.  Returns 0; EINVAL when no buffer is empty (a
 * caller's mistake: h265sw_dpb_before_picture made room); ENOBUFS as
 * above.
 */
int h265sw_dpb_store(struct h265sw_dpb *dpb, struct vcodec_frame *frame, int32_t poc, int pic_output, struct vcodec_output_queue *output);

/*
 * Outputs every picture still needed for output in increasing
 * PicOrderCntVal (repeated bumping) and empties the DPB: the end of the
 * stream, an end of sequence NAL unit, and a drain.  Returns 0 or
 * ENOBUFS.
 */
int h265sw_dpb_flush(struct h265sw_dpb *dpb, struct vcodec_output_queue *output);

/* Empties the DPB without output (a seek, or NoOutputOfPriorPicsFlag 1). */
void h265sw_dpb_clear(struct h265sw_dpb *dpb);

#endif
