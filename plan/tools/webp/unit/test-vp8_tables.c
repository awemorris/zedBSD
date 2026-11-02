/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8-tables.c: every table against RFC 6386's text read by
 * the extractor (rfc-vp8-tables.h), the shape of the trees and the
 * tables, tree walks worked out by hand and at random, the category
 * bases derived from the extra-bit counts, and a few values read from the
 * RFC's text by eye.
 */

#include "vp8-tables.h"
#include "check.h"
#include "rfc-vp8-tables.h"

#include <string.h>

/* The random generator's state. */
static unsigned long random_state = 0x13579bdfUL;

static unsigned random_below(unsigned limit);
static int walk_tree(const int8_t *tree, const char *bits);
static void check_tree_shape(const int8_t *tree, unsigned entries, unsigned values, const char *name);
static void collect_codes(const int8_t *tree, unsigned index, unsigned depth, unsigned *lengths, unsigned long *space);
static void check_tree_codes(const int8_t *tree, const unsigned *lengths, unsigned values);
static void check_tree_random(const int8_t *tree, unsigned entries, unsigned values);
static void test_trees_against_rfc(void);
static void test_trees_shape(void);
static void test_trees_hand(void);
static void test_trees_random(void);
static void test_mode_probs(void);
static void test_coeff_probs(void);
static void test_bands_zigzag(void);
static void test_categories(void);
static void test_quantizers(void);
static void test_eye_values(void);

/*
 * Runs the tests of vp8-tables.c.
 */
int
main(
	void)
{
	int status;

	/* Each helper in turn. */
	test_trees_against_rfc();
	test_trees_shape();
	test_trees_hand();
	test_trees_random();
	test_mode_probs();
	test_coeff_probs();
	test_bands_zigzag();
	test_categories();
	test_quantizers();
	test_eye_values();

	/* The summary line. */
	status = check_finish("vp8_tables");
	return status;
}

/*
 * Gives a pseudo-random number below limit (limit is at least 1).
 */
static unsigned
random_below(
	unsigned limit)
{
	unsigned long number;

	/* One step of the generator, reduced to the limit. */
	number = check_random(&random_state);
	return (unsigned)(number % (unsigned long)limit);
}

/*
 * Reads a leaf from tree along bits, a string of '0' and '1': the way RFC
 * 6386 8.1's tree reader walks (a positive entry is the next pair, the
 * entry at index + bit).  A path that does not end in a leaf gives -1000.
 */
static int
walk_tree(
	const int8_t *tree,
	const char *bits)
{
	int index;

	/* Start at the root and follow one entry per bit. */
	index = 0;
	for (; *bits != '\0'; bits++) {
		index = tree[index + (*bits == '1' ? 1 : 0)];
		if (index <= 0) {
			/* A leaf: the bits must end here. */
			if (bits[1] != '\0') {
				return -1000;
			}
			return -index;
		}
	}
	return -1000;
}

/*
 * Checks that a tree of entries entries has values leaves, each value
 * exactly once, that its inner entries are even indices ahead of their
 * own pair, and that every pair but the root is the target of exactly one
 * entry (so every path from the root ends in a leaf).
 */
static void
check_tree_shape(
	const int8_t *tree,
	unsigned entries,
	unsigned values,
	const char *name)
{
	unsigned seen[32];
	unsigned target[32];
	unsigned index;
	int entry;

	/* Nothing seen yet. */
	memset(seen, 0, sizeof(seen));
	memset(target, 0, sizeof(target));
	CHECK(entries == 2U * (values - 1U));
	for (index = 0U; index < entries; index++) {
		entry = tree[index];
		if (entry > 0) {
			/* An inner entry points to a pair ahead of this one. */
			CHECK((entry & 1) == 0);
			CHECK((unsigned)entry > index);
			CHECK((unsigned)entry < entries);
			if ((unsigned)entry < entries) {
				target[entry]++;
			}
		} else {
			/* A leaf names a value once. */
			CHECK((unsigned)(-entry) < values);
			if ((unsigned)(-entry) < values) {
				seen[-entry]++;
			}
		}
	}

	/* Every value is a leaf once, and every pair but the root has one parent. */
	for (index = 0U; index < values; index++) {
		CHECK(seen[index] == 1U);
	}
	for (index = 2U; index < entries; index += 2U) {
		CHECK(target[index] == 1U);
	}
	CHECK(target[0] == 0U);
	(void)name;
}

