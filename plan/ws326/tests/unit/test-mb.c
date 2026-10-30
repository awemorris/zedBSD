/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the macroblock types (Tables 7-11, 7-13, 7-14, 7-17, 7-18), the
 * block scans of 6.4.3 and 6.4.13, and the neighbours of macroblocks and
 * blocks (6.4.8 to 6.4.12.1, Table 6-3) of the software H.264 decoder.
 * The expected values are the rows of the tables as printed, shapes that
 * the table names imply, and neighbours worked out by hand from the
 * block layout of Figure 6-10.
 */

#include <errno.h>
#include <limits.h>
#include <string.h>

#include "check.h"
#include "userland/desktop/libmedia/h264sw-mb.h"
#include "userland/desktop/libmedia/h264sw-slice.h"

/* The picture of the neighbour tests: 4 by 3 macroblocks. */
#define PIC_W 4U
#define PIC_H 3U

/*
 * What one entry of an expected neighbour table says: the neighbour is in
 * the current macroblock (1), or in the macroblock A or B beside it (0),
 * and its block index there.
 */
struct expect_block {
	unsigned in_current;
	unsigned blk;
};

/* Forward declarations of the static functions. */
static void check_reset(void);
static void check_intra_types(void);
static void check_p_types(void);
static void check_b_types(void);
static void check_sub_types(void);
static void check_type_errors(void);
static void check_block_scan(void);
static void check_neighbours(void);
static void check_locate(void);
static void check_block_neighbours(void);
static void check_neighbour_errors(void);
static void make_picture(struct h264sw_mb *mbs);
static void check_type(const struct h264sw_mb_type *type, unsigned kind, unsigned parts, unsigned width, unsigned height, unsigned pred0, unsigned pred1);
static void check_sub(const struct h264sw_sub_mb_type *type, unsigned parts, unsigned width, unsigned height, unsigned pred);
static void check_expected(const struct h264sw_neighbours *n, struct h264sw_mb *(*finder)(const struct h264sw_neighbours *, unsigned, int, unsigned *), unsigned count, const struct expect_block *a, const struct expect_block *b);

/*
 * The neighbours of the 16 luma 4x4 blocks, worked out from Figure 6-10
 * (the blocks are numbered 0 1 4 5 / 2 3 6 7 / 8 9 12 13 / 10 11 14 15
 * by row): the block to the left (A) and the block above (B), as
 * block 0 has A in the left macroblock's block 5 and B in the upper
 * macroblock's block 10.
 */
static const struct expect_block luma4x4_a[16] = {
	{ 0, 5 }, { 1, 0 }, { 0, 7 }, { 1, 2 },
	{ 1, 1 }, { 1, 4 }, { 1, 3 }, { 1, 6 },
	{ 0, 13 }, { 1, 8 }, { 0, 15 }, { 1, 10 },
	{ 1, 9 }, { 1, 12 }, { 1, 11 }, { 1, 14 }
};
static const struct expect_block luma4x4_b[16] = {
	{ 0, 10 }, { 0, 11 }, { 1, 0 }, { 1, 1 },
	{ 0, 14 }, { 0, 15 }, { 1, 4 }, { 1, 5 },
	{ 1, 2 }, { 1, 3 }, { 1, 8 }, { 1, 9 },
	{ 1, 6 }, { 1, 7 }, { 1, 12 }, { 1, 13 }
};

/* The neighbours of the four luma 8x8 blocks, worked out from the 2 by 2 raster order. */
static const struct expect_block luma8x8_a[4] = {
	{ 0, 1 }, { 1, 0 }, { 0, 3 }, { 1, 2 }
};
static const struct expect_block luma8x8_b[4] = {
	{ 0, 2 }, { 0, 3 }, { 1, 0 }, { 1, 1 }
};

/* The neighbours of the four chroma 4x4 blocks of 4:2:0, in the same 2 by 2 raster order. */
static const struct expect_block chroma_a[4] = {
	{ 0, 1 }, { 1, 0 }, { 0, 3 }, { 1, 2 }
};
static const struct expect_block chroma_b[4] = {
	{ 0, 2 }, { 0, 3 }, { 1, 0 }, { 1, 1 }
};

/* Runs the checks. */
int
main(
	void)
{
	int status;

	/* Runs each group of checks. */
	check_reset();
	check_intra_types();
	check_p_types();
	check_b_types();
	check_sub_types();
	check_type_errors();
	check_block_scan();
	check_neighbours();
	check_locate();
	check_block_neighbours();
	check_neighbour_errors();

	/* Prints the summary line and takes the exit status from it. */
	status = check_finish("mb");
	if (status != 0)
		return status;

	/* Succeeded: every check passed. */
	return 0;
}

/* Builds a picture of PIC_W by PIC_H macroblocks, all decoded in slice 0. */
static void
make_picture(
	struct h264sw_mb *mbs)
{
	unsigned i;

	/* Resets every entry and puts it in slice 0. */
	for (i = 0; i < PIC_W * PIC_H; i++)
		h264sw_mb_reset(&mbs[i], 0);
}

