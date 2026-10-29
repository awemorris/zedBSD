/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the intra prediction of the software H.264 decoder (8.3): the
 * derivation of the Intra_4x4 and Intra_8x8 modes, the gathering of the
 * neighbouring samples and their availability, and the predictions of
 * the 4x4, 8x8, 16x16 and chroma blocks.  The expected values are
 * worked out by hand from the equations of the clauses (the equation
 * numbers are named at each table), never taken from another decoder's
 * output.
 */

#include <errno.h>
#include <string.h>

#include "check.h"
#include "userland/desktop/libmedia/h264sw-context.h"
#include "userland/desktop/libmedia/h264sw-intra.h"

/* The picture of the gathering and mode tests: 3 by 3 macroblocks, whose centre macroblock (address 4) has all four neighbours. */
#define PIC_W 3U
#define PIC_H 3U
#define LUMA_W 48U
#define CHROMA_W 24U

/* The parts of an edge that every block of the centre macroblock has on its left, above and above-left. */
#define ALL_BUT_RIGHT (H264SW_EDGE_LEFT | H264SW_EDGE_TOP | H264SW_EDGE_TOP_LEFT)
#define ALL_PARTS (ALL_BUT_RIGHT | H264SW_EDGE_TOP_RIGHT)

/*
 * The decoding state the tests build by hand: a picture of macroblocks,
 * a frame with its three planes, the active picture parameter set and a
 * slice context whose neighbours are those of the macroblock under test.
 * They are static because the slice context is large; every test that
 * uses them calls setup_picture first.
 */
static struct h264sw_mb test_mbs[PIC_W * PIC_H];
static struct h264sw_picture_ctx test_picture;
static struct h264sw_pps test_pps;
static struct h264sw_frame test_frame;
static uint8_t test_luma[LUMA_W * LUMA_W];
static uint8_t test_cb[CHROMA_W * CHROMA_W];
static uint8_t test_cr[CHROMA_W * CHROMA_W];
static struct h264sw_slice_ctx test_ctx;

/*
 * Intra_4x4 predictions for the edge top[0..7] = 10, 20, ..., 80, left[0..3] = 5, 15, 25, 35, top_left = 0, worked out from the equations.
 * Each table is read in raster order, 16 samples.
 */

/* Intra_4x4_Vertical (8-46): the top row in every row. */
static const uint8_t expect_4x4_vertical[16] = {
	10, 20, 30, 40,
	10, 20, 30, 40,
	10, 20, 30, 40,
	10, 20, 30, 40
};

/* Intra_4x4_Horizontal (8-47): the left column in every column. */
static const uint8_t expect_4x4_horizontal[16] = {
	5, 5, 5, 5,
	15, 15, 15, 15,
	25, 25, 25, 25,
	35, 35, 35, 35
};

/* Intra_4x4_Diagonal_Down_Left (8-52, 8-53): pred[ 0, 0 ] = ( 10 + 2 * 20 + 30 + 2 ) >> 2 = 20, pred[ 3, 3 ] = ( 70 + 3 * 80 + 2 ) >> 2 = 78. */
static const uint8_t expect_4x4_ddl[16] = {
	20, 30, 40, 50,
	30, 40, 50, 60,
	40, 50, 60, 70,
	50, 60, 70, 78
};

/* Intra_4x4_Diagonal_Down_Right (8-54 to 8-56): pred[ 0, 0 ] = ( 10 + 2 * 0 + 5 + 2 ) >> 2 = 4. */
static const uint8_t expect_4x4_ddr[16] = {
	4, 10, 20, 30,
	6, 4, 10, 20,
	15, 6, 4, 10,
	25, 15, 6, 4
};

/* Intra_4x4_Vertical_Right (8-57 to 8-60): pred[ 1, 1 ] has zVR 1, ( 0 + 2 * 10 + 20 + 2 ) >> 2 = 10. */
static const uint8_t expect_4x4_vr[16] = {
	5, 15, 25, 35,
	4, 10, 20, 30,
	6, 5, 15, 25,
	15, 4, 10, 20
};

/* Intra_4x4_Horizontal_Down (8-61 to 8-64): pred[ 0, 0 ] has zHD 0, ( 0 + 5 + 1 ) >> 1 = 3. */
static const uint8_t expect_4x4_hd[16] = {
	3, 4, 10, 20,
	10, 6, 3, 4,
	20, 15, 10, 6,
	30, 25, 20, 15
};

/* Intra_4x4_Vertical_Left (8-65, 8-66): pred[ 0, 0 ] = ( 10 + 20 + 1 ) >> 1 = 15. */
static const uint8_t expect_4x4_vl[16] = {
	15, 25, 35, 45,
	20, 30, 40, 50,
	25, 35, 45, 55,
	30, 40, 50, 60
};

/* Intra_4x4_Horizontal_Up (8-67 to 8-70): pred[ 3, 1 ] has zHU 5, ( 25 + 3 * 35 + 2 ) >> 2 = 33; zHU above 5 repeats 35. */
static const uint8_t expect_4x4_hu[16] = {
	10, 15, 20, 25,
	20, 25, 30, 33,
	30, 33, 35, 35,
	35, 35, 35, 35
};

/* Intra_4x4_Diagonal_Down_Left with p[ 4..7, -1 ] not available, so that p[ 3, -1 ] = 40 stands for them. */
static const uint8_t expect_4x4_ddl_substituted[16] = {
	20, 30, 38, 40,
	30, 38, 40, 40,
	38, 40, 40, 40,
	40, 40, 40, 40
};

/* Intra_4x4_Vertical_Left with p[ 4..7, -1 ] substituted by 40. */
static const uint8_t expect_4x4_vl_substituted[16] = {
	15, 25, 35, 40,
	20, 30, 38, 40,
	25, 35, 40, 40,
	30, 38, 40, 40
};

/*
 * Intra_8x8 predictions for the edge top[0..15] = 20, 25, 31, 40, 52,
 * 61, 70, 90, 100, 110, 130, 150, 170, 190, 200, 220, left[0..7] = 15,
 * 30, 33, 50, 80, 85, 120, 130, top_left = 12.  The reference sample
 * filtering of 8-78 to 8-88 gives p'[ x, -1 ] = 19, 25, 32, 41, 51, 61,
 * 73, 88, 100, 113, 130, 150, 170, 188, 203, 215, p'[ -1, y ] = 18, 27,
 * 37, 53, 74, 93, 114, 128 and p'[ -1, -1 ] = 15 (for example p'[ 0, -1
 * ] = ( 12 + 2 * 20 + 25 + 2 ) >> 2 = 19 and p'[ 15, -1 ] = ( 200 + 3 *
 * 220 + 2 ) >> 2 = 215).  Each table is read in raster order, 64
 * samples.
 */

/* Intra_8x8_Vertical (8-89): the filtered top row in every row. */
static const uint8_t expect_8x8_vertical[64] = {
	 19,  25,  32,  41,  51,  61,  73,  88,
	 19,  25,  32,  41,  51,  61,  73,  88,
	 19,  25,  32,  41,  51,  61,  73,  88,
	 19,  25,  32,  41,  51,  61,  73,  88,
	 19,  25,  32,  41,  51,  61,  73,  88,
	 19,  25,  32,  41,  51,  61,  73,  88,
	 19,  25,  32,  41,  51,  61,  73,  88,
	 19,  25,  32,  41,  51,  61,  73,  88,
};
/* Intra_8x8_Horizontal (8-90): the filtered left column in every column. */
static const uint8_t expect_8x8_horizontal[64] = {
	 18,  18,  18,  18,  18,  18,  18,  18,
	 27,  27,  27,  27,  27,  27,  27,  27,
	 37,  37,  37,  37,  37,  37,  37,  37,
	 53,  53,  53,  53,  53,  53,  53,  53,
	 74,  74,  74,  74,  74,  74,  74,  74,
	 93,  93,  93,  93,  93,  93,  93,  93,
	114, 114, 114, 114, 114, 114, 114, 114,
	128, 128, 128, 128, 128, 128, 128, 128,
};
/* Intra_8x8_Diagonal_Down_Left (8-95, 8-96). */
static const uint8_t expect_8x8_ddl[64] = {
	 25,  33,  41,  51,  62,  74,  87, 100,
	 33,  41,  51,  62,  74,  87, 100, 114,
	 41,  51,  62,  74,  87, 100, 114, 131,
	 51,  62,  74,  87, 100, 114, 131, 150,
	 62,  74,  87, 100, 114, 131, 150, 170,
	 74,  87, 100, 114, 131, 150, 170, 187,
	 87, 100, 114, 131, 150, 170, 187, 202,
	100, 114, 131, 150, 170, 187, 202, 212,
};
/* Intra_8x8_Diagonal_Down_Right (8-97 to 8-99). */
static const uint8_t expect_8x8_ddr[64] = {
	 17,  20,  25,  33,  41,  51,  62,  74,
	 20,  17,  20,  25,  33,  41,  51,  62,
	 27,  20,  17,  20,  25,  33,  41,  51,
	 39,  27,  20,  17,  20,  25,  33,  41,
	 54,  39,  27,  20,  17,  20,  25,  33,
	 74,  54,  39,  27,  20,  17,  20,  25,
	 94,  74,  54,  39,  27,  20,  17,  20,
	112,  94,  74,  54,  39,  27,  20,  17,
};
/* Intra_8x8_Vertical_Right (8-100 to 8-103). */
static const uint8_t expect_8x8_vr[64] = {
	 17,  22,  29,  37,  46,  56,  67,  81,
	 17,  20,  25,  33,  41,  51,  62,  74,
	 20,  17,  22,  29,  37,  46,  56,  67,
	 27,  17,  20,  25,  33,  41,  51,  62,
	 39,  20,  17,  22,  29,  37,  46,  56,
	 54,  27,  17,  20,  25,  33,  41,  51,
	 74,  39,  20,  17,  22,  29,  37,  46,
	 94,  54,  27,  17,  20,  25,  33,  41,
};
/* Intra_8x8_Horizontal_Down (8-104 to 8-107). */
static const uint8_t expect_8x8_hd[64] = {
	 17,  17,  20,  25,  33,  41,  51,  62,
	 23,  20,  17,  17,  20,  25,  33,  41,
	 32,  27,  23,  20,  17,  17,  20,  25,
	 45,  39,  32,  27,  23,  20,  17,  17,
	 64,  54,  45,  39,  32,  27,  23,  20,
	 84,  74,  64,  54,  45,  39,  32,  27,
	104,  94,  84,  74,  64,  54,  45,  39,
	121, 112, 104,  94,  84,  74,  64,  54,
};
/* Intra_8x8_Vertical_Left (8-108, 8-109). */
static const uint8_t expect_8x8_vl[64] = {
	 22,  29,  37,  46,  56,  67,  81,  94,
	 25,  33,  41,  51,  62,  74,  87, 100,
	 29,  37,  46,  56,  67,  81,  94, 107,
	 33,  41,  51,  62,  74,  87, 100, 114,
	 37,  46,  56,  67,  81,  94, 107, 122,
	 41,  51,  62,  74,  87, 100, 114, 131,
	 46,  56,  67,  81,  94, 107, 122, 140,
	 51,  62,  74,  87, 100, 114, 131, 150,
};
/* Intra_8x8_Horizontal_Up (8-110 to 8-113). */
static const uint8_t expect_8x8_hu[64] = {
	 23,  27,  32,  39,  45,  54,  64,  74,
	 32,  39,  45,  54,  64,  74,  84,  94,
	 45,  54,  64,  74,  84,  94, 104, 112,
	 64,  74,  84,  94, 104, 112, 121, 125,
	 84,  94, 104, 112, 121, 125, 128, 128,
	104, 112, 121, 125, 128, 128, 128, 128,
	121, 125, 128, 128, 128, 128, 128, 128,
	128, 128, 128, 128, 128, 128, 128, 128,
};
/* Intra_8x8_Diagonal_Down_Left when the top-right samples are substituted by p[ 7, -1 ] before filtering. */
static const uint8_t expect_8x8_ddl_substituted[64] = {
	 25,  33,  41,  51,  62,  73,  83,  89,
	 33,  41,  51,  62,  73,  83,  89,  90,
	 41,  51,  62,  73,  83,  89,  90,  90,
	 51,  62,  73,  83,  89,  90,  90,  90,
	 62,  73,  83,  89,  90,  90,  90,  90,
	 73,  83,  89,  90,  90,  90,  90,  90,
	 83,  89,  90,  90,  90,  90,  90,  90,
	 89,  90,  90,  90,  90,  90,  90,  90,
};
/* Intra_Chroma_Plane (8-144 to 8-149). */
static const uint8_t expect_chroma_plane[64] = {
	 44,  53,  62,  71,  80,  89,  98, 107,
	 57,  66,  75,  84,  93, 102, 111, 120,
	 70,  79,  88,  97, 106, 115, 124, 133,
	 83,  92, 101, 110, 119, 128, 137, 147,
	 96, 105, 114, 123, 132, 141, 150, 160,
	109, 118, 127, 136, 145, 154, 163, 173,
	122, 131, 140, 149, 158, 167, 176, 186,
	135, 144, 153, 162, 171, 180, 190, 199,
};

