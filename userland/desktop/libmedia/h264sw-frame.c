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

/*
 * Makes a pool for frames of width_mbs by height_mbs macroblocks.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-frame.h.
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
	UNUSED_PARAMETER(width_mbs);
	UNUSED_PARAMETER(height_mbs);
	UNUSED_PARAMETER(crop_x);
	UNUSED_PARAMETER(crop_y);
	UNUSED_PARAMETER(crop_width);
	UNUSED_PARAMETER(crop_height);
	UNUSED_PARAMETER(limit);
	UNUSED_PARAMETER(pool);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Takes a frame from the pool with one reference.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-frame.h.
 */
struct h264sw_frame *
h264sw_frame_get(
	struct h264sw_frame_pool *pool)
{
	UNUSED_PARAMETER(pool);

	/* Reports that nothing is made yet. */
	return NULL;
}

/*
 * Adds a reference to a frame.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-frame.h.
 */
void
h264sw_frame_ref(
	struct h264sw_frame *frame)
{
	UNUSED_PARAMETER(frame);

	/* Does nothing yet. */
	return;
}

/*
 * Drops a reference; the last one returns the frame to its pool's free list, or frees it when the pool is dead (and frees the pool with its last frame).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-frame.h.
 */
void
h264sw_frame_unref(
	struct h264sw_frame *frame)
{
	UNUSED_PARAMETER(frame);

	/* Does nothing yet. */
	return;
}

/*
 * Fills a frame's three planes with the sample value 128 (a non-existing frame, a concealed picture's start).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-frame.h.
 */
void
h264sw_frame_fill_grey(
	struct h264sw_frame *frame)
{
	UNUSED_PARAMETER(frame);

	/* Does nothing yet. */
	return;
}

/*
 * Frees a pool's free frames and marks it dead.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-frame.h.
 */
void
h264sw_frame_pool_destroy(
	struct h264sw_frame_pool *pool)
{
	UNUSED_PARAMETER(pool);

	/* Does nothing yet. */
	return;
}
