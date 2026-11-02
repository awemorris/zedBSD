/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8-idct.c: the inverse WHT and the 4x4 inverse DCT against
 * outputs worked out by hand from RFC 6386 14.3 and 14.4, the DC-only
 * shortcuts over every 16-bit value, the sum with the prediction (clamp,
 * stride, the pixels beside the block), extreme inputs, and random inputs
 * against a literal transcription of the RFC's formulas kept in the test.
 */

#include "check.h"
#include "vp8-idct.h"

#include <string.h>

/* The random generator's state. */
static unsigned long random_state = 0x13572468UL;

static long floor_shift(long value, unsigned shift);
static long wrap_short(long value);
static void reference_wht(const int16_t input[16], int16_t output[16]);
static void reference_idct(const int16_t input[16], int16_t residue[16]);
static void fill_random(int16_t block[16], unsigned magnitude_bits);
static void test_wht_zero_and_dc(void);
static void test_wht_hand(void);
static void test_wht_dc_every_value(void);
static void test_wht_wrap(void);
static void test_idct_zero_and_dc(void);
static void test_idct_dc_every_value(void);
static void test_idct_hand(void);
static void test_idct_clamp(void);
static void test_idct_stride(void);
static void test_extreme(void);
static void test_random(void);

/*
 * Runs the tests of vp8-idct.c.
 */
int
main(
	void)
{
	int status;

	/* Each helper in turn. */
	test_wht_zero_and_dc();
	test_wht_hand();
	test_wht_dc_every_value();
	test_wht_wrap();
	test_idct_zero_and_dc();
	test_idct_dc_every_value();
	test_idct_hand();
	test_idct_clamp();
	test_idct_stride();
	test_extreme();
	test_random();

	/* The summary line. */
	status = check_finish("vp8_idct");
	return status;
}

/*
 * Shifts right by the floor of the quotient, with division so that no shift
 * of a negative value is made.
 */
static long
floor_shift(
	long value,
	unsigned shift)
{
	long divisor;
	long quotient;

	/* The divisor is 2^shift; C division truncates toward zero. */
	divisor = 1L << shift;
	quotient = value / divisor;

	/* A negative value with a remainder truncated upward: step down. */
	if (value < 0L && quotient * divisor != value)
		quotient--;

	return quotient;
}

/*
 * Wraps a value to what a 16-bit short holds, by modular arithmetic.
 */
static long
wrap_short(
	long value)
{
	long wrapped;

	/* Moves to 0..65535 by the modulus, then back to -32768..32767. */
	wrapped = ((value % 65536L) + 65536L) % 65536L;
	if (wrapped >= 32768L)
		wrapped -= 65536L;

	return wrapped;
}

/*
 * The inverse WHT written as RFC 6386 14.3 gives it, every stored value a
 * short.
 */
static void
reference_wht(
	const int16_t input[16],
	int16_t output[16])
{
	long a1;
	long b1;
	long c1;
	long d1;
	long a2;
	long b2;
	long c2;
	long d2;
	int i;

	/* First pass: columns, results stored as shorts. */
	for (i = 0; i < 4; i++) {
		a1 = (long)input[i] + (long)input[i + 12];
		b1 = (long)input[i + 4] + (long)input[i + 8];
		c1 = (long)input[i + 4] - (long)input[i + 8];
		d1 = (long)input[i] - (long)input[i + 12];

		output[i] = (int16_t)wrap_short(a1 + b1);
		output[i + 4] = (int16_t)wrap_short(c1 + d1);
		output[i + 8] = (int16_t)wrap_short(a1 - b1);
		output[i + 12] = (int16_t)wrap_short(d1 - c1);
	}

	/* Second pass: rows, in place, with (x + 3) >> 3. */
	for (i = 0; i < 4; i++) {
		a1 = (long)output[i * 4] + (long)output[i * 4 + 3];
		b1 = (long)output[i * 4 + 1] + (long)output[i * 4 + 2];
		c1 = (long)output[i * 4 + 1] - (long)output[i * 4 + 2];
		d1 = (long)output[i * 4] - (long)output[i * 4 + 3];

		a2 = a1 + b1;
		b2 = c1 + d1;
		c2 = a1 - b1;
		d2 = d1 - c1;

		output[i * 4] = (int16_t)wrap_short(floor_shift(a2 + 3L, 3));
		output[i * 4 + 1] = (int16_t)wrap_short(floor_shift(b2 + 3L, 3));
		output[i * 4 + 2] = (int16_t)wrap_short(floor_shift(c2 + 3L, 3));
		output[i * 4 + 3] = (int16_t)wrap_short(floor_shift(d2 + 3L, 3));
	}
}

