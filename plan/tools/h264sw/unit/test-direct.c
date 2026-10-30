/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the direct prediction of B slices of the software H.264
 * decoder (8.4.1.2): DistScaleFactor (8-197, 8-198, 8-201, 8-202), the
 * temporal mode (8-191 to 8-200), the spatial mode (8-184 to 8-190 and
 * colZeroFlag), the co-located 4x4 block with and without
 * direct_8x8_inference_flag (8.4.1.2.1) and the error answers.  The
 * expected values are worked out by hand from those equations; the
 * macroblock under test is address 4 of a 3 by 3 picture, whose
 * neighbours A, B, C and D are addresses 3, 1, 2 and 0.
 */

#include <errno.h>
#include <string.h>

#include "check.h"
#include "userland/desktop/libmedia/h264sw-context.h"
#include "userland/desktop/libmedia/h264sw-direct.h"

/* The picture of the tests: 3 by 3 macroblocks, the middle one (address 4) is the current macroblock. */
#define PIC_W 3U
#define PIC_H 3U
#define CURRENT 4U

/* The ids of the frames the lists hold: two in list 0, the co-located frame in list 1. */
#define ID_REF_A 11U
#define ID_REF_B 12U
#define ID_COL 21U

/*
 * The slice context, the picture, the parameter set, the header, the
 * macroblocks and the three frames of the tests.  They are big, so they
 * live for the whole run of the program and every test rebuilds what
 * it needs with begin().
 */
static struct h264sw_slice_ctx ctx;
static struct h264sw_picture_ctx picture;
static struct h264sw_sps sps;
static struct h264sw_slice_header header;
static struct h264sw_mb mbs[PIC_W * PIC_H];
static struct h264sw_frame frame_a;
static struct h264sw_frame frame_b;
static struct h264sw_frame frame_col;
static struct h264sw_motion col_motion[PIC_W * PIC_H];

/* Forward declarations of the static functions. */
static void begin(int spatial, int inference);
static void set_lists(int32_t poc_a, int32_t poc_b, int32_t poc_col);
static void col_set_all(unsigned quadrant, int list, int ref, uint32_t ref_id, int x, int y);
static void col_set_block(unsigned blk, int list, int x, int y);
static void mb_set(unsigned mb, unsigned list, int ref, int x, int y);
static void mb_set_intra(unsigned mb);
static int quadrant_mv_is(unsigned list, unsigned quadrant, int x, int y);
static int quadrant_ref_is(unsigned list, unsigned quadrant, int ref, uint32_t ref_id);
static void check_setup(void);
static void check_temporal(void);
static void check_temporal_long_term(void);
static void check_temporal_inference(void);
static void check_temporal_range(void);
static void check_spatial_refs(void);
static void check_spatial_col_zero(void);
static void check_spatial_inference(void);
static void check_spatial_cache(void);
static void check_direct_bits(void);
static void check_errors(void);

int
main(void)
{
	int status;

	/* Runs every group of checks. */
	check_setup();
	check_temporal();
	check_temporal_long_term();
	check_temporal_inference();
	check_temporal_range();
	check_spatial_refs();
	check_spatial_col_zero();
	check_spatial_inference();
	check_spatial_cache();
	check_direct_bits();
	check_errors();

	/* Reports the outcome as the one last line. */
	status = check_finish("direct");

	/* The exit status says whether every check held. */
	return status;
}

/*
 * Rebuilds the context: every macroblock is reset in slice 0, the
 * co-located motion is all intra, the lists hold the frames A (POC 0),
 * B (POC 2) and the co-located frame (POC 8) for the current picture of
 * POC 4, and direct_spatial_mv_pred_flag and direct_8x8_inference_flag
 * are the given ones.
 */
static void
begin(
	int spatial,
	int inference)
{
	unsigned i;

	/* Resets every macroblock to the state before it is decoded, in slice 0. */
	for (i = 0; i < PIC_W * PIC_H; i++)
		h264sw_mb_reset(&mbs[i], 0);

	/* Makes the motion of the co-located frame all intra, as a frame with no decoded inter macroblock. */
	memset(col_motion, 0, sizeof(col_motion));
	for (i = 0; i < PIC_W * PIC_H; i++) {
		memset(col_motion[i].ref_idx, 0xff, sizeof(col_motion[i].ref_idx));
		col_motion[i].intra = 1;
	}

	/* Makes the three frames and gives them their ids. */
	memset(&frame_a, 0, sizeof(frame_a));
	memset(&frame_b, 0, sizeof(frame_b));
	memset(&frame_col, 0, sizeof(frame_col));
	frame_a.id = ID_REF_A;
	frame_b.id = ID_REF_B;
	frame_col.id = ID_COL;
	frame_col.motion = col_motion;
	frame_col.size_mbs = PIC_W * PIC_H;

	/* Clears the contexts and points them at each other. */
	memset(&ctx, 0, sizeof(ctx));
	memset(&picture, 0, sizeof(picture));
	memset(&sps, 0, sizeof(sps));
	memset(&header, 0, sizeof(header));
	sps.direct_8x8_inference = inference;
	header.direct_spatial = spatial;
	picture.mbs = mbs;
	picture.sps = &sps;
	picture.width_mbs = PIC_W;
	picture.height_mbs = PIC_H;
	picture.size_mbs = PIC_W * PIC_H;
	ctx.picture = &picture;
	ctx.header = &header;
	ctx.mb_addr = CURRENT;

	/* Finds the neighbouring macroblocks of the current one. */
	h264sw_mb_neighbours(mbs, PIC_W, CURRENT, &ctx.n);

	/* Fills the lists for the current picture of POC 4 and prepares the slice. */
	set_lists(0, 2, 8);
}

