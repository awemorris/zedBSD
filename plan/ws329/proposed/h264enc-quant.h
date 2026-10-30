/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The forward transforms and the quantization of the software H.264
 * encoder.  The specification defines only the decoder's side (the
 * scaling and the inverse transforms of 8.5.9 to 8.5.13, h264sw-transform.h);
 * the forward side here is the encoder's own, built so that the
 * decoder's inverse undoes it up to the quantization:
 *
 * - 4x4: W = Cf * X * Cf^T with Cf = [ 1 1 1 1; 2 1 -1 -2; 1 -1 -1 1;
 *   1 -2 2 -1 ] (rows), exactly in integers.  Cf * Ci = diag( 4, 5, 4, 5 )
 *   for the decoder's inverse Ci of 8.5.12.2, so with the flat scaling
 *   list (16) the factor that makes Z * LevelScale4x4 reproduce W is
 *   MF4( m, i, j ) = Round( 2^21 / ( v( m, k ) * s_i * s_j ) ) with s 4
 *   for even and 5 for odd indices and v, k the normAdjust4x4 table and
 *   column of 8-315 and 8-314 (h264sw_norm4x4); qbits = 15 + qP / 6.
 *   (MF4( 0, . ) is 13107, 5243 and 8066 for k 0, 1, 2: a check of the
 *   formula, not data.)
 * - 8x8: W = Tf * X * Tf^T with Tf the transpose of eight times the
 *   decoder's 8x8 inverse basis of 8.5.13.2 (rows 8 8 8 8 8 8 8 8; 12 10
 *   6 3 -3 -6 -10 -12; 8 4 -4 -8 -8 -4 4 8; 10 -3 -12 -6 6 12 3 -10; 8
 *   -8 -8 8 8 -8 -8 8; 6 -12 3 10 -10 -3 12 -6; 4 -8 8 -4 -4 8 -8 4; 3
 *   -6 10 -12 12 -10 6 -3), exactly in integers.  Tf * Ti = diag( E ) / 4
 *   with E_i 256 for i % 4 == 0, 289 for odd i, 160 for i % 4 == 2, so
 *   MF8( m, i, j ) = Round( 2^32 / ( v8( m, k ) * E_i * E_j ) ) with v8, k
 *   the normAdjust8x8 table and column of 8-318 and 8-317
 *   (h264sw_norm8x8); qbits = 20 + qP / 6.
 * - The Intra_16x16 luma DC: the sixteen W00 of the macroblock's 4x4
 *   blocks as a 4x4 matrix D (row = block row), Y = ( H * D * H ) / 2
 *   with H = [ 1 1 1 1; 1 1 -1 -1; 1 -1 -1 1; 1 -1 1 -1 ] and the halving
 *   rounded half away from zero; quantized with MF4( m, 0, 0 ) and qbits
 *   + 1.  The chroma DC of 4:2:0: the four W00 of a component as a 2x2
 *   matrix, Y = H2 * D * H2 with H2 = [ 1 1; 1 -1 ], quantized with
 *   MF4( m, 0, 0 ) and qbits + 1.  These match the decoder's 8.5.10 and
 *   8.5.11.
 * - Quantization: level = sign( W ) * ( ( |W| * MF + f ) >> qbits ), with
 *   |W| * MF in uint64_t, and the rounding offset f = 2^qbits / 3 for
 *   intra and 2^qbits / 6 for inter blocks (the DC paths use 2f with their
 *   qbits + 1).  The offsets are this encoder's choice (the usual dead
 *   zone of H.264 encoders), not the specification's.
 *
 * The encoder never sends scaling matrices, so every scaling list is flat
 * and these factors are the whole story.  All functions are pure.
 * Arrays of coefficients are raster order (index 4 * i + j or 8 * i + j,
 * row i), levels are in scan order (h264sw_zigzag4x4, h264sw_zigzag8x8).
 */

#ifndef LIBMEDIA_H264ENC_QUANT_H
#define LIBMEDIA_H264ENC_QUANT_H

#include "h264sw.h"

/*
 * The largest magnitude of a level CAVLC can send when level_prefix is at
 * most 15 (A.2.1 to A.2.3, profiles other than High): with suffixLength
 * 0, level_prefix 15 and a 12-bit level_suffix, levelCode reaches 4125
 * (9.2.2.1), so |level| 2063 always fits.  The encoder clamps levels to
 * it when it writes CAVLC in those profiles.
 */
#define H264ENC_CAVLC_LEVEL_LIMIT 2063

/* The factors MF4( m, raster position ) and MF8( m, raster position ) for m = qP % 6, computed by h264enc_quant_init. */
struct h264enc_quant {
	uint32_t mf4[6][16];
	uint32_t mf8[6][64];
};

/* Computes the factors by the formulas above (integer arithmetic only: the rounding is ( 2 * num + den ) / ( 2 * den ) in uint64_t). */
void h264enc_quant_init(struct h264enc_quant *quant);

/*
 * The forward 4x4 transform of a residual: coef[4 * i + j] = W(i, j) of
 * X = source - prediction for the 4x4 block at source and prediction
 * (each with its stride).
 */
void h264enc_fdct4x4(const uint8_t *source, size_t source_stride, const uint8_t *prediction, size_t prediction_stride, int32_t *coef);

/* The forward 8x8 transform of a residual, as h264enc_fdct4x4. */
void h264enc_fdct8x8(const uint8_t *source, size_t source_stride, const uint8_t *prediction, size_t prediction_stride, int32_t *coef);

/* Transforms the 16 luma DC values (dc[4 * row + column] = W00 of the block at that row and column) into Y in place, as above. */
void h264enc_luma_dc_transform(int32_t *dc);

/* Transforms the four chroma DC values (dc[b] = W00 of chroma4x4BlkIdx b) into Y in place, as above. */
void h264enc_chroma_dc_transform(int32_t *dc);

/*
 * Quantizes a 4x4 block.
 *
 * coef holds W in raster order; qp is qP (0..51); intra selects the
 * rounding offset; first is 0 for a whole block and 1 for an AC block
 * (Intra_16x16 or chroma), whose position 0 is not quantized (its level
 * is 0: the DC goes its own way).  levels receives 16 levels in zig-zag
 * scan order.  Returns the number of non-zero levels.
 */
unsigned h264enc_quant4x4(const struct h264enc_quant *quant, const int32_t *coef, int qp, int intra, unsigned first, int32_t *levels);

/* Quantizes an 8x8 block into 64 levels in 8x8 zig-zag scan order; returns the non-zero count. */
unsigned h264enc_quant8x8(const struct h264enc_quant *quant, const int32_t *coef, int qp, int intra, int32_t *levels);

/* Quantizes the transformed luma DC (raster order) into 16 levels in zig-zag scan order (Intra16x16DCLevel); returns the non-zero count. */
unsigned h264enc_quant_luma_dc(const struct h264enc_quant *quant, const int32_t *dc, int qp, int32_t *levels);

/* Quantizes the transformed chroma DC of a component into 4 levels (chroma DC levels 0..3 as c00, c01, c10, c11); qp is QP'C; intra selects the offset; returns the non-zero count. */
unsigned h264enc_quant_chroma_dc(const struct h264enc_quant *quant, const int32_t *dc, int qp, int intra, int32_t *levels);

/* Clamps each of count levels to -limit..limit; returns how many changed. */
unsigned h264enc_clamp_levels(int32_t *levels, unsigned count, int32_t limit);

#endif
