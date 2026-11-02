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

/*
 * The two constants of the inverse DCT, in 16-bit fixed point.
 *
 * Values referenced: RFC 6386 (November 2011) section 14.4
 * (cospi8sqrt2minus1 is sqrt(2) * cos(pi/8) - 1 times 65536, sinpi8sqrt2
 * is sqrt(2) * sin(pi/8) times 65536).
 */
#define IDCT_COS_MINUS_ONE	20091
#define IDCT_SIN		35468

static void idct_butterfly(const int32_t in[4], int32_t out[4]);

/*
 * Applies the one-dimensional inverse DCT to four values.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 14.4
 * (short_idct4x4llm_c, the body of either pass).
 *
 * in holds the four inputs, each in the 16-bit range; out receives the
 * four results before any rounding: out[0] and out[3] are a + d and a - d,
 * out[1] and out[2] are b + c and b - c.
 */
static void
idct_butterfly(
	const int32_t in[4],
	int32_t out[4])
{
	int32_t sum_even;
	int32_t difference_even;
	int32_t rotated_low;
	int32_t rotated_high;
	int32_t odd_low;
	int32_t odd_high;

	/* The even inputs combine without a multiplication. */
	sum_even = in[0] + in[2];
	difference_even = in[0] - in[2];

	/*
	 * The odd inputs are rotated.  A 16-bit input times either constant
	 * stays inside int32_t, and the shift is the floor of webpc_asr.
	 */
	rotated_low = webpc_asr(in[1] * IDCT_SIN, 16);
	odd_low = in[3] + webpc_asr(in[3] * IDCT_COS_MINUS_ONE, 16);
	rotated_high = in[1] + webpc_asr(in[1] * IDCT_COS_MINUS_ONE, 16);
	odd_high = webpc_asr(in[3] * IDCT_SIN, 16);

	/* The four outputs: the even part plus or minus the rotated odd part. */
	out[0] = sum_even + (rotated_high + odd_high);
	out[3] = sum_even - (rotated_high + odd_high);
	out[1] = difference_even + (rotated_low - odd_low);
	out[2] = difference_even - (rotated_low - odd_low);
}

/*
 * Inverts the WHT of a Y2 block (RFC 6386 14.3, vp8_short_inv_walsh4x4_c):
 * input the 16 dequantized coefficients in raster order; dc[i] receives the
 * DC coefficient of Y block i (raster), (a + 3) >> 3 of the second pass,
 * wrapped to 16 bits.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 14.3
 * (vp8_short_inv_walsh4x4_c).
 */
void
webpc_vp8_iwht(
	const int16_t input[16],
	int16_t dc[16])
{
	int32_t middle[16];
	int32_t sum_outer;
	int32_t sum_inner;
	int32_t difference_inner;
	int32_t difference_outer;
	int32_t value;
	int index;

	/*
	 * First pass, down each column.  The reference keeps these results in
	 * 16-bit values, so each is wrapped before the second pass reads it.
	 */
	for (index = 0; index < 4; index++) {
		sum_outer = (int32_t)input[index] + (int32_t)input[index + 12];
		sum_inner = (int32_t)input[index + 4] + (int32_t)input[index + 8];
		difference_inner = (int32_t)input[index + 4] - (int32_t)input[index + 8];
		difference_outer = (int32_t)input[index] - (int32_t)input[index + 12];

		middle[index] = webpc_wrap16(sum_outer + sum_inner);
		middle[index + 4] = webpc_wrap16(difference_inner + difference_outer);
		middle[index + 8] = webpc_wrap16(sum_outer - sum_inner);
		middle[index + 12] = webpc_wrap16(difference_outer - difference_inner);
	}

	/* Second pass, along each row, with the rounding and the 16-bit store. */
	for (index = 0; index < 4; index++) {
		sum_outer = middle[index * 4] + middle[index * 4 + 3];
		sum_inner = middle[index * 4 + 1] + middle[index * 4 + 2];
		difference_inner = middle[index * 4 + 1] - middle[index * 4 + 2];
		difference_outer = middle[index * 4] - middle[index * 4 + 3];

		value = webpc_asr(sum_outer + sum_inner + 3, 3);
		dc[index * 4] = (int16_t)webpc_wrap16(value);
		value = webpc_asr(difference_inner + difference_outer + 3, 3);
		dc[index * 4 + 1] = (int16_t)webpc_wrap16(value);
		value = webpc_asr(sum_outer - sum_inner + 3, 3);
		dc[index * 4 + 2] = (int16_t)webpc_wrap16(value);
		value = webpc_asr(difference_outer - difference_inner + 3, 3);
		dc[index * 4 + 3] = (int16_t)webpc_wrap16(value);
	}
}

/*
 * Inverts the DCT of one 4x4 block (RFC 6386 14.4, short_idct4x4llm_c) and
 * adds it to the prediction at dst (RFC 6386 14.5), stride bytes from one
 * row to the next, each sum clamped to 0..255.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 14.4
 * (short_idct4x4llm_c) and 14.5 (the summation of predictor and residue).
 */
void
webpc_vp8_idct_add(
	const int16_t input[16],
	uint8_t *dst,
	ptrdiff_t stride)
{
	int32_t middle[16];
	int32_t column_in[4];
	int32_t column_out[4];
	int32_t row_out[4];
	uint8_t *row_start;
	int32_t residue;
	int sum;
	int row;
	int column;

	/*
	 * First pass, down each column of the input.  The results are wrapped
	 * to 16 bits as the reference stores them in shorts.
	 */
	for (column = 0; column < 4; column++) {
		/* The column's four coefficients, top to bottom. */
		for (row = 0; row < 4; row++) {
			column_in[row] = (int32_t)input[row * 4 + column];
		}

		idct_butterfly(column_in, column_out);

		/* The four results go back into the same column. */
		for (row = 0; row < 4; row++) {
			middle[row * 4 + column] = webpc_wrap16(column_out[row]);
		}
	}

	/*
	 * Second pass, along each row of the first pass's result, rounded by
	 * (x + 4) >> 3 and added to the prediction row.
	 */
	for (row = 0; row < 4; row++) {
		row_start = dst + (ptrdiff_t)row * stride;
		idct_butterfly(&middle[row * 4], row_out);

		/* Each of the row's four residues is added to its pixel and clamped. */
		for (column = 0; column < 4; column++) {
			residue = webpc_asr(row_out[column] + 4, 3);
			sum = (int)row_start[column] + (int)residue;
			row_start[column] = (uint8_t)webpc_clamp255(sum);
		}
	}
}
