/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the scaling functions, the inverse scans, the scaling and the
 * inverse transforms of 4x4 and 8x8 blocks, the Intra_16x16 DC and the
 * chroma DC transforms, and the construction of samples (8.5.6 to
 * 8.5.14) of the software H.264 decoder.
 *
 * Every expected value is either worked out by hand from the equations
 * of the clauses (the comments show the steps), or computed by a plain
 * transcription of the scaling equations with a floor division instead
 * of a shift, over a sweep of qP and levels.  Nothing is taken from
 * another decoder.
 */

#include <limits.h>
#include <string.h>

#include "check.h"
#include "userland/desktop/libmedia/h264sw-tables.h"
#include "userland/desktop/libmedia/h264sw-transform.h"

/* The range the clauses give to cij, dij and the like for 8 bits. */
#define RANGE_MIN (-32768)
#define RANGE_MAX 32767

/*
 * Figure 8-8 (a): the zig-zag scan position idx at each raster position
 * 4 * row + column of a 4x4 block.
 */
static const int32_t figure_4x4[16] = {
	0, 1, 5, 6,
	2, 4, 7, 12,
	3, 8, 11, 13,
	9, 10, 14, 15
};

/*
 * Figure 8-9 (a): the zig-zag scan position idx at each raster position
 * 8 * row + column of an 8x8 block.
 */
static const int32_t figure_8x8[64] = {
	0, 1, 5, 6, 14, 15, 27, 28,
	2, 4, 7, 13, 16, 26, 29, 42,
	3, 8, 12, 17, 25, 30, 41, 43,
	9, 11, 18, 24, 31, 40, 44, 53,
	10, 19, 23, 32, 39, 45, 52, 54,
	20, 22, 33, 38, 46, 51, 55, 60,
	21, 34, 37, 47, 50, 56, 59, 61,
	35, 36, 48, 49, 57, 58, 62, 63
};

/* The matrix v of equation 8-315, one row per qP % 6. */
static const int32_t v4x4[6][3] = {
	{ 10, 16, 13 },
	{ 11, 18, 14 },
	{ 13, 20, 16 },
	{ 14, 23, 18 },
	{ 16, 25, 20 },
	{ 18, 29, 23 }
};

/* The matrix v of equation 8-318, one row per qP % 6. */
static const int32_t v8x8[6][6] = {
	{ 20, 18, 32, 19, 25, 24 },
	{ 22, 19, 35, 21, 28, 26 },
	{ 26, 23, 42, 24, 33, 31 },
	{ 28, 25, 45, 26, 35, 33 },
	{ 32, 28, 51, 30, 40, 38 },
	{ 36, 32, 58, 34, 46, 43 }
};

/*
 * The column of v that equation 8-314 selects at each raster position of
 * a 4x4 block: 0 where row and column are both even, 1 where both are
 * odd, 2 otherwise.
 */
static const int32_t column_4x4[16] = {
	0, 2, 0, 2,
	2, 1, 2, 1,
	0, 2, 0, 2,
	2, 1, 2, 1
};

/*
 * The column of v that equation 8-317 selects at each raster position of
 * an 8x8 block, worked out case by case from the equation: the pattern
 * repeats every four rows and every four columns.
 */
static const int32_t column_8x8[64] = {
	0, 3, 4, 3, 0, 3, 4, 3,
	3, 1, 5, 1, 3, 1, 5, 1,
	4, 5, 2, 5, 4, 5, 2, 5,
	3, 1, 5, 1, 3, 1, 5, 1,
	0, 3, 4, 3, 0, 3, 4, 3,
	3, 1, 5, 1, 3, 1, 5, 1,
	4, 5, 2, 5, 4, 5, 2, 5,
	3, 1, 5, 1, 3, 1, 5, 1
};

/* Levels of every kind of size, used to sweep the scaling equations. */
static const int32_t level_set[8] = {
	0, 1, -1, 3, -7, 1000, -32768, 32767
};

/* Forward declarations of the static functions. */
static void check_levelscale(void);
static void check_unscan(void);
static void check_scale4x4(void);
static void check_idct4x4(void);
static void check_scale8x8(void);
static void check_idct8x8(void);
static void check_luma_dc(void);
static void check_chroma_dc(void);
static void check_add_residual(void);
static void check_extremes(void);
static void check_missing(void);
static int64_t floor_divide(int64_t value, unsigned count);
static int32_t clamp_range(int64_t value);
static int64_t power_of_two(unsigned count);
static int32_t reference_scale(int32_t level, int32_t factor, int qp, int threshold, int base);
static int in_range(int32_t value);
static void flat_levelscale(struct h264sw_levelscale *levelscale);

/* Runs the checks. */
int
main(
	void)
{
	/* Runs each group of checks. */
	check_levelscale();
	check_unscan();
	check_scale4x4();
	check_idct4x4();
	check_scale8x8();
	check_idct8x8();
	check_luma_dc();
	check_chroma_dc();
	check_add_residual();
	check_extremes();
	check_missing();

	/* Succeeded when no check failed. */
	return check_finish("transform");
}

/* Divides by 2^count and rounds toward minus infinity, as an arithmetic right shift does. */
static int64_t
floor_divide(
	int64_t value,
	unsigned count)
{
	int64_t divisor;
	int64_t quotient;

	/* Divides, then corrects the rounding toward zero of a negative remainder. */
	divisor = power_of_two(count);
	quotient = value / divisor;
	if (value % divisor < 0)
		quotient = quotient - 1;

	/* Reports the floor of the quotient. */
	return quotient;
}

/* Holds a value to the range the clauses give to the scaled values. */
static int32_t
clamp_range(
	int64_t value)
{
	/* Takes a value below the range up to its lowest. */
	if (value < (int64_t)RANGE_MIN)
		return RANGE_MIN;

	/* Takes a value above the range down to its highest. */
	if (value > (int64_t)RANGE_MAX)
		return RANGE_MAX;

	/* Reports a value already in the range. */
	return (int32_t)value;
}

