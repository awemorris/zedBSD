/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the weighted sample prediction of the software H.264 decoder
 * (8.4.2.3, 8.4.3): the choice of the default, explicit and implicit
 * weights, the factors of each, and the combination of one or two
 * predictions with them.  The expected values are worked out by hand from
 * the equations 8-271 to 8-283 (the equation numbers are named at each
 * check), never taken from another decoder's output.
 */

#include <limits.h>
#include <string.h>

#include "check.h"
#include "userland/desktop/libmedia/h264sw-weight.h"

/*
 * The decoding state the tests build by hand: a slice header (large, so
 * static), a picture parameter set, the reference lists and the weights.
 */
static struct h264sw_slice_header test_header;
static struct h264sw_pps test_pps;
static struct h264sw_reflist test_lists;
static struct h264sw_weights test_weights;

/* Forward declarations of the static functions. */
static void reset_state(unsigned slice_type);
static void set_ref(unsigned list, unsigned index, int32_t poc, int long_term);
static void make_factors(struct h264sw_weight_factors *factors, int weighted, unsigned log_wd, int w0, int w1, int o0, int o1);
static uint8_t apply_one(const struct h264sw_weight_factors *factors, int has_p0, uint8_t p0, int has_p1, uint8_t p1);
static void test_default(void);
static void test_explicit_single(void);
static void test_explicit_bi(void);
static void test_apply_blocks(void);
static void test_mode_selection(void);
static void test_explicit_factors(void);
static void test_implicit_weights(void);
static void test_implicit_factors(void);
static void test_hostile(void);

/* Clears the state and gives the header the slice type. */
static void
reset_state(
	unsigned slice_type)
{
	/* Clears every structure, then names the slice type. */
	memset(&test_header, 0, sizeof(test_header));
	memset(&test_pps, 0, sizeof(test_pps));
	memset(&test_lists, 0, sizeof(test_lists));
	memset(&test_weights, 0, sizeof(test_weights));
	test_header.slice_type = slice_type;
}

/* Puts a reference picture with an order count into a list. */
static void
set_ref(
	unsigned list,
	unsigned index,
	int32_t poc,
	int long_term)
{
	/* Only the order count and the marking matter to the weights. */
	test_lists.list[list][index].poc = poc;
	test_lists.list[list][index].long_term = long_term;
}

/* Fills a factors structure. */
static void
make_factors(
	struct h264sw_weight_factors *factors,
	int weighted,
	unsigned log_wd,
	int w0,
	int w1,
	int o0,
	int o1)
{
	/* Stores each factor. */
	factors->weighted = weighted;
	factors->log_wd = log_wd;
	factors->w0 = w0;
	factors->w1 = w1;
	factors->o0 = o0;
	factors->o1 = o1;
}

/* Combines one sample of each list (or of one list) and reports the result. */
static uint8_t
apply_one(
	const struct h264sw_weight_factors *factors,
	int has_p0,
	uint8_t p0,
	int has_p1,
	uint8_t p1)
{
	uint8_t out;

	/* Passes the lists that are in use as pointers, the others as NULL. */
	out = 0xEEU;
	h264sw_weights_apply(factors, has_p0 ? &p0 : NULL, has_p1 ? &p1 : NULL, 1U, 1U, 1U, &out, 1U);

	/* Reports the sample. */
	return out;
}

/*
 * Checks the default prediction: one list is copied, two are averaged with upward rounding (8-271 to 8-273).
 */
static void
test_default(void)
{
	struct h264sw_weight_factors factors;

	make_factors(&factors, 0, 0U, 0, 0, 0, 0);

	/* One list is copied, list 0 or list 1 (8-271, 8-272). */
	CHECK(apply_one(&factors, 1, 77U, 0, 0U) == 77U);
	CHECK(apply_one(&factors, 0, 0U, 1, 201U) == 201U);

	/* Two lists are averaged: ( 3 + 4 + 1 ) >> 1 = 4, ( 10 + 20 + 1 ) >> 1 = 15, ( 255 + 255 + 1 ) >> 1 = 255, ( 0 + 0 + 1 ) >> 1 = 0 (8-273). */
	CHECK(apply_one(&factors, 1, 3U, 1, 4U) == 4U);
	CHECK(apply_one(&factors, 1, 10U, 1, 20U) == 15U);
	CHECK(apply_one(&factors, 1, 255U, 1, 255U) == 255U);
	CHECK(apply_one(&factors, 1, 0U, 1, 0U) == 0U);

	/* The factors of a default prediction are ignored: a stray weight changes nothing. */
	make_factors(&factors, 0, 5U, 99, 99, 99, 99);
	CHECK(apply_one(&factors, 1, 3U, 1, 4U) == 4U);
	CHECK(apply_one(&factors, 1, 50U, 0, 0U) == 50U);
}

/*
 * Checks the explicit prediction from one list (8-274, 8-275).
 */
