/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8-tokens.c: macroblocks' tokens written with the test's
 * boolean encoder and the token coding of RFC 6386 13.2 written out here
 * (its own tree, extra-bit probabilities, bands, zig-zag and context
 * indexes, not the library's tables), read back and compared with the
 * coefficients, contexts and coded bits the test works out by hand; then
 * every token's range, the positions the format treats specially, the
 * dequantization with its 16-bit wrap, chains of macroblocks that carry
 * their contexts from one to the next, skipped macroblocks, and partitions
 * that are empty, cut short or random, and contexts that hold values no
 * block has.
 */

#include "vp8-tokens.h"
#include "check.h"
#include "boolwriter.h"

#include <string.h>

/* The blocks of a macroblock as the test numbers them, and the positions of a block. */
#define TEST_BLOCKS		25U
#define TEST_Y_BLOCKS		16U
#define TEST_POSITIONS		16U

/* The frame the chains of macroblocks cover: macroblocks across and down. */
#define CHAIN_COLUMNS		3U
#define CHAIN_ROWS		2U
#define CHAIN_MACROBLOCKS	(CHAIN_COLUMNS * CHAIN_ROWS)

/*
 * One macroblock's coefficients as the test writes them: the value of each
 * position of each block in coding (zig-zag) order, and the last position
 * the block's tokens cover (one before its first position for a block that
 * is an immediate EOB, 15 for a block that runs to the end without an EOB).
 * Y2 is block 24.
 */
struct mb_spec {
	int values[TEST_BLOCKS][TEST_POSITIONS];
	int last[TEST_BLOCKS];
};

/*
 * The coding of RFC 6386 13.2 as the test knows it: the token tree, the
 * extra-bit probabilities and their count, and the smallest value of each
 * category.
 */
static const int8_t test_tree[22] = {
	-11, 2,
	0, 4,
	-1, 6,
	8, 12,
	-2, 10,
	-3, -4,
	14, 16,
	-5, -6,
	18, 20,
	-7, -8,
	-9, -10
};
static const uint8_t test_cat_probs[6][11] = {
	{ 159 },
	{ 165, 145 },
	{ 173, 148, 140 },
	{ 176, 155, 140, 135 },
	{ 180, 157, 141, 134, 130 },
	{ 254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129 }
};
static const unsigned test_cat_bits[6] = { 1U, 2U, 3U, 4U, 5U, 11U };
static const unsigned test_cat_min[6] = { 5U, 7U, 11U, 19U, 35U, 67U };

/* The band of each position (RFC 6386 13.3) and the raster place of each (13). */
static const unsigned test_bands[16] = { 0U, 1U, 2U, 3U, 6U, 4U, 5U, 6U, 6U, 6U, 6U, 6U, 6U, 6U, 6U, 7U };
static const unsigned test_zigzag[16] = { 0U, 1U, 4U, 8U, 5U, 2U, 3U, 6U, 9U, 12U, 13U, 10U, 7U, 11U, 14U, 15U };

/* The context entries each of the 25 blocks uses on the left and above (RFC 6386 20.16). */
static const unsigned test_left_index[25] = {
	0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 2U, 2U, 2U, 2U, 3U, 3U, 3U, 3U,
	4U, 4U, 5U, 5U, 6U, 6U, 7U, 7U, 8U
};
static const unsigned test_above_index[25] = {
	0U, 1U, 2U, 3U, 0U, 1U, 2U, 3U, 0U, 1U, 2U, 3U, 0U, 1U, 2U, 3U,
	4U, 5U, 4U, 5U, 6U, 7U, 6U, 7U, 8U
};

/* The random generator's state. */
static unsigned long random_state = 0x13579bdfUL;

/* The partition the tests write, kept out of the stack (it is 64 KiB). */
static struct boolwriter writer;

/* The chain's macroblocks: what each codes and whether it is skipped, kept for the reader's pass. */
static struct mb_spec chain_specs[CHAIN_MACROBLOCKS];
static int chain_has_y2[CHAIN_MACROBLOCKS];
static int chain_skip[CHAIN_MACROBLOCKS];

static unsigned random_below(unsigned limit);
static void default_header(struct webpc_vp8_header *header);
static void random_probs_header(struct webpc_vp8_header *header);
static void plain_dequant(struct webpc_vp8_dequant *dequant);
static void random_dequant(struct webpc_vp8_dequant *dequant);
static unsigned block_first(int has_y2, unsigned block);
static void spec_clear(struct mb_spec *spec, int has_y2);
static int random_value(void);
static void spec_random(struct mb_spec *spec, int has_y2);
static unsigned token_of(unsigned magnitude);
static void write_token(struct boolwriter *out, const uint8_t probs[WEBPC_VP8_COEFF_NODES], int after_zero, int value);
static void write_block(struct boolwriter *out, const uint8_t probs[WEBPC_VP8_COEFF_BANDS][WEBPC_VP8_COEFF_CONTEXTS][WEBPC_VP8_COEFF_NODES], unsigned first, unsigned context, const int *values, int last);
static void write_mb(struct boolwriter *out, const struct webpc_vp8_header *header, int has_y2, const struct mb_spec *spec, uint8_t left[WEBPC_VP8_CONTEXTS], uint8_t above[WEBPC_VP8_CONTEXTS]);
static void model_skip(int has_y2, uint8_t left[WEBPC_VP8_CONTEXTS], uint8_t above[WEBPC_VP8_CONTEXTS]);
static int32_t wrap_product(int value, int32_t factor);
static void expect_residue(struct webpc_vp8_residue *want, int has_y2, const struct mb_spec *spec, const struct webpc_vp8_dequant *dequant);
static void check_residue(const struct webpc_vp8_residue *got, const struct webpc_vp8_residue *want);
static void check_contexts(const uint8_t got[WEBPC_VP8_CONTEXTS], const uint8_t want[WEBPC_VP8_CONTEXTS]);
static void run_chain(const struct webpc_vp8_header *header, const struct webpc_vp8_dequant *dequant, unsigned rows, unsigned columns, int random_contexts, unsigned skip_percent);
static void run_single(const struct webpc_vp8_header *header, const struct webpc_vp8_dequant *dequant, int has_y2, const struct mb_spec *spec);
static void test_tokens_default(void);
static void test_ranges(void);
static void test_special_positions(void);
static void test_dequant(void);
static void test_known_wrap(void);
static void test_contexts(void);
static void test_chains(void);
static void test_skip_residue(void);
static void test_bad_contexts(void);
static void test_ended(void);
static void test_random_bytes(void);

/*
 * Runs the tests of vp8-tokens.c.
 */
int
main(
	void)
{
	int status;

	/* Each helper in turn. */
	test_tokens_default();
	test_ranges();
	test_special_positions();
	test_dequant();
	test_known_wrap();
	test_contexts();
	test_chains();
	test_skip_residue();
	test_bad_contexts();
	test_ended();
	test_random_bytes();

	/* The summary line. */
	status = check_finish("vp8_tokens");
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
 * Clears header and fills the token probabilities with the defaults a key
 * frame starts from.
 */
static void
default_header(
	struct webpc_vp8_header *header)
{
	/* Everything else is zero; the probabilities are the table's. */
	memset(header, 0, sizeof(*header));
	memcpy(header->coeff_probs, webpc_vp8_coeff_default, sizeof(header->coeff_probs));
}

/*
 * Clears header and fills the token probabilities with random values from
 * 1 to 255, so that a swapped type, band, context or node shows.
 */
static void
random_probs_header(
	struct webpc_vp8_header *header)
{
	unsigned type;
	unsigned band;
	unsigned context;
	unsigned node;

	/* Everything else is zero. */
	memset(header, 0, sizeof(*header));

	/* Every probability of every type, band and context. */
	for (type = 0U; type < WEBPC_VP8_COEFF_TYPES; type++) {
		for (band = 0U; band < WEBPC_VP8_COEFF_BANDS; band++) {
			for (context = 0U; context < WEBPC_VP8_COEFF_CONTEXTS; context++) {
				for (node = 0U; node < WEBPC_VP8_COEFF_NODES; node++) {
					header->coeff_probs[type][band][context][node] = (uint8_t)(1U + random_below(255U));
				}
			}
		}
	}
}

/*
 * Fills dequant with six different small factors, so that a swapped plane
 * or a swapped DC and AC shows.
 */
static void
plain_dequant(
	struct webpc_vp8_dequant *dequant)
{
	/* Y, Y2 and chroma, DC then AC. */
	dequant->y1[0] = 3;
	dequant->y1[1] = 5;
	dequant->y2[0] = 7;
	dequant->y2[1] = 11;
	dequant->uv[0] = 13;
	dequant->uv[1] = 17;
}

/*
 * Fills dequant with random factors, some of them far outside the range
 * the quantizer tables give, since the caller's array is not trusted.
 */
static void
random_dequant(
	struct webpc_vp8_dequant *dequant)
{
	static const int32_t extremes[6] = { 0, -1, 1, 0x7fffffff, -0x7fffffff - 1, 65536 };
	int32_t *entries[6];
	unsigned index;

	/* The six entries as one list. */
	entries[0] = &dequant->y1[0];
	entries[1] = &dequant->y1[1];
	entries[2] = &dequant->y2[0];
	entries[3] = &dequant->y2[1];
	entries[4] = &dequant->uv[0];
	entries[5] = &dequant->uv[1];

	/* Each is a quantizer's value, an extreme, or a random 32-bit number. */
	for (index = 0U; index < 6U; index++) {
		switch (random_below(3U)) {
		case 0U:
			*entries[index] = (int32_t)(4U + random_below(437U));
			break;
		case 1U:
			*entries[index] = extremes[random_below(6U)];
			break;
		default:
			*entries[index] = (int32_t)(check_random(&random_state) & 0x7fffffffUL);
			break;
		}
	}
}

/*
 * Gives the first position a block codes: 1 for a luma block whose
 * macroblock has Y2, 0 for every other.
 */
static unsigned
block_first(
	int has_y2,
	unsigned block)
{
	/* Only the Y blocks lose their DC to Y2. */
	if (has_y2 && block < TEST_Y_BLOCKS)
		return 1U;
	return 0U;
}

/*
 * Clears spec to a macroblock whose every block is an immediate EOB.
 */
static void
spec_clear(
	struct mb_spec *spec,
	int has_y2)
{
	unsigned block;

	/* No value, and each block ending before its first position. */
	memset(spec, 0, sizeof(*spec));
	for (block = 0U; block < TEST_BLOCKS; block++) {
		spec->last[block] = (int)block_first(has_y2, block) - 1;
	}
}

/*
 * Gives a random coefficient value: mostly small, sometimes of every
 * category, positive or negative; 0 is included.
 */
static int
random_value(void)
{
	int magnitude;

	/* The size class, then a magnitude within it. */
	switch (random_below(6U)) {
	case 0U:
		magnitude = 0;
		break;
	case 1U:
		magnitude = 1;
		break;
	case 2U:
		magnitude = (int)(2U + random_below(3U));
		break;
	case 3U:
		magnitude = (int)(5U + random_below(14U));
		break;
	case 4U:
		magnitude = (int)(19U + random_below(48U));
		break;
	default:
		magnitude = (int)(67U + random_below(2048U));
		break;
	}

	/* A coin chooses the sign. */
	if (magnitude != 0 && random_below(2U) == 0U)
		magnitude = -magnitude;
	return magnitude;
}

/*
 * Fills spec with random blocks: some immediate EOBs, some ending at a
 * random position (on a non-zero value, since a zero cannot be followed by
 * an EOB), some running to position 15, whose last value may be zero.
 */
static void
spec_random(
	struct mb_spec *spec,
	int has_y2)
{
	unsigned block;
	unsigned first;
	unsigned position;
	unsigned span;
	unsigned kind;
	int last;
	int value;

	/* Every block is coded unless it is Y2 of a macroblock without it. */
	spec_clear(spec, has_y2);
	for (block = 0U; block < TEST_BLOCKS; block++) {
		first = block_first(has_y2, block);
		kind = random_below(10U);

		/* An immediate EOB, about three blocks in ten. */
		if (kind < 3U)
			continue;

		/* The last position: anywhere from the first to 15, 15 for one block in five. */
		span = TEST_POSITIONS - first;
		last = (int)(first + random_below(span));
		if (kind >= 8U)
			last = 15;
		spec->last[block] = last;

		/* Values up to the last; zeros are common, and the last is non-zero unless it is 15. */
		for (position = first; position <= (unsigned)last; position++) {
			value = random_value();
			if (random_below(3U) == 0U)
				value = 0;
			spec->values[block][position] = value;
		}
		if (last < 15) {
			value = random_value();
			while (value == 0)
				value = random_value();
			spec->values[block][last] = value;
		}
	}
}

/*
 * Gives the token that codes a magnitude: the value itself up to 4, then
 * the category whose range holds it (RFC 6386 13.2).
 */
static unsigned
token_of(
	unsigned magnitude)
{
	unsigned category;

	/* The values 0 to 4 have their own tokens. */
	if (magnitude < 5U)
		return magnitude;

	/* Above, the category with the largest minimum not above the magnitude. */
	category = 0U;
	while (category < 5U && magnitude >= test_cat_min[category + 1U])
		category++;
	return 5U + category;
}

/*
 * Writes the token of value, then its extra bits and sign, with the
 * probabilities of one band and context; after_zero starts the tree below
 * its EOB branch.
 */
static void
write_token(
	struct boolwriter *out,
	const uint8_t probs[WEBPC_VP8_COEFF_NODES],
	int after_zero,
	int value)
{
	unsigned magnitude;
	unsigned token;
	unsigned category;
	unsigned bits;
	unsigned offset;
	unsigned index;
	unsigned start;
	int failed;

	/* The token from the tree, below the EOB branch after a zero. */
	if (value < 0)
		magnitude = (unsigned)(-value);
	else
		magnitude = (unsigned)value;
	token = token_of(magnitude);
	start = 0U;
	if (after_zero)
		start = 2U;
	failed = boolwriter_tree(out, test_tree, 22U, probs, start, (int)token);
	CHECK(failed == 0);

	/* A category's extra bits, most significant first. */
	if (token >= 5U) {
		category = token - 5U;
		bits = test_cat_bits[category];
		offset = magnitude - test_cat_min[category];
		for (index = 0U; index < bits; index++) {
			boolwriter_put(out, test_cat_probs[category][index], (int)((offset >> (bits - 1U - index)) & 1U));
		}
	}

	/* The sign of a non-zero value, 1 for negative, with probability 128. */
	if (magnitude != 0U)
		boolwriter_put(out, 128U, value < 0 ? 1 : 0);
}

/*
 * Writes one block's tokens from position first through position last, and
 * the EOB after it unless last is 15, given the context the block starts
 * in; the context of each later position is worked out here from the
 * previous value.
 */
static void
write_block(
	struct boolwriter *out,
	const uint8_t probs[WEBPC_VP8_COEFF_BANDS][WEBPC_VP8_COEFF_CONTEXTS][WEBPC_VP8_COEFF_NODES],
	unsigned first,
	unsigned context,
	const int *values,
	int last)
{
	unsigned position;
	int after_zero;
	int failed;
	int value;

	/* One token for each position up to the last. */
	after_zero = 0;
	for (position = first; (int)position <= last; position++) {
		value = values[position];
		write_token(out, probs[test_bands[position]][context], after_zero, value);

		/* The next context is 0 after a zero, 1 after a one, 2 after more. */
		if (value == 0) {
			context = 0U;
			after_zero = 1;
		} else {
			if (value == 1 || value == -1)
				context = 1U;
			else
				context = 2U;
			after_zero = 0;
		}
	}

	/* The EOB at the next position, which can never follow a zero. */
	if (last < 15) {
		CHECK(!after_zero);
		position = (unsigned)(last + 1);
		failed = boolwriter_tree(out, test_tree, 22U, probs[test_bands[position]][context], 0U, 11);
		CHECK(failed == 0);
	}
}

/*
 * Writes one macroblock's blocks in coding order with the contexts of the
 * test's own model (left and above are updated as the decoder should
 * update them).
 */
static void
write_mb(
	struct boolwriter *out,
	const struct webpc_vp8_header *header,
	int has_y2,
	const struct mb_spec *spec,
	uint8_t left[WEBPC_VP8_CONTEXTS],
	uint8_t above[WEBPC_VP8_CONTEXTS])
{
	static const unsigned order[25] = {
		24U, 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U,
		16U, 17U, 18U, 19U, 20U, 21U, 22U, 23U
	};
	unsigned index;
	unsigned block;
	unsigned type;
	unsigned first;
	unsigned context;
	unsigned entry;

	/* Y2 first when there is one, then the others in order. */
	for (index = 0U; index < 25U; index++) {
		block = order[index];
		if (block == 24U && !has_y2)
			continue;

		/* The type of the block's probabilities. */
		if (block == 24U)
			type = 1U;
		else if (block < 16U)
			type = has_y2 ? 0U : 3U;
		else
			type = 2U;

		/* The context is the sum of the left and above entries. */
		first = block_first(has_y2, block);
		context = (unsigned)left[test_left_index[block]] + (unsigned)above[test_above_index[block]];
		write_block(out, header->coeff_probs[type], first, context, spec->values[block], spec->last[block]);

		/* Both entries take 1 when the block went beyond its first position, else 0. */
		entry = 0U;
		if (spec->last[block] >= (int)first)
			entry = 1U;
		left[test_left_index[block]] = (uint8_t)entry;
		above[test_above_index[block]] = (uint8_t)entry;
	}
}

/*
 * Applies a skipped macroblock to the test's model of the contexts: Y, U
 * and V entries cleared, Y2's too only when the macroblock has Y2.
 */
static void
model_skip(
	int has_y2,
	uint8_t left[WEBPC_VP8_CONTEXTS],
	uint8_t above[WEBPC_VP8_CONTEXTS])
{
	unsigned index;

	/* Entries 0 to 7 are the Y, U and V blocks'. */
	for (index = 0U; index < 8U; index++) {
		left[index] = 0U;
		above[index] = 0U;
	}

	/* Entry 8 is Y2's, left alone when the macroblock has no Y2. */
	if (has_y2) {
		left[8] = 0U;
		above[8] = 0U;
	}
}

/*
 * Gives value times factor wrapped to the 16-bit range, worked out in 64
 * bits and by the plain rule (the value modulo 65536, into -32768..32767).
 */
static int32_t
wrap_product(
	int value,
	int32_t factor)
{
	int64_t product;
	int64_t low;

	/* The product's residue modulo 65536 is never negative. */
	product = (int64_t)value * (int64_t)factor;
	low = product % 65536;
	if (low < 0)
		low += 65536;

	/* The upper half of the range is the negative numbers. */
	if (low >= 32768)
		low -= 65536;
	return (int32_t)low;
}

/*
 * Works out the residue the reader should give: each coded position's value
 * times its factor at its raster place, and the coded bits.
 */
static void
expect_residue(
	struct webpc_vp8_residue *want,
	int has_y2,
	const struct mb_spec *spec,
	const struct webpc_vp8_dequant *dequant)
{
	unsigned block;
	unsigned first;
	unsigned position;
	const int32_t *factors;

	/* Nothing coded to begin with. */
	memset(want, 0, sizeof(*want));
	for (block = 0U; block < TEST_BLOCKS; block++) {
		if (block == 24U && !has_y2)
			continue;

		/* The factors of the block's plane. */
		if (block == 24U)
			factors = dequant->y2;
		else if (block < 16U)
			factors = dequant->y1;
		else
			factors = dequant->uv;

		/* Each position the block coded, DC factor for position 0 and AC for the rest. */
		first = block_first(has_y2, block);
		for (position = first; (int)position <= spec->last[block]; position++) {
			want->coeffs[block][test_zigzag[position]] =
			    (int16_t)wrap_product(spec->values[block][position], factors[position == 0U ? 0U : 1U]);
		}

		/* The block's bit, and the whole macroblock's bit 31, when it went beyond its first position. */
		if (spec->last[block] >= (int)first) {
			want->coded |= (uint32_t)1U << block;
			want->coded |= (uint32_t)1U << 31;
		}
	}
}

/*
 * Checks a residue against the expected one, block by block.
 */
static void
check_residue(
	const struct webpc_vp8_residue *got,
	const struct webpc_vp8_residue *want)
{
	unsigned block;

	/* One check for each block's 16 coefficients. */
	for (block = 0U; block < TEST_BLOCKS; block++) {
		CHECK(memcmp(got->coeffs[block], want->coeffs[block], sizeof(got->coeffs[block])) == 0);
	}

	/* The coded bits. */
	CHECK(got->coded == want->coded);
}

/*
 * Checks the nine context entries against the expected ones.
 */
static void
check_contexts(
	const uint8_t got[WEBPC_VP8_CONTEXTS],
	const uint8_t want[WEBPC_VP8_CONTEXTS])
{
	/* All nine at once. */
	CHECK(memcmp(got, want, WEBPC_VP8_CONTEXTS) == 0);
}

/*
 * Writes a frame of rows x columns macroblocks (random, or already placed in
 * chain_specs by the caller when skip_percent is above 100) into one
 * partition, then reads them back one at a time carrying the contexts as a
 * decoder does: above per column from the frame's start, left from each
 * row's start.  random_contexts starts those contexts from random 0 and 1
 * instead of 0.
 */
static void
run_chain(
	const struct webpc_vp8_header *header,
	const struct webpc_vp8_dequant *dequant,
	unsigned rows,
	unsigned columns,
	int random_contexts,
	unsigned skip_percent)
{
	static uint8_t above_model[CHAIN_COLUMNS][WEBPC_VP8_CONTEXTS];
	static uint8_t above_read[CHAIN_COLUMNS][WEBPC_VP8_CONTEXTS];
	static uint8_t left_model[WEBPC_VP8_CONTEXTS];
	static uint8_t left_read[WEBPC_VP8_CONTEXTS];
	static uint8_t left_initial[CHAIN_ROWS][WEBPC_VP8_CONTEXTS];
	struct webpc_bool tokens;
	struct webpc_vp8_residue want;
	struct webpc_vp8_residue got;
	size_t size;
	unsigned row;
	unsigned column;
	unsigned entry;
	unsigned count;

	/* The starting contexts: the same for the writer's model and the reader. */
	for (column = 0U; column < columns; column++) {
		for (entry = 0U; entry < WEBPC_VP8_CONTEXTS; entry++) {
			above_model[column][entry] = 0U;
			if (random_contexts)
				above_model[column][entry] = (uint8_t)random_below(2U);
			above_read[column][entry] = above_model[column][entry];
		}
	}
	for (row = 0U; row < rows; row++) {
		for (entry = 0U; entry < WEBPC_VP8_CONTEXTS; entry++) {
			left_initial[row][entry] = 0U;
			if (random_contexts)
				left_initial[row][entry] = (uint8_t)random_below(2U);
		}
	}

	/* Chooses what each macroblock codes, and whether it is skipped. */
	for (count = 0U; count < rows * columns; count++) {
		chain_has_y2[count] = (int)random_below(2U);
		chain_skip[count] = 0;
		if (random_below(100U) < skip_percent)
			chain_skip[count] = 1;
		spec_random(&chain_specs[count], chain_has_y2[count]);
	}

	/* Writes the partition: each macroblock after the one before it, the left contexts restarting at each row. */
	boolwriter_init(&writer);
	for (row = 0U; row < rows; row++) {
		memcpy(left_model, left_initial[row], WEBPC_VP8_CONTEXTS);
		for (column = 0U; column < columns; column++) {
			count = row * columns + column;
			if (chain_skip[count])
				model_skip(chain_has_y2[count], left_model, above_model[column]);
			else
				write_mb(&writer, header, chain_has_y2[count], &chain_specs[count], left_model, above_model[column]);
		}
	}
	size = boolwriter_finish(&writer);
	CHECK(writer.overflow == 0);

	/* Reads it back with the library, checking the residue and both contexts after each macroblock. */
	webpc_bool_init(&tokens, writer.bytes, size);
	for (column = 0U; column < columns; column++) {
		for (entry = 0U; entry < WEBPC_VP8_CONTEXTS; entry++)
			above_model[column][entry] = above_read[column][entry];
	}
	for (row = 0U; row < rows; row++) {
		memcpy(left_read, left_initial[row], WEBPC_VP8_CONTEXTS);
		memcpy(left_model, left_initial[row], WEBPC_VP8_CONTEXTS);
		for (column = 0U; column < columns; column++) {
			count = row * columns + column;
			if (chain_skip[count]) {
				webpc_vp8_skip_residue(chain_has_y2[count], left_read, above_read[column]);
				model_skip(chain_has_y2[count], left_model, above_model[column]);
			} else {
				memset(&got, 0x5a, sizeof(got));
				webpc_vp8_read_residue(&tokens, header, dequant, chain_has_y2[count], left_read, above_read[column], &got);
				expect_residue(&want, chain_has_y2[count], &chain_specs[count], dequant);
				check_residue(&got, &want);
				model_skip(0, left_model, above_model[column]);
				for (entry = 0U; entry < TEST_BLOCKS; entry++) {
					if (entry == 24U && !chain_has_y2[count])
						continue;
					left_model[test_left_index[entry]] = (uint8_t)(chain_specs[count].last[entry] >= (int)block_first(chain_has_y2[count], entry));
					above_model[column][test_above_index[entry]] = left_model[test_left_index[entry]];
				}
			}
			check_contexts(left_read, left_model);
			check_contexts(above_read[column], above_model[column]);
		}
	}

	/* A valid partition is never read past its end. */
	CHECK(webpc_bool_ended(&tokens) == 0);
}

/*
 * Writes one macroblock from spec with contexts of 0 and reads it back.
 */
static void
run_single(
	const struct webpc_vp8_header *header,
	const struct webpc_vp8_dequant *dequant,
	int has_y2,
	const struct mb_spec *spec)
{
	uint8_t left_model[WEBPC_VP8_CONTEXTS];
	uint8_t above_model[WEBPC_VP8_CONTEXTS];
	uint8_t left_read[WEBPC_VP8_CONTEXTS];
	uint8_t above_read[WEBPC_VP8_CONTEXTS];
	struct webpc_bool tokens;
	struct webpc_vp8_residue want;
	struct webpc_vp8_residue got;
	size_t size;
	unsigned entry;

	/* The writer's model starts clean. */
	memset(left_model, 0, sizeof(left_model));
	memset(above_model, 0, sizeof(above_model));
	memset(left_read, 0, sizeof(left_read));
	memset(above_read, 0, sizeof(above_read));

	/* Writes the macroblock. */
	boolwriter_init(&writer);
	write_mb(&writer, header, has_y2, spec, left_model, above_model);
	size = boolwriter_finish(&writer);
	CHECK(writer.overflow == 0);

	/* Reads it back and compares the residue and the contexts. */
	webpc_bool_init(&tokens, writer.bytes, size);
	memset(&got, 0x5a, sizeof(got));
	webpc_vp8_read_residue(&tokens, header, dequant, has_y2, left_read, above_read, &got);
	expect_residue(&want, has_y2, spec, dequant);
	check_residue(&got, &want);
	for (entry = 0U; entry < WEBPC_VP8_CONTEXTS; entry++) {
		CHECK(left_read[entry] == left_model[entry]);
		CHECK(above_read[entry] == above_model[entry]);
	}
	CHECK(webpc_bool_ended(&tokens) == 0);
}

/*
 * Every token at the first position of a Y block, a U block's last
 * position and (with Y2) Y2 and a Y block's first position, with the
 * default probabilities and both signs.
 */
static void
test_tokens_default(void)
{
	static const int magnitudes[] = { 1, 2, 3, 4, 5, 6, 7, 10, 11, 18, 19, 34, 35, 66, 67, 2114 };
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant dequant;
	struct mb_spec spec;
	unsigned index;
	unsigned sign;
	int value;

	/* Default probabilities and small distinct factors. */
	default_header(&header);
	plain_dequant(&dequant);

	/* Each magnitude, positive and negative, without Y2 and with it. */
	for (index = 0U; index < sizeof(magnitudes) / sizeof(magnitudes[0]); index++) {
		for (sign = 0U; sign < 2U; sign++) {
			value = magnitudes[index];
			if (sign)
				value = -value;

			/* Without Y2: block 0 at position 0 (type 3), block 17 at position 15 after fifteen zeros. */
			spec_clear(&spec, 0);
			spec.values[0][0] = value;
			spec.last[0] = 0;
			spec.values[17][15] = value;
			spec.last[17] = 15;
			spec.values[9][3] = value;
			spec.last[9] = 3;
			run_single(&header, &dequant, 0, &spec);

			/* With Y2: Y2 at position 0, block 5 at its first position (1), a V block at position 15. */
			spec_clear(&spec, 1);
			spec.values[24][0] = value;
			spec.last[24] = 0;
			spec.values[5][1] = value;
			spec.last[5] = 1;
			spec.values[21][15] = value;
			spec.last[21] = 15;
			run_single(&header, &dequant, 1, &spec);
		}
	}
}

/*
 * Every value from 0 to 2114 as the only value of a block at its first
 * position, then at a middle position, with random probabilities: the
 * category boundaries and every extra bit are read.
 */
static void
test_ranges(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant dequant;
	struct mb_spec spec;
	int magnitude;

	/* Random probabilities and the plain factors. */
	random_probs_header(&header);
	plain_dequant(&dequant);

	/* Each magnitude at position 0 of block 2 and position 6 of a chroma block, with the sign alternating. */
	for (magnitude = 1; magnitude <= 2114; magnitude++) {
		spec_clear(&spec, 0);
		spec.values[2][0] = (magnitude & 1) ? magnitude : -magnitude;
		spec.last[2] = 0;
		spec.values[22][6] = (magnitude & 1) ? -magnitude : magnitude;
		spec.last[22] = 6;
		run_single(&header, &dequant, 0, &spec);
	}
}

/*
 * The positions and blocks the format treats specially: immediate EOBs,
 * blocks of zeros to the end, the DC of Y blocks under Y2, zeros before a
 * value, a value at each position.
 */
static void
test_special_positions(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant dequant;
	struct mb_spec spec;
	unsigned position;
	unsigned has_y2;
	unsigned block;

	/* Random probabilities and the plain factors. */
	random_probs_header(&header);
	plain_dequant(&dequant);

	/* Every block an immediate EOB: nothing coded, both contexts 0. */
	for (has_y2 = 0U; has_y2 < 2U; has_y2++) {
		spec_clear(&spec, (int)has_y2);
		run_single(&header, &dequant, (int)has_y2, &spec);
	}

	/* Every block a run of zeros through position 15, with no EOB: coded, but all coefficients 0. */
	for (has_y2 = 0U; has_y2 < 2U; has_y2++) {
		spec_clear(&spec, (int)has_y2);
		for (block = 0U; block < TEST_BLOCKS; block++)
			spec.last[block] = 15;
		run_single(&header, &dequant, (int)has_y2, &spec);
	}

	/* A single value of -1 at each position, in a luma block, a chroma block and Y2. */
	for (has_y2 = 0U; has_y2 < 2U; has_y2++) {
		for (position = 0U; position < TEST_POSITIONS; position++) {
			spec_clear(&spec, (int)has_y2);
			spec.values[4][position] = -1;
			spec.last[4] = (int)position;
			spec.values[18][position] = 1;
			spec.last[18] = (int)position;
			spec.values[24][position] = 2;
			spec.last[24] = (int)position;
			if (has_y2 && position == 0U) {
				/* A luma block cannot code position 0 under Y2, so it stays an immediate EOB. */
				spec.last[4] = 0;
				spec.values[4][0] = 0;
			}
			run_single(&header, &dequant, (int)has_y2, &spec);
		}
	}

	/* Zeros to position 14 and a value at 15, then zeros to 15 after a value: the context of each follows. */
	spec_clear(&spec, 0);
	spec.values[0][15] = 3;
	spec.last[0] = 15;
	spec.values[1][0] = 9;
	spec.values[1][1] = 1;
	spec.values[1][2] = 0;
	spec.values[1][3] = 0;
	spec.values[1][4] = 5;
	spec.last[1] = 15;
	run_single(&header, &dequant, 0, &spec);
}

/*
 * The dequantization: each plane's DC and AC factors, negative values, and
 * products that leave 16 bits (the wrap) or even 32 bits (a hostile
 * factor).
 */
static void
test_dequant(void)
{
	static const int32_t factors[] = { 1, 4, 157, 284, 440, 1000, 32768, 65535, 65536, 65537, 0x7fffffff, -0x7fffffff - 1, -1, 0 };
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant dequant;
	struct mb_spec spec;
	unsigned index;
	unsigned has_y2;
	unsigned block;
	unsigned position;

	/* Random probabilities, then each factor in turn for all six entries. */
	random_probs_header(&header);
	for (index = 0U; index < sizeof(factors) / sizeof(factors[0]); index++) {
		dequant.y1[0] = factors[index];
		dequant.y1[1] = factors[(index + 1U) % (sizeof(factors) / sizeof(factors[0]))];
		dequant.y2[0] = factors[(index + 2U) % (sizeof(factors) / sizeof(factors[0]))];
		dequant.y2[1] = factors[(index + 3U) % (sizeof(factors) / sizeof(factors[0]))];
		dequant.uv[0] = factors[(index + 4U) % (sizeof(factors) / sizeof(factors[0]))];
		dequant.uv[1] = factors[(index + 5U) % (sizeof(factors) / sizeof(factors[0]))];

		/* The largest values at DC and AC positions of every plane. */
		for (has_y2 = 0U; has_y2 < 2U; has_y2++) {
			spec_clear(&spec, (int)has_y2);
			for (block = 0U; block < TEST_BLOCKS; block++) {
				if (block == 24U && !has_y2)
					continue;
				position = block_first((int)has_y2, block);
				spec.values[block][position] = (block & 1U) ? 2114 : -2114;
				spec.values[block][position + 1U] = (block & 2U) ? 1999 : -1999;
				spec.last[block] = (int)position + 1;
			}
			run_single(&header, &dequant, (int)has_y2, &spec);
		}
	}
}

/*
 * One wrap worked out by hand: 2114 times 440 is 930160, and 930160 less
 * 14 times 65536 is 12656, which the reader must store; the negative of it
 * is -12656.
 */
static void
test_known_wrap(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant dequant;
	struct webpc_bool tokens;
	struct webpc_vp8_residue got;
	struct mb_spec spec;
	uint8_t left[WEBPC_VP8_CONTEXTS];
	uint8_t above[WEBPC_VP8_CONTEXTS];
	size_t size;

	/* Y2 with 2114 at position 1 (an AC position) and -2114 at position 2, the AC factor 440. */
	default_header(&header);
	plain_dequant(&dequant);
	dequant.y2[1] = 440;
	spec_clear(&spec, 1);
	spec.values[24][1] = 2114;
	spec.values[24][2] = -2114;
	spec.last[24] = 2;

	/* Writes it with clean contexts. */
	memset(left, 0, sizeof(left));
	memset(above, 0, sizeof(above));
	boolwriter_init(&writer);
	write_mb(&writer, &header, 1, &spec, left, above);
	size = boolwriter_finish(&writer);

	/* Reads it back: positions 1 and 2 are raster places 1 and 4. */
	memset(left, 0, sizeof(left));
	memset(above, 0, sizeof(above));
	webpc_bool_init(&tokens, writer.bytes, size);
	webpc_vp8_read_residue(&tokens, &header, &dequant, 1, left, above, &got);
	CHECK(got.coeffs[24][1] == 12656);
	CHECK(got.coeffs[24][4] == -12656);
}

/*
 * The contexts: blocks coded after neighbours that went beyond their first
 * position use the sum of the left and above entries, which only a run
 * from random starting contexts exercises across all 9 entries.
 */
static void
test_contexts(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant dequant;
	unsigned round;

	/* Single macroblocks from random starting contexts, with random probabilities. */
	plain_dequant(&dequant);
	for (round = 0U; round < 60U; round++) {
		random_probs_header(&header);
		run_chain(&header, &dequant, 1U, 1U, 1, 0U);
	}
}

/*
 * Chains of macroblocks, 3 across and 2 down, with the contexts carried
 * from one to the next, some skipped, with random and default
 * probabilities and random factors.
 */
static void
test_chains(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant dequant;
	unsigned round;

	/* Random probabilities, random factors, and a skip chance that grows. */
	for (round = 0U; round < 60U; round++) {
		random_probs_header(&header);
		random_dequant(&dequant);
		run_chain(&header, &dequant, CHAIN_ROWS, CHAIN_COLUMNS, 0, (round % 4U) * 25U);
	}

	/* The default probabilities, plain factors, random starting contexts. */
	default_header(&header);
	plain_dequant(&dequant);
	for (round = 0U; round < 20U; round++)
		run_chain(&header, &dequant, CHAIN_ROWS, CHAIN_COLUMNS, 1, 30U);
}

/*
 * webpc_vp8_skip_residue: the Y, U and V entries become 0 whatever they
 * held, Y2's only when the macroblock has Y2.
 */
static void
test_skip_residue(void)
{
	uint8_t left[WEBPC_VP8_CONTEXTS];
	uint8_t above[WEBPC_VP8_CONTEXTS];
	unsigned has_y2;
	unsigned entry;

	/* Each case starts with every entry non-zero (1 on the left, 7 above). */
	for (has_y2 = 0U; has_y2 < 2U; has_y2++) {
		for (entry = 0U; entry < WEBPC_VP8_CONTEXTS; entry++) {
			left[entry] = 1U;
			above[entry] = 7U;
		}
		webpc_vp8_skip_residue((int)has_y2, left, above);

		/* Entries 0 to 7 are cleared both ways. */
		for (entry = 0U; entry < 8U; entry++) {
			CHECK(left[entry] == 0U);
			CHECK(above[entry] == 0U);
		}

		/* Entry 8 is cleared with Y2 and kept without. */
		if (has_y2) {
			CHECK(left[8] == 0U);
			CHECK(above[8] == 0U);
		} else {
			CHECK(left[8] == 1U);
			CHECK(above[8] == 7U);
		}
	}
}

/*
 * Context entries that no block gives (any non-zero byte) count as 1 and
 * leave the same result as a 1 does; the entries are rewritten with 0 or 1,
 * and Y2's stays as it was when the macroblock has no Y2.
 */
static void
test_bad_contexts(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant dequant;
	struct webpc_bool tokens_bad;
	struct webpc_bool tokens_good;
	struct webpc_vp8_residue got_bad;
	struct webpc_vp8_residue got_good;
	struct mb_spec spec;
	uint8_t left_bad[WEBPC_VP8_CONTEXTS];
	uint8_t above_bad[WEBPC_VP8_CONTEXTS];
	uint8_t left_good[WEBPC_VP8_CONTEXTS];
	uint8_t above_good[WEBPC_VP8_CONTEXTS];
	uint8_t left_start[WEBPC_VP8_CONTEXTS];
	uint8_t above_start[WEBPC_VP8_CONTEXTS];
	size_t size;
	unsigned round;
	unsigned entry;
	unsigned has_y2;

	/* Random macroblocks, random starting entries from the whole byte range. */
	plain_dequant(&dequant);
	for (round = 0U; round < 40U; round++) {
		has_y2 = random_below(2U);
		random_probs_header(&header);
		spec_random(&spec, (int)has_y2);

		/* A partition that any contexts can read (the bytes are random, the decoding is the point). */
		boolwriter_init(&writer);
		for (entry = 0U; entry < 300U; entry++)
			boolwriter_put(&writer, 1U + random_below(255U), (int)random_below(2U));
		size = boolwriter_finish(&writer);

		/* The same entries, once as given and once with every non-zero one made 1. */
		for (entry = 0U; entry < WEBPC_VP8_CONTEXTS; entry++) {
			left_start[entry] = (uint8_t)random_below(256U);
			above_start[entry] = (uint8_t)random_below(256U);
			left_bad[entry] = left_start[entry];
			above_bad[entry] = above_start[entry];
			left_good[entry] = left_start[entry] != 0U ? 1U : 0U;
			above_good[entry] = above_start[entry] != 0U ? 1U : 0U;
		}
		webpc_bool_init(&tokens_bad, writer.bytes, size);
		webpc_bool_init(&tokens_good, writer.bytes, size);
		webpc_vp8_read_residue(&tokens_bad, &header, &dequant, (int)has_y2, left_bad, above_bad, &got_bad);
		webpc_vp8_read_residue(&tokens_good, &header, &dequant, (int)has_y2, left_good, above_good, &got_good);

		/* The same coefficients, coded bits and entries 0 to 7. */
		check_residue(&got_bad, &got_good);
		CHECK(memcmp(left_bad, left_good, 8U) == 0);
		CHECK(memcmp(above_bad, above_good, 8U) == 0);

		/* Entry 8 is rewritten with Y2, and left as it was without. */
		if (has_y2) {
			CHECK(left_bad[8] == left_good[8]);
			CHECK(above_bad[8] == above_good[8]);
		} else {
			CHECK(left_bad[8] == left_start[8]);
			CHECK(above_bad[8] == above_start[8]);
		}
	}
}

/*
 * Partitions that end too early: an empty one reads as ended; one cut in
 * half does too (and neither crashes); the whole one does not.
 */
static void
test_ended(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant dequant;
	struct webpc_bool tokens;
	struct webpc_vp8_residue got;
	struct webpc_vp8_residue want;
	struct mb_spec spec;
	uint8_t left[WEBPC_VP8_CONTEXTS];
	uint8_t above[WEBPC_VP8_CONTEXTS];
	size_t size;
	unsigned block;
	unsigned position;

	/* A macroblock full of large values, so the partition is long. */
	random_probs_header(&header);
	plain_dequant(&dequant);
	spec_clear(&spec, 1);
	for (block = 0U; block < TEST_BLOCKS; block++) {
		spec.last[block] = 15;
		for (position = block_first(1, block); position < TEST_POSITIONS; position++)
			spec.values[block][position] = 100 + (int)position * 100;
	}
	memset(left, 0, sizeof(left));
	memset(above, 0, sizeof(above));
	boolwriter_init(&writer);
	write_mb(&writer, &header, 1, &spec, left, above);
	size = boolwriter_finish(&writer);
	CHECK(size > 200U);

	/* The whole partition reads correctly and is not ended. */
	memset(left, 0, sizeof(left));
	memset(above, 0, sizeof(above));
	webpc_bool_init(&tokens, writer.bytes, size);
	webpc_vp8_read_residue(&tokens, &header, &dequant, 1, left, above, &got);
	expect_residue(&want, 1, &spec, &dequant);
	check_residue(&got, &want);
	CHECK(webpc_bool_ended(&tokens) == 0);

	/* The empty partition is ended; it decodes zeros without reading anything. */
	memset(left, 0, sizeof(left));
	memset(above, 0, sizeof(above));
	webpc_bool_init(&tokens, NULL, 0U);
	webpc_vp8_read_residue(&tokens, &header, &dequant, 1, left, above, &got);
	CHECK(webpc_bool_ended(&tokens) == 1);

	/* Cut to half, and to a quarter: ended. */
	memset(left, 0, sizeof(left));
	memset(above, 0, sizeof(above));
	webpc_bool_init(&tokens, writer.bytes, size / 2U);
	webpc_vp8_read_residue(&tokens, &header, &dequant, 1, left, above, &got);
	CHECK(webpc_bool_ended(&tokens) == 1);
	memset(left, 0, sizeof(left));
	memset(above, 0, sizeof(above));
	webpc_bool_init(&tokens, writer.bytes, size / 4U);
	webpc_vp8_read_residue(&tokens, &header, &dequant, 1, left, above, &got);
	CHECK(webpc_bool_ended(&tokens) == 1);
}

/*
 * Random partitions, random probabilities (0 included), random factors
 * (extreme ones included): the reader never fails outright; the entries
 * stay 0 or 1; the coded bits stay inside bits 0 to 24 and 31 and bit 31 is
 * set exactly when some block's is.
 */
static void
test_random_bytes(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant dequant;
	struct webpc_bool tokens;
	struct webpc_vp8_residue got;
	uint8_t data[400];
	uint8_t left[WEBPC_VP8_CONTEXTS];
	uint8_t above[WEBPC_VP8_CONTEXTS];
	size_t size;
	unsigned round;
	unsigned entry;
	unsigned has_y2;
	unsigned type;
	unsigned band;
	unsigned context;
	unsigned node;
	uint32_t low_bits;

	/* Each round: random bytes, random probabilities, factors, contexts. */
	for (round = 0U; round < 300U; round++) {
		size = random_below(401U);
		for (entry = 0U; entry < (unsigned)size; entry++)
			data[entry] = (uint8_t)random_below(256U);
		memset(&header, 0, sizeof(header));
		for (type = 0U; type < WEBPC_VP8_COEFF_TYPES; type++)
			for (band = 0U; band < WEBPC_VP8_COEFF_BANDS; band++)
				for (context = 0U; context < WEBPC_VP8_COEFF_CONTEXTS; context++)
					for (node = 0U; node < WEBPC_VP8_COEFF_NODES; node++)
						header.coeff_probs[type][band][context][node] = (uint8_t)random_below(256U);
		random_dequant(&dequant);
		has_y2 = random_below(2U);
		for (entry = 0U; entry < WEBPC_VP8_CONTEXTS; entry++) {
			left[entry] = (uint8_t)random_below(2U);
			above[entry] = (uint8_t)random_below(2U);
		}

		/* Reads it, with a null pointer only when the size is 0. */
		webpc_bool_init(&tokens, size == 0U ? NULL : data, size);
		memset(&got, 0xa5, sizeof(got));
		webpc_vp8_read_residue(&tokens, &header, &dequant, (int)has_y2, left, above, &got);

		/* The entries are 0 or 1. */
		for (entry = 0U; entry < WEBPC_VP8_CONTEXTS; entry++) {
			CHECK(left[entry] <= 1U);
			CHECK(above[entry] <= 1U);
		}

		/* The coded bits: nothing in bits 25 to 30, and bit 31 when any other bit is set. */
		low_bits = got.coded & 0x01ffffffUL;
		CHECK((got.coded & 0x7e000000UL) == 0UL);
		CHECK(((got.coded >> 31) != 0U) == (low_bits != 0U));
		if (!has_y2)
			CHECK((got.coded & 0x01000000UL) == 0UL);

		/* Under Y2 the Y blocks have no DC. */
		if (has_y2) {
			for (entry = 0U; entry < TEST_Y_BLOCKS; entry++)
				CHECK(got.coeffs[entry][0] == 0);
		}
	}
}