/* Gives 2^count. */
static int64_t
power_of_two(
	unsigned count)
{
	int64_t result;

	/* Doubles once for each step of the exponent. */
	result = (int64_t)1;
	while (count > 0U) {
		result = result * 2;
		count--;
	}

	/* Reports the power. */
	return result;
}

/*
 * The scaling equations in the plain form of the clauses: from the
 * threshold on, ( level * factor ) times 2^( qP / 6 - base ) (8-336,
 * 8-356, 8-321); below it ( level * factor + 2^( base - 1 - qP / 6 ) )
 * divided by 2^( base - qP / 6 ), rounded down (8-337, 8-357, 8-322).
 */
static int32_t
reference_scale(
	int32_t level,
	int32_t factor,
	int qp,
	int threshold,
	int base)
{
	int64_t product;
	int64_t result;

	/* Forms the product of the level and the scaling factor. */
	product = (int64_t)level * (int64_t)factor;

	/* Scales up from the threshold, and with rounding below it. */
	if (qp >= threshold) {
		result = product * power_of_two((unsigned)(qp / 6 - base));
	} else {
		result = floor_divide(product + power_of_two((unsigned)(base - 1 - qp / 6)), (unsigned)(base - qp / 6));
	}

	/* Reports the value held to its range. */
	return clamp_range(result);
}

/* Tells whether a value is within the range the clauses give. */
static int
in_range(
	int32_t value)
{
	/* A value below the range fails. */
	if (value < RANGE_MIN)
		return 0;

	/* A value above the range fails. */
	if (value > RANGE_MAX)
		return 0;

	/* The value is in the range. */
	return 1;
}

/* Builds the scaling functions of flat 16 lists. */
static void
flat_levelscale(
	struct h264sw_levelscale *levelscale)
{
	struct h264sw_scaling scaling;

	/* Fills every list with 16, the Flat_4x4_16 and Flat_8x8_16 lists. */
	memset(&scaling, 16, sizeof(scaling));
	h264sw_levelscale_init(levelscale, &scaling);
}

/* Checks the scaling functions of 8.5.9. */
static void
check_levelscale(void)
{
	struct h264sw_levelscale levelscale;
	struct h264sw_scaling scaling;
	unsigned list;
	unsigned m;
	unsigned position;
	unsigned idx;
	int32_t expected;
	int32_t weight;

	/* Flat lists give the matrix v times 16, worked out: v00 * 16 = 160, v11 * 16 = 16 * 16 = 256, v02 * 16 = 13 * 16 = 208. */
	flat_levelscale(&levelscale);
	CHECK(levelscale.scale4x4[0][0][0] == 160);
	CHECK(levelscale.scale4x4[0][0][5] == 256);
	CHECK(levelscale.scale4x4[0][0][1] == 208);
	CHECK(levelscale.scale4x4[0][1][0] == 176);
	CHECK(levelscale.scale4x4[0][5][5] == 464);
	CHECK(levelscale.scale4x4[3][2][10] == 208);
	CHECK(levelscale.scale4x4[5][4][15] == 400);

	/* The 8x8 functions: one position for each of the six kinds, m = 0: 20, 18, 32, 19, 25, 24 times 16. */
	CHECK(levelscale.scale8x8[0][0][0] == 320);
	CHECK(levelscale.scale8x8[0][0][9] == 288);
	CHECK(levelscale.scale8x8[0][0][18] == 512);
	CHECK(levelscale.scale8x8[0][0][1] == 304);
	CHECK(levelscale.scale8x8[0][0][2] == 400);
	CHECK(levelscale.scale8x8[0][0][10] == 384);
	CHECK(levelscale.scale8x8[1][5][63] == 512);

	/* Every entry of the flat functions is v times 16, for every list and every m. */
	for (list = 0; list < 6U; list++) {
		for (m = 0; m < 6U; m++) {
			for (position = 0; position < 16U; position++) {
				expected = v4x4[m][column_4x4[position]] * 16;
				CHECK(levelscale.scale4x4[list][m][position] == expected);
			}
		}
	}
	for (list = 0; list < 2U; list++) {
		for (m = 0; m < 6U; m++) {
			for (position = 0; position < 64U; position++) {
				expected = v8x8[m][column_8x8[position]] * 16;
				CHECK(levelscale.scale8x8[list][m][position] == expected);
			}
		}
	}

	/*
	 * Lists that differ: the weight at a raster position is the list
	 * entry at the scan position Figure 8-8 gives that position.
	 */
	for (list = 0; list < 6U; list++) {
		for (idx = 0; idx < 16U; idx++)
			scaling.list4x4[list][idx] = (uint8_t)(1U + list * 16U + idx);
	}
	for (list = 0; list < 2U; list++) {
		for (idx = 0; idx < 64U; idx++)
			scaling.list8x8[list][idx] = (uint8_t)(1U + list * 100U + idx);
	}
	h264sw_levelscale_init(&levelscale, &scaling);
	for (list = 0; list < 6U; list++) {
		for (m = 0; m < 6U; m++) {
			for (position = 0; position < 16U; position++) {
				weight = (int32_t)scaling.list4x4[list][figure_4x4[position]];
				expected = weight * v4x4[m][column_4x4[position]];
				CHECK(levelscale.scale4x4[list][m][position] == expected);
			}
		}
	}
	for (list = 0; list < 2U; list++) {
		for (m = 0; m < 6U; m++) {
			for (position = 0; position < 64U; position++) {
				weight = (int32_t)scaling.list8x8[list][figure_8x8[position]];
				expected = weight * v8x8[m][column_8x8[position]];
				CHECK(levelscale.scale8x8[list][m][position] == expected);
			}
		}
	}

	/* The largest weight, 255, with the largest v still fits: 255 * 29 and 255 * 58. */
	memset(&scaling, 255, sizeof(scaling));
	h264sw_levelscale_init(&levelscale, &scaling);
	CHECK(levelscale.scale4x4[5][5][5] == 255 * 29);
	CHECK(levelscale.scale8x8[1][5][2 * 8 + 2] == 255 * 58);
}

