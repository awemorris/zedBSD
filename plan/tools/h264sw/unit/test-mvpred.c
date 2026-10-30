/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the motion vector prediction of the software H.264 decoder:
 * the neighbouring partitions (6.4.11.7, Table 6-2), the median
 * prediction (8.4.1.3.1), the directional prediction of 16x8 and 8x16
 * partitions (8-203 to 8-206), the motion data of neighbours (8.4.1.3.2)
 * and P_Skip (8.4.1.1).  The expected values are worked out by hand
 * from those clauses: every macroblock of the picture holds vectors
 * that name the 4x4 block they sit in, so that the vector a prediction
 * returns says which neighbouring block it was taken from.
 */

#include <string.h>

#include "check.h"
#include "userland/desktop/libmedia/h264sw-context.h"
#include "userland/desktop/libmedia/h264sw-mvpred.h"

/* The picture of the tests: 3 by 3 macroblocks, the middle one (address 4) is the current macroblock. */
#define PIC_W 3U
#define PIC_H 3U
#define CURRENT 4U

/*
 * The current macroblock's neighbours in this picture: A is address 3,
 * B is 1, C is 2 and D is 0.  The value that a vector built by
 * fill_ids holds in the 4x4 block blk of macroblock mb is
 * id_of( mb, blk ) = 100 * mb + blk horizontally and its negative
 * vertically.
 */
#define ID(mb, blk) (100 * (mb) + (blk))

/*
 * One row of the expected predictions for a 4x4 partition: the block of
 * the current macroblock and the id the median of its A, B and C (or D)
 * has, worked out by hand.
 */
struct expect_block_id {
	unsigned blk;
	int id;
};

/*
 * The slice context, the picture context and the macroblocks of the
 * tests.  They are big, so they live for the whole run of the program
 * and every test rebuilds what it needs with begin().
 */
static struct h264sw_slice_ctx ctx;
static struct h264sw_picture_ctx picture;
static struct h264sw_mb mbs[PIC_W * PIC_H];

/*
 * The medians of A, B and C for each 4x4 block of the current macroblock
 * (address 4), with every vector holding its own id and every block of
 * the macroblock before it in decoding order derived.  Worked out from
 * Table 6-2 and Table 6-3: for block (x, y) the neighbours are A at
 * ( x - 1, y ), B at ( x, y - 1 ), C at ( x + 4, y - 1 ) and, when C is
 * not available, D at ( x - 1, y - 1 ).  Macroblock 3 is A, 1 is B, 2 is
 * C and 0 is D of the current macroblock.  The blocks (4, 4), (12, 4),
 * (4, 12) and (12, 12) of the four 8x8 quadrants and (12, 0) and
 * (12, 8) have a C that is later in decoding order or outside, so D
 * stands for it.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 6-2 and Table 6-3
 * (locations ( xD, yD ) of A, B, C and D and the macroblock holding each
 * location), and 6.4.13.1 for the block that holds a location.
 *
 * block 0: A 305, B 110, C 111 -> 111
 * block 1: A 400, B 111, C 114 -> 114
 * block 2: A 307, B 400, C 401 -> 400
 * block 3: A 402, B 401, C (blk 4 not decoded) D 400 -> 401
 * block 4: A 401, B 114, C 115 -> 115
 * block 5: A 404, B 115, C (macroblock 2) 210 -> 210
 * block 6: A 403, B 404, C 405 -> 404
 * block 7: A 406, B 405, C (outside) D 404 -> 405
 * block 8: A 313, B 402, C 403 -> 402
 * block 9: A 408, B 403, C 406 -> 406
 * block 10: A 315, B 408, C 409 -> 408
 * block 11: A 410, B 409, C (blk 12 not decoded) D 408 -> 409
 * block 12: A 409, B 406, C 407 -> 407
 * block 13: A 412, B 407, C (outside) D 406 -> 407
 * block 14: A 411, B 412, C 413 -> 412
 * block 15: A 414, B 413, C (outside) D 412 -> 413
 */