/* Checks the fields of a macroblock type that every row of Tables 7-13 and 7-14 has. */
static void
check_type(
	const struct h264sw_mb_type *type,
	unsigned kind,
	unsigned parts,
	unsigned width,
	unsigned height,
	unsigned pred0,
	unsigned pred1)
{
	/* Compares each field with the row's value. */
	CHECK(type->kind == kind);
	CHECK(type->parts == parts);
	CHECK(type->part_width == width);
	CHECK(type->part_height == height);
	CHECK(type->pred[0] == pred0);
	CHECK(type->pred[1] == pred1);
}

/* Checks the fields of a sub-macroblock type. */
static void
check_sub(
	const struct h264sw_sub_mb_type *type,
	unsigned parts,
	unsigned width,
	unsigned height,
	unsigned pred)
{
	/* Compares each field with the row's value. */
	CHECK(type->parts == parts);
	CHECK(type->part_width == width);
	CHECK(type->part_height == height);
	CHECK(type->pred == pred);
}

/* Checks that a reset entry holds the state before decoding, in particular DC modes and unused references. */
static void
check_reset(void)
{
	struct h264sw_mb mb;
	unsigned i;
	unsigned list;

	/* Fills the entry with a value that no field of a reset entry has, then resets it into slice 7. */
	memset(&mb, 0xff, sizeof(mb));
	h264sw_mb_reset(&mb, 7);

	/* Checks the scalar fields. */
	CHECK(mb.slice == 7);
	CHECK(mb.kind == H264SW_MB_I_NXN);
	CHECK(mb.cbp == 0);
	CHECK(mb.transform_8x8 == 0);
	CHECK(mb.qp == 0);
	CHECK(mb.chroma_pred_mode == 0);
	CHECK(mb.direct == 0);
	CHECK(mb.coded_flags == 0U);
	CHECK(mb.nonzero == 0);
	CHECK(mb.motion.intra == 0);

	/* Every 4x4 block's Intra prediction mode is 2 (DC). */
	for (i = 0; i < 16U; i++)
		CHECK(mb.intra4x4_mode[i] == 2);

	/* Every TotalCoeff is 0. */
	for (i = 0; i < 24U; i++)
		CHECK(mb.total_coeff[i] == 0);

	/* Every mvd and motion vector is 0. */
	for (list = 0; list < 2U; list++) {
		for (i = 0; i < 16U; i++) {
			CHECK(mb.mvd[list][i][0] == 0);
			CHECK(mb.mvd[list][i][1] == 0);
			CHECK(mb.motion.mv[list][i][0] == 0);
			CHECK(mb.motion.mv[list][i][1] == 0);
		}
	}

	/* Every reference index is -1 (the list is not used) and every reference id 0. */
	for (list = 0; list < 2U; list++) {
		for (i = 0; i < 4U; i++) {
			CHECK(mb.motion.ref_idx[list][i] == -1);
			CHECK(mb.motion.ref_id[list][i] == 0U);
		}
	}

	/* A reset with slice -1 marks the entry as not decoded. */
	h264sw_mb_reset(&mb, -1);
	CHECK(mb.slice == -1);

	/* A missing entry is ignored. */
	h264sw_mb_reset(NULL, 0);
}

/* Checks Table 7-11 through the I slice, the P slice at an offset of 5 and the B slice at an offset of 23. */
static void
check_intra_types(void)
{
	struct h264sw_mb_type type;
	struct h264sw_mb_type shifted;
	unsigned t;
	unsigned slice_type;
	unsigned expected_luma;
	unsigned offset;
	int error;
	int difference;

	/* I_NxN: mb_type 0 has no partitions and its coded block pattern comes later. */
	error = h264sw_mb_type_of(H264SW_SLICE_I, 0, &type);
	CHECK(error == 0);
	CHECK(type.kind == H264SW_MB_I_NXN);
	CHECK(type.parts == 0);
	CHECK(type.pred[0] == H264SW_PRED_NONE);
	CHECK(type.pred[1] == H264SW_PRED_NONE);

	/* I_PCM: mb_type 25. */
	error = h264sw_mb_type_of(H264SW_SLICE_I, 25, &type);
	CHECK(error == 0);
	CHECK(type.kind == H264SW_MB_I_PCM);
	CHECK(type.parts == 0);

	/* Every I_16x16 row: the mode cycles through 0..3, the chroma pattern through 0..2 every four rows, and the luma pattern is 15 from mb_type 13. */
	for (t = 1; t <= 24U; t++) {
		error = h264sw_mb_type_of(H264SW_SLICE_I, t, &type);
		CHECK(error == 0);
		CHECK(type.kind == H264SW_MB_I_16X16);
		CHECK(type.intra16x16_mode == (t - 1U) % 4U);
		CHECK(type.cbp_chroma == ((t - 1U) / 4U) % 3U);
		expected_luma = 0U;
		if (t >= 13U)
			expected_luma = 15U;
		CHECK(type.cbp_luma == expected_luma);
		CHECK(type.parts == 0);
	}

	/* Rows of Table 7-11 as printed: 1 is I_16x16_0_0_0, 6 is I_16x16_1_1_0, 12 is I_16x16_3_2_0, 13 is I_16x16_0_0_1, 24 is I_16x16_3_2_1. */
	error = h264sw_mb_type_of(H264SW_SLICE_I, 1, &type);
	CHECK(error == 0 && type.intra16x16_mode == 0 && type.cbp_chroma == 0 && type.cbp_luma == 0);
	error = h264sw_mb_type_of(H264SW_SLICE_I, 6, &type);
	CHECK(error == 0 && type.intra16x16_mode == 1 && type.cbp_chroma == 1 && type.cbp_luma == 0);
	error = h264sw_mb_type_of(H264SW_SLICE_I, 12, &type);
	CHECK(error == 0 && type.intra16x16_mode == 3 && type.cbp_chroma == 2 && type.cbp_luma == 0);
	error = h264sw_mb_type_of(H264SW_SLICE_I, 13, &type);
	CHECK(error == 0 && type.intra16x16_mode == 0 && type.cbp_chroma == 0 && type.cbp_luma == 15);
	error = h264sw_mb_type_of(H264SW_SLICE_I, 24, &type);
	CHECK(error == 0 && type.intra16x16_mode == 3 && type.cbp_chroma == 2 && type.cbp_luma == 15);

	/* The intra types of P and B slices are the same rows at mb_type - 5 and mb_type - 23. */
	for (slice_type = H264SW_SLICE_P; slice_type <= H264SW_SLICE_B; slice_type++) {
		offset = 5U;
		if (slice_type == H264SW_SLICE_B)
			offset = 23U;
		for (t = 0; t <= 25U; t++) {
			error = h264sw_mb_type_of(H264SW_SLICE_I, t, &type);
			CHECK(error == 0);
			error = h264sw_mb_type_of(slice_type, t + offset, &shifted);
			CHECK(error == 0);
			difference = memcmp(&type, &shifted, sizeof(type));
			CHECK(difference == 0);
		}
	}
}

