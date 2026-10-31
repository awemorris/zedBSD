/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The macroblock types and the neighbours of the software H.264 decoder
 * (Tables 7-11 to 7-18, 6.4).
 */

#include "h264sw-mb.h"
#include "h264sw-slice.h"

#include <errno.h>
#include <stddef.h>

/*
 * The most the I_16x16 rows of Table 7-11 cover: mb_type 1..24 of an I
 * slice, 24 rows.  Rows are indexed by mb_type - 1.
 */
#define MB_I16X16_ROWS 24U

/*
 * The rows of Table 7-13 (P macroblock types 0..4) and Table 7-14 (B
 * macroblock types 0..22).
 */
#define MB_P_ROWS 5U
#define MB_B_ROWS 23U

/*
 * One row of Table 7-11 for an Intra_16x16 macroblock type: the
 * Intra16x16PredMode and the two coded block patterns that the mb_type
 * implies.  The table below holds one of these per mb_type 1..24.
 */
struct mb_i16x16_row {
	uint8_t mode;
	uint8_t cbp_chroma;
	uint8_t cbp_luma;
};

/*
 * One row of Table 7-13 or Table 7-14 for an inter macroblock type: the
 * decoder's kind, NumMbPart, MbPartWidth, MbPartHeight and the
 * MbPartPredMode of partitions 0 and 1.  The tables below hold one of
 * these per mb_type that the slice type's table names.
 */
struct mb_inter_row {
	uint8_t kind;
	uint8_t parts;
	uint8_t part_width;
	uint8_t part_height;
	uint8_t pred0;
	uint8_t pred1;
};

/*
 * One row of Table 7-17 or Table 7-18 for a sub-macroblock type:
 * NumSubMbPart, SubMbPartWidth, SubMbPartHeight and SubMbPredMode.
 */
struct mb_sub_row {
	uint8_t parts;
	uint8_t part_width;
	uint8_t part_height;
	uint8_t pred;
};

/*
 * The position of each luma4x4BlkIdx in its macroblock, in luma samples (6.4.3): x.
 *
 * Values referenced: ITU-T H.264 (08/2021) 6.4.3, equation 6-17 and
 * Figure 6-10 (the 8x8 quadrants in raster order, the 4x4 blocks in raster
 * order inside each).
 */
const uint8_t h264sw_blk4x4_x[16] = {
	0, 4, 0, 4,
	8, 12, 8, 12,
	0, 4, 0, 4,
	8, 12, 8, 12
};

/*
 * The position of each luma4x4BlkIdx in its macroblock, in luma samples (6.4.3): y.
 *
 * Values referenced: ITU-T H.264 (08/2021) 6.4.3, equation 6-18 and
 * Figure 6-10.
 */
const uint8_t h264sw_blk4x4_y[16] = {
	0, 0, 4, 4,
	0, 0, 4, 4,
	8, 8, 12, 12,
	8, 8, 12, 12
};

/*
 * The Intra_16x16 rows of the macroblock types of I slices, indexed by
 * mb_type - 1; read only, for the life of the program.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 7-11, mb_type 1..24
 * (columns Intra16x16PredMode, CodedBlockPatternChroma and
 * CodedBlockPatternLuma).
 */
static const struct mb_i16x16_row mb_i16x16_rows[MB_I16X16_ROWS] = {
	{ 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 }, { 3, 0, 0 },
	{ 0, 1, 0 }, { 1, 1, 0 }, { 2, 1, 0 }, { 3, 1, 0 },
	{ 0, 2, 0 }, { 1, 2, 0 }, { 2, 2, 0 }, { 3, 2, 0 },
	{ 0, 0, 15 }, { 1, 0, 15 }, { 2, 0, 15 }, { 3, 0, 15 },
	{ 0, 1, 15 }, { 1, 1, 15 }, { 2, 1, 15 }, { 3, 1, 15 },
	{ 0, 2, 15 }, { 1, 2, 15 }, { 2, 2, 15 }, { 3, 2, 15 }
};

/*
 * The macroblock types 0..4 of P slices; read only, for the life of the
 * program.  The partitions of P_8x8 and P_8x8ref0 are sub-macroblocks,
 * so their parts are 4 and their partition prediction modes are none.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 7-13, mb_type 0..4
 * (columns NumMbPart, MbPartPredMode( mb_type, 0 ), MbPartPredMode(
 * mb_type, 1 ), MbPartWidth and MbPartHeight).
 */
