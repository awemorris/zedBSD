/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The motion vector prediction of the software H.264 decoder (6.4.11.7,
 * 8.4.1.1, 8.4.1.3).
 */

#include "h264sw-mvpred.h"
#include "h264sw-context.h"

#include <stddef.h>

/* The width and the height of a macroblock in luma samples (maxW and maxH of 6.4.12). */
#define MVPRED_MB_SIZE 16U

/*
 * What the motion data of one neighbouring partition says (8.4.1.3.2).
 *
 * available is 1 when the partition is available in the sense of
 * 6.4.11.7, whether or not it holds a motion vector.  ref_idx is
 * refIdxLXN and mv is mvLXN ([0] horizontal, [1] vertical, quarter luma
 * samples); they are -1 and 0 when the partition is not available, is
 * intra, or does not use the list.  One instance lives in the stack frame
 * of the function that derives a prediction.
 */
struct mvpred_neighbour {
	int available;
	int ref_idx;
	int mv[2];
};

/* Forward declarations of the static functions. */
static void mvpred_neighbour_at(const struct h264sw_slice_ctx *ctx, unsigned list, int xn, int yn, struct mvpred_neighbour *neighbour);
static int mvpred_median(int first, int second, int third);

/*
 * Predicts the motion vector of a partition (8.4.1.3).
 *
 * An input that cannot describe a partition of the current macroblock
 * (a missing context, a list above 1, a negative reference index, an
 * empty rectangle or one that leaves the macroblock) gives a prediction
 * of 0 and 0.
 */
void
h264sw_mvpred(
	const struct h264sw_slice_ctx *ctx,
	unsigned list,
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height,
	int ref_idx,
	int16_t *mvp)
{
	struct mvpred_neighbour a;
	struct mvpred_neighbour b;
	struct mvpred_neighbour c;
	struct mvpred_neighbour d;
	int matches;
	int component;
	int found_mv[2];

	/* Ignores a missing output. */
	if (mvp == NULL)
		return;

	/* Starts from the prediction of 0, which stands for every input that is refused. */
	mvp[0] = 0;
	mvp[1] = 0;

	/* Refuses a missing context or a context without a current macroblock. */
	if (ctx == NULL)
		return;
	if (ctx->n.current == NULL)
		return;

	/* Refuses a list that is neither 0 nor 1 and a reference index that names no picture. */
	if (list > 1U)
		return;
	if (ref_idx < 0)
		return;

	/* Refuses a rectangle that is empty or does not lie inside the macroblock. */
	if (width == 0U || height == 0U)
		return;
	if (x >= MVPRED_MB_SIZE || y >= MVPRED_MB_SIZE)
		return;
	if (width > MVPRED_MB_SIZE - x)
		return;
	if (height > MVPRED_MB_SIZE - y)
		return;

	/* Derives the motion data of A, the partition holding the sample to the left of the upper-left sample. */
	mvpred_neighbour_at(ctx, list, (int)x - 1, (int)y, &a);

	/* Derives the motion data of B, the partition holding the sample above the upper-left sample. */
	mvpred_neighbour_at(ctx, list, (int)x, (int)y - 1, &b);

	/* Derives the motion data of C, the partition holding the sample above and to the right of the partition. */
	mvpred_neighbour_at(ctx, list, (int)(x + width), (int)y - 1, &c);

	/* Replaces C by D, the partition above and to the left, when C is not available (8-214 to 8-216). */
	if (!c.available) {
		mvpred_neighbour_at(ctx, list, (int)x - 1, (int)y - 1, &d);
		c = d;
	}

	/* Takes the vector of B for the upper 16x8 partition when B refers to the same picture (8-203). */
	if (width == 16U && height == 8U && y == 0U && b.ref_idx == ref_idx) {
		mvp[0] = (int16_t)b.mv[0];
		mvp[1] = (int16_t)b.mv[1];
		return;
	}

	/* Takes the vector of A for the lower 16x8 partition when A refers to the same picture (8-204). */
	if (width == 16U && height == 8U && y == 8U && a.ref_idx == ref_idx) {
		mvp[0] = (int16_t)a.mv[0];
		mvp[1] = (int16_t)a.mv[1];
		return;
	}

	/* Takes the vector of A for the left 8x16 partition when A refers to the same picture (8-205). */
	if (width == 8U && height == 16U && x == 0U && a.ref_idx == ref_idx) {
		mvp[0] = (int16_t)a.mv[0];
		mvp[1] = (int16_t)a.mv[1];
		return;
	}

	/* Takes the vector of C for the right 8x16 partition when C refers to the same picture (8-206). */
	if (width == 8U && height == 16U && x == 8U && c.ref_idx == ref_idx) {
		mvp[0] = (int16_t)c.mv[0];
		mvp[1] = (int16_t)c.mv[1];
		return;
	}

	/*
	 * Lets A stand for B and C when neither of them is available but A
	 * is (8-207 to 8-210), so that the vector of A alone decides.
	 */
	if (!b.available && !c.available && a.available) {
		b = a;
		c = a;
	}

	/* Counts how many of A, B and C refer to the same picture as the current partition and remembers the last such vector. */
	matches = 0;
	found_mv[0] = 0;
	found_mv[1] = 0;
	if (a.ref_idx == ref_idx) {
		matches++;
		found_mv[0] = a.mv[0];
		found_mv[1] = a.mv[1];
	}
	if (b.ref_idx == ref_idx) {
		matches++;
		found_mv[0] = b.mv[0];
		found_mv[1] = b.mv[1];
	}
	if (c.ref_idx == ref_idx) {
		matches++;
		found_mv[0] = c.mv[0];
		found_mv[1] = c.mv[1];
	}

	/* Takes the vector of the only neighbour that refers to the same picture (8-211). */
	if (matches == 1) {
		mvp[0] = (int16_t)found_mv[0];
		mvp[1] = (int16_t)found_mv[1];
		return;
	}

	/* Takes the median of each component of A, B and C otherwise (8-212 and 8-213). */
	for (component = 0; component < 2; component++) {
		mvp[component] = (int16_t)mvpred_median(a.mv[component], b.mv[component], c.mv[component]);
	}
}

