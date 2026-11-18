/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoded frames of the software H.264 decoder and the pool they come
 * from.
 *
 * A frame holds the samples of one decoded picture in three planes
 * (4:2:0, 8 bits) and the motion of each of its macroblocks, which later
 * pictures read for the direct prediction of B slices (8.4.1.2) and which
 * the deblocking filter of the frame itself reads (8.7.2.1).
 *
 * Frames are counted references: the picture being decoded, the decoded
 * picture buffer and the output queue each hold one.  The last unref
 * returns a frame to its pool's free list.  Frames never leave the
 * decoder: libmedia's back end copies the samples out (into its own
 * picture) before it unrefs an output frame.  A pool is used from one
 * thread at a time and has no lock.
 */

#ifndef LIBMEDIA_H264SW_FRAME_H
#define LIBMEDIA_H264SW_FRAME_H

#include "h264sw.h"

struct h264sw_frame_pool;

/* The most frames a pool makes (see struct h264sw_frame_pool's limit). */
#define H264SW_FRAME_POOL_MAX (2U * H264SW_MAX_DPB_FRAMES + 4U)

/*
 * The motion of one macroblock, kept with its frame.
 *
 * mv[list][blk] is the motion vector of list 0 or 1 for luma 4x4 block
 * blk (luma4x4BlkIdx, 6.4.3), in quarter luma samples, [0] horizontal
 * and [1] vertical.  ref_idx[list][part] is the reference index of 8x8
 * quadrant part (mbPartIdx of an 8x8 partitioning, 0 top-left, 1
 * top-right, 2 bottom-left, 3 bottom-right), -1 when the list is not
 * used (predFlagLX 0).  ref_id[list][part] is the id of the frame that
 * reference index named when this macroblock was decoded (0 when not
 * used): later pictures compare pictures, not indices (8.4.1.2.3
 * refPicCol, 8.7.2.1 "different reference pictures").  intra is 1 for a
 * macroblock coded in an intra mode (I or PCM, in any slice type).
 *
 * The motion vectors of an unused list are 0.
 */
struct h264sw_motion {
	int16_t mv[2][16][2];
	int8_t ref_idx[2][4];
	uint32_t ref_id[2][4];
	uint8_t intra;
};

/*
 * One decoded frame.
 *
 * plane[0..2] are Y, Cb and Cr; stride[n] is the bytes from one row of
 * plane n to the next; width and height are the luma size in samples
 * (PicWidthInMbs * 16 and FrameHeightInMbs * 16), the chroma planes are
 * half of each.  The planes have no border: the inter prediction clamps
 * its sample positions (8.4.2.2.1, 8-239 and 8-240; 8.4.2.2.2, 8-262 to
 * 8-269).
 *
 * The cropping window (crop_x, crop_y, crop_width, crop_height, luma
 * samples) is what is shown.  motion has size_mbs entries.
 *
 * id is the frame's identity among the frames of its pool (never 0, and
 * never reused while a motion record may name it: it is a counter of
 * the pool that is incremented each time a frame is handed out).
 *
 * The values a picture leaves for later pictures: poc (PicOrderCnt of the
 * frame, Min( TopFieldOrderCnt, BottomFieldOrderCnt ), 8-1), top_poc and
 * bottom_poc, frame_num, idr, and time_us, the presentation time the
 * caller gave the access unit.  concealed is the number of macroblocks
 * the decoder could not decode and filled in instead; non_existing is 1
 * for a frame that stands for a gap in frame_num (8.2.5.2), whose
 * samples are a flat 128 and whose motion records every macroblock as
 * intra (a broken stream that uses it as colPic then gets zero vectors).
 *
 * What the sequence says of how to show the frame, copied from the
 * active SPS when the picture starts (E.2.1): sar_width and sar_height
 * (0 and 0 when unknown), signal_present with full_range, and
 * colour_present with matrix_coefficients.
 */
struct h264sw_frame {
	struct h264sw_frame_pool *pool;
	struct h264sw_frame *next_free;
	unsigned refs;
	uint32_t id;