/* Checks Table 7-13 and P_Skip. */
static void
check_p_types(void)
{
	struct h264sw_mb_type type;
	int error;

	/* Row 0, P_L0_16x16. */
	error = h264sw_mb_type_of(H264SW_SLICE_P, 0, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_P, 1, 16, 16, H264SW_PRED_L0, H264SW_PRED_NONE);

	/* Row 1, P_L0_L0_16x8. */
	error = h264sw_mb_type_of(H264SW_SLICE_P, 1, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_P, 2, 16, 8, H264SW_PRED_L0, H264SW_PRED_L0);

	/* Row 2, P_L0_L0_8x16. */
	error = h264sw_mb_type_of(H264SW_SLICE_P, 2, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_P, 2, 8, 16, H264SW_PRED_L0, H264SW_PRED_L0);

	/* Row 3, P_8x8: four sub-macroblocks. */
	error = h264sw_mb_type_of(H264SW_SLICE_P, 3, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_P_8X8, 4, 8, 8, H264SW_PRED_NONE, H264SW_PRED_NONE);

	/* Row 4, P_8x8ref0. */
	error = h264sw_mb_type_of(H264SW_SLICE_P, 4, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_P_8X8REF0, 4, 8, 8, H264SW_PRED_NONE, H264SW_PRED_NONE);

	/* The inferred row, P_Skip: one 16x16 partition from list 0. */
	memset(&type, 0xff, sizeof(type));
	h264sw_mb_skip_type(H264SW_SLICE_P, &type);
	check_type(&type, H264SW_MB_P_SKIP, 1, 16, 16, H264SW_PRED_L0, H264SW_PRED_NONE);
	CHECK(type.intra16x16_mode == 0);
	CHECK(type.cbp_luma == 0);
	CHECK(type.cbp_chroma == 0);
}