/* Checks the inverse scans of 8.5.6 and 8.5.7. */
static void
check_unscan(void)
{
	int32_t levels[64];
	int32_t block[64];
	unsigned i;

	/* Scan position idx holds idx, so each raster position holds what Figure 8-8 (a) prints there. */
	for (i = 0; i < 16U; i++)
		levels[i] = (int32_t)i;
	memset(block, 0x7f, sizeof(block));
	h264sw_unscan4x4(levels, block);
	for (i = 0; i < 16U; i++)
		CHECK(block[i] == figure_4x4[i]);

	/* Likewise Figure 8-9 (a) for the 8x8 scan. */
	for (i = 0; i < 64U; i++)
		levels[i] = (int32_t)i;
	memset(block, 0x7f, sizeof(block));
	h264sw_unscan8x8(levels, block);
	for (i = 0; i < 64U; i++)
		CHECK(block[i] == figure_8x8[i]);

	/* Scan position 1 is c01 (right of the DC), position 2 is c10 (below it). */
	memset(levels, 0, sizeof(levels));
	levels[1] = 11;
	levels[2] = 22;
	h264sw_unscan4x4(levels, block);
	CHECK(block[1] == 11);
	CHECK(block[4] == 22);
	CHECK(block[0] == 0);

	/* The scan may run in place. */
	for (i = 0; i < 16U; i++)
		block[i] = (int32_t)i;
	h264sw_unscan4x4(block, block);
	for (i = 0; i < 16U; i++)
		CHECK(block[i] == figure_4x4[i]);
	for (i = 0; i < 64U; i++)
		block[i] = (int32_t)i;
	h264sw_unscan8x8(block, block);
	for (i = 0; i < 64U; i++)
		CHECK(block[i] == figure_8x8[i]);
}

/* Checks the scaling of a 4x4 block (8.5.12.1). */
static void
check_scale4x4(void)
{
	struct h264sw_levelscale levelscale;
	int32_t block[16];
	int32_t expected;
	int qp;
	unsigned i;

	flat_levelscale(&levelscale);

	/* qP 28 (m = 4, qP / 6 = 4): a left shift of 0, so d = c * LevelScale; c00 = 3 gives 3 * 256. */
	memset(block, 0, sizeof(block));
	block[0] = 3;
	h264sw_scale4x4(block, levelscale.scale4x4[0][4], 28, 0);
	CHECK(block[0] == 768);
	CHECK(block[1] == 0);

	/* qP 0: ( c * 160 + 8 ) >> 4; c = 1 gives 168 >> 4 = 10, c = -1 gives -152 >> 4 = -10 (rounded down). */
	block[0] = 1;
	h264sw_scale4x4(block, levelscale.scale4x4[0][0], 0, 0);
	CHECK(block[0] == 10);
	block[0] = -1;
	h264sw_scale4x4(block, levelscale.scale4x4[0][0], 0, 0);
	CHECK(block[0] == -10);

	/* qP 23 (m = 5, qP / 6 = 3): ( c * 288 + 1 ) >> 1; c = 1 gives 289 >> 1 = 144. */
	block[0] = 1;
	h264sw_scale4x4(block, levelscale.scale4x4[0][5], 23, 0);
	CHECK(block[0] == 144);

	/* qP 24 (m = 0, qP / 6 = 4): the shift is 0 and no rounding: 160. */
	block[0] = 1;
	h264sw_scale4x4(block, levelscale.scale4x4[0][0], 24, 0);
	CHECK(block[0] == 160);

	/* qP 51 (m = 3, qP / 6 = 8): shift 4, 224 * 16 = 3584. */
	block[0] = 1;
	h264sw_scale4x4(block, levelscale.scale4x4[0][3], 51, 0);
	CHECK(block[0] == 3584);

	/* A DC already scaled stays (8-335), the others are scaled. */
	for (i = 0; i < 16U; i++)
		block[i] = 1;
	h264sw_scale4x4(block, levelscale.scale4x4[0][0], 24, 1);
	CHECK(block[0] == 1);
	CHECK(block[1] == 208);
	CHECK(block[5] == 256);

	/* A sweep of qP and levels against the equations 8-336 and 8-337. */
	for (qp = 0; qp <= 51; qp++) {
		for (i = 0; i < 16U; i++)
			block[i] = level_set[(i + (unsigned)qp) % 8U];
		h264sw_scale4x4(block, levelscale.scale4x4[3][qp % 6], qp, 0);
		for (i = 0; i < 16U; i++) {
			expected = reference_scale(level_set[(i + (unsigned)qp) % 8U], levelscale.scale4x4[3][qp % 6][i], qp, 24, 4);
			CHECK(block[i] == expected);
		}
	}
}