static const struct expect_block_id expect_4x4[16] = {
	{ 0, ID(1, 11) }, { 1, ID(1, 14) }, { 2, ID(4, 0) }, { 3, ID(4, 1) },
	{ 4, ID(1, 15) }, { 5, ID(2, 10) }, { 6, ID(4, 4) }, { 7, ID(4, 5) },
	{ 8, ID(4, 2) }, { 9, ID(4, 6) }, { 10, ID(4, 8) }, { 11, ID(4, 9) },
	{ 12, ID(4, 7) }, { 13, ID(4, 7) }, { 14, ID(4, 12) }, { 15, ID(4, 13) }
};

/* Forward declarations of the static functions. */
static void begin(unsigned addr);
static void neighbours(unsigned addr);
static void fill_ids(unsigned mb);
static void set_block(unsigned mb, unsigned list, unsigned blk, int ref, int x, int y);
static void set_quadrant_ref(unsigned mb, unsigned list, unsigned quadrant, int ref);
static void set_all_quadrant_refs(unsigned mb, unsigned list, int ref);
static int mv_is(const int16_t *mv, int x, int y);
static void check_median(void);
static void check_single_reference(void);
static void check_neighbour_missing(void);
static void check_neighbour_motion(void);
static void check_directional(void);
static void check_inner_partitions(void);
static void check_pskip(void);
static void check_errors(void);

int
main(void)
{
	/* Runs every group of checks. */
	check_median();
	check_single_reference();
	check_neighbour_missing();
	check_neighbour_motion();
	check_directional();
	check_inner_partitions();
	check_pskip();
	check_errors();

	/* Reports the outcome as the one last line. */
	return check_finish("mvpred");
}

/*
 * Rebuilds the context for a macroblock: every macroblock is reset in
 * slice 0 and has no motion, the slice context is cleared and the
 * neighbours of the macroblock are found.
 */
static void
begin(
	unsigned addr)
{
	unsigned i;

	/* Resets every macroblock to the state before it is decoded, in slice 0. */
	for (i = 0; i < PIC_W * PIC_H; i++)
		h264sw_mb_reset(&mbs[i], 0);

	/* Clears the slice context and points it at the picture. */
	memset(&ctx, 0, sizeof(ctx));
	memset(&picture, 0, sizeof(picture));
	picture.mbs = mbs;
	picture.width_mbs = PIC_W;
	picture.height_mbs = PIC_H;
	picture.size_mbs = PIC_W * PIC_H;
	ctx.picture = &picture;
	ctx.mb_addr = addr;

	/* Finds the neighbouring macroblocks of the current one. */
	neighbours(addr);
}

/* Finds the neighbouring macroblocks of the macroblock at an address, from the slice numbers set now. */
static void
neighbours(
	unsigned addr)
{
	/* Fills the neighbour set of the slice context. */
	ctx.mb_addr = addr;
	h264sw_mb_neighbours(mbs, PIC_W, addr, &ctx.n);
}

/*
 * Gives every 4x4 block of a macroblock in list 0 a vector that names it
 * (ID( mb, blk ) horizontally, its negative vertically) and reference
 * index 0 for every 8x8 quadrant.
 */
static void
fill_ids(
	unsigned mb)
{
	unsigned blk;

	/* Names every block by its vector and marks all quadrants as using reference 0 of list 0. */
	for (blk = 0; blk < 16U; blk++)
		set_block(mb, 0, blk, 0, ID((int)mb, (int)blk), -ID((int)mb, (int)blk));
}

/* Sets the vector of one 4x4 block of a macroblock and the reference index of the 8x8 quadrant that holds it. */
static void
set_block(
	unsigned mb,
	unsigned list,
	unsigned blk,
	int ref,
	int x,
	int y)
{
	/* Stores the vector of the block. */
	mbs[mb].motion.mv[list][blk][0] = (int16_t)x;
	mbs[mb].motion.mv[list][blk][1] = (int16_t)y;

	/* Stores the reference index of the quadrant, which blocks 4 q to 4 q + 3 share. */
	mbs[mb].motion.ref_idx[list][blk / 4U] = (int8_t)ref;
}

