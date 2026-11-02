/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8l-transform.c: the inverse transforms against results
 * worked out by hand, and against forward transforms written here from
 * RFC 9649 3.5 (the forward direction an encoder takes) over random images.
 */

#include <stdlib.h>

#include "check.h"
#include "vp8l-transform.h"

/* The most pixels of the random images, which are small so that many are tried. */
#define RANDOM_SIDE_MAX		23U

static void test_subsample(void);
static void test_predictor_modes(void);
static void test_predictor_borders(void);
static void test_predictor_blocks(void);
static void test_predictor_arithmetic(void);
static void test_color_by_hand(void);
static void test_add_green(void);
static void test_indexing_by_hand(void);
static void test_indexing_in_place(void);
static void test_roundtrip_predictor(unsigned long seed);
static void test_roundtrip_color(unsigned long seed);
static void test_roundtrip_add_green(unsigned long seed);
static void test_roundtrip_indexing(unsigned long seed);
static void test_random_inverse(unsigned long seed);
static uint32_t *allocate_pixels(size_t count);
static uint32_t random_pixel(unsigned long *state);
static unsigned random_below(unsigned long *state, unsigned limit);
static uint32_t model_predict(unsigned mode, uint32_t left, uint32_t top, uint32_t top_right, uint32_t top_left);
static void model_predictor_forward(const uint32_t *image, uint32_t *residual, unsigned width, unsigned height,
	const uint32_t *modes, unsigned size_bits);
static void border_residuals(uint32_t *residual, const uint32_t *decoded, unsigned width, unsigned height);
static int model_delta(unsigned t, unsigned c);
static uint32_t model_color_forward(uint32_t pixel, uint32_t element);

/*
 * Runs the tests of vp8l-transform.c.
 */
int
main(
	void)
{
	unsigned long seed;
	int status;

	/* The results worked out by hand. */
	test_subsample();
	test_predictor_modes();
	test_predictor_borders();
	test_predictor_blocks();
	test_predictor_arithmetic();
	test_color_by_hand();
	test_add_green();
	test_indexing_by_hand();
	test_indexing_in_place();

	/* Random images, each from its own seed, through the forward transform and back. */
	for (seed = 1; seed <= 400UL; seed++) {
		test_roundtrip_predictor(seed);
		test_roundtrip_color(seed + 1000UL);
		test_roundtrip_add_green(seed + 2000UL);
		test_roundtrip_indexing(seed + 3000UL);
		test_random_inverse(seed + 4000UL);
	}

	/* The summary line. */
	status = check_finish("vp8l_transform");
	return status;
}

/* Checks the number of blocks that cover a size. */
static void
test_subsample(void)
{
	/* A size that fits one block exactly, and one pixel more. */
	CHECK(webpc_vp8l_subsample(1U, 0U) == 1U);
	CHECK(webpc_vp8l_subsample(16U, 2U) == 4U);
	CHECK(webpc_vp8l_subsample(17U, 2U) == 5U);
	CHECK(webpc_vp8l_subsample(15U, 2U) == 4U);
	CHECK(webpc_vp8l_subsample(1U, 9U) == 1U);

	/* The largest image with the largest block: 16384 / 512 is 32, and one more pixel needs a 33rd block. */
	CHECK(webpc_vp8l_subsample(16384U, 9U) == 32U);
	CHECK(webpc_vp8l_subsample(16383U, 9U) == 32U);
	CHECK(webpc_vp8l_subsample(16384U, 0U) == 16384U);

	/* The widths that a bundling of 2, 4 and 8 pixels leaves. */
	CHECK(webpc_vp8l_subsample(5U, 1U) == 3U);
	CHECK(webpc_vp8l_subsample(5U, 2U) == 2U);
	CHECK(webpc_vp8l_subsample(5U, 3U) == 1U);
	CHECK(webpc_vp8l_subsample(9U, 3U) == 2U);
}

/*
 * Checks every mode on a 3x3 image against the predictions worked out by
 * hand.  The border pixels are chosen and the other pixels get a residual
 * of zero, so that they come out as the prediction itself.
 */
