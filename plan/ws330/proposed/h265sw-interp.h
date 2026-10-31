/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fractional sample interpolation and the weighted sample prediction
 * of the software H.265 decoder (8.5.3.3.3, 8.5.3.3.4).
 *
 * Interpolation gives the prediction samples of one block and one list at
 * the intermediate precision of 14 bits (predSamplesLX, in int16_t):
 * luma at quarter-sample positions with the 8-tap filters (8-224 to
 * 8-238, Table 8-8), chroma at eighth-sample positions with the 4-tap
 * filters (8.5.3.3.3.3, Table 8-9), reading the reference picture at
 * positions clamped to it (8-222, 8-223, 8-239, 8-240), so a motion
 * vector may point anywhere.  The weighting turns one or two such blocks
 * into 8-bit samples: the default weighting (8.5.3.3.4.2) or the explicit
 * one (8.5.3.3.4.3).
 *
 * The functions are pure.
 */

#ifndef LIBMEDIA_H265SW_INTERP_H
#define LIBMEDIA_H265SW_INTERP_H

#include "h265sw.h"

/*
 * Predicts a width by height block of luma samples (8.5.3.3.3.2).
 *
 * plane (stride bytes per row) is the reference picture's luma of
 * plane_width by plane_height samples (pic_width_in_luma_samples by
 * pic_height_in_luma_samples).  ( x, y ) is ( xIntL, yIntL ) of the
 * block's first sample: the block's position plus ( mvLX[ 0 ] >> 2,
 * mvLX[ 1 ] >> 2 ) (8-214, 8-215; the caller uses vcodec_asr32); frac_x
 * and frac_y are mvLX & 3.  Full-sample positions give A << shift3 (shift3
 * = 14 - BitDepthY); the others the filters of 8-224 to 8-238 with
 * shift1 = BitDepthY - 8 (0) and shift2 = 6, chosen by Table 8-8.  x and
 * y may be any int; every read is clamped into the plane (8-222,
 * 8-223).  width and height are 4..64 (prediction block sizes of 8.5.3,
 * the chroma call halves them).  out receives width * height values,
 * out_stride elements per row.
 */
void h265sw_interp_luma(const uint8_t *plane, size_t stride, unsigned plane_width, unsigned plane_height, int x, int y, unsigned frac_x, unsigned frac_y, unsigned width, unsigned height, int16_t *out, size_t out_stride);

/*
 * Predicts a width by height block of chroma samples (8.5.3.3.3.3) of
 * one chroma plane (plane_width by plane_height: the luma size halved
 * for 4:2:0).  ( x, y ) is ( xIntC, yIntC ): the chroma position plus (
 * mvCLX[ 0 ] >> 3, mvCLX[ 1 ] >> 3 ) (8-218, 8-219) with mvCLX = mvLX for 4:2:0
 * (8.5.3.2.10, in eighth chroma samples); frac_x and frac_y are mvCLX &
 * 7.  The 4-tap filter coefficients of each eighth position (the
 * equations of 8.5.3.3.3.3) are data of this module; the assignment is
 * Table 8-9.  width and height are 2..32.
 *
 * Values referenced: ITU-T H.265 (01/2026) 8.5.3.3.3.3 (the chroma
 * filter coefficients).
 */
void h265sw_interp_chroma(const uint8_t *plane, size_t stride, unsigned plane_width, unsigned plane_height, int x, int y, unsigned frac_x, unsigned frac_y, unsigned width, unsigned height, int16_t *out, size_t out_stride);

/*
 * Default weighted sample prediction (8.5.3.3.4.2, 8-262 to 8-264) of a width by height
 * block: with b NULL, uni-prediction Clip3( 0, 255, ( a + offset1 ) >>
 * shift1 ) (shift1 = 14 - BitDepth = 6); with both, ( a + b + offset2 )
 * >> shift2 (shift2 = 15 - BitDepth = 7), clipped.  a and b have
 * ab_stride elements per row; out out_stride bytes.  Right shifts of
 * negative sums use vcodec_asr32.
 */
void h265sw_weight_default(const int16_t *a, const int16_t *b, size_t ab_stride, unsigned width, unsigned height, uint8_t *out, size_t out_stride);

/*
 * Explicit weighted sample prediction (8.5.3.3.4.3, 8-265 to 8-277).
 *
 * log2_denom is luma_log2_weight_denom or ChromaLog2WeightDenom (0..7);
 * log2WD = log2_denom + shift1 (shift1 = 14 - BitDepth).  With b NULL,
 * uni-prediction with w0 and o0 (rounding by 2^( log2WD - 1 ) when
 * log2WD >= 1, then + o0, clipped); with both, ( a * w0 + b * w1 + ( ( o0 + o1 + 1 )
 * << log2WD ) ) >> ( log2WD + 1 ), clipped.  w0, w1 are LumaWeightLX or
 * ChromaWeightLX (-128..255); o0, o1 the offsets already shifted by
 * WpOffsetBdShift (0 for 8 bits).  Products in int32_t fit (14-bit
 * samples times 9-bit weights); the shifts of negative values use
 * vcodec_asr32.
 */
void h265sw_weight_explicit(const int16_t *a, const int16_t *b, size_t ab_stride, unsigned width, unsigned height, unsigned log2_denom, int w0, int o0, int w1, int o1, uint8_t *out, size_t out_stride);

#endif