/* Sets the reference index of one 8x8 quadrant of a macroblock without touching its vectors. */
static void
set_quadrant_ref(
	unsigned mb,
	unsigned list,
	unsigned quadrant,
	int ref)
{
	/* Stores the reference index of the quadrant. */
	mbs[mb].motion.ref_idx[list][quadrant] = (int8_t)ref;
}

/* Sets the reference index of every 8x8 quadrant of a macroblock. */
static void
set_all_quadrant_refs(
	unsigned mb,
	unsigned list,
	int ref)
{
	unsigned quadrant;

	/* Stores the reference index in the four quadrants. */
	for (quadrant = 0; quadrant < 4U; quadrant++)
		set_quadrant_ref(mb, list, quadrant, ref);
}

/* Tells whether a vector has the given components. */
static int
mv_is(
	const int16_t *mv,
	int x,
	int y)
{
	/* A vector differs when either component differs. */
	if (mv[0] != x)
		return 0;
	if (mv[1] != y)
		return 0;

	/* Both components are equal. */
	return 1;
}

/* Checks the median of three neighbours (8-212, 8-213) and the 4x4 partitions' neighbour choice. */
static void
check_median(void)
{
	int16_t mvp[2];
	unsigned i;

	/*
	 * Three neighbours of the 16x16 partition with reference 0: A is
	 * block 5 of macroblock 3, B block 10 of macroblock 1 and C block 10
	 * of macroblock 2.  Every other block holds a vector and a reference
	 * index that would change the answer.
	 */
	begin(CURRENT);
	fill_ids(3);
	fill_ids(1);
	fill_ids(2);
	set_all_quadrant_refs(3, 0, 3);
	set_all_quadrant_refs(1, 0, 3);
	set_all_quadrant_refs(2, 0, 3);
	set_block(3, 0, 5, 0, 4, -8);
	set_block(1, 0, 10, 0, 10, 2);
	set_block(2, 0, 10, 0, -6, 6);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);

	/* Median of (4, 10, -6) is 4 and median of (-8, 2, 6) is 2. */
	CHECK(mv_is(mvp, 4, 2));

	/* Two equal components: median of (-3, -3, 5) is -3 and of (7, -9, 7) is 7. */
	set_block(3, 0, 5, 0, -3, 7);
	set_block(1, 0, 10, 0, -3, -9);
	set_block(2, 0, 10, 0, 5, 7);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, -3, 7));

	/* The extreme 16-bit vectors: median of (32767, -32768, 0) is 0 and of (32767, 32767, -32768) is 32767. */
	set_block(3, 0, 5, 0, 32767, 32767);
	set_block(1, 0, 10, 0, -32768, 32767);
	set_block(2, 0, 10, 0, 0, -32768);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 0, 32767));

	/* Every 4x4 block of a macroblock whose motion is derived in decoding order takes its neighbours from Table 6-2. */
	begin(CURRENT);
	fill_ids(0);
	fill_ids(1);
	fill_ids(2);
	fill_ids(3);
	fill_ids(4);
	for (i = 0; i < 16U; i++) {
		ctx.decoded_4x4 = ((uint32_t)1 << expect_4x4[i].blk) - 1U;
		h264sw_mvpred(&ctx, 0, h264sw_blk4x4_x[expect_4x4[i].blk], h264sw_blk4x4_y[expect_4x4[i].blk], 4, 4, 0, mvp);
		CHECK(mv_is(mvp, expect_4x4[i].id, -expect_4x4[i].id));
	}

	/* A block that is marked decoded is available: block 4 decoded makes C of block 3 available, so the median is of 402, 401 and 404. */
	ctx.decoded_4x4 = 0x1fU;
	h264sw_mvpred(&ctx, 0, 4, 4, 4, 4, 0, mvp);
	CHECK(mv_is(mvp, ID(4, 2), -ID(4, 2)));

	/* The same block without block 4 decoded takes D, the median of 402, 401 and 400. */
	ctx.decoded_4x4 = 0x07U;
	h264sw_mvpred(&ctx, 0, 4, 4, 4, 4, 0, mvp);
	CHECK(mv_is(mvp, ID(4, 1), -ID(4, 1)));
}

