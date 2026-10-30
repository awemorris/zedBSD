/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The software H.264 decoder as a whole: it takes NAL units one at a
 * time and gives decoded frames in output order.
 *
 * The flow for each NAL unit (8.1, 7.4.1.2):
 *
 *  1. h264sw_nal_parse: the header and the RBSP (into the decoder's
 *     buffer).
 *  2. A NAL unit that starts a new access unit (7.4.1.2.3: an access unit
 *     delimiter, SPS, PPS, SEI, types 15 to 18, or the first slice of a
 *     new picture by h264sw_slice_new_picture) and an end of sequence
 *     first finish the picture in progress (step 6).  Types 14, 20 and 21
 *     (prefix and extension NAL units of SVC and MVC streams, which
 *     come before or among the slices of the base layer, 7.4.1.2.3 NOTE
 *     1) do not: they are ignored.
 *  3. SPS and PPS: h264sw_ps_read_sps and h264sw_ps_read_pps.  Others
 *     that carry no picture (SEI, AUD, end of sequence, filler, types
 *     13 to 21 other than slices) are ignored after step 2.
 *  4. A slice (types 1 and 5): its header (h264sw_slice_read); slices of
 *     a redundant picture (redundant_pic_cnt above 0) are dropped.  The
 *     SPS: a picture that starts decoding (an IDR picture, or the first
 *     picture after the start or a reset) activates the SPS its PPS names:
 *     copied into sps with its generation; when the size or dpb_frames
 *     differ from the active ones, the old frames are output
 *     (h264sw_dpb_remove with changed 1) and a new pool, DPB and mbs
 *     arrays are made.  Any other slice whose PPS names another SPS id,
 *     or the same id with another generation, is dropped with EINVAL
 *     (7.4.1.2.1: an SPS changes only at an IDR picture), and so is a
 *     slice whose first_mb_in_slice is not below the active size_mbs or
 *     whose PPS differs from the picture's first slice's: nothing sized
 *     by the active SPS is indexed by another's values.  The first slice
 *     of a picture starts it: the gaps in frame_num (h264sw_dpb_gaps),
 *     the order count (h264sw_poc_compute), a frame from the pool (with
 *     the display fields of the SPS), the picture context (every struct
 *     h264sw_mb entry reset with slice -1, slice_count 0, the scaling
 *     functions).  A slice that would make slice_count exceed
 *     slices_capacity is dropped with EINVAL.
 *  5. Each slice: its slice parameters recorded, the reference lists
 *     (h264sw_reflist_build), the weights and the direct setup, CABAC
 *     started when used, and h264sw_slice_data.
 *  6. Finishing a picture: the macroblocks no slice decoded are concealed
 *     (each copied from the same place of RefPicList0[ 0 ] of the last
 *     slice when there was one, else from the frame output last, else
 *     grey; their entries set as an intra macroblock of the last slice),
 *     the deblocking (h264sw_deblock_picture), the motion copied into
 *     the frame, h264sw_poc_finish and its order counts (changed by an
 *     operation 5) copied into frame->poc, top_poc and bottom_poc, then
 *     h264sw_dpb_remove and h264sw_dpb_store (which may output frames).
 *
 * Data partitioning (types 2 to 4) is refused with ENOTSUP.
 *
 * Start and recovery: at the start and after h264sw_decoder_reset, the
 * decoder waits for an IDR picture or a picture whose first slice is an
 * I slice; the pictures before it are dropped (counted in dropped).
 * When it starts at a non-IDR picture, the pictures after it in decoding
 * order whose PicOrderCnt is below the start picture's (the leading
 * pictures of an open GOP, which predict from pictures before the start)
 * are decoded, so that the references stay right, but not output (the
 * DPB entry is marked not needed for output; counted in dropped), until
 * the next IDR picture or operation 5.  A P or B slice whose references
 * are missing is decoded with stand-ins (h264sw_reflist_build) and its
 * frame's concealed count says so.
 *
 * An error inside a slice ends that slice; the decoder goes on with the
 * next one and conceals what is left.  An error that leaves no picture
 * to decode (a slice naming an absent PPS, a broken header) drops the
 * slice.  No error leaves the decoder unable to take the next IDR
 * picture; only h264sw_decoder_destroy frees it.
 */

#ifndef LIBMEDIA_H264SW_DECODER_H
#define LIBMEDIA_H264SW_DECODER_H

#include "h264sw.h"
#include "h264sw-context.h"
#include "h264sw-dpb.h"
#include "h264sw-frame.h"
#include "h264sw-poc.h"
#include "h264sw-ps.h"
#include "h264sw-slice.h"

/*
 * What a decoder has done, for the log and the tests: pictures decoded,
 * frames output, macroblocks concealed, slices that ended in an error,
 * pictures dropped while waiting for a start, and NAL units refused as
 * unsupported.
 */
struct h264sw_decoder_stats {
	uint64_t pictures;
	uint64_t output;
	uint64_t concealed_mbs;
	uint64_t slice_errors;
	uint64_t dropped;
	uint64_t unsupported;
};

