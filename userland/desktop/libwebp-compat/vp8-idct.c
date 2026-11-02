/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inverse WHT and DCT of the lossy format.
 */

#include "vp8-idct.h"

#include <errno.h>

/*
 * Inverts the WHT of a Y2 block (RFC 6386 14.3, vp8_short_inv_walsh4x4_c):
 * input the 16 dequantized coefficients in raster order; dc[i] receives the
 * DC coefficient of Y block i (raster), (a + 3) >> 3 of the second pass,
 * wrapped to 16 bits.
 */
void
webpc_vp8_iwht(
	const int16_t input[16],
	int16_t dc[16])
{
	UNUSED_PARAMETER(input);
	UNUSED_PARAMETER(dc);

	/* Not implemented yet. */
}

/*
 * Inverts the DCT of one 4x4 block (RFC 6386 14.4, short_idct4x4llm_c) and
 * adds it to the prediction at dst (RFC 6386 14.5), stride bytes from one
 * row to the next, each sum clamped to 0..255.
 */
void
webpc_vp8_idct_add(
	const int16_t input[16],
	uint8_t *dst,
	ptrdiff_t stride)
{
	UNUSED_PARAMETER(input);
	UNUSED_PARAMETER(dst);
	UNUSED_PARAMETER(stride);

	/* Not implemented yet. */
}
