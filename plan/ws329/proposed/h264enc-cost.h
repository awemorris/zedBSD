/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The distortion and rate measures the software H.264 encoder decides
 * with.  None of this is in the specification: these are the encoder's
 * own choices, fixed here so that every decision is reproducible.
 *
 * - SAD: the sum of absolute differences of two blocks.
 * - SATD: for each 4x4 block of the difference D, T = H * D * H with H =
 *   [ 1 1 1 1; 1 1 -1 -1; 1 -1 -1 1; 1 -1 1 -1 ], and ( sum of |T| + 1 )
 *   >> 1; the blocks' values summed.
 * - SSD: the sum of squared differences.
 * - The Lagrange multipliers, per QP: lambda_mode = 0.85 *
 *   2^( ( QP - 12 ) / 3 ) for SSD-based costs and lambda_motion = Sqrt(
 *   lambda_mode ) for SAD- and SATD-based costs (T. Wiegand, H. Schwarz,
 *   A. Joch, F. Kossentini and G. J. Sullivan, "Rate-constrained coder
 *   control and comparison of video coding standards", IEEE Trans.
 *   Circuits Syst. Video Technol. 13(7), 2003), kept in fixed point with
 *   8 fraction bits.  A cost is distortion + ( ( lambda * bits ) >> 8 )
 *   with bits a whole number of bits, or + ( ( lambda * cost ) >> 16 )
 *   for a cost in 1/256 bit.
 */

#ifndef LIBMEDIA_H264ENC_COST_H
#define LIBMEDIA_H264ENC_COST_H

#include <stddef.h>
#include <stdint.h>

/*
 * The multipliers of every QP 0..51, as Round( 256 * lambda ): ssd[qp]
 * from lambda_mode, sad[qp] from lambda_motion.  Computed with <math.h>
 * by h264enc_lambda_init; owned by the encoder (embedded).
 */
struct h264enc_lambda {
	uint32_t ssd[52];
	uint32_t sad[52];
};

/* Computes the multipliers.  (sad[12] is Round( 256 * Sqrt( 0.85 ) ) = 236, ssd[12] 218: checks of the formula, not data.) */
void h264enc_lambda_init(struct h264enc_lambda *lambda);

/* SAD of two width by height blocks (each 1..16), each with its stride. */
uint32_t h264enc_sad(const uint8_t *a, size_t a_stride, const uint8_t *b, size_t b_stride, unsigned width, unsigned height);

/* SATD as above; width and height are 4, 8 or 16. */
uint32_t h264enc_satd(const uint8_t *a, size_t a_stride, const uint8_t *b, size_t b_stride, unsigned width, unsigned height);

/* SSD of two width by height blocks (each 1..16). */
uint64_t h264enc_ssd(const uint8_t *a, size_t a_stride, const uint8_t *b, size_t b_stride, unsigned width, unsigned height);

/*
 * The bits of a motion vector difference as CAVLC would send it: the
 * se(v) lengths of its two components (vcodec_se_bits).  Used as the rate
 * of a vector for both entropy coders.
 */
unsigned h264enc_mvd_bits(int dx, int dy);

/* distortion + ( ( lambda * bits ) >> 8 ), saturated at UINT32_MAX (the product in uint64_t). */
uint32_t h264enc_cost(uint32_t distortion, uint32_t lambda, uint32_t bits);

#endif