/* Fills list 0 with the frames A and B (short-term) and list 1 with the co-located frame, and prepares the slice for the current picture of POC 4. */
static void
set_lists(
	int32_t poc_a,
	int32_t poc_b,
	int32_t poc_col)
{
	/* Fills the lists. */
	memset(&ctx.lists, 0, sizeof(ctx.lists));
	ctx.lists.count[0] = 2;
	ctx.lists.count[1] = 1;
	ctx.lists.list[0][0].frame = &frame_a;
	ctx.lists.list[0][0].poc = poc_a;
	ctx.lists.list[0][1].frame = &frame_b;
	ctx.lists.list[0][1].poc = poc_b;
	ctx.lists.list[1][0].frame = &frame_col;
	ctx.lists.list[1][0].poc = poc_col;

	/* Prepares the direct prediction. */
	h264sw_direct_setup(&ctx.direct, &ctx.lists, 4);
}

/* Sets the co-located motion of the current macroblock's quadrant: one list, a reference index and frame, and one vector for its four blocks. */
static void
col_set_all(
	unsigned quadrant,
	int list,
	int ref,
	uint32_t ref_id,
	int x,
	int y)
{
	unsigned sub;

	/* Records the reference of the quadrant and clears the other list's. */
	col_motion[CURRENT].intra = 0;
	col_motion[CURRENT].ref_idx[list][quadrant] = (int8_t)ref;
	col_motion[CURRENT].ref_id[list][quadrant] = ref_id;
	col_motion[CURRENT].ref_idx[1 - list][quadrant] = -1;
	col_motion[CURRENT].ref_id[1 - list][quadrant] = 0;

	/* Gives the quadrant's four blocks the vector. */
	for (sub = 0; sub < 4U; sub++) {
		col_motion[CURRENT].mv[list][4U * quadrant + sub][0] = (int16_t)x;
		col_motion[CURRENT].mv[list][4U * quadrant + sub][1] = (int16_t)y;
	}
}

/* Sets the co-located vector of one 4x4 block. */
static void
col_set_block(
	unsigned blk,
	int list,
	int x,
	int y)
{
	/* Stores the vector of the block. */
	col_motion[CURRENT].mv[list][blk][0] = (int16_t)x;
	col_motion[CURRENT].mv[list][blk][1] = (int16_t)y;
}

/* Gives a neighbouring macroblock one reference index and one vector in a list for all its blocks, and no reference in the other list. */
static void
mb_set(
	unsigned mb,
	unsigned list,
	int ref,
	int x,
	int y)
{
	unsigned blk;

	/* Marks the macroblock as inter, and sets the reference index of every quadrant. */
	mbs[mb].motion.intra = 0;
	for (blk = 0; blk < 4U; blk++)
		mbs[mb].motion.ref_idx[list][blk] = (int8_t)ref;

	/* Sets the vector of every block. */
	for (blk = 0; blk < 16U; blk++) {
		mbs[mb].motion.mv[list][blk][0] = (int16_t)x;
		mbs[mb].motion.mv[list][blk][1] = (int16_t)y;
	}
}

/* Marks a macroblock as coded in an intra mode. */
static void
mb_set_intra(
	unsigned mb)
{
	/* Intra macroblocks use no list. */
	mbs[mb].motion.intra = 1;
}

/* Tells whether the four blocks of a quadrant of the current macroblock all hold the vector. */
static int
quadrant_mv_is(
	unsigned list,
	unsigned quadrant,
	int x,
	int y)
{
	unsigned sub;
	const int16_t *mv;

	/* The quadrant differs when any block differs. */
	for (sub = 0; sub < 4U; sub++) {
		mv = mbs[CURRENT].motion.mv[list][4U * quadrant + sub];
		if (mv[0] != x)
			return 0;
		if (mv[1] != y)
			return 0;
	}

	/* All four blocks hold the vector. */
	return 1;
}