static const struct mb_inter_row mb_p_rows[MB_P_ROWS] = {
	{ H264SW_MB_P, 1, 16, 16, H264SW_PRED_L0, H264SW_PRED_NONE },
	{ H264SW_MB_P, 2, 16, 8, H264SW_PRED_L0, H264SW_PRED_L0 },
	{ H264SW_MB_P, 2, 8, 16, H264SW_PRED_L0, H264SW_PRED_L0 },
	{ H264SW_MB_P_8X8, 4, 8, 8, H264SW_PRED_NONE, H264SW_PRED_NONE },
	{ H264SW_MB_P_8X8REF0, 4, 8, 8, H264SW_PRED_NONE, H264SW_PRED_NONE }
};

/*
 * The macroblock types 0..22 of B slices; read only, for the life of the
 * program.  B_Direct_16x16 has no NumMbPart in the table; its motion is
 * derived per 8x8 quadrant (MbPartWidth and MbPartHeight 8), so it is
 * recorded with 4 parts, like the 8x8 kinds.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 7-14, mb_type 0..22
 * (columns NumMbPart, MbPartPredMode( mb_type, 0 ), MbPartPredMode(
 * mb_type, 1 ), MbPartWidth and MbPartHeight).
 */
static const struct mb_inter_row mb_b_rows[MB_B_ROWS] = {
	{ H264SW_MB_B_DIRECT, 4, 8, 8, H264SW_PRED_DIRECT, H264SW_PRED_NONE },
	{ H264SW_MB_B, 1, 16, 16, H264SW_PRED_L0, H264SW_PRED_NONE },
	{ H264SW_MB_B, 1, 16, 16, H264SW_PRED_L1, H264SW_PRED_NONE },
	{ H264SW_MB_B, 1, 16, 16, H264SW_PRED_BI, H264SW_PRED_NONE },
	{ H264SW_MB_B, 2, 16, 8, H264SW_PRED_L0, H264SW_PRED_L0 },
	{ H264SW_MB_B, 2, 8, 16, H264SW_PRED_L0, H264SW_PRED_L0 },
	{ H264SW_MB_B, 2, 16, 8, H264SW_PRED_L1, H264SW_PRED_L1 },
	{ H264SW_MB_B, 2, 8, 16, H264SW_PRED_L1, H264SW_PRED_L1 },
	{ H264SW_MB_B, 2, 16, 8, H264SW_PRED_L0, H264SW_PRED_L1 },
	{ H264SW_MB_B, 2, 8, 16, H264SW_PRED_L0, H264SW_PRED_L1 },
	{ H264SW_MB_B, 2, 16, 8, H264SW_PRED_L1, H264SW_PRED_L0 },
	{ H264SW_MB_B, 2, 8, 16, H264SW_PRED_L1, H264SW_PRED_L0 },
	{ H264SW_MB_B, 2, 16, 8, H264SW_PRED_L0, H264SW_PRED_BI },
	{ H264SW_MB_B, 2, 8, 16, H264SW_PRED_L0, H264SW_PRED_BI },
	{ H264SW_MB_B, 2, 16, 8, H264SW_PRED_L1, H264SW_PRED_BI },
	{ H264SW_MB_B, 2, 8, 16, H264SW_PRED_L1, H264SW_PRED_BI },
	{ H264SW_MB_B, 2, 16, 8, H264SW_PRED_BI, H264SW_PRED_L0 },
	{ H264SW_MB_B, 2, 8, 16, H264SW_PRED_BI, H264SW_PRED_L0 },
	{ H264SW_MB_B, 2, 16, 8, H264SW_PRED_BI, H264SW_PRED_L1 },
	{ H264SW_MB_B, 2, 8, 16, H264SW_PRED_BI, H264SW_PRED_L1 },
	{ H264SW_MB_B, 2, 16, 8, H264SW_PRED_BI, H264SW_PRED_BI },
	{ H264SW_MB_B, 2, 8, 16, H264SW_PRED_BI, H264SW_PRED_BI },
	{ H264SW_MB_B_8X8, 4, 8, 8, H264SW_PRED_NONE, H264SW_PRED_NONE }
};

/*
 * The sub-macroblock types 0..3 of P macroblocks; read only, for the
 * life of the program.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 7-17, sub_mb_type 0..3
 * (columns NumSubMbPart, SubMbPredMode, SubMbPartWidth and
 * SubMbPartHeight).
 */
