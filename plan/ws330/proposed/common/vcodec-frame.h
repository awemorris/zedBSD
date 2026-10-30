/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoded frames of libmedia's software video codecs and the pool
 * they come from, the part every codec shares: three 8-bit planes of a
 * 4:2:0 picture, a counted reference, an identity, the order count and
 * time of the picture, how to show it, and one block of codec data (the
 * motion a later picture reads).
 *
 * A codec keeps its own per-frame fields by embedding struct
 * vcodec_frame as the first member of its own frame structure and making
 * the pool with that structure's size (frame_bytes): a pointer to the
 * codec's frame and a pointer to its first member are interchangeable
 * (ISO C 6.7.2.1).  The pool allocates frame_bytes per frame and
 * side_bytes of codec data next to it.
 *
 * Frames are counted references: the picture being decoded, the decoded
 * picture buffer, the output queue and the caller each hold one.  The
 * last unref returns a frame to its pool's free list.  A pool is used
 * from one thread at a time and has no lock.
 */

#ifndef LIBMEDIA_VCODEC_FRAME_H
#define LIBMEDIA_VCODEC_FRAME_H

#include "vcodec.h"

struct vcodec_frame_pool;

/*
 * The most frames a pool makes: twice the largest decoded picture buffer
 * of either codec (16 frames) and four more (the picture being decoded,
 * the last output, the frame a caller copies out, one spare).
 */
#define VCODEC_FRAME_POOL_MAX 36U

/*
 * One decoded frame.
 *
 * plane[0..2] are Y, Cb and Cr; stride[n] is the bytes from one row of
 * plane n to the next; width and height are the luma size in samples
 * of the decoded picture (even numbers); the chroma planes are half of
 * each.  The planes have no border: prediction clamps its positions.
 * The cropping window (crop_x, crop_y, crop_width, crop_height, luma
 * samples) is what is shown.
 *
 * side points to the pool's side_bytes of codec data for this frame (NULL
 * when side_bytes is 0).
 *
 * id is the frame's identity among the frames of its pool: never 0, a
 * counter of the pool incremented each time a frame is handed out, so a
 * motion record that names a frame by id never names a later frame.
 *
 * poc is the picture's order count (H.264 PicOrderCnt of the frame,
 * H.265 PicOrderCntVal); time_us the presentation time the caller gave
 * the access unit; concealed the number of blocks the decoder could not
 * decode and filled in.
 *
 * How to show it, copied from the active sequence parameter set when the
 * picture starts (H.264 E.2.1, H.265 E.3.1): sar_width and sar_height
 * (0 and 0 when unknown), signal_present with full_range, and
 * colour_present with matrix_coefficients.
 *
 * pool, next_free and refs belong to the pool's code.
 */
struct vcodec_frame {
	struct vcodec_frame_pool *pool;
	struct vcodec_frame *next_free;
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

	void *side;

	int32_t poc;
	int64_t time_us;
	unsigned concealed;

	unsigned sar_width;
	unsigned sar_height;
	int signal_present;
	int full_range;
	int colour_present;
	unsigned matrix_coefficients;
};

/*
 * Prepares the codec data of a frame the pool has just allocated (not a
 * reused one): for example, every block's motion recorded as intra.  The
 * memory is cleared to 0 before it is called.
 */
typedef void vcodec_side_init_fn(void *side, size_t side_bytes);

/*
 * The frames of one sequence's size.
 *
 * All frames of a pool have the same size; a new sequence of another
 * size makes a new pool.  frame_bytes is the size of the codec's frame
 * structure (at least sizeof( struct vcodec_frame )); side_bytes the
 * codec data per frame; side_init prepares new codec data (NULL: left
 * cleared).  allocated counts the frames made, free holds the ones no
 * one refers to, next_id is the id given next.  limit is the most frames
 * the pool makes; a get beyond it fails, so a stream cannot make the
 * decoder allocate without bound.
 *
 * The pool owns every frame it made.  vcodec_frame_pool_destroy frees
 * the free frames and marks the pool dead; a frame still referenced is
 * freed by its last unref, and the pool's own memory with its last
 * frame.
 */
struct vcodec_frame_pool {
	unsigned width;
	unsigned height;
	unsigned crop_x;
	unsigned crop_y;
	unsigned crop_width;
	unsigned crop_height;
	size_t frame_bytes;
	size_t side_bytes;
	vcodec_side_init_fn *side_init;
	unsigned allocated;
	unsigned limit;
	uint32_t next_id;
	int dead;
	struct vcodec_frame *free;
};

/*
 * Makes a pool for frames of width by height luma samples.
 *
 * width and height are even, 2..65534, and width * height at most the
 * caller's own limit (the codec checks its level limit before); the crop
 * window lies inside the frame; frame_bytes >= sizeof( struct
 * vcodec_frame ); limit 1..VCODEC_FRAME_POOL_MAX.  Allocates nothing but
 * the pool.  Returns 0 and *pool, EINVAL for a value out of range,
 * ENOMEM.
 */
int vcodec_frame_pool_create(unsigned width, unsigned height, unsigned crop_x, unsigned crop_y, unsigned crop_width, unsigned crop_height, size_t frame_bytes, size_t side_bytes, vcodec_side_init_fn *side_init, unsigned limit, struct vcodec_frame_pool **pool);

/*
 * Takes a frame from the pool with one reference.
 *
 * Reuses a free frame or allocates one.  The samples and the codec data
 * of a reused frame are left as they were; a new frame's samples are 0
 * and its codec data cleared and then given to side_init.  The bytes of
 * the codec's structure after struct vcodec_frame, and poc, time_us,
 * concealed and the display fields, are cleared; id is set from next_id.
 * Returns NULL when allocated has reached limit or an allocation fails.
 */
struct vcodec_frame *vcodec_frame_get(struct vcodec_frame_pool *pool);

/* Adds a reference to a frame. */
void vcodec_frame_ref(struct vcodec_frame *frame);

/*
 * Drops a reference; the last one returns the frame to its pool's free
 * list, or frees it when the pool is dead (and frees the pool with its
 * last frame).  NULL is ignored.
 */
void vcodec_frame_unref(struct vcodec_frame *frame);

/* Fills a frame's three planes with 128 (a grey picture); the codec data is the caller's to set. */
void vcodec_frame_fill_grey(struct vcodec_frame *frame);

/*
 * Copies the three planes of src into dst (frames of the same pool, or
 * of pools of the same size): the concealment of a whole picture.
 */
void vcodec_frame_copy_planes(struct vcodec_frame *dst, const struct vcodec_frame *src);

/* Frees a pool's free frames and marks it dead; frames still referenced stay valid until their last unref.  NULL is ignored. */
void vcodec_frame_pool_destroy(struct vcodec_frame_pool *pool);

#endif