static void
test_explicit_single(void)
{
	struct h264sw_weight_factors factors;

	/* logWD 0: Clip1( p * w + o ): 10 * 3 - 5 = 25. */
	make_factors(&factors, 1, 0U, 3, 0, -5, 0);
	CHECK(apply_one(&factors, 1, 10U, 0, 0U) == 25U);

	/* logWD 0 clips above: 200 * 2 + 0 = 400 gives 255; and below: 100 * -1 + 50 = -50 gives 0. */
	make_factors(&factors, 1, 0U, 2, 0, 0, 0);
	CHECK(apply_one(&factors, 1, 200U, 0, 0U) == 255U);
	make_factors(&factors, 1, 0U, -1, 0, 50, 0);
	CHECK(apply_one(&factors, 1, 100U, 0, 0U) == 0U);

	/* A negative weight with a large offset: 100 * -1 + 200 = 100. */
	make_factors(&factors, 1, 0U, -1, 0, 200, 0);
	CHECK(apply_one(&factors, 1, 100U, 0, 0U) == 100U);

	/* logWD 5: ( ( p * w + 16 ) >> 5 ) + o: w = 32 keeps the sample, ( 3200 + 16 ) >> 5 = 100. */
	make_factors(&factors, 1, 5U, 32, 0, 0, 0);
	CHECK(apply_one(&factors, 1, 100U, 0, 0U) == 100U);

	/* w = 40: ( 4000 + 16 ) >> 5 = 125 (125.5 rounds down by the shift). */
	make_factors(&factors, 1, 5U, 40, 0, 0, 0);
	CHECK(apply_one(&factors, 1, 100U, 0, 0U) == 125U);

	/* A positive offset is added after the shift: 125 + 10 = 135. */
	make_factors(&factors, 1, 5U, 40, 0, 10, 0);
	CHECK(apply_one(&factors, 1, 100U, 0, 0U) == 135U);

	/* A negative product is shifted toward minus infinity: p = 3, w = -20: ( -60 + 16 ) >> 5 = -44 >> 5 = -2, plus 10 gives 8 (truncation would give 9). */
	make_factors(&factors, 1, 5U, -20, 0, 10, 0);
	CHECK(apply_one(&factors, 1, 3U, 0, 0U) == 8U);

	/* A negative weight with a larger sample: p = 100, w = -20: ( -2000 + 16 ) >> 5 = -1984 >> 5 = -62, plus 70 gives 8. */
	make_factors(&factors, 1, 5U, -20, 0, 70, 0);
	CHECK(apply_one(&factors, 1, 100U, 0, 0U) == 8U);

	/* logWD 7, the largest: w = 128: ( 100 * 128 + 64 ) >> 7 = 12864 >> 7 = 100. */
	make_factors(&factors, 1, 7U, 128, 0, 0, 0);
	CHECK(apply_one(&factors, 1, 100U, 0, 0U) == 100U);

	/* logWD 1: ( p * w + 1 ) >> 1: ( 5 * 3 + 1 ) >> 1 = 8. */
	make_factors(&factors, 1, 1U, 3, 0, 0, 0);
	CHECK(apply_one(&factors, 1, 5U, 0, 0U) == 8U);

	/* A prediction from list 1 alone uses w1 and o1 (8-275), whatever w0 and o0 hold. */
	make_factors(&factors, 1, 0U, 100, 3, 100, -5);
	CHECK(apply_one(&factors, 0, 0U, 1, 10U) == 25U);
	make_factors(&factors, 1, 5U, 7, 40, 7, 10);
	CHECK(apply_one(&factors, 0, 0U, 1, 100U) == 135U);
}

/*
 * Checks the explicit prediction from two lists (8-276).
 */