static const struct mb_sub_row mb_sub_p_rows[4] = {
	{ 1, 8, 8, H264SW_PRED_L0 },
	{ 2, 8, 4, H264SW_PRED_L0 },
	{ 2, 4, 8, H264SW_PRED_L0 },
	{ 4, 4, 4, H264SW_PRED_L0 }
};

/*
 * The sub-macroblock types 0..12 of B macroblocks; read only, for the
 * life of the program.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 7-18, sub_mb_type 0..12
 * (columns NumSubMbPart, SubMbPredMode, SubMbPartWidth and
 * SubMbPartHeight).
 */
static const struct mb_sub_row mb_sub_b_rows[13] = {
	{ 4, 4, 4, H264SW_PRED_DIRECT },
	{ 1, 8, 8, H264SW_PRED_L0 },
	{ 1, 8, 8, H264SW_PRED_L1 },
	{ 1, 8, 8, H264SW_PRED_BI },
	{ 2, 8, 4, H264SW_PRED_L0 },
	{ 2, 4, 8, H264SW_PRED_L0 },
	{ 2, 8, 4, H264SW_PRED_L1 },
	{ 2, 4, 8, H264SW_PRED_L1 },
	{ 2, 8, 4, H264SW_PRED_BI },
	{ 2, 4, 8, H264SW_PRED_BI },
	{ 4, 4, 4, H264SW_PRED_L0 },
	{ 4, 4, 4, H264SW_PRED_L1 },
	{ 4, 4, 4, H264SW_PRED_BI }
};

/* Forward declarations of the static functions. */
static void mb_type_clear(struct h264sw_mb_type *type);
static void mb_type_from_row(struct h264sw_mb_type *type, const struct mb_inter_row *row);
static void mb_type_from_intra(struct h264sw_mb_type *type, unsigned intra_type);
static struct h264sw_mb *mb_same_slice(struct h264sw_mb *candidate, int32_t slice);

/*
 * Resets a macroblock's entry to the state before it is decoded.
 *
 * Every field takes the value that a macroblock which has written nothing
 * yet has: the Intra prediction modes are DC (2), not Vertical (0), and
 * no reference index is in use (-1), not index 0.
 */
void
h264sw_mb_reset(
	struct h264sw_mb *mb,
	int32_t slice)
{
	unsigned i;
	unsigned list;
	unsigned blk;

	/* Ignores a missing entry. */
	if (mb == NULL)
		return;

	/* Clears the scalar fields and records the slice the entry belongs to. */
	mb->slice = slice;
	mb->kind = H264SW_MB_I_NXN;
	mb->cbp = 0;
	mb->transform_8x8 = 0;
	mb->qp = 0;
	mb->chroma_pred_mode = 0;
	mb->direct = 0;
	mb->coded_flags = 0;
	mb->nonzero = 0;

	/* Sets the Intra prediction mode of every 4x4 block to DC, the mode a neighbour that is not Intra_4x4 or Intra_8x8 stands for. */
	for (i = 0; i < 16U; i++)
		mb->intra4x4_mode[i] = 2;

	/* Clears the TotalCoeff of every 4x4 block of the three colour components. */
	for (i = 0; i < 24U; i++)
		mb->total_coeff[i] = 0;

	/* Clears the absolute mvd of every 4x4 block of both lists. */
	for (list = 0; list < 2U; list++) {
		for (blk = 0; blk < 16U; blk++) {
			mb->mvd[list][blk][0] = 0;
			mb->mvd[list][blk][1] = 0;
		}
	}

	/* Clears the motion vectors of every 4x4 block of both lists. */
	for (list = 0; list < 2U; list++) {
		for (blk = 0; blk < 16U; blk++) {
			mb->motion.mv[list][blk][0] = 0;
			mb->motion.mv[list][blk][1] = 0;
		}
	}

	/* Marks every 8x8 quadrant of both lists as using no reference. */
	for (list = 0; list < 2U; list++) {
		for (i = 0; i < 4U; i++) {
			mb->motion.ref_idx[list][i] = -1;
			mb->motion.ref_id[list][i] = 0;
		}
	}

	/* Records that the macroblock is not intra coded until its type says so. */
	mb->motion.intra = 0;
}

