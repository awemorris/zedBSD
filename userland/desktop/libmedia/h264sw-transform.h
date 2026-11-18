/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transform coefficient decoding of the software H.264 decoder
 * (8.5): the scaling functions (8.5.9), the inverse scans (8.5.6,
 * 8.5.7), the scaling and the inverse transforms of 4x4 blocks
 * (8.5.12), 8x8 blocks (8.5.13), the Intra_16x16 DC (8.5.10) and the
 * chroma DC of 4:2:0 (8.5.11), and the construction of samples from the
 * prediction and the residual (8.5.14).
 *
 * Every function here is pure: it reads its arguments and writes its
 * outputs, so each can be tested alone.  Coefficient levels and
 * intermediate values are int32_t.  A stream whose values break the
 * ranges the clauses state still gives defined results: a product of a
 * level and a scale is computed in int64_t, the scaling's left shift is a
 * multiplication (h264sw.h), and every value the clauses bound (cij,
 * dij, eij, fij, gij, hij, dcYij, dcCij: -2^( 7 + bitDepth ) ..
 * 2^( 7 + bitDepth ) - 1, that is -32768..32767) is clamped to its range
 * as soon as it is computed, before it is stored in int32_t or used
 * again.  A conforming stream never reaches a clamp, so the results are
 * the clauses' own.
 *
 * The 4x4 and 8x8 arrays are in raster order: element (i, j), row i and
 * column j, is at index 4 * i + j (or 8 * i + j), as cij of the clauses.
 */

#ifndef LIBMEDIA_H264SW_TRANSFORM_H
#define LIBMEDIA_H264SW_TRANSFORM_H

#include "h264sw.h"
#include "h264sw-ps.h"

/*
 * The scaling functions of a picture (8.5.9): LevelScale4x4( m, i, j )
 * for each of the six 4x4 lists (index as ScalingList4x4: Intra Y, Cb,
 * Cr, Inter Y, Cb, Cr) and m = qP % 6, and LevelScale8x8 for the two
 * 8x8 lists (Intra Y, Inter Y), in raster order.
 */
struct h264sw_levelscale {
	int32_t scale4x4[6][6][16];
	int32_t scale8x8[2][6][64];
};

/*
 * Computes the scaling functions from a picture's scaling lists (8.5.9).
 *
 * weightScale4x4 is the list inverse-scanned by the zig-zag scan
 * (8.5.6), weightScale8x8 by the 8x8 zig-zag scan (8.5.7);
 * LevelScale4x4( m, i, j ) = weightScale4x4( i, j ) * normAdjust4x4( m,
 * i, j ) (8-313, 8-314 with h264sw_norm4x4), and the 8x8 one likewise
 * (8-316, 8-317 with h264sw_norm8x8).
 */
void h264sw_levelscale_init(struct h264sw_levelscale *levelscale, const struct h264sw_scaling *scaling);

/*
 * Places a 4x4 block's levels from scan order to raster order (8.5.6, zig-zag).
 *
 * levels[idx] is the level at scan position idx (0..15); block[4 * i +
 * j] receives cij.
 */
void h264sw_unscan4x4(const int32_t *levels, int32_t *block);

/* Places an 8x8 block's 64 levels from scan order to raster order (8.5.7, 8x8 zig-zag). */
void h264sw_unscan8x8(const int32_t *levels, int32_t *block);

/*
 * Scales a 4x4 block of levels (8.5.12.1).
 *
 * block holds cij in raster order and receives dij.  scale is
 * levelscale->scale4x4[list][qp % 6] for the block's list; qp is qP
 * (QP'Y for luma, QP'C for chroma, 8-330 and 8-332; with 8 bits QP'Y =
 * QPY).  dc_done is 1
 * for a block whose c00 is a DC already scaled by 8.5.10 or 8.5.11
 * (Intra_16x16 luma, chroma): d00 = c00 is kept; else d00 is scaled like
 * the others (8-335).  For qP >= 24: dij = ( cij * LevelScale4x4 ) << (
 * qP / 6 - 4 ) (8-336); else ( cij * LevelScale4x4 + 2^( 3 - qP / 6 ) ) >>
 * ( 4 - qP / 6 ) (8-337).
 */
void h264sw_scale4x4(int32_t *block, const int32_t *scale, int qp, int dc_done);

/*
 * Transforms a scaled 4x4 block into residual samples (8.5.12.2).
 *
 * block holds dij and receives rij = ( hij + 32 ) >> 6 (8-354), after
 * the row transforms (8-338 to 8-345) and the column transforms (8-346
 * to 8-353).
 */
void h264sw_idct4x4(int32_t *block);

/*
 * Scales an 8x8 block of levels (8.5.13.1).
 *
 * block holds cij and receives dij; scale is
 * levelscale->scale8x8[list][qp % 6]; for qP >= 36: dij = ( cij *
 * LevelScale8x8 ) << ( qP / 6 - 6 ); else ( cij * LevelScale8x8 + 2^( 5 -
 * qP / 6 ) ) >> ( 6 - qP / 6 ).
 */
void h264sw_scale8x8(int32_t *block, const int32_t *scale, int qp);

/* Transforms a scaled 8x8 block into residual samples (8.5.13.2: rows, then columns, then rij = ( mij + 32 ) >> 6). */
void h264sw_idct8x8(int32_t *block);

/*
 * Transforms and scales the 16 DC levels of an Intra_16x16 macroblock (8.5.10).
 *
 * dc holds the levels c as a 4x4 matrix in raster order (the
 * Intra16x16DCLevel list placed by the zig-zag inverse scan of 8.5.6)
 * and receives dcY: f = the transform of 8-320, then for qP >= 36
 * dcYij = ( fij * LevelScale4x4( qP % 6, 0, 0 ) ) << ( qP / 6 - 6 )
 * (8-321), else ( fij * LevelScale4x4( qP % 6, 0, 0 ) + 2^( 5 - qP / 6 ) )
 * >> ( 6 - qP / 6 ) (8-322).  scale00 is LevelScale4x4( qP % 6, 0, 0 ) of the Intra Y list.
 * dcYij is the DC of the 4x4 block at row i and column j of the
 * macroblock (luma4x4BlkIdx of (4 * j, 4 * i), 8.5.2).
 */
void h264sw_luma_dc(int32_t *dc, int32_t scale00, int qp);

/*
 * Transforms and scales the four DC levels of a 4:2:0 chroma component (8.5.11).
 *
 * dc holds c as a 2x2 matrix in raster order (chroma DC levels 0..3 as
 * c00, c01, c10, c11, the inverse raster scan of 8-304) and receives
 * dcC: f = the 2x2 transform of 8-324, then dcCij = ( ( fij *
 * LevelScale4x4( qP % 6, 0, 0 ) ) << ( qP / 6 ) ) >> 5 (8-326); qP is
 * QP'C of the component and scale00 LevelScale4x4( qP % 6, 0, 0 ) of
 * its list (intra or inter Cb or Cr).  dcC of chroma4x4BlkIdx b is
 * dc[b].
 */
void h264sw_chroma_dc(int32_t *dc, int32_t scale00, int qp);

/*
 * Adds a residual block to its prediction and clips (8.5.14).
 *
 * pred is the predicted samples (pred_stride bytes per row), residual
 * the size by size residual in raster order (size 4 or 8), out receives
 * Clip1( pred + residual ) with Clip1 to 0..255; out may be pred.
 */
void h264sw_add_residual(const uint8_t *pred, size_t pred_stride, const int32_t *residual, unsigned size, uint8_t *out, size_t out_stride);

#endif
