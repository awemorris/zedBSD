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

/*
 * The widest range a product keeps when it is later multiplied by at
 * most 2^4 and held to the range above: a product beyond 2^16 gives the
 * same bound either way.
 */
#define TRANSFORM_PRODUCT_LIMIT 65536

/* The largest qP of an 8-bit stream (QP'Y and QP'C range 0..51, 7.4.2.2 and 8.5.8). */
#define TRANSFORM_QP_MAX 51

/* Forward declarations of the static functions. */
static int32_t transform_clamp(int64_t value);
static int transform_clamp_qp(int qp);
static int32_t transform_shift_right32(int32_t value, unsigned count);
static int64_t transform_shift_right64(int64_t value, unsigned count);
static unsigned transform_norm8x8_column(unsigned row, unsigned column);
static __inline int32_t transform_clamp32(int32_t value);
static __inline int32_t transform_clamp_wide32(int32_t value);
static unsigned transform_scale_narrow(const int32_t *scale, unsigned count);
static void transform_scale_wide(int32_t *block, const int32_t *scale, unsigned count, int qp, int threshold, unsigned base);
static __inline void transform_core4(int32_t *out, const int32_t *in);
static void transform_row4(int32_t *row);
static __inline void transform_core8(int32_t *out, const int32_t *in);
static void transform_row8(int32_t *row);
static __inline void transform_add_row(const uint8_t *pred, const int32_t *residual, uint8_t *out, unsigned width);
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
	int32_t level;
	int32_t kept;
	int32_t product;
	int32_t add;
	unsigned position;
	unsigned qp_per;
	unsigned narrow;
	unsigned shift;

	/* Refuses a missing array. */
	if (block == NULL)
		return;
	if (scale == NULL)
		return;

	/* Takes qP into the range the shift counts below are defined for. */
	qp = transform_clamp_qp(qp);
	qp_per = (unsigned)qp / 6U;

	/* Keeps the level of the DC, which an earlier process may already have scaled (8-335). */
	kept = transform_clamp32(block[0]);

	/* Takes the product in int64_t when a scale is too large for int32_t, which no scaling list of a stream gives. */
	narrow = transform_scale_narrow(scale, 16U);
	if (narrow == 0U) {
		transform_scale_wide(block, scale, 16U, qp, 24, 4U);
	} else if (qp >= 24) {
		/*
		 * Scales by a left shift (8-336), written as a multiplication.
		 * The level is within 2^15 and the scale within 2^16, so the
		 * product fits int32_t; a product beyond 2^16 is held to it,
		 * since the result is clamped to 2^15 whatever the factor.
		 */
		shift = qp_per - 4U;
		/* Scales the 16 coefficients, one at a time. */
		for (position = 0; position < 16U; position++) {
			level = transform_clamp32(block[position]);
			product = level * scale[position];
			product = transform_clamp_wide32(product);
			block[position] = transform_clamp32(product * (int32_t)(1U << shift));
		}
	} else {
		/* Scales by a rounding right shift (8-337) of 4 - qP / 6 bits; the product plus the rounding stays below 2^31. */
		shift = 4U - qp_per;
		add = (int32_t)(1U << (shift - 1U));
		/* Scales the 16 coefficients, one at a time. */
		for (position = 0; position < 16U; position++) {
			level = transform_clamp32(block[position]);
			product = level * scale[position];
			block[position] = transform_clamp32(transform_shift_right32(product + add, shift));
		}
	}

	/* Puts back the DC that an earlier process scaled. */
	if (dc_done != 0)
		block[0] = kept;
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
	int32_t d[4];
	int32_t h[4];
	int32_t dc;
	uint32_t others;
	unsigned i;
	unsigned j;
	unsigned k;

	/* Refuses a missing array. */
	if (block == NULL)
		return;

	/* Gathers the bits of the 15 coefficients that are not the DC. */
	others = 0;
	for (i = 1; i < 16U; i++)
		others = others | (uint32_t)block[i];

	/*
	 * A block of a DC alone gives the same sample everywhere: the
	 * row of the DC transforms to four copies of it, and each column
	 * then holds it above three zeros, which transform to four copies.
	 */
	if (others == 0U) {
		/* Rounds the DC, held to its range, to the residual (8-354). */
		dc = transform_shift_right32(transform_clamp32(block[0]) + 32, 6U);

		/* Gives all 16 samples that residual. */
		for (i = 0; i < 16U; i++)
			block[i] = dc;

		/* Succeeded: the block holds the residual. */
		return;
	}

	/* Transforms each row (8-338 to 8-345); a row of zeros transforms to zeros and stays. */
	for (i = 0; i < 4U; i++) {
		if ((block[4U * i] | block[4U * i + 1U] | block[4U * i + 2U] | block[4U * i + 3U]) != 0)
			transform_row4(&block[4U * i]);
	}

	/* Transforms the four columns at once, one lane for each column (8-346 to 8-353), and rounds each sample to the residual (8-354). */
	for (j = 0; j < 4U; j++) {
		/* Reads the column, with one coefficient from each row. */
		for (k = 0; k < 4U; k++)
			d[k] = block[4U * k + j];
		transform_core4(h, d);
		/* Stores the column of residual samples. */
		for (k = 0; k < 4U; k++)
			block[4U * k + j] = transform_shift_right32(h[k] + 32, 6U);
	}
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
	int32_t level;
	int32_t product;
	int32_t add;
	unsigned position;
	unsigned qp_per;
	unsigned narrow;
	unsigned shift;

	/* Refuses a missing array. */
	if (block == NULL)
		return;
	if (scale == NULL)
		return;

	/* Takes qP into the range the shift counts below are defined for. */
	qp = transform_clamp_qp(qp);
	qp_per = (unsigned)qp / 6U;

	/* Takes the product in int64_t when a scale is too large for int32_t, which no scaling list of a stream gives. */
	narrow = transform_scale_narrow(scale, 64U);
	if (narrow == 0U) {
		transform_scale_wide(block, scale, 64U, qp, 36, 6U);
	} else if (qp >= 36) {
		/* Scales by a left shift (8-356), written as a multiplication, with the product held as the 4x4 scaling holds it. */
		shift = qp_per - 6U;
		/* Scales the 64 coefficients, one at a time. */
		for (position = 0; position < 64U; position++) {
			level = transform_clamp32(block[position]);
			product = level * scale[position];
			product = transform_clamp_wide32(product);
			block[position] = transform_clamp32(product * (int32_t)(1U << shift));
		}
	} else {
		/* Scales by a rounding right shift (8-357) of 6 - qP / 6 bits. */
		shift = 6U - qp_per;
		add = (int32_t)(1U << (shift - 1U));
		/* Scales the 64 coefficients, one at a time. */
		for (position = 0; position < 64U; position++) {
			level = transform_clamp32(block[position]);
			product = level * scale[position];
			block[position] = transform_clamp32(transform_shift_right32(product + add, shift));
		}
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
	int32_t d[8];
	int32_t h[8];
	int32_t dc;
	int32_t any;
	uint32_t others;
	unsigned i;
	unsigned j;
	unsigned k;

	/* Refuses a missing array. */
	if (block == NULL)
		return;

	/* Gathers the bits of the 63 coefficients that are not the DC. */
	others = 0;
	for (i = 1; i < 64U; i++)
		others = others | (uint32_t)block[i];

	/* A block of a DC alone gives the same sample everywhere, as a 4x4 block does. */
	if (others == 0U) {
		/* Rounds the DC, held to its range, to the residual (8-406). */
		dc = transform_shift_right32(transform_clamp32(block[0]) + 32, 6U);

		/* Gives all 64 samples that residual. */
		for (i = 0; i < 64U; i++)
			block[i] = dc;

		/* Succeeded: the block holds the residual. */
		return;
	}

	/* Transforms each row (8-358 to 8-381); a row of zeros transforms to zeros and stays. */
	for (i = 0; i < 8U; i++) {
		any = 0;
		/* Gathers the bits of the row. */
		for (k = 0; k < 8U; k++)
			any = any | block[8U * i + k];
		if (any != 0)
			transform_row8(&block[8U * i]);
	}

	/* Transforms the eight columns at once, one lane for each column (8-382 to 8-405), and rounds each sample to the residual (8-406). */
	for (j = 0; j < 8U; j++) {
		/* Reads the column, with one coefficient from each row. */
		for (k = 0; k < 8U; k++)
			d[k] = block[8U * k + j];
		transform_core8(h, d);
		/* Stores the column of residual samples. */
		for (k = 0; k < 8U; k++)
			block[8U * k + j] = transform_shift_right32(h[k] + 32, 6U);
	}
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
 * is.  Each row is read before it is written, so out may be pred.
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
	unsigned row;

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

	/* Takes a 4x4 block by its own loop, so each row is a constant four samples wide. */
	if (size == 4U) {
		/* Builds each row of the block, four samples at once. */
		for (row = 0; row < 4U; row++)
			transform_add_row(&pred[row * pred_stride], &residual[row * 4U], &out[row * out_stride], 4U);
	} else {
		/* Builds each row of an 8x8 block, eight samples at once, in a loop of its own. */
		for (row = 0; row < 8U; row++)
			transform_add_row(&pred[row * pred_stride], &residual[row * 8U], &out[row * out_stride], 8U);
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
 * Holds a value to the range the clauses give to cij, dij, eij and the
 * like for 8 bits, as transform_clamp does for an int64_t.
 */
static __inline int32_t
transform_clamp32(
	int32_t value)
{
	int32_t held;

	/* Starts from the value, which stays when it is in the range. */
	held = value;

	/* Takes a value below the range up to its lowest. */
	if (held < TRANSFORM_VALUE_MIN)
		held = TRANSFORM_VALUE_MIN;

	/* Takes a value above the range down to its highest. */
	if (held > TRANSFORM_VALUE_MAX)
		held = TRANSFORM_VALUE_MAX;

	/* Succeeded: the value held to the range. */
	return held;
}

/*
 * Holds a product to -2^16 .. 2^16, the widest range that still gives the
 * clamped result when the product is multiplied by a factor of 1 or more
 * and then held to the range of transform_clamp32 (every factor is at
 * most 2^4, so the multiplication stays in int32_t).
 */
static __inline int32_t
transform_clamp_wide32(
	int32_t value)
{
	int32_t held;

	/* Starts from the value, which stays when it is in the range. */
	held = value;

	/* Takes a value below the range up to its lowest. */
	if (held < -TRANSFORM_PRODUCT_LIMIT)
		held = -TRANSFORM_PRODUCT_LIMIT;

	/* Takes a value above the range down to its highest. */
	if (held > TRANSFORM_PRODUCT_LIMIT)
		held = TRANSFORM_PRODUCT_LIMIT;

	/* Succeeded: the value held to the range. */
	return held;
}

/*
 * Tells whether all count scale factors are within -65535 .. 65535, so
 * that the product of a level (at most 2^15) and a factor fits int32_t.
 * A scaling list entry is at most 255 and normAdjust at most 58, so the
 * factors of a stream are far below the limit.
 */
static unsigned
transform_scale_narrow(
	const int32_t *scale,
	unsigned count)
{
	unsigned outside;
	unsigned i;

	/* Marks a factor outside the range: after the offset of 65535 an unsigned sum above 131070 is outside. */
	outside = 0;
	/* Marks a factor outside the range. */
	for (i = 0; i < count; i++)
		outside = outside | ((uint32_t)scale[i] + 65535U > 131070U);

	/* Reports that a factor is outside the range. */
	if (outside != 0U)
		return 0U;

	/* Succeeded: every factor is inside the range. */
	return 1U;
}

/*
 * Scales count levels in int64_t (8.5.12.1, 8.5.13.1): from the qP of
 * threshold on a left shift (written as a multiplication) of qP / 6 -
 * base + 2, below it a rounding right shift.  Serves a scale that is too
 * large for int32_t.
 */
static void
transform_scale_wide(
	int32_t *block,
	const int32_t *scale,
	unsigned count,
	int qp,
	int threshold,
	unsigned base)
{
	int64_t product;
	int64_t scaled;
	int32_t level;
	unsigned position;
	unsigned qp_per;

	/* Takes the shift counts from qP / 6. */
	qp_per = (unsigned)qp / 6U;

	/* Scales the coefficients, one at a time. */
	for (position = 0; position < count; position++) {
		/* Takes the level into the range the bitstream may hold. */
		level = transform_clamp((int64_t)block[position]);

		/* The product fits int64_t: 2^15 times a scale of at most 2^31 stays below 2^46. */
		product = (int64_t)level * (int64_t)scale[position];

		/* Scales by the shift of qP / 6: a left shift (a multiplication) from the threshold on, a rounding right shift below it. */
		if (qp >= threshold) {
			scaled = product * ((int64_t)1 << (qp_per - base));
		} else {
			scaled = transform_shift_right64(product + ((int64_t)1 << ((base - 1U) - qp_per)), base - qp_per);
		}

		/* Keeps the scaled coefficient within the range dij may have. */
		block[position] = transform_clamp(scaled);
	}
}
/*
 * Applies the one-dimensional 4x4 inverse transform to the four values
 * in[0] .. in[3], which are within the range of dij, and gives the four
 * outputs in out[0] .. out[3] (8-338 to 8-345 for a row, 8-346 to 8-353
 * for a column), holding each intermediate and output value to its range.
 */
static __inline void
transform_core4(
	int32_t *out,
	const int32_t *in)
{
	int32_t e0;
	int32_t e1;
	int32_t e2;
	int32_t e3;

	/* Computes the intermediate values (8-338 to 8-341). */
	e0 = transform_clamp32(in[0] + in[2]);
	e1 = transform_clamp32(in[0] - in[2]);
	e2 = transform_clamp32(transform_shift_right32(in[1], 1U) - in[3]);
	e3 = transform_clamp32(in[1] + transform_shift_right32(in[3], 1U));

	/* Computes the outputs (8-342 to 8-345). */
	out[0] = transform_clamp32(e0 + e3);
	out[1] = transform_clamp32(e1 + e2);
	out[2] = transform_clamp32(e1 - e2);
	out[3] = transform_clamp32(e0 - e3);
}

/* Applies the 4x4 inverse transform to the four values of a row in place, after holding them to the range of dij. */
static void
transform_row4(
	int32_t *row)
{
	int32_t in[4];
	int32_t out[4];
	unsigned i;

	/* Reads the four inputs, held to the range of dij. */
	for (i = 0; i < 4U; i++)
		in[i] = transform_clamp32(row[i]);

	/* Transforms them. */
	transform_core4(out, in);

	/* Stores the four outputs. */
	for (i = 0; i < 4U; i++)
		row[i] = out[i];
}

/*
 * Applies the one-dimensional 8x8 inverse transform to the eight values
 * in[0] .. in[7], which are within the range of dij, and gives the eight
 * outputs in out[0] .. out[7] (8-358 to 8-381 for a row, 8-382 to 8-405
 * for a column), holding each intermediate and output value to its range.
 */
static __inline void
transform_core8(
	int32_t *out,
	const int32_t *in)
{
	int32_t e[8];
	int32_t f[8];

	/* Computes the first set of intermediate values (8-358 to 8-365). */
	e[0] = transform_clamp32(in[0] + in[4]);
	e[1] = transform_clamp32(-in[3] + in[5] - in[7] - transform_shift_right32(in[7], 1U));
	e[2] = transform_clamp32(in[0] - in[4]);
	e[3] = transform_clamp32(in[1] + in[7] - in[3] - transform_shift_right32(in[3], 1U));
	e[4] = transform_clamp32(transform_shift_right32(in[2], 1U) - in[6]);
	e[5] = transform_clamp32(-in[1] + in[7] + in[5] + transform_shift_right32(in[5], 1U));
	e[6] = transform_clamp32(in[2] + transform_shift_right32(in[6], 1U));
	e[7] = transform_clamp32(in[3] + in[5] + in[1] + transform_shift_right32(in[1], 1U));

	/* Computes the second set of intermediate values (8-366 to 8-373). */
	f[0] = transform_clamp32(e[0] + e[6]);
	f[1] = transform_clamp32(e[1] + transform_shift_right32(e[7], 2U));
	f[2] = transform_clamp32(e[2] + e[4]);
	f[3] = transform_clamp32(e[3] + transform_shift_right32(e[5], 2U));
	f[4] = transform_clamp32(e[2] - e[4]);
	f[5] = transform_clamp32(transform_shift_right32(e[3], 2U) - e[5]);
	f[6] = transform_clamp32(e[0] - e[6]);
	f[7] = transform_clamp32(e[7] - transform_shift_right32(e[1], 2U));

	/* Computes the outputs (8-374 to 8-381). */
	out[0] = transform_clamp32(f[0] + f[7]);
	out[1] = transform_clamp32(f[2] + f[5]);
	out[2] = transform_clamp32(f[4] + f[3]);
	out[3] = transform_clamp32(f[6] + f[1]);
	out[4] = transform_clamp32(f[6] - f[1]);
	out[5] = transform_clamp32(f[4] - f[3]);
	out[6] = transform_clamp32(f[2] - f[5]);
	out[7] = transform_clamp32(f[0] - f[7]);
}

/* Applies the 8x8 inverse transform to the eight values of a row in place, after holding them to the range of dij. */
static void
transform_row8(
	int32_t *row)
{
	int32_t in[8];
	int32_t out[8];
	unsigned i;

	/* Reads the eight inputs, held to the range of dij. */
	for (i = 0; i < 8U; i++)
		in[i] = transform_clamp32(row[i]);

	/* Transforms them. */
	transform_core8(out, in);

	/* Stores the eight outputs. */
	for (i = 0; i < 8U; i++)
		row[i] = out[i];
}

/*
 * Adds one row of a residual block of width 4 or 8 to the predicted
 * samples and clips (8.5.14).  A residual beyond -255 .. 255 clips the
 * sample whatever it is added to, so it is held to that range first and
 * the sum stays in int32_t.  The row is read whole before it is written.
 */
static __inline void
transform_add_row(
	const uint8_t *pred,
	const int32_t *residual,
	uint8_t *out,
	unsigned width)
{
	int32_t sum[8];
	int32_t delta;
	unsigned column;

	/* Sums each predicted sample and its residual. */
	for (column = 0; column < width; column++) {
		delta = residual[column];
		if (delta < -255)
			delta = -255;
		if (delta > 255)
			delta = 255;
		sum[column] = (int32_t)pred[column] + delta;
	}

	/* Clips each sum to the 8-bit range 0..255 and stores it. */
	for (column = 0; column < width; column++) {
		if (sum[column] < 0)
			sum[column] = 0;
		if (sum[column] > 255)
			sum[column] = 255;
		out[column] = (uint8_t)sum[column];
	}
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