static void
test_explicit_bi(void)
{
	struct h264sw_weight_factors factors;

	/* logWD 5, w0 = w1 = 32: ( 100 * 32 + 50 * 32 + 32 ) >> 6 = 4832 >> 6 = 75. */
	make_factors(&factors, 1, 5U, 32, 32, 0, 0);
	CHECK(apply_one(&factors, 1, 100U, 1, 50U) == 75U);

	/* The offsets are averaged with upward rounding: ( 3 + 4 + 1 ) >> 1 = 4, so 79. */
	make_factors(&factors, 1, 5U, 32, 32, 3, 4);
	CHECK(apply_one(&factors, 1, 100U, 1, 50U) == 79U);

	/* Negative offsets are averaged toward minus infinity: ( -3 + -3 + 1 ) >> 1 = -5 >> 1 = -3, so 72. */
	make_factors(&factors, 1, 5U, 32, 32, -3, -3);
	CHECK(apply_one(&factors, 1, 100U, 1, 50U) == 72U);

	/* ( -2 + -2 + 1 ) >> 1 = -3 >> 1 = -2, so 73 (truncation would give -1 and 74). */
	make_factors(&factors, 1, 5U, 32, 32, -2, -2);
	CHECK(apply_one(&factors, 1, 100U, 1, 50U) == 73U);

	/* ( -3 + -4 + 1 ) >> 1 = -6 >> 1 = -3, so 72. */
	make_factors(&factors, 1, 5U, 32, 32, -3, -4);
	CHECK(apply_one(&factors, 1, 100U, 1, 50U) == 72U);

	/* logWD 0: ( p0 * w0 + p1 * w1 + 1 ) >> 1 = ( 10 + 20 + 1 ) >> 1 = 15. */
	make_factors(&factors, 1, 0U, 1, 1, 0, 0);
	CHECK(apply_one(&factors, 1, 10U, 1, 20U) == 15U);

	/* Unequal weights with logWD 5: w0 = 48, w1 = 16: ( 100 * 48 + 20 * 16 + 32 ) >> 6 = 5152 >> 6 = 80. */
	make_factors(&factors, 1, 5U, 48, 16, 0, 0);
	CHECK(apply_one(&factors, 1, 100U, 1, 20U) == 80U);

	/* A negative weight: w0 = -20, w1 = 84: ( -2000 + 8400 + 32 ) >> 6 = 6432 >> 6 = 100. */
	make_factors(&factors, 1, 5U, -20, 84, 0, 0);
	CHECK(apply_one(&factors, 1, 100U, 1, 100U) == 100U);

	/* A negative sum clips to 0: w0 = -64, w1 = 0: ( 100 * -64 + 32 ) >> 6 = -6368 >> 6 = -100 gives 0. */
	make_factors(&factors, 1, 5U, -64, 0, 0, 0);
	CHECK(apply_one(&factors, 1, 100U, 1, 100U) == 0U);

	/* A large sum clips to 255: ( 255 * 64 + 255 * 64 + 32 ) >> 6 = 510. */
	make_factors(&factors, 1, 5U, 64, 64, 0, 0);
	CHECK(apply_one(&factors, 1, 255U, 1, 255U) == 255U);

	/* logWD 7: ( p0 * w0 + p1 * w1 + 128 ) >> 8: w0 = w1 = 128: ( 100 * 128 + 100 * 128 + 128 ) >> 8 = 25728 >> 8 = 100. */
	make_factors(&factors, 1, 7U, 128, 128, 0, 0);
	CHECK(apply_one(&factors, 1, 100U, 1, 100U) == 100U);
}

/*
 * Checks whole blocks: strides, the destination being a source, and untouched bytes around the block.
 */
static void
test_apply_blocks(void)
{
	struct h264sw_weight_factors factors;
	uint8_t p0[6 * 4];
	uint8_t p1[6 * 4];
	uint8_t out[8 * 4];
	unsigned row;
	unsigned col;
	int right;

	/* Fills two predictions of 3x2 samples in rows of 6. */
	memset(p0, 0, sizeof(p0));
	memset(p1, 0, sizeof(p1));
	for (row = 0U; row < 2U; row++) {
		for (col = 0U; col < 3U; col++) {
			p0[row * 6U + col] = (uint8_t)(10U * (row * 3U + col));
			p1[row * 6U + col] = (uint8_t)(10U * (row * 3U + col) + 4U);
		}
	}

	/* Averages them into rows of 8: ( a + a + 4 + 1 ) >> 1 = a + 2, and the bytes outside the block stay. */
	make_factors(&factors, 0, 0U, 0, 0, 0, 0);
	memset(out, 0xA5, sizeof(out));
	h264sw_weights_apply(&factors, p0, p1, 6U, 3U, 2U, out, 8U);
	right = 1;
	for (row = 0U; row < 4U; row++) {
		for (col = 0U; col < 8U; col++) {
			if (row < 2U && col < 3U) {
				if (out[row * 8U + col] != (uint8_t)(10U * (row * 3U + col) + 2U))
					right = 0;
			} else {
				if (out[row * 8U + col] != 0xA5U)
					right = 0;
			}
		}
	}
	CHECK(right);

	/* The destination may be the list 0 prediction: the weighted result replaces it. */
	make_factors(&factors, 1, 5U, 32, 32, 0, 0);
	h264sw_weights_apply(&factors, p0, p1, 6U, 3U, 2U, p0, 6U);
	right = 1;
	for (row = 0U; row < 2U; row++) {
		for (col = 0U; col < 3U; col++) {
			if (p0[row * 6U + col] != (uint8_t)(10U * (row * 3U + col) + 2U))
				right = 0;
		}
	}
	CHECK(right);

	/* A single list with weights is applied in place too. */
	make_factors(&factors, 1, 0U, 2, 0, 1, 0);
	h264sw_weights_apply(&factors, p1, NULL, 6U, 3U, 2U, p1, 6U);
	CHECK(p1[0] == 9U);
	CHECK(p1[6 + 2] == (uint8_t)(2U * 54U + 1U));
	CHECK(p1[3] == 0U);
}

