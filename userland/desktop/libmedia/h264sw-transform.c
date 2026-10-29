/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transform coefficient decoding of the software H.264 decoder (8.5).
 */

#include "h264sw-transform.h"
#include "h264sw-tables.h"

#include <stddef.h>

/*
 * The bounds of the values that the clauses limit to
 * -2^( 7 + bitDepth ) .. 2^( 7 + bitDepth ) - 1 (8.5.10 to 8.5.13).  The
 * decoder reads 8-bit streams only, so bitDepth is 8.
 */
#define TRANSFORM_VALUE_MIN (-32768)
#define TRANSFORM_VALUE_MAX 32767

/* The largest qP of an 8-bit stream (QP'Y and QP'C range 0..51, 7.4.2.2 and 8.5.8). */
#define TRANSFORM_QP_MAX 51

/* Forward declarations of the static functions. */
static int32_t transform_clamp(int64_t value);
static int transform_clamp_qp(int qp);
static int32_t transform_shift_right32(int32_t value, unsigned count);
static int64_t transform_shift_right64(int64_t value, unsigned count);
static unsigned transform_norm8x8_column(unsigned row, unsigned column);
static void transform_line4(int32_t *line, unsigned step);
static void transform_line8(int32_t *line, unsigned step);
static void transform_hadamard4(int32_t *line, unsigned step);

/*
 * Computes the scaling functions from a picture's scaling lists (8.5.9).
 *
 * Both outputs are filled completely, for every list and every qP % 6, so
 * a caller needs no further preparation.  A missing argument leaves the
 * output as it is.
 */
void
h264sw_levelscale_init(
	struct h264sw_levelscale *levelscale,
	const struct h264sw_scaling *scaling)
{
	int32_t weight4x4[16];
	int32_t weight8x8[64];
	unsigned list;
	unsigned m;
	unsigned idx;
	unsigned position;
	unsigned row;
	unsigned column;
	unsigned k;

	/* Refuses a missing argument without touching the output. */
	if (levelscale == NULL)
		return;
	if (scaling == NULL)
		return;

	/* Derives LevelScale4x4 for each of the six 4x4 lists. */
	for (list = 0; list < 6U; list++) {
		/* Inverse scans the list into weightScale4x4 (8.5.6): scan position idx lands at the raster position the zig-zag table names. */
		for (idx = 0; idx < 16U; idx++)
			weight4x4[h264sw_zigzag4x4[idx]] = (int32_t)scaling->list4x4[list][idx];

		/* Multiplies the weights by normAdjust4x4 for each remainder of qP / 6 (8-313). */
		for (m = 0; m < 6U; m++) {
			/* Walks the 16 positions of the block in raster order. */
			for (position = 0; position < 16U; position++) {
				row = position / 4U;
				column = position % 4U;

				/*
				 * Picks the column of v (8-314): 0 where both indices
				 * are even, 1 where both are odd, 2 otherwise.
				 */
				k = 2U;
				if (row % 2U == 0U && column % 2U == 0U)
					k = 0U;
				else if (row % 2U == 1U && column % 2U == 1U)
					k = 1U;

				/* Stores the product of the weight and the normalization (8-313). */
				levelscale->scale4x4[list][m][position] = weight4x4[position] * (int32_t)h264sw_norm4x4[m][k];
			}
		}
	}

	/* Derives LevelScale8x8 for each of the two 8x8 lists. */
	for (list = 0; list < 2U; list++) {
		/* Inverse scans the list into weightScale8x8 (8.5.7). */
		for (idx = 0; idx < 64U; idx++)
			weight8x8[h264sw_zigzag8x8[idx]] = (int32_t)scaling->list8x8[list][idx];

		/* Multiplies the weights by normAdjust8x8 for each remainder of qP / 6 (8-316). */
		for (m = 0; m < 6U; m++) {
			/* Walks the 64 positions of the block in raster order. */
			for (position = 0; position < 64U; position++) {
				row = position / 8U;
				column = position % 8U;
				k = transform_norm8x8_column(row, column);

				/* Stores the product of the weight and the normalization (8-316). */
				levelscale->scale8x8[list][m][position] = weight8x8[position] * (int32_t)h264sw_norm8x8[m][k];
			}
		}
	}
}

/*
 * Places a 4x4 block's levels from scan order to raster order (8.5.6, zig-zag).
 *
 * The two arrays may be the same array.  A missing array leaves the
 * output as it is.
 */