/* Tells whether a quadrant of the current macroblock holds a reference index and the id of its frame. */
static int
quadrant_ref_is(
	unsigned list,
	unsigned quadrant,
	int ref,
	uint32_t ref_id)
{
	/* The index differs. */
	if (mbs[CURRENT].motion.ref_idx[list][quadrant] != ref)
		return 0;

	/* The frame differs. */
	if (mbs[CURRENT].motion.ref_id[list][quadrant] != ref_id)
		return 0;

	/* Both equal. */
	return 1;
}

/*
 * Checks DistScaleFactor and the entries that take the co-located vector unscaled.
 *
 * Values by hand, with tb = POC( current ) - POC( pic0 ) and td =
 * POC( pic1 ) - POC( pic0 ), tx = ( 16384 + Abs( td / 2 ) ) / td and
 * DistScaleFactor = Clip3( -1024, 1023, ( tb * tx + 32 ) >> 6 ):
 * - POC 4, pic0 0, pic1 8: tb 4, td 8, tx 2048, ( 8192 + 32 ) >> 6 = 128;
 * - POC 4, pic0 2, pic1 8: tb 2, td 6, tx 2731, ( 5462 + 32 ) >> 6 = 85;
 * - POC 4, pic0 6, pic1 8: tb -2, td 2, tx 8192, ( -16384 + 32 ) >> 6 =
 *   floor( -255.5 ) = -256;
 * - POC 4, pic0 12, pic1 8: tb -8, td -4, tx -4096, ( 32768 + 32 ) >> 6 = 512;
 * - POC 200, pic0 0, pic1 1: tb clipped to 127, td 1, tx 16384, clipped to 1023.
 */
static void
check_setup(void)
{
	struct h264sw_reflist lists;
	struct h264sw_direct direct;

	/* A list 1 with one entry and a list 0 with four short-term entries. */
	memset(&lists, 0, sizeof(lists));
	lists.count[0] = 4;
	lists.count[1] = 1;
	lists.list[0][0].frame = &frame_a;
	lists.list[0][0].poc = 0;
	lists.list[0][1].frame = &frame_b;
	lists.list[0][1].poc = 2;
	lists.list[0][2].frame = &frame_a;
	lists.list[0][2].poc = 6;
	lists.list[0][3].frame = &frame_b;
	lists.list[0][3].poc = 12;
	lists.list[1][0].frame = &frame_col;
	lists.list[1][0].poc = 8;
	memset(&direct, 0xaa, sizeof(direct));
	h264sw_direct_setup(&direct, &lists, 4);

	/* The co-located frame is the first entry of list 1, short-term. */
	CHECK(direct.col == &frame_col);
	CHECK(direct.col_long_term == 0);

	/* The factors of the four entries. */
	CHECK(direct.dist_scale[0] == 128);
	CHECK(direct.dist_scale[1] == 85);
	CHECK(direct.dist_scale[2] == -256);
	CHECK(direct.dist_scale[3] == 512);
	CHECK(direct.long_term0[0] == 0);
	CHECK(direct.long_term0[1] == 0);
	CHECK(direct.long_term0[2] == 0);
	CHECK(direct.long_term0[3] == 0);

	/* Nothing is derived for the current macroblock yet. */
	CHECK(direct.spatial_ready == 0);

	/* A long-term entry of list 0 takes the vector unscaled. */
	lists.list[0][1].long_term = 1;
	h264sw_direct_setup(&direct, &lists, 4);
	CHECK(direct.long_term0[1] == 1);
	CHECK(direct.long_term0[0] == 0);

	/* A long-term list 1 entry is remembered for colZeroFlag. */
	lists.list[1][0].long_term = 1;
	h264sw_direct_setup(&direct, &lists, 4);
	CHECK(direct.col_long_term == 1);
	lists.list[1][0].long_term = 0;

	/* A distance of 0 between pic1 and pic0 takes the vector unscaled and does not divide by 0. */
	lists.list[0][1].long_term = 0;
	lists.list[0][1].poc = 8;
	h264sw_direct_setup(&direct, &lists, 4);
	CHECK(direct.long_term0[1] == 1);
	CHECK(direct.long_term0[0] == 0);

	/* The clip of tb and of the factor. */
	lists.list[0][0].poc = 0;
	lists.list[1][0].poc = 1;
	h264sw_direct_setup(&direct, &lists, 200);
	CHECK(direct.dist_scale[0] == 1023);

	/* The clip of td: POC 1000, pic0 0, pic1 2000: tb 127, td 127, tx 129, ( 16383 + 32 ) >> 6 = 256. */
	lists.list[1][0].poc = 2000;
	h264sw_direct_setup(&direct, &lists, 1000);
	CHECK(direct.dist_scale[0] == 256);

	/* Extreme POC values do not overflow: the differences are clipped. */
	lists.list[0][0].poc = (int32_t)(-2147483647 - 1);
	lists.list[1][0].poc = 2147483647;
	h264sw_direct_setup(&direct, &lists, 2147483647);
	CHECK(direct.dist_scale[0] >= -1024);
	CHECK(direct.dist_scale[0] <= 1023);

	/* A list 1 without an entry leaves no co-located frame, and a missing structure or list is ignored. */
	lists.count[1] = 0;
	h264sw_direct_setup(&direct, &lists, 4);
	CHECK(direct.col == NULL);
	h264sw_direct_setup(&direct, NULL, 4);
	CHECK(direct.col == NULL);
	h264sw_direct_setup(NULL, &lists, 4);
}

