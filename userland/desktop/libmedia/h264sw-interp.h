/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fractional sample interpolation of the software H.264 decoder
 * (8.4.2.2): the luma samples at quarter-sample positions with the
 * six-tap filter (8.4.2.2.1, Table 8-12) and the chroma samples at
 * eighth-sample positions (8.4.2.2.2), with the reference sample
 * positions clamped to the picture as 8-239, 8-240 and 8-262 to 8-269 say,
 * so a motion vector may point anywhere.
 *
 * The functions are pure (they read a reference frame's plane and write
 * a block); the prediction of a partition calls them once per list and
 * then weights the results (h264sw-weight.h).
 */

#ifndef LIBMEDIA_H264SW_INTERP_H
#define LIBMEDIA_H264SW_INTERP_H

#include "h264sw.h"

/*
 * Predicts a block of luma samples from a reference plane (8.4.2.2.1).
 *
 * plane is the reference luma plane of plane_width by plane_height samples with
 * stride bytes per row.  (x, y) is the full-sample location of the
 * block's upper-left sample plus the integer part of the motion vector:
 * xIntL = xAL + ( mvLX[0] >> 2 ) + xL and yIntL likewise (8-223, 8-224
 * with xAL, yAL the partition's location in the picture); frac_x and
 * frac_y are mvLX[0] & 3 and mvLX[1] & 3 (8-225, 8-226).  width and height are the
 * block size (4, 8 or 16).  The samples A..U of Figure 8-4 are read at
 * Clip3( 0, plane_width - 1, x ) and Clip3( 0, plane_height - 1, y )
 * (8-239, 8-240); b, h, j, s, m and the quarter positions a, c, d, n, f, i, k,
 * q, e, g, p, r are derived by 8-241 to 8-261 and selected by Table
 * 8-12.  out receives width by height samples, out_stride bytes per row.
 *
 * x and y may be any int (a motion vector far outside the picture
 * clamps to the edge); no read leaves the plane.
 */
void h264sw_interp_luma(const uint8_t *plane, size_t stride, unsigned plane_width, unsigned plane_height, int x, int y, unsigned frac_x, unsigned frac_y, unsigned width, unsigned height, uint8_t *out, size_t out_stride);

/*
 * Predicts a block of chroma samples from a reference plane (8.4.2.2.2).
 *
 * plane is a reference chroma plane (Cb or Cr) of plane_width by
 * plane_height samples.  (x, y) is xIntC and yIntC: the block's chroma
 * location plus ( mvCLX[0] >> 3 ) and ( mvCLX[1] >> 3 ) (8-227, 8-228
 * for 4:2:0, where mvCLX is the chroma vector of 8.4.1.4: equal to mvLX
 * for frames, in eighth chroma samples); frac_x and frac_y are mvCLX[0]
 * & 7 and mvCLX[1] & 7 (8-229, 8-230).  The samples A, B, C, D are read
 * at the clamped positions of 8-262 to 8-269 (refPicHeightEffectiveC is
 * the plane's height for frames) and combined by 8-270 ( ( 8 - xFracC ) * (
 * 8 - yFracC ) * A + ... + 32 ) >> 6.  width and height are 2, 4 or 8.
 */
void h264sw_interp_chroma(const uint8_t *plane, size_t stride, unsigned plane_width, unsigned plane_height, int x, int y, unsigned frac_x, unsigned frac_y, unsigned width, unsigned height, uint8_t *out, size_t out_stride);

#endif