/* Checks the inverse transform of a 4x4 block (8.5.12.2). */
static void
check_idct4x4(void)
{
	int32_t block[16];
	unsigned i;
	unsigned j;

	/* A DC alone: every sample is ( d + 32 ) >> 6; d = 768 gives 800 >> 6 = 12. */
	memset(block, 0, sizeof(block));
	block[0] = 768;
	h264sw_idct4x4(block);
	for (i = 0; i < 16U; i++)
		CHECK(block[i] == 12);

	/* A negative DC rounds down: d = -768 gives -736 >> 6 = -12 (floor of -11.5). */
	memset(block, 0, sizeof(block));
	block[0] = -768;
	h264sw_idct4x4(block);
	for (i = 0; i < 16U; i++)
		CHECK(block[i] == -12);

	/*
	 * d01 = 640: the row is e = (0, 0, 320, 640), f = (640, 320, -320,
	 * -640), the same in every row after the column step; r = ( f + 32 )
	 * >> 6 = 672 >> 6, 352 >> 6, -288 >> 6, -608 >> 6 = 10, 5, -5, -10.
	 */
	memset(block, 0, sizeof(block));
	block[1] = 640;
	h264sw_idct4x4(block);
	for (i = 0; i < 4U; i++) {
		CHECK(block[4U * i + 0U] == 10);
		CHECK(block[4U * i + 1U] == 5);
		CHECK(block[4U * i + 2U] == -5);
		CHECK(block[4U * i + 3U] == -10);
	}

	/* d10 = 640 is the same in the other direction. */
	memset(block, 0, sizeof(block));
	block[4] = 640;
	h264sw_idct4x4(block);
	for (j = 0; j < 4U; j++) {
		CHECK(block[0U * 4U + j] == 10);
		CHECK(block[1U * 4U + j] == 5);
		CHECK(block[2U * 4U + j] == -5);
		CHECK(block[3U * 4U + j] == -10);
	}

	/*
	 * d01 = -641 (odd, negative): e2 = ( -641 >> 1 ) = -321, e3 = -641;
	 * f = (-641, -321, 321, 641); r = -609 >> 6, -289 >> 6, 353 >> 6,
	 * 673 >> 6 = -10, -5, 5, 10.
	 */
	memset(block, 0, sizeof(block));
	block[1] = -641;
	h264sw_idct4x4(block);
	for (i = 0; i < 4U; i++) {
		CHECK(block[4U * i + 0U] == -10);
		CHECK(block[4U * i + 1U] == -5);
		CHECK(block[4U * i + 2U] == 5);
		CHECK(block[4U * i + 3U] == 10);
	}

	/*
	 * d03 = 640: e2 = 0 - 640, e3 = 0 + ( 640 >> 1 ) = 320; f = (320,
	 * -640, 640, -320); r = 352 >> 6, -608 >> 6, 672 >> 6, -288 >> 6
	 * = 5, -10, 10, -5.
	 */
	memset(block, 0, sizeof(block));
	block[3] = 640;
	h264sw_idct4x4(block);
	for (i = 0; i < 4U; i++) {
		CHECK(block[4U * i + 0U] == 5);
		CHECK(block[4U * i + 1U] == -10);
		CHECK(block[4U * i + 2U] == 10);
		CHECK(block[4U * i + 3U] == -5);
	}

	/* d02 = 640: e0 = 640, e1 = -640, so f = (640, -640, -640, 640) and r = 10, -10, -10, 10. */
	memset(block, 0, sizeof(block));
	block[2] = 640;
	h264sw_idct4x4(block);
	for (i = 0; i < 4U; i++) {
		CHECK(block[4U * i + 0U] == 10);
		CHECK(block[4U * i + 1U] == -10);
		CHECK(block[4U * i + 2U] == -10);
		CHECK(block[4U * i + 3U] == 10);
	}

	/*
	 * Every d equal to 64: each row has e = (128, 0, -32, 96), so f =
	 * (224, -32, 32, 32).  A column of x then gives g = (2x, 0, -x / 2,
	 * 3x / 2) for even x and h = (7x / 2, -x / 2, x / 2, x / 2): for 224
	 * (784, -112, 112, 112); for -32 g = (-64, 0, 16, -48) and h =
	 * (-112, 16, -16, -16); for 32 h = (112, -16, 16, 16).  r = ( h + 32 ) >> 6 gives the
	 * matrix (12, -2, 2, 2), (-2, 0, 0, 0), (2, 0, 0, 0), (2, 0, 0, 0).
	 */
	for (i = 0; i < 16U; i++)
		block[i] = 64;
	h264sw_idct4x4(block);
	CHECK(block[0] == 12 && block[1] == -2 && block[2] == 2 && block[3] == 2);
	CHECK(block[4] == -2 && block[5] == 0 && block[6] == 0 && block[7] == 0);
	CHECK(block[8] == 2 && block[9] == 0 && block[10] == 0 && block[11] == 0);
	CHECK(block[12] == 2 && block[13] == 0 && block[14] == 0 && block[15] == 0);
}

/* Checks the scaling of an 8x8 block (8.5.13.1). */
static void
check_scale8x8(void)
{
	struct h264sw_levelscale levelscale;
	int32_t block[64];
	int32_t expected;
	int qp;
	unsigned i;

	flat_levelscale(&levelscale);

	/* qP 36 (m = 0, qP / 6 = 6): no shift; c00 = 10 gives 10 * 320. */
	memset(block, 0, sizeof(block));
	block[0] = 10;
	h264sw_scale8x8(block, levelscale.scale8x8[0][0], 36);
	CHECK(block[0] == 3200);
	CHECK(block[1] == 0);

	/* qP 0: ( c * 320 + 32 ) >> 6; c = 1 gives 352 >> 6 = 5, c = -1 gives -288 >> 6 = -5 (-4.5 rounded down). */
	block[0] = 1;
	h264sw_scale8x8(block, levelscale.scale8x8[0][0], 0);
	CHECK(block[0] == 5);
	block[0] = -1;
	h264sw_scale8x8(block, levelscale.scale8x8[0][0], 0);
	CHECK(block[0] == -5);

	/* qP 35 (m = 5, qP / 6 = 5): ( c * 576 + 1 ) >> 1 = 577 >> 1 = 288. */
	block[0] = 1;
	h264sw_scale8x8(block, levelscale.scale8x8[0][5], 35);
	CHECK(block[0] == 288);

	/* qP 51 (m = 3, qP / 6 = 8): shift 2; 28 * 16 * 4 = 1792. */
	block[0] = 1;
	h264sw_scale8x8(block, levelscale.scale8x8[0][3], 51);
	CHECK(block[0] == 1792);

	/* A sweep of qP and levels against the equations 8-356 and 8-357. */
	for (qp = 0; qp <= 51; qp++) {
		for (i = 0; i < 64U; i++)
			block[i] = level_set[(i + (unsigned)qp) % 8U];
		h264sw_scale8x8(block, levelscale.scale8x8[1][qp % 6], qp);
		for (i = 0; i < 64U; i++) {
			expected = reference_scale(level_set[(i + (unsigned)qp) % 8U], levelscale.scale8x8[1][qp % 6][i], qp, 36, 6);
			CHECK(block[i] == expected);
		}
	}
}