void
h264sw_unscan4x4(
	const int32_t *levels,
	int32_t *block)
{
	int32_t copy[16];
	unsigned idx;

	/* Refuses a missing array. */
	if (levels == NULL)
		return;
	if (block == NULL)
		return;

	/* Copies the levels first, so the output may share the input's storage. */
	for (idx = 0; idx < 16U; idx++)
		copy[idx] = levels[idx];

	/* Puts the level at scan position idx at the raster position of that position. */
	for (idx = 0; idx < 16U; idx++)
		block[h264sw_zigzag4x4[idx]] = copy[idx];
}

/*
 * Places an 8x8 block's 64 levels from scan order to raster order (8.5.7, 8x8 zig-zag).
 *
 * The two arrays may be the same array.  A missing array leaves the
 * output as it is.
 */
void
h264sw_unscan8x8(
	const int32_t *levels,
	int32_t *block)
{
	int32_t copy[64];
	unsigned idx;

	/* Refuses a missing array. */
	if (levels == NULL)
		return;
	if (block == NULL)
		return;

	/* Copies the levels first, so the output may share the input's storage. */
	for (idx = 0; idx < 64U; idx++)
		copy[idx] = levels[idx];

	/* Puts the level at scan position idx at the raster position of that position. */
	for (idx = 0; idx < 64U; idx++)
		block[h264sw_zigzag8x8[idx]] = copy[idx];
}

/*
 * Scales a 4x4 block of levels (8.5.12.1).
 *
 * A qP outside 0..51 is taken as the nearest value in that range, and a
 * level outside the range of cij is taken as the nearest value in it.
 */
void
h264sw_scale4x4(
	int32_t *block,
	const int32_t *scale,
	int qp,
	int dc_done)
{
	int64_t product;
	int64_t scaled;
	int32_t level;
	unsigned position;
	unsigned qp_per;

	/* Refuses a missing array. */
	if (block == NULL)
		return;
	if (scale == NULL)
		return;

	/* Takes qP into the range the shift counts below are defined for. */
	qp = transform_clamp_qp(qp);
	qp_per = (unsigned)qp / 6U;

	/* Scales the 16 coefficients, one at a time. */
	for (position = 0; position < 16U; position++) {
		/* Takes the level into the range the bitstream may hold (8.5.12.1). */
		level = transform_clamp((int64_t)block[position]);

		/* Keeps a DC that an earlier process already scaled (8-335). */
		if (position == 0U && dc_done != 0) {
			block[position] = level;
			continue;
		}

		/* The product fits int64_t: 2^15 times a scale of at most 2^31 stays below 2^46. */
		product = (int64_t)level * (int64_t)scale[position];

		/*
		 * Scales by the shift of qP / 6: a left shift (written as a
		 * multiplication) from qP 24 on (8-336), a rounding right shift
		 * below it (8-337).
		 */
		if (qp >= 24) {
			scaled = product * ((int64_t)1 << (qp_per - 4U));
		} else {
			scaled = transform_shift_right64(product + ((int64_t)1 << (3U - qp_per)), 4U - qp_per);
		}

		/* Keeps the scaled coefficient within the range dij may have. */
		block[position] = transform_clamp(scaled);
	}
}

/*
 * Transforms a scaled 4x4 block into residual samples (8.5.12.2).
 *
 * A coefficient outside the range of dij is taken as the nearest value
 * in it, and every intermediate value is held to its range.
 */
void
h264sw_idct4x4(
	int32_t *block)
{
	unsigned i;

	/* Refuses a missing array. */
	if (block == NULL)
		return;

	/* Takes every coefficient into the range of dij. */
	for (i = 0; i < 16U; i++)
		block[i] = transform_clamp((int64_t)block[i]);

	/* Transforms each row (8-338 to 8-345). */
	for (i = 0; i < 4U; i++)
		transform_line4(&block[4U * i], 1U);

	/* Transforms each column (8-346 to 8-353). */
	for (i = 0; i < 4U; i++)
		transform_line4(&block[i], 4U);

	/* Rounds each sample to the residual (8-354). */
	for (i = 0; i < 16U; i++)
		block[i] = transform_shift_right32(block[i] + 32, 6U);
}

/*
 * Scales an 8x8 block of levels (8.5.13.1).
 *
 * A qP outside 0..51 is taken as the nearest value in that range, and a
 * level outside the range of cij is taken as the nearest value in it.
 */
