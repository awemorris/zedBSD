/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The accurate integer inverse DCT: the Loeffler, Ligtenberg and
 * Moschytz algorithm with 13-bit constants in 64-bit arithmetic (libjpeg's
 * JLONG), in two passes (columns, then
 * rows) with 2 extra bits kept between them, as libjpeg's "islow" method
 * computes it, so that the samples are the ones libjpeg gives.  The
 * results are clamped to 0..255 around 128 with libjpeg's range limit
 * (whose masking also folds values far out of range).
 *
 * The reduced transforms (ws157-p017) give a block as 4 by 4, 2 by 2 or
 * 1 by 1 samples for an output scaled by 1/2, 1/4 or 1/8: each sample is
 * the average of the 2 by 2, 4 by 4 or 8 by 8 samples the full transform
 * would give under it, worked out directly from the coefficients with
 * the averaged cosines of idct_reduced_weights_*.  The averages of the
 * frequencies that cancel over a cell (4 for 4 by 4; 2, 4 and 6 for 2 by
 * 2; all but the DC for 1 by 1) are exactly zero, as in libjpeg's reduced
 * transforms, whose results these match within the rounding.
 */

#include "internal.h"

/* The fraction bits of the constants, and the extra bits kept after the first pass. */
#define IDCT_CONST_BITS		13
#define IDCT_PASS1_BITS		2

/* The constants: FIX(x) = x * 2^13, rounded. */
#define FIX_0_298631336		((int32_t)2446)
#define FIX_0_390180644		((int32_t)3196)
#define FIX_0_541196100		((int32_t)4433)
#define FIX_0_765366865		((int32_t)6270)
#define FIX_0_899976223		((int32_t)7373)
#define FIX_1_175875602		((int32_t)9633)
#define FIX_1_501321110		((int32_t)12299)
#define FIX_1_847759065		((int32_t)15137)
#define FIX_1_961570560		((int32_t)16069)
#define FIX_2_053119869		((int32_t)16819)
#define FIX_2_562915447		((int32_t)20995)
#define FIX_3_072711026		((int32_t)25172)

/* The bits of the range limit's index (values are folded modulo 1024). */
#define IDCT_RANGE_MASK		1023

/* The most samples on a side a reduced block has. */
#define IDCT_REDUCED_MAX	4

/*
 * The eight outputs of one 8-point pass: the even part's four sums and
 * the odd part's four terms, combined as out[k] = even[k] +/- odd.
 */
struct idct_terms {
	int64_t tmp10;
	int64_t tmp11;
	int64_t tmp12;
	int64_t tmp13;
	int64_t tmp0;
	int64_t tmp1;
	int64_t tmp2;
	int64_t tmp3;
};

/*
 * The weights of the reduced transforms: entry [X][u] is how much of
 * coefficient u (a column's or a row's, dequantized) goes into sample X
 * of the reduced side, times 2^13.  It is the average, over the 8 / N
 * samples of the full side under sample X, of the full transform's
 * basis: k(u) cos((2x + 1) u pi / 16), where k(0) = 1 and k(u) = sqrt(2)
 * (the scale of libjpeg's passes, whose two passes leave a factor of 8
 * that the second pass's descaling removes).  The tables are constant
 * for the life of the program.
 */
static const int32_t idct_reduced_weights_4[4][DCTSIZE] = {
	{ 8192, 10498, 7568, 3686, 0, -2463, -3135, -2088 },
	{ 8192, 4348, -7568, -8900, 0, 5946, 3135, -865 },
	{ 8192, -4348, -7568, 8900, 0, -5946, 3135, 865 },
	{ 8192, -10498, 7568, -3686, 0, 2463, -3135, 2088 }
};

/* The weights of the 2 by 2 transform, as idct_reduced_weights_4 (constant). */
static const int32_t idct_reduced_weights_2[2][DCTSIZE] = {
	{ 8192, 7423, 0, -2607, 0, 1742, 0, -1477 },
	{ 8192, -7423, 0, 2607, 0, -1742, 0, 1477 }
};

