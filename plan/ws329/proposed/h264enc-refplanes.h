/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interpolated planes of a reference frame for the motion search of
 * the software H.264 encoder: the full-sample luma plane and the three
 * half-sample planes (b, h and j of 8.4.2.2.1, Figure 8-4), each padded
 * by H264ENC_PLANE_PAD samples on every side, so that the search can
 * read any candidate's prediction without clamping and without
 * filtering again.
 *
 * Every half sample is computed with the decoder's interpolation
 * (h264sw_interp_luma) at the position it stands for, including the
 * positions outside the frame (where the decoder clamps its reference
 * positions, 8-239 and 8-240), and the quarter samples are the rounded
 * averages Table 8-12 makes of them (8-250 to 8-261).  So a prediction
 * read here equals the decoder's prediction for the same vector.
 */

#ifndef LIBMEDIA_H264ENC_REFPLANES_H
#define LIBMEDIA_H264ENC_REFPLANES_H

#include "h264sw.h"
#include "h264sw-frame.h"

/* The padding of each plane on every side, in luma samples. */
#define H264ENC_PLANE_PAD 32

/*
 * The planes of one reference frame.
 *
 * frame_id is the id of the struct h264sw_frame they were built from (0
 * when not built).  plane[0] full samples G, [1] b (half sample to the
 * right), [2] h (half sample below), [3] j (both).  Each has stride bytes
 * per row and (width + 2 * pad) by (height + 2 * pad) samples; origin[n]
 * points to the sample of frame location (0, 0) in plane n.  width and
 * height are the frame's luma size.  One allocation (buffer), owned by
 * the struct.
 */
struct h264enc_refplanes {
	uint32_t frame_id;
	uint8_t *buffer;
	uint8_t *origin[4];
	size_t stride;
	unsigned width;
	unsigned height;
};

/* Allocates planes for frames of width by height (multiples of 16); 0, EINVAL, ENOMEM. */
int h264enc_refplanes_alloc(struct h264enc_refplanes *planes, unsigned width, unsigned height);

/* Frees the planes (NULL or never allocated is ignored). */
void h264enc_refplanes_free(struct h264enc_refplanes *planes);

/*
 * Fills the planes from a decoded (deblocked) frame of the allocated
 * size: G by copying with the edges repeated into the padding, b, h and j
 * at every padded position by h264sw_interp_luma with fractions (2, 0),
 * (0, 2) and (2, 2).  Sets frame_id.  Returns 0, or EINVAL for a frame of
 * another size.
 */
int h264enc_refplanes_build(struct h264enc_refplanes *planes, const struct h264sw_frame *frame);

/*
 * The range of full-sample vector components for a block of width by
 * height at (x, y) that keeps every read of h264enc_refplanes_predict
 * inside the padded planes, including a quarter-sample step either way:
 * x + mv_x >= -( pad - 4 ) and x + mv_x + width <= frame width + pad - 4,
 * the same vertically.  Gives min_x, max_x, min_y, max_y in full samples.
 */
void h264enc_refplanes_window(const struct h264enc_refplanes *planes, unsigned x, unsigned y, unsigned width, unsigned height, int *min_x, int *max_x, int *min_y, int *max_y);

/* The full-sample plane at frame location (x, y) (inside the window), for SAD over full-sample vectors. */
const uint8_t *h264enc_refplanes_full(const struct h264enc_refplanes *planes, int x, int y);

/*
 * Predicts a width by height luma block (4, 8 or 16 each) at frame
 * location (x, y) with vector (mv_x, mv_y) in quarter samples: the full
 * position ( x + ( mv_x >> 2 ), ... ) (an arithmetic floor, computed
 * without shifting a negative value) and the fraction ( mv_x & 3,
 * mv_y & 3 ) selected as Table 8-12, from the four planes.  The vector
 * must keep the block inside h264enc_refplanes_window (a caller's
 * mistake otherwise: the block is filled with 128).  out receives the
 * samples with out_stride bytes per row.
 */
void h264enc_refplanes_predict(const struct h264enc_refplanes *planes, unsigned x, unsigned y, int mv_x, int mv_y, unsigned width, unsigned height, uint8_t *out, size_t out_stride);

#endif
