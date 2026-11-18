/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DCT and WHT coefficients of one macroblock (RFC 6386 section 13):
 * their tokens, read with the contexts of the neighbouring blocks, and
 * their dequantization (section 14.1).
 */

#ifndef LIBWEBP_COMPAT_VP8_TOKENS_H
#define LIBWEBP_COMPAT_VP8_TOKENS_H

#include "common.h"
#include "bool.h"
#include "vp8-header.h"

/*
 * The blocks of a macroblock's residue as RFC 6386 20.16 numbers them: 16
 * Y, 4 U, 4 V in raster order, then Y2 (which is coded first, 13).
 */
#define WEBPC_VP8_BLOCK_Y		0U
#define WEBPC_VP8_BLOCK_U		16U
#define WEBPC_VP8_BLOCK_V		20U
#define WEBPC_VP8_BLOCK_Y2		24U
#define WEBPC_VP8_BLOCKS		25U

/*
 * The contexts of one side of a macroblock (RFC 6386 13.3, the third
 * dimension): whether each neighbouring block had coefficients, 1 or 0.
 * Block b (raster within its plane) uses, on the left, entry b / 4 for Y,
 * 4 + (b - 16) / 2 for U, 6 + (b - 20) / 2 for V and 8 for Y2; above,
 * entry b % 4 for Y, 4 + (b - 16) % 2 for U, 6 + (b - 20) % 2 for V and 8
 * for Y2 (20.16 left_context_index, above_context_index).  After the
 * block, both entries it used take its value.  The decoder keeps one for the left
 * (cleared at each row's start) and one per macroblock column for above
 * (cleared at the frame's start).
 */
#define WEBPC_VP8_CONTEXTS		9U

/*
 * A macroblock's dequantized coefficients: coeffs[block][position] in
 * raster order within the 4x4 block (positions de-zig-zagged), each
 * already multiplied by its factor and wrapped to 16 bits (webpc_wrap16,
 * as the reference decoder stores it).  Y blocks of a macroblock with Y2
 * have position 0 zero here (the WHT fills it later).  coded has bit b
 * set for each block b whose token sequence was more than an immediate
 * EOB (its context value), and bit 31 set when any block's was (the
 * macroblock "has coefficients" for the loop filter, 15.1, 20.16
 * eob_mask).
 */
struct webpc_vp8_residue {
	int16_t coeffs[WEBPC_VP8_BLOCKS][16];
	uint32_t coded;
};

/*
 * Reads a macroblock's tokens from its token partition (RFC 6386 13, 13.2,
 * 13.3, 20.16) and dequantizes them with dequant (14.1).
 *
 * has_y2 is 1 when the luma mode is not B_PRED.  Then Y2 is read first
 * (type WEBPC_VP8_TYPE_Y2, from position 0), the Y blocks from position 1
 * (type Y_AFTER_Y2); otherwise the Y blocks from position 0 (type
 * Y_WITH_DC).  Chroma is type CHROMA from position 0.  For each block: the
 * context is the sum of its left and above entries; for each position i
 * the probabilities are header->coeff_probs[type][coeff_bands[i]][ctx];
 * the token is read with coeff_tree, from node 2 (no EOB) right after a
 * DCT_0; EOB ends the block; DCT_0 is a zero; DCT_1..DCT_4 the values
 * 1..4; a category is its base plus its extra bits (cat_probs, most
 * significant first); a non-zero value is followed by its sign bit
 * (probability 128).  The context for the next position is 0 after a
 * zero, 1 after a magnitude 1, 2 after a larger one.  After position 15
 * the block ends without EOB.  The value at position i goes to raster
 * position zigzag[i], times dequant's DC factor for i = 0 and AC factor
 * otherwise (y2 for Y2, y1 for Y, uv for chroma), wrapped to 16 bits.
 *
 * Each block's context value, left and above alike, is 1 when its token
 * sequence ended at a later position than its first (at least one token
 * was not the immediate EOB, zeros included), else 0 (20.16: t = (c !=
 * first)).  RFC 6386 13.3's pseudocode differs here and in its plane
 * numbers; 20.16 is correct.
 *
 * Fills residue (all coefficients, coded); reading past the end shows in
 * webpc_bool_ended, which the caller checks.
 */
void webpc_vp8_read_residue(struct webpc_bool *tokens, const struct webpc_vp8_header *header,
	const struct webpc_vp8_dequant *dequant, int has_y2, uint8_t left[WEBPC_VP8_CONTEXTS],
	uint8_t above[WEBPC_VP8_CONTEXTS], struct webpc_vp8_residue *residue);

/*
 * Sets the contexts of a macroblock whose mb_skip_coeff is 1 (no tokens
 * read; RFC 6386 20.16 reset_mb_context): the Y, U and V entries of left
 * and above become 0, and the Y2 entries too unless has_y2 is 0 (a B_PRED
 * macroblock leaves the Y2 contexts as they were).
 */
void webpc_vp8_skip_residue(int has_y2, uint8_t left[WEBPC_VP8_CONTEXTS], uint8_t above[WEBPC_VP8_CONTEXTS]);

#endif