/*
 * Checks the choice of the weighting mode of a slice (8.4.2.3).
 */
static void
test_mode_selection(void)
{
	struct h264sw_weight_factors factors;

	/* A P slice weights explicitly when weighted_pred_flag is 1 and its table was read. */
	reset_state(H264SW_SLICE_P);
	test_pps.weighted_pred = 1;
	test_header.weighted = 1;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	CHECK(test_weights.mode == H264SW_WEIGHT_EXPLICIT);
	CHECK(test_weights.header == &test_header);

	/* An SP slice is weighted like a P slice. */
	reset_state(H264SW_SLICE_SP);
	test_pps.weighted_pred = 1;
	test_header.weighted = 1;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	CHECK(test_weights.mode == H264SW_WEIGHT_EXPLICIT);

	/* A P slice is default when weighted_pred_flag is 0, even with weighted_bipred_idc set. */
	reset_state(H264SW_SLICE_P);
	test_pps.weighted_pred = 0;
	test_pps.weighted_bipred_idc = 2U;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	CHECK(test_weights.mode == H264SW_WEIGHT_DEFAULT);

	/* A P slice without a table (the header read none) is default. */
	reset_state(H264SW_SLICE_P);
	test_pps.weighted_pred = 1;
	test_header.weighted = 0;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	CHECK(test_weights.mode == H264SW_WEIGHT_DEFAULT);

	/* A B slice follows weighted_bipred_idc: 0 default, 1 explicit, 2 implicit, whatever weighted_pred_flag says. */
	reset_state(H264SW_SLICE_B);
	test_pps.weighted_pred = 1;
	test_pps.weighted_bipred_idc = 0U;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	CHECK(test_weights.mode == H264SW_WEIGHT_DEFAULT);
	test_pps.weighted_bipred_idc = 1U;
	test_header.weighted = 1;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	CHECK(test_weights.mode == H264SW_WEIGHT_EXPLICIT);
	test_pps.weighted_bipred_idc = 2U;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	CHECK(test_weights.mode == H264SW_WEIGHT_IMPLICIT);

	/* A weighted_bipred_idc outside 0..2 (3 is reserved) predicts by default. */
	test_pps.weighted_bipred_idc = 3U;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	CHECK(test_weights.mode == H264SW_WEIGHT_DEFAULT);

	/* An I slice is default. */
	reset_state(H264SW_SLICE_I);
	test_pps.weighted_pred = 1;
	test_pps.weighted_bipred_idc = 2U;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	CHECK(test_weights.mode == H264SW_WEIGHT_DEFAULT);

	/* The default mode gives the default factors for any reference. */
	make_factors(&factors, 1, 3U, 4, 5, 6, 7);
	h264sw_weights_of(&test_weights, 0, 0, 0U, &factors);
	CHECK(factors.weighted == 0);
}

/*
 * Checks the factors of the explicit mode (8-284 to 8-297 for a frame).
 */