static void
test_predictor_modes(void)
{
	/*
	 * The decoded image, row by row.  Pixel (1, 1) (x, then y) has
	 * L = 0x40307010, T = 0x305040f0, TL = 0x20406080 and TR = 0x102090a0.
	 * Pixel (2, 1) is on the rightmost column, so its TR is the leftmost
	 * pixel of its own row, 0x40307010.
	 */
	static const uint32_t decoded[9] = {
		0x20406080U, 0x305040f0U, 0x102090a0U,
		0x40307010U, 0x00000000U, 0x00000000U,
		0x11223344U, 0x00000000U, 0x00000000U
	};

	/*
	 * The predictions by hand, mode 0 to 15, for pixel (1, 1) and (2, 1).
	 * Mode 7 on (1, 1) is the mean of 40307010 and 305040f0 by channel:
	 * (40 + 30) / 2 = 38, (30 + 50) / 2 = 40, (70 + 40) / 2 = 58 and
	 * (10 + f0) / 2 = 80.  Mode 11 on (1, 1): both distances are 176, a
	 * tie, so the top pixel.  Mode 12 on (1, 1): 40 + 30 - 20 = 50,
	 * 30 + 50 - 40 = 40, 70 + 40 - 60 = 50 and 10 + f0 - 80 = 80.  Mode 13
	 * on (1, 1) adds half of (mean - TL) to the mean: 38 + (38 - 20) / 2 =
	 * 0x44, 0x40 + 0, 0x58 + (0x58 - 0x60) / 2 = 0x58 - 4 = 0x54 (truncation
	 * and floor agree on an even difference) and 0x80 + 0.
	 */
	static const uint32_t expected_11[16] = {
		0xff000000U, 0x40307010U, 0x305040f0U, 0x102090a0U,
		0x20406080U, 0x2c3c60a4U, 0x30386848U, 0x38405880U,
		0x284850b8U, 0x203868c8U, 0x28386888U, 0x305040f0U,
		0x50405080U, 0x44405480U, 0xff000000U, 0xff000000U
	};
	static const uint32_t expected_12[16] = {
		0xff000000U, 0x40307010U, 0x102090a0U, 0x40307010U,
		0x305040f0U, 0x232b7c7dU, 0x3044549cU, 0x24307490U,
		0x203868c8U, 0x28288058U, 0x2a366a8aU, 0x102090a0U,
		0x3010a030U, 0x27208b60U, 0xff000000U, 0xff000000U
	};
	uint32_t residual[9];
	uint32_t modes[1];
	unsigned mode;

	/* Every mode, with the block's mode in the green of the single predictor pixel. */
	for (mode = 0U; mode < 16U; mode++) {
		/* The residuals that restore the border pixels, and zero elsewhere. */
		border_residuals(residual, decoded, 3U, 3U);

		/* The mode sits in the low four bits of green; the other bits of the pixel do not matter. */
		modes[0] = 0xff00a000U | (mode << 8) | 0x00010002U;
		webpc_vp8l_inverse_predictor(residual, 3U, 3U, modes, 2U);

		/* The border comes back as chosen. */
		CHECK(residual[0] == decoded[0]);
		CHECK(residual[1] == decoded[1]);
		CHECK(residual[2] == decoded[2]);
		CHECK(residual[3] == decoded[3]);
		CHECK(residual[6] == decoded[6]);

		/* The two pixels worked out by hand: (1, 1), then (2, 1) on the rightmost column. */
		CHECK(residual[4] == expected_11[mode]);
		CHECK(residual[5] == expected_12[mode]);
	}

	/* The mode is the green modulo 16: green 0x12 is mode 2, green 0xf1 is mode 1 and green 0x1e is mode 14. */
	border_residuals(residual, decoded, 3U, 3U);
	modes[0] = 0x00001200U;
	webpc_vp8l_inverse_predictor(residual, 3U, 3U, modes, 2U);
	CHECK(residual[4] == expected_11[2]);
	border_residuals(residual, decoded, 3U, 3U);
	modes[0] = 0x0000f100U;
	webpc_vp8l_inverse_predictor(residual, 3U, 3U, modes, 2U);
	CHECK(residual[4] == expected_11[1]);
	border_residuals(residual, decoded, 3U, 3U);
	modes[0] = 0x00001e00U;
	webpc_vp8l_inverse_predictor(residual, 3U, 3U, modes, 2U);
	CHECK(residual[4] == 0xff000000U);
}

/*
 * Checks the rules at the borders: the first pixel, the top row, the left
 * column and the rightmost column's top-right neighbour.
 */
static void
test_predictor_borders(void)
{
	uint32_t pixels[12];
	uint32_t modes[1];
	unsigned index;

	/*
	 * A residual of 0x01020304 everywhere, 4x3, with mode 3 (TR) for the
	 * block.  The first pixel is 0xff000000 + 0x01020304, the alpha
	 * wrapping to 0x00: 0x00020304.  The top row then adds the pixel on
	 * its left each time, whatever the mode.
	 */
	for (index = 0U; index < 12U; index++)
		pixels[index] = 0x01020304U;
	modes[0] = 0x00000300U;
	webpc_vp8l_inverse_predictor(pixels, 4U, 3U, modes, 2U);
	CHECK(pixels[0] == 0x00020304U);
	CHECK(pixels[1] == 0x01040608U);
	CHECK(pixels[2] == 0x0206090cU);
	CHECK(pixels[3] == 0x03080c10U);

	/* The left column adds the pixel above it, whatever the mode: 0x00020304 + 0x01020304, then + 0x01020304 again. */
	CHECK(pixels[4] == 0x01040608U);
	CHECK(pixels[8] == 0x0206090cU);

	/*
	 * Row 1 in mode 3: pixel (1, 1) adds the pixel above and to its right,
	 * (2, 0) = 0x0206090c: 0x03080c10; pixel (2, 1) adds (3, 0) =
	 * 0x03080c10: 0x040a0f14.  The rightmost pixel (3, 1) has no pixel
	 * above and to its right and adds the leftmost pixel of its own row,
	 * (0, 1) = 0x01040608: 0x0206090c.
	 */
	CHECK(pixels[5] == 0x03080c10U);
	CHECK(pixels[6] == 0x040a0f14U);
	CHECK(pixels[7] == 0x0206090cU);

	/*
	 * Row 2: pixel (1, 2) adds (2, 1) = 0x040a0f14: 0x050c1218; the
	 * rightmost (3, 2) adds the leftmost of row 2, (0, 2) = 0x0206090c:
	 * 0x03080c10 (the sum of the rightmost pixel above it does not enter).
	 */
	CHECK(pixels[9] == 0x050c1218U);
	CHECK(pixels[11] == 0x03080c10U);

	/* A one-pixel-wide image: the first pixel is black plus its residual, the others add the pixel above. */
	pixels[0] = 0x01020304U;
	pixels[1] = 0x01020304U;
	pixels[2] = 0x10203040U;
	modes[0] = 0x00000700U;
	webpc_vp8l_inverse_predictor(pixels, 1U, 3U, modes, 2U);
	CHECK(pixels[0] == 0x00020304U);
	CHECK(pixels[1] == 0x01040608U);
	CHECK(pixels[2] == 0x11243648U);

	/* A one-row image: every pixel after the first adds the pixel on its left. */
	pixels[0] = 0x01010101U;
	pixels[1] = 0x01010101U;
	pixels[2] = 0xffffffffU;
	webpc_vp8l_inverse_predictor(pixels, 3U, 1U, modes, 2U);
	CHECK(pixels[0] == 0x00010101U);
	CHECK(pixels[1] == 0x01020202U);
	CHECK(pixels[2] == 0x00010101U);
}

/*
 * Checks that each block of a predictor image with several blocks names
 * its own mode.
 */