/* The weights of the 1 by 1 transform: the DC alone (every other basis averages to zero over the block; constant). */
static const int32_t idct_reduced_weights_1[1][DCTSIZE] = {
	{ 8192, 0, 0, 0, 0, 0, 0, 0 }
};

static void idct_pass(int64_t in0, int64_t in1, int64_t in2, int64_t in3, int64_t in4, int64_t in5, int64_t in6, int64_t in7, struct idct_terms *terms);
static int32_t idct_descale(int64_t value, int bits);
static JSAMPLE idct_limit(int32_t value);
static int64_t idct_reduced_sum(const int32_t *weights, const int32_t *in, size_t step);

/*
 * Transforms one block: the coefficients (natural order) times the
 * quantization (natural order) into 8 rows of 8 samples, stride bytes
 * apart.
 */
void
jpeg_compat_idct(
	const JCOEF *coefficients,
	const UINT16 *quantization,
	JSAMPLE *out,
	size_t stride)
{
	struct idct_terms terms;
	int32_t work[DCTSIZE2];
	int32_t in[DCTSIZE];
	int32_t value;
	const int32_t *row;
	JSAMPLE *line;
	int column;
	int index;
	int zero;

	/* Pass 1: each column, dequantized, into the work array with PASS1_BITS more bits. */
	for (column = 0; column < DCTSIZE; column++) {
		zero = 1;
		for (index = 0; index < DCTSIZE; index++) {
			in[index] = (int32_t)coefficients[index * DCTSIZE + column] * (int32_t)quantization[index * DCTSIZE + column];
			if (index != 0 && in[index] != 0)
				zero = 0;
		}

		/* A column of DC alone is flat (the same as the full pass gives). */
		if (zero) {
			value = in[0] * (1 << IDCT_PASS1_BITS);
			for (index = 0; index < DCTSIZE; index++)
				work[index * DCTSIZE + column] = value;
			continue;
		}

		/* The 8-point transform, descaled to CONST_BITS - PASS1_BITS. */
		idct_pass(in[0], in[1], in[2], in[3], in[4], in[5], in[6], in[7], &terms);
		work[0 * DCTSIZE + column] = idct_descale(terms.tmp10 + terms.tmp3, IDCT_CONST_BITS - IDCT_PASS1_BITS);
		work[7 * DCTSIZE + column] = idct_descale(terms.tmp10 - terms.tmp3, IDCT_CONST_BITS - IDCT_PASS1_BITS);
		work[1 * DCTSIZE + column] = idct_descale(terms.tmp11 + terms.tmp2, IDCT_CONST_BITS - IDCT_PASS1_BITS);
		work[6 * DCTSIZE + column] = idct_descale(terms.tmp11 - terms.tmp2, IDCT_CONST_BITS - IDCT_PASS1_BITS);
		work[2 * DCTSIZE + column] = idct_descale(terms.tmp12 + terms.tmp1, IDCT_CONST_BITS - IDCT_PASS1_BITS);
		work[5 * DCTSIZE + column] = idct_descale(terms.tmp12 - terms.tmp1, IDCT_CONST_BITS - IDCT_PASS1_BITS);
		work[3 * DCTSIZE + column] = idct_descale(terms.tmp13 + terms.tmp0, IDCT_CONST_BITS - IDCT_PASS1_BITS);
		work[4 * DCTSIZE + column] = idct_descale(terms.tmp13 - terms.tmp0, IDCT_CONST_BITS - IDCT_PASS1_BITS);
	}

	/* Pass 2: each row of the work array into samples, descaled by 8 and the PASS1_BITS. */
	for (index = 0; index < DCTSIZE; index++) {
		row = &work[index * DCTSIZE];
		line = out + (size_t)index * stride;
		idct_pass(row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7], &terms);
		line[0] = idct_limit(idct_descale(terms.tmp10 + terms.tmp3, IDCT_CONST_BITS + IDCT_PASS1_BITS + 3));
		line[7] = idct_limit(idct_descale(terms.tmp10 - terms.tmp3, IDCT_CONST_BITS + IDCT_PASS1_BITS + 3));
		line[1] = idct_limit(idct_descale(terms.tmp11 + terms.tmp2, IDCT_CONST_BITS + IDCT_PASS1_BITS + 3));
		line[6] = idct_limit(idct_descale(terms.tmp11 - terms.tmp2, IDCT_CONST_BITS + IDCT_PASS1_BITS + 3));
		line[2] = idct_limit(idct_descale(terms.tmp12 + terms.tmp1, IDCT_CONST_BITS + IDCT_PASS1_BITS + 3));
		line[5] = idct_limit(idct_descale(terms.tmp12 - terms.tmp1, IDCT_CONST_BITS + IDCT_PASS1_BITS + 3));
		line[3] = idct_limit(idct_descale(terms.tmp13 + terms.tmp0, IDCT_CONST_BITS + IDCT_PASS1_BITS + 3));
		line[4] = idct_limit(idct_descale(terms.tmp13 - terms.tmp0, IDCT_CONST_BITS + IDCT_PASS1_BITS + 3));
	}
}

