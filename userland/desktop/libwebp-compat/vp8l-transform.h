/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inverse transforms of the lossless format (RFC 9649 3.5): the
 * predictor, colour, subtract-green and colour-indexing transforms, each
 * applied to an array of ARGB pixels.  Nothing here reads the bitstream;
 * the transform data comes decoded.
 */

#ifndef LIBWEBP_COMPAT_VP8L_TRANSFORM_H
#define LIBWEBP_COMPAT_VP8L_TRANSFORM_H

#include "common.h"

/* The transform types (RFC 9649 3.5, Table 1). */
#define WEBPC_VP8L_PREDICTOR		0U
#define WEBPC_VP8L_COLOR		1U
#define WEBPC_VP8L_SUBTRACT_GREEN	2U
#define WEBPC_VP8L_COLOR_INDEXING	3U

/* The most transforms an image has: each type at most once (RFC 9649 3.5). */
#define WEBPC_VP8L_TRANSFORMS_MAX	4U

/* The size of a colour table, padded so any 8-bit index finds an entry (RFC 9649 3.5.4). */
#define WEBPC_VP8L_COLOR_TABLE_SIZE	256U

/*
 * Gives DIV_ROUND_UP(size, 1 << bits) (RFC 9649 3.5.1), for size 1 or
 * more and bits 0..9.
 */
unsigned webpc_vp8l_subsample(unsigned size, unsigned bits);

/*
 * Undoes the predictor transform (RFC 9649 3.5.1) in place over pixels
 * (width by height, rows top to bottom), in scan-line order so that each
 * prediction uses pixels already restored.  modes is the predictor image,
 * webpc_vp8l_subsample(width, size_bits) pixels a row, size_bits 2..9;
 * the mode of pixel (x, y) is the green of modes[(y >> size_bits) *
 * modes_width + (x >> size_bits)] taken modulo 16, and the modes 14 and 15
 * (which RFC 9649 does not define) predict as mode 0 does, as libwebp
 * reads them.
 *
 * The predictions are Table 2's, per channel, with Average2 the floor of
 * the mean, Select as the RFC writes it (L when pL < pT, else T), and
 * ClampAddSubtractHalf(a, b) = Clamp(a + (a - b) / 2) with C's division
 * (rounding toward zero, as the RFC's code divides).  The border rules
 * override the mode: pixel (0, 0) is predicted 0xff000000, the rest of
 * the top row from L, the rest of the left column from T; for the
 * rightmost column TR is the leftmost pixel of the current row.  The
 * residual is added per channel modulo 256.
 */
void webpc_vp8l_inverse_predictor(uint32_t *pixels, unsigned width, unsigned height, const uint32_t *modes,
	unsigned size_bits);

/*
 * Undoes the colour transform (RFC 9649 3.5.2) in place over pixels.
 * elements is the colour transform image, webpc_vp8l_subsample(width,
 * size_bits) pixels a row, size_bits 2..9; an element pixel carries
 * red_to_blue in its red, green_to_blue in its green and green_to_red in
 * its blue.  Per pixel: red += delta(green_to_red, green); blue +=
 * delta(green_to_blue, green); blue += delta(red_to_blue, new red), each
 * modulo 256, where delta(t, c) = (t * c) >> 5 on t and c as signed 8-bit
 * values (the shift a floor; only its low 8 bits are used).
 */
void webpc_vp8l_inverse_color(uint32_t *pixels, unsigned width, unsigned height, const uint32_t *elements,
	unsigned size_bits);

/*
 * Undoes the subtract-green transform (RFC 9649 3.5.3) over count pixels:
 * red and blue each get green added modulo 256.
 */
void webpc_vp8l_add_green(uint32_t *pixels, size_t count);

/*
 * Undoes the colour-indexing transform (RFC 9649 3.5.4): the packed image
 * (packed_width = webpc_vp8l_subsample(width, width_bits) pixels a row,
 * height rows) becomes the image of width pixels a row in out.  The index
 * of pixel (x, y) is, for width_bits 0, the green of the packed pixel; for
 * width_bits 1, 2 or 3, the bit field of 8 >> width_bits bits at bit
 * (x & ((1 << width_bits) - 1)) * (8 >> width_bits) of the green of packed
 * pixel (x >> width_bits, y).  The pixel is table[index].  table has
 * WEBPC_VP8L_COLOR_TABLE_SIZE entries: the decoded colours, then
 * 0x00000000 (transparent black) for the indices past the colour count.
 * packed and out may be the same buffer (of width * height pixels, the
 * packed rows at its start): the expansion then runs from the last pixel
 * back so that nothing is overwritten before it is read.
 */
void webpc_vp8l_inverse_indexing(const uint32_t *packed, unsigned width, unsigned height, unsigned width_bits,
	const uint32_t *table, uint32_t *out);

#endif
