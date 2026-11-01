/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoded picture buffer of the H.265 Vulkan Video path: the marking
 * of the reference picture set (the second part of 8.3.2), the output
 * order DPB of C.5.2 (removal and "bumping" before a picture, C.5.2.2;
 * storage and additional bumping after it, C.5.2.3; the bumping process,
 * C.5.2.4), and the Vulkan DPB slot that holds each picture's image.
 *
 * Vulkan Video leaves the DPB to the application: which pictures are
 * references, which slot each new picture is written into, and when a
 * picture is shown.  This module keeps the H.265 pictures of one decode
 * session as entries (one per picture storage buffer of C.5.2), each
 * with the slot of its image, and answers those questions for each
 * picture.  It knows nothing of Vulkan's types or of images: the slots
 * are small numbers the caller maps to its images, and h265-std.h turns
 * a plan into the Vulkan structures.
 *
 * The steps for one picture, in this order: h265_dpb_begin (8.3.2 and
 * C.5.2.2), h265_dpb_take_slot (a free slot for the picture, which a
 * caller may retry), the decode by the caller, then h265_dpb_end
 * (C.5.2.3) or, when the decode failed, h265_dpb_abort.  Pictures that
 * leave by bumping are reported in output order; their slots stay busy
 * (not reused) until the caller has read them and calls
 * h265_dpb_release.
 *
 * Two parts of the clauses are not done, because the Vulkan Video path
 * never decodes a picture that needs them: the generation of unavailable
 * reference pictures (8.3.3), which only RASL pictures associated with a
 * CRA or BLA picture with NoRaslOutputFlag 1 would read (8.3.3.1 NOTE;
 * h265-stream.h drops those pictures), and a stand-in for a missing
 * reference of the current picture, which only a broken or cut stream
 * has: the entry is reported missing and the caller decides (skip the
 * picture, or decode it with STD_VIDEO_H265_NO_REFERENCE_PICTURE).
 *
 * Only the base layer is kept (every entry has nuh_layer_id 0, so the
 * layer conditions of 8.3.2 always hold), and TwoVersionsOfCurrDecPicFlag
 * is 0 (no screen content tools).
 */

#ifndef LIBMEDIA_H265_DPB_H
#define LIBMEDIA_H265_DPB_H

#include "h265sw.h"
#include "h265sw-poc.h"
#include "h265sw-ps.h"

/* The most pictures of the buffer (A.4.2: MaxDpbSize at most 16, the current picture included). */
#define H265_DPB_PICTURES H265SW_MAX_DPB

/* The most Vulkan DPB slots a session uses (a 32-bit busy mask); the fewest is H265_DPB_PICTURES. */
#define H265_DPB_MAX_SLOTS 32U

/* The slot of no picture: a missing reference, or no slot taken yet. */
#define H265_DPB_NO_SLOT (-1)

/* The marking of a picture (8.3.2): unused, used for short-term, or used for long-term reference. */
#define H265_DPB_UNUSED 0U
#define H265_DPB_SHORT 1U
#define H265_DPB_LONG 2U

/*
 * The most pictures one step outputs: every picture of the buffer, and
 * the current one.
 */
#define H265_DPB_MAX_OUTPUT (H265_DPB_PICTURES + 1U)

/*
 * One picture storage buffer (C.5.2.1).
 *
 * used is 1 while it holds a picture.  marking is H265_DPB_UNUSED,
 * _SHORT or _LONG; needed is "needed for output"; latency is
 * PicLatencyCount (C.5.2.3; counted in uint64_t, so it cannot wrap
 * within any stream); poc is PicOrderCntVal; slot the slot of its image
 * (0..slots - 1, never H265_DPB_NO_SLOT for a used entry); id the
 * caller's number of the picture (its decode order, given to
 * h265_dpb_end).  An unused entry is all 0 except slot, which is
 * H265_DPB_NO_SLOT.
 */
struct h265_dpb_entry {
	int used;
	unsigned marking;
	int needed;
	uint64_t latency;
	int32_t poc;
	int slot;
	uint64_t id;
};

/* One picture output by bumping: its slot (busy until h265_dpb_release), order count and id. */
struct h265_dpb_output {
	int slot;
	int32_t poc;
	uint64_t id;
};

/*
 * The decoded picture buffer of one decode session.
 *
 * slots is the number of Vulkan DPB slots the caller made
 * (H265_DPB_PICTURES..H265_DPB_MAX_SLOTS); busy has bit s set while slot
 * s holds a picture the caller is still reading (set by every output,
 * cleared by h265_dpb_release; the busy slots of the pictures emptied by
 * a reset stay busy until released).  size is
 * sps_max_dec_pic_buffering_minus1[ HighestTid ] + 1, reorder
 * sps_max_num_reorder_pics[ HighestTid ], latency_limited
 * sps_max_latency_increase_plus1[ HighestTid ] != 0 and max_latency
 * SpsMaxLatencyPictures[ HighestTid ] (7-9: reorder +
 * sps_max_latency_increase_plus1 - 1, in uint64_t), all of the SPS of
 * the last h265_dpb_begin; HighestTid is sps_max_sub_layers_minus1 (the
 * caller decodes every sub-layer).  started is 0 until the first picture
 * after h265_dpb_init, h265_dpb_flush or h265_dpb_clear (that picture is
 * "picture 0" of C.5.2.2 for this session).  current is the slot
 * h265_dpb_take_slot gave the picture between begin and end or abort
 * (H265_DPB_NO_SLOT otherwise), and in_picture is 1 between them.
 *
 * Embedded in the caller's decoder; about 1 KiB.
 */