/* Checks Table 7-14 and B_Skip. */
static void
check_b_types(void)
{
	struct h264sw_mb_type type;
	unsigned t;
	unsigned k;
	unsigned shape_wide;
	unsigned pair;
	int error;

	/*
	 * The first and second partition's prediction of mb_type 4..21 in the
	 * order of the names in Table 7-14: L0_L0, L1_L1, L0_L1, L1_L0,
	 * L0_Bi, L1_Bi, Bi_L0, Bi_L1, Bi_Bi, each as 16x8 then 8x16.
	 */
	static const unsigned first[9] = {
		H264SW_PRED_L0, H264SW_PRED_L1, H264SW_PRED_L0, H264SW_PRED_L1,
		H264SW_PRED_L0, H264SW_PRED_L1, H264SW_PRED_BI, H264SW_PRED_BI,
		H264SW_PRED_BI
	};
	static const unsigned second[9] = {
		H264SW_PRED_L0, H264SW_PRED_L1, H264SW_PRED_L1, H264SW_PRED_L0,
		H264SW_PRED_BI, H264SW_PRED_BI, H264SW_PRED_L0, H264SW_PRED_L1,
		H264SW_PRED_BI
	};

	/* Row 0, B_Direct_16x16: direct, with 8x8 as MbPartWidth and MbPartHeight. */
	error = h264sw_mb_type_of(H264SW_SLICE_B, 0, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_B_DIRECT, 4, 8, 8, H264SW_PRED_DIRECT, H264SW_PRED_NONE);

	/* Rows 1 to 3, B_L0_16x16, B_L1_16x16 and B_Bi_16x16. */
	error = h264sw_mb_type_of(H264SW_SLICE_B, 1, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_B, 1, 16, 16, H264SW_PRED_L0, H264SW_PRED_NONE);
	error = h264sw_mb_type_of(H264SW_SLICE_B, 2, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_B, 1, 16, 16, H264SW_PRED_L1, H264SW_PRED_NONE);
	error = h264sw_mb_type_of(H264SW_SLICE_B, 3, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_B, 1, 16, 16, H264SW_PRED_BI, H264SW_PRED_NONE);

	/* Rows 4 to 21: two partitions, 16x8 for even mb_type and 8x16 for odd, with the prediction pair of the name. */
	for (t = 4; t <= 21U; t++) {
		k = t - 4U;
		pair = k / 2U;
		shape_wide = 1;
		if (k % 2U == 1U)
			shape_wide = 0;
		error = h264sw_mb_type_of(H264SW_SLICE_B, t, &type);
		CHECK(error == 0);
		if (shape_wide != 0U)
			check_type(&type, H264SW_MB_B, 2, 16, 8, first[pair], second[pair]);
		else
			check_type(&type, H264SW_MB_B, 2, 8, 16, first[pair], second[pair]);
	}

	/* Row 22, B_8x8. */
	error = h264sw_mb_type_of(H264SW_SLICE_B, 22, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_B_8X8, 4, 8, 8, H264SW_PRED_NONE, H264SW_PRED_NONE);

	/* Spot rows as printed: 8 is B_L0_L1_16x8, 11 is B_L1_L0_8x16, 20 is B_Bi_Bi_16x8. */
	error = h264sw_mb_type_of(H264SW_SLICE_B, 8, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_B, 2, 16, 8, H264SW_PRED_L0, H264SW_PRED_L1);
	error = h264sw_mb_type_of(H264SW_SLICE_B, 11, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_B, 2, 8, 16, H264SW_PRED_L1, H264SW_PRED_L0);
	error = h264sw_mb_type_of(H264SW_SLICE_B, 20, &type);
	CHECK(error == 0);
	check_type(&type, H264SW_MB_B, 2, 16, 8, H264SW_PRED_BI, H264SW_PRED_BI);

	/* The inferred row, B_Skip: direct. */
	memset(&type, 0xff, sizeof(type));
	h264sw_mb_skip_type(H264SW_SLICE_B, &type);
	check_type(&type, H264SW_MB_B_SKIP, 4, 8, 8, H264SW_PRED_DIRECT, H264SW_PRED_NONE);
	CHECK(type.intra16x16_mode == 0);
	CHECK(type.cbp_luma == 0);
	CHECK(type.cbp_chroma == 0);
}

/* Checks Tables 7-17 and 7-18. */
static void
check_sub_types(void)
{
	struct h264sw_sub_mb_type type;
	unsigned t;
	int error;

	/* Table 7-17: P_L0_8x8, P_L0_8x4, P_L0_4x8, P_L0_4x4. */
	error = h264sw_sub_mb_type_of(H264SW_SLICE_P, 0, &type);
	CHECK(error == 0);
	check_sub(&type, 1, 8, 8, H264SW_PRED_L0);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_P, 1, &type);
	CHECK(error == 0);
	check_sub(&type, 2, 8, 4, H264SW_PRED_L0);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_P, 2, &type);
	CHECK(error == 0);
	check_sub(&type, 2, 4, 8, H264SW_PRED_L0);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_P, 3, &type);
	CHECK(error == 0);
	check_sub(&type, 4, 4, 4, H264SW_PRED_L0);

	/* Table 7-18, row 0: B_Direct_8x8 has four 4x4 parts in direct mode. */
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 0, &type);
	CHECK(error == 0);
	check_sub(&type, 4, 4, 4, H264SW_PRED_DIRECT);

	/* Table 7-18, rows 1 to 3: B_L0_8x8, B_L1_8x8, B_Bi_8x8. */
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 1, &type);
	CHECK(error == 0);
	check_sub(&type, 1, 8, 8, H264SW_PRED_L0);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 2, &type);
	CHECK(error == 0);
	check_sub(&type, 1, 8, 8, H264SW_PRED_L1);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 3, &type);
	CHECK(error == 0);
	check_sub(&type, 1, 8, 8, H264SW_PRED_BI);

	/* Table 7-18, rows 4 to 9: the 8x4 and 4x8 shapes with each prediction, in the order of the names. */
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 4, &type);
	CHECK(error == 0);
	check_sub(&type, 2, 8, 4, H264SW_PRED_L0);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 5, &type);
	CHECK(error == 0);
	check_sub(&type, 2, 4, 8, H264SW_PRED_L0);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 6, &type);
	CHECK(error == 0);
	check_sub(&type, 2, 8, 4, H264SW_PRED_L1);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 7, &type);
	CHECK(error == 0);
	check_sub(&type, 2, 4, 8, H264SW_PRED_L1);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 8, &type);
	CHECK(error == 0);
	check_sub(&type, 2, 8, 4, H264SW_PRED_BI);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 9, &type);
	CHECK(error == 0);
	check_sub(&type, 2, 4, 8, H264SW_PRED_BI);

	/* Table 7-18, rows 10 to 12: B_L0_4x4, B_L1_4x4, B_Bi_4x4. */
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 10, &type);
	CHECK(error == 0);
	check_sub(&type, 4, 4, 4, H264SW_PRED_L0);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 11, &type);
	CHECK(error == 0);
	check_sub(&type, 4, 4, 4, H264SW_PRED_L1);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 12, &type);
	CHECK(error == 0);
	check_sub(&type, 4, 4, 4, H264SW_PRED_BI);

	/* The parts times the part size cover an 8x8 sub-macroblock in every row but none. */
	for (t = 0; t < 13U; t++) {
		error = h264sw_sub_mb_type_of(H264SW_SLICE_B, t, &type);
		CHECK(error == 0);
		CHECK(type.parts * type.part_width * type.part_height == 64U);
	}
}

