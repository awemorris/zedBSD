/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the fractional sample interpolation of the software H.264
 * decoder (8.4.2.2): the sixteen luma positions of Table 8-12 with the
 * six-tap filter, the chroma bilinear interpolation of 8-270, the clamping
 * of the reference positions (8-239, 8-240, 8-262 to 8-269) and the
 * handling of hostile input.  The expected values are worked out by hand
 * from the equations (impulse responses of the filter, linear ramps, which
 * the filter reproduces exactly) or follow from the clamping rules, never
 * from another decoder's output.  The planes are allocated with exactly
 * their size, so a read outside one is reported by the address sanitizer.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "userland/desktop/libmedia/h264sw-interp.h"

/* The value the output buffers are filled with, so a write outside the block is seen. */
#define SENTINEL 0xA5U

/*
 * The state of the pseudo-random generator that gives the tests their
 * planes: a linear congruential generator, so the planes are the same on
 * every run.
 */
static uint32_t rng_state = 12345U;

/* Forward declarations of the static functions. */
static uint32_t rng_next(void);
static uint8_t *plane_new(unsigned width, unsigned height);
static void predict_luma(const uint8_t *plane, unsigned width, unsigned height, int x, int y, unsigned frac_x, unsigned frac_y, unsigned block_w, unsigned block_h, uint8_t *out);
static void check_luma_table(const uint8_t *plane, unsigned width, unsigned height, int x, int y, const uint8_t *table);
static void test_luma_impulse(void);
static void test_luma_clip(void);
static void test_luma_ramp(void);
static void test_luma_far_outside(void);
static void test_luma_partial_edge(void);
static void test_luma_refused(void);
static void test_chroma_corners(void);
static void test_chroma_ramp(void);
static void test_chroma_clamp(void);
static void test_chroma_refused(void);
static void test_fuzz(void);

/* Gives the next pseudo-random 32-bit value. */
static uint32_t
rng_next(void)
{
	/* Steps the generator and reports its high bits, which are the better ones. */
	rng_state = rng_state * 1664525U + 1013904223U;
	return rng_state >> 8;
}

/* Allocates a plane of exactly width by height samples, all 0, with the stride equal to the width. */
static uint8_t *
plane_new(
	unsigned width,
	unsigned height)
{
	uint8_t *plane;

	/* Allocates exactly the plane, so that any read past it is a sanitizer report. */
	plane = (uint8_t *)malloc((size_t)width * height);
	CHECK(plane != NULL);
	if (plane == NULL)
		exit(1);

	/* Clears the plane. */
	memset(plane, 0, (size_t)width * height);

	/* Reports the plane. */
	return plane;
}

/*
 * Predicts a luma block into a buffer of exactly its size and copies it to out, a tight block.
 *
 * The block is written with the stride equal to the block width.
 */
static void
predict_luma(
	const uint8_t *plane,
	unsigned width,
	unsigned height,
	int x,
	int y,
	unsigned frac_x,
	unsigned frac_y,
	unsigned block_w,
	unsigned block_h,
	uint8_t *out)
{
	uint8_t *exact;

	/* Predicts into a buffer of exactly the block, so a write past it is a sanitizer report. */
	exact = (uint8_t *)malloc((size_t)block_w * block_h);
	CHECK(exact != NULL);
	if (exact == NULL)
		exit(1);
	h264sw_interp_luma(plane, width, width, height, x, y, frac_x, frac_y, block_w, block_h, exact, block_w);

	/* Hands the block back. */
	memcpy(out, exact, (size_t)block_w * block_h);
	free(exact);
}

/*
 * Checks the sixteen positions of one sample against a table.
 *
 * table[ xFracL * 4 + yFracL ] is the expected sample of the 1x1 block
 * whose full-sample location G is ( x, y ).
 */
static void
check_luma_table(
	const uint8_t *plane,
	unsigned width,
	unsigned height,
	int x,
	int y,
	const uint8_t *table)
{
	unsigned frac_x;
	unsigned frac_y;
	uint8_t sample;

	/* Predicts the sample at each of the sixteen positions. */
	for (frac_x = 0U; frac_x < 4U; frac_x++) {
		for (frac_y = 0U; frac_y < 4U; frac_y++) {
			predict_luma(plane, width, height, x, y, frac_x, frac_y, 1U, 1U, &sample);
			CHECK(sample == table[frac_x * 4U + frac_y]);
		}
	}
}

/*
 * Checks the filter by its impulse responses.
 *
 * The plane is 9x9 and 0 except one sample of 100 at ( 4, 4 ).  The
 * six-tap filter of 8.4.2.2.1 gives the impulse the tap of its place in
 * the row or column: 1, -5, 20, 20, -5, 1.  So a 100 at the tap 20 gives
 * b1 = 2000 and b = ( 2000 + 16 ) >> 5 = 63; at the tap 1 gives b1 = 100
 * and b = ( 100 + 16 ) >> 5 = 3; at a tap -5 gives a negative value that
 * clips to 0.  j1 filters the vertical intermediate values, so a 100 at
 * the centre gives j1 = 20 * 2000 = 40000 and j = ( 40000 + 512 ) >> 10 =
 * 39, and a 100 at the tap 1 of the columns gives j1 = 2000 and j = 2.
 * The quarter positions then follow 8-250 to 8-261.
 */