/*
 * Transforms one block into size by size samples (4, 2 or 1: the block
 * scaled by 1/2, 1/4 or 1/8), each the average of the full transform's
 * samples under it, stride bytes apart.
 */
void
jpeg_compat_idct_reduced(
	const JCOEF *coefficients,
	const UINT16 *quantization,
	int size,
	JSAMPLE *out,
	size_t stride)
{
	const int32_t (*weights)[DCTSIZE];
	int32_t work[IDCT_REDUCED_MAX * DCTSIZE];
	int32_t in[DCTSIZE2];
	int32_t value;
	int64_t sum;
	JSAMPLE *line;
	int column;
	int index;
	int zero;

	/* The weights of the size. */
	weights = idct_reduced_weights_4;
	if (size == 2) {
		weights = idct_reduced_weights_2;
	} else if (size == 1) {
		weights = idct_reduced_weights_1;
	}

	/* The coefficients dequantized. */
	for (index = 0; index < DCTSIZE2; index++)
		in[index] = (int32_t)coefficients[index] * (int32_t)quantization[index];

	/* Pass 1: each column into size values of the work array, with PASS1_BITS more bits. */
	for (column = 0; column < DCTSIZE; column++) {
		zero = 1;
		for (index = 1; index < DCTSIZE; index++) {
			if (in[index * DCTSIZE + column] != 0)
				zero = 0;
		}

		/* A column of DC alone is flat (every weight of the DC is 1). */
		if (zero) {
			value = in[column] * (1 << IDCT_PASS1_BITS);
			for (index = 0; index < size; index++)
				work[index * DCTSIZE + column] = value;
			continue;
		}

		/* Each reduced sample of the column: the weighted coefficients, descaled to CONST_BITS - PASS1_BITS. */
		for (index = 0; index < size; index++) {
			sum = idct_reduced_sum(weights[index], &in[column], DCTSIZE);
			work[index * DCTSIZE + column] = idct_descale(sum, IDCT_CONST_BITS - IDCT_PASS1_BITS);
		}
	}

	/* Pass 2: each of the size rows of the work array into samples, descaled by 8 and the PASS1_BITS. */
	for (index = 0; index < size; index++) {
		line = out + (size_t)index * stride;
		for (column = 0; column < size; column++) {
			sum = idct_reduced_sum(weights[column], &work[index * DCTSIZE], 1);
			line[column] = idct_limit(idct_descale(sum, IDCT_CONST_BITS + IDCT_PASS1_BITS + 3));
		}
	}
}