/* Forward declarations of the static functions. */
static unsigned luma_value(unsigned x, unsigned y);
static unsigned cb_value(unsigned x, unsigned y);
static unsigned cr_value(unsigned x, unsigned y);
static void setup_picture(void);
static void select_mb(unsigned addr);
static int same_samples(const uint8_t *actual, const uint8_t *expected, unsigned count);
static int all_equal(const uint8_t *actual, unsigned count, unsigned value);
static void make_edge4(struct h264sw_intra_edge *edge);
static void make_edge8(struct h264sw_intra_edge *edge);
static void make_edge16(struct h264sw_intra_edge *edge, unsigned base, int top_step, int left_step, unsigned top_left);
static void make_edge_chroma(struct h264sw_intra_edge *edge);
static void block_origin_4x4(unsigned blk, unsigned *x, unsigned *y);
static void check_luma_edge(unsigned addr, unsigned size, unsigned blk, unsigned expected);
static void check_chroma_edge(unsigned addr, unsigned plane, unsigned expected);
static void check_pred4x4(void);
static void check_pred4x4_requirements(void);
static void check_pred8x8(void);
static void check_pred8x8_variants(void);
static void check_pred16x16(void);
static void check_pred16x16_plane(void);
static void check_pred_chroma(void);
static void check_pred_errors(void);
static void check_modes4x4(void);
static void check_modes8x8(void);
static void check_edge4x4(void);
static void check_edge8x8(void);
static void check_edge16_and_chroma(void);
static void check_edge_errors(void);

int
main(void)
{
	/* Checks the predictions of each block size from hand-made edges. */
	check_pred4x4();
	check_pred4x4_requirements();
	check_pred8x8();
	check_pred8x8_variants();
	check_pred16x16();
	check_pred16x16_plane();
	check_pred_chroma();
	check_pred_errors();

	/* Checks the derivation of the prediction modes from the neighbours. */
	check_modes4x4();
	check_modes8x8();

	/* Checks the gathering of the neighbouring samples and their availability. */
	check_edge4x4();
	check_edge8x8();
	check_edge16_and_chroma();
	check_edge_errors();

	/* Reports how many checks passed. */
	return check_finish("intra");
}

/* Gives the luma sample the tests put at (x, y) of the picture: a pattern whose neighbours differ. */
static unsigned
luma_value(
	unsigned x,
	unsigned y)
{
	return (x * 5U + y * 3U) % 251U;
}

/* Gives the Cb sample the tests put at (x, y) of the chroma plane. */
static unsigned
cb_value(
	unsigned x,
	unsigned y)
{
	return (x * 9U + y * 2U + 7U) % 241U;
}

/* Gives the Cr sample the tests put at (x, y) of the chroma plane, a pattern other than Cb's. */
static unsigned
cr_value(
	unsigned x,
	unsigned y)
{
	return (x * 3U + y * 8U + 11U) % 239U;
}

/*
 * Builds the picture of the gathering and mode tests from nothing.
 *
 * Every macroblock is an Intra_16x16 macroblock of slice 0 with all
 * Intra4x4PredMode entries 2; constrained_intra_pred_flag is 0; the
 * planes hold the patterns above; the current macroblock is the centre
 * one.
 */
static void
setup_picture(void)
{
	unsigned i;
	unsigned x;
	unsigned y;

	/* Clears the whole state, so that no test sees what an earlier one left. */
	memset(test_mbs, 0, sizeof(test_mbs));
	memset(&test_picture, 0, sizeof(test_picture));
	memset(&test_pps, 0, sizeof(test_pps));
	memset(&test_frame, 0, sizeof(test_frame));
	memset(&test_ctx, 0, sizeof(test_ctx));

	/* Makes every macroblock decoded, intra and in slice 0. */
	for (i = 0; i < PIC_W * PIC_H; i++) {
		h264sw_mb_reset(&test_mbs[i], 0);
		test_mbs[i].kind = H264SW_MB_I_16X16;
	}

	/* Describes the frame: a luma plane of 48 by 48 and two chroma planes of 24 by 24. */
	test_frame.plane[0] = test_luma;
	test_frame.plane[1] = test_cb;
	test_frame.plane[2] = test_cr;
	test_frame.stride[0] = LUMA_W;
	test_frame.stride[1] = CHROMA_W;
	test_frame.stride[2] = CHROMA_W;
	test_frame.width = LUMA_W;
	test_frame.height = LUMA_W;

	/* Fills the luma plane with its pattern. */
	for (y = 0; y < LUMA_W; y++) {
		for (x = 0; x < LUMA_W; x++)
			test_luma[y * LUMA_W + x] = (uint8_t)luma_value(x, y);
	}

	/* Fills the chroma planes with their patterns. */
	for (y = 0; y < CHROMA_W; y++) {
		for (x = 0; x < CHROMA_W; x++) {
			test_cb[y * CHROMA_W + x] = (uint8_t)cb_value(x, y);
			test_cr[y * CHROMA_W + x] = (uint8_t)cr_value(x, y);
		}
	}

	/* Ties the picture and the slice context together. */
	test_picture.frame = &test_frame;
	test_picture.pps = &test_pps;
	test_picture.mbs = test_mbs;
	test_picture.width_mbs = PIC_W;
	test_picture.height_mbs = PIC_H;
	test_picture.size_mbs = PIC_W * PIC_H;
	test_ctx.picture = &test_picture;

	/* Makes the centre macroblock the current one. */
	select_mb(4U);
}

/* Makes a macroblock the current one by finding its neighbours (6.4.9). */
static void
select_mb(
	unsigned addr)
{
	/* Finds the neighbours of the macroblock in the slice context. */
	h264sw_mb_neighbours(test_mbs, PIC_W, addr, &test_ctx.n);
	test_ctx.mb_addr = addr;
}

/* Tells whether two runs of samples are equal. */
static int
same_samples(
	const uint8_t *actual,
	const uint8_t *expected,
	unsigned count)
{
	unsigned i;

	/* Compares the samples one by one. */
	for (i = 0; i < count; i++) {
		if (actual[i] != expected[i])
			return 0;
	}

	/* The runs are equal. */
	return 1;
}

/* Tells whether every one of count samples has the value. */
static int
all_equal(
	const uint8_t *actual,
	unsigned count,
	unsigned value)
{
	unsigned i;

	/* Compares the samples with the value. */
	for (i = 0; i < count; i++) {
		if (actual[i] != value)
			return 0;
	}

	/* Every sample has the value. */
	return 1;
}

/* Makes the edge of the 4x4 tables: top 10..80, left 5, 15, 25, 35, top-left 0, every part available. */
static void
make_edge4(
	struct h264sw_intra_edge *edge)
{
	unsigned i;

	/* Clears the edge and puts the samples in. */
	memset(edge, 0, sizeof(*edge));
	for (i = 0; i < 8U; i++)
		edge->top[i] = (uint8_t)(10U * (i + 1U));
	edge->left[0] = 5;
	edge->left[1] = 15;
	edge->left[2] = 25;
	edge->left[3] = 35;
	edge->top_left = 0;
	edge->available = ALL_PARTS;
}

/* Makes the edge of the 8x8 tables, every part available. */
static void
make_edge8(
	struct h264sw_intra_edge *edge)
{
	static const uint8_t top[16] = { 20, 25, 31, 40, 52, 61, 70, 90, 100, 110, 130, 150, 170, 190, 200, 220 };
	static const uint8_t left[8] = { 15, 30, 33, 50, 80, 85, 120, 130 };