void
h264sw_scale8x8(
	int32_t *block,
	const int32_t *scale,
	int qp)
{
	int64_t product;
	int64_t scaled;
	int32_t level;
	unsigned position;
	unsigned qp_per;

	/* Refuses a missing array. */
	if (block == NULL)
		return;
	if (scale == NULL)
		return;

	/* Takes qP into the range the shift counts below are defined for. */
	qp = transform_clamp_qp(qp);
	qp_per = (unsigned)qp / 6U;

	/* Scales the 64 coefficients, one at a time. */
	for (position = 0; position < 64U; position++) {
		/* Takes the level into the range the bitstream may hold (8.5.13.1). */
		level = transform_clamp((int64_t)block[position]);

		/* The product fits int64_t: 2^15 times a scale of at most 2^31 stays below 2^46. */
		product = (int64_t)level * (int64_t)scale[position];

		/*
		 * Scales by the shift of qP / 6: a left shift (written as a
		 * multiplication) from qP 36 on (8-356), a rounding right shift
		 * below it (8-357).
		 */
		if (qp >= 36) {
			scaled = product * ((int64_t)1 << (qp_per - 6U));
		} else {
			scaled = transform_shift_right64(product + ((int64_t)1 << (5U - qp_per)), 6U - qp_per);
		}

		/* Keeps the scaled coefficient within the range dij may have. */
		block[position] = transform_clamp(scaled);
	}
}

/*
 * Transforms a scaled 8x8 block into residual samples (8.5.13.2: rows, then columns, then rij = ( mij + 32 ) >> 6).
 *
 * A coefficient outside the range of dij is taken as the nearest value
 * in it, and every intermediate value is held to its range.
 */
void
h264sw_idct8x8(
	int32_t *block)
{
	unsigned i;

	/* Refuses a missing array. */
	if (block == NULL)
		return;

	/* Takes every coefficient into the range of dij. */
	for (i = 0; i < 64U; i++)
		block[i] = transform_clamp((int64_t)block[i]);

	/* Transforms each row (8-358 to 8-381). */
	for (i = 0; i < 8U; i++)
		transform_line8(&block[8U * i], 1U);

	/* Transforms each column (8-382 to 8-405). */
	for (i = 0; i < 8U; i++)
		transform_line8(&block[i], 8U);

	/* Rounds each sample to the residual (8-406). */
	for (i = 0; i < 64U; i++)
		block[i] = transform_shift_right32(block[i] + 32, 6U);
}

/*
 * Transforms and scales the 16 DC levels of an Intra_16x16 macroblock (8.5.10).
 *
 * A qP outside 0..51 is taken as the nearest value in that range, and a
 * level outside the range of cij is taken as the nearest value in it.
 */
void
h264sw_luma_dc(
	int32_t *dc,
	int32_t scale00,
	int qp)
{
	int64_t scaled;
	unsigned i;
	unsigned qp_per;

	/* Refuses a missing array. */
	if (dc == NULL)
		return;

	/* Takes qP into the range the shift counts below are defined for. */
	qp = transform_clamp_qp(qp);
	qp_per = (unsigned)qp / 6U;

	/* Takes every level into the range of cij. */
	for (i = 0; i < 16U; i++)
		dc[i] = transform_clamp((int64_t)dc[i]);

	/* Transforms each row with the 4x4 matrix of 8-320 (the right factor). */
	for (i = 0; i < 4U; i++)
		transform_hadamard4(&dc[4U * i], 1U);

	/* Transforms each column with the 4x4 matrix of 8-320 (the left factor). */
	for (i = 0; i < 4U; i++)
		transform_hadamard4(&dc[i], 4U);

	/* Scales the 16 transformed values, one at a time. */
	for (i = 0; i < 16U; i++) {
		/* Holds fij to its range before it is used again. */
		dc[i] = transform_clamp((int64_t)dc[i]);

		/*
		 * Scales by the shift of qP / 6: a left shift (written as a
		 * multiplication) from qP 36 on (8-321), a rounding right shift
		 * below it (8-322).
		 */
		scaled = (int64_t)dc[i] * (int64_t)scale00;
		if (qp >= 36) {
			scaled = scaled * ((int64_t)1 << (qp_per - 6U));
		} else {
			scaled = transform_shift_right64(scaled + ((int64_t)1 << (5U - qp_per)), 6U - qp_per);
		}

		/* Keeps the scaled DC within the range dcYij may have. */
		dc[i] = transform_clamp(scaled);
	}
}