static void
test_explicit_factors(void)
{
	struct h264sw_weight_factors factors;

	/* A B slice with explicit weights and a table of distinct values. */
	reset_state(H264SW_SLICE_B);
	test_pps.weighted_bipred_idc = 1U;
	test_header.weighted = 1;
	test_header.luma_log2_denom = 5U;
	test_header.chroma_log2_denom = 3U;
	test_header.weight[0][1].luma_weight = 30;
	test_header.weight[0][1].luma_offset = -4;
	test_header.weight[0][1].chroma_weight[0] = 5;
	test_header.weight[0][1].chroma_weight[1] = 6;
	test_header.weight[0][1].chroma_offset[0] = 7;
	test_header.weight[0][1].chroma_offset[1] = -8;
	test_header.weight[1][2].luma_weight = 41;
	test_header.weight[1][2].luma_offset = 9;
	test_header.weight[1][2].chroma_weight[0] = -11;
	test_header.weight[1][2].chroma_weight[1] = 12;
	test_header.weight[1][2].chroma_offset[0] = 13;
	test_header.weight[1][2].chroma_offset[1] = -14;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	CHECK(test_weights.mode == H264SW_WEIGHT_EXPLICIT);

	/* Luma of both lists: logWD is luma_log2_weight_denom, the weights and offsets are those of the two reference indices (8-288 to 8-292). */
	memset(&factors, 0x7F, sizeof(factors));
	h264sw_weights_of(&test_weights, 1, 2, 0U, &factors);
	CHECK(factors.weighted == 1);
	CHECK(factors.log_wd == 5U);
	CHECK(factors.w0 == 30 && factors.o0 == -4);
	CHECK(factors.w1 == 41 && factors.o1 == 9);

	/* Cb: the chroma denominator, weights and offsets (8-293 to 8-297). */
	h264sw_weights_of(&test_weights, 1, 2, 1U, &factors);
	CHECK(factors.weighted == 1);
	CHECK(factors.log_wd == 3U);
	CHECK(factors.w0 == 5 && factors.o0 == 7);
	CHECK(factors.w1 == -11 && factors.o1 == 13);

	/* Cr: the second chroma entries. */
	h264sw_weights_of(&test_weights, 1, 2, 2U, &factors);
	CHECK(factors.weighted == 1);
	CHECK(factors.log_wd == 3U);
	CHECK(factors.w0 == 6 && factors.o0 == -8);
	CHECK(factors.w1 == 12 && factors.o1 == -14);

	/* Only list 0 in use: the list 1 factors are not used and are 0. */
	h264sw_weights_of(&test_weights, 1, -1, 0U, &factors);
	CHECK(factors.weighted == 1);
	CHECK(factors.w0 == 30 && factors.o0 == -4);
	CHECK(factors.w1 == 0 && factors.o1 == 0);

	/* Only list 1 in use: explicit weights apply to either list (8.4.3 explicitModeFlag). */
	h264sw_weights_of(&test_weights, -1, 2, 0U, &factors);
	CHECK(factors.weighted == 1);
	CHECK(factors.w1 == 41 && factors.o1 == 9);
	CHECK(factors.w0 == 0 && factors.o0 == 0);

	/* A P slice with explicit weights has only list 0. */
	reset_state(H264SW_SLICE_P);
	test_pps.weighted_pred = 1;
	test_header.weighted = 1;
	test_header.luma_log2_denom = 6U;
	test_header.weight[0][0].luma_weight = 70;
	test_header.weight[0][0].luma_offset = 3;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	h264sw_weights_of(&test_weights, 0, -1, 0U, &factors);
	CHECK(factors.weighted == 1 && factors.log_wd == 6U && factors.w0 == 70 && factors.o0 == 3);

	/* The reference indices of the table's ends are in range: 31 is the last. */
	test_header.weight[0][31].luma_weight = -128;
	test_header.weight[1][31].luma_weight = 127;
	h264sw_weights_of(&test_weights, 31, 31, 0U, &factors);
	CHECK(factors.w0 == -128 && factors.w1 == 127);
}

/*
 * Checks the implicit weights of the pairs of references (8-197, 8-198, 8-201, 8-202, 8-280 to 8-283).
 *
 * Each case gives the order counts of pic0 (list 0), pic1 (list 1) and the
 * current picture, then tb = Clip3( -128, 127, cur - pic0 ), td = Clip3(
 * -128, 127, pic1 - pic0 ), tx = ( 16384 + Abs( td / 2 ) ) / td and
 * DistScaleFactor = Clip3( -1024, 1023, ( tb * tx + 32 ) >> 6 ), worked by
 * hand.
 */