	/* Clears the edge and puts the samples in. */
	memset(edge, 0, sizeof(*edge));
	memcpy(edge->top, top, sizeof(top));
	memcpy(edge->left, left, sizeof(left));
	edge->top_left = 12;
	edge->available = ALL_PARTS;
}

/* Makes a 16x16 edge whose top row is base + top_step * x and left column is base + left_step * y, without a top-right. */
static void
make_edge16(
	struct h264sw_intra_edge *edge,
	unsigned base,
	int top_step,
	int left_step,
	unsigned top_left)
{
	unsigned i;

	/* Clears the edge and puts the ramps in. */
	memset(edge, 0, sizeof(*edge));
	for (i = 0; i < 16U; i++) {
		edge->top[i] = (uint8_t)((int)base + top_step * (int)i);
		edge->left[i] = (uint8_t)((int)base + left_step * (int)i);
	}
	edge->top_left = (uint8_t)top_left;
	edge->available = ALL_BUT_RIGHT;
}

/* Makes the edge of the chroma tables, every part but the top-right available. */
static void
make_edge_chroma(
	struct h264sw_intra_edge *edge)
{
	static const uint8_t top[8] = { 40, 45, 50, 60, 70, 85, 90, 100 };
	static const uint8_t left[8] = { 30, 35, 45, 60, 80, 100, 110, 120 };

	/* Clears the edge and puts the samples in. */
	memset(edge, 0, sizeof(*edge));
	memcpy(edge->top, top, sizeof(top));
	memcpy(edge->left, left, sizeof(left));
	edge->top_left = 25;
	edge->available = ALL_BUT_RIGHT;
}

/* Gives the position of a luma 4x4 block in its macroblock by the scan of Figure 6-10: 8x8 quadrants in raster order, 4x4 blocks in raster order inside each. */
static void
block_origin_4x4(
	unsigned blk,
	unsigned *x,
	unsigned *y)
{
	/* Takes the quadrant's corner and the block's offset inside the quadrant. */
	*x = 8U * ((blk / 4U) % 2U) + 4U * (blk % 2U);
	*y = 8U * (blk / 8U) + 4U * ((blk / 2U) % 2U);
}

/*
 * Checks the gathered luma edge of a block of the macroblock at addr.
 *
 * expected is the set of parts that must be available; each of them must
 * hold the samples of the picture next to the block.
 */
static void
check_luma_edge(
	unsigned addr,
	unsigned size,
	unsigned blk,
	unsigned expected)
{
	struct h264sw_intra_edge edge;
	unsigned ox;
	unsigned oy;
	unsigned ax;
	unsigned ay;
	unsigned i;
	unsigned want;
	int left_ok;
	int top_ok;
	int corner_ok;
	int right_ok;

	/* Gathers the edge. */
	select_mb(addr);
	memset(&edge, 0xaa, sizeof(edge));
	h264sw_intra_edge_luma(&test_ctx, size, blk, &edge);

	/* Checks which parts are available. */
	CHECK(edge.available == expected);

	/* Finds the block's upper-left sample in the picture. */
	ox = 0;
	oy = 0;
	if (size == 4U)
		block_origin_4x4(blk, &ox, &oy);
	if (size == 8U) {
		ox = 8U * (blk % 2U);
		oy = 8U * (blk / 2U);
	}
	ax = (addr % PIC_W) * 16U + ox;
	ay = (addr / PIC_W) * 16U + oy;

	/* Checks the samples of each part that is available. */
	left_ok = 1;
	top_ok = 1;
	corner_ok = 1;
	right_ok = 1;
	for (i = 0; i < size; i++) {
		if ((expected & H264SW_EDGE_LEFT) != 0U) {
			want = luma_value(ax - 1U, ay + i);
			if (edge.left[i] != want)
				left_ok = 0;
		}
		if ((expected & H264SW_EDGE_TOP) != 0U) {
			want = luma_value(ax + i, ay - 1U);
			if (edge.top[i] != want)
				top_ok = 0;
		}
		if ((expected & H264SW_EDGE_TOP_RIGHT) != 0U) {
			want = luma_value(ax + size + i, ay - 1U);
			if (edge.top[size + i] != want)
				right_ok = 0;
		}
	}
	if ((expected & H264SW_EDGE_TOP_LEFT) != 0U) {
		want = luma_value(ax - 1U, ay - 1U);
		if (edge.top_left != want)
			corner_ok = 0;
	}
	CHECK(left_ok);
	CHECK(top_ok);
	CHECK(corner_ok);
	CHECK(right_ok);
}

/* Checks the gathered chroma edge of plane (1 Cb, 2 Cr) of the macroblock at addr, as check_luma_edge does for luma. */
static void
check_chroma_edge(
	unsigned addr,
	unsigned plane,
	unsigned expected)
{
	struct h264sw_intra_edge edge;
	unsigned ax;
	unsigned ay;
	unsigned i;
	unsigned want;
	int left_ok;
	int top_ok;
	int corner_ok;

	/* Gathers the edge. */
	select_mb(addr);
	memset(&edge, 0xaa, sizeof(edge));
	h264sw_intra_edge_chroma(&test_ctx, plane, &edge);

	/* Checks which parts are available; chroma has no top-right. */
	CHECK(edge.available == expected);

	/* Finds the block's upper-left sample in the plane. */
	ax = (addr % PIC_W) * 8U;
	ay = (addr / PIC_W) * 8U;

	/* Checks the samples of each part that is available. */
	left_ok = 1;
	top_ok = 1;
	corner_ok = 1;
	for (i = 0; i < 8U; i++) {
		if ((expected & H264SW_EDGE_LEFT) != 0U) {
			want = cb_value(ax - 1U, ay + i);
			if (plane == 2U)
				want = cr_value(ax - 1U, ay + i);
			if (edge.left[i] != want)
				left_ok = 0;
		}
		if ((expected & H264SW_EDGE_TOP) != 0U) {
			want = cb_value(ax + i, ay - 1U);
			if (plane == 2U)
				want = cr_value(ax + i, ay - 1U);
			if (edge.top[i] != want)
				top_ok = 0;
		}
	}
	if ((expected & H264SW_EDGE_TOP_LEFT) != 0U) {
		want = cb_value(ax - 1U, ay - 1U);
		if (plane == 2U)
			want = cr_value(ax - 1U, ay - 1U);
		if (edge.top_left != want)
			corner_ok = 0;
	}
	CHECK(left_ok);
	CHECK(top_ok);
	CHECK(corner_ok);
}

/* Checks the nine Intra_4x4 modes against the tables, the DC rules and the substitution of the top-right samples. */
static void
check_pred4x4(void)
{
	struct h264sw_intra_edge edge;
	uint8_t pred[16];
	int status;

	/* Predicts with every part available, in each mode, and compares with the worked-out tables. */
	make_edge4(&edge);
	status = h264sw_intra_pred4x4(H264SW_I4_VERTICAL, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_4x4_vertical, 16U));
	status = h264sw_intra_pred4x4(H264SW_I4_HORIZONTAL, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_4x4_horizontal, 16U));
	status = h264sw_intra_pred4x4(H264SW_I4_DIAGONAL_DOWN_LEFT, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_4x4_ddl, 16U));
	status = h264sw_intra_pred4x4(H264SW_I4_DIAGONAL_DOWN_RIGHT, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_4x4_ddr, 16U));
	status = h264sw_intra_pred4x4(H264SW_I4_VERTICAL_RIGHT, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_4x4_vr, 16U));
	status = h264sw_intra_pred4x4(H264SW_I4_HORIZONTAL_DOWN, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_4x4_hd, 16U));
	status = h264sw_intra_pred4x4(H264SW_I4_VERTICAL_LEFT, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_4x4_vl, 16U));
	status = h264sw_intra_pred4x4(H264SW_I4_HORIZONTAL_UP, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_4x4_hu, 16U));

	/* Predicts DC with both sides: ( 10 + 20 + 30 + 40 + 5 + 15 + 25 + 35 + 4 ) >> 3 = 23 (8-48). */
	status = h264sw_intra_pred4x4(H264SW_I4_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 16U, 23U));

	/* Predicts DC with the top only: ( 100 + 2 ) >> 2 = 25 (8-50). */
	edge.available = H264SW_EDGE_TOP;
	status = h264sw_intra_pred4x4(H264SW_I4_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 16U, 25U));

	/* Predicts DC with the left only: ( 80 + 2 ) >> 2 = 20 (8-49). */
	edge.available = H264SW_EDGE_LEFT;
	status = h264sw_intra_pred4x4(H264SW_I4_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 16U, 20U));

	/* Predicts DC with the top-left and top-right only: nothing it uses is available, so 128 (8-51). */
	edge.available = H264SW_EDGE_TOP_LEFT | H264SW_EDGE_TOP_RIGHT;
	status = h264sw_intra_pred4x4(H264SW_I4_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 16U, 128U));

	/* Predicts DC with the left and a top-right but no top: the top-right is not the top, so the left only counts (8-49). */
	edge.available = H264SW_EDGE_LEFT | H264SW_EDGE_TOP_RIGHT;
	status = h264sw_intra_pred4x4(H264SW_I4_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 16U, 20U));

	/* Substitutes p[ 3, -1 ] for the absent top-right samples (8.3.1.2): the garbage in top[4..7] must not matter. */
	make_edge4(&edge);
	edge.available = ALL_BUT_RIGHT;
	edge.top[4] = 250;
	edge.top[5] = 251;
	edge.top[6] = 252;
	edge.top[7] = 253;
	status = h264sw_intra_pred4x4(H264SW_I4_DIAGONAL_DOWN_LEFT, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_4x4_ddl_substituted, 16U));
	status = h264sw_intra_pred4x4(H264SW_I4_VERTICAL_LEFT, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_4x4_vl_substituted, 16U));

	/* Uses the top-right samples when they are available: the same garbage then matters. */
	edge.available = ALL_PARTS;
	status = h264sw_intra_pred4x4(H264SW_I4_DIAGONAL_DOWN_LEFT, &edge, pred);
	CHECK(status == 0);
	CHECK(pred[15] == (uint8_t)((252 + 3 * 253 + 2) >> 2));
}