static void
test_predictor_blocks(void)
{
	uint32_t decoded[25];
	uint32_t residual[25];
	uint32_t modes[4];
	unsigned index;

	/* A 5x5 image of 4x4 blocks has 2x2 blocks: mode 3 (TR), 2 (T), 1 (L) and 3 (TR). */
	for (index = 0U; index < 25U; index++)
		decoded[index] = 0x01010101U * (index + 1U) + 0x00112233U * (index % 5U);
	modes[0] = 0x00000300U;
	modes[1] = 0x00000200U;
	modes[2] = 0x00000100U;
	modes[3] = 0x00000300U;
	border_residuals(residual, decoded, 5U, 5U);
	webpc_vp8l_inverse_predictor(residual, 5U, 5U, modes, 2U);

	/* Pixel (1, 1) is in block 0, mode 3: its TR is pixel (2, 0). */
	CHECK(residual[1 * 5 + 1] == decoded[2]);

	/* Pixel (4, 1) is in block 1, mode 2: its T is pixel (4, 0). */
	CHECK(residual[1 * 5 + 4] == decoded[4]);

	/* Pixel (3, 1) is in block 0, mode 3: its TR is pixel (4, 0). */
	CHECK(residual[1 * 5 + 3] == decoded[4]);

	/* Pixel (2, 2) is in block 0, mode 3: its TR is the restored pixel (3, 1), which is pixel (4, 0). */
	CHECK(residual[2 * 5 + 2] == decoded[4]);

	/* Pixel (1, 4) is in block 2, mode 1: its L is the left column's pixel (0, 4). */
	CHECK(residual[4 * 5 + 1] == residual[4 * 5 + 0]);
	CHECK(residual[4 * 5 + 0] == decoded[20]);

	/* Pixel (4, 4) is in block 3, mode 3, on the rightmost column: its TR is the leftmost pixel of row 4. */
	CHECK(residual[4 * 5 + 4] == decoded[20]);

	/* A 5x4 image of 4x4 blocks has 2x1 blocks, mode 2 then mode 1. */
	for (index = 0U; index < 20U; index++)
		decoded[index] = 0x02020202U * (index + 3U) + 0x00091827U * (index % 4U);
	modes[0] = 0x0000f200U;
	modes[1] = 0x00000100U;
	border_residuals(residual, decoded, 5U, 4U);
	webpc_vp8l_inverse_predictor(residual, 5U, 4U, modes, 2U);
	CHECK(residual[1 * 5 + 1] == decoded[1]);
	CHECK(residual[1 * 5 + 3] == decoded[3]);

	/* The rightmost pixel of row 1 is in block 1, mode 1: its L is pixel (3, 1), the top pixel's column 3. */
	CHECK(residual[1 * 5 + 4] == decoded[3]);
}

/*
 * Restores the one pixel (1, 1) of a 2x2 image by a mode, from the three
 * decoded pixels around it, and gives it.  The pixel is on the rightmost
 * column, so a mode that reads TR reads the leftmost pixel of its row, left.
 */
static uint32_t
predict_corner(
	unsigned mode,
	uint32_t top_left,
	uint32_t top,
	uint32_t left)
{
	uint32_t decoded[4];
	uint32_t residual[4];
	uint32_t modes[1];

	/* The decoded pixels with a zero for the one to predict, whose residual is then zero. */
	decoded[0] = top_left;
	decoded[1] = top;
	decoded[2] = left;
	decoded[3] = 0U;
	border_residuals(residual, decoded, 2U, 2U);
	modes[0] = mode << 8;
	webpc_vp8l_inverse_predictor(residual, 2U, 2U, modes, 2U);
	return residual[3];
}

/* Checks the average, select and clamp arithmetic on cases chosen for the edges of each rule. */
static void
test_predictor_arithmetic(void)
{
	/*
	 * ClampAddSubtractHalf where the half-difference is negative and odd:
	 * L = T = 0x0a in every channel gives a mean of 10, and TL = 0x0d gives
	 * 10 + (10 - 13) / 2 = 10 - 1 = 9 when the division truncates toward
	 * zero (the flooring shift would give 8).
	 */
	CHECK(predict_corner(13U, 0x0d0d0d0dU, 0x0a0a0a0aU, 0x0a0a0a0aU) == 0x09090909U);

	/* The same with a positive odd difference: mean 13, TL 10: 13 + (13 - 10) / 2 = 14. */
	CHECK(predict_corner(13U, 0x0a0a0a0aU, 0x0d0d0d0dU, 0x0d0d0d0dU) == 0x0e0e0e0eU);

	/* The clamp: a mean of 0xf0 and a TL of 0x00 give 0xf0 + 0x78 = 0x168, which clamps to 0xff. */
	CHECK(predict_corner(13U, 0x00000000U, 0xf0f0f0f0U, 0xf0f0f0f0U) == 0xffffffffU);

	/* The clamp below zero: a mean of 0x10 and a TL of 0xf0 give 0x10 + (0x10 - 0xf0) / 2 = 0x10 - 0x70, which clamps to 0. */
	CHECK(predict_corner(13U, 0xf0f0f0f0U, 0x10101010U, 0x10101010U) == 0x00000000U);

	/* ClampAddSubtractFull below zero: L = T = 0x10, TL = 0x80 gives -0x60, which clamps to 0. */
	CHECK(predict_corner(12U, 0x80808080U, 0x10101010U, 0x10101010U) == 0x00000000U);

	/* ClampAddSubtractFull above 255: L = T = 0xf0, TL = 0x10 gives 0x1d0, which clamps to 0xff. */
	CHECK(predict_corner(12U, 0x10101010U, 0xf0f0f0f0U, 0xf0f0f0f0U) == 0xffffffffU);

	/* ClampAddSubtractFull within range: 0x50 + 0x30 - 0x20 = 0x60 in every channel. */
	CHECK(predict_corner(12U, 0x20202020U, 0x30303030U, 0x50505050U) == 0x60606060U);

	/*
	 * Select with pL < pT, where pL is the distance of the estimate from L,
	 * which is the summed difference of T and TL: L = 0x50, T = 0x20 and
	 * TL = 0x22 give pL = 4 * 2 and pT = 4 * 0x2e, so L.
	 */
	CHECK(predict_corner(11U, 0x22222222U, 0x20202020U, 0x50505050U) == 0x50505050U);

	/* Select with pL > pT: L = 0x20, T = 0x50 and TL = 0x22 give pL = 4 * 0x2e and pT = 4 * 2, so T. */
	CHECK(predict_corner(11U, 0x22222222U, 0x50505050U, 0x20202020U) == 0x50505050U);

	/* Select sums the distances over the channels, alpha too.  TL = 0, T = 0x30 in blue, L = 0x20 in alpha: pL = 0x30 and pT = 0x20, so T. */
	CHECK(predict_corner(11U, 0x00000000U, 0x00000030U, 0x20000000U) == 0x00000030U);

	/* The same with T = 0x10 in blue: pL = 0x10 and pT = 0x20, so L. */
	CHECK(predict_corner(11U, 0x00000000U, 0x00000010U, 0x20000000U) == 0x20000000U);

	/*
	 * Average2 is the floor, channel by channel: L = 0x02ff0100 and
	 * T = 0x01ff0301 give alpha (2 + 1) / 2 = 1, red (255 + 255) / 2 = 255,
	 * green (1 + 3) / 2 = 2 and blue (0 + 1) / 2 = 0, and the low bit of
	 * one channel does not enter the one below.
	 */
	CHECK(predict_corner(7U, 0x00000000U, 0x01ff0301U, 0x02ff0100U) == 0x01ff0200U);

	/* Both odd in several channels: alpha (0xff + 0x01) / 2 = 0x80, red (0x03 + 0x01) / 2 = 2, green (0x01 + 0x01) / 2 = 1, blue (0x7f + 0x7f) / 2 = 0x7f. */
	CHECK(predict_corner(7U, 0x00000000U, 0xff03017fU, 0x0101017fU) == 0x8002017fU);
}