/*
 * The residual row that an 8x8 block with the single coefficient 640 at
 * column k of row 0 gives, in every row (and, transposed, the column
 * that the coefficient at row k of column 0 gives), worked out by hand
 * from equations 8-358 to 8-406.  The case k = 1 is: e3 = 640, e5 =
 * -640, e7 = 640 + 320 = 960, the others 0; f1 = 0 + ( 960 >> 2 ) =
 * 240, f3 = 640 + ( -640 >> 2 ) = 480, f5 = ( 640 >> 2 ) + 640 = 800,
 * f7 = 960, the even f 0; g = (960, 800, 480, 240, -240, -480, -800,
 * -960), and r = ( g + 32 ) >> 6.  The case k = 7 is: e1 = -640 - 320 =
 * -960, e3 = 640, e5 = 640, the others 0; f1 = -960, f3 = 640 + 160 =
 * 800, f5 = 160 - 640 = -480, f7 = 0 + 240 = 240; g = (240, -480, 800,
 * -960, 960, -800, 480, -240).  The other cases go the same way.
 */
static const int32_t single_coefficient[8][8] = {
	{ 10, 10, 10, 10, 10, 10, 10, 10 },
	{ 15, 13, 8, 4, -4, -7, -12, -15 },
	{ 10, 5, -5, -10, -10, -5, 5, 10 },
	{ 13, -4, -15, -7, 8, 15, 4, -12 },
	{ 10, -10, -10, 10, 10, -10, -10, 10 },
	{ 8, -15, 4, 13, -12, -4, 15, -7 },
	{ 5, -10, 10, -5, -5, 10, -10, 5 },
	{ 4, -7, 13, -15, 15, -12, 8, -4 }
};

/* Checks the inverse transform of an 8x8 block (8.5.13.2). */
static void
check_idct8x8(void)
{
	int32_t block[64];
	unsigned i;
	unsigned j;
	unsigned k;

	/* A DC alone: every sample is ( d + 32 ) >> 6; d = 3200 gives 3232 >> 6 = 50. */
	memset(block, 0, sizeof(block));
	block[0] = 3200;
	h264sw_idct8x8(block);
	for (i = 0; i < 64U; i++)
		CHECK(block[i] == 50);

	/* A negative DC: d = -3200 gives -3168 >> 6 = -50 (floor of -49.5). */
	memset(block, 0, sizeof(block));
	block[0] = -3200;
	h264sw_idct8x8(block);
	for (i = 0; i < 64U; i++)
		CHECK(block[i] == -50);

	/* Each horizontal frequency alone gives its row in every row. */
	for (k = 0; k < 8U; k++) {
		memset(block, 0, sizeof(block));
		block[k] = 640;
		h264sw_idct8x8(block);
		for (i = 0; i < 8U; i++) {
			for (j = 0; j < 8U; j++)
				CHECK(block[8U * i + j] == single_coefficient[k][j]);
		}
	}

	/* Each vertical frequency alone gives the same, down the columns. */
	for (k = 0; k < 8U; k++) {
		memset(block, 0, sizeof(block));
		block[8U * k] = 640;
		h264sw_idct8x8(block);
		for (i = 0; i < 8U; i++) {
			for (j = 0; j < 8U; j++)
				CHECK(block[8U * i + j] == single_coefficient[k][i]);
		}
	}
}