/* Checks that each Intra_4x4 mode refuses an edge that lacks the samples it shall use, with a block of 128 (8.3.1.2.1 to 8.3.1.2.9). */
static void
check_pred4x4_requirements(void)
{
	static const unsigned top_only[3] = { H264SW_I4_VERTICAL, H264SW_I4_DIAGONAL_DOWN_LEFT, H264SW_I4_VERTICAL_LEFT };
	static const unsigned left_only[2] = { H264SW_I4_HORIZONTAL, H264SW_I4_HORIZONTAL_UP };
	static const unsigned corner_all[3] = { H264SW_I4_DIAGONAL_DOWN_RIGHT, H264SW_I4_VERTICAL_RIGHT, H264SW_I4_HORIZONTAL_DOWN };
	struct h264sw_intra_edge edge;
	uint8_t pred[16];
	unsigned i;
	int status;

	/* Offers no top: the modes that need it refuse. */
	make_edge4(&edge);
	edge.available = H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT;
	for (i = 0; i < 3U; i++) {
		memset(pred, 0x55, sizeof(pred));
		status = h264sw_intra_pred4x4(top_only[i], &edge, pred);
		CHECK(status == EINVAL);
		CHECK(all_equal(pred, 16U, 128U));
	}

	/* Offers no left: the modes that need it refuse. */
	edge.available = H264SW_EDGE_TOP | H264SW_EDGE_TOP_LEFT;
	for (i = 0; i < 2U; i++) {
		memset(pred, 0x55, sizeof(pred));
		status = h264sw_intra_pred4x4(left_only[i], &edge, pred);
		CHECK(status == EINVAL);
		CHECK(all_equal(pred, 16U, 128U));
	}

	/* Offers no top-left: the modes that need the three parts refuse. */
	edge.available = H264SW_EDGE_TOP | H264SW_EDGE_LEFT;
	for (i = 0; i < 3U; i++) {
		memset(pred, 0x55, sizeof(pred));
		status = h264sw_intra_pred4x4(corner_all[i], &edge, pred);
		CHECK(status == EINVAL);
		CHECK(all_equal(pred, 16U, 128U));
	}

	/* Offers no left: Diagonal_Down_Right refuses too, and no top: so does Horizontal_Down. */
	edge.available = H264SW_EDGE_TOP | H264SW_EDGE_TOP_LEFT;
	status = h264sw_intra_pred4x4(H264SW_I4_DIAGONAL_DOWN_RIGHT, &edge, pred);
	CHECK(status == EINVAL);
	edge.available = H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT;
	status = h264sw_intra_pred4x4(H264SW_I4_HORIZONTAL_DOWN, &edge, pred);
	CHECK(status == EINVAL);

	/* Offers a top-right but no top: Diagonal_Down_Left and Vertical_Left refuse, there is nothing to substitute from. */
	edge.available = H264SW_EDGE_LEFT | H264SW_EDGE_TOP_RIGHT;
	status = h264sw_intra_pred4x4(H264SW_I4_DIAGONAL_DOWN_LEFT, &edge, pred);
	CHECK(status == EINVAL);
	status = h264sw_intra_pred4x4(H264SW_I4_VERTICAL_LEFT, &edge, pred);
	CHECK(status == EINVAL);

	/* Offers nothing: DC still works, a block of 128. */
	edge.available = 0;
	status = h264sw_intra_pred4x4(H264SW_I4_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 16U, 128U));
}

/* Checks the nine Intra_8x8 modes against the tables, with the reference sample filtering visible in the vertical and horizontal modes. */
static void
check_pred8x8(void)
{
	struct h264sw_intra_edge edge;
	uint8_t pred[64];
	int status;

	/* Predicts with every part available, in each mode, and compares with the worked-out tables. */
	make_edge8(&edge);
	status = h264sw_intra_pred8x8(H264SW_I4_VERTICAL, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_8x8_vertical, 64U));
	status = h264sw_intra_pred8x8(H264SW_I4_HORIZONTAL, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_8x8_horizontal, 64U));
	status = h264sw_intra_pred8x8(H264SW_I4_DIAGONAL_DOWN_LEFT, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_8x8_ddl, 64U));
	status = h264sw_intra_pred8x8(H264SW_I4_DIAGONAL_DOWN_RIGHT, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_8x8_ddr, 64U));
	status = h264sw_intra_pred8x8(H264SW_I4_VERTICAL_RIGHT, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_8x8_vr, 64U));
	status = h264sw_intra_pred8x8(H264SW_I4_HORIZONTAL_DOWN, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_8x8_hd, 64U));
	status = h264sw_intra_pred8x8(H264SW_I4_VERTICAL_LEFT, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_8x8_vl, 64U));
	status = h264sw_intra_pred8x8(H264SW_I4_HORIZONTAL_UP, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_8x8_hu, 64U));

	/* Predicts DC with both sides: p' top sums to 390 and p' left to 544, ( 390 + 544 + 8 ) >> 4 = 58 (8-91). */
	status = h264sw_intra_pred8x8(H264SW_I4_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 64U, 58U));

	/* Predicts DC with the left and the top-left only: ( 544 + 4 ) >> 3 = 68 (8-92). */
	edge.available = H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT;
	status = h264sw_intra_pred8x8(H264SW_I4_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 64U, 68U));

	/* Predicts DC with the top and the top-left only: ( 390 + 4 ) >> 3 = 49 (8-93). */
	edge.available = H264SW_EDGE_TOP | H264SW_EDGE_TOP_LEFT | H264SW_EDGE_TOP_RIGHT;
	status = h264sw_intra_pred8x8(H264SW_I4_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 64U, 49U));

	/* Predicts DC with neither side: 128 (8-94). */
	edge.available = H264SW_EDGE_TOP_LEFT;
	status = h264sw_intra_pred8x8(H264SW_I4_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 64U, 128U));
}

/* Checks the Intra_8x8 reference sample filtering where the corner or the top-right is absent (8-79, 8-86, 8.3.2.2). */
static void
check_pred8x8_variants(void)
{
	static const uint8_t vertical_no_corner[8] = { 21, 25, 32, 41, 51, 61, 73, 88 };
	static const uint8_t horizontal_no_corner[8] = { 19, 27, 37, 53, 74, 93, 114, 128 };
	struct h264sw_intra_edge edge;
	uint8_t pred[64];
	int status;
	int rows_ok;
	int same;
	unsigned y;

	/* Predicts vertically without the corner: p'[ 0, -1 ] = ( 3 * 20 + 25 + 2 ) >> 2 = 21 (8-79), the others as before (8-80, 8-81). */
	make_edge8(&edge);
	edge.available = H264SW_EDGE_TOP | H264SW_EDGE_LEFT | H264SW_EDGE_TOP_RIGHT;
	status = h264sw_intra_pred8x8(H264SW_I4_VERTICAL, &edge, pred);
	CHECK(status == 0);
	rows_ok = 1;
	for (y = 0; y < 8U; y++) {
		same = same_samples(pred + y * 8U, vertical_no_corner, 8U);
		if (same == 0)
			rows_ok = 0;
	}
	CHECK(rows_ok);

	/* Predicts horizontally without the corner: p'[ -1, 0 ] = ( 3 * 15 + 30 + 2 ) >> 2 = 19 (8-86). */
	status = h264sw_intra_pred8x8(H264SW_I4_HORIZONTAL, &edge, pred);
	CHECK(status == 0);
	CHECK(pred[0] == horizontal_no_corner[0]);
	CHECK(pred[8] == horizontal_no_corner[1]);
	CHECK(pred[16] == horizontal_no_corner[2]);
	CHECK(pred[24] == horizontal_no_corner[3]);
	CHECK(pred[32] == horizontal_no_corner[4]);
	CHECK(pred[40] == horizontal_no_corner[5]);
	CHECK(pred[48] == horizontal_no_corner[6]);
	CHECK(pred[56] == horizontal_no_corner[7]);

	/* Substitutes p[ 7, -1 ] = 90 for the absent top-right samples before filtering, so p'[ 7, -1 ] = ( 70 + 2 * 90 + 90 + 2 ) >> 2 = 85 and the garbage in top[8..15] is not used. */
	make_edge8(&edge);
	edge.available = ALL_BUT_RIGHT;
	status = h264sw_intra_pred8x8(H264SW_I4_DIAGONAL_DOWN_LEFT, &edge, pred);
	CHECK(status == 0);
	CHECK(same_samples(pred, expect_8x8_ddl_substituted, 64U));

	/* Refuses a mode that needs a part the edge lacks, with a block of 128, and refuses a mode above 8. */
	edge.available = H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT;
	memset(pred, 0x55, sizeof(pred));
	status = h264sw_intra_pred8x8(H264SW_I4_VERTICAL, &edge, pred);
	CHECK(status == EINVAL);
	CHECK(all_equal(pred, 64U, 128U));
	edge.available = H264SW_EDGE_TOP | H264SW_EDGE_LEFT;
	memset(pred, 0x55, sizeof(pred));
	status = h264sw_intra_pred8x8(H264SW_I4_VERTICAL_RIGHT, &edge, pred);
	CHECK(status == EINVAL);
	CHECK(all_equal(pred, 64U, 128U));
	edge.available = ALL_PARTS;
	status = h264sw_intra_pred8x8(9U, &edge, pred);
	CHECK(status == EINVAL);
}

