/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The output of a decoded image in the layouts of libwebp's colour spaces:
 * Y'CbCr 4:2:0 planes (with an alpha plane or none) or ARGB pixels into
 * rows of 3 or 4 bytes a pixel, with straight or premultiplied alpha.
 *
 * The conversion is libwebp's default, as libwebp 1.5.0 gives it (RFC 9649
 * 2.5 asks for Recommendation 601 and leaves the details to the decoder):
 * Y'CbCr with the limited ranges of ITU-R BT.601 to R'G'B', in fixed point,
 * with the constants below taken as data.
 */

#ifndef LIBWEBP_COMPAT_OUTPUT_H
#define LIBWEBP_COMPAT_OUTPUT_H

#include "common.h"

/*
 * The byte layout of one output pixel: bytes (3 or 4), the offsets of red,
 * green and blue within it, the offset of alpha (-1 when the layout has
 * none), and premultiply (1 when red, green and blue are multiplied by
 * alpha, webpc_output_premultiply).
 */
struct webpc_layout {
	unsigned bytes;
	unsigned red;
	unsigned green;
	unsigned blue;
	int alpha;
	int premultiply;
};

/*
 * A decoded lossy image's planes (pointing into a picture's memory, not
 * owned): y width by height, u and v (width + 1) / 2 by (height + 1) / 2,
 * with their strides.
 */
struct webpc_yuv_view {
	const uint8_t *y;
	const uint8_t *u;
	const uint8_t *v;
	ptrdiff_t y_stride;
	ptrdiff_t uv_stride;
	unsigned width;
	unsigned height;
};

/*
 * Converts one pixel's Y', Cb (u) and Cr (v), each 0..255, to rgb[0..2]
 * (red, green, blue).  With mul(x, k) = (x * k) >> 8 and clip(t) = 0 below
 * 0, 255 when t >> 6 is above 255, else t >> 6:
 *
 *   red   = clip(mul(y, 19077) + mul(v, 26149) - 14234)
 *   green = clip(mul(y, 19077) - mul(u, 6419) - mul(v, 13320) + 8708)
 *   blue  = clip(mul(y, 19077) + mul(u, 33050) - 17685)
 *
 * The constants are BT.601's limited-range coefficients in 14-bit fixed
 * point with the offsets of 16 and 128 folded in, checked against dwebp
 * 1.5.0's output.
 *
 * Values referenced: libwebp 1.5.0 (BSD-3-Clause) src/dsp/yuv.h, the
 * constants of its default YUV to RGB conversion.
 */
void webpc_output_yuv_pixel(unsigned y, unsigned u, unsigned v, uint8_t rgb[3]);

/*
 * Procedure referenced: libwebp 1.5.0 (BSD-3-Clause)
 * src/dsp/alpha_processing.c, the premultiplication of its rgbA modes.
 *
 * Premultiplies a component x (0..255) by alpha a (0..255) as libwebp does:
 * (x * a) / 255, rounded down (x itself when a is 255).  libwebp computes it
 * as (x * a * 32897) >> 23, which gives the same value for every x and a
 * (checked over all of them, and against libwebp 1.5.0's output).
 */
unsigned webpc_output_premultiply(unsigned x, unsigned a);

/*
 * Procedure referenced: libwebp 1.5.0 (BSD-3-Clause) src/dsp/upsampling.c,
 * its default ("fancy") chroma upsampling, which the rule below states.
 *
 * Writes a lossy image as RGB rows: the row of image row r starts at
 * first_row + r * row_step (row_step negative to flip; the caller has
 * checked that every row fits its buffer).  Each pixel's chroma is, with
 * fancy 1, the 9-3-3-1 interpolation of the four chroma samples nearest
 * to it: for pixel (x, y), with i = x / 2, j = y / 2, the horizontal
 * neighbour i - 1 for even x and i + 1 for odd x, the vertical neighbour j
 * - 1 for even y and j + 1 for odd y, each index clamped to the plane,
 * and the value (9 * near + 3 * horizontal + 3 * vertical + diagonal + 8)
 * >> 4; with fancy 0 it is the sample (i, j).  Then webpc_output_yuv_pixel.
 * alpha (width bytes a row, alpha_stride apart) gives the alpha of each
 * pixel, or NULL for 255; it is written when the layout has alpha and,
 * with premultiply, multiplies the colour.
 */
void webpc_output_from_yuv(const struct webpc_yuv_view *image, const uint8_t *alpha, ptrdiff_t alpha_stride,
	int fancy, const struct webpc_layout *layout, uint8_t *first_row, ptrdiff_t row_step);

/*
 * Writes a lossless image (width * height ARGB pixels, rows top to bottom)
 * as rows of layout, the row of image row r at first_row + r * row_step.
 * Alpha is the pixel's own; with premultiply it multiplies the colour.
 */
void webpc_output_from_argb(const uint32_t *pixels, unsigned width, unsigned height, const struct webpc_layout *layout,
	uint8_t *first_row, ptrdiff_t row_step);

#endif