struct h265_dpb {
	unsigned slots;
	uint32_t busy;
	unsigned size;
	unsigned reorder;
	int latency_limited;
	uint64_t max_latency;
	int started;
	int in_picture;
	int current;
	struct h265_dpb_entry entry[H265_DPB_PICTURES];
};

/*
 * The plan of one picture's decode, from h265_dpb_begin.
 *
 * reset is 1 for the first picture after h265_dpb_init, h265_dpb_flush or
 * h265_dpb_clear: the caller resets the video session's state before the
 * decode (no picture of the session is a reference any more).
 *
 * st_before, st_after and lt_curr are RefPicSetStCurrBefore,
 * RefPicSetStCurrAfter and RefPicSetLtCurr (8.3.2) as slots, in the
 * order of the lists of struct h265sw_rps_pocs, with H265_DPB_NO_SLOT for
 * "no reference picture"; missing counts those (the caller decides what
 * to do, see above).  The Foll entries are not listed: they are kept as
 * references (and listed in reference_slot) but the picture does not
 * read them.
 *
 * reference_count and reference_slot, reference_poc and reference_long
 * list every picture marked as a reference after 8.3.2 (the Curr and
 * Foll sets), by slot, with its PicOrderCntVal and whether it is a
 * long-term reference: the slots the caller keeps active for the decode
 * (a Vulkan slot left out is deactivated).  At most H265_DPB_PICTURES - 1.
 *
 * output_count and output are the pictures bumped by C.5.2.2 before this
 * picture, in output order; discarded counts the pictures emptied without
 * output (NoOutputOfPriorPicsFlag 1); evicted counts the pictures emptied
 * only to make room in a buffer a broken stream filled (see
 * h265_dpb_begin).
 *
 * setup_slot is the slot the picture is decoded into, H265_DPB_NO_SLOT
 * until h265_dpb_take_slot.
 */
struct h265_dpb_plan {
	int reset;
	unsigned num_st_before;
	unsigned num_st_after;
	unsigned num_lt_curr;
	int st_before[H265SW_MAX_RPS];
	int st_after[H265SW_MAX_RPS];
	int lt_curr[H265SW_MAX_RPS];
	unsigned missing;
	unsigned reference_count;
	int reference_slot[H265_DPB_PICTURES];
	int32_t reference_poc[H265_DPB_PICTURES];
	uint8_t reference_long[H265_DPB_PICTURES];
	unsigned output_count;
	struct h265_dpb_output output[H265_DPB_MAX_OUTPUT];
	unsigned discarded;
	unsigned evicted;
	int setup_slot;
};

/*
 * Makes an empty buffer with slots Vulkan DPB slots
 * (H265_DPB_PICTURES..H265_DPB_MAX_SLOTS; another value is clamped into
 * that range).  busy 0, size 1 until the first h265_dpb_begin.
 */
void h265_dpb_init(struct h265_dpb *dpb, unsigned slots);