/*
 * Transforms and scales the four DC levels of a 4:2:0 chroma component (8.5.11).
 *
 * A qP outside 0..51 is taken as the nearest value in that range, and a
 * level outside the range of cij is taken as the nearest value in it.
 */
void
h264sw_chroma_dc(
	int32_t *dc,
	int32_t scale00,
	int qp)
{
	int64_t c00;
	int64_t c01;
	int64_t c10;
	int64_t c11;
	int64_t f[4];
	int64_t scaled;
	unsigned i;
	unsigned qp_per;

	/* Refuses a missing array. */
	if (dc == NULL)
		return;

	/* Takes qP into the range the shift count below is defined for. */
	qp = transform_clamp_qp(qp);
	qp_per = (unsigned)qp / 6U;

	/* Takes every level into the range of cij. */
	c00 = (int64_t)transform_clamp((int64_t)dc[0]);
	c01 = (int64_t)transform_clamp((int64_t)dc[1]);
	c10 = (int64_t)transform_clamp((int64_t)dc[2]);
	c11 = (int64_t)transform_clamp((int64_t)dc[3]);

	/* Applies the 2x2 transform of 8-324: f = [1 1; 1 -1] * c * [1 1; 1 -1]. */
	f[0] = c00 + c01 + c10 + c11;
	f[1] = c00 - c01 + c10 - c11;
	f[2] = c00 + c01 - c10 - c11;
	f[3] = c00 - c01 - c10 + c11;

	/* Scales the four transformed values, one at a time. */
	for (i = 0; i < 4U; i++) {
		/* Holds fij to its range before it is used again. */
		f[i] = (int64_t)transform_clamp(f[i]);

		/* Scales by ( ( fij * LevelScale4x4 ) << ( qP / 6 ) ) >> 5 (8-326); the left shift is a multiplication. */
		scaled = f[i] * (int64_t)scale00;
		scaled = scaled * ((int64_t)1 << qp_per);
		scaled = transform_shift_right64(scaled, 5U);

		/* Keeps the scaled DC within the range dcCij may have. */
		dc[i] = transform_clamp(scaled);
	}
}

/*
 * Adds a residual block to its prediction and clips (8.5.14).
 *
 * A size other than 4 and 8, and a missing array, leave the output as it
 * is.  Each sample is read before it is written, so out may be pred.
 */
void
h264sw_add_residual(
	const uint8_t *pred,
	size_t pred_stride,
	const int32_t *residual,
	unsigned size,
	uint8_t *out,
	size_t out_stride)
{
	int64_t sum;
	unsigned row;
	unsigned column;

	/* Refuses a missing array. */
	if (pred == NULL)
		return;
	if (residual == NULL)
		return;
	if (out == NULL)
		return;

	/* Refuses a block size the residual layout does not have. */
	if (size != 4U && size != 8U)
		return;

	/* Builds each row of the block. */
	for (row = 0; row < size; row++) {
		/* Builds each sample of the row. */
		for (column = 0; column < size; column++) {
			/* Sums in int64_t, so a residual of any int32_t value cannot overflow. */
			sum = (int64_t)pred[row * pred_stride + column] + (int64_t)residual[row * size + column];

			/* Clips the sample to the 8-bit range 0..255. */
			if (sum < 0) {
				sum = 0;
			} else if (sum > 255) {
				sum = 255;
			}

			out[row * out_stride + column] = (uint8_t)sum;
		}
	}
}

/*
 * Holds a value to the range the clauses give to cij, dij, eij and the
 * like for 8 bits: -32768..32767.
 */
static int32_t
transform_clamp(
	int64_t value)
{
	/* Takes a value below the range up to its lowest. */
	if (value < (int64_t)TRANSFORM_VALUE_MIN)
		return TRANSFORM_VALUE_MIN;

	/* Takes a value above the range down to its highest. */
	if (value > (int64_t)TRANSFORM_VALUE_MAX)
		return TRANSFORM_VALUE_MAX;

	/* The value is in the range and stays as it is. */
	return (int32_t)value;
}

/* Holds qP to 0..51, the range a shift count of qP / 6 stays defined for. */
static int
transform_clamp_qp(
	int qp)
{
	/* Takes a qP below the range up to 0. */
	if (qp < 0)
		return 0;

	/* Takes a qP above the range down to its highest. */
	if (qp > TRANSFORM_QP_MAX)
		return TRANSFORM_QP_MAX;

	/* The qP is in the range and stays as it is. */
	return qp;
}

