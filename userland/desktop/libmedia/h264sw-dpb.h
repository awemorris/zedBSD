/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoded picture buffer of the software H.264 decoder: the marking
 * of reference pictures (8.2.5), the frames that stand for gaps in
 * frame_num (8.2.5.2), and the storage and the output order of the
 * output order conformance of C.4 (C.4.2 to C.4.5.3, the "bumping"
 * process).
 *
 * Frames only: every entry is a decoded frame or a non-existing frame.
 * The steps of C.4.1 for one picture are, in this order:
 * h264sw_dpb_gaps (C.4.2), the decoding of the picture (C.4.3),
 * h264sw_dpb_remove (C.4.4, which runs the marking of 8.2.5), and
 * h264sw_dpb_store (C.4.5).
 *
 * The frames the bumping process outputs are appended to an output
 * queue, in output order, each with a reference the queue holds; the
 * decoder takes them from there.
 */

#ifndef LIBMEDIA_H264SW_DPB_H
#define LIBMEDIA_H264SW_DPB_H

#include "h264sw.h"
#include "h264sw-frame.h"
#include "h264sw-poc.h"
#include "h264sw-ps.h"
#include "h264sw-slice.h"

/* The marking of an entry (8.2.5). */
#define H264SW_REF_UNUSED 0U
#define H264SW_REF_SHORT 1U
#define H264SW_REF_LONG 2U

/*
 * The most frames one picture's steps can output: every frame of a full
 * buffer, the current one, and a margin for the gap frames that pass
 * through a full buffer.
 */
#define H264SW_OUTPUT_MAX (2U * H264SW_MAX_DPB_FRAMES + 2U)

/*
 * One frame buffer of the DPB.
 *
 * frame is NULL for an empty buffer; the entry holds one reference to
 * it.  reference is H264SW_REF_*; long_term_frame_idx is
 * LongTermFrameIdx for a long-term frame; output_needed is 1 while the
 * frame is "needed for output" (never for a non-existing frame).
 * frame_num is the frame's frame_num (equal to frame->frame_num, kept
 * here for the marking's PicNum arithmetic).
 */
struct h264sw_dpb_entry {
	struct h264sw_frame *frame;
	unsigned reference;
	uint32_t long_term_frame_idx;
	int output_needed;
	uint32_t frame_num;
};

/*
 * The frames output by the bumping process, oldest first, waiting for
 * the decoder to take them.  Each holds one frame reference.
 */
struct h264sw_output_queue {
	struct h264sw_frame *frame[H264SW_OUTPUT_MAX];
	unsigned head;
	unsigned count;
};

/*
 * The decoded picture buffer.
 *
 * size is the number of frame buffers (the SPS's dpb_frames, 1..16);
 * max_num_ref_frames the SPS's; reorder the most frames that may wait
 * for output before one must be output (max_num_reorder_frames when the
 * VUI sends it, else size).  max_long_term_frame_idx is
 * MaxLongTermFrameIdx, -1 for "no long-term frame indices".  Owned by
 * the decoder, embedded in it.
 */
struct h264sw_dpb {
	unsigned size;
	unsigned max_num_ref_frames;
	unsigned reorder;
	int32_t max_long_term_frame_idx;
	struct h264sw_dpb_entry entry[H264SW_MAX_DPB_FRAMES];
};

/* Empties a queue (its frames must have been taken or released before). */
void h264sw_output_init(struct h264sw_output_queue *queue);

/* Takes the oldest frame of the queue with its reference (the caller unrefs it), or NULL when empty. */
struct h264sw_frame *h264sw_output_take(struct h264sw_output_queue *queue);

/* Drops every frame of the queue (unref), for a seek or a close. */
void h264sw_output_clear(struct h264sw_output_queue *queue);

/*
 * Prepares an empty DPB for a sequence.
 *
 * sps gives dpb_frames, max_num_ref_frames and max_num_reorder_frames.
 * Every entry is empty and max_long_term_frame_idx is -1.  The DPB must
 * be empty (h264sw_dpb_clear) when it is prepared again.
 */
void h264sw_dpb_init(struct h264sw_dpb *dpb, const struct h264sw_sps *sps);

/*
 * Drops every entry without output (unref), for a seek, a close, or an
 * IDR picture with no_output_of_prior_pics_flag (C.4.4 step 3).
 */
void h264sw_dpb_clear(struct h264sw_dpb *dpb);

/*
 * Outputs every frame still needed for output and empties the DPB
 * (the bumping process repeated, C.4.5.3), for the end of a stream.
 *
 * Returns 0, or ENOBUFS when the queue fills (it cannot when the caller
 * empties it between pictures).
 */
int h264sw_dpb_flush(struct h264sw_dpb *dpb, struct h264sw_output_queue *queue);

