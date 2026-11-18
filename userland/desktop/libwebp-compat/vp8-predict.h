/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The intra prediction of the lossy format (RFC 6386 section 12): the
 * 16x16 luma and 8x8 chroma modes and the ten 4x4 subblock modes, each
 * from edge pixels the caller gathers.
 */

#ifndef LIBWEBP_COMPAT_VP8_PREDICT_H
#define LIBWEBP_COMPAT_VP8_PREDICT_H

#include "common.h"

/*
 * The edges a 16x16 (size 16) or 8x8 (size 8) block is predicted from.
 * corner is the pixel above and to the left (P); above[0..size - 1] the
 * row above (A), left[0..size - 1] the column on the left (L).  The caller
 * fills them with the frame's unfiltered reconstructed pixels, or with
 * the out-of-frame values (RFC 6386 12.2, 12.3, 20.14): 127 for the row
 * above the frame (the corner too, on the top row), 129 for the column
 * left of the frame (and the corner of a macroblock in the left column
 * below the top row).  have_above and have_left say whether the block
 * has a macroblock above and on the left; only DC_PRED looks at them.
 */
struct webpc_vp8_edges {
	uint8_t corner;
	uint8_t above[16];
	uint8_t left[16];
	int have_above;
	int have_left;
};

/*
 * Predicts a block of size by size (16 for luma, 8 for chroma) pixels into
 * dst (stride bytes a row) with mode WEBPC_VP8_DC_PRED, V_PRED, H_PRED or
 * TM_PRED (RFC 6386 12.2, 12.3):
 *
 *   DC_PRED  the rounded mean of the above row and the left column; of
 *            the one available alone (rounded mean of size values); 128
 *            with neither (shift = 3 + have_above + have_left for 16, one
 *            less for 8, as 20.14 rounds);
 *   V_PRED   every row the above row; H_PRED every column the left one;
 *   TM_PRED  left[r] + above[c] - corner, clamped to 0..255.
 */
void webpc_vp8_predict_block(unsigned mode, unsigned size, const struct webpc_vp8_edges *edges, uint8_t *dst,
	ptrdiff_t stride);

/*
 * Predicts one 4x4 luma subblock into dst (stride bytes a row) with mode
 * WEBPC_VP8_B_* (RFC 6386 12.3, its subblock code, and 20.14).  above
 * points at 8 pixels: above[0..3] the row above the subblock and
 * above[4..7] the 4 above and to the right; above[-1] is the corner P.
 * left[0..3] is the column on the left.  The caller fills them as 12.3
 * says, including the special above-right of the right column of
 * subblocks (rows 1 to 3 use the pixels above-right of the macroblock,
 * row -1 columns 16..19; the rightmost macroblock of a row below the top
 * uses row -1 column 15 for all four; the top macroblock row 127) and the
 * out-of-frame values 127 and 129.  The subblock modes never look at
 * whether a neighbour exists: B_DC_PRED always averages the 4 above and
 * the 4 left values.
 */
void webpc_vp8_predict_subblock(unsigned mode, const uint8_t *above, const uint8_t *left, uint8_t *dst,
	ptrdiff_t stride);

#endif