static void
test_luma_impulse(void)
{
	uint8_t *plane;
	uint8_t sample;

	/*
	 * The impulse is G: b = h = 63, j = 39, s = m = 0, H = M = 0.
	 * a = ( 100 + 63 + 1 ) >> 1 = 82 = d, e = ( 63 + 63 + 1 ) >> 1 = 63,
	 * i = f = ( 63 + 39 + 1 ) >> 1 = 51, p = ( 63 + 0 + 1 ) >> 1 = 32 = c = n = g,
	 * q = k = ( 39 + 0 + 1 ) >> 1 = 20, r = ( 0 + 0 + 1 ) >> 1 = 0.
	 * The table is indexed by xFracL * 4 + yFracL (Table 8-12 reads G d h n, a e i p, b f j q, c g k r).
	 */
	static const uint8_t at_g[16] = {
		100, 82, 63, 32,
		82, 63, 51, 32,
		63, 51, 39, 20,
		32, 32, 20, 0
	};

	/*
	 * The impulse is H (G is one sample to its left): b = m = 63, j = 39, h = 0, s = 0, G = M = 0.
	 * a = ( 0 + 63 + 1 ) >> 1 = 32, c = ( 100 + 63 + 1 ) >> 1 = 82,
	 * e = ( 63 + 0 + 1 ) >> 1 = 32, i = ( 0 + 39 + 1 ) >> 1 = 20, p = ( 0 + 0 + 1 ) >> 1 = 0,
	 * f = ( 63 + 39 + 1 ) >> 1 = 51, q = ( 39 + 0 + 1 ) >> 1 = 20,
	 * g = ( 63 + 63 + 1 ) >> 1 = 63, k = ( 39 + 63 + 1 ) >> 1 = 51, r = ( 63 + 0 + 1 ) >> 1 = 32.
	 */
	static const uint8_t at_h[16] = {
		0, 0, 0, 0,
		32, 32, 20, 0,
		63, 51, 39, 20,
		82, 63, 51, 32
	};

	/*
	 * The impulse is E (G is two samples to its right), the tap 1: b = 3, j = 2, h = m = s = 0.
	 * a = ( 0 + 3 + 1 ) >> 1 = 2, e = ( 3 + 0 + 1 ) >> 1 = 2, i = ( 0 + 2 + 1 ) >> 1 = 1,
	 * f = ( 3 + 2 + 1 ) >> 1 = 3, q = ( 2 + 0 + 1 ) >> 1 = 1, c = g = ( 3 + 0 + 1 ) >> 1 = 2,
	 * k = ( 2 + 0 + 1 ) >> 1 = 1, p = r = ( 0 + 0 + 1 ) >> 1 = 0.
	 */
	static const uint8_t at_e[16] = {
		0, 0, 0, 0,
		2, 2, 1, 0,
		3, 3, 2, 1,
		2, 2, 1, 0
	};

	/*
	 * The impulse is A (G is two samples below it), the tap 1 vertically: h = 3, j = 2, b = m = s = 0.
	 * d = n = ( 0 + 3 + 1 ) >> 1 = 2, e = ( 0 + 3 + 1 ) >> 1 = 2, i = ( 3 + 2 + 1 ) >> 1 = 3,
	 * p = ( 3 + 0 + 1 ) >> 1 = 2, f = q = ( 0 + 2 + 1 ) >> 1 = 1 and ( 2 + 0 + 1 ) >> 1 = 1,
	 * k = ( 2 + 0 + 1 ) >> 1 = 1, a = c = g = r = ( 0 + 0 + 1 ) >> 1 = 0.
	 */
	static const uint8_t at_a[16] = {
		0, 2, 3, 2,
		0, 2, 3, 2,
		0, 1, 2, 1,
		0, 0, 1, 0
	};

	/* Makes the plane with its single impulse. */
	plane = plane_new(9U, 9U);
	plane[4 * 9 + 4] = 100U;

	/* Checks the impulse at G, at H, at E and at A. */
	check_luma_table(plane, 9U, 9U, 4, 4, at_g);
	check_luma_table(plane, 9U, 9U, 3, 4, at_h);
	check_luma_table(plane, 9U, 9U, 6, 4, at_e);
	check_luma_table(plane, 9U, 9U, 4, 6, at_a);

	/* An impulse at a tap of -5 (F, one sample to the left of G) gives only clipped 0 for b, and 0 elsewhere. */
	predict_luma(plane, 9U, 9U, 5, 4, 2U, 0U, 1U, 1U, &sample);
	CHECK(sample == 0U);

	free(plane);
}