/*
 * Gives the meaning of an mb_type value of a slice type.
 *
 * I slices take 0..25 (Table 7-11); P slices 0..4 (Table 7-13) and 5..30
 * (Table 7-11 at mb_type - 5); B slices 0..22 (Table 7-14) and 23..48
 * (Table 7-11 at mb_type - 23).  Any other value or slice type is EINVAL
 * and leaves *type alone.
 */
int
h264sw_mb_type_of(
	unsigned slice_type,
	unsigned mb_type,
	struct h264sw_mb_type *type)
{
	int error;

	/* Refuses a missing output. */
	if (type == NULL)
		return EINVAL;

	/* Looks the value up in the tables of its slice type; anything not found stays EINVAL. */
	error = EINVAL;
	switch (slice_type) {
	case H264SW_SLICE_I:
		/* An I slice has only the types of Table 7-11. */
		if (mb_type <= 25U) {
			mb_type_from_intra(type, mb_type);
			error = 0;
		}

		break;

	case H264SW_SLICE_P:
		/* The first five P values are Table 7-13; the intra types follow at an offset of 5. */
		if (mb_type < MB_P_ROWS) {
			mb_type_from_row(type, &mb_p_rows[mb_type]);
			error = 0;
		} else if (mb_type <= 30U) {
			mb_type_from_intra(type, mb_type - 5U);
			error = 0;
		}

		break;

	case H264SW_SLICE_B:
		/* The first 23 B values are Table 7-14; the intra types follow at an offset of 23. */
		if (mb_type < MB_B_ROWS) {
			mb_type_from_row(type, &mb_b_rows[mb_type]);
			error = 0;
		} else if (mb_type <= 48U) {
			mb_type_from_intra(type, mb_type - 23U);
			error = 0;
		}

		break;

	default:
		/* Other slice types have no macroblock types here. */
		break;
	}

	/* Reports a value outside its slice type's range. */
	if (error != 0)
		return error;

	/* Succeeded: *type holds the meaning. */
	return 0;
}

/*
 * Gives the meaning of P_Skip (slice_type P) or B_Skip (slice_type B), as inferred by 7.4.4.
 *
 * P_Skip is one 16x16 partition predicted from list 0 (Table 7-13).
 * B_Skip is direct prediction per 8x8 (Table 7-14), recorded with 4
 * parts like B_Direct_16x16.  For another slice type *type is cleared.
 */
void
h264sw_mb_skip_type(
	unsigned slice_type,
	struct h264sw_mb_type *type)
{
	/* Ignores a missing output. */
	if (type == NULL)
		return;

	/* Starts from a cleared description, which is what a slice type without skipped macroblocks gets. */
	mb_type_clear(type);

	/* Describes the skipped macroblock of the slice type. */
	if (slice_type == H264SW_SLICE_P) {
		type->kind = H264SW_MB_P_SKIP;
		type->parts = 1;
		type->part_width = 16;
		type->part_height = 16;
		type->pred[0] = H264SW_PRED_L0;
		type->pred[1] = H264SW_PRED_NONE;
	} else if (slice_type == H264SW_SLICE_B) {
		type->kind = H264SW_MB_B_SKIP;
		type->parts = 4;
		type->part_width = 8;
		type->part_height = 8;
		type->pred[0] = H264SW_PRED_DIRECT;
		type->pred[1] = H264SW_PRED_NONE;
	}
}

/*
 * Gives the meaning of a sub_mb_type value (Table 7-17 for P: 0..3, Table 7-18 for B: 0..12).
 *
 * Any other value or slice type is EINVAL and leaves *type alone.
 */
int
h264sw_sub_mb_type_of(
	unsigned slice_type,
	unsigned sub_mb_type,
	struct h264sw_sub_mb_type *type)
{
	const struct mb_sub_row *row;

	/* Refuses a missing output. */
	if (type == NULL)
		return EINVAL;

	/* Picks the row of the table that belongs to the slice type, or none. */
	row = NULL;
	if (slice_type == H264SW_SLICE_P) {
		if (sub_mb_type < 4U)
			row = &mb_sub_p_rows[sub_mb_type];
	} else if (slice_type == H264SW_SLICE_B) {
		if (sub_mb_type < 13U)
			row = &mb_sub_b_rows[sub_mb_type];
	}

	/* Refuses a value outside the range of its table. */
	if (row == NULL)
		return EINVAL;

	/* Copies the row into the caller's description. */
	type->parts = row->parts;
	type->part_width = row->part_width;
	type->part_height = row->part_height;
	type->pred = row->pred;