/* Checks that values outside the ranges are refused and leave the output alone. */
static void
check_type_errors(void)
{
	struct h264sw_mb_type type;
	struct h264sw_mb_type before;
	struct h264sw_sub_mb_type sub;
	int error;
	int difference;

	/* Marks the outputs so that a write on error shows. */
	memset(&type, 0x5a, sizeof(type));
	memset(&before, 0x5a, sizeof(before));
	memset(&sub, 0x5a, sizeof(sub));

	/* I slices stop at 25. */
	error = h264sw_mb_type_of(H264SW_SLICE_I, 26, &type);
	CHECK(error == EINVAL);
	error = h264sw_mb_type_of(H264SW_SLICE_I, 0xffffffffU, &type);
	CHECK(error == EINVAL);

	/* P slices stop at 30. */
	error = h264sw_mb_type_of(H264SW_SLICE_P, 31, &type);
	CHECK(error == EINVAL);
	error = h264sw_mb_type_of(H264SW_SLICE_P, 0x80000000U, &type);
	CHECK(error == EINVAL);

	/* B slices stop at 48. */
	error = h264sw_mb_type_of(H264SW_SLICE_B, 49, &type);
	CHECK(error == EINVAL);
	error = h264sw_mb_type_of(H264SW_SLICE_B, 0xffffffffU, &type);
	CHECK(error == EINVAL);

	/* SP, SI and unknown slice types have no table here. */
	error = h264sw_mb_type_of(H264SW_SLICE_SP, 0, &type);
	CHECK(error == EINVAL);
	error = h264sw_mb_type_of(H264SW_SLICE_SI, 0, &type);
	CHECK(error == EINVAL);
	error = h264sw_mb_type_of(99, 0, &type);
	CHECK(error == EINVAL);

	/* A missing output is refused. */
	error = h264sw_mb_type_of(H264SW_SLICE_I, 0, NULL);
	CHECK(error == EINVAL);

	/* The output was not written by any refusal. */
	difference = memcmp(&type, &before, sizeof(type));
	CHECK(difference == 0);

	/* Sub-macroblock types: P stops at 3, B at 12, other slice types have none. */
	error = h264sw_sub_mb_type_of(H264SW_SLICE_P, 4, &sub);
	CHECK(error == EINVAL);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 13, &sub);
	CHECK(error == EINVAL);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_B, 0xffffffffU, &sub);
	CHECK(error == EINVAL);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_I, 0, &sub);
	CHECK(error == EINVAL);
	error = h264sw_sub_mb_type_of(H264SW_SLICE_P, 0, NULL);
	CHECK(error == EINVAL);
	CHECK(sub.parts == 0x5a5a5a5aU);

	/* A skip type of an I slice is cleared, and a missing output is ignored. */
	h264sw_mb_skip_type(H264SW_SLICE_I, &type);
	CHECK(type.kind == H264SW_MB_I_NXN);
	CHECK(type.parts == 0);
	h264sw_mb_skip_type(H264SW_SLICE_P, NULL);
}

/* Checks the scan of the 4x4 blocks (6.4.3) and the index of a location (6.4.13.1). */
static void
check_block_scan(void)
{
	unsigned block_index;

	/* Figure 6-10 read by row: 0 1 4 5 / 2 3 6 7 / 8 9 12 13 / 10 11 14 15. */
	static const unsigned expect_x[16] = { 0, 4, 0, 4, 8, 12, 8, 12, 0, 4, 0, 4, 8, 12, 8, 12 };
	static const unsigned expect_y[16] = { 0, 0, 4, 4, 0, 0, 4, 4, 8, 8, 12, 12, 8, 8, 12, 12 };
	unsigned blk;
	unsigned x;
	unsigned y;

	/* Checks the position of each block and that its index comes back from any sample inside it. */
	for (blk = 0; blk < 16U; blk++) {
		CHECK(h264sw_blk4x4_x[blk] == expect_x[blk]);
		CHECK(h264sw_blk4x4_y[blk] == expect_y[blk]);
		block_index = h264sw_blk4x4_at(expect_x[blk], expect_y[blk]);
		CHECK(block_index == blk);
		block_index = h264sw_blk4x4_at(expect_x[blk] + 3U, expect_y[blk] + 3U);
		CHECK(block_index == blk);
	}

	/* Every sample of the macroblock is in the block whose square holds it. */
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			blk = h264sw_blk4x4_at(x, y);
			CHECK(blk < 16U);
			CHECK(x >= h264sw_blk4x4_x[blk] && x < h264sw_blk4x4_x[blk] + 4U);
			CHECK(y >= h264sw_blk4x4_y[blk] && y < h264sw_blk4x4_y[blk] + 4U);
		}
	}

	/* A location beyond the macroblock still gives a block index. */
	block_index = h264sw_blk4x4_at(0xffffffffU, 0xffffffffU);
	CHECK(block_index < 16U);
	block_index = h264sw_blk4x4_at(16, 0);
	CHECK(block_index == 0);
}