/*
 * Checks the clipping of the half samples to 0..255 (Clip1Y of 8-243, 8-247).
 *
 * The plane is 9x9 and 0 except 255 at ( 4, 4 ) and ( 5, 4 ).
 */
static void
test_luma_clip(void)
{
	uint8_t *plane;
	uint8_t sample;

	/* Makes the plane with two bright samples side by side. */
	plane = plane_new(9U, 9U);
	plane[4 * 9 + 4] = 255U;
	plane[4 * 9 + 5] = 255U;

	/* G = ( 4, 4 ): b1 = 20 * 255 + 20 * 255 = 10200, ( 10200 + 16 ) >> 5 = 319 clips to 255. */
	predict_luma(plane, 9U, 9U, 4, 4, 2U, 0U, 1U, 1U, &sample);
	CHECK(sample == 255U);

	/* The same G: h1 = 20 * 255 = 5100 and m1 = 5100, j1 = 20 * 5100 + 20 * 5100 = 204000, ( 204000 + 512 ) >> 10 = 199. */
	predict_luma(plane, 9U, 9U, 4, 4, 2U, 2U, 1U, 1U, &sample);
	CHECK(sample == 199U);

	/* G = ( 3, 4 ): the samples E to J are 0, 0, 0, 255, 255, 0, so b1 = 20 * 255 - 5 * 255 = 3825, ( 3825 + 16 ) >> 5 = 120. */
	predict_luma(plane, 9U, 9U, 3, 4, 2U, 0U, 1U, 1U, &sample);
	CHECK(sample == 120U);

	/* G = ( 2, 4 ): the samples are 0, 0, 0, 0, 255, 255 with the taps 1, -5, 20, 20, -5, 1: b1 = -5 * 255 + 255 = -1020 clips to 0. */
	predict_luma(plane, 9U, 9U, 2, 4, 2U, 0U, 1U, 1U, &sample);
	CHECK(sample == 0U);

	/*
	 * The rounding constants sit exactly on their boundaries for suitable impulses.
	 * An impulse of 32 at G: h1 = 20 * 32 = 640, j1 = 20 * 640 = 12800, and ( 12800 + 512 ) >> 10 = 13312 >> 10 = 13 exactly (a constant of 511 would give 12).
	 */
	free(plane);
	plane = plane_new(9U, 9U);
	plane[4 * 9 + 4] = 32U;
	predict_luma(plane, 9U, 9U, 4, 4, 2U, 2U, 1U, 1U, &sample);
	CHECK(sample == 13U);

	/* An impulse of 4 at G: b1 = 20 * 4 = 80, and ( 80 + 16 ) >> 5 = 3 exactly (a constant of 15 would give 2); h likewise. */
	free(plane);
	plane = plane_new(9U, 9U);
	plane[4 * 9 + 4] = 4U;
	predict_luma(plane, 9U, 9U, 4, 4, 2U, 0U, 1U, 1U, &sample);
	CHECK(sample == 3U);
	predict_luma(plane, 9U, 9U, 4, 4, 0U, 2U, 1U, 1U, &sample);
	CHECK(sample == 3U);

	/* The vertical filter clips alike: the same pair turned on its side gives 120 and 0 for h. */
	free(plane);
	plane = plane_new(9U, 9U);
	plane[4 * 9 + 4] = 255U;
	plane[5 * 9 + 4] = 255U;
	predict_luma(plane, 9U, 9U, 4, 3, 0U, 2U, 1U, 1U, &sample);
	CHECK(sample == 120U);
	predict_luma(plane, 9U, 9U, 4, 2, 0U, 2U, 1U, 1U, &sample);
	CHECK(sample == 0U);
	predict_luma(plane, 9U, 9U, 4, 4, 0U, 2U, 1U, 1U, &sample);
	CHECK(sample == 255U);

	free(plane);
}

/*
 * Checks every position against a linear ramp, which the filter reproduces.
 *
 * With the plane 8 * x + 4 * y the half samples are exact (b = G + 4,
 * h = G + 2, j = G + 6, s = G + 8, m = G + 10), and with the averages of
 * 8-250 to 8-261 every position gives G + 2 * xFracL + yFracL: for example
 * a = ( G + G + 4 + 1 ) >> 1 = G + 2, r = ( G + 10 + G + 8 + 1 ) >> 1 = G + 9.
 * Blocks of 8x8 and 16x16, and the copy of a whole block at the full-sample position, are checked.
 */