	/* Succeeded: *type holds the meaning. */
	return 0;
}

/*
 * Finds the neighbouring macroblocks of a macroblock (6.4.8, 6.4.9).
 *
 * A neighbour is available when it lies inside the picture and its slice
 * number equals the current macroblock's.  The macroblocks after the
 * current one in decoding order are never named: A, B, C and D have
 * smaller addresses, and an undecoded entry has slice -1.  When the
 * current entry itself has no slice (-1) no neighbour is available.
 */
void
h264sw_mb_neighbours(
	struct h264sw_mb *mbs,
	unsigned width_mbs,
	unsigned addr,
	struct h264sw_neighbours *n)
{
	int32_t slice;

	/* Ignores a missing output. */
	if (n == NULL)
		return;

	/* Starts with nothing available. */
	n->current = NULL;
	n->a = NULL;
	n->b = NULL;
	n->c = NULL;
	n->d = NULL;
	n->addr = addr;
	n->x = 0;
	n->y = 0;

	/* Leaves everything unavailable for a picture without macroblocks. */
	if (mbs == NULL)
		return;
	if (width_mbs == 0U)
		return;

	/* Places the current macroblock in the picture and takes its slice. */
	n->current = &mbs[addr];
	n->x = addr % width_mbs;
	n->y = addr / width_mbs;
	slice = n->current->slice;

	/* A macroblock that belongs to no slice has no neighbour in any slice. */
	if (slice < 0)
		return;

	/* Finds A, the macroblock to the left, which exists when the column is not the first. */
	if (n->x > 0U)
		n->a = mb_same_slice(&mbs[addr - 1U], slice);

	/* Finds B, the macroblock above, which exists when the row is not the first. */
	if (n->y > 0U)
		n->b = mb_same_slice(&mbs[addr - width_mbs], slice);

	/* Finds C, the macroblock above and to the right, which needs a next column and a row above. */
	if (n->y > 0U && n->x < width_mbs - 1U)
		n->c = mb_same_slice(&mbs[addr - width_mbs + 1U], slice);

	/* Finds D, the macroblock above and to the left, which needs a previous column and a row above. */
	if (n->y > 0U && n->x > 0U)
		n->d = mb_same_slice(&mbs[addr - width_mbs - 1U], slice);
}

/*
 * Finds the macroblock that holds a location near the current one (6.4.12.1, Table 6-3).
 *
 * The location is the one relative to the upper-left sample of the
 * current macroblock; *xw and *yw are 0 when the result is NULL.  A
 * component size of 0 or a missing input gives NULL.
 */
struct h264sw_mb *
h264sw_mb_locate(
	const struct h264sw_neighbours *n,
	int xn,
	int yn,
	unsigned max_w,
	unsigned max_h,
	unsigned *xw,
	unsigned *yw)
{
	struct h264sw_mb *found;
	int64_t width;
	int64_t height;
	int64_t x_inside;
	int64_t y_inside;

	/* Clears the outputs, which stay 0 when no macroblock is found. */
	if (xw != NULL)
		*xw = 0;
	if (yw != NULL)
		*yw = 0;

	/* Refuses a missing neighbour set or a component of no size. */
	if (n == NULL)
		return NULL;
	if (max_w == 0U || max_h == 0U)
		return NULL;

	/* Widens the sizes and the location so that the sums below cannot overflow. */
	width = (int64_t)max_w;
	height = (int64_t)max_h;

	/*
	 * Chooses mbAddrN by Table 6-3: below the macroblock is never
	 * available; to the left the row decides between D and A; inside
	 * the column range between B and the current macroblock; to the right
	 * only the row above (C) is available.
	 */
	found = NULL;
	if ((int64_t)yn > height - 1) {
		/* A location below the macroblock is not available. */
		found = NULL;
	} else if (xn < 0) {
		/* A location to the left is D above the macroblock and A beside it. */
		if (yn < 0)
			found = n->d;
		else
			found = n->a;
	} else if ((int64_t)xn <= width - 1) {
		/* A location in the column range is B above the macroblock and the macroblock itself inside it. */
		if (yn < 0)
			found = n->b;
		else
			found = n->current;
	} else {
		/* A location to the right is C above the macroblock and not available beside it. */
		if (yn < 0)
			found = n->c;
		else
			found = NULL;
	}

	/* Reports that Table 6-3 or the availability rule gives no macroblock. */
	if (found == NULL)
		return NULL;

	/* Expresses the location relative to the macroblock found (equations 6-34 and 6-35), as a value in 0..max - 1 for any input. */
	x_inside = ((((int64_t)xn + width) % width) + width) % width;
	y_inside = ((((int64_t)yn + height) % height) + height) % height;
	if (xw != NULL)
		*xw = (unsigned)x_inside;
	if (yw != NULL)
		*yw = (unsigned)y_inside;

	/* Succeeded: the macroblock that holds the location. */
	return found;
}