/* Checks the Intra_16x16 DC transform and scaling (8.5.10). */
static void
check_luma_dc(void)
{
	int32_t dc[16];
	int32_t expected;
	int qp;
	unsigned i;
	unsigned row;
	unsigned column;
	static const int32_t sign_c01[4] = { 1, 1, -1, -1 };
	static const int32_t sign_c02[4] = { 1, -1, -1, 1 };
	static const int32_t sign_c03[4] = { 1, -1, 1, -1 };

	/* c00 = 1 transforms to f = 1 everywhere; qP 36 (m = 0): 160 << 0 = 160. */
	memset(dc, 0, sizeof(dc));
	dc[0] = 1;
	h264sw_luma_dc(dc, 160, 36);
	for (i = 0; i < 16U; i++)
		CHECK(dc[i] == 160);

	/* qP 35 (m = 5, scale 288, qP / 6 = 5): ( 288 + 1 ) >> 1 = 144. */
	memset(dc, 0, sizeof(dc));
	dc[0] = 1;
	h264sw_luma_dc(dc, 288, 35);
	for (i = 0; i < 16U; i++)
		CHECK(dc[i] == 144);

	/* qP 0 (scale 160): ( 160 + 32 ) >> 6 = 3. */
	memset(dc, 0, sizeof(dc));
	dc[0] = 1;
	h264sw_luma_dc(dc, 160, 0);
	for (i = 0; i < 16U; i++)
		CHECK(dc[i] == 3);

	/* qP 1 (scale 176): +1 gives 208 >> 6 = 3, -1 gives -144 >> 6 = -3 (floor of -2.25). */
	memset(dc, 0, sizeof(dc));
	dc[0] = 1;
	h264sw_luma_dc(dc, 176, 1);
	CHECK(dc[0] == 3);
	memset(dc, 0, sizeof(dc));
	dc[0] = -1;
	h264sw_luma_dc(dc, 176, 1);
	CHECK(dc[0] == -3);

	/*
	 * A single c01 = 1: f = H c H with H the matrix of 8-320 gives, at
	 * row i and column j, H[i][0] * H[1][j] = H[1][j] = (1, 1, -1, -1)
	 * for every row; c02 gives H[2][j] = (1, -1, -1, 1); c03 gives
	 * H[3][j] = (1, -1, 1, -1).  Scaled at qP 36 with scale 160.
	 */
	memset(dc, 0, sizeof(dc));
	dc[1] = 1;
	h264sw_luma_dc(dc, 160, 36);
	for (row = 0; row < 4U; row++) {
		for (column = 0; column < 4U; column++)
			CHECK(dc[4U * row + column] == 160 * sign_c01[column]);
	}
	memset(dc, 0, sizeof(dc));
	dc[2] = 1;
	h264sw_luma_dc(dc, 160, 36);
	for (row = 0; row < 4U; row++) {
		for (column = 0; column < 4U; column++)
			CHECK(dc[4U * row + column] == 160 * sign_c02[column]);
	}
	memset(dc, 0, sizeof(dc));
	dc[3] = 1;
	h264sw_luma_dc(dc, 160, 36);
	for (row = 0; row < 4U; row++) {
		for (column = 0; column < 4U; column++)
			CHECK(dc[4U * row + column] == 160 * sign_c03[column]);
	}

	/* c10, c20, c30 give the same in the other direction: H[i][1], H[i][2], H[i][3] down the rows. */
	memset(dc, 0, sizeof(dc));
	dc[4] = 1;
	h264sw_luma_dc(dc, 160, 36);
	for (row = 0; row < 4U; row++) {
		for (column = 0; column < 4U; column++)
			CHECK(dc[4U * row + column] == 160 * sign_c01[row]);
	}
	memset(dc, 0, sizeof(dc));
	dc[8] = 1;
	h264sw_luma_dc(dc, 160, 36);
	for (row = 0; row < 4U; row++) {
		for (column = 0; column < 4U; column++)
			CHECK(dc[4U * row + column] == 160 * sign_c02[row]);
	}
	memset(dc, 0, sizeof(dc));
	dc[12] = 1;
	h264sw_luma_dc(dc, 160, 36);
	for (row = 0; row < 4U; row++) {
		for (column = 0; column < 4U; column++)
			CHECK(dc[4U * row + column] == 160 * sign_c03[row]);
	}

	/* c11 = 1: f = H[i][1] * H[1][j]. */
	memset(dc, 0, sizeof(dc));
	dc[5] = 1;
	h264sw_luma_dc(dc, 160, 36);
	for (row = 0; row < 4U; row++) {
		for (column = 0; column < 4U; column++)
			CHECK(dc[4U * row + column] == 160 * sign_c01[row] * sign_c01[column]);
	}

	/* A sweep of qP over a block of mixed levels: f of c00 = a, c01 = b only is a + b, a + b, a - b, a - b per row. */
	for (qp = 0; qp <= 51; qp++) {
		memset(dc, 0, sizeof(dc));
		dc[0] = 7;
		dc[1] = -3;
		h264sw_luma_dc(dc, 208, qp);
		for (row = 0; row < 4U; row++) {
			for (column = 0; column < 4U; column++) {
				expected = reference_scale(7 + (-3) * sign_c01[column], 208, qp, 36, 6);
				CHECK(dc[4U * row + column] == expected);
			}
		}
	}
}

/* Checks the chroma DC transform and scaling for 4:2:0 (8.5.11). */
static void
check_chroma_dc(void)
{
	int32_t dc[4];
	int qp;
	int64_t scaled;

	/*
	 * c = (1, 2, 3, 4) as c00, c01, c10, c11: f00 = 10, f01 = 1 - 2 + 3 -
	 * 4 = -2, f10 = 1 + 2 - 3 - 4 = -4, f11 = 1 - 2 - 3 + 4 = 0.  At qP
	 * 0 with scale 160: ( f * 160 ) >> 5 = 50, -10, -20, 0.
	 */
	dc[0] = 1;
	dc[1] = 2;
	dc[2] = 3;
	dc[3] = 4;
	h264sw_chroma_dc(dc, 160, 0);
	CHECK(dc[0] == 50);
	CHECK(dc[1] == -10);
	CHECK(dc[2] == -20);
	CHECK(dc[3] == 0);

	/* The same levels at qP 12 (qP / 6 = 2): the product is shifted left by 2 first: 4 times the values above. */
	dc[0] = 1;
	dc[1] = 2;
	dc[2] = 3;
	dc[3] = 4;
	h264sw_chroma_dc(dc, 160, 12);
	CHECK(dc[0] == 200);
	CHECK(dc[1] == -40);
	CHECK(dc[2] == -80);
	CHECK(dc[3] == 0);

	/* c00 = 1: f = 1 everywhere; qP 0, scale 160: 160 >> 5 = 5. */
	dc[0] = 1;
	dc[1] = 0;
	dc[2] = 0;
	dc[3] = 0;
	h264sw_chroma_dc(dc, 160, 0);
	CHECK(dc[0] == 5 && dc[1] == 5 && dc[2] == 5 && dc[3] == 5);

	/* Scale 176 (m = 1): 176 >> 5 = 5 (5.5), and -176 >> 5 = -6 (floor of -5.5). */
	dc[0] = 1;
	dc[1] = 0;
	dc[2] = 0;
	dc[3] = 0;
	h264sw_chroma_dc(dc, 176, 1);
	CHECK(dc[0] == 5 && dc[3] == 5);
	dc[0] = -1;
	dc[1] = 0;
	dc[2] = 0;
	dc[3] = 0;
	h264sw_chroma_dc(dc, 176, 1);
	CHECK(dc[0] == -6 && dc[1] == -6 && dc[2] == -6 && dc[3] == -6);

	/* c01 = 1 alone: f = (1, -1, 1, -1); at qP 51 (qP / 6 = 8) and scale 224 the product 224 << 8 = 57344 >> 5 = 1792. */
	dc[0] = 0;
	dc[1] = 1;
	dc[2] = 0;
	dc[3] = 0;
	h264sw_chroma_dc(dc, 224, 51);
	CHECK(dc[0] == 1792 && dc[1] == -1792 && dc[2] == 1792 && dc[3] == -1792);

	/* c10 = 1 alone: f = (1, 1, -1, -1); and c11 = 1 alone: f = (1, -1, -1, 1). */
	dc[0] = 0;
	dc[1] = 0;
	dc[2] = 1;
	dc[3] = 0;
	h264sw_chroma_dc(dc, 160, 0);
	CHECK(dc[0] == 5 && dc[1] == 5 && dc[2] == -5 && dc[3] == -5);
	dc[0] = 0;
	dc[1] = 0;
	dc[2] = 0;
	dc[3] = 1;
	h264sw_chroma_dc(dc, 160, 0);
	CHECK(dc[0] == 5 && dc[1] == -5 && dc[2] == -5 && dc[3] == 5);

	/* A sweep of qP for c00 = 9, c01 = -4 (f = 5, 13, 5, 13): ( f * 192 * 2^( qP / 6 ) ) >> 5, rounded down. */
	for (qp = 0; qp <= 51; qp++) {
		dc[0] = 9;
		dc[1] = -4;
		dc[2] = 0;
		dc[3] = 0;
		h264sw_chroma_dc(dc, 192, qp);
		scaled = floor_divide((int64_t)5 * 192 * ((int64_t)1 << (qp / 6)), 5U);
		CHECK(dc[0] == clamp_range(scaled));
		CHECK(dc[2] == clamp_range(scaled));
		scaled = floor_divide((int64_t)13 * 192 * ((int64_t)1 << (qp / 6)), 5U);
		CHECK(dc[1] == clamp_range(scaled));
		CHECK(dc[3] == clamp_range(scaled));
	}
}