/* Checks the neighbouring macroblocks A, B, C and D (6.4.8, 6.4.9) of a 4 by 3 picture. */
static void
check_neighbours(void)
{
	struct h264sw_mb mbs[PIC_W * PIC_H];
	struct h264sw_neighbours n;

	/* Addresses are 4 * row + column: 0 1 2 3 / 4 5 6 7 / 8 9 10 11. */
	make_picture(mbs);

	/* The corner at address 0 has no neighbour but itself. */
	h264sw_mb_neighbours(mbs, PIC_W, 0, &n);
	CHECK(n.current == &mbs[0]);
	CHECK(n.addr == 0U && n.x == 0U && n.y == 0U);
	CHECK(n.a == NULL && n.b == NULL && n.c == NULL && n.d == NULL);

	/* The top row has only A. */
	h264sw_mb_neighbours(mbs, PIC_W, 2, &n);
	CHECK(n.current == &mbs[2]);
	CHECK(n.x == 2U && n.y == 0U);
	CHECK(n.a == &mbs[1] && n.b == NULL && n.c == NULL && n.d == NULL);

	/* The left column has B and C but not A and D. */
	h264sw_mb_neighbours(mbs, PIC_W, 4, &n);
	CHECK(n.x == 0U && n.y == 1U);
	CHECK(n.a == NULL && n.b == &mbs[0] && n.c == &mbs[1] && n.d == NULL);

	/* An inner macroblock has all four. */
	h264sw_mb_neighbours(mbs, PIC_W, 5, &n);
	CHECK(n.x == 1U && n.y == 1U);
	CHECK(n.a == &mbs[4] && n.b == &mbs[1] && n.c == &mbs[2] && n.d == &mbs[0]);

	/* The right column has no C, because the macroblock above and to the right is in the next row of the picture. */
	h264sw_mb_neighbours(mbs, PIC_W, 7, &n);
	CHECK(n.x == 3U && n.y == 1U);
	CHECK(n.a == &mbs[6] && n.b == &mbs[3] && n.c == NULL && n.d == &mbs[2]);

	/* The bottom-right corner has A, B and D. */
	h264sw_mb_neighbours(mbs, PIC_W, 11, &n);
	CHECK(n.x == 3U && n.y == 2U);
	CHECK(n.a == &mbs[10] && n.b == &mbs[7] && n.c == NULL && n.d == &mbs[6]);

	/* A macroblock of another slice is not available: B of 5 is in slice 1. */
	h264sw_mb_reset(&mbs[1], 1);
	h264sw_mb_neighbours(mbs, PIC_W, 5, &n);
	CHECK(n.a == &mbs[4] && n.b == NULL && n.c == &mbs[2] && n.d == &mbs[0]);

	/* The other slice rule holds for A, C and D too. */
	h264sw_mb_reset(&mbs[4], 2);
	h264sw_mb_reset(&mbs[2], 3);
	h264sw_mb_reset(&mbs[0], 4);
	h264sw_mb_neighbours(mbs, PIC_W, 5, &n);
	CHECK(n.a == NULL && n.b == NULL && n.c == NULL && n.d == NULL);

	/* A macroblock not decoded yet (slice -1) is not available. */
	make_picture(mbs);
	h264sw_mb_reset(&mbs[4], -1);
	h264sw_mb_neighbours(mbs, PIC_W, 5, &n);
	CHECK(n.a == NULL && n.b == &mbs[1] && n.c == &mbs[2] && n.d == &mbs[0]);

	/* A current macroblock with no slice has no neighbour. */
	h264sw_mb_reset(&mbs[5], -1);
	h264sw_mb_neighbours(mbs, PIC_W, 5, &n);
	CHECK(n.current == &mbs[5]);
	CHECK(n.a == NULL && n.b == NULL && n.c == NULL && n.d == NULL);

	/* A picture one macroblock wide has only B. */
	make_picture(mbs);
	h264sw_mb_neighbours(mbs, 1, 2, &n);
	CHECK(n.x == 0U && n.y == 2U);
	CHECK(n.a == NULL && n.b == &mbs[1] && n.c == NULL && n.d == NULL);
}

