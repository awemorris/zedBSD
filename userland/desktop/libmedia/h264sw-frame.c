/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoded frames of the software H.264 decoder and their pool.
 */

#include "h264sw-frame.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The sample value of a flat grey picture, half of the range of 8-bit samples. */
#define FRAME_GREY_SAMPLE 128

/* The side of a macroblock in luma samples, and the width of its chroma block (4:2:0). */
#define FRAME_MB_SIDE 16U

/* Static function declarations. */
static struct h264sw_frame *frame_make(struct h264sw_frame_pool *pool);
static void frame_release(struct h264sw_frame *frame);
static void frame_start(struct h264sw_frame_pool *pool, struct h264sw_frame *frame);
static void frame_motion_reset(struct h264sw_motion *motion, unsigned count);

/*
 * Makes a pool for frames of width_mbs by height_mbs macroblocks.
 *
 * Only the pool itself is allocated; the frames come with the first gets.
 */
int
h264sw_frame_pool_create(
	unsigned width_mbs,
	unsigned height_mbs,
	unsigned crop_x,
	unsigned crop_y,
	unsigned crop_width,
	unsigned crop_height,
	unsigned limit,
	struct h264sw_frame_pool **pool)
{
	struct h264sw_frame_pool *made;
	unsigned width;
	unsigned height;

	/* Refuses a place to report to that does not exist. */
	if (pool == NULL)
		return EINVAL;

	/* A side of a picture is 1 macroblock up to the largest side level 6.2 allows (A.3.1). */
	if (width_mbs == 0U ||
	    width_mbs > H264SW_MAX_SIDE_MBS)
		return EINVAL;
	if (height_mbs == 0U ||
	    height_mbs > H264SW_MAX_SIDE_MBS)
		return EINVAL;

	/* The picture has at most as many macroblocks as level 6.2 allows (A.3.1). */
	if (width_mbs * height_mbs > H264SW_MAX_FRAME_MBS)
		return EINVAL;

	/* The cropping window shows at least one sample and lies inside the frame. */
	width = width_mbs * FRAME_MB_SIDE;
	height = height_mbs * FRAME_MB_SIDE;
	if (crop_width == 0U ||
	    crop_height == 0U)
		return EINVAL;
	if (crop_x >= width ||
	    crop_width > width - crop_x)
		return EINVAL;
	if (crop_y >= height ||
	    crop_height > height - crop_y)
		return EINVAL;

	/* A pool makes at least one frame and at most what the decoder can need. */
	if (limit == 0U ||
	    limit > H264SW_FRAME_POOL_MAX)
		return EINVAL;

	/* Allocates the pool. */
	made = calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Records the size, the window and the limit every frame of the pool shares. */
	made->width = width;
	made->height = height;
	made->size_mbs = width_mbs * height_mbs;
	made->crop_x = crop_x;
	made->crop_y = crop_y;
	made->crop_width = crop_width;
	made->crop_height = crop_height;
	made->limit = limit;

	/* The first frame is handed out with id 1, so 0 can mean no frame. */
	made->next_id = 1U;

	/* Publishes the pool. */
	*pool = made;

	/* Succeeded: the caller owns the pool. */
	return 0;
}

/*
 * Takes a frame from the pool with one reference.
 *
 * A free frame is reused before a new one is made, so a pool stays as
 * small as the use of it.
 */
struct h264sw_frame *
h264sw_frame_get(
	struct h264sw_frame_pool *pool)
{
	struct h264sw_frame *frame;

	/* A pool that is not there, or that was destroyed, hands out nothing. */
	if (pool == NULL)
		return NULL;
	if (pool->dead)
		return NULL;

	/* Reuses the most recently freed frame if there is one. */
	frame = pool->free;
	if (frame != NULL) {
		pool->free = frame->next_free;
		frame->next_free = NULL;
		frame_start(pool, frame);
		return frame;
	}

	/* The pool does not make more frames than its limit, so a stream cannot make the decoder grow without bound. */
	if (pool->allocated >= pool->limit)
		return NULL;