/* Checks the colour transform on elements and pixels worked out by hand. */
static void
test_color_by_hand(void)
{
	uint32_t pixels[1];
	uint32_t elements[1];

	/*
	 * green_to_red 0xff (-1) with green 0x20 (32): -32 >> 5 = -1, so red
	 * 0x0a becomes 9.  No other element has an effect.
	 */
	pixels[0] = 0xff0a2000U;
	elements[0] = 0xff0000ffU;
	webpc_vp8l_inverse_color(pixels, 1U, 1U, elements, 2U);
	CHECK(pixels[0] == 0xff092000U);

	/*
	 * The shift is a floor: t = -1 and c = 1 give -1 >> 5 = -1, where a
	 * truncating division would give 0.  Red 5 becomes 4.
	 */
	pixels[0] = 0xff050100U;
	elements[0] = 0xff0000ffU;
	webpc_vp8l_inverse_color(pixels, 1U, 1U, elements, 2U);
	CHECK(pixels[0] == 0xff040100U);

	/*
	 * t = 0x80 (-128), c = 0x7f (127): -16256 >> 5 = -508, whose low 8
	 * bits are 4 (-508 + 512).  Red 0 becomes 4.
	 */
	pixels[0] = 0xff007f00U;
	elements[0] = 0xff000080U;
	webpc_vp8l_inverse_color(pixels, 1U, 1U, elements, 2U);
	CHECK(pixels[0] == 0xff047f00U);

	/* Both factors negative: t = c = 0x80 gives 16384 >> 5 = 512, whose low 8 bits are 0. */
	pixels[0] = 0xff338000U;
	elements[0] = 0xff000080U;
	webpc_vp8l_inverse_color(pixels, 1U, 1U, elements, 2U);
	CHECK(pixels[0] == 0xff338000U);

	/* Red wraps modulo 256: red 0xfa with delta 8 (t = 0x40 (64), c = 0x20 (32): 2048 >> 5 = 64) gives 0xfa + 0x40 = 0x13a -> 0x3a. */
	pixels[0] = 0xfffa2000U;
	elements[0] = 0xff000040U;
	webpc_vp8l_inverse_color(pixels, 1U, 1U, elements, 2U);
	CHECK(pixels[0] == 0xff3a2000U);

	/* green_to_blue adds to blue: t = 0x20 (32), c = 0x40 (64): 2048 >> 5 = 64; blue 0x10 becomes 0x50. */
	pixels[0] = 0xff004010U;
	elements[0] = 0xff002000U;
	webpc_vp8l_inverse_color(pixels, 1U, 1U, elements, 2U);
	CHECK(pixels[0] == 0xff004050U);

	/*
	 * red_to_blue uses the new red, not the old.  Red 0x00 with green_to_red
	 * 0x40 and green 0x40 (64 * 64 >> 5 = 128) gives a new red of 0x80, as
	 * a signed value -128.  With red_to_blue 0x20 (32): 32 * -128 >> 5 =
	 * -128, so blue 0x00 becomes 0x80.  Using the old red 0 it would
	 * stay 0x00.
	 */
	pixels[0] = 0xff004000U;
	elements[0] = 0xff200040U;
	webpc_vp8l_inverse_color(pixels, 1U, 1U, elements, 2U);
	CHECK(pixels[0] == 0xff804080U);

	/* The alpha and green are untouched: alpha 0x12, green 0x34 with a zero element. */
	pixels[0] = 0x12563478U;
	elements[0] = 0x00000000U;
	webpc_vp8l_inverse_color(pixels, 1U, 1U, elements, 2U);
	CHECK(pixels[0] == 0x12563478U);
}

/* Checks the subtract-green transform's inverse. */
static void
test_add_green(void)
{
	uint32_t pixels[4];

	/* Red 200 + green 100 = 300, which wraps to 44; blue 255 + green 100 = 355 wraps to 99; alpha and green stay. */
	pixels[0] = 0x80c864ffU;
	pixels[1] = 0xff000000U;
	pixels[2] = 0x01ff01ffU;
	pixels[3] = 0x7f010203U;
	webpc_vp8l_add_green(pixels, 4U);
	CHECK(pixels[0] == 0x802c6463U);

	/* A zero green changes nothing. */
	CHECK(pixels[1] == 0xff000000U);

	/* Green 0x01: red 0xff wraps to 0, blue 0xff wraps to 0, and the carry does not reach alpha. */
	CHECK(pixels[2] == 0x01000100U);

	/* Green 0x02: red 0x01 -> 0x03, blue 0x03 -> 0x05. */
	CHECK(pixels[3] == 0x7f030205U);

	/* A count of 0 touches nothing. */
	webpc_vp8l_add_green(pixels, 0U);
	CHECK(pixels[3] == 0x7f030205U);
}