static void
test_luma_ramp(void)
{
	uint8_t *plane;
	uint8_t *block;
	unsigned side;
	unsigned frac_x;
	unsigned frac_y;
	unsigned row;
	unsigned col;
	unsigned expect;
	unsigned size;
	unsigned plane_side;
	int wrong;

	/* Checks the 8x8 block of a 13x13 plane (the block's window reaches x = 2 + 7 + 3 = 12), then the 16x16 block of a 21x21 plane. */
	for (side = 0U; side < 2U; side++) {
		size = 8U;
		plane_side = 13U;
		if (side == 1U) {
			size = 16U;
			plane_side = 21U;
		}

		/* Makes the ramp plane. */
		plane = plane_new(plane_side, plane_side);
		for (row = 0U; row < plane_side; row++) {
			for (col = 0U; col < plane_side; col++)
				plane[row * plane_side + col] = (uint8_t)(8U * col + 4U * row);
		}

		block = (uint8_t *)malloc((size_t)size * size);
		CHECK(block != NULL);

		/* Predicts the block at each position and compares each sample with G + 2 * xFracL + yFracL. */
		for (frac_x = 0U; frac_x < 4U; frac_x++) {
			for (frac_y = 0U; frac_y < 4U; frac_y++) {
				predict_luma(plane, plane_side, plane_side, 2, 2, frac_x, frac_y, size, size, block);
				wrong = 0;
				for (row = 0U; row < size; row++) {
					for (col = 0U; col < size; col++) {
						expect = 8U * (2U + col) + 4U * (2U + row) + 2U * frac_x + frac_y;
						if (block[row * size + col] != (uint8_t)expect)
							wrong = 1;
					}
				}
				CHECK(!wrong);
			}
		}

		free(block);
		free(plane);
	}
}

/*
 * Checks that a vector far outside the plane repeats the edge samples.
 *
 * With every sample of the filter's rows or columns equal, the half
 * samples equal the full sample (the taps add up to 32, so ( 32 * G + 16 )
 * >> 5 = G) and so does j.  A block beyond the left or right edge is
 * therefore the same at all four xFracL, and one beyond the top or bottom
 * the same at all four yFracL, and a block beyond a corner is that corner
 * sample.
 */
static void
test_luma_far_outside(void)
{
	static const int far_values[4] = { -1000, 1000, INT_MIN, INT_MAX };
	enum { PW = 12, PH = 10 };
	uint8_t *plane;
	uint8_t base[16];
	uint8_t other[16];
	unsigned index;
	unsigned frac_x;
	unsigned frac_y;
	unsigned i;
	int far_value;
	int inside;
	int same;

	/* Fills the plane with random samples. */
	plane = plane_new(PW, PH);
	for (i = 0U; i < (unsigned)(PW * PH); i++)
		plane[i] = (uint8_t)rng_next();

	/* Tries each far value on each side. */
	for (index = 0U; index < 4U; index++) {
		far_value = far_values[index];

		/* Left or right of the plane (far x), at a row inside: all xFracL give the xFracL 0 block, whose columns are the edge column. */
		inside = 3;
		for (frac_y = 0U; frac_y < 4U; frac_y++) {
			predict_luma(plane, PW, PH, far_value, inside, 0U, frac_y, 4U, 4U, base);
			for (frac_x = 1U; frac_x < 4U; frac_x++) {
				predict_luma(plane, PW, PH, far_value, inside, frac_x, frac_y, 4U, 4U, other);
				same = memcmp(base, other, 16U) == 0;
				CHECK(same);
			}
		}

		/* Above or below the plane (far y), at a column inside: all yFracL give the yFracL 0 block. */
		inside = 5;
		for (frac_x = 0U; frac_x < 4U; frac_x++) {
			predict_luma(plane, PW, PH, inside, far_value, frac_x, 0U, 4U, 4U, base);
			for (frac_y = 1U; frac_y < 4U; frac_y++) {
				predict_luma(plane, PW, PH, inside, far_value, frac_x, frac_y, 4U, 4U, other);
				same = memcmp(base, other, 16U) == 0;
				CHECK(same);
			}
		}

		/* The block at an x far to the left with the whole fraction ( 0, 0 ) is the edge column. */
		predict_luma(plane, PW, PH, far_value, 3, 0U, 0U, 4U, 4U, base);
		for (i = 0U; i < 16U; i++) {
			if (far_value < 0)
				CHECK(base[i] == plane[(3U + i / 4U) * PW + 0U]);
			else
				CHECK(base[i] == plane[(3U + i / 4U) * PW + (PW - 1U)]);
		}
	}

	/* A block beyond a corner is that corner sample at every position. */
	for (frac_x = 0U; frac_x < 4U; frac_x++) {
		for (frac_y = 0U; frac_y < 4U; frac_y++) {
			predict_luma(plane, PW, PH, -1000, -1000, frac_x, frac_y, 4U, 4U, base);
			for (i = 0U; i < 16U; i++)
				CHECK(base[i] == plane[0]);

			predict_luma(plane, PW, PH, 1000, -1000, frac_x, frac_y, 4U, 4U, base);
			for (i = 0U; i < 16U; i++)
				CHECK(base[i] == plane[PW - 1]);

			predict_luma(plane, PW, PH, -1000, 1000, frac_x, frac_y, 4U, 4U, base);
			for (i = 0U; i < 16U; i++)
				CHECK(base[i] == plane[(PH - 1) * PW]);

			predict_luma(plane, PW, PH, 1000, 1000, frac_x, frac_y, 4U, 4U, base);
			for (i = 0U; i < 16U; i++)
				CHECK(base[i] == plane[PH * PW - 1]);
		}
	}

	free(plane);
}