/* Checks the four Intra_16x16 modes other than plane: V, H and the DC rules (8-116 to 8-121). */
static void
check_pred16x16(void)
{
	struct h264sw_intra_edge edge;
	uint8_t pred[256];
	unsigned x;
	unsigned y;
	int status;
	int vertical_ok;
	int horizontal_ok;

	/* Predicts vertically: pred[ x, y ] = p[ x, -1 ] = 100 + 3 * x (8-116). */
	make_edge16(&edge, 100U, 3, 5, 90U);
	status = h264sw_intra_pred16x16(H264SW_I16_VERTICAL, &edge, pred);
	CHECK(status == 0);
	vertical_ok = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			if (pred[y * 16U + x] != 100U + 3U * x)
				vertical_ok = 0;
		}
	}
	CHECK(vertical_ok);

	/* Predicts horizontally: pred[ x, y ] = p[ -1, y ] = 100 + 5 * y (8-117). */
	status = h264sw_intra_pred16x16(H264SW_I16_HORIZONTAL, &edge, pred);
	CHECK(status == 0);
	horizontal_ok = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			if (pred[y * 16U + x] != 100U + 5U * y)
				horizontal_ok = 0;
		}
	}
	CHECK(horizontal_ok);

	/* Predicts DC with both sides: the top sums to 16 * 100 + 3 * 120 = 1960, the left to 1600 + 5 * 120 = 2200, ( 1960 + 2200 + 16 ) >> 5 = 130 (8-118). */
	status = h264sw_intra_pred16x16(H264SW_I16_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 256U, 130U));

	/* Predicts DC with the left only: ( 2200 + 8 ) >> 4 = 138 (8-119). */
	edge.available = H264SW_EDGE_LEFT;
	status = h264sw_intra_pred16x16(H264SW_I16_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 256U, 138U));

	/* Predicts DC with the top only: ( 1960 + 8 ) >> 4 = 123 (8-120). */
	edge.available = H264SW_EDGE_TOP;
	status = h264sw_intra_pred16x16(H264SW_I16_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 256U, 123U));

	/* Predicts DC with neither: 128 (8-121). */
	edge.available = H264SW_EDGE_TOP_LEFT;
	status = h264sw_intra_pred16x16(H264SW_I16_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 256U, 128U));

	/* Refuses a vertical mode without the top, a horizontal one without the left, and a mode above 3, with a block of 128. */
	edge.available = H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT;
	memset(pred, 0x55, sizeof(pred));
	status = h264sw_intra_pred16x16(H264SW_I16_VERTICAL, &edge, pred);
	CHECK(status == EINVAL);
	CHECK(all_equal(pred, 256U, 128U));
	edge.available = H264SW_EDGE_TOP | H264SW_EDGE_TOP_LEFT;
	status = h264sw_intra_pred16x16(H264SW_I16_HORIZONTAL, &edge, pred);
	CHECK(status == EINVAL);
	status = h264sw_intra_pred16x16(4U, &edge, pred);
	CHECK(status == EINVAL);
}

/*
 * Checks the Intra_16x16 plane mode at points worked out by hand (8-122 to 8-127).
 *
 * Case one: p[ x, -1 ] = 100 + 3 * x, p[ -1, y ] = 50 + 5 * y, p[ -1,
 * -1 ] = 90.  H = 1 * 6 + 2 * 12 + ... + 7 * 42 + 8 * ( 145 - 90 ) =
 * 840 + 440 = 1280 (the terms x' = 0..6 are ( x' + 1 ) * ( 6 + 6 * x' ),
 * the term x' = 7 uses p[ -1, -1 ]); V = 10 * 140 + 8 * ( 125 - 90 ) =
 * 1680; a = 16 * ( 125 + 145 ) = 4320; b = ( 5 * 1280 + 32 ) >> 6 = 100;
 * c = ( 5 * 1680 + 32 ) >> 6 = 131.  pred[ x, y ] = ( 4320 + 100 * ( x -
 * 7 ) + 131 * ( y - 7 ) + 16 ) >> 5: pred[ 0, 0 ] = 2719 >> 5 = 84,
 * pred[ 15, 0 ] = 4219 >> 5 = 131, pred[ 0, 15 ] = 4684 >> 5 = 146,
 * pred[ 15, 15 ] = 6184 >> 5 = 193, pred[ 7, 7 ] = 4336 >> 5 = 135.
 *
 * Case two climbs beyond the sample range: p[ x, -1 ] = p[ -1, y ] = 150 +
 * 7 * x, p[ -1, -1 ] = 140; H = V = 14 * 140 + 8 * ( 255 - 140 ) = 2880,
 * b = c = ( 5 * 2880 + 32 ) >> 6 = 225, a = 8160; pred[ 0, 0 ] = ( 8160
 * - 3150 + 16 ) >> 5 = 157 and pred[ 15, 15 ] = 11776 >> 5 = 368, clipped
 * to 255.
 *
 * Case three falls below zero: p[ x, -1 ] = p[ -1, y ] = 100 - 6 * x, p[
 * -1, -1 ] = 110; H = V = -12 * 140 + 8 * ( 10 - 110 ) = -2480, b = c =
 * ( -12400 + 32 ) >> 6 = -194 (rounded down), a = 320; pred[ 0, 0 ] =
 * ( 320 + 2716 + 16 ) >> 5 = 95, pred[ 0, 15 ] = ( 320 + 1358 - 1552 +
 * 16 ) >> 5 = 4, pred[ 15, 15 ] = -2768 >> 5 = -87, clipped to 0;
 * pred[ 6, 0 ] = ( 320 + 194 + 1358 + 16 ) >> 5 = 59 and pred[ 0, 3 ] =
 * ( 320 + 1358 + 776 + 16 ) >> 5 = 77 are the points where a gradient
 * rounded towards zero (-193) would give 58 and 76.
 */
static void
check_pred16x16_plane(void)
{
	struct h264sw_intra_edge edge;
	uint8_t pred[256];
	unsigned i;
	int status;

	/* Predicts case one, whose left column starts at 50 and so differs from the top row's base. */
	make_edge16(&edge, 100U, 3, 0, 90U);
	for (i = 0; i < 16U; i++)
		edge.left[i] = (uint8_t)(50U + 5U * i);
	status = h264sw_intra_pred16x16(H264SW_I16_PLANE, &edge, pred);
	CHECK(status == 0);
	CHECK(pred[0] == 84U);
	CHECK(pred[15] == 131U);
	CHECK(pred[15 * 16] == 146U);
	CHECK(pred[255] == 193U);
	CHECK(pred[7 * 16 + 7] == 135U);

	/* Predicts case two, whose far corner is clipped to 255. */
	make_edge16(&edge, 150U, 7, 7, 140U);
	status = h264sw_intra_pred16x16(H264SW_I16_PLANE, &edge, pred);
	CHECK(status == 0);
	CHECK(pred[0] == 157U);
	CHECK(pred[255] == 255U);

	/* Predicts case three, whose far corner is clipped to 0 and whose gradients are rounded down. */
	make_edge16(&edge, 100U, -6, -6, 110U);
	status = h264sw_intra_pred16x16(H264SW_I16_PLANE, &edge, pred);
	CHECK(status == 0);
	CHECK(pred[0] == 95U);
	CHECK(pred[15 * 16] == 4U);
	CHECK(pred[255] == 0U);
	CHECK(pred[6] == 59U);
	CHECK(pred[3 * 16] == 77U);

	/* Refuses the plane mode without the top-left, with a block of 128. */
	edge.available = H264SW_EDGE_TOP | H264SW_EDGE_LEFT;
	memset(pred, 0x55, sizeof(pred));
	status = h264sw_intra_pred16x16(H264SW_I16_PLANE, &edge, pred);
	CHECK(status == EINVAL);
	CHECK(all_equal(pred, 256U, 128U));
}

/* Checks the four chroma modes: DC with the rules of each 4x4 block's position, horizontal, vertical and plane (8-132 to 8-149). */
static void
check_pred_chroma(void)
{
	struct h264sw_intra_edge edge;
	uint8_t pred[64];
	uint8_t row[8];
	unsigned y;
	int status;
	int same;
	int vertical_ok;
	int horizontal_ok;

	/*
	 * Predicts DC with both sides.  The top is 40, 45, 50, 60 | 70, 85, 90,
	 * 100 and the left 30, 35, 45, 60 | 80, 100, 110, 120.  Block 0:
	 * ( 195 + 170 + 4 ) >> 3 = 46 (8-132); block 1 (upper right) takes the
	 * top, ( 345 + 2 ) >> 2 = 86 (8-136); block 2 (lower left) takes the
	 * left, ( 410 + 2 ) >> 2 = 103 (8-139); block 3: ( 345 + 410 + 4 ) >> 3
	 * = 94 (8-132).
	 */
	make_edge_chroma(&edge);
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(pred[0] == 46U && pred[3] == 46U && pred[3 * 8] == 46U && pred[3 * 8 + 3] == 46U);
	CHECK(pred[4] == 86U && pred[7] == 86U && pred[3 * 8 + 4] == 86U && pred[3 * 8 + 7] == 86U);
	CHECK(pred[4 * 8] == 103U && pred[4 * 8 + 3] == 103U && pred[7 * 8] == 103U && pred[7 * 8 + 3] == 103U);
	CHECK(pred[4 * 8 + 4] == 94U && pred[4 * 8 + 7] == 94U && pred[7 * 8 + 4] == 94U && pred[7 * 8 + 7] == 94U);

	/*
	 * Predicts DC with the top only: blocks 0 and 2 use the top's first
	 * four, ( 195 + 2 ) >> 2 = 49 (8-134, 8-140); blocks 1 and 3 the last
	 * four, 86 (8-136, 8-134).
	 */
	edge.available = H264SW_EDGE_TOP;
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(pred[0] == 49U && pred[4] == 86U && pred[4 * 8] == 49U && pred[4 * 8 + 4] == 86U);

	/*
	 * Predicts DC with the left only: blocks 0 and 1 use the left's first
	 * four, ( 170 + 2 ) >> 2 = 43 (8-133, 8-137); blocks 2 and 3 the last
	 * four, 103 (8-139, 8-133).
	 */
	edge.available = H264SW_EDGE_LEFT;
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(pred[0] == 43U && pred[4] == 43U && pred[4 * 8] == 103U && pred[4 * 8 + 4] == 103U);

	/* Predicts DC with neither: every block is 128 (8-135, 8-138, 8-141). */
	edge.available = H264SW_EDGE_TOP_LEFT;
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_DC, &edge, pred);
	CHECK(status == 0);
	CHECK(all_equal(pred, 64U, 128U));

	/* Predicts horizontally and vertically (8-142, 8-143). */
	make_edge_chroma(&edge);
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_HORIZONTAL, &edge, pred);
	CHECK(status == 0);
	horizontal_ok = 1;
	for (y = 0; y < 8U; y++) {
		memset(row, edge.left[y], sizeof(row));
		same = same_samples(pred + y * 8U, row, 8U);
		if (same == 0)
			horizontal_ok = 0;
	}
	CHECK(horizontal_ok);
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_VERTICAL, &edge, pred);
	CHECK(status == 0);
	vertical_ok = 1;
	for (y = 0; y < 8U; y++) {
		same = same_samples(pred + y * 8U, edge.top, 8U);
		if (same == 0)
			vertical_ok = 0;
	}
	CHECK(vertical_ok);

	/*
	 * Predicts the plane.  H = 1 * ( 70 - 50 ) + 2 * ( 85 - 45 ) + 3 * ( 90
	 * - 40 ) + 4 * ( 100 - 25 ) = 550, V = 1 * ( 80 - 45 ) + 2 * ( 100 -
	 * 35 ) + 3 * ( 110 - 30 ) + 4 * ( 120 - 25 ) = 785, b = ( 34 * 550 +
	 * 32 ) >> 6 = 292, c = ( 34 * 785 + 32 ) >> 6 = 417, a = 16 * ( 120 +
	 * 100 ) = 3520; pred[ x, y ] = ( 3520 + 292 * ( x - 3 ) + 417 * ( y - 3
	 * ) + 16 ) >> 5, so pred[ 0, 0 ] = 1409 >> 5 = 44, pred[ 3, 3 ] =
	 * 3536 >> 5 = 110, pred[ 7, 0 ] = 3453 >> 5 = 107 and pred[ 7, 7 ] =
	 * 6372 >> 5 = 199 (8-144 to 8-149).  The whole block is compared with
	 * the table of the same equations.
	 */
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_PLANE, &edge, pred);
	CHECK(status == 0);
	CHECK(pred[0] == 44U);
	CHECK(pred[3 * 8 + 3] == 110U);
	CHECK(pred[7] == 107U);
	CHECK(pred[63] == 199U);
	CHECK(same_samples(pred, expect_chroma_plane, 64U));

	/* Refuses each mode whose samples are absent, with a block of 128. */
	edge.available = H264SW_EDGE_TOP | H264SW_EDGE_LEFT;
	memset(pred, 0x55, sizeof(pred));
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_PLANE, &edge, pred);
	CHECK(status == EINVAL);
	CHECK(all_equal(pred, 64U, 128U));
	edge.available = H264SW_EDGE_TOP | H264SW_EDGE_TOP_LEFT;
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_HORIZONTAL, &edge, pred);
	CHECK(status == EINVAL);
	edge.available = H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT;
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_VERTICAL, &edge, pred);
	CHECK(status == EINVAL);
}