static void
test_implicit_weights(void)
{
	/* A B picture between its references: pic0 at 0, pic1 at 8, the current at 4: tb 4, td 8, tx = 16388 / 8 = 2048, DSF = ( 8192 + 32 ) >> 6 = 128, w1 = 32, w0 = 32. */
	reset_state(H264SW_SLICE_B);
	test_pps.weighted_bipred_idc = 2U;
	test_lists.count[0] = 1U;
	test_lists.count[1] = 1U;
	set_ref(0U, 0U, 0, 0);
	set_ref(1U, 0U, 8, 0);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 4);
	CHECK(test_weights.mode == H264SW_WEIGHT_IMPLICIT);
	CHECK(test_weights.implicit_w0[0][0] == 32);

	/* The current at 2: tb 2, DSF = ( 2 * 2048 + 32 ) >> 6 = 4128 >> 6 = 64, DSF >> 2 = 16, w0 = 48 (w1 = 16). */
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 2);
	CHECK(test_weights.implicit_w0[0][0] == 48);

	/* The current at 6: tb 6, DSF = ( 12288 + 32 ) >> 6 = 12320 >> 6 = 192, DSF >> 2 = 48, w0 = 16 (w1 = 48). */
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 6);
	CHECK(test_weights.implicit_w0[0][0] == 16);

	/* The current beyond pic1, at 12: tb 12, DSF = ( 24576 + 32 ) >> 6 = 24608 >> 6 = 384, DSF >> 2 = 96, w0 = -32 (w1 = 96), inside -64..128. */
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 12);
	CHECK(test_weights.implicit_w0[0][0] == -32);

	/* At 16: DSF = ( 32768 + 32 ) >> 6 = 512, DSF >> 2 = 128, which is not above 128, so w0 = 64 - 128 = -64. */
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 16);
	CHECK(test_weights.implicit_w0[0][0] == -64);

	/* At 17: DSF = ( 34816 + 32 ) >> 6 = 544, DSF >> 2 = 136, above 128: 32. */
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 17);
	CHECK(test_weights.implicit_w0[0][0] == 32);

	/* Before pic0, at -16: tb -16, DSF = ( -32768 + 32 ) >> 6 = -32736 >> 6 = -512 (floor of -511.5), DSF >> 2 = -128, below -64: 32. */
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, -16);
	CHECK(test_weights.implicit_w0[0][0] == 32);

	/* At -4: tb -4, DSF = ( -8192 + 32 ) >> 6 = -8160 >> 6 = -128 (floor of -127.5), DSF >> 2 = -32, w0 = 96 (w1 = -32). */
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, -4);
	CHECK(test_weights.implicit_w0[0][0] == 96);

	/* Reversed references, pic0 at 8 and pic1 at 0, the current at 2: tb -6, td -8, tx = ( 16384 + 4 ) / -8 = -2048 (truncated), DSF = ( 12288 + 32 ) >> 6 = 192, w0 = 64 - 48 = 16. */
	set_ref(0U, 0U, 8, 0);
	set_ref(1U, 0U, 0, 0);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 2);
	CHECK(test_weights.implicit_w0[0][0] == 16);

	/* An odd distance, pic0 at 0, pic1 at 3, the current at 1: tb 1, td 3, tx = 16385 / 3 = 5461, DSF = ( 5461 + 32 ) >> 6 = 85, DSF >> 2 = 21, w0 = 43 (w1 = 21). */
	set_ref(0U, 0U, 0, 0);
	set_ref(1U, 0U, 3, 0);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 1);
	CHECK(test_weights.implicit_w0[0][0] == 43);

	/* The same with the references swapped, pic0 at 3, pic1 at 0, the current at 2: tb -1, td -3, tx = 16385 / -3 = -5461, DSF = ( 5461 + 32 ) >> 6 = 85, w0 = 43. */
	set_ref(0U, 0U, 3, 0);
	set_ref(1U, 0U, 0, 0);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 2);
	CHECK(test_weights.implicit_w0[0][0] == 43);

	/* Equal order counts of pic0 and pic1 (DiffPicOrderCnt of 0) give 32. */
	set_ref(0U, 0U, 5, 0);
	set_ref(1U, 0U, 5, 0);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 2);
	CHECK(test_weights.implicit_w0[0][0] == 32);

	/* A long-term reference in either list gives 32 where the short-term pair would give 48. */
	set_ref(0U, 0U, 0, 1);
	set_ref(1U, 0U, 8, 0);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 2);
	CHECK(test_weights.implicit_w0[0][0] == 32);
	set_ref(0U, 0U, 0, 0);
	set_ref(1U, 0U, 8, 1);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 2);
	CHECK(test_weights.implicit_w0[0][0] == 32);
	set_ref(1U, 0U, 8, 0);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 2);
	CHECK(test_weights.implicit_w0[0][0] == 48);

	/* Every pair of the lists is derived: two entries in list 0 and three in list 1. */
	test_lists.count[0] = 2U;
	test_lists.count[1] = 3U;
	set_ref(0U, 0U, 0, 0);
	set_ref(0U, 1U, 4, 0);
	set_ref(1U, 0U, 8, 0);
	set_ref(1U, 1U, 12, 0);
	set_ref(1U, 2U, 4, 0);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 6);

	/* ( pic0 0, pic1 8 ): w0 16. */
	CHECK(test_weights.implicit_w0[0][0] == 16);

	/* ( pic0 0, pic1 12 ), the current at 6: tb 6, td 12, tx = ( 16384 + 6 ) / 12 = 1365, DSF = ( 8190 + 32 ) >> 6 = 8222 >> 6 = 128, w0 = 32. */
	CHECK(test_weights.implicit_w0[0][1] == 32);

	/* ( pic0 0, pic1 4 ): tb 6, td 4, tx = ( 16384 + 2 ) / 4 = 4096, DSF = ( 24576 + 32 ) >> 6 = 384, DSF >> 2 = 96, w0 = -32. */
	CHECK(test_weights.implicit_w0[0][2] == -32);

	/* ( pic0 4, pic1 8 ): tb 2, td 4, tx = 4096, DSF = ( 8192 + 32 ) >> 6 = 128, w0 = 32. */
	CHECK(test_weights.implicit_w0[1][0] == 32);

	/* ( pic0 4, pic1 12 ): tb 2, td 8, tx = 2048, DSF = ( 4096 + 32 ) >> 6 = 64, w0 = 48. */
	CHECK(test_weights.implicit_w0[1][1] == 48);

	/* ( pic0 4, pic1 4 ): equal order counts: 32. */
	CHECK(test_weights.implicit_w0[1][2] == 32);

	/* A pair beyond the lists' lengths is left at 32. */
	CHECK(test_weights.implicit_w0[2][0] == 32);
	CHECK(test_weights.implicit_w0[0][3] == 32);
	CHECK(test_weights.implicit_w0[31][31] == 32);
}