/*
 * Checks blocks that overlap the edge of the plane against a plane padded by repetition.
 *
 * Clamping a position (8-239, 8-240) is the same as reading a plane
 * whose border repeats the edge samples.  The padded plane has a border
 * of 24 samples, more than any block and filter reach here, so the
 * prediction made on it with the shifted location never clamps.
 */
static void
test_luma_partial_edge(void)
{
	enum { PW = 11, PH = 9, BORDER = 24 };
	uint8_t *plane;
	uint8_t *padded;
	uint8_t from_plane[64];
	uint8_t from_padded[64];
	unsigned sizes[3];
	unsigned size_index;
	unsigned size;
	unsigned frac_x;
	unsigned frac_y;
	unsigned row;
	unsigned col;
	int x;
	int y;
	int same;
	int source_x;
	int source_y;

	/* Fills the plane with random samples. */
	plane = plane_new(PW, PH);
	for (row = 0U; row < (unsigned)(PW * PH); row++)
		plane[row] = (uint8_t)rng_next();

	/* Builds the padded plane: each sample is the plane's sample at the clamped place. */
	padded = plane_new(PW + 2 * BORDER, PH + 2 * BORDER);
	for (row = 0U; row < (unsigned)(PH + 2 * BORDER); row++) {
		for (col = 0U; col < (unsigned)(PW + 2 * BORDER); col++) {
			source_x = (int)col - BORDER;
			source_y = (int)row - BORDER;
			if (source_x < 0)
				source_x = 0;
			if (source_x > PW - 1)
				source_x = PW - 1;
			if (source_y < 0)
				source_y = 0;
			if (source_y > PH - 1)
				source_y = PH - 1;
			padded[row * (PW + 2 * BORDER) + col] = plane[source_y * PW + source_x];
		}
	}

	sizes[0] = 2U;
	sizes[1] = 4U;
	sizes[2] = 8U;

	/* Compares the two predictions for locations from beyond the left and top edges to beyond the right and bottom ones. */
	for (size_index = 0U; size_index < 3U; size_index++) {
		size = sizes[size_index];
		for (y = -9; y <= PH + 3; y += 2) {
			for (x = -9; x <= PW + 3; x += 3) {
				for (frac_x = 0U; frac_x < 4U; frac_x++) {
					for (frac_y = 0U; frac_y < 4U; frac_y++) {
						predict_luma(plane, PW, PH, x, y, frac_x, frac_y, size, size, from_plane);
						predict_luma(padded, PW + 2 * BORDER, PH + 2 * BORDER, x + BORDER, y + BORDER, frac_x, frac_y, size, size, from_padded);
						same = memcmp(from_plane, from_padded, (size_t)size * size) == 0;
						CHECK(same);
					}
				}
			}
		}
	}

	free(padded);
	free(plane);
}

/*
 * Checks the calls that cannot describe a block: nothing is written and nothing is read.
 */
static void
test_luma_refused(void)
{
	uint8_t *plane;
	uint8_t out[64];
	uint8_t expected[64];
	uint8_t masked[16];
	uint8_t direct[16];
	unsigned i;
	int same;

	plane = plane_new(8U, 8U);
	for (i = 0U; i < 64U; i++)
		plane[i] = (uint8_t)(i * 3U);

	/* A width or height of 0 or above 16 writes nothing. */
	memset(out, (int)SENTINEL, sizeof(out));
	memset(expected, (int)SENTINEL, sizeof(expected));
	h264sw_interp_luma(plane, 8U, 8U, 8U, 0, 0, 0U, 0U, 0U, 4U, out, 8U);
	h264sw_interp_luma(plane, 8U, 8U, 8U, 0, 0, 0U, 0U, 4U, 0U, out, 8U);
	h264sw_interp_luma(plane, 8U, 8U, 8U, 0, 0, 0U, 0U, 17U, 4U, out, 8U);
	h264sw_interp_luma(plane, 8U, 8U, 8U, 0, 0, 0U, 0U, 4U, 17U, out, 8U);
	h264sw_interp_luma(plane, 8U, 8U, 8U, 0, 0, 0U, 0U, 0xffffffffU, 0xffffffffU, out, 8U);
	same = memcmp(out, expected, sizeof(out)) == 0;
	CHECK(same);

	/* A plane without samples or a missing plane or output writes nothing and does not crash. */
	h264sw_interp_luma(plane, 8U, 0U, 8U, 0, 0, 0U, 0U, 4U, 4U, out, 8U);
	h264sw_interp_luma(plane, 8U, 8U, 0U, 0, 0, 0U, 0U, 4U, 4U, out, 8U);
	h264sw_interp_luma(NULL, 8U, 8U, 8U, 0, 0, 0U, 0U, 4U, 4U, out, 8U);
	h264sw_interp_luma(plane, 8U, 8U, 8U, 0, 0, 0U, 0U, 4U, 4U, NULL, 8U);
	same = memcmp(out, expected, sizeof(out)) == 0;
	CHECK(same);

	/* A block writes only its own samples: a 4x2 block in rows of 8 leaves the other 48 bytes alone. */
	memset(out, (int)SENTINEL, sizeof(out));
	h264sw_interp_luma(plane, 8U, 8U, 8U, 1, 1, 0U, 0U, 4U, 2U, out, 8U);
	for (i = 0U; i < 16U; i++) {
		if (i % 8U < 4U)
			CHECK(out[i] == plane[(1U + i / 8U) * 8U + 1U + i % 8U]);
		else
			CHECK(out[i] == SENTINEL);
	}
	for (i = 16U; i < 64U; i++)
		CHECK(out[i] == SENTINEL);

	/* A fractional part above 3 is taken modulo 4, as the mask of 8-225 and 8-226 does. */
	predict_luma(plane, 8U, 8U, 2, 2, 5U, 7U, 4U, 4U, masked);
	predict_luma(plane, 8U, 8U, 2, 2, 1U, 3U, 4U, 4U, direct);
	same = memcmp(masked, direct, 16U) == 0;
	CHECK(same);
	predict_luma(plane, 8U, 8U, 2, 2, 0xfffffffeU, 0xffffffffU, 4U, 4U, masked);
	predict_luma(plane, 8U, 8U, 2, 2, 2U, 3U, 4U, 4U, direct);
	same = memcmp(masked, direct, 16U) == 0;
	CHECK(same);

	/* A one-sample plane gives that sample at every position. */
	free(plane);
	plane = plane_new(1U, 1U);
	plane[0] = 77U;
	predict_luma(plane, 1U, 1U, -3, 5, 3U, 1U, 4U, 4U, direct);
	for (i = 0U; i < 16U; i++)
		CHECK(direct[i] == 77U);

	free(plane);
}