/*
 * Derives the motion vector of a P_Skip macroblock (8.4.1.1).
 *
 * A missing context or output gives nothing (a missing context leaves
 * *mv at 0 and 0).
 */
void
h264sw_mvpred_pskip(
	const struct h264sw_slice_ctx *ctx,
	int16_t *mv)
{
	struct mvpred_neighbour a;
	struct mvpred_neighbour b;

	/* Ignores a missing output. */
	if (mv == NULL)
		return;

	/* Starts from the vector 0, the result of every case that does not predict. */
	mv[0] = 0;
	mv[1] = 0;

	/* Refuses a missing context. */
	if (ctx == NULL)
		return;

	/* Derives the motion data of A, the partition to the left of the macroblock treated as one 16x16 partition of list 0. */
	mvpred_neighbour_at(ctx, 0, -1, 0, &a);

	/* Derives the motion data of B, the partition above the macroblock. */
	mvpred_neighbour_at(ctx, 0, 0, -1, &b);

	/* Keeps the vector 0 when the macroblock on the left is not available. */
	if (!a.available)
		return;

	/* Keeps the vector 0 when the macroblock above is not available. */
	if (!b.available)
		return;

	/* Keeps the vector 0 when A refers to the first picture without motion. */
	if (a.ref_idx == 0 && a.mv[0] == 0 && a.mv[1] == 0)
		return;

	/* Keeps the vector 0 when B refers to the first picture without motion. */
	if (b.ref_idx == 0 && b.mv[0] == 0 && b.mv[1] == 0)
		return;

	/* Predicts the vector of the whole macroblock for the first reference picture. */
	h264sw_mvpred(ctx, 0, 0, 0, MVPRED_MB_SIZE, MVPRED_MB_SIZE, 0, mv);
}

/*
 * Derives the motion data of the partition holding a luma location (8.4.1.3.2).
 *
 * The location is relative to the upper-left sample of the current
 * macroblock.  A location inside the current macroblock counts only
 * when its 4x4 block is in ctx->decoded_4x4 (6.4.11.7, step 7c).
 */
static void
mvpred_neighbour_at(
	const struct h264sw_slice_ctx *ctx,
	unsigned list,
	int xn,
	int yn,
	struct mvpred_neighbour *neighbour)
{
	struct h264sw_mb *found;
	unsigned xw;
	unsigned yw;
	unsigned blk;
	int ref_idx;

	/* Starts with the answer for a partition that is not available. */
	neighbour->available = 0;
	neighbour->ref_idx = -1;
	neighbour->mv[0] = 0;
	neighbour->mv[1] = 0;

	/* Finds the macroblock holding the location, as Table 6-3 says. */
	found = h264sw_mb_locate(&ctx->n, xn, yn, MVPRED_MB_SIZE, MVPRED_MB_SIZE, &xw, &yw);
	if (found == NULL)
		return;

	/* Finds the 4x4 block of that macroblock holding the location. */
	blk = h264sw_blk4x4_at(xw, yw);

	/* Treats a block of the current macroblock whose motion is not derived yet as not available. */
	if (found == ctx->n.current) {
		if ((ctx->decoded_4x4 & ((uint32_t)1 << blk)) == 0U)
			return;
	}

	/* The partition is available from here on, whatever it holds. */
	neighbour->available = 1;

	/* Leaves -1 and 0 for an intra macroblock. */
	if (found->motion.intra != 0U)
		return;

	/* Leaves -1 and 0 for a partition that does not use the list (predFlagLX 0). */
	ref_idx = found->motion.ref_idx[list][blk / 4U];
	if (ref_idx < 0)
		return;

	/* Takes the reference index of the 8x8 quadrant and the vector of the 4x4 block. */
	neighbour->ref_idx = ref_idx;
	neighbour->mv[0] = found->motion.mv[list][blk][0];
	neighbour->mv[1] = found->motion.mv[list][blk][1];
}

/*
 * Gives the median of three values: the one that is neither the smallest nor the largest.
 *
 * Median( x, y, z ) of equation 5-15 is x + y + z - Min( x, Min( y, z ) ) - Max( x, Max( y, z ) ).
 */
static int
mvpred_median(
	int first,
	int second,
	int third)
{
	int smallest;
	int largest;

	/* Finds the smallest of the three. */
	smallest = first;
	if (second < smallest)
		smallest = second;
	if (third < smallest)
		smallest = third;

	/* Finds the largest of the three. */
	largest = first;
	if (second > largest)
		largest = second;
	if (third > largest)
		largest = third;

	/* Gives the value left when the smallest and the largest are taken away. */
	return first + second + third - smallest - largest;
}