/*
 * Shifts an int32_t right as the clauses' ">>" does: an arithmetic shift,
 * the floor of value / 2^count, whatever the sign of the value.
 */
static int32_t
transform_shift_right32(
	int32_t value,
	unsigned count)
{
	/* A non-negative value shifts right with no sign to extend. */
	if (value >= 0)
		return value >> count;

	/*
	 * A negative value is complemented, shifted and complemented back:
	 * the complement of a negative value is non-negative, and shifting
	 * that rounds the original value toward minus infinity.
	 */
	return ~((~value) >> count);
}

/* Shifts an int64_t right as the clauses' ">>" does, as transform_shift_right32 does for an int32_t. */
static int64_t
transform_shift_right64(
	int64_t value,
	unsigned count)
{
	/* A non-negative value shifts right with no sign to extend. */
	if (value >= 0)
		return value >> count;

	/* A negative value is complemented, shifted and complemented back, which rounds toward minus infinity. */
	return ~((~value) >> count);
}

/*
 * Picks the column of v (8-318) that normAdjust8x8 uses at row and
 * column (8-317), the cases tried in the order the equation lists them.
 */
static unsigned
transform_norm8x8_column(
	unsigned row,
	unsigned column)
{
	unsigned row4;
	unsigned column4;
	unsigned row2;
	unsigned column2;

	/* Reduces the indices as the equation does. */
	row4 = row % 4U;
	column4 = column % 4U;
	row2 = row % 2U;
	column2 = column % 2U;

	/* ( i % 4, j % 4 ) equal to ( 0, 0 ) selects column 0. */
	if (row4 == 0U && column4 == 0U)
		return 0U;

	/* ( i % 2, j % 2 ) equal to ( 1, 1 ) selects column 1. */
	if (row2 == 1U && column2 == 1U)
		return 1U;

	/* ( i % 4, j % 4 ) equal to ( 2, 2 ) selects column 2. */
	if (row4 == 2U && column4 == 2U)
		return 2U;

	/* ( i % 4, j % 2 ) equal to ( 0, 1 ) selects column 3. */
	if (row4 == 0U && column2 == 1U)
		return 3U;

	/* ( i % 2, j % 4 ) equal to ( 1, 0 ) selects column 3. */
	if (row2 == 1U && column4 == 0U)
		return 3U;

	/* ( i % 4, j % 4 ) equal to ( 0, 2 ) selects column 4. */
	if (row4 == 0U && column4 == 2U)
		return 4U;

	/* ( i % 4, j % 4 ) equal to ( 2, 0 ) selects column 4. */
	if (row4 == 2U && column4 == 0U)
		return 4U;

	/* Every other position selects column 5. */
	return 5U;
}

/*
 * Applies the one-dimensional 4x4 inverse transform to the four values
 * line[0], line[step], line[2 * step] and line[3 * step] (8-338 to 8-345
 * for a row, 8-346 to 8-353 for a column), holding the intermediate and
 * the output values to the range the clauses give them.
 */
static void
transform_line4(
	int32_t *line,
	unsigned step)
{
	int32_t d0;
	int32_t d1;
	int32_t d2;
	int32_t d3;
	int32_t e0;
	int32_t e1;
	int32_t e2;
	int32_t e3;

	/* Reads the four inputs, already within the range. */
	d0 = line[0];
	d1 = line[step];
	d2 = line[2U * step];
	d3 = line[3U * step];

	/* Computes the intermediate values (8-338 to 8-341); the sums are formed in int64_t and held to the range. */
	e0 = transform_clamp((int64_t)d0 + (int64_t)d2);
	e1 = transform_clamp((int64_t)d0 - (int64_t)d2);
	e2 = transform_clamp((int64_t)transform_shift_right32(d1, 1U) - (int64_t)d3);
	e3 = transform_clamp((int64_t)d1 + (int64_t)transform_shift_right32(d3, 1U));

	/* Computes the outputs (8-342 to 8-345) and stores them. */
	line[0] = transform_clamp((int64_t)e0 + (int64_t)e3);
	line[step] = transform_clamp((int64_t)e1 + (int64_t)e2);
	line[2U * step] = transform_clamp((int64_t)e1 - (int64_t)e2);
	line[3U * step] = transform_clamp((int64_t)e0 - (int64_t)e3);
}