/* Checks the bit positions of the indices in a packed pixel and the table lookup. */
static void
test_indexing_by_hand(void)
{
	uint32_t table[WEBPC_VP8L_COLOR_TABLE_SIZE];
	uint32_t packed[2];
	uint32_t out[16];
	unsigned index;
	unsigned x;

	/* A table whose entry i is 0x11000000 + i for i < 16, and transparent black after (the colour count is 16). */
	for (index = 0U; index < WEBPC_VP8L_COLOR_TABLE_SIZE; index++)
		table[index] = 0U;
	for (index = 0U; index < 16U; index++)
		table[index] = 0x11000000U + index;

	/* width_bits 0: the green is the index itself. */
	packed[0] = 0xff000500U;
	packed[1] = 0xff000f00U;
	webpc_vp8l_inverse_indexing(packed, 2U, 1U, 0U, table, out);
	CHECK(out[0] == 0x11000005U);
	CHECK(out[1] == 0x1100000fU);

	/* An index past the colour count finds transparent black: 0xc3 = 195 is past the 16 colours. */
	packed[0] = 0xff00c300U;
	webpc_vp8l_inverse_indexing(packed, 1U, 1U, 0U, table, out);
	CHECK(out[0] == 0x00000000U);

	/* width_bits 1: 4 bits each, the first pixel in the low bits.  Green 0x3a: pixel 0 is 0xa, pixel 1 is 0x3. */
	packed[0] = 0xff003a00U;
	webpc_vp8l_inverse_indexing(packed, 2U, 1U, 1U, table, out);
	CHECK(out[0] == 0x1100000aU);
	CHECK(out[1] == 0x11000003U);

	/* width_bits 2: 2 bits each.  Green 0xe4 = 11 10 01 00 gives 0, 1, 2, 3 from the first pixel on. */
	packed[0] = 0xff00e400U;
	webpc_vp8l_inverse_indexing(packed, 4U, 1U, 2U, table, out);
	CHECK(out[0] == 0x11000000U);
	CHECK(out[1] == 0x11000001U);
	CHECK(out[2] == 0x11000002U);
	CHECK(out[3] == 0x11000003U);

	/* width_bits 3: 1 bit each.  Green 0xb5 = 1011 0101 gives 1, 0, 1, 0, 1, 1, 0, 1 from the first pixel on. */
	packed[0] = 0xff00b500U;
	webpc_vp8l_inverse_indexing(packed, 8U, 1U, 3U, table, out);
	for (x = 0U; x < 8U; x++)
		CHECK(out[x] == 0x11000000U + ((0xb5U >> x) & 1U));

	/* A width that is not a multiple of the pack: 5 pixels of 8 per packed pixel use the low 5 bits only. */
	packed[0] = 0xff00b500U;
	webpc_vp8l_inverse_indexing(packed, 5U, 1U, 3U, table, out);
	CHECK(out[0] == 0x11000001U);
	CHECK(out[1] == 0x11000000U);
	CHECK(out[2] == 0x11000001U);
	CHECK(out[3] == 0x11000000U);
	CHECK(out[4] == 0x11000001U);

	/* Width 9 in 8-pixel packs needs two packed pixels a row: the ninth pixel is the first bit of the second. */
	packed[0] = 0xff00b500U;
	packed[1] = 0xff000100U;
	webpc_vp8l_inverse_indexing(packed, 9U, 1U, 3U, table, out);
	CHECK(out[7] == 0x11000001U);
	CHECK(out[8] == 0x11000001U);

	/* Two rows: the second packed row starts after the first, at packed_width pixels. */
	packed[0] = 0xff000100U;
	packed[1] = 0xff000200U;
	webpc_vp8l_inverse_indexing(packed, 3U, 2U, 3U, table, out);
	CHECK(out[0] == 0x11000001U);
	CHECK(out[1] == 0x11000000U);
	CHECK(out[2] == 0x11000000U);
	CHECK(out[3] == 0x11000000U);
	CHECK(out[4] == 0x11000001U);
	CHECK(out[5] == 0x11000000U);

	/* The index of a packed pixel ignores its alpha, red and blue. */
	packed[0] = 0x12345678U;
	webpc_vp8l_inverse_indexing(packed, 1U, 1U, 0U, table, out);
	CHECK(out[0] == table[0x56U]);
}

/* Checks that expanding in place gives the same pixels as expanding into another buffer. */
static void
test_indexing_in_place(void)
{
	uint32_t table[WEBPC_VP8L_COLOR_TABLE_SIZE];
	uint32_t *packed;
	uint32_t *separate;
	uint32_t *shared;
	unsigned long state;
	unsigned width_bits;
	unsigned width;
	unsigned height;
	unsigned packed_width;
	unsigned index;
	unsigned trial;
	size_t count;

	/* A table of distinct colours. */
	state = 77UL;
	for (index = 0U; index < WEBPC_VP8L_COLOR_TABLE_SIZE; index++)
		table[index] = 0x01000000U + index * 0x00010101U;

	/* Random sizes at every bundling. */
	for (trial = 0U; trial < 200U; trial++) {
		width_bits = trial % 4U;
		width = 1U + random_below(&state, RANDOM_SIDE_MAX);
		height = 1U + random_below(&state, RANDOM_SIDE_MAX);
		packed_width = webpc_vp8l_subsample(width, width_bits);

		/* The packed rows at the start of a buffer as large as the output. */
		count = (size_t)width * height;
		packed = allocate_pixels((size_t)packed_width * height);
		separate = allocate_pixels(count);
		shared = allocate_pixels(count);
		for (index = 0U; index < packed_width * height; index++) {
			packed[index] = random_pixel(&state);
			shared[index] = packed[index];
		}

		/* The same expansion into a separate buffer and in place. */
		webpc_vp8l_inverse_indexing(packed, width, height, width_bits, table, separate);
		webpc_vp8l_inverse_indexing(shared, width, height, width_bits, table, shared);
		for (index = 0U; index < count; index++)
			CHECK(shared[index] == separate[index]);

		free(packed);
		free(separate);
		free(shared);
	}
}