/* Checks the neighbouring locations of Table 6-3 and equations 6-34 and 6-35. */
static void
check_locate(void)
{
	struct h264sw_mb mbs[PIC_W * PIC_H];
	struct h264sw_neighbours n;
	struct h264sw_mb *found;
	unsigned xw;
	unsigned yw;

	/* Works on the inner macroblock 5, whose four neighbours are all available. */
	make_picture(mbs);
	h264sw_mb_neighbours(mbs, PIC_W, 5, &n);

	/* Row 1 of Table 6-3: xN and yN both negative is D. */
	found = h264sw_mb_locate(&n, -1, -1, 16, 16, &xw, &yw);
	CHECK(found == &mbs[0]);
	CHECK(xw == 15U && yw == 15U);

	/* Row 2: xN negative and yN in 0..maxH - 1 is A. */
	found = h264sw_mb_locate(&n, -1, 5, 16, 16, &xw, &yw);
	CHECK(found == &mbs[4]);
	CHECK(xw == 15U && yw == 5U);
	found = h264sw_mb_locate(&n, -1, 15, 16, 16, &xw, &yw);
	CHECK(found == &mbs[4]);
	CHECK(xw == 15U && yw == 15U);

	/* Row 3: xN in 0..maxW - 1 and yN negative is B. */
	found = h264sw_mb_locate(&n, 3, -1, 16, 16, &xw, &yw);
	CHECK(found == &mbs[1]);
	CHECK(xw == 3U && yw == 15U);

	/* Row 4: both inside is the current macroblock. */
	found = h264sw_mb_locate(&n, 3, 4, 16, 16, &xw, &yw);
	CHECK(found == &mbs[5]);
	CHECK(xw == 3U && yw == 4U);
	found = h264sw_mb_locate(&n, 15, 15, 16, 16, &xw, &yw);
	CHECK(found == &mbs[5]);
	CHECK(xw == 15U && yw == 15U);

	/* Row 5: xN above maxW - 1 and yN negative is C. */
	found = h264sw_mb_locate(&n, 16, -1, 16, 16, &xw, &yw);
	CHECK(found == &mbs[2]);
	CHECK(xw == 0U && yw == 15U);

	/* Row 6: xN above maxW - 1 and yN inside is not available. */
	found = h264sw_mb_locate(&n, 16, 5, 16, 16, &xw, &yw);
	CHECK(found == NULL);

	/* Row 7: yN above maxH - 1 is not available, whatever xN is. */
	found = h264sw_mb_locate(&n, 3, 16, 16, 16, &xw, &yw);
	CHECK(found == NULL);
	found = h264sw_mb_locate(&n, -1, 16, 16, 16, &xw, &yw);
	CHECK(found == NULL);
	found = h264sw_mb_locate(&n, 16, 16, 16, 16, &xw, &yw);
	CHECK(found == NULL);

	/* Chroma of 4:2:0 uses maxW = maxH = 8: left of column 0 is column 7, above row 0 is row 7. */
	found = h264sw_mb_locate(&n, -1, 3, 8, 8, &xw, &yw);
	CHECK(found == &mbs[4]);
	CHECK(xw == 7U && yw == 3U);
	found = h264sw_mb_locate(&n, 4, -1, 8, 8, &xw, &yw);
	CHECK(found == &mbs[1]);
	CHECK(xw == 4U && yw == 7U);
	found = h264sw_mb_locate(&n, 8, -1, 8, 8, &xw, &yw);
	CHECK(found == &mbs[2]);
	CHECK(xw == 0U && yw == 7U);
	found = h264sw_mb_locate(&n, 7, 8, 8, 8, &xw, &yw);
	CHECK(found == NULL);

	/* A neighbour that is not available is NULL even when the location is in range: on the corner macroblock. */
	h264sw_mb_neighbours(mbs, PIC_W, 0, &n);
	found = h264sw_mb_locate(&n, -1, 0, 16, 16, &xw, &yw);
	CHECK(found == NULL);
	CHECK(xw == 0U && yw == 0U);
	found = h264sw_mb_locate(&n, 0, -1, 16, 16, &xw, &yw);
	CHECK(found == NULL);
	found = h264sw_mb_locate(&n, 0, 0, 16, 16, &xw, &yw);
	CHECK(found == &mbs[0]);

	/* Extreme locations do not overflow and stay inside the macroblock found. */
	h264sw_mb_neighbours(mbs, PIC_W, 5, &n);
	found = h264sw_mb_locate(&n, INT_MIN, INT_MIN, 16, 16, &xw, &yw);
	CHECK(found == &mbs[0]);
	CHECK(xw < 16U && yw < 16U);
	found = h264sw_mb_locate(&n, INT_MAX, INT_MIN, 16, 16, &xw, &yw);
	CHECK(found == &mbs[2]);
	CHECK(xw < 16U && yw < 16U);
	found = h264sw_mb_locate(&n, 0, INT_MIN, 16, 16, &xw, &yw);
	CHECK(found == &mbs[1]);
	CHECK(xw == 0U && yw < 16U);
	found = h264sw_mb_locate(&n, INT_MAX, INT_MAX, 16, 16, &xw, &yw);
	CHECK(found == NULL);
	found = h264sw_mb_locate(&n, 0, 0, 0xffffffffU, 0xffffffffU, &xw, &yw);
	CHECK(found == &mbs[5]);

	/* A missing neighbour set, a component of no size and missing outputs are tolerated. */
	found = h264sw_mb_locate(NULL, 0, 0, 16, 16, &xw, &yw);
	CHECK(found == NULL);
	found = h264sw_mb_locate(&n, 0, 0, 0, 16, &xw, &yw);
	CHECK(found == NULL);
	found = h264sw_mb_locate(&n, 0, 0, 16, 0, &xw, &yw);
	CHECK(found == NULL);
	found = h264sw_mb_locate(&n, -1, -1, 16, 16, NULL, NULL);
	CHECK(found == &mbs[0]);
}