/*
 * Checks the temporal mode (8-191 to 8-200).
 *
 * Current POC 4, list 0 = { A (POC 0), B (POC 2) }, list 1 = { co-located
 * frame, POC 8 }: DistScaleFactor 128 for A and 85 for B.
 * - quadrant 0: mvCol (16, -8) from list 0, frame A: refIdxL0 0, mvL0 =
 *   ( 128 * 16 + 128 ) >> 8 = 8 and ( 128 * -8 + 128 ) >> 8 = floor(
 *   -3.5 ) = -4, mvL1 = mvL0 - mvCol = (-8, 4);
 * - quadrant 1: mvCol (6, 6), frame B: refIdxL0 1, mvL0 = ( 85 * 6 + 128
 *   ) >> 8 = 2, mvL1 = (-4, -4);
 * - quadrant 2: an intra co-located macroblock gives refIdxL0 0 and 0;
 * - quadrant 3: list 0 unused in the co-located block, so list 1 gives mvCol
 *   (-16, 8) and frame A: mvL0 = ( 128 * -16 + 128 ) >> 8 = floor( -7.5
 *   ) = -8 and ( 128 * 8 + 128 ) >> 8 = 4, mvL1 = (8, -4).
 */
static void
check_temporal(void)
{
	unsigned quadrant;

	/* Builds the co-located macroblock of the description. */
	begin(0, 1);
	col_set_all(0, 0, 0, ID_REF_A, 16, -8);
	col_set_all(1, 0, 1, ID_REF_B, 6, 6);
	col_set_all(3, 1, 0, ID_REF_A, -16, 8);
	col_motion[CURRENT].ref_idx[0][2] = -1;
	col_motion[CURRENT].ref_idx[1][2] = -1;

	/* Quadrant 0. */
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_ref_is(0, 0, 0, ID_REF_A));
	CHECK(quadrant_ref_is(1, 0, 0, ID_COL));
	CHECK(quadrant_mv_is(0, 0, 8, -4));
	CHECK(quadrant_mv_is(1, 0, -8, 4));

	/* Quadrant 1: the frame B is the second entry of list 0. */
	CHECK(h264sw_direct_quadrant(&ctx, 1) == 0);
	CHECK(quadrant_ref_is(0, 1, 1, ID_REF_B));
	CHECK(quadrant_ref_is(1, 1, 0, ID_COL));
	CHECK(quadrant_mv_is(0, 1, 2, 2));
	CHECK(quadrant_mv_is(1, 1, -4, -4));

	/* Quadrant 3: the co-located block used only list 1. */
	CHECK(h264sw_direct_quadrant(&ctx, 3) == 0);
	CHECK(quadrant_ref_is(0, 3, 0, ID_REF_A));
	CHECK(quadrant_mv_is(0, 3, -8, 4));
	CHECK(quadrant_mv_is(1, 3, 8, -4));

	/* Quadrant 2: a block with no reference gives index 0 and zero vectors. */
	CHECK(h264sw_direct_quadrant(&ctx, 2) == 0);
	CHECK(quadrant_ref_is(0, 2, 0, ID_REF_A));
	CHECK(quadrant_ref_is(1, 2, 0, ID_COL));
	CHECK(quadrant_mv_is(0, 2, 0, 0));
	CHECK(quadrant_mv_is(1, 2, 0, 0));

	/* An intra co-located macroblock gives index 0 and zero vectors in every quadrant. */
	begin(0, 1);
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		CHECK(h264sw_direct_quadrant(&ctx, quadrant) == 0);
		CHECK(quadrant_ref_is(0, quadrant, 0, ID_REF_A));
		CHECK(quadrant_ref_is(1, quadrant, 0, ID_COL));
		CHECK(quadrant_mv_is(0, quadrant, 0, 0));
		CHECK(quadrant_mv_is(1, quadrant, 0, 0));
	}

	/* After the loop every quadrant was served from the intra block, so nothing was concealed. */
	CHECK(ctx.concealed == 0U);

	/* A frame the co-located block referred to that list 0 lacks: index 0, counted as concealed. */
	begin(0, 1);
	col_set_all(0, 0, 0, 99, 16, -8);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_ref_is(0, 0, 0, ID_REF_A));
	CHECK(ctx.concealed == 1U);

	/* Two entries of list 0 holding the same frame: the lowest index is taken. */
	begin(0, 1);
	ctx.lists.list[0][0].frame = &frame_b;
	h264sw_direct_setup(&ctx.direct, &ctx.lists, 4);
	col_set_all(0, 0, 0, ID_REF_B, 16, -8);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_ref_is(0, 0, 0, ID_REF_B));
}

