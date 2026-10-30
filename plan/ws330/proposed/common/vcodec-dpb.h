/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoded picture buffer of libmedia's software video decoders, the
 * part both codecs share: the picture storage buffers with their marking
 * ("unused", "used for short-term reference", "used for long-term
 * reference") and their "needed for output" state, the removal of
 * buffers that are neither, the bumping process that outputs the picture
 * with the smallest order count (ITU-T H.264 C.4.5.3, ITU-T H.265
 * C.5.2.4: the same process), and the flush and clear of the whole
 * buffer.
 *
 * What each codec decides on top stays with it: when to bump and how to
 * mark (H.264: the sliding window and the memory management operations
 * of 8.2.5, the gaps in frame_num, C.4.4 and C.4.5; H.265: the reference
 * picture set of 8.3.2, the generated pictures of 8.3.3, C.5.2.2 and
 * C.5.2.3 with sps_max_num_reorder_pics and the latency count), and how
 * to free a buffer when a broken stream fills the DPB with references.
 * The codec fields of an entry (frame_num and long_term_idx for H.264,
 * latency for H.265, generated for both) are carried here but read only
 * by the codec.
 *
 * Frames are struct vcodec_frame (or a codec's frame that embeds it as
 * its first member); bumped frames go to a struct vcodec_output_queue.
 */

#ifndef LIBMEDIA_VCODEC_DPB_H
#define LIBMEDIA_VCODEC_DPB_H

#include "vcodec.h"
#include "vcodec-frame.h"
#include "vcodec-output.h"

/* The most picture storage buffers (H.264 A.3.1 h) and H.265 A.4.2 both give at most 16). */
#define VCODEC_DPB_MAX 16U

/* The marking of an entry (H.264 8.2.5, H.265 8.3.2). */
#define VCODEC_REF_UNUSED 0U
#define VCODEC_REF_SHORT 1U
#define VCODEC_REF_LONG 2U

/*
 * One picture storage buffer.
 *
 * frame is NULL for an empty buffer; the entry holds one reference to it
 * otherwise.  reference is VCODEC_REF_*; output_needed 1 while "needed
 * for output"; poc the picture's order count (H.264 PicOrderCnt of the
 * frame, H.265 PicOrderCntVal), the key of the bumping.  generated 1 for
 * a picture the decoder made rather than decoded (H.264 a non-existing
 * frame of a gap, H.265 a generated unavailable picture or a stand-in):
 * never needed for output.
 *
 * Codec fields: frame_num (H.264 frame_num, for FrameNumWrap),
 * long_term_idx (H.264 LongTermFrameIdx), latency (H.265
 * PicLatencyCount).  A codec leaves the others 0.
 */
struct vcodec_dpb_entry {
	struct vcodec_frame *frame;
	unsigned reference;
	int output_needed;
	int32_t poc;
	int generated;
	uint32_t frame_num;
	uint32_t long_term_idx;
	uint32_t latency;
};

/*
 * The buffers: size of entry[] in use (1..VCODEC_DPB_MAX, the sequence's
 * DPB size: H.264 dpb_frames, H.265 sps_max_dec_pic_buffering_minus1 + 1).
 * Embedded in the codec's DPB structure.
 */
struct vcodec_dpb {
	unsigned size;
	struct vcodec_dpb_entry entry[VCODEC_DPB_MAX];
};

/* Makes every buffer empty and sets size (1..VCODEC_DPB_MAX); the DPB must hold no frame (vcodec_dpb_clear first). */
void vcodec_dpb_init(struct vcodec_dpb *dpb, unsigned size);

/* Returns the index of an empty buffer, or -1 when every buffer holds a picture. */
int vcodec_dpb_find_empty(const struct vcodec_dpb *dpb);

/* Counts the buffers that hold a picture (the DPB fullness). */
unsigned vcodec_dpb_fullness(const struct vcodec_dpb *dpb);

/* Counts the pictures needed for output. */
unsigned vcodec_dpb_needed(const struct vcodec_dpb *dpb);

/* Counts the pictures marked as used for reference (short or long). */
unsigned vcodec_dpb_references(const struct vcodec_dpb *dpb);

/*
 * Stores frame in the empty buffer index with a new reference: reference,
 * output_needed, poc and generated as given, the codec fields 0 (the
 * caller sets them after).  Returns 0; EINVAL when the buffer is not
 * empty or index is out of range.
 */
int vcodec_dpb_put(struct vcodec_dpb *dpb, unsigned index, struct vcodec_frame *frame, unsigned reference, int output_needed, int32_t poc, int generated);

/* Empties buffer index (drops its frame reference), whatever its state. */
void vcodec_dpb_empty(struct vcodec_dpb *dpb, unsigned index);

/* Empties every buffer that is not needed for output and unused for reference (H.264 C.4.4 last step, H.265 C.5.2.2). */
void vcodec_dpb_remove_unused(struct vcodec_dpb *dpb);

/*
 * The bumping process once (H.264 C.4.5.3, H.265 C.5.2.4): the picture
 * needed for output with the smallest poc (the first such entry on a
 * tie, which a conforming stream never has) is appended to queue with a
 * reference and marked not needed for output; its buffer is emptied when
 * it is unused for reference.  Returns 1 when a picture was output, 0
 * when none is needed for output, -ENOBUFS when the queue is full
 * (nothing changed).
 */
int vcodec_dpb_bump(struct vcodec_dpb *dpb, struct vcodec_output_queue *queue);

/*
 * Outputs every picture needed for output by repeated bumping, then
 * empties every buffer (the end of a stream, H.264 C.4.5.3 at the end,
 * H.265 end of sequence and drain).  Returns 0 or ENOBUFS.
 */
int vcodec_dpb_flush(struct vcodec_dpb *dpb, struct vcodec_output_queue *queue);

/* Empties every buffer without output (a seek, a close, no_output_of_prior_pics). */
void vcodec_dpb_clear(struct vcodec_dpb *dpb);

/*
 * Gives the frames still needed for output, in entry order, into
 * frames[0..capacity) and returns their count: the waiting set of
 * vcodec_output_retime.
 */
unsigned vcodec_dpb_waiting(const struct vcodec_dpb *dpb, struct vcodec_frame **frames, unsigned capacity);

#endif