/*
 * Gives the luma4x4BlkIdx of the 4x4 block holding luma location (x, y) of a macroblock, 0..15 each (6.4.13.1).
 *
 * A coordinate above 15 is reduced modulo 16, so the result is always a
 * block index of the macroblock.
 */
unsigned
h264sw_blk4x4_at(
	unsigned x,
	unsigned y)
{
	unsigned xp;
	unsigned yp;

	/* Keeps the location inside the macroblock. */
	xp = x % 16U;
	yp = y % 16U;

	/* Succeeded: the index of equation 6-38. */
	return 8U * (yp / 8U) + 4U * (xp / 8U) + 2U * ((yp % 8U) / 4U) + ((xp % 8U) / 4U);
}

/*
 * Finds the left (A) or upper (B) neighbour of a luma 4x4 block (6.4.11.4).
 *
 * A block index of 16 or more gives NULL; *blk_n is 0 when the result is
 * NULL.
 */
struct h264sw_mb *
h264sw_mb_neighbour_4x4(
	const struct h264sw_neighbours *n,
	unsigned blk,
	int upper,
	unsigned *blk_n)
{
	struct h264sw_mb *found;
	unsigned xw;
	unsigned yw;
	int xn;
	int yn;

	/* Clears the output, which stays 0 when no neighbour is found. */
	if (blk_n != NULL)
		*blk_n = 0;

	/* Refuses a block index that no macroblock has. */
	if (blk >= 16U)
		return NULL;

	/* Forms the location of the neighbour: one sample left (A) or above (B) the block's upper-left sample (equations 6-25 and 6-26, Table 6-2). */
	xn = (int)h264sw_blk4x4_x[blk];
	yn = (int)h264sw_blk4x4_y[blk];
	if (upper != 0)
		yn = yn - 1;
	else
		xn = xn - 1;

	/* Finds the macroblock that holds the location. */
	found = h264sw_mb_locate(n, xn, yn, 16U, 16U, &xw, &yw);
	if (found == NULL)
		return NULL;

	/* Names the 4x4 block of that macroblock that holds the location (6.4.13.1). */
	if (blk_n != NULL)
		*blk_n = h264sw_blk4x4_at(xw, yw);

	/* Succeeded: the macroblock that holds the neighbouring block. */
	return found;
}

/*
 * As h264sw_mb_neighbour_4x4 for a luma 8x8 block (6.4.11.2): blk is luma8x8BlkIdx, *blk_n the neighbour's.
 *
 * A block index of 4 or more gives NULL; *blk_n is 0 when the result is
 * NULL.
 */
struct h264sw_mb *
h264sw_mb_neighbour_8x8(
	const struct h264sw_neighbours *n,
	unsigned blk,
	int upper,
	unsigned *blk_n)
{
	struct h264sw_mb *found;
	unsigned xw;
	unsigned yw;
	int xn;
	int yn;

	/* Clears the output, which stays 0 when no neighbour is found. */
	if (blk_n != NULL)
		*blk_n = 0;

	/* Refuses a block index that no macroblock has. */
	if (blk >= 4U)
		return NULL;

	/* Forms the location of the neighbour: one sample left (A) or above (B) the block's upper-left sample (equations 6-23 and 6-24, Table 6-2). */
	xn = (int)(blk % 2U) * 8;
	yn = (int)(blk / 2U) * 8;
	if (upper != 0)
		yn = yn - 1;
	else
		xn = xn - 1;

	/* Finds the macroblock that holds the location. */
	found = h264sw_mb_locate(n, xn, yn, 16U, 16U, &xw, &yw);
	if (found == NULL)
		return NULL;

	/* Names the 8x8 block of that macroblock that holds the location (6.4.13.3, equation 6-40). */
	if (blk_n != NULL)
		*blk_n = 2U * (yw / 8U) + (xw / 8U);

	/* Succeeded: the macroblock that holds the neighbouring block. */
	return found;
}

