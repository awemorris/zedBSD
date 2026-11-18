/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inverse transforms of the lossy format (RFC 6386 section 14): the
 * Walsh-Hadamard transform of the Y2 block and the 4x4 inverse DCT added
 * to a prediction.
 */

#ifndef LIBWEBP_COMPAT_VP8_IDCT_H
#define LIBWEBP_COMPAT_VP8_IDCT_H

#include "common.h"

/*
 * Inverts the WHT of a Y2 block (RFC 6386 14.3, vp8_short_inv_walsh4x4_c):
 * input the 16 dequantized coefficients in raster order; dc[i] receives
 * the DC coefficient of Y block i (raster), (a + 3) >> 3 of the second
 * pass, wrapped to 16 bits.  The first pass's results are wrapped to 16
 * bits as the reference decoder keeps them in shorts (RFC 6386 14.3,
 * 20.8); a valid stream never reaches the wrap, so the output is the
 * same as with int32_t intermediates.  ">>" is webpc_asr.
 */
void webpc_vp8_iwht(const int16_t input[16], int16_t dc[16]);

/*
 * Inverts the DCT of one 4x4 block (RFC 6386 14.4, short_idct4x4llm_c)
 * and adds it to the prediction at dst (RFC 6386 14.5), stride bytes from
 * one row to the next, each sum clamped to 0..255.  input is the 16
 * dequantized coefficients in raster order.  The two passes are 14.4's
 * (vertical first, then horizontal, with 20091 and 35468 in 16-bit fixed
 * point and the final (x + 4) >> 3); the first pass's results are
 * wrapped to 16 bits (webpc_wrap16) as the reference keeps them in
 * shorts, so no product of the second pass leaves int32_t.  Every ">>"
 * of a signed value is webpc_asr.
 */
void webpc_vp8_idct_add(const int16_t input[16], uint8_t *dst, ptrdiff_t stride);

#endif