/* Checks the construction of samples (8.5.14). */
static void
check_add_residual(void)
{
	uint8_t pred[8 * 10];
	uint8_t out[8 * 12];
	int32_t residual[64];
	unsigned row;
	unsigned column;
	unsigned i;

	/* The sum is clipped to 0..255: pred 100 with +200 gives 255, with -200 gives 0. */
	memset(pred, 100, sizeof(pred));
	for (i = 0; i < 16U; i++)
		residual[i] = 200;
	memset(out, 9, sizeof(out));
	h264sw_add_residual(pred, 10, residual, 4, out, 12);
	for (row = 0; row < 4U; row++) {
		for (column = 0; column < 4U; column++)
			CHECK(out[row * 12U + column] == 255);
		CHECK(out[row * 12U + 4U] == 9);
	}
	CHECK(out[4U * 12U] == 9);
	for (i = 0; i < 16U; i++)
		residual[i] = -200;
	h264sw_add_residual(pred, 10, residual, 4, out, 12);
	for (row = 0; row < 4U; row++) {
		for (column = 0; column < 4U; column++)
			CHECK(out[row * 12U + column] == 0);
	}

	/* The edges: 0 - 1 gives 0, 255 + 1 gives 255, 250 + 5 gives 255, 5 - 5 gives 0, 254 + 1 gives 255, 1 - 1 gives 0, 7 + 0 gives 7. */
	pred[0] = 0;
	pred[1] = 255;
	pred[2] = 250;
	pred[3] = 5;
	pred[10] = 254;
	pred[11] = 1;
	pred[12] = 7;
	pred[13] = 128;
	residual[0] = -1;
	residual[1] = 1;
	residual[2] = 5;
	residual[3] = -5;
	residual[4] = 1;
	residual[5] = -1;
	residual[6] = 0;
	residual[7] = -128;
	for (i = 8; i < 16U; i++)
		residual[i] = 0;
	h264sw_add_residual(pred, 10, residual, 4, out, 12);
	CHECK(out[0] == 0 && out[1] == 255 && out[2] == 255 && out[3] == 0);
	CHECK(out[12] == 255 && out[13] == 0 && out[14] == 7 && out[15] == 0);

	/* An 8x8 block reads the residual in raster order of 8 and the strides apply to rows. */
	for (row = 0; row < 8U; row++) {
		for (column = 0; column < 10U; column++)
			pred[row * 10U + column] = (uint8_t)(row * 10U + column);
	}
	for (i = 0; i < 64U; i++)
		residual[i] = (int32_t)i;
	memset(out, 0, sizeof(out));
	h264sw_add_residual(pred, 10, residual, 8, out, 12);
	for (row = 0; row < 8U; row++) {
		for (column = 0; column < 8U; column++)
			CHECK(out[row * 12U + column] == (uint8_t)(row * 10U + column + row * 8U + column));
		CHECK(out[row * 12U + 8U] == 0);
	}

	/* The output may be the prediction itself. */
	for (row = 0; row < 4U; row++) {
		for (column = 0; column < 4U; column++)
			pred[row * 10U + column] = (uint8_t)(50U + row);
	}
	for (i = 0; i < 16U; i++)
		residual[i] = (int32_t)i - 8;
	h264sw_add_residual(pred, 10, residual, 4, pred, 10);
	for (row = 0; row < 4U; row++) {
		for (column = 0; column < 4U; column++)
			CHECK(pred[row * 10U + column] == (uint8_t)(50 + (int)row + (int)(row * 4U + column) - 8));
	}

	/* The largest and the smallest residuals clip without overflow. */
	pred[0] = 255;
	pred[1] = 0;
	pred[2] = 255;
	pred[3] = 0;
	residual[0] = INT_MAX;
	residual[1] = INT_MAX;
	residual[2] = INT_MIN;
	residual[3] = INT_MIN;
	for (i = 4; i < 16U; i++)
		residual[i] = 0;
	h264sw_add_residual(pred, 10, residual, 4, out, 12);
	CHECK(out[0] == 255 && out[1] == 255 && out[2] == 0 && out[3] == 0);

	/* A size other than 4 and 8 leaves the output alone. */
	memset(out, 77, sizeof(out));
	h264sw_add_residual(pred, 10, residual, 5, out, 12);
	h264sw_add_residual(pred, 10, residual, 0, out, 12);
	h264sw_add_residual(pred, 10, residual, 16, out, 12);
	for (i = 0; i < sizeof(out); i++)
		CHECK(out[i] == 77);
}