/*
 * Predicts a pixel the way RFC 9649 3.5.1 describes it, a channel at a
 * time from arrays of four values.
 */
static uint32_t
model_predict(
	unsigned mode,
	uint32_t left,
	uint32_t top,
	uint32_t top_right,
	uint32_t top_left)
{
	int l[4];
	int t[4];
	int tr[4];
	int tl[4];
	int r[4];
	int c;
	int pl;
	int pt;
	int p;
	int half;
	uint32_t result;

	/* The channels, blue first. */
	for (c = 0; c < 4; c++) {
		l[c] = (int)((left >> (8 * c)) & 0xffU);
		t[c] = (int)((top >> (8 * c)) & 0xffU);
		tr[c] = (int)((top_right >> (8 * c)) & 0xffU);
		tl[c] = (int)((top_left >> (8 * c)) & 0xffU);
	}

	/* The distances Select compares. */
	pl = 0;
	pt = 0;
	for (c = 0; c < 4; c++) {
		p = l[c] + t[c] - tl[c];
		pl += (p - l[c] < 0) ? l[c] - p : p - l[c];
		pt += (p - t[c] < 0) ? t[c] - p : p - t[c];
	}

	/* The modes 14 and 15 are 0. */
	if (mode >= 14U)
		mode = 0U;
	for (c = 0; c < 4; c++) {
		switch (mode) {
		case 0:
			r[c] = (c == 3) ? 255 : 0;
			break;
		case 1:
			r[c] = l[c];
			break;
		case 2:
			r[c] = t[c];
			break;
		case 3:
			r[c] = tr[c];
			break;
		case 4:
			r[c] = tl[c];
			break;
		case 5:
			r[c] = (((l[c] + tr[c]) / 2) + t[c]) / 2;
			break;
		case 6:
			r[c] = (l[c] + tl[c]) / 2;
			break;
		case 7:
			r[c] = (l[c] + t[c]) / 2;
			break;
		case 8:
			r[c] = (tl[c] + t[c]) / 2;
			break;
		case 9:
			r[c] = (t[c] + tr[c]) / 2;
			break;
		case 10:
			r[c] = (((l[c] + tl[c]) / 2) + ((t[c] + tr[c]) / 2)) / 2;
			break;
		case 11:
			r[c] = (pl < pt) ? l[c] : t[c];
			break;
		case 12:
			r[c] = l[c] + t[c] - tl[c];
			break;
		default:
			half = (l[c] + t[c]) / 2;
			p = half - tl[c];
			r[c] = half + ((p < 0) ? -((-p) / 2) : p / 2);
			break;
		}
		if (r[c] < 0)
			r[c] = 0;
		if (r[c] > 255)
			r[c] = 255;
	}

	/* Packs the channels back. */
	result = 0U;
	for (c = 0; c < 4; c++)
		result |= (uint32_t)r[c] << (8 * c);
	return result;
}

/*
 * The forward predictor transform: each residual is the actual pixel minus
 * the prediction from the actual neighbours, per channel modulo 256.
 */
static void
model_predictor_forward(
	const uint32_t *image,
	uint32_t *residual,
	unsigned width,
	unsigned height,
	const uint32_t *modes,
	unsigned size_bits)
{
	unsigned x;
	unsigned y;
	unsigned c;
	unsigned mode;
	unsigned modes_width;
	uint32_t predicted;
	uint32_t actual;
	uint32_t result;
	const uint32_t *row;
	const uint32_t *above;

	/* The blocks across. */
	modes_width = (width + (1U << size_bits) - 1U) >> size_bits;
	for (y = 0U; y < height; y++) {
		for (x = 0U; x < width; x++) {
			row = image + (size_t)y * width;
			above = row - width;
			if (x == 0U && y == 0U) {
				predicted = 0xff000000U;
			} else if (y == 0U) {
				predicted = row[x - 1U];
			} else if (x == 0U) {
				predicted = above[0];
			} else {
				mode = (modes[(size_t)(y >> size_bits) * modes_width + (x >> size_bits)] >> 8) & 15U;
				predicted = model_predict(mode, row[x - 1U], above[x], (x + 1U < width) ? above[x + 1U] : row[0],
				    above[x - 1U]);
			}

			/* The residual channel by channel. */
			actual = row[x];
			result = 0U;
			for (c = 0U; c < 4U; c++) {
				result |= ((((actual >> (8U * c)) & 0xffU) - ((predicted >> (8U * c)) & 0xffU)) & 0xffU) << (8U * c);
			}
			residual[(size_t)y * width + x] = result;
		}
	}
}

/*
 * Makes residuals that restore the given border pixels (the first pixel,
 * the top row and the left column) and are zero elsewhere.
 */
static void
border_residuals(
	uint32_t *residual,
	const uint32_t *decoded,
	unsigned width,
	unsigned height)
{
	unsigned x;
	unsigned y;
	unsigned c;
	uint32_t predicted;
	uint32_t result;

	/* Each border pixel minus its border prediction, per channel. */
	for (y = 0U; y < height; y++) {
		for (x = 0U; x < width; x++) {
			residual[(size_t)y * width + x] = 0U;
			if (x != 0U && y != 0U)
				continue;
			if (x == 0U && y == 0U)
				predicted = 0xff000000U;
			else if (y == 0U)
				predicted = decoded[x - 1U];
			else
				predicted = decoded[(size_t)(y - 1U) * width];
			result = 0U;
			for (c = 0U; c < 4U; c++) {
				result |= ((((decoded[(size_t)y * width + x] >> (8U * c)) & 0xffU) -
				    ((predicted >> (8U * c)) & 0xffU)) & 0xffU) << (8U * c);
			}
			residual[(size_t)y * width + x] = result;
		}
	}
}

/* The colour transform delta, written out: the product's floor division by 32 with explicit signed bytes. */
static int
model_delta(
	unsigned t,
	unsigned c)
{
	int st;
	int sc;
	int product;
	int quotient;

	/* The signed bytes. */
	st = (t < 128U) ? (int)t : (int)t - 256;
	sc = (c < 128U) ? (int)c : (int)c - 256;
	product = st * sc;

	/* Floor division by 32. */
	if (product >= 0)
		quotient = product / 32;
	else
		quotient = -((-product + 31) / 32);
	return quotient;
}