	/* Makes a new frame. */
	frame = frame_make(pool);
	if (frame == NULL)
		return NULL;

	/* Counts the new frame so the limit and the pool's lifetime see it. */
	pool->allocated++;
	frame_start(pool, frame);

	/* Succeeded: the caller holds the one reference of the new frame. */
	return frame;
}

/*
 * Adds a reference to a frame.
 *
 * The count is unsigned, so a count that wraps is defined and a frame
 * that was never unreferenced that often is not affected in practice.
 */
void
h264sw_frame_ref(
	struct h264sw_frame *frame)
{
	/* A missing frame has nothing to count. */
	if (frame == NULL)
		return;

	/* One more holder keeps the frame out of the free list. */
	frame->refs++;
}

/*
 * Drops a reference; the last one returns the frame to its pool's free list, or frees it when the pool is dead (and frees the pool with its last frame).
 *
 * A frame that is already free is left alone, so a second unref of a
 * returned frame does not put it on the list twice.
 */
void
h264sw_frame_unref(
	struct h264sw_frame *frame)
{
	struct h264sw_frame_pool *pool;

	/* A missing frame has nothing to drop. */
	if (frame == NULL)
		return;

	/* A frame nobody refers to is already back in its pool. */
	if (frame->refs == 0U)
		return;

	/* Drops this holder's reference; frames still referenced stay out of the pool. */
	frame->refs--;
	if (frame->refs != 0U)
		return;

	/* A live pool takes the frame back for the next get. */
	pool = frame->pool;
	if (!pool->dead) {
		frame->next_free = pool->free;
		pool->free = frame;
		return;
	}

	/* A dead pool no longer reuses frames: the frame is freed, and the pool with the last one. */
	pool->allocated--;
	frame_release(frame);
	if (pool->allocated == 0U)
		free(pool);
}

/*
 * Fills a frame's three planes with the sample value 128 (a non-existing frame, a concealed picture's start).
 *
 * The motion is reset too, so a picture that reads this frame as its
 * colocated picture finds every macroblock intra and gets zero vectors.
 */
void
h264sw_frame_fill_grey(
	struct h264sw_frame *frame)
{
	unsigned plane;
	size_t rows;

	/* A missing frame has nothing to fill. */
	if (frame == NULL)
		return;

	/* Sets each plane's samples to the middle of the range: luma has height rows, chroma half as many. */
	for (plane = 0U;
	     plane < 3U;
	     plane++) {
		rows = frame->height;
		if (plane != 0U)
			rows = rows / 2U;
		memset(frame->plane[plane], FRAME_GREY_SAMPLE, frame->stride[plane] * rows);
	}

	/* Records every macroblock as intra with no reference. */
	frame_motion_reset(frame->motion, frame->size_mbs);
}

/*
 * Frees a pool's free frames and marks it dead.
 *
 * The pool is freed here when no frame is out, else with the last unref.
 */
void
h264sw_frame_pool_destroy(
	struct h264sw_frame_pool *pool)
{
	struct h264sw_frame *frame;

	/* A missing pool has nothing to destroy. */
	if (pool == NULL)
		return;

	/* The pool makes and reuses no frame from now on. */
	pool->dead = 1;

	/* Frees the frames no one refers to; the others are freed by their last unref. */
	while (pool->free != NULL) {
		frame = pool->free;
		pool->free = frame->next_free;
		pool->allocated--;
		frame_release(frame);
	}

	/* A pool with no frame out has no later unref to free it, so it goes now. */
	if (pool->allocated == 0U)
		free(pool);
}

/*
 * Allocates a frame of the pool's size with cleared samples and all-intra motion.
 *
 * Returns NULL when an allocation fails; nothing stays allocated then.
 */
static struct h264sw_frame *
frame_make(
	struct h264sw_frame_pool *pool)
{
	struct h264sw_frame *frame;
	size_t luma_size;
	size_t chroma_size;

	/* Allocates the frame's record. */
	frame = calloc(1U, sizeof(*frame));
	if (frame == NULL)
		return NULL;