/*
 * Walks every path below the pair at index, recording the length of the
 * code of each leaf in lengths and adding 2^-length to space (in units of
 * 2^-16); depth is the length of the path to the pair's entries.
 */
static void
collect_codes(
	const int8_t *tree,
	unsigned index,
	unsigned depth,
	unsigned *lengths,
	unsigned long *space)
{
	unsigned bit;
	int entry;

	/* Each of the pair's two entries is a leaf or leads to another pair. */
	for (bit = 0U; bit < 2U; bit++) {
		entry = tree[index + bit];
		if (entry > 0) {
			collect_codes(tree, (unsigned)entry, depth + 1U, lengths, space);
		} else {
			lengths[-entry] = depth;
			*space += 1UL << (16U - depth);
		}
	}
}

/*
 * Checks the code length of each leaf against the lengths worked out by
 * hand, and that the lengths fill the code space exactly (the sum of
 * 2^-length is 1).
 */
static void
check_tree_codes(
	const int8_t *tree,
	const unsigned *lengths,
	unsigned values)
{
	unsigned found[16];
	unsigned long space;
	unsigned value;

	/* Every path from the root. */
	memset(found, 0, sizeof(found));
	space = 0UL;
	collect_codes(tree, 0U, 1U, found, &space);
	for (value = 0U; value < values; value++) {
		CHECK(found[value] == lengths[value]);
	}
	CHECK(space == 65536UL);
}

/*
 * Walks 2000 random bit strings of the tree: each ends at a leaf within
 * entries / 2 bits and gives a value in range.
 */
static void
check_tree_random(
	const int8_t *tree,
	unsigned entries,
	unsigned values)
{
	unsigned round;
	unsigned steps;
	int index;

	/* Random bits follow the tree until a leaf. */
	for (round = 0U; round < 2000U; round++) {
		index = 0;
		steps = 0U;
		do {
			index = tree[index + (int)random_below(2U)];
			steps++;
		} while (index > 0 && steps <= entries);
		CHECK(index <= 0);
		CHECK(steps <= entries / 2U);
		CHECK((unsigned)(-index) < values);
	}
}

/* Checks every tree against the extractor's values from RFC 6386's text. */
static void
test_trees_against_rfc(void)
{
	unsigned index;

	/* Each tree entry by entry. */
	for (index = 0U; index < 8U; index++) {
		CHECK(webpc_vp8_kf_ymode_tree[index] == rfc_kf_ymode_tree[index]);
	}
	for (index = 0U; index < 6U; index++) {
		CHECK(webpc_vp8_uv_mode_tree[index] == rfc_uv_mode_tree[index]);
		CHECK(webpc_vp8_segment_tree[index] == rfc_mb_segment_tree[index]);
	}
	for (index = 0U; index < 18U; index++) {
		CHECK(webpc_vp8_bmode_tree[index] == rfc_bmode_tree[index]);
	}
	for (index = 0U; index < 22U; index++) {
		CHECK(webpc_vp8_coeff_tree[index] == rfc_coeff_tree[index]);
	}
}

/* Checks the shape of each tree. */
static void
test_trees_shape(void)
{
	/* The number of leaves is the number of values. */
	check_tree_shape(webpc_vp8_kf_ymode_tree, 8U, 5U, "kf_ymode");
	check_tree_shape(webpc_vp8_uv_mode_tree, 6U, 4U, "uv_mode");
	check_tree_shape(webpc_vp8_bmode_tree, 18U, WEBPC_VP8_BMODES, "bmode");
	check_tree_shape(webpc_vp8_segment_tree, 6U, 4U, "segment");
	check_tree_shape(webpc_vp8_coeff_tree, 22U, WEBPC_VP8_TOKENS, "coeff");
}

/*
 * Checks code lengths and paths worked out by hand from the trees' pairs
 * (the RFC's comments name some codes one bit too long; the pairs rule).
 */
