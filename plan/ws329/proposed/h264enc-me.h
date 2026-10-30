/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The motion search of the software H.264 encoder: for one block of the
 * source picture and one reference frame, the quarter-sample motion
 * vector of least cost, cost = distortion + lambda_motion * bits of the
 * vector difference from its predictor (h264enc-cost.h).
 *
 * The search is the encoder's own and is fixed here step by step so that
 * it is reproducible and testable:
 *
 *  1. Candidates.  Each candidate vector the caller gives (in quarter
 *     samples) is rounded to full samples (( c + 2 ) >> 2 per component,
 *     as an arithmetic floor), clamped into the search window, and
 *     evaluated with SAD + lambda * h264enc_mvd_bits( 4 * full - mvp ),
 *     skipping a full vector already evaluated.  The predictor mvp itself
 *     is always the first candidate; ( 0, 0 ) is always evaluated.
 *  2. Large diamond (when large_diamond is 1): from the best, the eight
 *     points ( +-2, 0 ), ( 0, +-2 ), ( +-1, +-1 ) in that order; move to
 *     the best when it is strictly better; repeat at most iterations
 *     times.  (The large and small diamond patterns of S. Zhu and K.-K.
 *     Ma, "A new diamond search algorithm for fast block-matching motion
 *     estimation", IEEE Trans. Image Processing 9(2), 2000.)
 *  3. Small diamond: from the best, ( 0, -1 ), ( -1, 0 ), ( 1, 0 ), ( 0, 1 )
 *     in that order; move when strictly better; repeat at most iterations
 *     times.  A point outside the window or the range is skipped.
 *  4. Refinement (QUALITY): every point of the square of radius 2 around
 *     the best not yet evaluated.
 *  5. Half samples (subpel >= 1): around the best full vector (in quarter
 *     units, v = 4 * full), the points v + 2 * d for d in subpel_points
 *     (4: the small diamond order; 8: those then ( -1, -1 ), ( 1, -1 ),
 *     ( -1, 1 ), ( 1, 1 )), cost SATD + lambda * bits; the best by strict
 *     improvement, the full vector re-costed with SATD to compare.
 *  6. Quarter samples (subpel 2): the same around the best half vector
 *     with steps of 1.
 *
 * Ties keep the earlier point.  Every prediction comes from
 * h264enc_refplanes (equal to the decoder's).  The result is in quarter
 * samples, inside the window and the range.
 */

#ifndef LIBMEDIA_H264ENC_ME_H
#define LIBMEDIA_H264ENC_ME_H

#include "h264sw.h"
#include "h264enc-refplanes.h"

/* The most candidates one search takes besides the predictor. */
#define H264ENC_ME_CANDIDATES 8U

/*
 * The settings of a search.
 *
 * range: the largest full-sample distance from the predictor (rounded
 * to full samples) in either component.  iterations: the most steps of
 * each diamond.  large_diamond, refine: steps 2 and 4.  subpel: 0 full
 * samples only, 1 half, 2 quarter.  subpel_points: 4 or 8.
 * mv_min and mv_max: the allowed quarter-sample range of [0] the
 * horizontal and [1] the vertical component (the level's limits:
 * horizontal -8192..8191 for levels below 6, vertical -4 * MaxVmvR ..
 * 4 * MaxVmvR - 1; A.3.1 and Table A-1).  lambda: lambda_motion of the
 * QP (h264enc_lambda's sad[qp]).
 */
struct h264enc_me_params {
	unsigned range;
	unsigned iterations;
	int large_diamond;
	int refine;
	unsigned subpel;
	unsigned subpel_points;
	int mv_min[2];
	int mv_max[2];
	uint32_t lambda;
};

/* The result: the vector (quarter samples), its cost and its distortion (SATD, or SAD when subpel is 0). */
struct h264enc_me_result {
	int16_t mv[2];
	uint32_t cost;
	uint32_t distortion;
};

/*
 * Searches one block.
 *
 * source is the source plane at the block's upper-left sample (stride
 * source_stride); (x, y) the block's frame location and width by height
 * its size (16x16, 16x8, 8x16 or 8x8); ref the reference's planes; mvp
 * the predictor (quarter samples, from h264sw_mvpred); candidates the
 * other starting vectors (count up to H264ENC_ME_CANDIDATES).  The
 * search window is h264enc_refplanes_window intersected with the range
 * and with mv_min..mv_max.  scratch holds at least 256 bytes for
 * predictions.  Fills *result.  Cannot fail: a window that holds no
 * vector (impossible for a frame of at least one macroblock) gives the
 * clamped predictor.
 */
void h264enc_me_search(const struct h264enc_me_params *params, const uint8_t *source, size_t source_stride, unsigned x, unsigned y, unsigned width, unsigned height, const struct h264enc_refplanes *ref, const int16_t *mvp, const int16_t (*candidates)[2], unsigned count, uint8_t *scratch, struct h264enc_me_result *result);

/*
 * Costs one given vector (no search): SATD of its prediction + lambda *
 * bits, for the vectors decisions take without a search (P_Skip, direct,
 * the second list of a bi-predicted candidate).  The vector must be
 * inside the window (clamped otherwise).
 */
uint32_t h264enc_me_cost(const struct h264enc_me_params *params, const uint8_t *source, size_t source_stride, unsigned x, unsigned y, unsigned width, unsigned height, const struct h264enc_refplanes *ref, const int16_t *mv, const int16_t *mvp, uint8_t *scratch);

#endif