/*
 * As h264sw_mb_neighbour_4x4 for a chroma 4x4 block of 4:2:0 (6.4.11.5): blk is chroma4x4BlkIdx 0..3.
 *
 * A block index of 4 or more gives NULL; *blk_n is 0 when the result is
 * NULL.
 */
struct h264sw_mb *
h264sw_mb_neighbour_chroma(
	const struct h264sw_neighbours *n,
	unsigned blk,
	int upper,
	unsigned *blk_n)
{
	struct h264sw_mb *found;
	unsigned xw;
	unsigned yw;
	int xn;
	int yn;

	/* Clears the output, which stays 0 when no neighbour is found. */
	if (blk_n != NULL)
		*blk_n = 0;

	/* Refuses a block index that no macroblock has. */
	if (blk >= 4U)
		return NULL;

	/* Forms the location of the neighbour: one sample left (A) or above (B) the block's upper-left chroma sample (6.4.7, equations 6-27 and 6-28, Table 6-2). */
	xn = (int)(blk % 2U) * 4;
	yn = (int)(blk / 2U) * 4;
	if (upper != 0)
		yn = yn - 1;
	else
		xn = xn - 1;

	/* Finds the macroblock that holds the location, in a chroma block of 8x8 samples (4:2:0). */
	found = h264sw_mb_locate(n, xn, yn, 8U, 8U, &xw, &yw);
	if (found == NULL)
		return NULL;

	/* Names the 4x4 chroma block of that macroblock that holds the location (6.4.13.2, equation 6-39). */
	if (blk_n != NULL)
		*blk_n = 2U * (yw / 4U) + (xw / 4U);

	/* Succeeded: the macroblock that holds the neighbouring block. */
	return found;
}

/* Clears a macroblock type description to kind 0 with no partitions and no prediction. */
static void
mb_type_clear(
	struct h264sw_mb_type *type)
{
	/* Zeroes every field. */
	type->kind = H264SW_MB_I_NXN;
	type->parts = 0;
	type->part_width = 0;
	type->part_height = 0;
	type->pred[0] = H264SW_PRED_NONE;
	type->pred[1] = H264SW_PRED_NONE;
	type->intra16x16_mode = 0;
	type->cbp_luma = 0;
	type->cbp_chroma = 0;
}

/* Fills a macroblock type description from a row of Table 7-13 or Table 7-14. */
static void
mb_type_from_row(
	struct h264sw_mb_type *type,
	const struct mb_inter_row *row)
{
	/* Starts from a cleared description so the intra-only fields are 0. */
	mb_type_clear(type);

	/* Copies the partitioning and prediction of the row. */
	type->kind = row->kind;
	type->parts = row->parts;
	type->part_width = row->part_width;
	type->part_height = row->part_height;
	type->pred[0] = row->pred0;
	type->pred[1] = row->pred1;
}

/* Fills a macroblock type description for mb_type 0..25 of Table 7-11 (intra_type is the value there). */
static void
mb_type_from_intra(
	struct h264sw_mb_type *type,
	unsigned intra_type)
{
	const struct mb_i16x16_row *row;

	/* Starts from a cleared description: an intra type has no partitions and no inter prediction. */
	mb_type_clear(type);

	/* Describes I_NxN, whose transform size and coded block patterns come later in the syntax. */
	if (intra_type == 0U) {
		type->kind = H264SW_MB_I_NXN;
		return;
	}

	/* Describes I_PCM, whose samples are in the bitstream and which has no prediction mode. */
	if (intra_type == 25U) {
		type->kind = H264SW_MB_I_PCM;
		return;
	}

	/* Describes an Intra_16x16 type from its row of Table 7-11. */
	row = &mb_i16x16_rows[intra_type - 1U];
	type->kind = H264SW_MB_I_16X16;
	type->intra16x16_mode = row->mode;
	type->cbp_luma = row->cbp_luma;
	type->cbp_chroma = row->cbp_chroma;
}

/* Gives the candidate macroblock when it belongs to the slice, NULL otherwise (6.4.8). */
static struct h264sw_mb *
mb_same_slice(
	struct h264sw_mb *candidate,
	int32_t slice)
{
	/* A macroblock of another slice, or not decoded yet, is not available. */
	if (candidate->slice != slice)
		return NULL;

	/* Succeeded: the candidate is available. */
	return candidate;
}