static void
test_trees_hand(void)
{
	/* Lengths by value: DC V H TM B. */
	static const unsigned ymode_lengths[5] = { 3U, 3U, 3U, 3U, 1U };
	/* Lengths by value: DC V H TM. */
	static const unsigned uv_lengths[4] = { 1U, 2U, 3U, 3U };
	/* Lengths by value: DC TM VE HE LD RD VR VL HD HU. */
	static const unsigned bmode_lengths[10] = { 1U, 2U, 3U, 5U, 5U, 6U, 6U, 6U, 7U, 7U };
	/* Lengths by value: every segment id takes two bits. */
	static const unsigned segment_lengths[4] = { 2U, 2U, 2U, 2U };
	/* Lengths by value: 0 to 4, CAT1 to CAT6, end of block. */
	static const unsigned coeff_lengths[12] = { 2U, 3U, 5U, 6U, 6U, 6U, 6U, 7U, 7U, 7U, 7U, 1U };

	/* The lengths from the pairs. */
	check_tree_codes(webpc_vp8_kf_ymode_tree, ymode_lengths, 5U);
	check_tree_codes(webpc_vp8_uv_mode_tree, uv_lengths, 4U);
	check_tree_codes(webpc_vp8_bmode_tree, bmode_lengths, 10U);
	check_tree_codes(webpc_vp8_segment_tree, segment_lengths, 4U);
	check_tree_codes(webpc_vp8_coeff_tree, coeff_lengths, 12U);

	/* The key frame luma tree: "0" B_PRED, "100" DC, "101" V, "110" H, "111" TM. */
	CHECK(walk_tree(webpc_vp8_kf_ymode_tree, "0") == (int)WEBPC_VP8_B_PRED);
	CHECK(walk_tree(webpc_vp8_kf_ymode_tree, "100") == (int)WEBPC_VP8_DC_PRED);
	CHECK(walk_tree(webpc_vp8_kf_ymode_tree, "101") == (int)WEBPC_VP8_V_PRED);
	CHECK(walk_tree(webpc_vp8_kf_ymode_tree, "110") == (int)WEBPC_VP8_H_PRED);
	CHECK(walk_tree(webpc_vp8_kf_ymode_tree, "111") == (int)WEBPC_VP8_TM_PRED);

	/* The chroma tree: "0" DC, "10" V, "110" H, "111" TM. */
	CHECK(walk_tree(webpc_vp8_uv_mode_tree, "0") == (int)WEBPC_VP8_DC_PRED);
	CHECK(walk_tree(webpc_vp8_uv_mode_tree, "10") == (int)WEBPC_VP8_V_PRED);
	CHECK(walk_tree(webpc_vp8_uv_mode_tree, "110") == (int)WEBPC_VP8_H_PRED);
	CHECK(walk_tree(webpc_vp8_uv_mode_tree, "111") == (int)WEBPC_VP8_TM_PRED);

	/* The subblock tree. */
	CHECK(walk_tree(webpc_vp8_bmode_tree, "0") == (int)WEBPC_VP8_B_DC_PRED);
	CHECK(walk_tree(webpc_vp8_bmode_tree, "10") == (int)WEBPC_VP8_B_TM_PRED);
	CHECK(walk_tree(webpc_vp8_bmode_tree, "110") == (int)WEBPC_VP8_B_VE_PRED);
	CHECK(walk_tree(webpc_vp8_bmode_tree, "11100") == (int)WEBPC_VP8_B_HE_PRED);
	CHECK(walk_tree(webpc_vp8_bmode_tree, "111010") == (int)WEBPC_VP8_B_RD_PRED);
	CHECK(walk_tree(webpc_vp8_bmode_tree, "111011") == (int)WEBPC_VP8_B_VR_PRED);
	CHECK(walk_tree(webpc_vp8_bmode_tree, "11110") == (int)WEBPC_VP8_B_LD_PRED);
	CHECK(walk_tree(webpc_vp8_bmode_tree, "111110") == (int)WEBPC_VP8_B_VL_PRED);
	CHECK(walk_tree(webpc_vp8_bmode_tree, "1111110") == (int)WEBPC_VP8_B_HD_PRED);
	CHECK(walk_tree(webpc_vp8_bmode_tree, "1111111") == (int)WEBPC_VP8_B_HU_PRED);

	/* The segment tree: the two bits are the id. */
	CHECK(walk_tree(webpc_vp8_segment_tree, "00") == 0);
	CHECK(walk_tree(webpc_vp8_segment_tree, "01") == 1);
	CHECK(walk_tree(webpc_vp8_segment_tree, "10") == 2);
	CHECK(walk_tree(webpc_vp8_segment_tree, "11") == 3);

	/* The token tree. */
	CHECK(walk_tree(webpc_vp8_coeff_tree, "0") == (int)WEBPC_VP8_DCT_EOB);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "10") == (int)WEBPC_VP8_DCT_0);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "110") == (int)WEBPC_VP8_DCT_1);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "11100") == (int)WEBPC_VP8_DCT_2);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "111010") == (int)WEBPC_VP8_DCT_3);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "111011") == (int)WEBPC_VP8_DCT_4);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "111100") == (int)WEBPC_VP8_DCT_CAT1);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "111101") == (int)WEBPC_VP8_DCT_CAT2);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "1111100") == (int)WEBPC_VP8_DCT_CAT3);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "1111101") == (int)WEBPC_VP8_DCT_CAT4);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "1111110") == (int)WEBPC_VP8_DCT_CAT5);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "1111111") == (int)WEBPC_VP8_DCT_CAT6);

	/* A path that stops early or runs on past a leaf is not a code. */
	CHECK(walk_tree(webpc_vp8_coeff_tree, "11") == -1000);
	CHECK(walk_tree(webpc_vp8_coeff_tree, "00") == -1000);

	/* After a DCT_0 the token is read from entry 2: "0" is DCT_0 again, no end of block. */
	CHECK(webpc_vp8_coeff_tree[2] == 0);
	CHECK(webpc_vp8_coeff_tree[0] == -(int)WEBPC_VP8_DCT_EOB);
}