/*
 * One decoder.
 *
 * ps: the parameter sets seen.  rbsp: the RBSP of the NAL unit being
 * read (rbsp_capacity bytes, grown to the largest NAL unit; a NAL unit
 * above 64 MiB is refused with EINVAL).
 *
 * The sequence: has_sequence once an SPS was activated; sps the copy of
 * the active SPS (so a new SPS of the same id does not change it under
 * a picture) and sps_generation its store generation; pool, dpb, output
 * and poc the frames, the decoded picture buffer, the output queue and
 * the order count state of the sequence.
 *
 * The picture in progress: in_picture; first the header of its first
 * slice (the marking and the order count use it), previous the header of
 * its last slice (for h264sw_slice_new_picture), header the slice being
 * decoded; poc its order counts; time_us the time of the NAL unit of its
 * first slice; picture and slice the contexts of h264sw-context.h, with
 * mbs (mbs_capacity entries) and slices (slices_capacity entries) the
 * arrays they point to, allocated for the sequence's size.
 *
 * waiting: 1 at the start and after a reset, until a picture can start
 * decoding (an IDR picture or an I slice first).  leading: 1 after a
 * start at a non-IDR picture, until the next IDR picture or operation 5;
 * start_poc the start picture's PicOrderCnt (the leading pictures' rule
 * above).  last_output: the frame output last (with a reference), the
 * concealment's fall-back.  decode_order_times: set by the caller when
 * the times it gives are in decoding order (h264sw_decoder_output).
 * stats: the counters.
 */
struct h264sw_decoder {
	struct h264sw_ps_store ps;
	uint8_t *rbsp;
	size_t rbsp_capacity;

	int has_sequence;
	struct h264sw_sps sps;
	unsigned sps_generation;
	struct h264sw_frame_pool *pool;
	struct h264sw_dpb dpb;
	struct h264sw_output_queue output;
	struct h264sw_poc_state poc;

	int in_picture;
	struct h264sw_slice_header first;
	struct h264sw_slice_header previous;
	struct h264sw_slice_header header;
	struct h264sw_poc picture_poc;
	int64_t time_us;
	struct h264sw_picture_ctx picture;
	struct h264sw_slice_ctx slice;
	struct h264sw_mb *mbs;
	unsigned mbs_capacity;
	struct h264sw_slice_params *slices;
	unsigned slices_capacity;

	int waiting;
	int leading;
	int32_t start_poc;
	struct h264sw_frame *last_output;
	int decode_order_times;
	struct h264sw_decoder_stats stats;
};

/* Makes a decoder in its start state.  Returns 0 and *decoder, or ENOMEM. */
int h264sw_decoder_create(struct h264sw_decoder **decoder);

/*
 * Frees a decoder: the picture in progress is dropped, the DPB and the
 * output queue cleared, the pool destroyed.  Frames the caller still
 * holds stay valid until it unrefs them.  NULL is ignored.
 */
void h264sw_decoder_destroy(struct h264sw_decoder *decoder);

/*
 * Decodes one NAL unit.
 *
 * nal and size are the NAL unit's bytes after its start code (from
 * vcodec_nal_find); time_us is the presentation time of the packet it
 * came in (kept for a picture whose first slice it is).  Runs the flow
 * above; frames that become ready for output are appended to the output
 * queue.
 *
 * The caller takes every output frame (h264sw_decoder_output) before it
 * gives the next NAL unit: when the queue is not empty this returns
 * EBUSY and does nothing.
 *
 * Returns 0; EINVAL for a NAL unit or a slice that could not be used (the
 * decoder has recovered: the next NAL unit can be given); ENOTSUP for a
 * stream outside the decoder's range (an SPS or PPS refused as
 * unsupported, a slice of an unsupported kind; a caller that has not yet
 * had a frame may switch to another decoder); ENOMEM.
 */
int h264sw_decoder_nal(struct h264sw_decoder *decoder, const uint8_t *nal, size_t size, int64_t time_us);

/*
 * Finishes the picture in progress, for a caller that knows for certain
 * that the access unit has ended, so the picture does not wait for the
 * next NAL unit.  A caller that is not certain (a container whose
 * packets may split an access unit, as MPEG-TS can) does not call it: a
 * picture finished early would see its next slice start another picture.
 * Same returns as h264sw_decoder_nal; 0 when no picture is in progress.
 */
int h264sw_decoder_end_access_unit(struct h264sw_decoder *decoder);

/*
 * Ends the stream: finishes the picture in progress and outputs every
 * frame still waiting in the DPB (h264sw_dpb_flush), in output order.
 * The decoder then waits for a new start as after a reset.
 */
int h264sw_decoder_flush(struct h264sw_decoder *decoder);

/*
 * Takes the next output frame with its reference (the caller unrefs it), or NULL when none is ready.
 *
 * With decode_order_times 1 the times the caller gave are in decoding
 * order (a container without composition times), so they belong to the
 * output order, not to the pictures: the frame taken gets the smallest
 * time_us among itself, the frames still in the output queue and the DPB
 * frames still needed for output, and the frame that had that time gets
 * the taken frame's time, so the set of times is kept.  The frame taken
 * also becomes last_output (the decoder's own reference moves to it).
 */
struct h264sw_frame *h264sw_decoder_output(struct h264sw_decoder *decoder);

/*
 * Drops the picture in progress, the DPB, the output queue and the order
 * count state, for a seek; the parameter sets and the frame pool are
 * kept.  The decoder waits for a new start.
 */
void h264sw_decoder_reset(struct h264sw_decoder *decoder);

#endif
