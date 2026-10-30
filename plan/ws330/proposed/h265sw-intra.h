/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intra prediction of the software H.265 decoder: the luma mode from
 * the most probable modes (8.4.2), the chroma mode (8.4.3, Table 8-2 for
 * 4:2:0), and the prediction samples of one transform block (8.4.4.2):
 * the substitution of unavailable reference samples (8.4.4.2.2), their
 * filtering with the strong smoothing of 32x32 luma blocks (8.4.4.2.3,
 * Table 8-4), and the planar (8.4.4.2.4), DC (8.4.4.2.5) and angular
 * (8.4.4.2.6, Tables 8-5 and 8-6) modes with their boundary filters.
 *
 * The functions are pure: the caller (h265sw-recon.h) gathers the
 * neighbouring samples and their availability from the picture.
 */

#ifndef LIBMEDIA_H265SW_INTRA_H
#define LIBMEDIA_H265SW_INTRA_H

#include "h265sw.h"

/* Intra prediction modes (Table 8-1). */
#define H265SW_INTRA_PLANAR 0U
#define H265SW_INTRA_DC 1U
#define H265SW_INTRA_HOR 10U
#define H265SW_INTRA_VER 26U

/*
 * The reference samples of one transform block of size n (4..32): the
 * 4 * n + 1 samples p[ -1 ][ 2n - 1 ] up to p[ -1 ][ -1 ] and on to
 * p[ 2n - 1 ][ -1 ], stored as sample[0] = p[ -1 ][ 2n - 1 ] (the
 * bottom of the left column) ... sample[2n - 1] = p[ -1 ][ 0 ],
 * sample[2n] = p[ -1 ][ -1 ], sample[2n + 1] = p[ 0 ][ -1 ] ...
 * sample[4n] = p[ 2n - 1 ][ -1 ] (the order the substitution of
 * 8.4.4.2.2 walks).  available[k] is 1 when sample[k] was available
 * ("available for intra prediction" of 8.4.4.2.1 with
 * constrained_intra_pred_flag applied by the caller).
 */
struct h265sw_intra_ref {
	uint8_t sample[4U * 32U + 1U];
	uint8_t available[4U * 32U + 1U];
};

/*
 * Derives IntraPredModeY of a prediction block (8.4.2 step 3 and 4,
 * 8-21 to 8-31).  cand_a and cand_b are candIntraPredModeA and B as
 * step 2 gives them (the caller applies availability, non-intra, PCM and
 * the CTB-row rule for B, each giving INTRA_DC).  prev_flag, mpm_idx
 * (0..2) and rem_mode (0..31) are the syntax elements.  Returns 0..34.
 */
unsigned h265sw_intra_luma_mode(unsigned cand_a, unsigned cand_b, int prev_flag, unsigned mpm_idx, unsigned rem_mode);

/* Derives IntraPredModeC for 4:2:0 (8.4.3, Table 8-2) from intra_chroma_pred_mode (0..4) and IntraPredModeY (0..34). */
unsigned h265sw_intra_chroma_mode(unsigned chroma_pred_mode, unsigned luma_mode);

/*
 * Substitutes the unavailable reference samples (8.4.4.2.2): none
 * available gives 1 << ( BitDepth - 1 ) everywhere; otherwise a search
 * from sample[0] upwards for the first available one, which fills
 * sample[0] when that is unavailable, and each later unavailable sample
 * copies the one before it.  n is 4..32.
 */
void h265sw_intra_substitute(struct h265sw_intra_ref *ref, unsigned n);

/*
 * Filters the reference samples when 8.4.4.2.1 step 1 and 8.4.4.2.3 say
 * so: only for luma (c_idx 0; chroma of 4:2:0 is never filtered), mode
 * not DC, n above 4, and Min( Abs( mode - 26 ), Abs( mode - 10 ) )
 * above intraHorVerDistThres[ n ] (Table 8-4).  With strong_smoothing
 * (strong_intra_smoothing_enabled_flag), n 32 and the two flatness
 * tests of biIntFlag, the bi-linear strong filter (8-36 to 8-40); else
 * the [1 2 1] filter (8-41 to 8-45).
 */
void h265sw_intra_filter(struct h265sw_intra_ref *ref, unsigned n, unsigned c_idx, unsigned mode, int strong_smoothing);

/*
 * Predicts the n by n samples of a transform block with the given mode
 * from (filtered) reference samples (8.4.4.2.4 to 8.4.4.2.6) and writes
 * them to out (out_stride bytes per row).
 *
 * Planar: 8-46.  DC: 8-47 to 8-52, with the edge filtering for luma and
 * n below 32.  Angular: 8-53 to 8-68: intraPredAngle (Table 8-5) and
 * invAngle (Table 8-6) build ref[] (with the projection of the other
 * side for negative angles), then iIdx, iFact and the two-tap
 * interpolation; for modes 26 and 10 with luma and n below 32 the first
 * column or row is adjusted by the gradient (disableIntraBoundaryFilter
 * is 0: no range extension tools).  Negative intermediate values in the
 * index arithmetic use vcodec_asr32.
 */
void h265sw_intra_predict(const struct h265sw_intra_ref *ref, unsigned n, unsigned c_idx, unsigned mode, uint8_t *out, size_t out_stride);

#endif