/* Checks the rule that one and only one neighbour with the reference index decides (8-211). */
static void
check_single_reference(void)
{
	int16_t mvp[2];

	/*
	 * A (block 5 of macroblock 3) refers to picture 1, B (block 10 of
	 * macroblock 1) to 0 and C (block 10 of macroblock 2) to 2.
	 */
	begin(CURRENT);
	set_block(3, 0, 5, 1, 3, 3);
	set_block(1, 0, 10, 0, 50, 50);
	set_block(2, 0, 10, 2, -40, -40);

	/* Only A has reference 1, so its vector is the prediction, not the median (3, 50, -40). */
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 1, mvp);
	CHECK(mv_is(mvp, 3, 3));

	/* Only B has reference 0. */
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 50, 50));

	/* Only C has reference 2. */
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 2, mvp);
	CHECK(mv_is(mvp, -40, -40));

	/* Two neighbours with reference 0 (A and C now) give the median again: median of (3, 50, -40) is 3. */
	set_block(3, 0, 5, 0, 3, 3);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 3, 3));

	/* A (1, 1) and B (5, 9) refer to picture 0 and C (100, 100) to picture 1: two matches, so the median of the three vectors, (5, 9). */
	begin(CURRENT);
	set_block(3, 0, 5, 0, 1, 1);
	set_block(1, 0, 10, 0, 5, 9);
	set_block(2, 0, 10, 1, 100, 100);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 5, 9));

	/* No neighbour with reference 3 gives the median of the three: median of (1, 4, -7) is 1 and of (2, -5, 8) is 2. */
	set_block(3, 0, 5, 0, 1, 2);
	set_block(1, 0, 10, 0, 4, -5);
	set_block(2, 0, 10, 0, -7, 8);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 3, mvp);
	CHECK(mv_is(mvp, 1, 2));
}

/* Checks neighbours that are missing: only A (8-207 to 8-210), C replaced by D (8-214), B missing with C present. */
static void
check_neighbour_missing(void)
{
	int16_t mvp[2];

	/*
	 * Macroblock 1 (x 1, y 0) has only A (macroblock 0): B, C and D are
	 * above the picture.  A refers to picture 0 with (7, -2).
	 */
	begin(1);
	set_block(0, 0, 5, 0, 7, -2);

	/* With reference 1 no neighbour matches; A stands for B and C, so the median is A's own vector, not (0, 0). */
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 1, mvp);
	CHECK(mv_is(mvp, 7, -2));

	/* With reference 0 the match of A (three times) gives the same vector. */
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 7, -2));

	/* The first macroblock has no neighbour at all, and the prediction is (0, 0). */
	begin(0);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));

	/*
	 * Macroblock 5 (x 2, y 1) has no C (it is in the last column); D is
	 * macroblock 1, whose block 15 holds the location ( -1, -1 ).  A is
	 * block 5 of macroblock 4 with (4, 4), B is block 10 of macroblock 2
	 * with (8, 0) and D is (-4, 20), all with reference 0.
	 */
	begin(5);
	set_block(4, 0, 5, 0, 4, 4);
	set_block(2, 0, 10, 0, 8, 0);
	set_block(1, 0, 15, 0, -4, 20);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);

	/* The median of (4, 8, -4) is 4 and of (4, 0, 20) is 4; without D it would be (4, 0). */
	CHECK(mv_is(mvp, 4, 4));

	/*
	 * B missing (another slice) while A and C exist: no replacement of B
	 * by A takes place.  A is (4, 4), C is (8, 8), both with reference 0.
	 */
	begin(CURRENT);
	set_block(3, 0, 5, 0, 4, 4);
	set_block(2, 0, 10, 0, 8, 8);
	mbs[1].slice = 1;
	neighbours(CURRENT);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);

	/* Two neighbours match; the median of (4, 0, 8) horizontally and (4, 0, 8) vertically is (4, 4). */
	CHECK(mv_is(mvp, 4, 4));

	/* With reference 5 none matches and the median is the same, B counting as (0, 0). */
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 5, mvp);
	CHECK(mv_is(mvp, 4, 4));
}