/* One 8-point inverse transform: the even part from inputs 0, 2, 4, 6, the odd part from 1, 3, 5, 7. */
static void
idct_pass(
	int64_t in0,
	int64_t in1,
	int64_t in2,
	int64_t in3,
	int64_t in4,
	int64_t in5,
	int64_t in6,
	int64_t in7,
	struct idct_terms *terms)
{
	int64_t z1;
	int64_t z2;
	int64_t z3;
	int64_t z4;
	int64_t z5;
	int64_t tmp0;
	int64_t tmp1;
	int64_t tmp2;
	int64_t tmp3;

	/* The even part: the rotation of 2 and 6, and the butterfly of 0 and 4. */
	z1 = (in2 + in6) * FIX_0_541196100;
	tmp2 = z1 + in6 * -FIX_1_847759065;
	tmp3 = z1 + in2 * FIX_0_765366865;
	tmp0 = (in0 + in4) * (1 << IDCT_CONST_BITS);
	tmp1 = (in0 - in4) * (1 << IDCT_CONST_BITS);
	terms->tmp10 = tmp0 + tmp3;
	terms->tmp13 = tmp0 - tmp3;
	terms->tmp11 = tmp1 + tmp2;
	terms->tmp12 = tmp1 - tmp2;

	/* The odd part: 7, 5, 3 and 1 through the shared rotation. */
	tmp0 = in7;
	tmp1 = in5;
	tmp2 = in3;
	tmp3 = in1;
	z1 = tmp0 + tmp3;
	z2 = tmp1 + tmp2;
	z3 = tmp0 + tmp2;
	z4 = tmp1 + tmp3;
	z5 = (z3 + z4) * FIX_1_175875602;
	tmp0 = tmp0 * FIX_0_298631336;
	tmp1 = tmp1 * FIX_2_053119869;
	tmp2 = tmp2 * FIX_3_072711026;
	tmp3 = tmp3 * FIX_1_501321110;
	z1 = z1 * -FIX_0_899976223;
	z2 = z2 * -FIX_2_562915447;
	z3 = z3 * -FIX_1_961570560 + z5;
	z4 = z4 * -FIX_0_390180644 + z5;
	terms->tmp0 = tmp0 + z1 + z3;
	terms->tmp1 = tmp1 + z2 + z4;
	terms->tmp2 = tmp2 + z2 + z3;
	terms->tmp3 = tmp3 + z1 + z4;
}

/* Divides by 2^bits, rounding half up (libjpeg's DESCALE with an arithmetic shift). */
static int32_t
idct_descale(
	int64_t value,
	int bits)
{
	/* The rounded quotient (it fits a sample's range after the passes, or folds as libjpeg's does). */
	return (int32_t)((value + ((int64_t)1 << (bits - 1))) >> bits);
}

/* Clamps a centred value to a sample as libjpeg's range-limit table does (folded modulo 1024). */
static JSAMPLE
idct_limit(
	int32_t value)
{
	int32_t folded;

	/* 0..127 above the centre, the rest of the top saturated, the far bottom saturated, and -128..-1. */
	folded = value & IDCT_RANGE_MASK;
	if (folded < 128)
		return (JSAMPLE)(folded + 128);
	if (folded < 384)
		return 255;
	if (folded < 896)
		return 0;

	/* A value from -128 to -1. */
	return (JSAMPLE)(folded - 896);
}

/* Weighs eight inputs (step apart) by a row of a reduced transform's weights. */
static int64_t
idct_reduced_sum(
	const int32_t *weights,
	const int32_t *in,
	size_t step)
{
	int64_t sum;
	int index;

	/* Each input times its weight. */
	sum = 0;
	for (index = 0; index < DCTSIZE; index++)
		sum += (int64_t)weights[index] * (int64_t)in[(size_t)index * step];

	/* Succeeded: the weighted sum, at 2^13 times the scale of the inputs. */
	return sum;
}