/* Checks random walks over each tree. */
static void
test_trees_random(void)
{
	/* Every walk ends in a leaf of the tree's values. */
	check_tree_random(webpc_vp8_kf_ymode_tree, 8U, 5U);
	check_tree_random(webpc_vp8_uv_mode_tree, 6U, 4U);
	check_tree_random(webpc_vp8_bmode_tree, 18U, WEBPC_VP8_BMODES);
	check_tree_random(webpc_vp8_segment_tree, 6U, 4U);
	check_tree_random(webpc_vp8_coeff_tree, 22U, WEBPC_VP8_TOKENS);
}

/* Checks the fixed mode probabilities against the RFC and their range. */
static void
test_mode_probs(void)
{
	unsigned above;
	unsigned left;
	unsigned node;
	unsigned index;

	/* The luma and chroma probabilities. */
	for (index = 0U; index < 4U; index++) {
		CHECK(webpc_vp8_kf_ymode_prob[index] == rfc_kf_ymode_prob[index]);
		CHECK(webpc_vp8_kf_ymode_prob[index] >= 1U);
	}
	for (index = 0U; index < 3U; index++) {
		CHECK(webpc_vp8_kf_uv_mode_prob[index] == rfc_kf_uv_mode_prob[index]);
		CHECK(webpc_vp8_kf_uv_mode_prob[index] >= 1U);
	}

	/* The subblock probabilities, [above][left][node], all 900. */
	for (above = 0U; above < WEBPC_VP8_BMODES; above++) {
		for (left = 0U; left < WEBPC_VP8_BMODES; left++) {
			for (node = 0U; node < WEBPC_VP8_BMODES - 1U; node++) {
				index = (above * WEBPC_VP8_BMODES + left) * (WEBPC_VP8_BMODES - 1U) + node;
				CHECK(webpc_vp8_kf_bmode_prob[above][left][node] == rfc_kf_bmode_prob[index]);
				CHECK(webpc_vp8_kf_bmode_prob[above][left][node] >= 1U);
			}
		}
	}
}

/* Checks the token probabilities and their update probabilities against the RFC. */
static void
test_coeff_probs(void)
{
	unsigned type;
	unsigned band;
	unsigned context;
	unsigned node;
	unsigned index;

	/* All 1056 of each, in the order type, band, context, node. */
	for (type = 0U; type < WEBPC_VP8_COEFF_TYPES; type++) {
		for (band = 0U; band < WEBPC_VP8_COEFF_BANDS; band++) {
			for (context = 0U; context < WEBPC_VP8_COEFF_CONTEXTS; context++) {
				for (node = 0U; node < WEBPC_VP8_COEFF_NODES; node++) {
					index = ((type * WEBPC_VP8_COEFF_BANDS + band) * WEBPC_VP8_COEFF_CONTEXTS + context)
					    * WEBPC_VP8_COEFF_NODES + node;
					CHECK(webpc_vp8_coeff_default[type][band][context][node] == rfc_default_coeff_probs[index]);
					CHECK(webpc_vp8_coeff_update[type][band][context][node] == rfc_coeff_update_probs[index]);
					CHECK(webpc_vp8_coeff_default[type][band][context][node] >= 1U);
					CHECK(webpc_vp8_coeff_update[type][band][context][node] >= 1U);
				}
			}
		}
	}
}