/* Checks intra neighbours and neighbours that do not use the list (8.4.1.3.2). */
static void
check_neighbour_motion(void)
{
	int16_t mvp[2];

	/* An intra A has refIdx -1 and vector 0: with B (3, 3) and C (5, 5) at reference 0 the median is of (0, 3, 5). */
	begin(CURRENT);
	mbs[3].motion.intra = 1;
	set_block(3, 0, 5, 0, 99, 99);
	set_block(1, 0, 10, 0, 3, 3);
	set_block(2, 0, 10, 0, 5, 5);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 3, 3));

	/* A neighbour whose list-0 reference is -1 (predFlagL0 0) counts as vector 0 even if a vector is stored. */
	begin(CURRENT);
	set_block(3, 0, 5, -1, 99, 99);
	set_block(1, 0, 10, 0, 3, 3);
	set_block(2, 0, 10, 0, 5, 5);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 3, 3));

	/* List 1 is read separately: only A uses it, with reference 0 and (9, 9), while its list 0 is (1, 1). */
	begin(CURRENT);
	set_block(3, 0, 5, 0, 1, 1);
	set_block(3, 1, 5, 0, 9, 9);
	set_block(1, 0, 10, 0, 40, 40);
	set_block(2, 0, 10, 0, 60, 60);
	h264sw_mvpred(&ctx, 1, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 9, 9));
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 40, 40));

	/* No neighbour uses list 1 for the 16x16 partition of a macroblock whose neighbours use list 0 only: the prediction is (0, 0). */
	begin(CURRENT);
	fill_ids(3);
	fill_ids(1);
	fill_ids(2);
	h264sw_mvpred(&ctx, 1, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));
}