/*
 * Stores the non-existing frames of a gap in frame_num (8.2.5.2, C.4.2).
 *
 * Called before a picture is decoded, with its first slice header and
 * the SPS.  When frame_num is neither PrevRefFrameNum nor
 * ( PrevRefFrameNum + 1 ) % MaxFrameNum (7.4.3) and the picture is not
 * IDR: with gaps_in_frame_num_value_allowed_flag 1, for each
 * UnusedShortTermFrameNum (7-24) a frame is taken from pool, filled grey,
 * marked non-existing, given that frame_num, marked short-term by the
 * sliding window (8.2.5.3) and stored as C.4.2 says (bumping when full).
 * Only the last max_num_ref_frames of a longer gap are made: the sliding
 * window would remove the earlier ones at once, so the result is the
 * same.  With the flag 0 the gap is an error of the stream (lost
 * pictures, 8.2.5.2 informative): the same frames are made, for
 * concealment, and 0 is returned all the same.
 *
 * poc is the decoder's order count state (prev_ref_frame_num is taken
 * from it and moved past each frame made).  Returns 0, ENOMEM when the
 * pool gives no frame, ENOBUFS when the queue fills.
 */
int h264sw_dpb_gaps(struct h264sw_dpb *dpb, const struct h264sw_sps *sps, const struct h264sw_slice_header *header, struct h264sw_poc_state *poc, struct h264sw_frame_pool *pool, struct h264sw_output_queue *queue);

/*
 * Marks the references and removes frames before the current picture is stored (C.4.4, 8.2.5).
 *
 * header is the current picture's first slice header, current the
 * decoded frame (its frame_num and poc set).  For an IDR picture: every
 * entry is marked unused (8.2.5.1); when no_output_of_prior_pics_flag is
 * 1, or inferred 1 because the size or dpb_frames changed from the
 * previous sequence (changed is 1 then), the DPB is cleared without
 * output.  For another picture: the marking process of 8.2.5.1 runs for
 * the current picture when it is a reference picture: the sliding window
 * (8.2.5.3) or the adaptive operations 1 to 6 (8.2.5.4, with picNumX of
 * 8-39 and the long-term rules), in their order.  Then frames neither
 * needed for output nor used for reference are removed.  For an IDR
 * picture without no_output_of_prior_pics_flag, or a picture with
 * operation 5, every remaining frame is output by bumping and the DPB is
 * emptied.
 *
 * The marking of the current picture itself (short-term, or long-term by
 * operation 6 or long_term_reference_flag) is recorded for
 * h264sw_dpb_store in *current_marking (H264SW_REF_*) and
 * *current_long_idx.
 *
 * An operation that names no picture (a picNumX or LongTermPicNum not in
 * the DPB) is skipped: the stream is broken but the rest still applies.
 * Returns 0, ENOBUFS when the queue fills.
 */
int h264sw_dpb_remove(struct h264sw_dpb *dpb, const struct h264sw_sps *sps, const struct h264sw_slice_header *header, const struct h264sw_frame *current, int changed, unsigned *current_marking, uint32_t *current_long_idx, struct h264sw_output_queue *queue);

/*
 * Stores the current picture (C.4.5).
 *
 * marking and long_idx are what h264sw_dpb_remove gave (H264SW_REF_UNUSED
 * for a non-reference picture).  A reference picture (C.4.5.1): bumping
 * until a buffer is empty, then stored with its marking and needed for
 * output.  A non-reference picture (C.4.5.2): when a buffer is empty it
 * is stored needed for output; when none is and its PicOrderCnt is lower
 * than that of every frame needed for output, it is output at once
 * without being stored; otherwise bumping until a buffer is empty, then
 * stored.  The DPB takes its own reference to current.
 *
 * Then, as the output order allows and to keep the latency low, frames
 * are output by bumping while more than reorder frames are needed for
 * output.  A reference picture that the DPB cannot hold (more references
 * than max_num_ref_frames after the marking: a broken stream) has its
 * oldest short-term reference marked unused first.
 *
 * Returns 0, ENOBUFS when the queue fills.
 */
int h264sw_dpb_store(struct h264sw_dpb *dpb, struct h264sw_frame *current, unsigned marking, uint32_t long_idx, struct h264sw_output_queue *queue);

/*
 * Runs the bumping process once (C.4.5.3).
 *
 * Outputs the frame needed for output with the smallest PicOrderCnt
 * (appends it to queue with a reference), marks it not needed for
 * output, and empties its buffer when it is not used for reference.
 * Returns 1 when a frame was output, 0 when none is needed for output,
 * ENOBUFS (as a negative value: -ENOBUFS) when the queue is full.
 */
int h264sw_dpb_bump(struct h264sw_dpb *dpb, struct h264sw_output_queue *queue);

/* Counts the entries marked as used for reference (short and long). */
unsigned h264sw_dpb_references(const struct h264sw_dpb *dpb);

#endif