/*
 * Checks the factors of the implicit mode and the prediction made with them (8-277 to 8-283, 8-276).
 */
static void
test_implicit_factors(void)
{
	struct h264sw_weight_factors factors;

	/* A B slice with implicit weights and the pair of the order counts 0, 8 with the current at 2 (w0 = 48, w1 = 16). */
	reset_state(H264SW_SLICE_B);
	test_pps.weighted_bipred_idc = 2U;
	test_lists.count[0] = 1U;
	test_lists.count[1] = 1U;
	set_ref(0U, 0U, 0, 0);
	set_ref(1U, 0U, 8, 0);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 2);

	/* A bi-predicted partition gets logWD 5, w0 and w1 summing to 64 and no offsets, for every component. */
	memset(&factors, 0x7F, sizeof(factors));
	h264sw_weights_of(&test_weights, 0, 0, 0U, &factors);
	CHECK(factors.weighted == 1);
	CHECK(factors.log_wd == 5U);
	CHECK(factors.w0 == 48 && factors.w1 == 16);
	CHECK(factors.o0 == 0 && factors.o1 == 0);
	h264sw_weights_of(&test_weights, 0, 0, 1U, &factors);
	CHECK(factors.weighted == 1 && factors.w0 == 48 && factors.w1 == 16);
	h264sw_weights_of(&test_weights, 0, 0, 2U, &factors);
	CHECK(factors.weighted == 1 && factors.w0 == 48 && factors.w1 == 16);

	/* Predicting with them: ( 100 * 48 + 20 * 16 + 32 ) >> 6 = 5152 >> 6 = 80. */
	h264sw_weights_of(&test_weights, 0, 0, 0U, &factors);
	CHECK(apply_one(&factors, 1, 100U, 1, 20U) == 80U);

	/* A partition of one list is predicted by default in the implicit mode, in list 0 or in list 1. */
	h264sw_weights_of(&test_weights, 0, -1, 0U, &factors);
	CHECK(factors.weighted == 0);
	h264sw_weights_of(&test_weights, -1, 0, 0U, &factors);
	CHECK(factors.weighted == 0);

	/* Equal weights of 32 and 32 average the way the default prediction does: ( 3 * 32 + 4 * 32 + 32 ) >> 6 = 256 >> 6 = 4. */
	make_factors(&factors, 1, 5U, 32, 32, 0, 0);
	CHECK(apply_one(&factors, 1, 3U, 1, 4U) == 4U);
	CHECK(apply_one(&factors, 1, 254U, 1, 255U) == 255U);

	/* A different pair of references takes its own entry. */
	test_lists.count[0] = 2U;
	set_ref(0U, 1U, 4, 0);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 2);
	h264sw_weights_of(&test_weights, 1, 0, 0U, &factors);

	/* The pair ( pic0 4, pic1 8 ), the current at 2: tb -2, td 4, tx = 4096, DSF = ( -8192 + 32 ) >> 6 = -8160 >> 6 = -128 (floor of -127.5), DSF >> 2 = -32, w0 = 96 (w1 = -32). */
	CHECK(factors.w0 == 96 && factors.w1 == -32);
}

/*
 * Checks that missing and out-of-range inputs give the default prediction and do not crash.
 */