/* Checks the directional prediction of 16x8 and 8x16 partitions (8-203 to 8-206). */
static void
check_directional(void)
{
	int16_t mvp[2];

	/*
	 * Upper 16x8 (y 0) of macroblock 4: A is block 5 of macroblock 3, B
	 * is block 10 of macroblock 1 and C block 10 of macroblock 2.  B has
	 * reference 0 and (9, -9); A (1, 1) and C (2, 2) also refer to 0, so
	 * the median would be (2, 1).
	 */
	begin(CURRENT);
	set_block(3, 0, 5, 0, 1, 1);
	set_block(1, 0, 10, 0, 9, -9);
	set_block(2, 0, 10, 0, 2, 2);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 8, 0, mvp);
	CHECK(mv_is(mvp, 9, -9));

	/* B at reference 1 and C at reference 3: the directional rule does not apply and A alone refers to picture 0, so A's vector is the answer. */
	set_block(1, 0, 10, 1, 9, -9);
	set_block(2, 0, 10, 3, 2, 2);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 8, 0, mvp);
	CHECK(mv_is(mvp, 1, 1));

	/* B still at reference 1, A and C at reference 0: two matches, so the median of (1, 9, 2) and (1, -9, 2) is (2, 1). */
	set_block(2, 0, 10, 0, 2, 2);
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 8, 0, mvp);
	CHECK(mv_is(mvp, 2, 1));

	/*
	 * Lower 16x8 (y 8): A is block 13 of macroblock 3, B block 2 of the
	 * current macroblock (its upper half, blocks 0 to 7, is derived) and
	 * C is outside, so D is block 7 of macroblock 3.
	 */
	begin(CURRENT);
	ctx.decoded_4x4 = 0xffU;
	set_block(3, 0, 13, 0, 6, -6);
	set_block(4, 0, 2, 1, 9, 9);
	set_block(3, 0, 7, 1, 1, 1);
	h264sw_mvpred(&ctx, 0, 0, 8, 16, 8, 0, mvp);

	/* A has reference 0, so its vector is taken; the median of (6, 9, 1) would be 6 and of (-6, 9, 1) would be 1. */
	CHECK(mv_is(mvp, 6, -6));

	/* A with reference 2 does not match; B (9, 9) and D (3, 3) match reference 0 and the median of (6, 9, 3) and (-6, 9, 3) follows. */
	set_block(3, 0, 13, 2, 6, -6);
	set_block(4, 0, 2, 0, 9, 9);
	set_block(3, 0, 7, 0, 3, 3);
	h264sw_mvpred(&ctx, 0, 0, 8, 16, 8, 0, mvp);
	CHECK(mv_is(mvp, 6, 3));

	/* Left 8x16 (x 0): A is block 5 of macroblock 3, B block 10 of macroblock 1 and C block 14 of macroblock 1. */
	begin(CURRENT);
	set_block(3, 0, 5, 0, -5, 5);
	set_block(1, 0, 10, 0, 20, 20);
	set_block(1, 0, 14, 0, 30, 30);
	h264sw_mvpred(&ctx, 0, 0, 0, 8, 16, 0, mvp);

	/* A matches, so its vector is taken: the median of (-5, 20, 30) would be 20. */
	CHECK(mv_is(mvp, -5, 5));

	/* A with another reference falls back to the median: B and C match, median of (0, 20, 30) is 20 and of (0, 20, 30) is 20. */
	set_block(3, 0, 5, 4, -5, 5);
	h264sw_mvpred(&ctx, 0, 0, 0, 8, 16, 0, mvp);
	CHECK(mv_is(mvp, 20, 20));

	/*
	 * Right 8x16 (x 8): A is block 1 of the current macroblock (the left
	 * half, blocks 0 to 3 and 8 to 11, is derived), B is block 14 of
	 * macroblock 1 and C is block 10 of macroblock 2.
	 */
	begin(CURRENT);
	ctx.decoded_4x4 = 0x0f0fU;
	set_block(4, 0, 1, 0, 11, 11);
	set_block(1, 0, 14, 0, 22, 22);
	set_block(2, 0, 10, 0, -33, 33);
	h264sw_mvpred(&ctx, 0, 8, 0, 8, 16, 0, mvp);

	/* C matches, so its vector is taken: the median of (11, 22, -33) would be 11. */
	CHECK(mv_is(mvp, -33, 33));

	/* C with another reference falls back to the median of A (11, 11), B (22, 22) and C (-33, 33): (11, 22). */
	set_block(2, 0, 10, 7, -33, 33);
	h264sw_mvpred(&ctx, 0, 8, 0, 8, 16, 0, mvp);
	CHECK(mv_is(mvp, 11, 22));

	/* The right 8x16 partition in the last column of the picture has no C, so D at ( 7, -1 ), block 11 of macroblock 2, stands in for it. */
	begin(5);
	ctx.decoded_4x4 = 0x0f0fU;
	set_block(5, 0, 1, 1, 1, 1);
	set_block(2, 0, 14, 1, 2, 2);
	set_block(2, 0, 11, 0, 77, -77);
	h264sw_mvpred(&ctx, 0, 8, 0, 8, 16, 0, mvp);

	/* D has reference 0, so its vector is taken. */
	CHECK(mv_is(mvp, 77, -77));
}

