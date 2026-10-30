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
 * The buffers, the removal of unneeded pictures, the bumping, the flush
 * and the clear are the shared ones of vcodec-dpb.h; this module adds
 * the H.265 marking and the H.265 conditions for bumping.  The frames
 * are struct vcodec_frame whose side data is the motion h265sw-tmvp.h
 * defines (struct h265sw_col_motion per 16x16 block).
 */

#ifndef LIBMEDIA_H265SW_DPB_H
#define LIBMEDIA_H265SW_DPB_H

#include "h265sw.h"
#include "h265sw-poc.h"
#include "vcodec-dpb.h"
#include "vcodec-frame.h"
#include "vcodec-output.h"

/*
 * The decoded picture buffer of H.265: the shared buffers base (size is
 * sps_max_dec_pic_buffering_minus1[ HighestTid ] + 1, 1..16; each entry
 * with the marking VCODEC_REF_*, poc PicOrderCntVal, latency
 * PicLatencyCount, and generated for the pictures of 8.3.3 and the
 * stand-ins), and the
 * sequence's output rules: reorder sps_max_num_reorder_pics[ HighestTid ],
 * max_latency SpsMaxLatencyPictures[ HighestTid ] (7-9: reorder +
 * sps_max_latency_increase_plus1 - 1) with latency_limited
 * sps_max_latency_increase_plus1 != 0.  HighestTid is the highest
 * sub-layer (the decoder decodes all of them).  Embedded in the decoder.
 */
struct h265sw_dpb {
	struct vcodec_dpb base;
	unsigned reorder;
	uint64_t max_latency;
	int latency_limited;
};

/*
 * The reference picture set of the current picture as entries of the
 * DPB (8.3.2): RefPicSetStCurrBefore, StCurrAfter, StFoll, LtCurr and
 * LtFoll, each an index into dpb->base.entry or -1 for "no reference
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

/* Makes an empty DPB with the sequence's parameters (vcodec_dpb_init with size 1..16, and the output rules as struct h265sw_dpb says). */
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
 * The flush (the end of the stream, an end of sequence NAL unit, a
 * drain) and the clear (a seek, NoOutputOfPriorPicsFlag 1) are
 * vcodec_dpb_flush and vcodec_dpb_clear on dpb->base.
 */

#endif