/* Checks values far outside what a conforming stream holds: nothing overflows and results stay in range. */
static void
check_extremes(void)
{
	struct h264sw_levelscale levelscale;
	struct h264sw_scaling scaling;
	int32_t block[64];
	int32_t scale4[16];
	int32_t scale8[64];
	static const int32_t big[6] = { INT_MAX, INT_MIN, 32767, -32768, 1000000, -1000000 };
	static const int qps[9] = { INT_MIN, -100, -1, 0, 35, 51, 52, 1000, INT_MAX };
	unsigned a;
	unsigned q;
	unsigned i;

	/* Scales of the largest weights, 255 times the largest v, with every level at 2^15 - 1 and -2^15. */
	memset(&scaling, 255, sizeof(scaling));
	h264sw_levelscale_init(&levelscale, &scaling);
	for (a = 0; a < 2U; a++) {
		for (q = 0; q <= 51U; q++) {
			for (i = 0; i < 16U; i++)
				block[i] = (a == 0U) ? 32767 : -32768;
			h264sw_scale4x4(block, levelscale.scale4x4[3][q % 6U], (int)q, 0);
			for (i = 0; i < 16U; i++)
				CHECK(in_range(block[i]));
			h264sw_idct4x4(block);
			for (i = 0; i < 16U; i++)
				CHECK(block[i] >= -512 && block[i] <= 512);
			for (i = 0; i < 64U; i++)
				block[i] = (a == 0U) ? 32767 : -32768;
			h264sw_scale8x8(block, levelscale.scale8x8[1][q % 6U], (int)q);
			for (i = 0; i < 64U; i++)
				CHECK(in_range(block[i]));
			h264sw_idct8x8(block);
			for (i = 0; i < 64U; i++)
				CHECK(block[i] >= -512 && block[i] <= 512);
		}
	}

	/* Levels and scales of any int32_t value, with any qP: the result is clamped and the transforms stay quiet. */
	for (i = 0; i < 16U; i++)
		scale4[i] = big[i % 6U];
	for (i = 0; i < 64U; i++)
		scale8[i] = big[(i + 1U) % 6U];
	for (a = 0; a < 6U; a++) {
		for (q = 0; q < 9U; q++) {
			for (i = 0; i < 16U; i++)
				block[i] = big[(i + a) % 6U];
			h264sw_scale4x4(block, scale4, qps[q], 0);
			for (i = 0; i < 16U; i++)
				CHECK(in_range(block[i]));
			for (i = 0; i < 16U; i++)
				block[i] = big[(i + a) % 6U];
			h264sw_scale4x4(block, scale4, qps[q], 1);
			for (i = 0; i < 16U; i++)
				CHECK(in_range(block[i]));
			for (i = 0; i < 64U; i++)
				block[i] = big[(i + a) % 6U];
			h264sw_scale8x8(block, scale8, qps[q]);
			for (i = 0; i < 64U; i++)
				CHECK(in_range(block[i]));
			for (i = 0; i < 16U; i++)
				block[i] = big[(i + a) % 6U];
			h264sw_luma_dc(block, big[a], qps[q]);
			for (i = 0; i < 16U; i++)
				CHECK(in_range(block[i]));
			for (i = 0; i < 4U; i++)
				block[i] = big[(i + a) % 6U];
			h264sw_chroma_dc(block, big[(a + 1U) % 6U], qps[q]);
			for (i = 0; i < 4U; i++)
				CHECK(in_range(block[i]));
		}
	}

	/* Transforms of blocks that hold any int32_t value stay in the range of a residual. */
	for (a = 0; a < 6U; a++) {
		for (i = 0; i < 16U; i++)
			block[i] = big[(i + a) % 6U];
		h264sw_idct4x4(block);
		for (i = 0; i < 16U; i++)
			CHECK(block[i] >= -512 && block[i] <= 512);
		for (i = 0; i < 64U; i++)
			block[i] = big[(i * 7U + a) % 6U];
		h264sw_idct8x8(block);
		for (i = 0; i < 64U; i++)
			CHECK(block[i] >= -512 && block[i] <= 512);
	}

	/* All levels at the top of the range with the largest weight, in the luma DC and the chroma DC at the largest qP. */
	for (i = 0; i < 16U; i++)
		block[i] = 32767;
	h264sw_luma_dc(block, 255 * 18, 51);
	for (i = 0; i < 16U; i++)
		CHECK(in_range(block[i]));
	for (i = 0; i < 4U; i++)
		block[i] = -32768;
	h264sw_chroma_dc(block, 255 * 18, 51);
	for (i = 0; i < 4U; i++)
		CHECK(in_range(block[i]));
}

/* Checks that a missing array is refused quietly. */
static void
check_missing(void)
{
	struct h264sw_levelscale levelscale;
	struct h264sw_scaling scaling;
	int32_t block[64];
	uint8_t samples[16];

	/* Every function accepts a missing array and does nothing. */
	memset(&levelscale, 0, sizeof(levelscale));
	memset(&scaling, 16, sizeof(scaling));
	memset(block, 0, sizeof(block));
	memset(samples, 0, sizeof(samples));
	h264sw_levelscale_init(NULL, &scaling);
	h264sw_levelscale_init(&levelscale, NULL);
	CHECK(levelscale.scale4x4[0][0][0] == 0);
	h264sw_unscan4x4(NULL, block);
	h264sw_unscan4x4(block, NULL);
	h264sw_unscan8x8(NULL, block);
	h264sw_unscan8x8(block, NULL);
	h264sw_scale4x4(NULL, block, 20, 0);
	h264sw_scale4x4(block, NULL, 20, 0);
	h264sw_idct4x4(NULL);
	h264sw_scale8x8(NULL, block, 20);
	h264sw_scale8x8(block, NULL, 20);
	h264sw_idct8x8(NULL);
	h264sw_luma_dc(NULL, 160, 20);
	h264sw_chroma_dc(NULL, 160, 20);
	h264sw_add_residual(NULL, 4, block, 4, samples, 4);
	h264sw_add_residual(samples, 4, NULL, 4, samples, 4);
	h264sw_add_residual(samples, 4, block, 4, NULL, 4);
	CHECK(block[0] == 0);
	CHECK(samples[0] == 0);
}