/*
 * Checks the four corners and the middle of an 8x8 chroma block by hand.
 *
 * The 8x8 plane is 3 * x + 17 * y + 5.  The block is at ( 0, 0 ) with
 * xFracC 3 and yFracC 5, so the weights of 8-270 are A 5 * 3 = 15,
 * B 3 * 3 = 9, C 5 * 5 = 25 and D 3 * 5 = 15, which add up to 64.
 *
 * ( 0, 0 ): A = 5, B = 8, C = 22, D = 25: ( 75 + 72 + 550 + 375 + 32 ) >> 6 = 1104 >> 6 = 17.
 * ( 4, 4 ): A = 85, B = 88, C = 102, D = 105: ( 1275 + 792 + 2550 + 1575 + 32 ) >> 6 = 6224 >> 6 = 97.
 * ( 7, 0 ): A = B = 26, C = D = 43 (the column 8 clamps to 7): ( 390 + 234 + 1075 + 645 + 32 ) >> 6 = 2376 >> 6 = 37.
 * ( 0, 7 ): A = C = 124, B = D = 127 (the row 8 clamps to 7): ( 1860 + 1143 + 3100 + 1905 + 32 ) >> 6 = 8040 >> 6 = 125.
 * ( 7, 7 ): all four are 145 (both clamp): ( 64 * 145 + 32 ) >> 6 = 145.
 */
static void
test_chroma_corners(void)
{
	uint8_t plane[64];
	uint8_t block[64];
	unsigned row;
	unsigned col;

	/* Makes the plane. */
	for (row = 0U; row < 8U; row++) {
		for (col = 0U; col < 8U; col++)
			plane[row * 8U + col] = (uint8_t)(3U * col + 17U * row + 5U);
	}

	/* Predicts the whole 8x8 block. */
	memset(block, 0, sizeof(block));
	h264sw_interp_chroma(plane, 8U, 8U, 8U, 0, 0, 3U, 5U, 8U, 8U, block, 8U);

	/* Compares the corners and the middle with the hand-worked values. */
	CHECK(block[0 * 8 + 0] == 17U);
	CHECK(block[4 * 8 + 4] == 97U);
	CHECK(block[0 * 8 + 7] == 37U);
	CHECK(block[7 * 8 + 0] == 125U);
	CHECK(block[7 * 8 + 7] == 145U);

	/* The full-sample position ( 0, 0 ) copies the plane: ( 64 * A + 32 ) >> 6 = A. */
	memset(block, 0, sizeof(block));
	h264sw_interp_chroma(plane, 8U, 8U, 8U, 0, 0, 0U, 0U, 8U, 8U, block, 8U);
	CHECK(memcmp(block, plane, 64U) == 0);
}

/*
 * Checks all 64 fractional positions of the chroma interpolation on the ramp.
 *
 * On the plane 3 * x + 17 * y + 5 the neighbours are B = A + 3, C = A + 17
 * and D = A + 20, so the numerator of 8-270 is 64 * A + 24 * xFracC + 136
 * * yFracC (the products of the two fractions cancel: -3 - 17 + 20 = 0),
 * and the sample is A + ( ( 24 * xFracC + 136 * yFracC + 32 ) >> 6 ).  A
 * 2x2 block at ( 1, 1 ) reads only inside the plane.
 */