/*
 * Checks a long-term entry of list 0 and a distance of 0: mvL0 = mvCol and
 * mvL1 = 0 (8-195, 8-196).
 */
static void
check_temporal_long_term(void)
{
	/* A long-term reference A. */
	begin(0, 1);
	ctx.lists.list[0][0].long_term = 1;
	h264sw_direct_setup(&ctx.direct, &ctx.lists, 4);
	col_set_all(0, 0, 0, ID_REF_A, 16, -8);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_mv_is(0, 0, 16, -8));
	CHECK(quadrant_mv_is(1, 0, 0, 0));
	CHECK(quadrant_ref_is(0, 0, 0, ID_REF_A));

	/* A short-term A at the same POC as the co-located frame (td 0). */
	begin(0, 1);
	set_lists(8, 2, 8);
	col_set_all(0, 0, 0, ID_REF_A, -5, 7);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_mv_is(0, 0, -5, 7));
	CHECK(quadrant_mv_is(1, 0, 0, 0));
}

/*
 * Checks direct_8x8_inference_flag in the temporal mode: with 1 the corner
 * blocks 0, 5, 10 and 15 of the co-located macroblock stand for their
 * quadrants, with 0 every 4x4 block takes its own.
 *
 * The co-located vector of block b is (4 b, 0) and DistScaleFactor is 128:
 * mvL0 = ( 128 * 4 b + 128 ) >> 8 = 2 b, mvL1 = 2 b - 4 b = -2 b.
 */
static void
check_temporal_inference(void)
{
	unsigned quadrant;
	unsigned sub;
	unsigned blk;
	unsigned corner;
	static const unsigned corners[4] = { 0, 5, 10, 15 };

	/* With the inference every block of a quadrant takes the corner's vector. */
	begin(0, 1);
	for (quadrant = 0; quadrant < 4U; quadrant++)
		col_set_all(quadrant, 0, 0, ID_REF_A, 0, 0);
	for (blk = 0; blk < 16U; blk++)
		col_set_block(blk, 0, (int)(4U * blk), 0);
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		CHECK(h264sw_direct_quadrant(&ctx, quadrant) == 0);
		corner = corners[quadrant];
		CHECK(quadrant_mv_is(0, quadrant, (int)(2U * corner), 0));
		CHECK(quadrant_mv_is(1, quadrant, -(int)(2U * corner), 0));
	}

	/* Without it each block takes its own co-located block. */
	begin(0, 0);
	for (quadrant = 0; quadrant < 4U; quadrant++)
		col_set_all(quadrant, 0, 0, ID_REF_A, 0, 0);
	for (blk = 0; blk < 16U; blk++)
		col_set_block(blk, 0, (int)(4U * blk), 0);
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		CHECK(h264sw_direct_quadrant(&ctx, quadrant) == 0);
		for (sub = 0; sub < 4U; sub++) {
			blk = 4U * quadrant + sub;
			CHECK(mbs[CURRENT].motion.mv[0][blk][0] == (int)(2U * blk));
			CHECK(mbs[CURRENT].motion.mv[1][blk][0] == -(int)(2U * blk));
			CHECK(mbs[CURRENT].motion.mv[0][blk][1] == 0);
		}
	}
}

/*
 * Checks that scaled vectors are clamped to the stored range.
 *
 * With DistScaleFactor -1024 and mvCol (32767, -32768): mvL0 = ( -1024 *
 * 32767 + 128 ) >> 8 = -131068 clamped to -32768 and ( -1024 * -32768 +
 * 128 ) >> 8 = 131072 clamped to 32767; mvL1 = mvL0 - mvCol = -65535
 * clamped to -32768 and 65535 clamped to 32767.
 */
static void
check_temporal_range(void)
{
	/* Forces the largest negative factor that 8-198 allows. */
	begin(0, 1);
	ctx.direct.dist_scale[0] = -1024;
	ctx.direct.long_term0[0] = 0;
	col_set_all(0, 0, 0, ID_REF_A, 32767, -32768);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_mv_is(0, 0, -32768, 32767));
	CHECK(quadrant_mv_is(1, 0, -32768, 32767));

	/* A factor far out of the range does not overflow either. */
	ctx.direct.dist_scale[0] = 2147483647;
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_mv_is(0, 0, 32767, -32768));
}

/*
 * Checks the reference indices and the vectors of the spatial mode
 * (8-184 to 8-190) with a co-located macroblock that is intra.
 *
 * Neighbours: A (macroblock 3) refers to index 2 of list 0 with vector (1,
 * 2), B (macroblock 1) to 1 with (10, 20), C (macroblock 2) to 3 with
 * (30, 40); none uses list 1.  refIdxL0 = MinPositive( 2, MinPositive( 1,
 * 3 ) ) = 1, refIdxL1 = -1 (the list is unused).  Only B refers to index
 * 1, so the prediction is B's vector (8-211).
 */