/*
 * Applies the reference picture set of the current picture and the
 * removal and bumping before it (8.3.2, C.5.2.2), and plans its decode.
 *
 * sps is the picture's active SPS (it gives size, reorder and the latency
 * of HighestTid, and MaxPicOrderCntLsb); pocs from h265sw_poc_rps;
 * irap_no_rasl 1 for an IRAP picture with NoRaslOutputFlag 1;
 * no_output_of_prior_pics is NoOutputOfPriorPicsFlag as the caller
 * derived it (C.5.2.2 step 1: 1 for a CRA picture, else
 * no_output_of_prior_pics_flag; read only for irap_no_rasl 1 and not
 * picture 0).  The steps:
 *
 * 1. With irap_no_rasl 1, every reference picture is marked unused
 *    (8.3.2).  Then 8.3.2's steps 1 to 4 (8-6, 8-7): the long-term
 *    entries are found first (a reference picture whose PicOrderCntVal,
 *    or with the msb flag 0 whose PicOrderCntVal & ( MaxPicOrderCntLsb -
 *    1 ), equals the entry; when several match, the one with the
 *    greatest id, a choice the clause leaves open only for broken
 *    streams) and marked long-term, then the short-term ones among the
 *    short-term reference pictures (PicOrderCntVal equal), and every
 *    reference picture in none of the five sets is marked unused.  A
 *    picture already chosen for one set is not chosen again (8.3.2 NOTE
 *    5).
 * 2. C.5.2.2: for an IRAP picture with NoRaslOutputFlag 1 that is not
 *    picture 0 (started 1), with no_output_of_prior_pics 1 every entry
 *    is emptied without output (counted in discarded), else the entries
 *    not needed for output and unused are emptied and the rest are
 *    bumped until all are empty.  Otherwise the entries not needed for
 *    output and unused are emptied, then bumping repeats while the
 *    pictures needed for output are more than reorder, or latency_limited
 *    and one of them has latency >= max_latency, or the used entries are
 *    at least size.  A bumped picture is output (appended to plan->output
 *    and its slot made busy) and marked not needed; its entry is emptied
 *    when it is unused for reference (C.5.2.4).  Bumping also stops when
 *    no picture is needed for output (nothing is left to bump).
 * 3. When the used entries are still at least size (every one a
 *    reference not needed for output, which only a broken stream leaves),
 *    the entry with the smallest PicOrderCntVal that is in no Curr set is
 *    emptied (counted in evicted) until one is free.
 * 4. The plan: the Curr sets as slots, missing, the references.
 *
 * On success started becomes 1 and in_picture 1.  *plan is written in
 * full (setup_slot H265_DPB_NO_SLOT) on success and on the step 3
 * failure.
 *
 * Returns 0; EINVAL when in_picture is already 1 (a caller's mistake)
 * or the SPS's size is above H265_DPB_PICTURES (nothing changed in
 * either case), or when step 3 finds every used entry in a Curr set (the
 * buffer stays as steps 1 and 2 left it, with their outputs in plan,
 * and in_picture stays 0; the caller reads the outputs and skips the
 * picture).
 */
int h265_dpb_begin(struct h265_dpb *dpb, const struct h265sw_sps *sps, const struct h265sw_rps_pocs *pocs, int irap_no_rasl, int no_output_of_prior_pics, struct h265_dpb_plan *plan);

/*
 * Chooses the slot the current picture is decoded into: the lowest slot
 * held by no used entry and not busy.  Writes plan->setup_slot and
 * dpb->current.  Calling it again after success returns the same slot.
 *
 * Returns 0; EBUSY when every free slot is busy (nothing changed: the
 * caller finishes reading output pictures, releases their slots and calls
 * again); EINVAL when in_picture is 0.
 */
int h265_dpb_take_slot(struct h265_dpb *dpb, struct h265_dpb_plan *plan);

/*
 * Stores the decoded current picture and bumps after it (C.5.2.3).
 *
 * plan is the one begin and take_slot filled (setup_slot not
 * H265_DPB_NO_SLOT, else EINVAL); poc its PicOrderCntVal; pic_output
 * PicOutputFlag; id the caller's number for it.  Steps: when pic_output
 * is 1, latency + 1 for each picture needed for output whose poc is
 * greater than poc (it follows the current one in output order); the
 * current picture goes into an empty entry with slot setup_slot, marked
 * short-term (8.1.3 step 4), needed = pic_output, latency 0; then
 * bumping repeats while the pictures needed for output are more than
 * reorder, or latency_limited and one has latency >= max_latency.
 * Outputs are written to output[0..*count) (capacity H265_DPB_MAX_OUTPUT)
 * in output order, each slot made busy.  in_picture becomes 0 and current
 * H265_DPB_NO_SLOT.
 *
 * Returns 0; EINVAL when in_picture is 0, setup_slot is not the slot
 * take_slot gave, or no entry is empty (begin made room, so a caller's
 * mistake).  On EINVAL nothing changed and *count is 0.
 */
int h265_dpb_end(struct h265_dpb *dpb, const struct h265_dpb_plan *plan, int32_t poc, int pic_output, uint64_t id, struct h265_dpb_output *output, unsigned *count);

/*
 * Ends a picture that was not decoded (the caller skipped it or the
 * decode failed): the markings and outputs of begin stand (they are the
 * clause's, made before decoding), the current picture is not stored and
 * its slot is free again.  in_picture becomes 0.  Does nothing when
 * in_picture is 0.
 */
void h265_dpb_abort(struct h265_dpb *dpb);

/*
 * Outputs every picture needed for output in output order and empties
 * the buffer (the end of the stream, an end of sequence NAL unit, a
 * drain).  Writes output[0..*count) (capacity H265_DPB_MAX_OUTPUT), each
 * slot made busy.  started becomes 0.  Returns 0; EINVAL when in_picture
 * is 1 (nothing changed, *count 0).
 */
int h265_dpb_flush(struct h265_dpb *dpb, struct h265_dpb_output *output, unsigned *count);

/*
 * Empties the buffer without output (a seek); started becomes 0 and
 * in_picture 0.  busy is kept: slots the caller is reading stay busy
 * until released.
 */
void h265_dpb_clear(struct h265_dpb *dpb);

/*
 * Marks slot as read: clears its busy bit.  A slot below 0 or at or
 * above slots is ignored.  A slot whose entry is still used (a reference
 * picture that was output) stays held by the entry; it becomes free when
 * the entry is emptied.
 */
void h265_dpb_release(struct h265_dpb *dpb, int slot);

#endif