/* Checks the bands and the zig-zag scan. */
static void
test_bands_zigzag(void)
{
	unsigned char used[16];
	unsigned index;
	int row;
	int col;

	/* The 16 bands match the RFC and are in range; the 17th is 0. */
	for (index = 0U; index < 16U; index++) {
		CHECK(webpc_vp8_coeff_bands[index] == rfc_coeff_bands[index]);
		CHECK(webpc_vp8_coeff_bands[index] < WEBPC_VP8_COEFF_BANDS);
	}
	CHECK(webpc_vp8_coeff_bands[16] == 0U);

	/* The zig-zag scan matches the RFC and is a permutation of 0..15 that starts at 0 and ends at 15. */
	memset(used, 0, sizeof(used));
	for (index = 0U; index < 16U; index++) {
		CHECK(webpc_vp8_zigzag[index] == rfc_zigzag[index]);
		CHECK(webpc_vp8_zigzag[index] < 16U);
		if (webpc_vp8_zigzag[index] < 16U) {
			used[webpc_vp8_zigzag[index]]++;
		}
	}
	for (index = 0U; index < 16U; index++) {
		CHECK(used[index] == 1U);
	}
	CHECK(webpc_vp8_zigzag[0] == 0U);
	CHECK(webpc_vp8_zigzag[15] == 15U);

	/* Consecutive positions of the scan are neighbours (at most 3 apart in both axes). */
	for (index = 1U; index < 16U; index++) {
		row = (int)(webpc_vp8_zigzag[index] / 4U) - (int)(webpc_vp8_zigzag[index - 1U] / 4U);
		col = (int)(webpc_vp8_zigzag[index] % 4U) - (int)(webpc_vp8_zigzag[index - 1U] % 4U);
		CHECK(row >= -3 && row <= 3);
		CHECK(col >= -3 && col <= 3);
	}
}

/*
 * Checks the category tables: the rows against Pcat1 to Pcat6, the extra
 * bit counts 1, 2, 3, 4, 5, 11, and the bases derived from them (5, then
 * each base plus 2 to the power of the previous count).
 */
static void
test_categories(void)
{
	static const int *const rows[6] = { rfc_pcat1, rfc_pcat2, rfc_pcat3, rfc_pcat4, rfc_pcat5, rfc_pcat6 };
	static const unsigned sizes[6] = { 2U, 3U, 4U, 5U, 6U, 12U };
	static const unsigned bits[6] = { 1U, 2U, 3U, 4U, 5U, 11U };
	static const unsigned bases[6] = { 5U, 7U, 11U, 19U, 35U, 67U };
	unsigned row;
	unsigned index;
	unsigned count;
	unsigned base;

	/* Each row matches its RFC array, ends with 0, and is 0 after. */
	for (row = 0U; row < 6U; row++) {
		for (index = 0U; index < 12U; index++) {
			if (index < sizes[row]) {
				CHECK(webpc_vp8_cat_probs[row][index] == rows[row][index]);
			} else {
				CHECK(webpc_vp8_cat_probs[row][index] == 0U);
			}
		}
		count = 0U;
		while (webpc_vp8_cat_probs[row][count] != 0U) {
			count++;
		}
		CHECK(count == bits[row]);
		CHECK(count == sizes[row] - 1U);
	}

	/* The bases are the listed numbers and follow from the extra bit counts. */
	base = 5U;
	for (row = 0U; row < 6U; row++) {
		CHECK(webpc_vp8_cat_base[row] == bases[row]);
		CHECK(webpc_vp8_cat_base[row] == base);
		base += 1U << bits[row];
	}
	/* The last category tops out at 2048 + 66 = DCT_CAT6's largest magnitude 2114. */
	CHECK(base - 1U == 2114U);
}