static void
check_spatial_refs(void)
{
	unsigned quadrant;

	/* MinPositive picks the smallest non-negative index, and the single match decides the vector. */
	begin(1, 1);
	ctx.lists.count[0] = 4;
	ctx.lists.list[0][2].frame = &frame_a;
	ctx.lists.list[0][3].frame = &frame_b;
	mb_set(3, 0, 2, 1, 2);
	mb_set(1, 0, 1, 10, 20);
	mb_set(2, 0, 3, 30, 40);
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		CHECK(h264sw_direct_quadrant(&ctx, quadrant) == 0);
		CHECK(quadrant_ref_is(0, quadrant, 1, ID_REF_B));
		CHECK(quadrant_ref_is(1, quadrant, -1, 0));
		CHECK(quadrant_mv_is(0, quadrant, 10, 20));
		CHECK(quadrant_mv_is(1, quadrant, 0, 0));
	}

	/* After the loop the neighbour scan ran once and is cached, and no zero direct flag was set. */
	CHECK(ctx.direct.spatial_ready == 1);
	CHECK(ctx.direct.direct_zero == 0);

	/* A neighbour that is intra or lacks the list counts as -1: only C refers to index 0 of list 1, and list 0 is -1. */
	begin(1, 1);
	mb_set_intra(3);
	mb_set_intra(1);
	mb_set(2, 1, 0, -3, 9);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_ref_is(0, 0, -1, 0));
	CHECK(quadrant_ref_is(1, 0, 0, ID_COL));
	CHECK(quadrant_mv_is(0, 0, 0, 0));
	CHECK(quadrant_mv_is(1, 0, -3, 9));
	CHECK(ctx.direct.direct_zero == 0);

	/* Both lists below 0 (all neighbours intra): indices 0 and 0 with zero vectors, directZeroPredictionFlag 1. */
	begin(1, 1);
	mb_set_intra(3);
	mb_set_intra(1);
	mb_set_intra(2);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_ref_is(0, 0, 0, ID_REF_A));
	CHECK(quadrant_ref_is(1, 0, 0, ID_COL));
	CHECK(quadrant_mv_is(0, 0, 0, 0));
	CHECK(quadrant_mv_is(1, 0, 0, 0));
	CHECK(ctx.direct.direct_zero == 1);

	/* Neighbours that are not available (another slice) give the same answer. */
	begin(1, 1);
	mbs[3].slice = -1;
	mbs[1].slice = -1;
	mbs[2].slice = -1;
	mbs[0].slice = -1;
	h264sw_mb_neighbours(mbs, PIC_W, CURRENT, &ctx.n);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_ref_is(0, 0, 0, ID_REF_A));
	CHECK(quadrant_ref_is(1, 0, 0, ID_COL));
	CHECK(quadrant_mv_is(0, 0, 0, 0));
	CHECK(ctx.direct.direct_zero == 1);

	/* D stands for C when C is not available: D (macroblock 0) refers to index 0 of list 0. */
	begin(1, 1);
	mbs[2].slice = -1;
	h264sw_mb_neighbours(mbs, PIC_W, CURRENT, &ctx.n);
	mb_set_intra(3);
	mb_set_intra(1);
	mb_set(0, 0, 0, 6, -6);
	mb_set(2, 0, 1, 99, 99);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_ref_is(0, 0, 0, ID_REF_A));
	CHECK(quadrant_mv_is(0, 0, 6, -6));
}

/*
 * Checks colZeroFlag: RefPicList1[ 0 ] short-term, refIdxCol 0 and both
 * components of mvCol within -1..1 give the vector 0 to a list whose
 * reference index is 0.
 *
 * Neighbours A, B and C all refer to index 1 of list 0 with vector (5, 5)
 * and to index 0 of list 1 with vector (7, 7): refIdxL0 1, refIdxL1 0, and
 * the vectors are the medians (5, 5) and (7, 7).
 * - quadrant 0: list 0 of the co-located block, index 0, mvCol (1, -1):
 *   colZeroFlag 1: list 0 keeps (5, 5) (its index is 1), list 1 gets 0;
 * - quadrant 1: mvCol (2, 0): colZeroFlag 0;
 * - quadrant 2: mvCol (0, 0) but refIdxCol 1: colZeroFlag 0;
 * - quadrant 3: only list 1 used, index 0, mvCol (0, -1): colZeroFlag 1.
 */