static void
test_chroma_ramp(void)
{
	uint8_t plane[64];
	uint8_t block[4];
	unsigned frac_x;
	unsigned frac_y;
	unsigned row;
	unsigned col;
	unsigned expect;
	int wrong;

	/* Makes the plane. */
	for (row = 0U; row < 8U; row++) {
		for (col = 0U; col < 8U; col++)
			plane[row * 8U + col] = (uint8_t)(3U * col + 17U * row + 5U);
	}

	/* Predicts the 2x2 block at each fractional position and compares it with the formula. */
	for (frac_x = 0U; frac_x < 8U; frac_x++) {
		for (frac_y = 0U; frac_y < 8U; frac_y++) {
			h264sw_interp_chroma(plane, 8U, 8U, 8U, 1, 1, frac_x, frac_y, 2U, 2U, block, 2U);
			wrong = 0;
			for (row = 0U; row < 2U; row++) {
				for (col = 0U; col < 2U; col++) {
					expect = 3U * (1U + col) + 17U * (1U + row) + 5U + ((24U * frac_x + 136U * frac_y + 32U) >> 6);
					if (block[row * 2U + col] != (uint8_t)expect)
						wrong = 1;
				}
			}
			CHECK(!wrong);
		}
	}
}

/*
 * Checks the clamping of chroma positions (8-262 to 8-269).
 *
 * Far to the left every sample of a row is the edge sample, so B = A and
 * the result is ( ( 8 - yFracC ) * 8 * A + yFracC * 8 * C + 32 ) >> 6 =
 * ( ( 8 - yFracC ) * A + yFracC * C + 4 ) >> 3 down the edge column.
 */
static void
test_chroma_clamp(void)
{
	uint8_t plane[40];
	uint8_t block[8];
	unsigned row;
	unsigned frac_x;
	unsigned expect;
	int wrong;
	int far_values[4];
	unsigned index;

	far_values[0] = -1000;
	far_values[1] = INT_MIN;
	far_values[2] = 1000;
	far_values[3] = INT_MAX;

	/* Makes a 5-wide, 8-high plane whose first column is 10, 20, ..., 80, whose last column is 250, 230, ..., 110, and whose middle columns are 1, 2 and 3. */
	for (row = 0U; row < 8U; row++) {
		plane[row * 5U + 0U] = (uint8_t)(10U * (row + 1U));
		plane[row * 5U + 1U] = 1U;
		plane[row * 5U + 2U] = 2U;
		plane[row * 5U + 3U] = 3U;
		plane[row * 5U + 4U] = (uint8_t)(250U - 20U * row);
	}

	/* Beyond the left edge a column of 1 x 4 samples is the first column, interpolated vertically by yFracC 3. */
	for (index = 0U; index < 4U; index++) {
		for (frac_x = 0U; frac_x < 8U; frac_x += 3U) {
			h264sw_interp_chroma(plane, 5U, 5U, 8U, far_values[index], 1, frac_x, 3U, 1U, 4U, block, 1U);
			wrong = 0;
			for (row = 0U; row < 4U; row++) {
				if (far_values[index] < 0)
					expect = (5U * plane[(1U + row) * 5U + 0U] + 3U * plane[(2U + row) * 5U + 0U] + 4U) >> 3;
				else
					expect = (5U * plane[(1U + row) * 5U + 4U] + 3U * plane[(2U + row) * 5U + 4U] + 4U) >> 3;
				if (block[row] != (uint8_t)expect)
					wrong = 1;
			}
			CHECK(!wrong);
		}
	}

	/* Beyond the bottom or the top, rows repeat: the result is the row's samples interpolated horizontally only. */
	h264sw_interp_chroma(plane, 5U, 5U, 8U, 0, 1000, 2U, 5U, 2U, 2U, block, 2U);
	expect = (6U * plane[7 * 5 + 0] + 2U * plane[7 * 5 + 1] + 4U) >> 3;
	CHECK(block[0] == (uint8_t)expect);
	CHECK(block[2] == (uint8_t)expect);
	h264sw_interp_chroma(plane, 5U, 5U, 8U, 0, -1000, 2U, 5U, 2U, 2U, block, 2U);
	expect = (6U * plane[0] + 2U * plane[1] + 4U) >> 3;
	CHECK(block[0] == (uint8_t)expect);
	CHECK(block[2] == (uint8_t)expect);
}

/*
 * Checks the chroma calls that cannot describe a block.
 */