/* Checks that every predictor fails safely on a missing argument or a mode outside its table. */
static void
check_pred_errors(void)
{
	struct h264sw_intra_edge edge;
	uint8_t pred[256];
	int status;

	/* Refuses a missing output without touching anything. */
	make_edge4(&edge);
	status = h264sw_intra_pred4x4(H264SW_I4_DC, &edge, NULL);
	CHECK(status == EINVAL);
	status = h264sw_intra_pred8x8(H264SW_I4_DC, &edge, NULL);
	CHECK(status == EINVAL);
	status = h264sw_intra_pred16x16(H264SW_I16_DC, &edge, NULL);
	CHECK(status == EINVAL);
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_DC, &edge, NULL);
	CHECK(status == EINVAL);

	/* Refuses a missing edge, with a block of 128 of the block's size. */
	memset(pred, 0x55, sizeof(pred));
	status = h264sw_intra_pred4x4(H264SW_I4_DC, NULL, pred);
	CHECK(status == EINVAL);
	CHECK(all_equal(pred, 16U, 128U));
	memset(pred, 0x55, sizeof(pred));
	status = h264sw_intra_pred8x8(H264SW_I4_DC, NULL, pred);
	CHECK(status == EINVAL);
	CHECK(all_equal(pred, 64U, 128U));
	memset(pred, 0x55, sizeof(pred));
	status = h264sw_intra_pred16x16(H264SW_I16_DC, NULL, pred);
	CHECK(status == EINVAL);
	CHECK(all_equal(pred, 256U, 128U));
	memset(pred, 0x55, sizeof(pred));
	status = h264sw_intra_pred_chroma(H264SW_ICHROMA_DC, NULL, pred);
	CHECK(status == EINVAL);
	CHECK(all_equal(pred, 64U, 128U));

	/* Refuses a mode beyond the last one of its table, including a huge value, with a block of 128. */
	memset(pred, 0x55, sizeof(pred));
	status = h264sw_intra_pred4x4(9U, &edge, pred);
	CHECK(status == EINVAL);
	CHECK(all_equal(pred, 16U, 128U));
	status = h264sw_intra_pred4x4(0xffffffffU, &edge, pred);
	CHECK(status == EINVAL);
	status = h264sw_intra_pred8x8(0xffffffffU, &edge, pred);
	CHECK(status == EINVAL);
	status = h264sw_intra_pred16x16(0xffffffffU, &edge, pred);
	CHECK(status == EINVAL);
	status = h264sw_intra_pred_chroma(4U, &edge, pred);
	CHECK(status == EINVAL);

	/* Reads an edge whose available field has stray high bits as having only the bits it knows. */
	edge.available = 0xfffffff0U | H264SW_EDGE_TOP;
	status = h264sw_intra_pred4x4(H264SW_I4_VERTICAL, &edge, pred);
	CHECK(status == 0);
	CHECK(pred[0] == 10U && pred[15] == 40U);
}

/*
 * Checks Intra4x4PredMode (8.3.1.1) in the centre macroblock (address 4): A is the left macroblock (3), B the one above (1).
 */
static void
check_modes4x4(void)
{
	unsigned mode;

	/* Uses the smaller of the two neighbours' modes: A's block 5 (left of block 0) is 5, B's block 10 (above block 0) is 3, so the predicted mode is 3. */
	setup_picture();
	test_mbs[3].kind = H264SW_MB_I_4X4;
	test_mbs[3].intra4x4_mode[5] = 5;
	test_mbs[1].kind = H264SW_MB_I_4X4;
	test_mbs[1].intra4x4_mode[10] = 3;
	mode = h264sw_intra4x4_mode(&test_ctx, 0U, 1U, 0U);
	CHECK(mode == 3U);

	/* With the flag 0, a rem below the predicted mode is the mode: rem 0, 1, 2 give 0, 1, 2 (8-41). */
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 0U, 0U) == 0U);
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 0U, 1U) == 1U);
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 0U, 2U) == 2U);

	/* With the flag 0, a rem at or above the predicted mode is one more: rem 3 gives 4, rem 7 gives 8 (8-41). */
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 0U, 3U) == 4U);
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 0U, 7U) == 8U);

	/* Never gives a mode above 8, even for a rem the syntax cannot carry. */
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 0U, 8U) <= 8U);
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 0U, 0xffffffffU) <= 8U);

	/* Predicts DC when A is not available (the macroblock at the left edge, address 3): the predicted mode is 2 whatever B holds. */
	select_mb(3U);
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 1U, 0U) == 2U);
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 0U, 1U) == 1U);
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 0U, 2U) == 3U);

	/* Predicts DC when B is not available (the macroblock in the top row, address 1). */
	test_mbs[0].kind = H264SW_MB_I_4X4;
	test_mbs[0].intra4x4_mode[5] = 0;
	select_mb(1U);
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 1U, 0U) == 2U);

	/* Predicts DC when A is in another slice (not available, 6.4.8). */
	setup_picture();
	test_mbs[3].kind = H264SW_MB_I_4X4;
	test_mbs[3].intra4x4_mode[5] = 0;
	test_mbs[3].slice = 7;
	test_mbs[1].kind = H264SW_MB_I_4X4;
	test_mbs[1].intra4x4_mode[10] = 0;
	select_mb(4U);
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 1U, 0U) == 2U);

	/* Predicts DC for both when A is inter coded and constrained_intra_pred_flag is 1, though B is Intra_4x4 with mode 0 (8.3.1.1 step 2). */
	setup_picture();
	test_mbs[3].kind = H264SW_MB_P;
	test_mbs[1].kind = H264SW_MB_I_4X4;
	test_mbs[1].intra4x4_mode[10] = 0;
	test_pps.constrained_intra_pred = 1;
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 1U, 0U) == 2U);

	/* Takes B's mode 0 when the same A is inter coded but constrained_intra_pred_flag is 0: A counts as DC, so the minimum is 0 (8.3.1.1 step 3). */
	test_pps.constrained_intra_pred = 0;
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 1U, 0U) == 0U);

	/* Predicts DC for both when B is inter coded and constrained_intra_pred_flag is 1. */
	setup_picture();
	test_mbs[3].kind = H264SW_MB_I_4X4;
	test_mbs[3].intra4x4_mode[5] = 0;
	test_mbs[1].kind = H264SW_MB_B_SKIP;
	test_pps.constrained_intra_pred = 1;
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 1U, 0U) == 2U);

	/* Uses an Intra_4x4 neighbour in a P slice when constrained_intra_pred_flag is 1: Intra is not barred. */
	test_mbs[1].kind = H264SW_MB_I_4X4;
	test_mbs[1].intra4x4_mode[10] = 1;
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 1U, 0U) == 0U);

	/* Reads a neighbour that is not Intra_4x4 or Intra_8x8 as DC whatever its mode entries hold: Intra_16x16 with a stale 7, and I_PCM. */
	setup_picture();
	test_mbs[3].kind = H264SW_MB_I_16X16;
	test_mbs[3].intra4x4_mode[5] = 7;
	test_mbs[1].kind = H264SW_MB_I_4X4;
	test_mbs[1].intra4x4_mode[10] = 6;
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 1U, 0U) == 2U);
	test_mbs[3].kind = H264SW_MB_I_4X4;
	test_mbs[3].intra4x4_mode[5] = 7;
	test_mbs[1].kind = H264SW_MB_I_PCM;
	test_mbs[1].intra4x4_mode[10] = 7;
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 1U, 0U) == 2U);

	/* Reads an Intra_8x8 neighbour's mode from its 4x4 entries: A's four entries 4..7 hold 7 (8x8 block 1), B's 8..11 hold 4, so block 0 predicts 4 (8.3.1.1 step 3). */
	setup_picture();
	test_mbs[3].kind = H264SW_MB_I_8X8;
	test_mbs[3].intra4x4_mode[4] = 7;
	test_mbs[3].intra4x4_mode[5] = 7;
	test_mbs[3].intra4x4_mode[6] = 7;
	test_mbs[3].intra4x4_mode[7] = 7;
	test_mbs[1].kind = H264SW_MB_I_8X8;
	test_mbs[1].intra4x4_mode[8] = 4;
	test_mbs[1].intra4x4_mode[9] = 4;
	test_mbs[1].intra4x4_mode[10] = 4;
	test_mbs[1].intra4x4_mode[11] = 4;
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0U, 1U, 0U) == 4U);

	/* Takes both neighbours inside the current macroblock: block 3's A is block 2 and its B is block 1. */
	setup_picture();
	test_mbs[4].kind = H264SW_MB_I_4X4;
	test_mbs[4].intra4x4_mode[2] = 6;
	test_mbs[4].intra4x4_mode[1] = 4;
	CHECK(h264sw_intra4x4_mode(&test_ctx, 3U, 1U, 0U) == 4U);

	/* Takes one neighbour in the current macroblock and one outside: block 5's A is block 4 of the current macroblock, its B is block 15 of the macroblock above. */
	test_mbs[4].intra4x4_mode[4] = 8;
	test_mbs[1].kind = H264SW_MB_I_4X4;
	test_mbs[1].intra4x4_mode[15] = 5;
	CHECK(h264sw_intra4x4_mode(&test_ctx, 5U, 1U, 0U) == 5U);

	/* Falls back to DC for a missing context and a block that does not exist. */
	CHECK(h264sw_intra4x4_mode(NULL, 0U, 1U, 0U) == 2U);
	CHECK(h264sw_intra4x4_mode(&test_ctx, 16U, 1U, 0U) == 2U);
	CHECK(h264sw_intra4x4_mode(&test_ctx, 0xffffffffU, 0U, 3U) == 2U);
}