	uint8_t *plane[3];
	size_t stride[3];
	unsigned width;
	unsigned height;
	unsigned crop_x;
	unsigned crop_y;
	unsigned crop_width;
	unsigned crop_height;

	struct h264sw_motion *motion;
	unsigned size_mbs;

	int32_t poc;
	int32_t top_poc;
	int32_t bottom_poc;
	uint32_t frame_num;
	int idr;
	int64_t time_us;
	unsigned concealed;
	int non_existing;

	unsigned sar_width;
	unsigned sar_height;
	int signal_present;
	int full_range;
	int colour_present;
	unsigned matrix_coefficients;
};

/*
 * The frames of one sequence's size.
 *
 * All frames of a pool have the same size; a new sequence of another
 * size makes a new pool.  allocated counts the frames made, free holds
 * the ones no one refers to, next_id is the id given next.  limit is the
 * most frames the pool makes; a get beyond it fails, so a stream cannot
 * make the decoder allocate without bound.  The decoder sets it to
 * 2 * dpb_frames + 4: the frames in the DPB, as many again bumped from
 * a full DPB into the output queue at once (an IDR picture), the frame
 * being decoded, the decoder's last output, the frame the caller holds
 * while it copies one out, and one spare.
 *
 * The pool owns every frame it made.  h264sw_frame_pool_destroy frees the
 * free frames and marks the pool dead; a frame still referenced is freed
 * by its last unref, and the pool's own memory with its last frame.
 */
struct h264sw_frame_pool {
	unsigned width;
	unsigned height;
	unsigned size_mbs;
	unsigned crop_x;
	unsigned crop_y;
	unsigned crop_width;
	unsigned crop_height;
	unsigned allocated;
	unsigned limit;
	uint32_t next_id;
	int dead;
	struct h264sw_frame *free;
};

/*
 * Makes a pool for frames of width_mbs by height_mbs macroblocks.
 *
 * width_mbs and height_mbs are 1..H264SW_MAX_SIDE_MBS and their product
 * at most H264SW_MAX_FRAME_MBS; the crop window is in luma samples and
 * inside the frame; limit is 1..H264SW_FRAME_POOL_MAX.  Allocates
 * nothing but the pool.  Returns 0 and *pool, EINVAL for a size out of
 * range, ENOMEM.
 */
int h264sw_frame_pool_create(unsigned width_mbs, unsigned height_mbs, unsigned crop_x, unsigned crop_y, unsigned crop_width, unsigned crop_height, unsigned limit, struct h264sw_frame_pool **pool);

/*
 * Takes a frame from the pool with one reference.
 *
 * Reuses a free frame or allocates one (planes and motion in one
 * allocation each is acceptable).  The samples and the motion of a reused
 * frame are left as they were (the decoder writes every macroblock or
 * conceals it, and copies the motion of every macroblock at the end of
 * the picture); a new frame's memory is cleared, its motion all intra
 * with ref_idx -1.  The per-picture fields are cleared and id is set from
 * next_id.  Returns NULL when allocated has reached limit or the
 * allocation fails (the decoder reports ENOMEM for both).
 */
struct h264sw_frame *h264sw_frame_get(struct h264sw_frame_pool *pool);

/* Adds a reference to a frame. */
void h264sw_frame_ref(struct h264sw_frame *frame);

/*
 * Drops a reference; the last one returns the frame to its pool's free
 * list, or frees it when the pool is dead (and frees the pool with its
 * last frame).  NULL is ignored.
 */
void h264sw_frame_unref(struct h264sw_frame *frame);

/* Fills a frame's three planes with 128 and records every macroblock's motion as intra with ref_idx -1, mv 0 and ref_id 0 (a non-existing frame). */
void h264sw_frame_fill_grey(struct h264sw_frame *frame);

/*
 * Frees a pool's free frames and marks it dead.
 *
 * Frames still referenced stay valid until their last unref.  NULL is
 * ignored.
 */
void h264sw_frame_pool_destroy(struct h264sw_frame_pool *pool);

#endif