/* Checks the quantizer tables against the RFC, their range and their order. */
static void
test_quantizers(void)
{
	unsigned index;

	/* Every entry against the RFC. */
	for (index = 0U; index < WEBPC_VP8_QINDEX_RANGE; index++) {
		CHECK(webpc_vp8_dc_q[index] == rfc_dc_qlookup[index]);
		CHECK(webpc_vp8_ac_q[index] == rfc_ac_qlookup[index]);
	}

	/* The ends and the order: both rise, the AC steps strictly, the DC steps with a few equal pairs. */
	CHECK(webpc_vp8_dc_q[0] == 4U);
	CHECK(webpc_vp8_dc_q[127] == 157U);
	CHECK(webpc_vp8_ac_q[0] == 4U);
	CHECK(webpc_vp8_ac_q[127] == 284U);
	for (index = 1U; index < WEBPC_VP8_QINDEX_RANGE; index++) {
		CHECK(webpc_vp8_dc_q[index] >= webpc_vp8_dc_q[index - 1U]);
		CHECK(webpc_vp8_ac_q[index] > webpc_vp8_ac_q[index - 1U]);
		CHECK(webpc_vp8_dc_q[index] - webpc_vp8_dc_q[index - 1U] <= 4);
		CHECK(webpc_vp8_ac_q[index] - webpc_vp8_ac_q[index - 1U] <= 6);
	}
	for (index = 0U; index < WEBPC_VP8_QINDEX_RANGE; index++) {
		CHECK(webpc_vp8_dc_q[index] >= 4U && webpc_vp8_dc_q[index] <= 157U);
		CHECK(webpc_vp8_ac_q[index] >= 4U && webpc_vp8_ac_q[index] <= 284U);
	}

	/* The AC step equals the index plus 4 up to index 54, where the RFC's list first skips 59. */
	for (index = 0U; index <= 54U; index++) {
		CHECK(webpc_vp8_ac_q[index] == index + 4U);
	}
	CHECK(webpc_vp8_ac_q[55] == 60U);
}

/* Checks values read from RFC 6386's text by eye, apart from the extractor. */
static void
test_eye_values(void)
{
	/* kf_bmode_prob: the first row of [0][0], and the first of [1][0] (RFC 11.5). */
	CHECK(webpc_vp8_kf_bmode_prob[0][0][0] == 231U);
	CHECK(webpc_vp8_kf_bmode_prob[0][0][8] == 112U);
	CHECK(webpc_vp8_kf_bmode_prob[0][9][0] == 81U);
	CHECK(webpc_vp8_kf_bmode_prob[0][9][8] == 36U);
	CHECK(webpc_vp8_kf_bmode_prob[1][0][0] == 134U);

	/* default_coeff_probs (RFC 13.5): type 0 band 0 is all 128; band 1 context 0; band 2 context 0 and 1. */
	CHECK(webpc_vp8_coeff_default[0][0][0][0] == 128U);
	CHECK(webpc_vp8_coeff_default[0][0][2][10] == 128U);
	CHECK(webpc_vp8_coeff_default[0][1][0][0] == 253U);
	CHECK(webpc_vp8_coeff_default[0][1][0][1] == 136U);
	CHECK(webpc_vp8_coeff_default[0][1][2][0] == 106U);
	CHECK(webpc_vp8_coeff_default[0][2][0][0] == 1U);
	CHECK(webpc_vp8_coeff_default[0][2][0][1] == 98U);
	CHECK(webpc_vp8_coeff_default[0][2][1][0] == 181U);
	CHECK(webpc_vp8_coeff_default[3][7][0][1] == 1U);
	CHECK(webpc_vp8_coeff_default[3][7][2][0] == 238U);

	/* coeff_update_probs (RFC 13.4): type 0 band 0 is all 255; band 1; the last band. */
	CHECK(webpc_vp8_coeff_update[0][0][0][0] == 255U);
	CHECK(webpc_vp8_coeff_update[0][1][0][0] == 176U);
	CHECK(webpc_vp8_coeff_update[0][1][0][1] == 246U);
	CHECK(webpc_vp8_coeff_update[0][1][1][0] == 223U);
	CHECK(webpc_vp8_coeff_update[0][1][2][0] == 249U);
	CHECK(webpc_vp8_coeff_update[0][2][1][0] == 234U);
	CHECK(webpc_vp8_coeff_update[0][6][1][0] == 250U);
	CHECK(webpc_vp8_coeff_update[0][7][1][5] == 255U);

	/* The quantizer tables around the RFC's eye-read values. */
	CHECK(webpc_vp8_dc_q[7] == 10U);
	CHECK(webpc_vp8_dc_q[8] == 11U);
	CHECK(webpc_vp8_dc_q[100] == 98U);
	CHECK(webpc_vp8_ac_q[100] == 167U);
}
