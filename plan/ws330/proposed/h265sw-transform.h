/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The quantization parameters and the residual of one transform block in
 * the software H.265 decoder: QpY, Qp'Cb and Qp'Cr from the predicted QP
 * (8-282 to 8-290, Table 8-10), the scaling of the coefficient levels
 * (8.6.3), the inverse transforms (8.6.4: the DST of 4x4 intra luma
 * blocks and the DCT of 4 to 32), transform skip and transquant bypass
 * (8.6.2), giving the residual samples.
 *
 * The prediction of the QP from the neighbouring quantization groups
 * (8.6.1 steps 1 to 4) needs the picture and is h265sw-recon.h's; what is
 * here is pure arithmetic.
 */

#ifndef LIBMEDIA_H265SW_TRANSFORM_H
#define LIBMEDIA_H265SW_TRANSFORM_H

#include "h265sw.h"

/*
 * The quantization parameters of a coding unit: qp_y QpY (0..51),
 * qp_prime[cIdx] Qp'Y, Qp'Cb and Qp'Cr (8-284, 8-289, 8-290; QpBdOffset
 * is 0 for 8 bits).
 */
struct h265sw_qp {
	int qp_y;
	int qp_prime[3];
};

/*
 * Derives the quantization parameters from qPY_PRED (8-282, given by the
 * caller), CuQpDeltaVal (-26..25) and the chroma offsets cb_offset =
 * pps_cb_qp_offset + slice_cb_qp_offset and cr_offset likewise
 * (CuQpOffsetCb and Cr are 0: no chroma QP offset lists): QpY by 8-283,
 * qPiCb and qPiCr by 8-285 and 8-286 (clipped to -QpBdOffsetC..57), QpC
 * by Table 8-10.
 */
void h265sw_transform_qp(int qp_y_pred, int cu_qp_delta, int cb_offset, int cr_offset, struct h265sw_qp *qp);

/* Returns QpC of Table 8-10 for qPi (-QpBdOffsetC..57). */
int h265sw_transform_qpc(int qpi);

/*
 * Scales the coefficient levels of an n by n block (8.6.3, 8-300 to
 * 8-309): bdShift = BitDepth + Log2( n ) + 10 - 15, levelScale = { 40,
 * 45, 51, 57, 64, 72 }, m from matrix (rows of n entries, [ y * n + x ])
 * or 16 when matrix is NULL, d = Clip3( -32768, 32767, ( level * m *
 * levelScale[ qp % 6 ] << ( qp / 6 ) ) + ( 1 << ( bdShift - 1 ) ) ) >>
 * bdShift ), computed in int64_t.  qp is qP (0..51).  levels and d are
 * [ y * n + x ]; log2_size 2..5.
 */
void h265sw_transform_scale(const int16_t *levels, unsigned log2_size, int qp, const uint8_t *matrix, int32_t *d);

/*
 * The inverse transform of a scaled block (8.6.4): columns first by the
 * one-dimensional transform of 8.6.4.2 (trType 1: the 4x4 DST of 8-315
 * and 8-316; trType 0: y[ i ] = Sum of transMatrix[ i ][ j * 2^( 5 -
 * Log2( n ) ) ] * x[ j ] (8-317), the 32x32 transMatrix of 8-318 to
 * 8-321 being data of this module), the intermediate clip of 8-314 (( e + 64 )
 * >> 7 to -32768..32767), then the rows.  The output is r before the
 * final bdShift of 8-299, in int32_t, [ y * n + x ].
 *
 * Values referenced: ITU-T H.265 (01/2026) 8.6.4.2, 8-316 and the
 * transMatrix of 8-318 to 8-321.
 */
void h265sw_transform_inverse(const int32_t *d, unsigned log2_size, unsigned tr_type, int32_t *r);

/*
 * The residual of one transform block (8.6.2).
 *
 * levels are TransCoeffLevel ([ y * n + x ], n = 1 << log2_size);
 * c_idx and intra choose trType (1 for intra luma 4x4, 8.6.4.1);
 * bypass is cu_transquant_bypass_flag (r = levels); transform_skip is
 * transform_skip_flag (r = d << tsShift, tsShift = 5 + Log2( n ), as a
 * product); otherwise the scaling and the inverse transform.  The final
 * ( r + ( 1 << ( bdShift - 1 ) ) ) >> bdShift with bdShift = 20 -
 * BitDepth (8-295, 8-299) applies to all but bypass and uses
 * vcodec_asr32.  qp is qP of 8-291 to 8-293; matrix as for
 * h265sw_transform_scale (the caller passes NULL for the flat cases:
 * scaling lists off, or transform skip with n above 4).  residual
 * receives n * n values in -2^15..2^15 - 1, [ y * n + x ].
 */
void h265sw_transform_residual(const int16_t *levels, unsigned log2_size, unsigned c_idx, int intra, int bypass, int transform_skip, int qp, const uint8_t *matrix, int16_t *residual);

#endif
