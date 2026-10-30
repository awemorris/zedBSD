/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The source pictures the software H.264 encoder codes: a copy of each
 * picture the caller gives, in three planes of whole macroblocks, held
 * while B-frames wait for their anchor.
 *
 * The picture is copied (the caller's frame is free when h264enc_send
 * returns) and padded to whole macroblocks by repeating its last column
 * to the right and its last row downwards (the SPS's cropping window
 * hides the padding; repeating the edge makes the padding cheap to code).
 */

#ifndef LIBMEDIA_H264ENC_SOURCE_H
#define LIBMEDIA_H264ENC_SOURCE_H

#include "h264enc.h"

/*
 * One source picture.
 *
 * plane[0..2] Y, Cb, Cr of width_mbs * 16 by height_mbs * 16 luma
 * samples (chroma half each way), stride[n] bytes per row (no other
 * padding).  width and height the visible size.  time_us and flags as
 * given (struct h264enc_picture); index the picture's number in
 * presentation order from the start of the stream.  The planes are one
 * allocation owned by the struct.
 */
struct h264enc_source {
	uint8_t *buffer;
	uint8_t *plane[3];
	size_t stride[3];
	unsigned width_mbs;
	unsigned height_mbs;
	unsigned width;
	unsigned height;
	int64_t time_us;
	unsigned flags;
	uint64_t index;
};

/* Allocates the planes for a picture of width by height (even, at least 16 each); 0, EINVAL, ENOMEM. */
int h264enc_source_alloc(struct h264enc_source *source, unsigned width, unsigned height);

/* Frees the planes (NULL or never allocated is ignored). */
void h264enc_source_free(struct h264enc_source *source);

/*
 * Copies a picture in (I420 or NV12, the chroma of NV12 split into Cb and
 * Cr) and pads it as above; time_us and flags are taken, index is set.
 * Returns 0, or EINVAL for a format other than the two, a NULL plane, or
 * a stride smaller than a row.
 */
int h264enc_source_load(struct h264enc_source *source, const struct h264enc_picture *picture, uint64_t index);

#endif