static void
check_spatial_col_zero(void)
{
	/* Builds the neighbours and the co-located macroblock of the description. */
	begin(1, 1);
	mb_set(3, 0, 1, 5, 5);
	mb_set(1, 0, 1, 5, 5);
	mb_set(2, 0, 1, 5, 5);
	mb_set(3, 1, 0, 7, 7);
	mb_set(1, 1, 0, 7, 7);
	mb_set(2, 1, 0, 7, 7);
	col_set_all(0, 0, 0, ID_REF_A, 1, -1);
	col_set_all(1, 0, 0, ID_REF_A, 2, 0);
	col_set_all(2, 0, 1, ID_REF_B, 0, 0);
	col_set_all(3, 1, 0, ID_REF_A, 0, -1);

	/* Quadrant 0. */
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_ref_is(0, 0, 1, ID_REF_B));
	CHECK(quadrant_ref_is(1, 0, 0, ID_COL));
	CHECK(quadrant_mv_is(0, 0, 5, 5));
	CHECK(quadrant_mv_is(1, 0, 0, 0));

	/* Quadrant 1: the vector is outside -1..1. */
	CHECK(h264sw_direct_quadrant(&ctx, 1) == 0);
	CHECK(quadrant_mv_is(0, 1, 5, 5));
	CHECK(quadrant_mv_is(1, 1, 7, 7));

	/* Quadrant 2: refIdxCol is not 0. */
	CHECK(h264sw_direct_quadrant(&ctx, 2) == 0);
	CHECK(quadrant_mv_is(0, 2, 5, 5));
	CHECK(quadrant_mv_is(1, 2, 7, 7));

	/* Quadrant 3: the co-located block used list 1 only. */
	CHECK(h264sw_direct_quadrant(&ctx, 3) == 0);
	CHECK(quadrant_mv_is(0, 3, 5, 5));
	CHECK(quadrant_mv_is(1, 3, 0, 0));

	/* A long-term RefPicList1[ 0 ] never gives colZeroFlag. */
	ctx.lists.list[1][0].long_term = 1;
	h264sw_direct_setup(&ctx.direct, &ctx.lists, 4);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_mv_is(1, 0, 7, 7));
	CHECK(h264sw_direct_quadrant(&ctx, 3) == 0);
	CHECK(quadrant_mv_is(1, 3, 7, 7));
	ctx.lists.list[1][0].long_term = 0;

	/* Both indices 0: both lists are zeroed by colZeroFlag. */
	begin(1, 1);
	mb_set(3, 0, 0, 5, 5);
	mb_set(1, 0, 0, 5, 5);
	mb_set(2, 0, 0, 5, 5);
	mb_set(3, 1, 0, 7, 7);
	mb_set(1, 1, 0, 7, 7);
	mb_set(2, 1, 0, 7, 7);
	col_set_all(0, 0, 0, ID_REF_A, -1, 1);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_mv_is(0, 0, 0, 0));
	CHECK(quadrant_mv_is(1, 0, 0, 0));
	CHECK(quadrant_ref_is(0, 0, 0, ID_REF_A));
	CHECK(quadrant_ref_is(1, 0, 0, ID_COL));
}

/*
 * Checks direct_8x8_inference_flag in the spatial mode.  Neighbours give
 * the prediction (5, 5) for list 0 with index 0.  In quadrant 0 the
 * co-located block 0 has the vector (0, 0) (colZeroFlag 1) and blocks 1 to
 * 3 have (5, 5) (colZeroFlag 0).  With the inference all four blocks take
 * block 0; without it only block 0 is zeroed.
 */
static void
check_spatial_inference(void)
{
	/* With the inference the whole quadrant is zeroed. */
	begin(1, 1);
	mb_set(3, 0, 0, 5, 5);
	mb_set(1, 0, 0, 5, 5);
	mb_set(2, 0, 0, 5, 5);
	col_set_all(0, 0, 0, ID_REF_A, 5, 5);
	col_set_block(0, 0, 0, 0);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_mv_is(0, 0, 0, 0));

	/* Without it, only the block whose co-located block is still is zeroed. */
	begin(1, 0);
	mb_set(3, 0, 0, 5, 5);
	mb_set(1, 0, 0, 5, 5);
	mb_set(2, 0, 0, 5, 5);
	col_set_all(0, 0, 0, ID_REF_A, 5, 5);
	col_set_block(0, 0, 0, 0);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(mbs[CURRENT].motion.mv[0][0][0] == 0);
	CHECK(mbs[CURRENT].motion.mv[0][0][1] == 0);
	CHECK(mbs[CURRENT].motion.mv[0][1][0] == 5);
	CHECK(mbs[CURRENT].motion.mv[0][2][0] == 5);
	CHECK(mbs[CURRENT].motion.mv[0][3][1] == 5);
}