/* Checks partitions inside a sub-macroblock: 8x8, 8x4 and 4x8 partitions and the decoded set. */
static void
check_inner_partitions(void)
{
	int16_t mvp[2];

	/* Gives every block of the five macroblocks its own vector, reference 0. */
	begin(CURRENT);
	fill_ids(0);
	fill_ids(1);
	fill_ids(2);
	fill_ids(3);
	fill_ids(4);

	/*
	 * The 8x8 partition at (8, 8) with quadrants 0 to 2 derived: A is
	 * block 9 (409), B block 6 (406), C at (16, 7) is outside so D at
	 * (7, 7) is block 3 (403): the median is 406.
	 */
	ctx.decoded_4x4 = 0x0fffU;
	h264sw_mvpred(&ctx, 0, 8, 8, 8, 8, 0, mvp);
	CHECK(mv_is(mvp, ID(4, 6), -ID(4, 6)));

	/* The 8x8 partition at (0, 8) with quadrants 0 and 1 derived: A is block 13 of macroblock 3 (313), B block 2 (402), C at (8, 7) block 6 (406): 402. */
	ctx.decoded_4x4 = 0x00ffU;
	h264sw_mvpred(&ctx, 0, 0, 8, 8, 8, 0, mvp);
	CHECK(mv_is(mvp, ID(4, 2), -ID(4, 2)));

	/* The 8x8 partition at (8, 0): A is block 1 (401), B block 14 of macroblock 1 (114), C at (16, -1) block 10 of macroblock 2 (210): 210. */
	ctx.decoded_4x4 = 0x000fU;
	h264sw_mvpred(&ctx, 0, 8, 0, 8, 8, 0, mvp);
	CHECK(mv_is(mvp, ID(2, 10), -ID(2, 10)));

	/*
	 * The lower 8x4 partition of quadrant 0, at (0, 4) with the upper
	 * one (blocks 0 and 1) derived: A is block 7 of macroblock 3 (307),
	 * B block 0 (400), C at (8, 3) is block 4, not derived, so D at
	 * ( -1, 3 ) is block 5 of macroblock 3 (305): the median is 307.
	 */
	ctx.decoded_4x4 = 0x0003U;
	h264sw_mvpred(&ctx, 0, 0, 4, 8, 4, 0, mvp);
	CHECK(mv_is(mvp, ID(3, 7), -ID(3, 7)));

	/*
	 * The right 4x8 partition of quadrant 0, at (4, 0) with the left one
	 * (blocks 0 and 2) derived: A is block 0 (400), B block 11 of
	 * macroblock 1 (111) and C at (8, -1) block 14 of macroblock 1 (114):
	 * the median is 114.
	 */
	ctx.decoded_4x4 = 0x0005U;
	h264sw_mvpred(&ctx, 0, 4, 0, 4, 8, 0, mvp);
	CHECK(mv_is(mvp, ID(1, 14), -ID(1, 14)));
}

/* Checks P_Skip (8.4.1.1). */
static void
check_pskip(void)
{
	int16_t mv[2];

	/* The first macroblock has no A and no B: (0, 0). */
	begin(0);
	mv[0] = 77;
	mv[1] = 77;
	h264sw_mvpred_pskip(&ctx, mv);
	CHECK(mv_is(mv, 0, 0));

	/* A macroblock in the first row has no B (macroblock 1): (0, 0) although A would predict (5, 5). */
	begin(1);
	set_block(0, 0, 5, 0, 5, 5);
	mv[0] = 77;
	mv[1] = 77;
	h264sw_mvpred_pskip(&ctx, mv);
	CHECK(mv_is(mv, 0, 0));

	/* A macroblock in the first column has no A (macroblock 3): (0, 0) although B would predict (5, 5). */
	begin(3);
	set_block(0, 0, 10, 0, 5, 5);
	mv[0] = 77;
	mv[1] = 77;
	h264sw_mvpred_pskip(&ctx, mv);
	CHECK(mv_is(mv, 0, 0));

	/* A refers to picture 0 with vector (0, 0): (0, 0) although B and C would predict otherwise. */
	begin(CURRENT);
	set_block(3, 0, 5, 0, 0, 0);
	set_block(1, 0, 10, 0, 6, 8);
	set_block(2, 0, 10, 0, 6, 8);
	mv[0] = 77;
	mv[1] = 77;
	h264sw_mvpred_pskip(&ctx, mv);
	CHECK(mv_is(mv, 0, 0));

	/* B refers to picture 0 with vector (0, 0): (0, 0). */
	begin(CURRENT);
	set_block(3, 0, 5, 0, 6, 8);
	set_block(1, 0, 10, 0, 0, 0);
	set_block(2, 0, 10, 0, 6, 8);
	mv[0] = 77;
	mv[1] = 77;
	h264sw_mvpred_pskip(&ctx, mv);
	CHECK(mv_is(mv, 0, 0));

	/* Neither case applies (A (1, 0), B (4, 5), C (-2, 9), all reference 0): the median (1, 5) of the 16x16 prediction. */
	begin(CURRENT);
	set_block(3, 0, 5, 0, 1, 0);
	set_block(1, 0, 10, 0, 4, 5);
	set_block(2, 0, 10, 0, -2, 9);
	h264sw_mvpred_pskip(&ctx, mv);
	CHECK(mv_is(mv, 1, 5));

	/* A refers to picture 1 with vector (0, 0), which is not the zero case: only B refers to picture 0, so its vector (4, 5) is taken. */
	begin(CURRENT);
	set_block(3, 0, 5, 1, 0, 0);
	set_block(1, 0, 10, 0, 4, 5);
	set_block(2, 0, 10, 2, 7, 7);
	h264sw_mvpred_pskip(&ctx, mv);
	CHECK(mv_is(mv, 4, 5));

	/* A is intra (refIdx -1, vector 0), which is not the zero case: B (2, 2) and C (6, -6) at reference 0 give the median of (0, 2, 6) and (0, 2, -6): (2, 0). */
	begin(CURRENT);
	mbs[3].motion.intra = 1;
	set_block(1, 0, 10, 0, 2, 2);
	set_block(2, 0, 10, 0, 6, -6);
	h264sw_mvpred_pskip(&ctx, mv);
	CHECK(mv_is(mv, 2, 0));

	/* A refers to picture 0 with (0, 1), not the zero vector: the median again, A (0, 1), B (4, 4), C (8, 8): (4, 4). */
	begin(CURRENT);
	set_block(3, 0, 5, 0, 0, 1);
	set_block(1, 0, 10, 0, 4, 4);
	set_block(2, 0, 10, 0, 8, 8);
	h264sw_mvpred_pskip(&ctx, mv);
	CHECK(mv_is(mv, 4, 4));

	/* B in another slice is not available: (0, 0). */
	begin(CURRENT);
	set_block(3, 0, 5, 0, 1, 1);
	mbs[1].slice = 1;
	neighbours(CURRENT);
	mv[0] = 77;
	mv[1] = 77;
	h264sw_mvpred_pskip(&ctx, mv);
	CHECK(mv_is(mv, 0, 0));
}