/*
 * The inverse DCT written as RFC 6386 14.4 gives it, giving the residue
 * (before the sum with a prediction) as shorts in raster order.
 */
static void
reference_idct(
	const int16_t input[16],
	int16_t residue[16])
{
	long a1;
	long b1;
	long c1;
	long d1;
	long temp1;
	long temp2;
	int i;

	/* First pass: columns (ip[0], ip[4], ip[8], ip[12]), stored as shorts. */
	for (i = 0; i < 4; i++) {
		a1 = (long)input[i] + (long)input[i + 8];
		b1 = (long)input[i] - (long)input[i + 8];

		temp1 = floor_shift((long)input[i + 4] * 35468L, 16);
		temp2 = (long)input[i + 12] + floor_shift((long)input[i + 12] * 20091L, 16);
		c1 = temp1 - temp2;

		temp1 = (long)input[i + 4] + floor_shift((long)input[i + 4] * 20091L, 16);
		temp2 = floor_shift((long)input[i + 12] * 35468L, 16);
		d1 = temp1 + temp2;

		residue[i] = (int16_t)wrap_short(a1 + d1);
		residue[i + 12] = (int16_t)wrap_short(a1 - d1);
		residue[i + 4] = (int16_t)wrap_short(b1 + c1);
		residue[i + 8] = (int16_t)wrap_short(b1 - c1);
	}

	/* Second pass: rows, in place, with (x + 4) >> 3. */
	for (i = 0; i < 4; i++) {
		a1 = (long)residue[i * 4] + (long)residue[i * 4 + 2];
		b1 = (long)residue[i * 4] - (long)residue[i * 4 + 2];

		temp1 = floor_shift((long)residue[i * 4 + 1] * 35468L, 16);
		temp2 = (long)residue[i * 4 + 3] + floor_shift((long)residue[i * 4 + 3] * 20091L, 16);
		c1 = temp1 - temp2;

		temp1 = (long)residue[i * 4 + 1] + floor_shift((long)residue[i * 4 + 1] * 20091L, 16);
		temp2 = floor_shift((long)residue[i * 4 + 3] * 35468L, 16);
		d1 = temp1 + temp2;

		residue[i * 4] = (int16_t)wrap_short(floor_shift(a1 + d1 + 4L, 3));
		residue[i * 4 + 3] = (int16_t)wrap_short(floor_shift(a1 - d1 + 4L, 3));
		residue[i * 4 + 1] = (int16_t)wrap_short(floor_shift(b1 + c1 + 4L, 3));
		residue[i * 4 + 2] = (int16_t)wrap_short(floor_shift(b1 - c1 + 4L, 3));
	}
}

/*
 * Fills a block with random coefficients of up to magnitude_bits bits plus
 * a sign (magnitude_bits 15 gives the whole 16-bit range).
 */
static void
fill_random(
	int16_t block[16],
	unsigned magnitude_bits)
{
	unsigned long raw;
	long value;
	long limit;
	int i;

	/* The range is -2^bits .. 2^bits - 1, kept inside 16 bits. */
	limit = 1L << magnitude_bits;

	/* Each coefficient from the generator. */
	for (i = 0; i < 16; i++) {
		raw = check_random(&random_state);
		value = (long)(raw % (unsigned long)(2L * limit)) - limit;
		block[i] = (int16_t)value;
	}
}