/* Checks that the macroblock's reference indices and prediction are derived once and kept for its other quadrants. */
static void
check_spatial_cache(void)
{
	/* Derives quadrant 0, which fills the cache. */
	begin(1, 1);
	mb_set(3, 0, 0, 5, 5);
	mb_set(1, 0, 0, 5, 5);
	mb_set(2, 0, 0, 5, 5);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(ctx.direct.spatial_ready == 1);
	CHECK(ctx.direct.spatial_ref[0] == 0);
	CHECK(ctx.direct.spatial_ref[1] == -1);
	CHECK(ctx.direct.spatial_mv[0][0] == 5);
	CHECK(ctx.direct.spatial_mv[0][1] == 5);

	/* A change of the neighbours does not change the later quadrants of the same macroblock. */
	mb_set(3, 0, 0, 9, 9);
	mb_set(1, 0, 0, 9, 9);
	mb_set(2, 0, 0, 9, 9);
	CHECK(h264sw_direct_quadrant(&ctx, 3) == 0);
	CHECK(quadrant_mv_is(0, 3, 5, 5));

	/* Clearing spatial_ready, as the slice data loop does for each macroblock, derives it again. */
	ctx.direct.spatial_ready = 0;
	CHECK(h264sw_direct_quadrant(&ctx, 3) == 0);
	CHECK(quadrant_mv_is(0, 3, 9, 9));
}

/* Checks that the direct bit of the macroblock is set per quadrant and kept. */
static void
check_direct_bits(void)
{
	/* Spatial mode: quadrants 0 and 2 only. */
	begin(1, 1);
	CHECK(mbs[CURRENT].direct == 0U);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(mbs[CURRENT].direct == 0x1U);
	CHECK(h264sw_direct_quadrant(&ctx, 2) == 0);
	CHECK(mbs[CURRENT].direct == 0x5U);

	/* Temporal mode: all four quadrants, B_Skip or B_Direct_16x16. */
	begin(0, 1);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(h264sw_direct_quadrant(&ctx, 1) == 0);
	CHECK(h264sw_direct_quadrant(&ctx, 2) == 0);
	CHECK(h264sw_direct_quadrant(&ctx, 3) == 0);
	CHECK(mbs[CURRENT].direct == 0xfU);
}

/* Checks that bad input gives EINVAL and writes nothing. */
static void
check_errors(void)
{
	struct h264sw_motion before;
	struct h264sw_frame *saved_frame;

	/* A missing context. */
	CHECK(h264sw_direct_quadrant(NULL, 0) == EINVAL);

	/* A quadrant a macroblock does not have, in both modes. */
	begin(1, 1);
	CHECK(h264sw_direct_quadrant(&ctx, 4) == EINVAL);
	CHECK(h264sw_direct_quadrant(&ctx, 0xffffffffU) == EINVAL);
	begin(0, 1);
	CHECK(h264sw_direct_quadrant(&ctx, 4) == EINVAL);
	CHECK(mbs[CURRENT].direct == 0U);

	/* Missing parts of the context. */
	begin(1, 1);
	ctx.picture = NULL;
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);
	begin(1, 1);
	picture.sps = NULL;
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);
	begin(1, 1);
	ctx.header = NULL;
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);
	begin(1, 1);
	ctx.n.current = NULL;
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);

	/* No co-located frame, no motion in it, or a macroblock address beyond it. */
	begin(0, 1);
	ctx.direct.col = NULL;
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);
	begin(0, 1);
	frame_col.motion = NULL;
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);
	begin(0, 1);
	ctx.mb_addr = PIC_W * PIC_H;
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);
	begin(0, 1);
	ctx.mb_addr = 0xffffffffU;
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);

	/* Empty lists. */
	begin(0, 1);
	ctx.lists.count[0] = 0;
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);
	begin(1, 1);
	ctx.lists.count[1] = 0;
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);

	/* A list entry without a frame. */
	begin(0, 1);
	saved_frame = ctx.lists.list[0][0].frame;
	ctx.lists.list[0][0].frame = NULL;
	memcpy(&before, &mbs[CURRENT].motion, sizeof(before));
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);
	CHECK(memcmp(&before, &mbs[CURRENT].motion, sizeof(before)) == 0);
	ctx.lists.list[0][0].frame = saved_frame;

	/* Spatial: neighbours name an index beyond the list (a broken stream); nothing is written and the direct bit stays clear. */
	begin(1, 1);
	mb_set(3, 0, 5, 1, 1);
	mb_set(1, 0, 6, 1, 1);
	mb_set(2, 0, 7, 1, 1);
	memcpy(&before, &mbs[CURRENT].motion, sizeof(before));
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);
	CHECK(memcmp(&before, &mbs[CURRENT].motion, sizeof(before)) == 0);
	CHECK(mbs[CURRENT].direct == 0U);

	/* Spatial: an index of 127 (the largest int8_t) is refused the same way. */
	begin(1, 1);
	mb_set(3, 0, 127, 1, 1);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == EINVAL);

	/* Temporal: a co-located block that names a huge reference index and an unknown frame does not crash. */
	begin(0, 1);
	col_set_all(0, 0, 127, 0xffffffffU, 32767, -32768);
	CHECK(h264sw_direct_quadrant(&ctx, 0) == 0);
	CHECK(quadrant_ref_is(0, 0, 0, ID_REF_A));
}