static void
test_hostile(void)
{
	struct h264sw_weight_factors factors;
	struct h264sw_weights fresh;
	uint8_t out[4];
	uint8_t p[4];
	int32_t extremes[2];
	unsigned i;

	extremes[0] = INT32_MIN;
	extremes[1] = INT32_MAX;

	/* A missing weights structure, factors, pps or header is tolerated. */
	reset_state(H264SW_SLICE_B);
	h264sw_weights_setup(NULL, &test_pps, &test_header, &test_lists, 0);
	h264sw_weights_setup(&fresh, NULL, &test_header, &test_lists, 0);
	CHECK(fresh.mode == H264SW_WEIGHT_DEFAULT);
	h264sw_weights_setup(&fresh, &test_pps, NULL, &test_lists, 0);
	CHECK(fresh.mode == H264SW_WEIGHT_DEFAULT);
	test_pps.weighted_bipred_idc = 2U;
	h264sw_weights_setup(&fresh, &test_pps, &test_header, NULL, 0);
	CHECK(fresh.mode == H264SW_WEIGHT_IMPLICIT);
	CHECK(fresh.implicit_w0[0][0] == 32);
	h264sw_weights_of(&fresh, 0, 0, 0U, NULL);
	make_factors(&factors, 1, 2U, 3, 4, 5, 6);
	h264sw_weights_of(NULL, 0, 0, 0U, &factors);
	CHECK(factors.weighted == 0);

	/* Reference indices that name no entry, and a component above 2, give the default prediction. */
	test_pps.weighted_bipred_idc = 1U;
	test_header.weighted = 1;
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	make_factors(&factors, 1, 2U, 3, 4, 5, 6);
	h264sw_weights_of(&test_weights, -1, -1, 0U, &factors);
	CHECK(factors.weighted == 0);
	make_factors(&factors, 1, 2U, 3, 4, 5, 6);
	h264sw_weights_of(&test_weights, 32, 0, 0U, &factors);
	CHECK(factors.weighted == 0);
	make_factors(&factors, 1, 2U, 3, 4, 5, 6);
	h264sw_weights_of(&test_weights, 0, 1000, 0U, &factors);
	CHECK(factors.weighted == 0);
	make_factors(&factors, 1, 2U, 3, 4, 5, 6);
	h264sw_weights_of(&test_weights, INT_MAX, INT_MIN, 0U, &factors);
	CHECK(factors.weighted == 0);
	make_factors(&factors, 1, 2U, 3, 4, 5, 6);
	h264sw_weights_of(&test_weights, 0, 0, 3U, &factors);
	CHECK(factors.weighted == 0);
	make_factors(&factors, 1, 2U, 3, 4, 5, 6);
	h264sw_weights_of(&test_weights, 0, 0, 0xffffffffU, &factors);
	CHECK(factors.weighted == 0);

	/* An explicit mode whose header is missing gives the default prediction. */
	test_weights.header = NULL;
	make_factors(&factors, 1, 2U, 3, 4, 5, 6);
	h264sw_weights_of(&test_weights, 0, 0, 0U, &factors);
	CHECK(factors.weighted == 0);

	/* Lists whose counts exceed the arrays are limited, and extreme order counts do not overflow. */
	reset_state(H264SW_SLICE_B);
	test_pps.weighted_bipred_idc = 2U;
	test_lists.count[0] = 1000U;
	test_lists.count[1] = 0xffffffffU;
	for (i = 0U; i < H264SW_MAX_LIST; i++) {
		set_ref(0U, i, extremes[i % 2U], 0);
		set_ref(1U, i, extremes[(i + 1U) % 2U], 0);
	}
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, INT32_MIN);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, INT32_MAX);
	h264sw_weights_setup(&test_weights, &test_pps, &test_header, &test_lists, 0);
	for (i = 0U; i < H264SW_MAX_LIST; i++) {
		/* Each weight stays within what the derivation allows, -64..128, or is 32. */
		CHECK(test_weights.implicit_w0[i][i] >= -64 && test_weights.implicit_w0[i][i] <= 128);
	}

	/* A logWD above 7 is taken as 7 and extreme weights and offsets do not overflow. */
	p[0] = 255U;
	p[1] = 255U;
	make_factors(&factors, 1, 1000U, INT_MAX, INT_MAX, INT_MAX, INT_MAX);
	h264sw_weights_apply(&factors, p, p + 1, 1U, 1U, 1U, out, 1U);
	CHECK(out[0] == 255U);
	make_factors(&factors, 1, 0xffffffffU, INT_MIN, INT_MIN, INT_MIN, INT_MIN);
	h264sw_weights_apply(&factors, p, p + 1, 1U, 1U, 1U, out, 1U);
	CHECK(out[0] == 0U);
	h264sw_weights_apply(&factors, p, NULL, 1U, 1U, 1U, out, 1U);
	CHECK(out[0] == 0U);
	h264sw_weights_apply(&factors, NULL, p, 1U, 1U, 1U, out, 1U);
	CHECK(out[0] == 0U);

	/* A logWD of 1000 acts as 7: w = 128: ( 100 * 128 + 64 ) >> 7 = 100. */
	make_factors(&factors, 1, 1000U, 128, 0, 0, 0);
	CHECK(apply_one(&factors, 1, 100U, 0, 0U) == 100U);

	/* Calls that cannot describe a block write nothing. */
	memset(out, 0xA5, sizeof(out));
	make_factors(&factors, 0, 0U, 0, 0, 0, 0);
	h264sw_weights_apply(NULL, p, p, 1U, 1U, 1U, out, 1U);
	h264sw_weights_apply(&factors, NULL, NULL, 1U, 1U, 1U, out, 1U);
	h264sw_weights_apply(&factors, p, p, 1U, 0U, 1U, out, 1U);
	h264sw_weights_apply(&factors, p, p, 1U, 1U, 0U, out, 1U);
	h264sw_weights_apply(&factors, p, p, 1U, 1U, 1U, NULL, 1U);
	for (i = 0U; i < 4U; i++)
		CHECK(out[i] == 0xA5U);
}

/*
 * Runs every check and prints the summary line.
 */
int
main(void)
{
	test_default();
	test_explicit_single();
	test_explicit_bi();
	test_apply_blocks();
	test_mode_selection();
	test_explicit_factors();
	test_implicit_weights();
	test_implicit_factors();
	test_hostile();

	/* Reports the checks. */
	return check_finish("weight");
}