/* Checks the WHT of all zero and of a lone DC. */
static void
test_wht_zero_and_dc(void)
{
	int16_t input[16];
	int16_t dc[16];
	int i;

	/* All zero gives all zero (the +3 rounds away in the >> 3). */
	memset(input, 0, sizeof(input));
	memset(dc, 0x55, sizeof(dc));
	webpc_vp8_iwht(input, dc);
	for (i = 0; i < 16; i++) {
		CHECK(dc[i] == 0);
	}

	/* A lone DC of 16 gives (16 + 3) >> 3 = 2 in every block. */
	input[0] = 16;
	webpc_vp8_iwht(input, dc);
	for (i = 0; i < 16; i++) {
		CHECK(dc[i] == 2);
	}
}

/*
 * Checks three inputs traced by hand through RFC 6386 14.3.
 */
static void
test_wht_hand(void)
{
	static const int16_t want_first[16] = {
		3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1
	};
	static const int16_t want_second[16] = {
		-1, -1, -1, -1, -1, -1, -1, -1, 1, 1, 1, 1, 1, 1, 1, 1
	};
	static const int16_t want_third[16] = {
		0, -2, 0, -2, 2, 0, 2, 0, 0, -2, 0, -2, 2, 0, 2, 0
	};
	int16_t input[16];
	int16_t dc[16];
	int i;

	/*
	 * Input 16 at raster 0 and 8 at raster 1.  First pass, columns: the
	 * column of 16 gives 16 in all four rows, the column of 8 gives 8,
	 * so every row of the middle is 16, 8, 0, 0.  Second pass on one row:
	 * a1 = 16 + 0 = 16, b1 = 8 + 0 = 8, c1 = 8 - 0 = 8, d1 = 16 - 0 = 16;
	 * a2 = 24, b2 = c1 + d1 = 24, c2 = 8, d2 = d1 - c1 = 8; the outputs
	 * (27 >> 3, 27 >> 3, 11 >> 3, 11 >> 3) are 3, 3, 1, 1.
	 */
	memset(input, 0, sizeof(input));
	input[0] = 16;
	input[1] = 8;
	webpc_vp8_iwht(input, dc);
	for (i = 0; i < 16; i++) {
		CHECK(dc[i] == want_first[i]);
	}

	/*
	 * Input -5 at raster 4 (second row, first column).  First pass on
	 * column 0: a1 = 0, b1 = -5, c1 = -5, d1 = 0; the column of the
	 * middle is a1 + b1 = -5, c1 + d1 = -5, a1 - b1 = 5, d1 - c1 = 5.
	 * Second pass on a row whose only entry is m: a2 = b2 = c2 = d2 = m,
	 * so every output is (m + 3) >> 3: (-2) >> 3 = -1 for m = -5 (the
	 * floor), 8 >> 3 = 1 for m = 5.  Rows 0 and 1 are -1, rows 2 and 3
	 * are 1.
	 */
	memset(input, 0, sizeof(input));
	input[4] = -5;
	webpc_vp8_iwht(input, dc);
	for (i = 0; i < 16; i++) {
		CHECK(dc[i] == want_second[i]);
	}

	/*
	 * Input 7 at raster 3 and -9 at raster 12.  First pass: column 0 has
	 * only -9 at its bottom, a1 = -9, b1 = 0, c1 = 0, d1 = 9, giving
	 * -9, 9, -9, 9 down the middle's column 0; column 3 has only 7 at
	 * its top, a1 = 7, d1 = 7, giving 7 in all four rows.  The middle's
	 * rows are (-9, 0, 0, 7) and (9, 0, 0, 7) alternating.
	 * Second pass on (-9, 0, 0, 7): a1 = -2, b1 = 0, c1 = 0, d1 = -16;
	 * a2 = -2, b2 = -16, c2 = -2, d2 = -16; the outputs 1 >> 3 = 0,
	 * (-13) >> 3 = -2 (the floor of -1.625), 0, -2.  On (9, 0, 0, 7):
	 * a1 = 16, d1 = 2; a2 = 16, b2 = 2, c2 = 16, d2 = 2; the outputs
	 * 19 >> 3 = 2, 5 >> 3 = 0, 2, 0.
	 */
	memset(input, 0, sizeof(input));
	input[3] = 7;
	input[12] = -9;
	webpc_vp8_iwht(input, dc);
	for (i = 0; i < 16; i++) {
		CHECK(dc[i] == want_third[i]);
	}
}