/* The forward colour transform of one pixel (RFC 9649 3.5.2's ColorTransform): the deltas are subtracted. */
static uint32_t
model_color_forward(
	uint32_t pixel,
	uint32_t element)
{
	int red;
	int green;
	int blue;
	int new_red;
	int new_blue;

	/* The channels, the deltas taken from the original red and green. */
	red = (int)((pixel >> 16) & 0xffU);
	green = (int)((pixel >> 8) & 0xffU);
	blue = (int)(pixel & 0xffU);
	new_red = red - model_delta((unsigned)(element & 0xffU), (unsigned)green);
	new_blue = blue - model_delta((unsigned)((element >> 8) & 0xffU), (unsigned)green);
	new_blue -= model_delta((unsigned)((element >> 16) & 0xffU), (unsigned)red);
	new_red &= 0xff;
	new_blue &= 0xff;
	return (pixel & 0xff00ff00U) | ((uint32_t)new_red << 16) | (uint32_t)new_blue;
}

/* Allocates an array of pixels of exactly count entries, so that the sanitizer sees any access past it. */
static uint32_t *
allocate_pixels(
	size_t count)
{
	uint32_t *pixels;

	/* The array is exactly count entries, so that an access one past it is a sanitizer error. */
	pixels = (uint32_t *)malloc(count * sizeof(uint32_t));
	if (pixels == NULL) {
		printf("test-vp8l_transform: out of memory\n");
		exit(2);
	}
	return pixels;
}

/* Gives a random pixel; the generator's low bits are poor, so the high bits are used. */
static uint32_t
random_pixel(
	unsigned long *state)
{
	uint32_t high;
	uint32_t low;

	high = (uint32_t)(check_random(state) >> 8) & 0xffffU;
	low = (uint32_t)(check_random(state) >> 8) & 0xffffU;
	return (high << 16) | low;
}

/* Gives a random number from 0 to limit - 1. */
static unsigned
random_below(
	unsigned long *state,
	unsigned limit)
{
	unsigned long value;

	value = check_random(state) >> 4;
	return (unsigned)(value % limit);
}

/* Checks that the predictor transform's forward and inverse undo each other for random images and modes. */
static void
test_roundtrip_predictor(
	unsigned long seed)
{
	unsigned long state;
	unsigned width;
	unsigned height;
	unsigned size_bits;
	unsigned modes_width;
	unsigned modes_height;
	unsigned index;
	uint32_t *image;
	uint32_t *residual;
	uint32_t *modes;
	size_t count;

	/* A random image, a random block size and a random mode (any green value) for every block. */
	state = seed * 2654435761UL + 1UL;
	width = 1U + random_below(&state, RANDOM_SIDE_MAX);
	height = 1U + random_below(&state, RANDOM_SIDE_MAX);
	size_bits = 2U + random_below(&state, 4U);
	modes_width = webpc_vp8l_subsample(width, size_bits);
	modes_height = webpc_vp8l_subsample(height, size_bits);
	count = (size_t)width * height;
	image = allocate_pixels(count);
	residual = allocate_pixels(count);
	modes = allocate_pixels((size_t)modes_width * modes_height);
	for (index = 0U; index < count; index++)
		image[index] = random_pixel(&state);
	for (index = 0U; index < modes_width * modes_height; index++)
		modes[index] = random_pixel(&state);

	/* Smooth parts as well as noise, so that Select and the clamps see both near and far values. */
	if ((seed & 1UL) != 0UL) {
		for (index = 0U; index < count; index++)
			image[index] = (image[index] & 0x03030303U) + 0x20406080U;
	}

	/* Forward, then inverse: the image comes back. */
	model_predictor_forward(image, residual, width, height, modes, size_bits);
	webpc_vp8l_inverse_predictor(residual, width, height, modes, size_bits);
	for (index = 0U; index < count; index++)
		CHECK(residual[index] == image[index]);

	free(image);
	free(residual);
	free(modes);
}

/* Checks that the colour transform's forward and inverse undo each other for random images and elements. */
static void
test_roundtrip_color(
	unsigned long seed)
{
	unsigned long state;
	unsigned width;
	unsigned height;
	unsigned size_bits;
	unsigned elements_width;
	unsigned elements_height;
	unsigned index;
	unsigned x;
	unsigned y;
	uint32_t *image;
	uint32_t *transformed;
	uint32_t *elements;
	size_t count;

	/* A random image and a random element for every block. */
	state = seed * 2654435761UL + 1UL;
	width = 1U + random_below(&state, RANDOM_SIDE_MAX);
	height = 1U + random_below(&state, RANDOM_SIDE_MAX);
	size_bits = 2U + random_below(&state, 4U);
	elements_width = webpc_vp8l_subsample(width, size_bits);
	elements_height = webpc_vp8l_subsample(height, size_bits);
	count = (size_t)width * height;
	image = allocate_pixels(count);
	transformed = allocate_pixels(count);
	elements = allocate_pixels((size_t)elements_width * elements_height);
	for (index = 0U; index < count; index++)
		image[index] = random_pixel(&state);
	for (index = 0U; index < elements_width * elements_height; index++)
		elements[index] = random_pixel(&state);

	/* The forward transform with each block's element. */
	for (y = 0U; y < height; y++) {
		for (x = 0U; x < width; x++) {
			transformed[(size_t)y * width + x] = model_color_forward(image[(size_t)y * width + x],
			    elements[(size_t)(y >> size_bits) * elements_width + (x >> size_bits)]);
		}
	}

	/* The inverse restores the image. */
	webpc_vp8l_inverse_color(transformed, width, height, elements, size_bits);
	for (index = 0U; index < count; index++)
		CHECK(transformed[index] == image[index]);

	free(image);
	free(transformed);
	free(elements);
}