/* Compares a block-neighbour function with an expected table for every block. */
static void
check_expected(
	const struct h264sw_neighbours *n,
	struct h264sw_mb *(*finder)(const struct h264sw_neighbours *, unsigned, int, unsigned *),
	unsigned count,
	const struct expect_block *a,
	const struct expect_block *b)
{
	struct h264sw_mb *found;
	unsigned blk;
	unsigned blk_n;

	/* Checks A (upper 0) and B (upper 1) of each block against the table. */
	for (blk = 0; blk < count; blk++) {
		blk_n = 99;
		found = finder(n, blk, 0, &blk_n);
		if (a[blk].in_current != 0U)
			CHECK(found == n->current);
		else
			CHECK(found == n->a);
		CHECK(found != NULL);
		CHECK(blk_n == a[blk].blk);

		/* Looks up neighbour B with the same marker that a lookup must overwrite. */
		blk_n = 99;
		found = finder(n, blk, 1, &blk_n);
		if (b[blk].in_current != 0U)
			CHECK(found == n->current);
		else
			CHECK(found == n->b);
		CHECK(found != NULL);
		CHECK(blk_n == b[blk].blk);
	}
}

/* Checks the neighbouring 4x4, 8x8 and chroma blocks (6.4.11.2, 6.4.11.4, 6.4.11.5). */
static void
check_block_neighbours(void)
{
	struct h264sw_mb mbs[PIC_W * PIC_H];
	struct h264sw_neighbours n;
	struct h264sw_mb *found;
	unsigned blk_n;

	/* An inner macroblock, whose A and B are available: every block, in the current macroblock and beside it. */
	make_picture(mbs);
	h264sw_mb_neighbours(mbs, PIC_W, 5, &n);
	check_expected(&n, h264sw_mb_neighbour_4x4, 16, luma4x4_a, luma4x4_b);
	check_expected(&n, h264sw_mb_neighbour_8x8, 4, luma8x8_a, luma8x8_b);
	check_expected(&n, h264sw_mb_neighbour_chroma, 4, chroma_a, chroma_b);

	/* The corner macroblock has no A or B: only the blocks that start at the edge lose their neighbour. */
	h264sw_mb_neighbours(mbs, PIC_W, 0, &n);
	blk_n = 99;
	found = h264sw_mb_neighbour_4x4(&n, 0, 0, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_4x4(&n, 0, 1, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_4x4(&n, 3, 0, &blk_n);
	CHECK(found == &mbs[0] && blk_n == 2U);
	found = h264sw_mb_neighbour_4x4(&n, 3, 1, &blk_n);
	CHECK(found == &mbs[0] && blk_n == 1U);
	found = h264sw_mb_neighbour_4x4(&n, 2, 0, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_4x4(&n, 1, 1, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_8x8(&n, 0, 0, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_8x8(&n, 3, 1, &blk_n);
	CHECK(found == &mbs[0] && blk_n == 1U);
	found = h264sw_mb_neighbour_chroma(&n, 0, 1, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_chroma(&n, 3, 0, &blk_n);
	CHECK(found == &mbs[0] && blk_n == 2U);

	/* A left neighbour in another slice is not available but the current macroblock's own blocks are. */
	make_picture(mbs);
	h264sw_mb_reset(&mbs[4], 1);
	h264sw_mb_neighbours(mbs, PIC_W, 5, &n);
	found = h264sw_mb_neighbour_4x4(&n, 0, 0, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_4x4(&n, 1, 0, &blk_n);
	CHECK(found == &mbs[5] && blk_n == 0U);
	found = h264sw_mb_neighbour_4x4(&n, 0, 1, &blk_n);
	CHECK(found == &mbs[1] && blk_n == 10U);
}

/* Checks that block indices outside their ranges and missing inputs give NULL without a crash. */
static void
check_neighbour_errors(void)
{
	struct h264sw_mb mbs[PIC_W * PIC_H];
	struct h264sw_neighbours n;
	struct h264sw_mb *found;
	unsigned blk_n;

	/* Builds the neighbours of an inner macroblock. */
	make_picture(mbs);
	h264sw_mb_neighbours(mbs, PIC_W, 5, &n);

	/* A luma 4x4 block index of 16 or more is refused. */
	blk_n = 99;
	found = h264sw_mb_neighbour_4x4(&n, 16, 0, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_4x4(&n, 0xffffffffU, 1, &blk_n);
	CHECK(found == NULL);

	/* A luma 8x8 block index of 4 or more is refused. */
	found = h264sw_mb_neighbour_8x8(&n, 4, 0, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_8x8(&n, 0xffffffffU, 1, &blk_n);
	CHECK(found == NULL);

	/* A chroma block index of 4 or more is refused. */
	found = h264sw_mb_neighbour_chroma(&n, 4, 0, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_chroma(&n, 0xffffffffU, 1, &blk_n);
	CHECK(found == NULL);

	/* Missing neighbours and a missing index output are tolerated. */
	found = h264sw_mb_neighbour_4x4(NULL, 0, 0, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_8x8(NULL, 0, 0, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_chroma(NULL, 0, 0, &blk_n);
	CHECK(found == NULL);
	found = h264sw_mb_neighbour_4x4(&n, 0, 0, NULL);
	CHECK(found == &mbs[4]);

	/* A neighbour set with a missing picture and one with width 0 name no macroblock. */
	h264sw_mb_neighbours(NULL, PIC_W, 5, &n);
	CHECK(n.current == NULL && n.a == NULL && n.b == NULL && n.c == NULL && n.d == NULL);
	h264sw_mb_neighbours(mbs, 0, 5, &n);
	CHECK(n.current == NULL && n.a == NULL && n.b == NULL && n.c == NULL && n.d == NULL);
	h264sw_mb_neighbours(mbs, PIC_W, 5, NULL);
}