static void
test_chroma_refused(void)
{
	uint8_t plane[16];
	uint8_t out[64];
	uint8_t expected[64];
	uint8_t masked[4];
	uint8_t direct[4];
	unsigned i;
	int same;

	for (i = 0U; i < 16U; i++)
		plane[i] = (uint8_t)(i * 11U);

	/* A width or height of 0 or above 16, a missing plane or output, and a plane without samples write nothing. */
	memset(out, (int)SENTINEL, sizeof(out));
	memset(expected, (int)SENTINEL, sizeof(expected));
	h264sw_interp_chroma(plane, 4U, 4U, 4U, 0, 0, 1U, 1U, 0U, 2U, out, 8U);
	h264sw_interp_chroma(plane, 4U, 4U, 4U, 0, 0, 1U, 1U, 2U, 0U, out, 8U);
	h264sw_interp_chroma(plane, 4U, 4U, 4U, 0, 0, 1U, 1U, 17U, 2U, out, 8U);
	h264sw_interp_chroma(NULL, 4U, 4U, 4U, 0, 0, 1U, 1U, 2U, 2U, out, 8U);
	h264sw_interp_chroma(plane, 4U, 4U, 4U, 0, 0, 1U, 1U, 2U, 2U, NULL, 8U);
	h264sw_interp_chroma(plane, 4U, 0U, 4U, 0, 0, 1U, 1U, 2U, 2U, out, 8U);
	h264sw_interp_chroma(plane, 4U, 4U, 0U, 0, 0, 1U, 1U, 2U, 2U, out, 8U);
	same = memcmp(out, expected, sizeof(out)) == 0;
	CHECK(same);

	/* A block writes only its own samples: 2x2 in rows of 8 leaves the rest alone. */
	h264sw_interp_chroma(plane, 4U, 4U, 4U, 1, 1, 0U, 0U, 2U, 2U, out, 8U);
	CHECK(out[0] == plane[5] && out[1] == plane[6]);
	CHECK(out[8] == plane[9] && out[9] == plane[10]);
	CHECK(out[2] == SENTINEL && out[7] == SENTINEL && out[10] == SENTINEL);

	/* A fractional part above 7 is taken modulo 8. */
	h264sw_interp_chroma(plane, 4U, 4U, 4U, 0, 0, 9U, 0xfffffffdU, 2U, 2U, masked, 2U);
	h264sw_interp_chroma(plane, 4U, 4U, 4U, 0, 0, 1U, 5U, 2U, 2U, direct, 2U);
	same = memcmp(masked, direct, 4U) == 0;
	CHECK(same);
}

/*
 * Runs random planes, locations, fractions and block sizes through both functions.
 *
 * The sanitizers report any read outside a plane or write outside a
 * block; the check is that every output sample lies between the smallest
 * and largest sample of the whole plane for the chroma bilinear filter
 * (a convex combination) and that nothing crashes for the luma filter.
 */
static void
test_fuzz(void)
{
	static const unsigned sizes[4] = { 2U, 4U, 8U, 16U };
	unsigned iteration;
	unsigned plane_w;
	unsigned plane_h;
	unsigned block_w;
	unsigned block_h;
	unsigned i;
	unsigned lowest;
	unsigned highest;
	uint8_t *plane;
	uint8_t *block;
	int x;
	int y;
	int in_range;

	/* Makes many predictions from planes of random shapes. */
	for (iteration = 0U; iteration < 3000U; iteration++) {
		plane_w = 1U + rng_next() % 40U;
		plane_h = 1U + rng_next() % 40U;
		block_w = sizes[rng_next() % 4U];
		block_h = sizes[rng_next() % 4U];
		x = (int)(rng_next() % 200U) - 100;
		y = (int)(rng_next() % 200U) - 100;

		plane = plane_new(plane_w, plane_h);
		lowest = 255U;
		highest = 0U;
		for (i = 0U; i < plane_w * plane_h; i++) {
			plane[i] = (uint8_t)rng_next();
			if (plane[i] < lowest)
				lowest = plane[i];
			if (plane[i] > highest)
				highest = plane[i];
		}

		block = (uint8_t *)malloc((size_t)block_w * block_h);
		CHECK(block != NULL);

		/* The luma filter runs without a report. */
		h264sw_interp_luma(plane, plane_w, plane_w, plane_h, x, y, rng_next(), rng_next(), block_w, block_h, block, block_w);

		/* The chroma filter keeps every sample within the plane's range. */
		h264sw_interp_chroma(plane, plane_w, plane_w, plane_h, x, y, rng_next(), rng_next(), block_w, block_h, block, block_w);
		in_range = 1;
		for (i = 0U; i < block_w * block_h; i++) {
			if (block[i] < lowest || block[i] > highest)
				in_range = 0;
		}
		CHECK(in_range);

		free(block);
		free(plane);
	}
}

/*
 * Runs every check and prints the summary line.
 */
int
main(void)
{
	test_luma_impulse();
	test_luma_clip();
	test_luma_ramp();
	test_luma_far_outside();
	test_luma_partial_edge();
	test_luma_refused();
	test_chroma_corners();
	test_chroma_ramp();
	test_chroma_clamp();
	test_chroma_refused();
	test_fuzz();

	/* Reports the checks. */
	return check_finish("interp");
}