/* Checks that adding green undoes the subtraction of green for random pixels. */
static void
test_roundtrip_add_green(
	unsigned long seed)
{
	unsigned long state;
	unsigned count;
	unsigned index;
	uint32_t *image;
	uint32_t *transformed;
	uint32_t pixel;
	uint32_t green;

	/* The forward transform: red and blue each lose green, modulo 256. */
	state = seed * 2654435761UL + 1UL;
	count = 1U + random_below(&state, 300U);
	image = allocate_pixels(count);
	transformed = allocate_pixels(count);
	for (index = 0U; index < count; index++) {
		pixel = random_pixel(&state);
		image[index] = pixel;
		green = (pixel >> 8) & 0xffU;
		transformed[index] = (pixel & 0xff00ff00U) | ((((pixel >> 16) - green) & 0xffU) << 16) | ((pixel - green) & 0xffU);
	}

	/* The inverse restores the pixels. */
	webpc_vp8l_add_green(transformed, count);
	for (index = 0U; index < count; index++)
		CHECK(transformed[index] == image[index]);

	free(image);
	free(transformed);
}

/* Checks the colour-indexing transform against a packing written here, for every bundling and random indices. */
static void
test_roundtrip_indexing(
	unsigned long seed)
{
	unsigned long state;
	unsigned width;
	unsigned height;
	unsigned width_bits;
	unsigned packed_width;
	unsigned colors;
	unsigned bits;
	unsigned x;
	unsigned y;
	unsigned index;
	uint32_t table[WEBPC_VP8L_COLOR_TABLE_SIZE];
	uint32_t *indices;
	uint32_t *packed;
	uint32_t *out;
	uint32_t green;
	size_t count;

	/* The number of colours a bundling allows, and a table that is transparent black past them. */
	state = seed * 2654435761UL + 1UL;
	width_bits = random_below(&state, 4U);
	bits = 8U >> width_bits;
	colors = 1U + random_below(&state, 1U << bits);
	for (index = 0U; index < WEBPC_VP8L_COLOR_TABLE_SIZE; index++)
		table[index] = 0U;
	for (index = 0U; index < colors; index++)
		table[index] = random_pixel(&state);
	width = 1U + random_below(&state, RANDOM_SIDE_MAX);
	height = 1U + random_below(&state, RANDOM_SIDE_MAX);
	packed_width = (width + (1U << width_bits) - 1U) / (1U << width_bits);
	count = (size_t)width * height;
	indices = allocate_pixels(count);
	packed = allocate_pixels((size_t)packed_width * height);
	out = allocate_pixels(count);

	/* Random indices, including ones past the colour count (the bundling allows them). */
	for (index = 0U; index < count; index++)
		indices[index] = random_below(&state, 1U << bits);

	/* The packing: the index of pixel x goes at bit (x % pack) * bits of green, the pixels of a pack from the low bits. */
	for (y = 0U; y < height; y++) {
		for (x = 0U; x < packed_width; x++) {
			/* Random alpha, red and blue, which the transform ignores, and an empty green to fill with indices. */
			packed[(size_t)y * packed_width + x] = random_pixel(&state) & 0xffff00ffU;
		}
		for (x = 0U; x < width; x++) {
			green = (uint32_t)indices[(size_t)y * width + x] << ((x % (1U << width_bits)) * bits);
			packed[(size_t)y * packed_width + (x >> width_bits)] |= green << 8;
		}
	}

	/* The expansion gives the table's colour for every index. */
	webpc_vp8l_inverse_indexing(packed, width, height, width_bits, table, out);
	for (index = 0U; index < count; index++)
		CHECK(out[index] == table[indices[index]]);

	/* An index past the colour count gives transparent black. */
	for (index = 0U; index < count; index++) {
		if (indices[index] >= colors)
			CHECK(out[index] == 0x00000000U);
	}

	free(indices);
	free(packed);
	free(out);
}

/*
 * Runs every inverse on random input that no encoder made, with exactly
 * sized buffers, so that the sanitizers see any access outside them.
 */
static void
test_random_inverse(
	unsigned long seed)
{
	unsigned long state;
	unsigned width;
	unsigned height;
	unsigned size_bits;
	unsigned blocks;
	unsigned index;
	unsigned width_bits;
	unsigned packed_width;
	uint32_t table[WEBPC_VP8L_COLOR_TABLE_SIZE];
	uint32_t *pixels;
	uint32_t *aux;
	uint32_t *out;
	size_t count;

	/* A random image and random blocks of every size. */
	state = seed * 2654435761UL + 1UL;
	width = 1U + random_below(&state, 40U);
	height = 1U + random_below(&state, 40U);
	size_bits = 2U + random_below(&state, 8U);
	blocks = webpc_vp8l_subsample(width, size_bits) * webpc_vp8l_subsample(height, size_bits);
	count = (size_t)width * height;
	pixels = allocate_pixels(count);
	aux = allocate_pixels(blocks);
	for (index = 0U; index < count; index++)
		pixels[index] = random_pixel(&state);
	for (index = 0U; index < blocks; index++)
		aux[index] = random_pixel(&state);

	/* The predictor and the colour transform accept any pixel values. */
	webpc_vp8l_inverse_predictor(pixels, width, height, aux, size_bits);
	webpc_vp8l_inverse_color(pixels, width, height, aux, size_bits);
	webpc_vp8l_add_green(pixels, count);
	CHECK(count != 0U);

	/* The indexing accepts any green, whatever the bundling: indices past the table's colours find its zero entries. */
	for (index = 0U; index < WEBPC_VP8L_COLOR_TABLE_SIZE; index++)
		table[index] = (index < 5U) ? random_pixel(&state) : 0U;
	width_bits = random_below(&state, 4U);
	packed_width = webpc_vp8l_subsample(width, width_bits);
	free(aux);
	aux = allocate_pixels((size_t)packed_width * height);
	out = allocate_pixels(count);
	for (index = 0U; index < packed_width * height; index++)
		aux[index] = random_pixel(&state);
	webpc_vp8l_inverse_indexing(aux, width, height, width_bits, table, out);
	for (index = 0U; index < count; index++) {
		CHECK(out[index] == table[0] || out[index] == table[1] || out[index] == table[2] || out[index] == table[3] ||
		    out[index] == table[4] || out[index] == 0U);
	}

	free(pixels);
	free(aux);
	free(out);
}