/* Checks that wrong inputs give the prediction (0, 0) or nothing, without a fault. */
static void
check_errors(void)
{
	int16_t mvp[2];
	struct h264sw_slice_ctx empty;

	/* Gives a context with motion that would predict (7, 7), so a refusal shows. */
	begin(CURRENT);
	set_block(3, 0, 5, 0, 7, 7);
	set_block(1, 0, 10, 0, 7, 7);
	set_block(2, 0, 10, 0, 7, 7);

	/* Sanity: the valid call predicts (7, 7). */
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 7, 7));

	/* A list above 1 is refused. */
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 2, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0xffffffffU, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));

	/* A negative reference index is refused, including the value that stands for an unused list. */
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, -1, mvp);
	CHECK(mv_is(mvp, 0, 0));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, -2147483647 - 1, mvp);
	CHECK(mv_is(mvp, 0, 0));

	/* A huge reference index is valid but matches nothing, so the median of the three equal vectors (7, 7) is given. */
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 2147483647, mvp);
	CHECK(mv_is(mvp, 7, 7));

	/* An empty rectangle is refused. */
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 0, 0, 0, 16, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 0, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));

	/* A rectangle that leaves the macroblock is refused, with small and with huge values. */
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 8, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 0, 8, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 16, 0, 4, 4, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 0xffffffffU, 0, 4, 4, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 0, 0xffffffffU, 4, 4, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 8, 8, 0xffffffffU, 8, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 8, 8, 8, 0xffffffffU, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&ctx, 0, 0xfffffff0U, 0xfffffff0U, 0x20U, 0x20U, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));

	/* A missing context and a context without a current macroblock are refused. */
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(NULL, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));
	memset(&empty, 0, sizeof(empty));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred(&empty, 0, 0, 0, 16, 16, 0, mvp);
	CHECK(mv_is(mvp, 0, 0));

	/* A missing output is tolerated. */
	h264sw_mvpred(&ctx, 0, 0, 0, 16, 16, 0, NULL);
	h264sw_mvpred_pskip(&ctx, NULL);
	h264sw_mvpred_pskip(NULL, NULL);

	/* P_Skip with a missing context gives (0, 0); with an empty context (nothing available) as well. */
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred_pskip(NULL, mvp);
	CHECK(mv_is(mvp, 0, 0));
	mvp[0] = 55;
	mvp[1] = 55;
	h264sw_mvpred_pskip(&empty, mvp);
	CHECK(mv_is(mvp, 0, 0));
}