/*
 * Checks Intra8x8PredMode (8.3.2.1) in the centre macroblock: the Intra_4x4 neighbours' blocks 4 * luma8x8BlkIdxN + n with n 1 for A and 2 for B (8-72).
 */
static void
check_modes8x8(void)
{
	/* Reads an Intra_4x4 neighbour at n 1 for A and n 2 for B: A's 8x8 block 1 holds 4x4 blocks 4..7 = 0, 7, 1, 2 and B's 8x8 block 2 holds 8..11 = 1, 2, 3, 4, so block 0 takes A's entry 5 (7) and B's entry 10 (3). */
	setup_picture();
	test_mbs[3].kind = H264SW_MB_I_4X4;
	test_mbs[3].intra4x4_mode[4] = 0;
	test_mbs[3].intra4x4_mode[5] = 7;
	test_mbs[3].intra4x4_mode[6] = 1;
	test_mbs[3].intra4x4_mode[7] = 2;
	test_mbs[1].kind = H264SW_MB_I_4X4;
	test_mbs[1].intra4x4_mode[8] = 1;
	test_mbs[1].intra4x4_mode[9] = 2;
	test_mbs[1].intra4x4_mode[10] = 3;
	test_mbs[1].intra4x4_mode[11] = 4;
	CHECK(h264sw_intra8x8_mode(&test_ctx, 0U, 1U, 0U) == 3U);

	/* Applies the flag and the rem as the 4x4 derivation does: predicted 3, rem 2 gives 2, rem 3 gives 4 (8-73). */
	CHECK(h264sw_intra8x8_mode(&test_ctx, 0U, 0U, 2U) == 2U);
	CHECK(h264sw_intra8x8_mode(&test_ctx, 0U, 0U, 3U) == 4U);
	CHECK(h264sw_intra8x8_mode(&test_ctx, 0U, 0U, 7U) == 8U);
	CHECK(h264sw_intra8x8_mode(&test_ctx, 0U, 0U, 0xffffffffU) <= 8U);

	/* Reads an Intra_8x8 neighbour's 8x8 mode: A's block 1 is 5 and B's block 2 is 6, so block 0 predicts 5. */
	setup_picture();
	test_mbs[3].kind = H264SW_MB_I_8X8;
	test_mbs[3].intra4x4_mode[4] = 5;
	test_mbs[3].intra4x4_mode[5] = 5;
	test_mbs[3].intra4x4_mode[6] = 5;
	test_mbs[3].intra4x4_mode[7] = 5;
	test_mbs[1].kind = H264SW_MB_I_8X8;
	test_mbs[1].intra4x4_mode[8] = 6;
	test_mbs[1].intra4x4_mode[9] = 6;
	test_mbs[1].intra4x4_mode[10] = 6;
	test_mbs[1].intra4x4_mode[11] = 6;
	CHECK(h264sw_intra8x8_mode(&test_ctx, 0U, 1U, 0U) == 5U);

	/* Takes both neighbours inside an Intra_8x8 current macroblock: block 3's A is block 2 (4) and its B is block 1 (1). */
	test_mbs[4].kind = H264SW_MB_I_8X8;
	test_mbs[4].intra4x4_mode[8] = 4;
	test_mbs[4].intra4x4_mode[9] = 4;
	test_mbs[4].intra4x4_mode[10] = 4;
	test_mbs[4].intra4x4_mode[11] = 4;
	test_mbs[4].intra4x4_mode[4] = 1;
	test_mbs[4].intra4x4_mode[5] = 1;
	test_mbs[4].intra4x4_mode[6] = 1;
	test_mbs[4].intra4x4_mode[7] = 1;
	CHECK(h264sw_intra8x8_mode(&test_ctx, 3U, 1U, 0U) == 1U);

	/* Takes the 4x4 entries of a current macroblock coded in Intra_4x4 for blocks of its own: block 3's A is block 2, entry 9, and its B is block 1, entry 6. */
	test_mbs[4].kind = H264SW_MB_I_4X4;
	test_mbs[4].intra4x4_mode[9] = 8;
	test_mbs[4].intra4x4_mode[6] = 7;
	test_mbs[4].intra4x4_mode[8] = 0;
	test_mbs[4].intra4x4_mode[10] = 0;
	CHECK(h264sw_intra8x8_mode(&test_ctx, 3U, 1U, 0U) == 7U);

	/* Takes a neighbour in the macroblock above for block 1: its B is the 8x8 block 3 of the macroblock above. */
	setup_picture();
	test_mbs[1].kind = H264SW_MB_I_8X8;
	test_mbs[1].intra4x4_mode[12] = 3;
	test_mbs[1].intra4x4_mode[13] = 3;
	test_mbs[1].intra4x4_mode[14] = 3;
	test_mbs[1].intra4x4_mode[15] = 3;
	test_mbs[4].kind = H264SW_MB_I_8X8;
	test_mbs[4].intra4x4_mode[0] = 6;
	test_mbs[4].intra4x4_mode[1] = 6;
	test_mbs[4].intra4x4_mode[2] = 6;
	test_mbs[4].intra4x4_mode[3] = 6;
	CHECK(h264sw_intra8x8_mode(&test_ctx, 1U, 1U, 0U) == 3U);

	/* Predicts DC when a neighbour is not available, or inter coded under constrained_intra_pred_flag 1, and reads a barred-free inter neighbour as DC. */
	select_mb(3U);
	CHECK(h264sw_intra8x8_mode(&test_ctx, 0U, 1U, 0U) == 2U);
	select_mb(1U);
	CHECK(h264sw_intra8x8_mode(&test_ctx, 0U, 1U, 0U) == 2U);
	setup_picture();
	test_mbs[3].kind = H264SW_MB_P_8X8;
	test_mbs[1].kind = H264SW_MB_I_8X8;
	test_mbs[1].intra4x4_mode[8] = 0;
	test_mbs[1].intra4x4_mode[9] = 0;
	test_mbs[1].intra4x4_mode[10] = 0;
	test_mbs[1].intra4x4_mode[11] = 0;
	test_pps.constrained_intra_pred = 1;
	CHECK(h264sw_intra8x8_mode(&test_ctx, 0U, 1U, 0U) == 2U);
	test_pps.constrained_intra_pred = 0;
	CHECK(h264sw_intra8x8_mode(&test_ctx, 0U, 1U, 0U) == 0U);

	/* Falls back to DC for a missing context and a block that does not exist. */
	CHECK(h264sw_intra8x8_mode(NULL, 0U, 1U, 0U) == 2U);
	CHECK(h264sw_intra8x8_mode(&test_ctx, 4U, 1U, 0U) == 2U);
}

/*
 * Checks the gathering of the luma 4x4 edges (8.3.1.2) around the centre macroblock and the corners, slice borders and Inter neighbours.
 */
static void
check_edge4x4(void)
{
	unsigned blk;
	unsigned expected;

	/* Gathers every 4x4 block of the centre macroblock: all have left, top and top-left; the top-right is missing for blocks 3 and 11 (later blocks, 8.3.1.2) and 7, 13 and 15 (right of the macroblock below its top row, Table 6-3) and present for block 5 (macroblock C). */
	setup_picture();
	for (blk = 0; blk < 16U; blk++) {
		expected = ALL_PARTS;
		if (blk == 3U || blk == 7U || blk == 11U || blk == 13U || blk == 15U)
			expected = ALL_BUT_RIGHT;
		check_luma_edge(4U, 4U, blk, expected);
	}

	/* Gathers the blocks of the macroblock at the right edge (address 5): block 5 has no macroblock C, so no top-right; block 1 and 4 take theirs from B. */
	check_luma_edge(5U, 4U, 5U, ALL_BUT_RIGHT);
	check_luma_edge(5U, 4U, 1U, ALL_PARTS);
	check_luma_edge(5U, 4U, 4U, ALL_PARTS);

	/* Gathers the blocks of the macroblock at the left edge (address 3): the blocks in its first column have no left, and those at its top also no top-left; block 2's top-left would be in A. */
	check_luma_edge(3U, 4U, 0U, H264SW_EDGE_TOP | H264SW_EDGE_TOP_RIGHT);
	check_luma_edge(3U, 4U, 2U, H264SW_EDGE_TOP | H264SW_EDGE_TOP_RIGHT);
	check_luma_edge(3U, 4U, 8U, H264SW_EDGE_TOP | H264SW_EDGE_TOP_RIGHT);
	check_luma_edge(3U, 4U, 1U, ALL_PARTS);

	/* Gathers the blocks of the macroblock in the top row (address 1): the blocks in its first row have no top, top-left or top-right; block 2's top-left is in A (the left macroblock, address 0) and its top-right in the finished block 1. */
	check_luma_edge(1U, 4U, 0U, H264SW_EDGE_LEFT);
	check_luma_edge(1U, 4U, 1U, H264SW_EDGE_LEFT);
	check_luma_edge(1U, 4U, 4U, H264SW_EDGE_LEFT);
	check_luma_edge(1U, 4U, 5U, H264SW_EDGE_LEFT);
	check_luma_edge(1U, 4U, 2U, ALL_PARTS);
	check_luma_edge(1U, 4U, 6U, ALL_PARTS);

	/* Gathers the corner macroblock (address 0): block 0 has nothing; block 3 has left, top and top-left but no top-right. */
	check_luma_edge(0U, 4U, 0U, 0U);
	check_luma_edge(0U, 4U, 3U, ALL_BUT_RIGHT);

	/* Leaves out the macroblock above when it is in another slice: block 0 keeps its left and its top-left (macroblock D), loses its top and top-right; block 5 keeps the top-right of macroblock C and the left but loses the top and the top-left. */
	setup_picture();
	test_mbs[1].slice = 5;
	check_luma_edge(4U, 4U, 0U, H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT);
	check_luma_edge(4U, 4U, 5U, H264SW_EDGE_LEFT | H264SW_EDGE_TOP_RIGHT);

	/* Leaves out the left macroblock when it is Inter coded and constrained_intra_pred_flag is 1: block 0 loses its left, block 2 its left and top-left (macroblock A), block 1 loses nothing; the top-left of block 0 is in D, which is Intra. */
	setup_picture();
	test_mbs[3].kind = H264SW_MB_P;
	test_pps.constrained_intra_pred = 1;
	check_luma_edge(4U, 4U, 0U, H264SW_EDGE_TOP | H264SW_EDGE_TOP_LEFT | H264SW_EDGE_TOP_RIGHT);
	check_luma_edge(4U, 4U, 2U, H264SW_EDGE_TOP | H264SW_EDGE_TOP_RIGHT);
	check_luma_edge(4U, 4U, 1U, ALL_PARTS);

	/* Uses the same macroblock when constrained_intra_pred_flag is 0. */
	test_pps.constrained_intra_pred = 0;
	check_luma_edge(4U, 4U, 0U, ALL_PARTS);
	check_luma_edge(4U, 4U, 2U, ALL_PARTS);

	/* Leaves out the macroblock above and to the right when it is Inter coded under constrained_intra_pred_flag 1: block 5 loses its top-right; block 1's top-right is in B and stays. */
	setup_picture();
	test_mbs[2].kind = H264SW_MB_B_8X8;
	test_pps.constrained_intra_pred = 1;
	check_luma_edge(4U, 4U, 5U, ALL_BUT_RIGHT);
	check_luma_edge(4U, 4U, 1U, ALL_PARTS);

	/* Leaves out the macroblock above-left when it is Inter coded under constrained_intra_pred_flag 1: block 0 loses its top-left only. */
	setup_picture();
	test_mbs[0].kind = H264SW_MB_P_SKIP;
	test_pps.constrained_intra_pred = 1;
	check_luma_edge(4U, 4U, 0U, H264SW_EDGE_LEFT | H264SW_EDGE_TOP | H264SW_EDGE_TOP_RIGHT);

	/* Leaves out a run that would leave the frame's plane (a frame smaller than the macroblock array says): the macroblock at the right edge reads nothing. */
	setup_picture();
	test_frame.width = 20U;
	check_luma_edge(5U, 4U, 0U, 0U);
}