	/* The planes are 4:2:0: a luma plane and two chroma planes of a quarter of its samples, without borders. */
	luma_size = (size_t)pool->width * (size_t)pool->height;
	chroma_size = luma_size / 4U;

	/* Allocates the three planes in one block, cleared. */
	frame->plane[0] = calloc(luma_size + 2U * chroma_size, 1U);
	if (frame->plane[0] == NULL) {
		free(frame);
		return NULL;
	}

	/* Allocates the motion records of the macroblocks, cleared. */
	frame->motion = calloc(pool->size_mbs, sizeof(*frame->motion));
	if (frame->motion == NULL) {
		free(frame->plane[0]);
		free(frame);
		return NULL;
	}

	/* Locates the chroma planes after the luma plane. */
	frame->plane[1] = frame->plane[0] + luma_size;
	frame->plane[2] = frame->plane[1] + chroma_size;
	frame->stride[0] = (size_t)pool->width;
	frame->stride[1] = (size_t)pool->width / 2U;
	frame->stride[2] = (size_t)pool->width / 2U;

	/* Records the frame's size and its pool. */
	frame->pool = pool;
	frame->width = pool->width;
	frame->height = pool->height;
	frame->size_mbs = pool->size_mbs;

	/* A new frame has no motion yet: every macroblock is intra with no reference. */
	frame_motion_reset(frame->motion, frame->size_mbs);

	/* Succeeded: the frame is ready to be started. */
	return frame;
}

/*
 * Frees a frame's memory; the pool's counts are the caller's to keep.
 */
static void
frame_release(
	struct h264sw_frame *frame)
{
	/* The three planes are one block starting at the luma plane. */
	free(frame->plane[0]);
	free(frame->motion);
	free(frame);
}

/*
 * Starts a frame's life as the picture being decoded.
 *
 * The samples and the motion are kept as they were; every field of the
 * picture is cleared and the frame gets a new identity.
 */
static void
frame_start(
	struct h264sw_frame_pool *pool,
	struct h264sw_frame *frame)
{
	/* One reference: the caller's. */
	frame->refs = 1U;

	/* The window is the pool's, whatever the last picture did with the fields. */
	frame->crop_x = pool->crop_x;
	frame->crop_y = pool->crop_y;
	frame->crop_width = pool->crop_width;
	frame->crop_height = pool->crop_height;

	/*
	 * The identity is a counter of the pool that is incremented each time a
	 * frame is handed out, so a motion record naming an earlier use of
	 * this frame's memory names another id.  The counter skips 0 when it
	 * wraps: 0 means no frame.
	 */
	frame->id = pool->next_id;
	pool->next_id++;
	if (pool->next_id == 0U)
		pool->next_id = 1U;

	/* Clears what the last picture left for later pictures. */
	frame->poc = 0;
	frame->top_poc = 0;
	frame->bottom_poc = 0;
	frame->frame_num = 0U;
	frame->idr = 0;
	frame->time_us = 0;
	frame->concealed = 0U;
	frame->non_existing = 0;

	/* Clears how the last picture was to be shown. */
	frame->sar_width = 0U;
	frame->sar_height = 0U;
	frame->signal_present = 0;
	frame->full_range = 0;
	frame->colour_present = 0;
	frame->matrix_coefficients = 0U;
}

/*
 * Records the motion of count macroblocks as intra with no reference and zero vectors.
 */
static void
frame_motion_reset(
	struct h264sw_motion *motion,
	unsigned count)
{
	unsigned index;
	unsigned part;

	/* Zeroes every vector and reference id. */
	memset(motion, 0, (size_t)count * sizeof(*motion));

	/* Marks each macroblock intra, with every list unused. */
	for (index = 0U;
	     index < count;
	     index++) {
		motion[index].intra = 1U;
		for (part = 0U;
		     part < 4U;
		     part++) {
			motion[index].ref_idx[0][part] = -1;
			motion[index].ref_idx[1][part] = -1;
		}
	}
}