/*
 * Applies the one-dimensional 8x8 inverse transform to the eight values
 * line[0], line[step] .. line[7 * step] (8-358 to 8-381 for a row, 8-382
 * to 8-405 for a column), holding the intermediate and the output values
 * to the range the clauses give them.
 */
static void
transform_line8(
	int32_t *line,
	unsigned step)
{
	int32_t d[8];
	int32_t e[8];
	int32_t f[8];
	unsigned i;

	/* Reads the eight inputs, already within the range. */
	for (i = 0; i < 8U; i++)
		d[i] = line[i * step];

	/* Computes the first set of intermediate values (8-358 to 8-365). */
	e[0] = transform_clamp((int64_t)d[0] + (int64_t)d[4]);
	e[1] = transform_clamp(-(int64_t)d[3] + (int64_t)d[5] - (int64_t)d[7] - (int64_t)transform_shift_right32(d[7], 1U));
	e[2] = transform_clamp((int64_t)d[0] - (int64_t)d[4]);
	e[3] = transform_clamp((int64_t)d[1] + (int64_t)d[7] - (int64_t)d[3] - (int64_t)transform_shift_right32(d[3], 1U));
	e[4] = transform_clamp((int64_t)transform_shift_right32(d[2], 1U) - (int64_t)d[6]);
	e[5] = transform_clamp(-(int64_t)d[1] + (int64_t)d[7] + (int64_t)d[5] + (int64_t)transform_shift_right32(d[5], 1U));
	e[6] = transform_clamp((int64_t)d[2] + (int64_t)transform_shift_right32(d[6], 1U));
	e[7] = transform_clamp((int64_t)d[3] + (int64_t)d[5] + (int64_t)d[1] + (int64_t)transform_shift_right32(d[1], 1U));

	/* Computes the second set of intermediate values (8-366 to 8-373). */
	f[0] = transform_clamp((int64_t)e[0] + (int64_t)e[6]);
	f[1] = transform_clamp((int64_t)e[1] + (int64_t)transform_shift_right32(e[7], 2U));
	f[2] = transform_clamp((int64_t)e[2] + (int64_t)e[4]);
	f[3] = transform_clamp((int64_t)e[3] + (int64_t)transform_shift_right32(e[5], 2U));
	f[4] = transform_clamp((int64_t)e[2] - (int64_t)e[4]);
	f[5] = transform_clamp((int64_t)transform_shift_right32(e[3], 2U) - (int64_t)e[5]);
	f[6] = transform_clamp((int64_t)e[0] - (int64_t)e[6]);
	f[7] = transform_clamp((int64_t)e[7] - (int64_t)transform_shift_right32(e[1], 2U));

	/* Computes the outputs (8-374 to 8-381) and stores them. */
	line[0] = transform_clamp((int64_t)f[0] + (int64_t)f[7]);
	line[step] = transform_clamp((int64_t)f[2] + (int64_t)f[5]);
	line[2U * step] = transform_clamp((int64_t)f[4] + (int64_t)f[3]);
	line[3U * step] = transform_clamp((int64_t)f[6] + (int64_t)f[1]);
	line[4U * step] = transform_clamp((int64_t)f[6] - (int64_t)f[1]);
	line[5U * step] = transform_clamp((int64_t)f[4] - (int64_t)f[3]);
	line[6U * step] = transform_clamp((int64_t)f[2] - (int64_t)f[5]);
	line[7U * step] = transform_clamp((int64_t)f[0] - (int64_t)f[7]);
}

/*
 * Multiplies the four values line[0], line[step], line[2 * step] and
 * line[3 * step] by the 4x4 matrix of 8-320, whose rows are (1, 1, 1,
 * 1), (1, 1, -1, -1), (1, -1, -1, 1) and (1, -1, 1, -1).  The matrix is
 * symmetric, so it serves a row (a product on the right) and a column
 * (a product on the left) alike.  The inputs are at most 2^15 and the
 * two passes sum four values each, so the sums stay below 2^19 and fit
 * int32_t.
 */
static void
transform_hadamard4(
	int32_t *line,
	unsigned step)
{
	int32_t a;
	int32_t b;
	int32_t c;
	int32_t d;

	/* Reads the four inputs. */
	a = line[0];
	b = line[step];
	c = line[2U * step];
	d = line[3U * step];

	/* Stores the four rows of the matrix product. */
	line[0] = a + b + c + d;
	line[step] = a + b - c - d;
	line[2U * step] = a - b - c + d;
	line[3U * step] = a - b + c - d;
}