/* Checks the gathering of the luma 8x8 edges (8.3.2.2): the top-right of each block of the centre macroblock. */
static void
check_edge8x8(void)
{
	/* Gathers the four 8x8 blocks of the centre macroblock: block 0's top-right is in B, block 1's in C, block 2's is the finished block 1, block 3's is right of the macroblock and never available. */
	setup_picture();
	check_luma_edge(4U, 8U, 0U, ALL_PARTS);
	check_luma_edge(4U, 8U, 1U, ALL_PARTS);
	check_luma_edge(4U, 8U, 2U, ALL_PARTS);
	check_luma_edge(4U, 8U, 3U, ALL_BUT_RIGHT);

	/* Takes block 1's top-right from the macroblock above and to the right: not at the right edge (address 5). */
	check_luma_edge(5U, 8U, 1U, ALL_BUT_RIGHT);
	check_luma_edge(5U, 8U, 0U, ALL_PARTS);

	/* Gathers at the corner macroblock: block 0 has nothing, block 3 has all but the top-right, block 2 has its top-right in block 1. */
	check_luma_edge(0U, 8U, 0U, 0U);
	check_luma_edge(0U, 8U, 3U, ALL_BUT_RIGHT);
	check_luma_edge(0U, 8U, 2U, H264SW_EDGE_TOP | H264SW_EDGE_TOP_RIGHT);

	/* Leaves out an Inter macroblock when constrained_intra_pred_flag is 1: macroblock A for block 0 loses the left, macroblock C for block 1 the top-right. */
	setup_picture();
	test_mbs[3].kind = H264SW_MB_P_8X8REF0;
	test_mbs[2].kind = H264SW_MB_P_8X8;
	test_pps.constrained_intra_pred = 1;
	check_luma_edge(4U, 8U, 0U, H264SW_EDGE_TOP | H264SW_EDGE_TOP_LEFT | H264SW_EDGE_TOP_RIGHT);
	check_luma_edge(4U, 8U, 1U, ALL_BUT_RIGHT);
	check_luma_edge(4U, 8U, 2U, H264SW_EDGE_TOP | H264SW_EDGE_TOP_RIGHT);
}

/* Checks the gathering of the 16x16 luma edge (8.3.3) and of the chroma edges (8.3.4). */
static void
check_edge16_and_chroma(void)
{
	/* Gathers the 16x16 edge of the centre macroblock: left, top and top-left, never a top-right; the block index is ignored. */
	setup_picture();
	check_luma_edge(4U, 16U, 0U, ALL_BUT_RIGHT);
	check_luma_edge(4U, 16U, 3U, ALL_BUT_RIGHT);

	/* Gathers at the borders: the left edge loses the left and top-left, the top row loses the top and top-left, the corner has nothing. */
	check_luma_edge(3U, 16U, 0U, H264SW_EDGE_TOP);
	check_luma_edge(1U, 16U, 0U, H264SW_EDGE_LEFT);
	check_luma_edge(0U, 16U, 0U, 0U);
	check_luma_edge(8U, 16U, 0U, ALL_BUT_RIGHT);

	/* Gathers the chroma edges of both planes at the centre and at the borders. */
	check_chroma_edge(4U, 1U, ALL_BUT_RIGHT);
	check_chroma_edge(4U, 2U, ALL_BUT_RIGHT);
	check_chroma_edge(3U, 1U, H264SW_EDGE_TOP);
	check_chroma_edge(1U, 2U, H264SW_EDGE_LEFT);
	check_chroma_edge(0U, 1U, 0U);

	/* Leaves out the Inter macroblocks under constrained_intra_pred_flag 1: A for the left, B for the top, D for the top-left. */
	setup_picture();
	test_pps.constrained_intra_pred = 1;
	test_mbs[3].kind = H264SW_MB_P;
	check_luma_edge(4U, 16U, 0U, H264SW_EDGE_TOP | H264SW_EDGE_TOP_LEFT);
	check_chroma_edge(4U, 1U, H264SW_EDGE_TOP | H264SW_EDGE_TOP_LEFT);
	test_mbs[3].kind = H264SW_MB_I_16X16;
	test_mbs[1].kind = H264SW_MB_B;
	check_luma_edge(4U, 16U, 0U, H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT);
	check_chroma_edge(4U, 2U, H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT);
	test_mbs[1].kind = H264SW_MB_I_16X16;
	test_mbs[0].kind = H264SW_MB_P_SKIP;
	check_luma_edge(4U, 16U, 0U, H264SW_EDGE_LEFT | H264SW_EDGE_TOP);
	check_chroma_edge(4U, 1U, H264SW_EDGE_LEFT | H264SW_EDGE_TOP);

	/* Uses them all when the flag is 0. */
	test_pps.constrained_intra_pred = 0;
	check_luma_edge(4U, 16U, 0U, ALL_BUT_RIGHT);
	check_chroma_edge(4U, 1U, ALL_BUT_RIGHT);

	/* Leaves out the macroblock above when it is in another slice. */
	test_mbs[1].slice = 3;
	check_luma_edge(4U, 16U, 0U, H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT);
	check_chroma_edge(4U, 1U, H264SW_EDGE_LEFT | H264SW_EDGE_TOP_LEFT);
}

/* Checks that the gatherers fail safely on a missing argument, a size or index that does not exist and a picture without a frame. */
static void
check_edge_errors(void)
{
	struct h264sw_intra_edge edge;

	/* Leaves nothing available for a missing context. */
	setup_picture();
	memset(&edge, 0xaa, sizeof(edge));
	h264sw_intra_edge_luma(NULL, 4U, 0U, &edge);
	CHECK(edge.available == 0U);
	memset(&edge, 0xaa, sizeof(edge));
	h264sw_intra_edge_chroma(NULL, 1U, &edge);
	CHECK(edge.available == 0U);

	/* Leaves nothing available for a size luma blocks do not have and for block indices beyond their size. */
	memset(&edge, 0xaa, sizeof(edge));
	h264sw_intra_edge_luma(&test_ctx, 5U, 0U, &edge);
	CHECK(edge.available == 0U);
	memset(&edge, 0xaa, sizeof(edge));
	h264sw_intra_edge_luma(&test_ctx, 4U, 16U, &edge);
	CHECK(edge.available == 0U);
	memset(&edge, 0xaa, sizeof(edge));
	h264sw_intra_edge_luma(&test_ctx, 8U, 4U, &edge);
	CHECK(edge.available == 0U);
	memset(&edge, 0xaa, sizeof(edge));
	h264sw_intra_edge_luma(&test_ctx, 0U, 0U, &edge);
	CHECK(edge.available == 0U);

	/* Leaves nothing available for a plane 4:2:0 does not have. */
	memset(&edge, 0xaa, sizeof(edge));
	h264sw_intra_edge_chroma(&test_ctx, 0U, &edge);
	CHECK(edge.available == 0U);
	memset(&edge, 0xaa, sizeof(edge));
	h264sw_intra_edge_chroma(&test_ctx, 3U, &edge);
	CHECK(edge.available == 0U);

	/* Leaves nothing available for a picture without a frame, and for a frame without a plane. */
	memset(&edge, 0xaa, sizeof(edge));
	test_picture.frame = NULL;
	h264sw_intra_edge_luma(&test_ctx, 16U, 0U, &edge);
	CHECK(edge.available == 0U);
	test_picture.frame = &test_frame;
	test_frame.plane[1] = NULL;
	memset(&edge, 0xaa, sizeof(edge));
	h264sw_intra_edge_chroma(&test_ctx, 1U, &edge);
	CHECK(edge.available == 0U);

	/* Does nothing, and does not fail, for a missing edge; the next gathering still works. */
	h264sw_intra_edge_luma(&test_ctx, 4U, 0U, NULL);
	h264sw_intra_edge_chroma(&test_ctx, 1U, NULL);
	test_frame.plane[1] = test_cb;
	check_chroma_edge(4U, 1U, ALL_BUT_RIGHT);
}