/*
 * Checks a lone DC of every 16-bit value: every block gets (v + 3) >> 3.
 */
static void
test_wht_dc_every_value(void)
{
	int16_t input[16];
	int16_t dc[16];
	long value;
	long want;
	int mismatches;
	int i;

	/* Every value from -32768 to 32767 in raster 0. */
	memset(input, 0, sizeof(input));
	mismatches = 0;
	for (value = -32768L; value <= 32767L; value++) {
		input[0] = (int16_t)value;
		webpc_vp8_iwht(input, dc);
		want = floor_shift(value + 3L, 3);
		for (i = 0; i < 16; i++) {
			if ((long)dc[i] != want)
				mismatches++;
		}
	}
	CHECK(mismatches == 0);
}

/*
 * Checks the 16-bit store of the first pass, as the reference's short
 * intermediate gives it.
 */
static void
test_wht_wrap(void)
{
	static const int16_t want[16] = {
		-2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	int16_t input[16];
	int16_t dc[16];
	int i;

	/*
	 * Every input 32767.  First pass, a column: a1 = 65534, b1 = 65534,
	 * c1 = 0, d1 = 0: the middle's row 0 is 131068, which as a short is
	 * 131068 - 131072 = -4; the other rows are 0.  Second pass on row 0
	 * (-4, -4, -4, -4): a1 = -8, b1 = -8, c1 = 0, d1 = 0, a2 = -16,
	 * b2 = 0, c2 = 0, d2 = 0; the outputs (-13) >> 3 = -2, 3 >> 3 = 0.
	 * Rows 1 to 3 give (0 + 3) >> 3 = 0.
	 */
	for (i = 0; i < 16; i++)
		input[i] = 32767;
	webpc_vp8_iwht(input, dc);
	for (i = 0; i < 16; i++) {
		CHECK(dc[i] == want[i]);
	}
}

/* Checks the DCT of all zero and of a lone DC against the prediction. */
static void
test_idct_zero_and_dc(void)
{
	int16_t input[16];
	uint8_t block[16];
	int i;

	/* All zero leaves every prediction pixel as it is. */
	memset(input, 0, sizeof(input));
	for (i = 0; i < 16; i++)
		block[i] = (uint8_t)(i * 16 + 7);
	webpc_vp8_idct_add(input, block, 4);
	for (i = 0; i < 16; i++) {
		CHECK(block[i] == (uint8_t)(i * 16 + 7));
	}

	/*
	 * A lone DC of 80: the first pass gives 80 down column 0, the second
	 * gives (80 + 4) >> 3 = 10 in every pixel of the residue.
	 */
	input[0] = 80;
	memset(block, 100, sizeof(block));
	webpc_vp8_idct_add(input, block, 4);
	for (i = 0; i < 16; i++) {
		CHECK(block[i] == 110);
	}

	/* A lone DC of -80: (-80 + 4) >> 3 = -76 >> 3 = -10 (the floor of -9.5). */
	input[0] = -80;
	memset(block, 100, sizeof(block));
	webpc_vp8_idct_add(input, block, 4);
	for (i = 0; i < 16; i++) {
		CHECK(block[i] == 90);
	}
}

/*
 * Checks a lone DC of every 16-bit value over three predictions: the
 * residue is flat (v + 4) >> 3.
 */
static void
test_idct_dc_every_value(void)
{
	static const uint8_t predictions[3] = { 0U, 100U, 255U };
	int16_t input[16];
	uint8_t block[16];
	long value;
	long residue;
	long want;
	int mismatches;
	int p;
	int i;

	/* Every DC value, each against each prediction. */
	memset(input, 0, sizeof(input));
	mismatches = 0;
	for (value = -32768L; value <= 32767L; value++) {
		input[0] = (int16_t)value;
		residue = floor_shift(value + 4L, 3);
		for (p = 0; p < 3; p++) {
			memset(block, predictions[p], sizeof(block));
			webpc_vp8_idct_add(input, block, 4);
			want = (long)predictions[p] + residue;
			if (want < 0L)
				want = 0L;
			if (want > 255L)
				want = 255L;
			for (i = 0; i < 16; i++) {
				if ((long)block[i] != want)
					mismatches++;
			}
		}
	}
	CHECK(mismatches == 0);
}

/*
 * Checks three inputs traced by hand through RFC 6386 14.4.
 */
static void
test_idct_hand(void)
{
	static const uint8_t want_second[4] = { 144U, 135U, 121U, 112U };
	static const uint8_t want_third[4] = { 84U, 93U, 107U, 116U };
	int16_t input[16];
	uint8_t block[16];
	int row;
	int column;

	/*
	 * Input 100 at raster 1 (first row, second column), prediction 128.
	 * First pass on column 1: the only entry is on top, so a1 = b1 = 100
	 * and c1 = d1 = 0, and all four rows of the column are 100.  Second
	 * pass on a row (0, 100, 0, 0): a1 = b1 = 0; (100 * 35468) >> 16 =
	 * 3546800 >> 16 = 54, so c1 = 54 - 0 = 54; 100 + ((100 * 20091) >>
	 * 16) = 100 + (2009100 >> 16) = 100 + 30 = 130, so d1 = 130.  The
	 * outputs (130 + 4) >> 3 = 16, (54 + 4) >> 3 = 7, (-54 + 4) >> 3 =
	 * -50 >> 3 = -7 (the floor of -6.25), (-130 + 4) >> 3 = -126 >> 3 =
	 * -16 (the floor of -15.75), in the order of columns 0, 1, 2, 3.
	 * The residue is the same in every row.
	 */
	memset(input, 0, sizeof(input));
	input[1] = 100;
	memset(block, 128, sizeof(block));
	webpc_vp8_idct_add(input, block, 4);
	for (row = 0; row < 4; row++) {
		for (column = 0; column < 4; column++) {
			CHECK(block[row * 4 + column] == want_second[column]);
		}
	}

	/*
	 * Input -100 at raster 4 (second row, first column), prediction 100.
	 * First pass on column 0 (0, -100, 0, 0): a1 = b1 = 0;
	 * (-100 * 35468) >> 16 = -3546800 >> 16 = -55 (the floor of
	 * -54.12), c1 = -55; -100 + ((-100 * 20091) >> 16) = -100 + (-31)
	 * = -131 (the floor of -30.65 is -31), d1 = -131.  The column of
	 * the middle is a1 + d1 = -131, b1 + c1 = -55, b1 - c1 = 55,
	 * a1 - d1 = 131.  Second pass on a row with only m in column 0: the
	 * four outputs are all (m + 4) >> 3: -127 >> 3 = -16, -51 >> 3 = -7,
	 * 59 >> 3 = 7, 135 >> 3 = 16.  So row 0 is 100 - 16 = 84 across, row
	 * 1 is 93, row 2 is 107, row 3 is 116.
	 */
	memset(input, 0, sizeof(input));
	input[4] = -100;
	memset(block, 100, sizeof(block));
	webpc_vp8_idct_add(input, block, 4);
	for (row = 0; row < 4; row++) {
		for (column = 0; column < 4; column++) {
			CHECK(block[row * 4 + column] == want_third[row]);
		}
	}

	/*
	 * Input 8 at raster 0 and 8 at raster 15 (last row, last column),
	 * prediction 50.  First pass on column 0: a1 = b1 = 8, so all four
	 * rows are 8.  First pass on column 3, whose only entry is the
	 * bottom one (ip[12] = 8): a1 = b1 = 0; c1 = 0 - (8 + ((8 * 20091)
	 * >> 16)) = -(8 + 2) = -10 (160728 >> 16 = 2); d1 = 0 + ((8 * 35468)
	 * >> 16) = 4 (283744 >> 16 = 4).  The column is a1 + d1 = 4,
	 * b1 + c1 = -10, b1 - c1 = 10, a1 - d1 = -4, so the middle's rows
	 * are (8, 0, 0, 4), (8, 0, 0, -10), (8, 0, 0, 10), (8, 0, 0, -4).
	 * Second pass on (8, 0, 0, 4): a1 = 8, b1 = 8, c1 = 0 - (4 + ((4 *
	 * 20091) >> 16)) = -(4 + 1) = -5 (80364 >> 16 = 1), d1 = (4 * 35468)
	 * >> 16 = 2 (141872 >> 16 = 2); the outputs (8 + 2 + 4) >> 3 = 1,
	 * (8 - 5 + 4) >> 3 = 0, (8 + 5 + 4) >> 3 = 2, (8 - 2 + 4) >> 3 = 1,
	 * in the order of columns 0, 1, 2, 3; so row 0 is 51, 50, 52, 51.
	 */
	memset(input, 0, sizeof(input));
	input[0] = 8;
	input[15] = 8;
	memset(block, 50, sizeof(block));
	webpc_vp8_idct_add(input, block, 4);
	CHECK(block[0] == 51);
	CHECK(block[1] == 50);
	CHECK(block[2] == 52);
	CHECK(block[3] == 51);
}

/* Checks the clamp at both ends of the pixel range. */
static void
test_idct_clamp(void)
{
	/* The residue of the second hand trace of test_idct_hand: 16, 7, -7, -16. */
	static const uint8_t before[4] = { 250U, 5U, 130U, 10U };
	static const uint8_t after[4] = { 255U, 12U, 123U, 0U };
	static const uint8_t before_edges[4] = { 255U, 0U, 255U, 0U };
	int16_t input[16];
	uint8_t block[16];
	int column;
	int row;

	/*
	 * 250 + 16 = 266 clamps to 255; 5 + 7 = 12; 130 - 7 = 123; 10 - 16
	 * = -6 clamps to 0.
	 */
	memset(input, 0, sizeof(input));
	input[1] = 100;
	for (row = 0; row < 4; row++) {
		for (column = 0; column < 4; column++) {
			block[row * 4 + column] = before[column];
		}
	}
	webpc_vp8_idct_add(input, block, 4);
	for (row = 0; row < 4; row++) {
		for (column = 0; column < 4; column++) {
			CHECK(block[row * 4 + column] == after[column]);
		}
	}

	/*
	 * Pixels already at 255 with a positive residue stay 255, pixels at 0
	 * with a negative one stay 0: columns 0 and 2 get +16 and -7 against
	 * 255 and 255, columns 1 and 3 get +7 and -16 against 0 and 0.
	 */
	for (row = 0; row < 4; row++) {
		for (column = 0; column < 4; column++) {
			block[row * 4 + column] = before_edges[column];
		}
	}
	webpc_vp8_idct_add(input, block, 4);
	for (row = 0; row < 4; row++) {
		CHECK(block[row * 4 + 0] == 255);
		CHECK(block[row * 4 + 1] == 7);
		CHECK(block[row * 4 + 2] == 248);
		CHECK(block[row * 4 + 3] == 0);
	}
}

/*
 * Checks that the stride separates the rows and that no pixel outside the
 * four columns of the four rows changes.
 */
static void
test_idct_stride(void)
{
	static const ptrdiff_t strides[3] = { 4, 16, 33 };
	int16_t input[16];
	uint8_t buffer[4 * 40 + 8];
	uint8_t expected[4 * 40 + 8];
	ptrdiff_t stride;
	int s;
	int row;
	int column;
	int i;
	int different;

	/* A fixed block with a residue that differs in every row. */
	memset(input, 0, sizeof(input));
	input[4] = -100;

	/* Each stride in turn. */
	for (s = 0; s < 3; s++) {
		stride = strides[s];

		/* A recognisable background; the block starts 2 bytes in. */
		for (i = 0; i < (int)sizeof(buffer); i++)
			buffer[i] = (uint8_t)(30 + (i * 3) % 190);
		memcpy(expected, buffer, sizeof(buffer));

		/* The expected: rows get 100's residue -16, -7, 7, 16 added (hand trace). */
		for (row = 0; row < 4; row++) {
			for (column = 0; column < 4; column++) {
				i = 2 + (int)(row * stride) + column;
				if (row == 0)
					expected[i] = (uint8_t)(expected[i] - 16);
				if (row == 1)
					expected[i] = (uint8_t)(expected[i] - 7);
				if (row == 2)
					expected[i] = (uint8_t)(expected[i] + 7);
				if (row == 3)
					expected[i] = (uint8_t)(expected[i] + 16);
			}
		}

		webpc_vp8_idct_add(input, buffer + 2, stride);

		/* Every byte, in or out of the block, against the expected. */
		different = 0;
		for (i = 0; i < (int)sizeof(buffer); i++) {
			if (buffer[i] != expected[i])
				different++;
		}
		CHECK(different == 0);
	}
}

/*
 * Checks that extreme inputs finish without undefined behaviour (the
 * sanitizers report it) and keep every output in range.
 */
static void
test_extreme(void)
{
	static const int16_t values[3] = { 32767, -32768, 0 };
	int16_t input[16];
	int16_t dc[16];
	int16_t residue[16];
	uint8_t block[16];
	int v;
	int w;
	int i;
	int bad;

	/* Every pairing of the extremes in alternating positions. */
	bad = 0;
	for (v = 0; v < 3; v++) {
		for (w = 0; w < 3; w++) {
			for (i = 0; i < 16; i++)
				input[i] = (i % 2 == 0) ? values[v] : values[w];

			webpc_vp8_iwht(input, dc);
			reference_wht(input, residue);
			for (i = 0; i < 16; i++) {
				if (dc[i] != residue[i])
					bad++;
			}

			memset(block, 128, sizeof(block));
			webpc_vp8_idct_add(input, block, 4);
			reference_idct(input, residue);
			for (i = 0; i < 16; i++) {
				if ((long)block[i] != (long)(128L + residue[i] < 0L ? 0L :
				    (128L + residue[i] > 255L ? 255L : 128L + residue[i])))
					bad++;
			}
		}
	}
	CHECK(bad == 0);

	/* A single extreme coefficient at every position. */
	bad = 0;
	for (w = 0; w < 2; w++) {
		for (v = 0; v < 16; v++) {
			memset(input, 0, sizeof(input));
			input[v] = values[w];
			webpc_vp8_iwht(input, dc);
			reference_wht(input, residue);
			for (i = 0; i < 16; i++) {
				if (dc[i] != residue[i])
					bad++;
			}
		}
	}
	CHECK(bad == 0);
}

/*
 * Checks random blocks, small and full-range, against the transcription.
 */
static void
test_random(void)
{
	static const unsigned bits[4] = { 4U, 8U, 11U, 15U };
	int16_t input[16];
	int16_t want_dc[16];
	int16_t dc[16];
	int16_t residue[16];
	uint8_t block[16];
	uint8_t seed[16];
	long want;
	unsigned long raw;
	int iteration;
	int b;
	int i;
	int wht_bad;
	int idct_bad;

	/* Several thousand blocks of each magnitude. */
	wht_bad = 0;
	idct_bad = 0;
	for (b = 0; b < 4; b++) {
		for (iteration = 0; iteration < 5000; iteration++) {
			fill_random(input, bits[b]);

			webpc_vp8_iwht(input, dc);
			reference_wht(input, want_dc);
			for (i = 0; i < 16; i++) {
				if (dc[i] != want_dc[i])
					wht_bad++;
			}

			for (i = 0; i < 16; i++) {
				raw = check_random(&random_state);
				seed[i] = (uint8_t)(raw & 0xffUL);
			}
			memcpy(block, seed, sizeof(block));
			webpc_vp8_idct_add(input, block, 4);
			reference_idct(input, residue);
			for (i = 0; i < 16; i++) {
				want = (long)seed[i] + (long)residue[i];
				if (want < 0L)
					want = 0L;
				if (want > 255L)
					want = 255L;
				if ((long)block[i] != want)
					idct_bad++;
			}
		}
	}
	CHECK(wht_bad == 0);
	CHECK(idct_bad == 0);
}
