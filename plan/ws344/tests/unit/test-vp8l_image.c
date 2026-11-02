/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8l-image.c: the length and distance values of Table 4, the
 * distance map of Figure 20, the colour cache's hash, and whole entropy-coded
 * images written with the lbitwriter from RFC 9649 3.6 and 3.7 (literals,
 * backward references, cache indices, meta prefix codes), with the pixels the
 * test works out for itself while it writes them.
 */

#include "check.h"
#include "lbitwriter.h"
#include "vp8l-image.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The marker the tests write after an image, to show the reader used exactly the image's bits. */
#define TEST_MARKER		0xa5U

/* The most tokens (literals, copies and cache indices) of one test image, and the most pixels. */
#define TEST_TOKENS_MAX		8192U
#define TEST_PIXELS_MAX		16384U

/* The most groups of one test image. */
#define TEST_GROUPS_MAX		8U

/* A table size that never limits the images the tests decode. */
#define TEST_LIMIT		((size_t)1 << 24)

/* The kinds of token a test image is made of. */
#define TOKEN_LITERAL		0
#define TOKEN_COPY		1
#define TOKEN_CACHE		2

/*
 * One thing a test image says: a literal pixel, a backward reference (length
 * pixels at the distance code dcode) or a colour cache index.
 */
struct test_token {
	int kind;
	uint32_t argb;
	uint32_t length;
	uint32_t dcode;
	unsigned index;
};

/*
 * A test image as the test describes it: its size and role, its colour
 * cache, its meta prefix codes (prefix_bits 0 for none; else blocks gives the
 * group of each block), its tokens, and how many symbols an unused group's
 * codes hold (so the unused groups can be made as large as the used ones).
 */
struct test_image {
	unsigned width;
	unsigned height;
	int role;
	unsigned cache_bits;
	unsigned prefix_bits;
	const uint32_t *blocks;
	unsigned group_count;
	unsigned unused_symbols;
	const struct test_token *tokens;
	unsigned token_count;
};

/*
 * A prefix code as the test sees it: the length of each symbol, its codeword
 * (assigned as RFC 1951 3.2.2 assigns them), and how many symbols are in it.
 */
struct test_code {
	uint8_t lengths[WEBPC_PREFIX_ALPHABET_MAX];
	uint32_t words[WEBPC_PREFIX_ALPHABET_MAX];
	unsigned alphabet;
	unsigned present;
};

/* The stream a test writes an image into, and the tokens of the image. */
static struct lbitwriter stream;
static struct test_token tokens[TEST_TOKENS_MAX];

/* The pixels the test works out, and the pixels the decoder gives. */
static uint32_t expected_pixels[TEST_PIXELS_MAX];

/* The tokens and image description of the entropy image a test image has inside. */
static struct test_token entropy_tokens[TEST_PIXELS_MAX];

/* Which symbols each code of each group uses, and the codes built from them. */
static uint8_t used_symbols[TEST_GROUPS_MAX][WEBPC_VP8L_GROUP_CODES][WEBPC_PREFIX_ALPHABET_MAX];
static struct test_code group_codes[TEST_GROUPS_MAX][WEBPC_VP8L_GROUP_CODES];

/* The order the code length code's lengths are written in (RFC 9649 3.7.2.1.2). */
static const uint8_t test_order[19] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

/*
 * The neighbouring pixel offsets (xi, yi) of distance codes 1 to 120.
 *
 * Values referenced: RFC 9649 (WebP Image Format, 2024-11) 3.6.2.2.1 Figure
 * 20 (Distance Code to Neighboring Pixel Offset Mapping).
 */
static const int8_t test_distance_map[120][2] = {
	{ 0, 1 }, { 1, 0 }, { 1, 1 }, { -1, 1 }, { 0, 2 }, { 2, 0 },
	{ 1, 2 }, { -1, 2 }, { 2, 1 }, { -2, 1 }, { 2, 2 }, { -2, 2 },
	{ 0, 3 }, { 3, 0 }, { 1, 3 }, { -1, 3 }, { 3, 1 }, { -3, 1 },
	{ 2, 3 }, { -2, 3 }, { 3, 2 }, { -3, 2 }, { 0, 4 }, { 4, 0 },
	{ 1, 4 }, { -1, 4 }, { 4, 1 }, { -4, 1 }, { 3, 3 }, { -3, 3 },
	{ 2, 4 }, { -2, 4 }, { 4, 2 }, { -4, 2 }, { 0, 5 }, { 3, 4 },
	{ -3, 4 }, { 4, 3 }, { -4, 3 }, { 5, 0 }, { 1, 5 }, { -1, 5 },
	{ 5, 1 }, { -5, 1 }, { 2, 5 }, { -2, 5 }, { 5, 2 }, { -5, 2 },
	{ 4, 4 }, { -4, 4 }, { 3, 5 }, { -3, 5 }, { 5, 3 }, { -5, 3 },
	{ 0, 6 }, { 6, 0 }, { 1, 6 }, { -1, 6 }, { 6, 1 }, { -6, 1 },
	{ 2, 6 }, { -2, 6 }, { 6, 2 }, { -6, 2 }, { 4, 5 }, { -4, 5 },
	{ 5, 4 }, { -5, 4 }, { 3, 6 }, { -3, 6 }, { 6, 3 }, { -6, 3 },
	{ 0, 7 }, { 7, 0 }, { 1, 7 }, { -1, 7 }, { 5, 5 }, { -5, 5 },
	{ 7, 1 }, { -7, 1 }, { 4, 6 }, { -4, 6 }, { 6, 4 }, { -6, 4 },
	{ 2, 7 }, { -2, 7 }, { 7, 2 }, { -7, 2 }, { 3, 7 }, { -3, 7 },
	{ 7, 3 }, { -7, 3 }, { 5, 6 }, { -5, 6 }, { 6, 5 }, { -6, 5 },
	{ 8, 0 }, { 4, 7 }, { -4, 7 }, { 7, 4 }, { -7, 4 }, { 8, 1 },
	{ 8, 2 }, { 6, 6 }, { -6, 6 }, { 8, 3 }, { 5, 7 }, { -5, 7 },
	{ 7, 5 }, { -7, 5 }, { 8, 4 }, { 6, 7 }, { -6, 7 }, { 7, 6 },
	{ -7, 6 }, { 8, 5 }, { 7, 7 }, { -7, 7 }, { 8, 6 }, { 8, 7 },
};

/* The state of the generator of random images and streams. */
static unsigned long test_state = 0x2545f491UL;

static void test_lz77_value(void);
static void test_plane_distance(void);
static void test_distance_table_shape(void);
static void test_cache_index(void);
static void test_literals(void);
static void test_cache(void);
static void test_cache_updates(void);
static void test_copies(void);
static void test_copy_errors(void);
static void test_meta(void);
static void test_role_zero(void);
static void test_cache_bits_errors(void);
static void test_size_errors(void);
static void test_limit(void);
static void test_truncated(void);
static void test_random(void);
static unsigned ref_prefix_code(uint32_t value, unsigned *extra_bits, uint32_t *extra);
static unsigned ref_hash(uint32_t argb, unsigned bits);
static size_t ref_distance(uint32_t code, unsigned width);
static void flat_lengths(uint8_t *lengths, unsigned alphabet, const uint8_t *used);
static void assign_words(struct test_code *code);
static void code_define(struct test_code *code, unsigned alphabet, const uint8_t *used);
static void code_put_symbol(struct lbitwriter *writer, const struct test_code *code, unsigned symbol);
static void code_put_normal(struct lbitwriter *writer, const struct test_code *code);
static void code_put_simple(struct lbitwriter *writer, const struct test_code *code);
static void code_put(struct lbitwriter *writer, const struct test_code *code, int allow_simple);
static unsigned token_group(const struct test_image *image, size_t position);
static void build_image(struct lbitwriter *writer, const struct test_image *image, uint32_t *expected, int *valid);
static int decode_stream(const uint8_t *bytes, size_t size, unsigned width, unsigned height, int role, size_t limit,
	uint32_t *pixels, int *marker_ok);
static void check_image(const struct test_image *image);
static unsigned gen_tokens(unsigned width, unsigned height, unsigned cache_bits, unsigned colours);
static uint32_t random_colour(void);
static size_t min_limit(const uint8_t *bytes, size_t size, const struct test_image *image);

/*
 * Runs the tests of vp8l-image.c.
 */
int
main(
	void)
{
	int status;

	/* The three small functions against the spec's tables and hand-worked values. */
	test_lz77_value();
	test_plane_distance();
	test_distance_table_shape();
	test_cache_index();

	/* Whole images. */
	test_literals();
	test_cache();
	test_cache_updates();
	test_copies();
	test_copy_errors();
	test_meta();
	test_role_zero();
	test_cache_bits_errors();
	test_size_errors();
	test_limit();
	test_truncated();
	test_random();

	/* The summary line. */
	status = check_finish("vp8l_image");
	return status;
}

/*
 * Gives the prefix code of a length or distance value by inverting RFC 9649
 * Table 4 (codes 0 to 3 are the values 1 to 4; above, the code 2 * m + s of
 * the value's top bit m and the bit under it s, with m - 1 extra bits), and
 * the extra bits.
 */
static unsigned
ref_prefix_code(
	uint32_t value,
	unsigned *extra_bits,
	uint32_t *extra)
{
	uint32_t offset;
	unsigned top;
	unsigned second;

	/* The first four values have codes of their own and no extra bits. */
	if (value <= 4U) {
		*extra_bits = 0;
		*extra = 0;
		return (unsigned)(value - 1U);
	}

	/* Finds the top bit of value - 1, and the bit under it. */
	offset = value - 1U;
	top = 0;
	while ((offset >> (top + 1U)) != 0U)
		top++;
	second = (unsigned)((offset >> (top - 1U)) & 1U);

	/* The extra bits are the rest of the offset. */
	*extra_bits = top - 1U;
	*extra = offset & ((1UL << (top - 1U)) - 1UL);
	return 2U * top + second;
}

/*
 * Gives the colour cache index of a colour, from RFC 9649 3.6.2.3, with a
 * product held in a type wider than 32 bits and masked.
 */
static unsigned
ref_hash(
	uint32_t argb,
	unsigned bits)
{
	unsigned long product;

	/* The product modulo 2^32, then its top bits. */
	product = (0x1e35a7bdUL * (unsigned long)argb) & 0xffffffffUL;
	return (unsigned)(product >> (32U - bits));
}

/*
 * Gives the distance a code stands for, from RFC 9649 3.6.2.2.1.
 */
static size_t
ref_distance(
	uint32_t code,
	unsigned width)
{
	long distance;

	/* Beyond the neighbourhood the code is the distance plus 120. */
	if (code > 120U)
		return (size_t)(code - 120U);

	/* A neighbour: xi + yi * width, at least 1. */
	distance = (long)test_distance_map[code - 1U][0] + (long)test_distance_map[code - 1U][1] * (long)width;
	if (distance < 1L)
		distance = 1L;
	return (size_t)distance;
}

/*
 * Checks the length and distance values of every row of Table 4 that the RFC
 * lists, and all values up to 4200 and sampled ones above by the inverse.
 */
static void
test_lz77_value(void)
{
	/* Rows of Table 4: prefix code, extra bits, lowest value, highest value. */
	static const unsigned long rows[][4] = {
		{ 0, 0, 1, 1 }, { 1, 0, 2, 2 }, { 2, 0, 3, 3 }, { 3, 0, 4, 4 },
		{ 4, 1, 5, 6 }, { 5, 1, 7, 8 }, { 6, 2, 9, 12 }, { 7, 2, 13, 16 },
		{ 23, 10, 3073, 4096 }, { 38, 18, 524289UL, 786432UL }, { 39, 18, 786433UL, 1048576UL }
	};
	struct webpc_lbits reader;
	uint8_t bytes[8];
	uint32_t value;
	uint32_t extra;
	uint32_t round_value;
	unsigned extra_bits;
	unsigned prefix_code;
	unsigned row;
	unsigned index;

	/* Each listed row: no extra bits gives the lowest value, all ones the highest, and the bits read are exact. */
	for (row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
		memset(bytes, 0, sizeof(bytes));
		webpc_lbits_init(&reader, bytes, sizeof(bytes));
		value = webpc_vp8l_lz77_value(&reader, (unsigned)rows[row][0]);
		CHECK(value == rows[row][2]);

		/* Extra bits all one, written by hand as bytes: the last value of the row. */
		memset(bytes, 0xff, sizeof(bytes));
		webpc_lbits_init(&reader, bytes, sizeof(bytes));
		value = webpc_vp8l_lz77_value(&reader, (unsigned)rows[row][0]);
		CHECK(value == rows[row][3]);

		/* Exactly the row's extra bits were consumed: the next 24 bits of 0xff are still all one. */
		CHECK(webpc_lbits_read(&reader, 24) == 0xffffffU);
	}

	/* A zero reader proves the number of bits: with extra bits 5 (0b101) at code 6 the value is 9 + 1 = 10. */
	memset(bytes, 0, sizeof(bytes));
	bytes[0] = 0x01;
	webpc_lbits_init(&reader, bytes, sizeof(bytes));
	CHECK(webpc_vp8l_lz77_value(&reader, 6) == 10U);
	CHECK(webpc_lbits_read(&reader, 3) == 0U);

	/* A code beyond 39 stands for no value and reads nothing. */
	memset(bytes, 0xff, sizeof(bytes));
	webpc_lbits_init(&reader, bytes, sizeof(bytes));
	CHECK(webpc_vp8l_lz77_value(&reader, 40) == 0U);
	CHECK(webpc_vp8l_lz77_value(&reader, 1000) == 0U);
	CHECK(webpc_lbits_read(&reader, 24) == 0xffffffU);

	/* Every value: the test's inverse of the table gives the code and extra bits, and the decoder returns the value. */
	value = 1;
	for (;;) {
		prefix_code = ref_prefix_code(value, &extra_bits, &extra);
		CHECK(prefix_code < 40U);
		memset(bytes, 0, sizeof(bytes));
		for (index = 0; index < 4U; index++)
			bytes[index] = (uint8_t)((extra >> (8U * index)) & 0xffU);
		bytes[4] = 0xff;
		webpc_lbits_init(&reader, bytes, 5);
		round_value = webpc_vp8l_lz77_value(&reader, prefix_code);
		CHECK(round_value == value);

		/* Exactly the extra bits were taken: the other bits of the 32 are still unread zeros, then 0xff. */
		CHECK(webpc_lbits_read(&reader, 24U - extra_bits) == 0U);
		CHECK(webpc_lbits_read(&reader, 8) == 0U);
		CHECK(webpc_lbits_read(&reader, 8) == 0xffU);
		if (value == 1048576UL)
			break;

		/* Every value up to 4200, then a sparse set, ending on the last. */
		value += 1U;
		if (value > 4200U)
			value += 996U;
		if (value > 1048576UL)
			value = 1048576UL;
	}
}

/*
 * Checks webpc_vp8l_plane_distance against the figure for every code and
 * several widths, and the examples worked by hand.
 */
static void
test_plane_distance(void)
{
	static const unsigned widths[4] = { 1, 10, 1000, 16384 };
	uint32_t code;
	unsigned index;
	unsigned width;

	/* Every code of the neighbourhood, for each width. */
	for (index = 0; index < 4U; index++) {
		width = widths[index];
		for (code = 1; code <= 120U; code++)
			CHECK(webpc_vp8l_plane_distance(code, width) == ref_distance(code, width));

		/* Worked by hand from the first entries and the last: (0,1) (1,0) (1,1) (-1,1) and (8,7). */
		CHECK(webpc_vp8l_plane_distance(1, width) == (size_t)width);
		CHECK(webpc_vp8l_plane_distance(2, width) == 1U);
		CHECK(webpc_vp8l_plane_distance(3, width) == (size_t)width + 1U);
		CHECK(webpc_vp8l_plane_distance(120, width) == 8U + 7U * (size_t)width);
	}

	/* Code 4 is (-1, 1): width - 1, and 1 when that is not above 0 (width 1). */
	CHECK(webpc_vp8l_plane_distance(4, 1) == 1U);
	CHECK(webpc_vp8l_plane_distance(4, 10) == 9U);
	CHECK(webpc_vp8l_plane_distance(4, 1000) == 999U);

	/* A code of 121 or more is the distance plus 120. */
	CHECK(webpc_vp8l_plane_distance(121, 10) == 1U);
	CHECK(webpc_vp8l_plane_distance(122, 10) == 2U);
	CHECK(webpc_vp8l_plane_distance(1000, 7) == 880U);
	CHECK(webpc_vp8l_plane_distance(1048576UL + 120UL, 7) == 1048576UL);
	CHECK(webpc_vp8l_plane_distance(0xffffffffUL, 7) == (size_t)(0xffffffffUL - 120UL));

	/* Code 0 is no code and gives the nearest distance. */
	CHECK(webpc_vp8l_plane_distance(0, 10) == 1U);
}

/*
 * Checks the shape of the test's copy of Figure 20: 120 different pairs,
 * none (0, 0), xi from -7 to 8 and yi from 0 to 7, and a pair with yi 0 is
 * to the left (xi above 0).
 */
static void
test_distance_table_shape(void)
{
	unsigned first;
	unsigned second;
	unsigned same;
	unsigned count;
	int x_offset;
	int y_offset;

	/* No pair is the same as an earlier one. */
	same = 0;
	for (first = 0; first < 120U; first++) {
		for (second = first + 1U; second < 120U; second++) {
			if (test_distance_map[first][0] == test_distance_map[second][0] &&
			    test_distance_map[first][1] == test_distance_map[second][1])
				same++;
		}
	}
	CHECK(same == 0U);

	/* The ranges of the offsets. */
	count = 0;
	for (first = 0; first < 120U; first++) {
		x_offset = test_distance_map[first][0];
		y_offset = test_distance_map[first][1];
		CHECK(x_offset >= -7 && x_offset <= 8);
		CHECK(y_offset >= 0 && y_offset <= 7);
		CHECK(x_offset != 0 || y_offset != 0);
		if (y_offset == 0)
			CHECK(x_offset > 0);
		count++;
	}
	CHECK(count == 120U);

	/* All 112 pairs of the rows above (xi -7..8, yi 1..7) and the 8 of the same row (xi 1..8) are present. */
	for (y_offset = 0; y_offset <= 7; y_offset++) {
		for (x_offset = -7; x_offset <= 8; x_offset++) {
			if (y_offset == 0 && x_offset <= 0)
				continue;
			same = 0;
			for (first = 0; first < 120U; first++) {
				if (test_distance_map[first][0] == x_offset && test_distance_map[first][1] == y_offset)
					same++;
			}
			CHECK(same == 1U);
		}
	}
}

/*
 * Checks the cache index against three values worked by hand (the product of
 * 0x1e35a7bd and the colour modulo 2^32, then its top bits).
 */
static void
test_cache_index(void)
{
	/* 0 hashes to 0. */
	CHECK(webpc_vp8l_cache_index(0, 1) == 0U);
	CHECK(webpc_vp8l_cache_index(0, 4) == 0U);
	CHECK(webpc_vp8l_cache_index(0, 11) == 0U);

	/* 0xffffffff times the multiplier is minus it, 0xe1ca5843 modulo 2^32. */
	CHECK(webpc_vp8l_cache_index(0xffffffffU, 1) == 1U);
	CHECK(webpc_vp8l_cache_index(0xffffffffU, 4) == 14U);
	CHECK(webpc_vp8l_cache_index(0xffffffffU, 11) == 1806U);

	/* 0x12345678 gives 0x4e... in the top bits. */
	CHECK(webpc_vp8l_cache_index(0x12345678U, 1) == 1U);
	CHECK(webpc_vp8l_cache_index(0x12345678U, 4) == 8U);
	CHECK(webpc_vp8l_cache_index(0x12345678U, 11) == 1119U);

	/* A size the format does not have gives 0 and does not shift out of range. */
	CHECK(webpc_vp8l_cache_index(0xffffffffU, 0) == 0U);
	CHECK(webpc_vp8l_cache_index(0xffffffffU, 12) == 0U);
}

/*
 * Gives code lengths that make a complete code over the symbols marked in
 * used: one symbol gets length 1, k symbols get a flat code (some of length
 * b - 1 and the rest of length b, b the bits k needs).  Others are 0.
 */
static void
flat_lengths(
	uint8_t *lengths,
	unsigned alphabet,
	const uint8_t *used)
{
	unsigned count;
	unsigned symbol;
	unsigned bits;
	unsigned shorter;

	/* Counts the symbols in use. */
	count = 0;
	for (symbol = 0; symbol < alphabet; symbol++) {
		if (used[symbol] != 0U)
			count++;
	}

	/* A single symbol, or no symbol (never made by the tests). */
	memset(lengths, 0, alphabet);
	if (count == 1U) {
		for (symbol = 0; symbol < alphabet; symbol++) {
			if (used[symbol] != 0U)
				lengths[symbol] = 1;
		}
		return;
	}

	/* The bits k symbols need, and how many get one bit less so the code is complete. */
	bits = 1;
	while ((1U << bits) < count)
		bits++;
	shorter = (1U << bits) - count;

	/* The first symbols in use take the shorter length. */
	for (symbol = 0; symbol < alphabet; symbol++) {
		if (used[symbol] == 0U)
			continue;
		if (shorter > 0U) {
			lengths[symbol] = (uint8_t)(bits - 1U);
			shorter--;
		} else {
			lengths[symbol] = (uint8_t)bits;
		}
	}
}

/*
 * Assigns the codewords of a code's lengths as RFC 1951 3.2.2 does: the
 * codes of a length are consecutive in the order of the symbols, and the
 * first code of the next length follows them, shifted left.
 */
static void
assign_words(
	struct test_code *code)
{
	unsigned length;
	unsigned symbol;
	unsigned longest;
	uint32_t word;

	/* The longest length, and the number of symbols present. */
	longest = 0;
	code->present = 0;
	for (symbol = 0; symbol < code->alphabet; symbol++) {
		if (code->lengths[symbol] > longest)
			longest = code->lengths[symbol];
		if (code->lengths[symbol] != 0U)
			code->present++;
	}

	/* Hands out consecutive codewords length by length. */
	word = 0;
	for (length = 1; length <= longest; length++) {
		for (symbol = 0; symbol < code->alphabet; symbol++) {
			if (code->lengths[symbol] != length)
				continue;
			code->words[symbol] = word;
			word++;
		}
		word <<= 1;
	}
}

/*
 * Makes the code over the symbols marked in used (symbol 0 when none is).
 */
static void
code_define(
	struct test_code *code,
	unsigned alphabet,
	const uint8_t *used)
{
	uint8_t marks[WEBPC_PREFIX_ALPHABET_MAX];
	unsigned symbol;
	unsigned count;

	/* A code needs a symbol: a code nothing uses gets symbol 0. */
	count = 0;
	for (symbol = 0; symbol < alphabet; symbol++) {
		marks[symbol] = used[symbol];
		if (used[symbol] != 0U)
			count++;
	}
	if (count == 0U)
		marks[0] = 1;

	/* The lengths and codewords. */
	code->alphabet = alphabet;
	flat_lengths(code->lengths, alphabet, marks);
	assign_words(code);
}

/*
 * Writes a symbol with its code: nothing for a code of one symbol.
 */
static void
code_put_symbol(
	struct lbitwriter *writer,
	const struct test_code *code,
	unsigned symbol)
{
	/* A code of one symbol reads no bits. */
	if (code->present == 1U)
		return;

	/* The codeword, its most significant bit first. */
	lbitwriter_put_code(writer, code->words[symbol], code->lengths[symbol]);
}

/*
 * Writes a code as a normal code length code (RFC 9649 3.7.2.1.2): all 19
 * lengths of the code length code, no max_symbol, and one symbol per length.
 */
static void
code_put_normal(
	struct lbitwriter *writer,
	const struct test_code *code)
{
	struct test_code length_code;
	uint8_t length_used[19];
	unsigned symbol;
	unsigned index;

	/* The length values this code uses are the symbols of its code length code. */
	memset(length_used, 0, sizeof(length_used));
	for (symbol = 0; symbol < code->alphabet; symbol++)
		length_used[code->lengths[symbol]] = 1;
	length_code.alphabet = 19;
	flat_lengths(length_code.lengths, 19, length_used);
	assign_words(&length_code);

	/* Not simple, 19 lengths (15 + 4), the lengths in order, and no max_symbol. */
	lbitwriter_put(writer, 0, 1);
	lbitwriter_put(writer, 15, 4);
	for (index = 0; index < 19U; index++)
		lbitwriter_put(writer, length_code.lengths[test_order[index]], 3);
	lbitwriter_put(writer, 0, 1);

	/* One code length symbol for each symbol of the alphabet. */
	for (symbol = 0; symbol < code->alphabet; symbol++)
		code_put_symbol(writer, &length_code, code->lengths[symbol]);
}

/*
 * Writes a code of one or two symbols below 256 as a simple code length code
 * (RFC 9649 3.7.2.1.1).
 */
static void
code_put_simple(
	struct lbitwriter *writer,
	const struct test_code *code)
{
	unsigned symbols[2];
	unsigned found;
	unsigned symbol;
	unsigned first_8bits;

	/* The symbols in order. */
	found = 0;
	symbols[1] = 0;
	for (symbol = 0; symbol < code->alphabet && found < 2U; symbol++) {
		if (code->lengths[symbol] != 0U) {
			symbols[found] = symbol;
			found++;
		}
	}

	/* is_simple, the number of symbols less one, and whether the first needs 8 bits. */
	first_8bits = 0;
	if (symbols[0] > 1U)
		first_8bits = 1;
	lbitwriter_put(writer, 1, 1);
	lbitwriter_put(writer, found - 1U, 1);
	lbitwriter_put(writer, first_8bits, 1);

	/* The symbols. */
	lbitwriter_put(writer, symbols[0], first_8bits != 0U ? 8U : 1U);
	if (found == 2U)
		lbitwriter_put(writer, symbols[1], 8);
}

/*
 * Writes a code the way that fits: simple when it may be and allow_simple is
 * set, else normal.
 */
static void
code_put(
	struct lbitwriter *writer,
	const struct test_code *code,
	int allow_simple)
{
	unsigned symbol;
	int simple_ok;

	/* A simple code holds one or two symbols, all below 256. */
	simple_ok = 0;
	if (allow_simple != 0 && code->present <= 2U) {
		simple_ok = 1;
		for (symbol = 256; symbol < code->alphabet; symbol++) {
			if (code->lengths[symbol] != 0U)
				simple_ok = 0;
		}
	}

	/* Writes it in the chosen form. */
	if (simple_ok != 0)
		code_put_simple(writer, code);
	else
		code_put_normal(writer, code);
}

/*
 * Gives the group of the block pixel position is in.
 */
static unsigned
token_group(
	const struct test_image *image,
	size_t position)
{
	size_t x;
	size_t y;
	size_t block_width;

	/* One group when there are no meta prefix codes, or past the last pixel. */
	if (image->prefix_bits == 0U)
		return 0;
	if (position >= (size_t)image->width * image->height)
		return 0;

	/* The block of the pixel, as RFC 9649 3.7.2.2.2 works it out. */
	x = position % image->width;
	y = position / image->width;
	block_width = ((size_t)image->width + ((size_t)1 << image->prefix_bits) - 1U) >> image->prefix_bits;
	return (unsigned)image->blocks[(y >> image->prefix_bits) * block_width + (x >> image->prefix_bits)];
}

/*
 * Writes an image in the order of RFC 9649 3.8.3 and works out the pixels it
 * must decode to.  valid is set to 0 when a backward reference is not
 * possible (before the first pixel, or past the last), which the decoder must
 * refuse.
 */
static void
build_image(
	struct lbitwriter *writer,
	const struct test_image *image,
	uint32_t *expected,
	int *valid)
{
	struct test_image inner;
	uint32_t cache[2048];
	size_t block_width;
	size_t block_height;
	size_t block_count;
	size_t position;
	size_t total;
	size_t distance;
	size_t copy;
	unsigned groups;
	unsigned group;
	unsigned code_index;
	unsigned token_index;
	unsigned prefix_code;
	unsigned extra_bits;
	unsigned alphabet[WEBPC_VP8L_GROUP_CODES];
	uint32_t extra;
	uint32_t argb;
	const struct test_token *token;

	/* The cache information: 1 bit, then the size's bits when there is a cache. */
	if (image->cache_bits == 0U) {
		lbitwriter_put(writer, 0, 1);
	} else {
		lbitwriter_put(writer, 1, 1);
		lbitwriter_put(writer, image->cache_bits, 4);
	}

	/* The meta prefix codes, for the ARGB role only: the bit, prefix_bits and the entropy image inside. */
	groups = 1;
	if (image->role == 1) {
		if (image->prefix_bits == 0U) {
			lbitwriter_put(writer, 0, 1);
		} else {
			lbitwriter_put(writer, 1, 1);
			lbitwriter_put(writer, image->prefix_bits - 2U, 3);
			block_width = ((size_t)image->width + ((size_t)1 << image->prefix_bits) - 1U) >> image->prefix_bits;
			block_height = ((size_t)image->height + ((size_t)1 << image->prefix_bits) - 1U) >> image->prefix_bits;
			block_count = block_width * block_height;

			/* The entropy image: one literal pixel per block holding the group in its red and green. */
			groups = 0;
			for (token_index = 0; token_index < block_count; token_index++) {
				entropy_tokens[token_index].kind = TOKEN_LITERAL;
				entropy_tokens[token_index].argb = (uint32_t)image->blocks[token_index] << 8;
				if (image->blocks[token_index] + 1U > groups)
					groups = (unsigned)image->blocks[token_index] + 1U;
			}
			memset(&inner, 0, sizeof(inner));
			inner.width = (unsigned)block_width;
			inner.height = (unsigned)block_height;
			inner.role = 0;
			inner.tokens = entropy_tokens;
			inner.token_count = (unsigned)block_count;
			build_image(writer, &inner, NULL, NULL);
		}
	}

	/* Which symbols each group's codes use, from the tokens; the sizes of the alphabets. */
	memset(used_symbols, 0, sizeof(used_symbols));
	alphabet[WEBPC_VP8L_CODE_GREEN] = 256U + 24U;
	if (image->cache_bits != 0U)
		alphabet[WEBPC_VP8L_CODE_GREEN] += 1U << image->cache_bits;
	alphabet[WEBPC_VP8L_CODE_RED] = 256U;
	alphabet[WEBPC_VP8L_CODE_BLUE] = 256U;
	alphabet[WEBPC_VP8L_CODE_ALPHA] = 256U;
	alphabet[WEBPC_VP8L_CODE_DISTANCE] = 40U;
	position = 0;
	for (token_index = 0; token_index < image->token_count; token_index++) {
		token = &image->tokens[token_index];
		group = token_group(image, position);
		if (token->kind == TOKEN_LITERAL) {
			used_symbols[group][WEBPC_VP8L_CODE_GREEN][(token->argb >> 8) & 0xffU] = 1;
			used_symbols[group][WEBPC_VP8L_CODE_RED][(token->argb >> 16) & 0xffU] = 1;
			used_symbols[group][WEBPC_VP8L_CODE_BLUE][token->argb & 0xffU] = 1;
			used_symbols[group][WEBPC_VP8L_CODE_ALPHA][token->argb >> 24] = 1;
			position += 1U;
		} else if (token->kind == TOKEN_COPY) {
			prefix_code = ref_prefix_code(token->length, &extra_bits, &extra);
			used_symbols[group][WEBPC_VP8L_CODE_GREEN][256U + prefix_code] = 1;
			prefix_code = ref_prefix_code(token->dcode, &extra_bits, &extra);
			used_symbols[group][WEBPC_VP8L_CODE_DISTANCE][prefix_code] = 1;
			position += token->length;
		} else {
			used_symbols[group][WEBPC_VP8L_CODE_GREEN][280U + token->index] = 1;
			position += 1U;
		}
	}

	/* A group no token uses gets as many symbols in each code as the image asks for (at least one). */
	for (group = 0; group < groups; group++) {
		/* The group's token count is not known here, so a group with no symbol in green is unused. */
		argb = 0;
		for (code_index = 0; code_index < alphabet[WEBPC_VP8L_CODE_GREEN]; code_index++) {
			if (used_symbols[group][WEBPC_VP8L_CODE_GREEN][code_index] != 0U)
				argb = 1;
		}
		if (argb != 0U)
			continue;
		for (code_index = 0; code_index < WEBPC_VP8L_GROUP_CODES; code_index++) {
			for (token_index = 0; token_index < image->unused_symbols && token_index < alphabet[code_index];
			    token_index++)
				used_symbols[group][code_index][token_index] = 1;
		}
	}

	/* Defines every group's five codes and writes them; every other group uses the simple form when it can. */
	for (group = 0; group < groups; group++) {
		for (code_index = 0; code_index < WEBPC_VP8L_GROUP_CODES; code_index++) {
			code_define(&group_codes[group][code_index], alphabet[code_index], used_symbols[group][code_index]);
			code_put(writer, &group_codes[group][code_index], ((group + code_index) & 1U) == 0U);
		}
	}

	/* The pixels: each token's bits, and the pixels the test works out for itself. */
	if (valid != NULL)
		*valid = 1;
	memset(cache, 0, sizeof(cache));
	position = 0;
	total = (size_t)image->width * image->height;
	for (token_index = 0; token_index < image->token_count; token_index++) {
		token = &image->tokens[token_index];
		group = token_group(image, position);
		if (token->kind == TOKEN_LITERAL) {
			code_put_symbol(writer, &group_codes[group][WEBPC_VP8L_CODE_GREEN], (token->argb >> 8) & 0xffU);
			code_put_symbol(writer, &group_codes[group][WEBPC_VP8L_CODE_RED], (token->argb >> 16) & 0xffU);
			code_put_symbol(writer, &group_codes[group][WEBPC_VP8L_CODE_BLUE], token->argb & 0xffU);
			code_put_symbol(writer, &group_codes[group][WEBPC_VP8L_CODE_ALPHA], token->argb >> 24);
			if (expected != NULL && position < total) {
				expected[position] = token->argb;
				if (image->cache_bits != 0U)
					cache[ref_hash(token->argb, image->cache_bits)] = token->argb;
			}
			position += 1U;
		} else if (token->kind == TOKEN_COPY) {
			/* The length: its prefix as a green symbol and its extra bits; then the distance code's. */
			prefix_code = ref_prefix_code(token->length, &extra_bits, &extra);
			code_put_symbol(writer, &group_codes[group][WEBPC_VP8L_CODE_GREEN], 256U + prefix_code);
			lbitwriter_put(writer, extra, extra_bits);
			prefix_code = ref_prefix_code(token->dcode, &extra_bits, &extra);
			code_put_symbol(writer, &group_codes[group][WEBPC_VP8L_CODE_DISTANCE], prefix_code);
			lbitwriter_put(writer, extra, extra_bits);

			/* The copy must start inside the image and end inside it. */
			distance = ref_distance(token->dcode, image->width);
			if (distance > position || token->length > total - position) {
				if (valid != NULL)
					*valid = 0;
				position += token->length;
				continue;
			}

			/* One pixel at a time, caching each, as the spec says. */
			for (copy = 0; copy < token->length; copy++) {
				if (expected != NULL) {
					argb = expected[position + copy - distance];
					expected[position + copy] = argb;
					if (image->cache_bits != 0U)
						cache[ref_hash(argb, image->cache_bits)] = argb;
				}
			}
			position += token->length;
		} else {
			code_put_symbol(writer, &group_codes[group][WEBPC_VP8L_CODE_GREEN], 280U + token->index);
			if (expected != NULL && position < total) {
				argb = cache[token->index];
				expected[position] = argb;
				cache[ref_hash(argb, image->cache_bits)] = argb;
			}
			position += 1U;
		}
	}

	/* The tokens must fill the image exactly for a valid image. */
	if (position != total && valid != NULL)
		*valid = 0;
}

/*
 * Decodes bytes (a copy of exactly size bytes, so a read past the end is
 * caught) with webpc_vp8l_decode_image into pixels, and when it succeeds
 * reads the marker that follows.  Gives the error.
 */
static int
decode_stream(
	const uint8_t *bytes,
	size_t size,
	unsigned width,
	unsigned height,
	int role,
	size_t limit,
	uint32_t *pixels,
	int *marker_ok)
{
	struct webpc_lbits reader;
	uint8_t *copy;
	uint32_t marker;
	int error;

	/* An exact-size copy, so AddressSanitizer sees a read past the end. */
	copy = (uint8_t *)malloc(size > 0U ? size : 1U);
	CHECK(copy != NULL);
	if (copy == NULL)
		return -1;
	if (size > 0U)
		memcpy(copy, bytes, size);

	/* Decodes. */
	webpc_lbits_init(&reader, copy, size);
	error = webpc_vp8l_decode_image(&reader, width, height, role, limit, pixels);

	/* The marker after the image, when the image decoded. */
	if (marker_ok != NULL) {
		*marker_ok = 0;
		if (error == 0) {
			marker = webpc_lbits_read(&reader, 8);
			if (marker == TEST_MARKER && webpc_lbits_ended(&reader) == 0)
				*marker_ok = 1;
		}
	}

	free(copy);
	return error;
}

/*
 * Writes the image with the marker after it, and checks the decoder gives
 * exactly the pixels the test worked out and stops exactly at the marker.
 */
static void
check_image(
	const struct test_image *image)
{
	uint32_t *pixels;
	size_t total;
	size_t index;
	int valid;
	int marker_ok;
	int error;

	/* Writes the image and the marker. */
	lbitwriter_init(&stream);
	build_image(&stream, image, expected_pixels, &valid);
	lbitwriter_put(&stream, TEST_MARKER, 8);
	CHECK(stream.overflow == 0);
	CHECK(valid != 0);

	/* Decodes into a buffer of exactly the image's size. */
	total = (size_t)image->width * image->height;
	pixels = (uint32_t *)malloc(total * sizeof(uint32_t));
	CHECK(pixels != NULL);
	if (pixels == NULL)
		return;
	error = decode_stream(stream.bytes, lbitwriter_size(&stream), image->width, image->height, image->role,
		TEST_LIMIT, pixels, &marker_ok);
	CHECK(error == 0);
	CHECK(marker_ok != 0);

	/* Every pixel is the one the test worked out. */
	if (error == 0) {
		for (index = 0; index < total; index++)
			CHECK(pixels[index] == expected_pixels[index]);
	}

	free(pixels);
}

/*
 * Gives a random colour with every channel random.
 */
static uint32_t
random_colour(void)
{
	/* Four random bytes. */
	return (uint32_t)check_random(&test_state);
}

/*
 * Fills tokens with a random image of width by height pixels (a palette of
 * the given number of colours, copies by a neighbour code or a distance,
 * cache indices when cache_bits is not 0).  Gives the token count.
 */
static unsigned
gen_tokens(
	unsigned width,
	unsigned height,
	unsigned cache_bits,
	unsigned colours)
{
	uint32_t palette[16];
	size_t position;
	size_t total;
	size_t remaining;
	size_t limit;
	unsigned count;
	unsigned choice;
	unsigned index;
	uint32_t code;

	/* The palette. */
	for (index = 0; index < colours && index < 16U; index++)
		palette[index] = random_colour();

	/* One token at a time until the pixels are all given. */
	total = (size_t)width * height;
	position = 0;
	count = 0;
	while (position < total && count < TEST_TOKENS_MAX) {
		remaining = total - position;
		choice = (unsigned)(check_random(&test_state) % 8UL);
		tokens[count].argb = 0;
		tokens[count].length = 0;
		tokens[count].dcode = 0;
		tokens[count].index = 0;

		if (choice == 0U && position > 0U) {
			/* A copy by a neighbour code, when the neighbour is inside the image so far. */
			code = 1U + (uint32_t)(check_random(&test_state) % 120UL);
			if (ref_distance(code, width) <= position) {
				tokens[count].kind = TOKEN_COPY;
				tokens[count].dcode = code;
				limit = remaining;
				if (limit > 12U)
					limit = 12U;
				tokens[count].length = 1U + (uint32_t)(check_random(&test_state) % limit);
				position += tokens[count].length;
				count++;
				continue;
			}
		}
		if (choice == 1U && position > 0U) {
			/* A copy by a plain distance. */
			tokens[count].kind = TOKEN_COPY;
			tokens[count].dcode = 121U + (uint32_t)(check_random(&test_state) % position);
			limit = remaining;
			if (limit > 30U)
				limit = 30U;
			tokens[count].length = 1U + (uint32_t)(check_random(&test_state) % limit);
			position += tokens[count].length;
			count++;
			continue;
		}
		if (choice == 2U && cache_bits != 0U) {
			/* A cache index. */
			tokens[count].kind = TOKEN_CACHE;
			tokens[count].index = (unsigned)(check_random(&test_state) % (1UL << cache_bits));
			position += 1U;
			count++;
			continue;
		}

		/* A literal from the palette. */
		tokens[count].kind = TOKEN_LITERAL;
		tokens[count].argb = palette[check_random(&test_state) % colours];
		position += 1U;
		count++;
	}

	/* The count. */
	return count;
}

/*
 * A literal-only image, with and without the meta bit, and one pixel.
 */
static void
test_literals(void)
{
	struct test_image image;
	unsigned index;

	/* 4 by 3 pixels, every colour different, role 1 with no meta prefix codes. */
	for (index = 0; index < 12U; index++) {
		tokens[index].kind = TOKEN_LITERAL;
		tokens[index].argb = random_colour();
	}
	memset(&image, 0, sizeof(image));
	image.width = 4;
	image.height = 3;
	image.role = 1;
	image.tokens = tokens;
	image.token_count = 12;
	check_image(&image);

	/* The same as a role 0 image. */
	image.role = 0;
	check_image(&image);

	/* A single pixel, and a single-colour image (every code of one symbol: no bits per pixel). */
	image.width = 1;
	image.height = 1;
	image.token_count = 1;
	check_image(&image);
	image.width = 3;
	image.height = 1;
	image.token_count = 3;
	tokens[1].argb = tokens[0].argb;
	tokens[2].argb = tokens[0].argb;
	image.role = 1;
	check_image(&image);

	/* A wide image of 256 different greens, reds, blues and alphas (the codes are full 256-symbol codes). */
	for (index = 0; index < 256U; index++) {
		tokens[index].kind = TOKEN_LITERAL;
		tokens[index].argb = ((uint32_t)index << 24) | ((uint32_t)((index * 7U) & 255U) << 16) |
		    ((uint32_t)((index * 13U + 5U) & 255U) << 8) | (uint32_t)((index * 29U + 1U) & 255U);
	}
	image.width = 16;
	image.height = 16;
	image.token_count = 256;
	check_image(&image);
}

/*
 * The colour cache: indices of colours seen, an unset entry, and cache sizes
 * 1, 4 and 11.
 */
static void
test_cache(void)
{
	struct test_image image;
	uint32_t colours[6];
	unsigned index;
	unsigned bits;

	/* Six colours. */
	for (index = 0; index < 6U; index++)
		colours[index] = random_colour();

	/* Literals, then cache indices of colours the cache holds, an arbitrary index and a literal again. */
	for (bits = 1; bits <= 11U; bits += 5U) {
		tokens[0].kind = TOKEN_LITERAL;
		tokens[0].argb = colours[0];
		tokens[1].kind = TOKEN_LITERAL;
		tokens[1].argb = colours[1];
		tokens[2].kind = TOKEN_LITERAL;
		tokens[2].argb = colours[2];
		tokens[3].kind = TOKEN_CACHE;
		tokens[3].index = ref_hash(colours[1], bits);
		tokens[4].kind = TOKEN_CACHE;
		tokens[4].index = ref_hash(colours[0], bits);
		tokens[5].kind = TOKEN_LITERAL;
		tokens[5].argb = colours[3];
		tokens[6].kind = TOKEN_CACHE;
		tokens[6].index = ref_hash(colours[2], bits);
		tokens[7].kind = TOKEN_CACHE;
		tokens[7].index = ((1U << bits) - 1U) & 5U;
		tokens[8].kind = TOKEN_CACHE;
		tokens[8].index = ref_hash(colours[3], bits);
		memset(&image, 0, sizeof(image));
		image.width = 3;
		image.height = 3;
		image.role = 1;
		image.cache_bits = bits;
		image.tokens = tokens;
		image.token_count = 9;
		check_image(&image);

		/* As a role 0 image, which also carries the cache information. */
		image.role = 0;
		check_image(&image);
	}

	/* Random images with a cache of each size from 1 to 11. */
	for (bits = 1; bits <= 11U; bits++) {
		memset(&image, 0, sizeof(image));
		image.width = 9;
		image.height = 7;
		image.role = 1;
		image.cache_bits = bits;
		image.tokens = tokens;
		image.token_count = gen_tokens(9, 7, bits, 5);
		check_image(&image);
	}
}

/*
 * What a pixel does to the cache: a cache hit enters the cache again, a copy
 * enters each of its pixels.
 */
static void
test_cache_updates(void)
{
	struct test_image image;
	uint32_t first;
	uint32_t second;
	uint32_t colour;

	/* Finds two different non-zero colours that hash to entry 0 of a 1-bit cache. */
	first = 0;
	second = 0;
	while (first == 0U || second == 0U) {
		colour = random_colour();
		if (colour == 0U || ref_hash(colour, 1) != 0U)
			continue;
		if (first == 0U)
			first = colour;
		else if (colour != first)
			second = colour;
	}

	/*
	 * A literal puts entry 0 at first; the unset entry 1 reads 0, and 0 hashes
	 * to entry 0, so the cache hit puts 0 over first; the next index 0 reads 0.
	 */
	tokens[0].kind = TOKEN_LITERAL;
	tokens[0].argb = first;
	tokens[1].kind = TOKEN_CACHE;
	tokens[1].index = 1;
	tokens[2].kind = TOKEN_CACHE;
	tokens[2].index = 0;
	memset(&image, 0, sizeof(image));
	image.width = 3;
	image.height = 1;
	image.role = 1;
	image.cache_bits = 1;
	image.tokens = tokens;
	image.token_count = 3;
	check_image(&image);
	CHECK(expected_pixels[2] == 0U);

	/*
	 * Two colours of the same entry: the second evicts the first; a copy of the
	 * first (distance 2) puts it back, so index 0 then reads the first again.
	 */
	tokens[0].kind = TOKEN_LITERAL;
	tokens[0].argb = first;
	tokens[1].kind = TOKEN_LITERAL;
	tokens[1].argb = second;
	tokens[2].kind = TOKEN_COPY;
	tokens[2].length = 1;
	tokens[2].dcode = 122;
	tokens[3].kind = TOKEN_CACHE;
	tokens[3].index = 0;
	image.width = 4;
	image.token_count = 4;
	check_image(&image);
	CHECK(expected_pixels[2] == first);
	CHECK(expected_pixels[3] == first);
}

/*
 * Backward references: overlapping copies, neighbour codes, the longest
 * length, far distances.
 */
static void
test_copies(void)
{
	struct test_image image;
	unsigned index;

	/* A copy at distance 1 of length 10 overlaps what it makes; then a copy at distance 2. */
	memset(&image, 0, sizeof(image));
	tokens[0].kind = TOKEN_LITERAL;
	tokens[0].argb = 0x80102030U;
	tokens[1].kind = TOKEN_COPY;
	tokens[1].length = 10;
	tokens[1].dcode = 121;
	tokens[2].kind = TOKEN_LITERAL;
	tokens[2].argb = 0x7f405060U;
	tokens[3].kind = TOKEN_COPY;
	tokens[3].length = 4;
	tokens[3].dcode = 122;
	image.width = 16;
	image.height = 1;
	image.role = 1;
	image.tokens = tokens;
	image.token_count = 4;
	check_image(&image);
	for (index = 0; index < 11U; index++)
		CHECK(expected_pixels[index] == 0x80102030U);
	CHECK(expected_pixels[11] == 0x7f405060U);
	CHECK(expected_pixels[12] == 0x80102030U);
	CHECK(expected_pixels[13] == 0x7f405060U);

	/* The same with distance code 2, the pixel to the left (1, 0). */
	tokens[1].dcode = 2;
	check_image(&image);

	/* A 4 by 3 image of role 0: a row, the row above (code 1), up-left (code 3) and up-right (code 4) copies. */
	for (index = 0; index < 4U; index++) {
		tokens[index].kind = TOKEN_LITERAL;
		tokens[index].argb = random_colour();
	}
	tokens[4].kind = TOKEN_COPY;
	tokens[4].length = 4;
	tokens[4].dcode = 1;
	tokens[5].kind = TOKEN_COPY;
	tokens[5].length = 1;
	tokens[5].dcode = 3;
	tokens[6].kind = TOKEN_COPY;
	tokens[6].length = 3;
	tokens[6].dcode = 4;
	image.width = 4;
	image.height = 3;
	image.role = 0;
	image.token_count = 7;
	check_image(&image);
	CHECK(expected_pixels[4] == expected_pixels[0]);
	CHECK(expected_pixels[7] == expected_pixels[3]);

	/* The longest copies: 4096 pixels twice in a row, then 63 more; the distance 1 fills the image. */
	tokens[0].kind = TOKEN_LITERAL;
	tokens[0].argb = random_colour();
	tokens[1].kind = TOKEN_COPY;
	tokens[1].length = 4096;
	tokens[1].dcode = 121;
	tokens[2].kind = TOKEN_COPY;
	tokens[2].length = 63;
	tokens[2].dcode = 122;
	image.width = 64;
	image.height = 65;
	image.role = 1;
	image.token_count = 3;
	check_image(&image);

	/* Far distances with many extra bits: 1000 back, and 1 + 1100 pixels before it. */
	tokens[0].kind = TOKEN_LITERAL;
	tokens[0].argb = random_colour();
	tokens[1].kind = TOKEN_LITERAL;
	tokens[1].argb = random_colour();
	tokens[2].kind = TOKEN_COPY;
	tokens[2].length = 1098;
	tokens[2].dcode = 121;
	tokens[3].kind = TOKEN_COPY;
	tokens[3].length = 50;
	tokens[3].dcode = 120U + 1000U;
	image.width = 1150;
	image.height = 1;
	image.token_count = 4;
	check_image(&image);

	/* Random images with copies of every kind. */
	for (index = 0; index < 40U; index++) {
		image.width = 3U + index % 17U;
		image.height = 1U + index % 9U;
		image.role = (int)(index & 1U);
		image.token_count = gen_tokens(image.width, image.height, 0, 2U + index % 5U);
		check_image(&image);
	}
}

/*
 * Backward references the format does not allow, and the borders of the
 * allowed ones.
 */
static void
test_copy_errors(void)
{
	struct test_image image;
	uint32_t pixels[8];
	int valid;
	int marker_ok;
	int error;

	/* A reference as the first thing: nothing before the first pixel. */
	memset(&image, 0, sizeof(image));
	tokens[0].kind = TOKEN_COPY;
	tokens[0].length = 1;
	tokens[0].dcode = 121;
	tokens[1].kind = TOKEN_LITERAL;
	tokens[1].argb = 0x11223344U;
	image.width = 2;
	image.height = 1;
	image.role = 1;
	image.tokens = tokens;
	image.token_count = 2;
	lbitwriter_init(&stream);
	build_image(&stream, &image, expected_pixels, &valid);
	CHECK(valid == 0);
	error = decode_stream(stream.bytes, lbitwriter_size(&stream), 2, 1, 1, TEST_LIMIT, pixels, &marker_ok);
	CHECK(error == EINVAL);

	/* A distance one more than the pixels before it: a literal and a copy at distance 2. */
	tokens[0].kind = TOKEN_LITERAL;
	tokens[0].argb = 0x55667788U;
	tokens[1].kind = TOKEN_COPY;
	tokens[1].length = 1;
	tokens[1].dcode = 122;
	image.width = 2;
	lbitwriter_init(&stream);
	build_image(&stream, &image, expected_pixels, &valid);
	CHECK(valid == 0);
	error = decode_stream(stream.bytes, lbitwriter_size(&stream), 2, 1, 1, TEST_LIMIT, pixels, &marker_ok);
	CHECK(error == EINVAL);

	/* The same with distance 1: exactly the first pixel is the source. */
	tokens[1].dcode = 121;
	check_image(&image);

	/* A neighbour code that reaches before the start: code 1 (the row above) on the first row. */
	tokens[1].dcode = 1;
	image.width = 4;
	tokens[2].kind = TOKEN_LITERAL;
	tokens[2].argb = 0x01020304U;
	tokens[3].kind = TOKEN_LITERAL;
	tokens[3].argb = 0x05060708U;
	image.height = 2;
	image.token_count = 2;
	lbitwriter_init(&stream);
	build_image(&stream, &image, expected_pixels, &valid);
	error = decode_stream(stream.bytes, lbitwriter_size(&stream), 4, 2, 1, TEST_LIMIT, pixels, &marker_ok);
	CHECK(error == EINVAL);

	/* A copy that ends exactly at the last pixel is fine; one pixel more is refused. */
	tokens[0].kind = TOKEN_LITERAL;
	tokens[0].argb = 0x0a0b0c0dU;
	tokens[1].kind = TOKEN_COPY;
	tokens[1].length = 3;
	tokens[1].dcode = 121;
	image.width = 4;
	image.height = 1;
	image.token_count = 2;
	check_image(&image);
	tokens[1].length = 4;
	lbitwriter_init(&stream);
	build_image(&stream, &image, expected_pixels, &valid);
	CHECK(valid == 0);
	error = decode_stream(stream.bytes, lbitwriter_size(&stream), 4, 1, 1, TEST_LIMIT, pixels, &marker_ok);
	CHECK(error == EINVAL);

	/* A reference on the second row that passes the end of the image. */
	tokens[1].length = 4;
	image.width = 2;
	image.height = 2;
	tokens[1].dcode = 121;
	tokens[0].kind = TOKEN_LITERAL;
	image.token_count = 2;
	lbitwriter_init(&stream);
	build_image(&stream, &image, expected_pixels, &valid);
	error = decode_stream(stream.bytes, lbitwriter_size(&stream), 2, 2, 1, TEST_LIMIT, pixels, &marker_ok);
	CHECK(error == EINVAL);
}

/*
 * Meta prefix codes: a different group per block, groups no block names,
 * blocks that are partial at the edges, copies that cross blocks.
 */
static void
test_meta(void)
{
	static const uint32_t blocks_a[6] = { 0, 3, 3, 3, 0, 0 };
	static const uint32_t blocks_b[2] = { 1, 0 };
	static const uint32_t blocks_c[16] = { 2, 0, 1, 2, 1, 1, 0, 2, 2, 1, 0, 0, 1, 2, 0, 1 };
	static const uint32_t blocks_d[6] = { 1, 0, 2, 0, 2, 1 };
	struct test_image image;
	unsigned seed;

	/* 12 by 8 pixels in blocks of 4: 3 by 2 blocks of groups 0 and 3 (1 and 2 unused, kept as 3 symbols each). */
	memset(&image, 0, sizeof(image));
	image.width = 12;
	image.height = 8;
	image.role = 1;
	image.cache_bits = 0;
	image.prefix_bits = 2;
	image.blocks = blocks_a;
	image.unused_symbols = 3;
	image.tokens = tokens;
	for (seed = 0; seed < 20U; seed++) {
		image.token_count = gen_tokens(12, 8, 0, 3U + seed % 6U);
		check_image(&image);
	}

	/* The same with a colour cache. */
	image.cache_bits = 5;
	for (seed = 0; seed < 20U; seed++) {
		image.token_count = gen_tokens(12, 8, 5, 3U + seed % 6U);
		check_image(&image);
	}

	/* 10 by 5 pixels in blocks of 8: 2 by 1 blocks, the second partial; groups 1 and 0. */
	image.width = 10;
	image.height = 5;
	image.prefix_bits = 3;
	image.blocks = blocks_b;
	image.cache_bits = 0;
	for (seed = 0; seed < 20U; seed++) {
		image.token_count = gen_tokens(10, 5, 0, 4U);
		check_image(&image);
	}

	/* 13 by 13 pixels in blocks of 4: 4 by 4 blocks, the last of each row and column one pixel wide. */
	image.width = 13;
	image.height = 13;
	image.prefix_bits = 2;
	image.blocks = blocks_c;
	image.cache_bits = 0;
	for (seed = 0; seed < 10U; seed++) {
		image.token_count = gen_tokens(13, 13, 0, 4U);
		check_image(&image);
	}

	/* 17 by 9 pixels in blocks of 8: 3 by 2 blocks with a one-pixel edge, then 9 by 17 with 2 by 3. */
	image.width = 17;
	image.height = 9;
	image.prefix_bits = 3;
	image.blocks = blocks_d;
	for (seed = 0; seed < 10U; seed++) {
		image.token_count = gen_tokens(17, 9, 0, 4U);
		check_image(&image);
	}
	image.width = 9;
	image.height = 17;
	for (seed = 0; seed < 10U; seed++) {
		image.token_count = gen_tokens(9, 17, 0, 4U);
		check_image(&image);
	}

	/* 15 by 15 pixels in blocks of 4: 4 by 4 blocks of groups 0 to 2. */
	image.width = 15;
	image.height = 15;
	image.prefix_bits = 2;
	image.blocks = blocks_c;
	image.cache_bits = 3;
	for (seed = 0; seed < 20U; seed++) {
		image.token_count = gen_tokens(15, 15, 3, 4U);
		check_image(&image);
	}
}

/*
 * A role 0 image reads no meta prefix bit: the same stream as role 1 would
 * read a bit that is not there.
 */
static void
test_role_zero(void)
{
	struct test_image image;
	uint32_t pixels[12];
	int marker_ok;
	int error;
	unsigned index;
	int same;

	/* A small role 0 image with a cache: its stream has no meta bit. */
	memset(&image, 0, sizeof(image));
	image.width = 4;
	image.height = 3;
	image.role = 0;
	image.cache_bits = 2;
	image.tokens = tokens;
	image.token_count = gen_tokens(4, 3, 2, 3);
	check_image(&image);

	/* Read as role 1 the next bit would be taken for the meta bit; the result is not the same image. */
	error = decode_stream(stream.bytes, lbitwriter_size(&stream), 4, 3, 1, TEST_LIMIT, pixels, &marker_ok);
	same = 0;
	if (error == 0 && marker_ok != 0) {
		same = 1;
		for (index = 0; index < 12U; index++) {
			if (pixels[index] != expected_pixels[index])
				same = 0;
		}
	}
	CHECK(same == 0);
}

/*
 * A colour cache of 0 bits, or of more than 11, is refused.
 */
static void
test_cache_bits_errors(void)
{
	static const unsigned bad_bits[5] = { 0, 12, 13, 14, 15 };
	struct test_image image;
	uint32_t pixels[12];
	size_t size;
	unsigned index;
	unsigned role;
	int error;

	/* A valid 4 by 3 image of each role with a cache of 4 bits. */
	for (role = 0; role < 2U; role++) {
		memset(&image, 0, sizeof(image));
		image.width = 4;
		image.height = 3;
		image.role = (int)role;
		image.cache_bits = 4;
		image.tokens = tokens;
		image.token_count = gen_tokens(4, 3, 4, 3);
		check_image(&image);
		size = lbitwriter_size(&stream);

		/* The 5 first bits are "a cache" and 4 bits of size; the size 0 and 12 to 15 are refused. */
		for (index = 0; index < 5U; index++) {
			stream.bytes[0] = (uint8_t)((stream.bytes[0] & 0xe0U) | 1U | (bad_bits[index] << 1));
			error = decode_stream(stream.bytes, size, 4, 3, (int)role, TEST_LIMIT, pixels, NULL);
			CHECK(error == EINVAL);
		}
	}
}

/*
 * A width or height outside 1 to 16384 is refused.
 */
static void
test_size_errors(void)
{
	struct test_image image;
	uint32_t pixels[4];
	size_t size;
	unsigned index;
	int error;

	/* A valid 1 by 1 image: it decodes at its own size, and a size outside the range is judged first. */
	memset(&image, 0, sizeof(image));
	image.width = 1;
	image.height = 1;
	image.role = 1;
	image.tokens = tokens;
	image.token_count = 1;
	tokens[0].kind = TOKEN_LITERAL;
	tokens[0].argb = 0x12345678U;
	check_image(&image);
	size = lbitwriter_size(&stream);
	error = decode_stream(stream.bytes, size, 0, 1, 1, TEST_LIMIT, pixels, NULL);
	CHECK(error == EINVAL);
	error = decode_stream(stream.bytes, size, 1, 0, 1, TEST_LIMIT, pixels, NULL);
	CHECK(error == EINVAL);
	error = decode_stream(stream.bytes, size, WEBPC_VP8L_SIDE_MAX + 1U, 1, 1, TEST_LIMIT, pixels, NULL);
	CHECK(error == EINVAL);
	error = decode_stream(stream.bytes, size, 1, WEBPC_VP8L_SIDE_MAX + 1U, 1, TEST_LIMIT, pixels, NULL);
	CHECK(error == EINVAL);

	/* The largest widths and heights are valid: 16384 by 1 and 1 by 16384, a literal and copies of 4096 and 4095. */
	tokens[1].kind = TOKEN_COPY;
	tokens[1].length = 4096;
	tokens[1].dcode = 121;
	tokens[2].kind = TOKEN_COPY;
	tokens[2].length = 4096;
	tokens[2].dcode = 121;
	tokens[3].kind = TOKEN_COPY;
	tokens[3].length = 4096;
	tokens[3].dcode = 121;
	tokens[4].kind = TOKEN_COPY;
	tokens[4].length = 4095;
	tokens[4].dcode = 121;
	image.token_count = 5;
	for (index = 0; index < 2U; index++) {
		image.width = index == 0U ? WEBPC_VP8L_SIDE_MAX : 1U;
		image.height = index == 0U ? 1U : WEBPC_VP8L_SIDE_MAX;
		check_image(&image);
	}
}

/*
 * Gives the smallest table limit with which the stream decodes, found by
 * halving.
 */
static size_t
min_limit(
	const uint8_t *bytes,
	size_t size,
	const struct test_image *image)
{
	uint32_t *pixels;
	size_t low;
	size_t high;
	size_t middle;
	int error;

	/* The pixels' buffer. */
	pixels = (uint32_t *)malloc((size_t)image->width * image->height * sizeof(uint32_t));
	CHECK(pixels != NULL);
	if (pixels == NULL)
		return 0;

	/* The largest limit works; find the smallest that does. */
	low = 0;
	high = 1U << 20;
	error = decode_stream(bytes, size, image->width, image->height, image->role, high, pixels, NULL);
	CHECK(error == 0);
	while (low < high) {
		middle = low + (high - low) / 2U;
		error = decode_stream(bytes, size, image->width, image->height, image->role, middle, pixels, NULL);
		if (error == 0) {
			high = middle;
		} else {
			CHECK(error == ENOMEM);
			low = middle + 1U;
		}
	}

	free(pixels);
	return low;
}

/*
 * The table limit: too small is ENOMEM; groups no block names do not count
 * against it, though each is read in a pool with the same limit.
 */
static void
test_limit(void)
{
	static const uint32_t blocks[4] = { 0, 7, 7, 0 };
	struct test_image image;
	size_t limit_cheap;
	size_t limit_large;
	size_t size_cheap;
	uint8_t *bytes_cheap;
	uint32_t pixels[256];
	int error;

	/* A 16 by 16 image in blocks of 8, groups 0 and 7 used (1 to 6 unused), 16 colours so the codes have tables. */
	memset(&image, 0, sizeof(image));
	image.width = 16;
	image.height = 16;
	image.role = 1;
	image.prefix_bits = 3;
	image.blocks = blocks;
	image.unused_symbols = 1;
	image.tokens = tokens;
	image.token_count = gen_tokens(16, 16, 0, 16);

	/* Writes it with cheap unused groups and finds the smallest limit. */
	lbitwriter_init(&stream);
	build_image(&stream, &image, expected_pixels, NULL);
	size_cheap = lbitwriter_size(&stream);
	bytes_cheap = (uint8_t *)malloc(size_cheap);
	CHECK(bytes_cheap != NULL);
	if (bytes_cheap == NULL)
		return;
	memcpy(bytes_cheap, stream.bytes, size_cheap);
	limit_cheap = min_limit(bytes_cheap, size_cheap, &image);
	CHECK(limit_cheap > 0U);

	/* One less is ENOMEM, a limit of 0 too. */
	error = decode_stream(bytes_cheap, size_cheap, 16, 16, 1, limit_cheap - 1U, pixels, NULL);
	CHECK(error == ENOMEM);
	error = decode_stream(bytes_cheap, size_cheap, 16, 16, 1, 0, pixels, NULL);
	CHECK(error == ENOMEM);
	free(bytes_cheap);

	/*
	 * Unused groups whose codes are large (one alone fits the limit, the six
	 * together do not): the limit that is enough does not grow, so they are
	 * neither counted against the kept tables nor added up in the scratch pool.
	 */
	image.unused_symbols = 16;
	lbitwriter_init(&stream);
	build_image(&stream, &image, expected_pixels, NULL);
	limit_large = min_limit(stream.bytes, lbitwriter_size(&stream), &image);
	CHECK(limit_large == limit_cheap);
}

/*
 * A stream cut short at any byte is ENOBUFS.
 */
static void
test_truncated(void)
{
	static const uint32_t blocks[6] = { 0, 2, 1, 2, 0, 1 };
	struct test_image image;
	uint32_t pixels[96];
	size_t size;
	size_t cut;
	int error;

	/* A 12 by 8 image with meta prefix codes, a cache and copies. */
	memset(&image, 0, sizeof(image));
	image.width = 12;
	image.height = 8;
	image.role = 1;
	image.cache_bits = 4;
	image.prefix_bits = 2;
	image.blocks = blocks;
	image.unused_symbols = 2;
	image.tokens = tokens;
	image.token_count = gen_tokens(12, 8, 4, 6);
	lbitwriter_init(&stream);
	build_image(&stream, &image, expected_pixels, NULL);
	size = lbitwriter_size(&stream);

	/* The whole stream decodes, and every shorter one is cut short. */
	error = decode_stream(stream.bytes, size, 12, 8, 1, TEST_LIMIT, pixels, NULL);
	CHECK(error == 0);
	for (cut = 0; cut < size; cut++) {
		error = decode_stream(stream.bytes, cut, 12, 8, 1, TEST_LIMIT, pixels, NULL);
		CHECK(error == ENOBUFS);
	}
}

/*
 * Random bytes, and valid streams with a few bits flipped or cut: the
 * decoder gives one of its errors or an image, and never reads or writes out
 * of range.
 */
static void
test_random(void)
{
	static const uint32_t blocks[4] = { 0, 1, 1, 0 };
	struct test_image image;
	uint8_t bytes[96];
	uint32_t *pixels;
	size_t size;
	size_t total;
	unsigned width;
	unsigned height;
	unsigned round;
	unsigned flips;
	unsigned bit;
	int error;
	int role;

	/* 1000 streams of random bytes of random sizes and images of random sizes. */
	for (round = 0; round < 1000U; round++) {
		size = (size_t)(check_random(&test_state) % 96UL);
		for (bit = 0; bit < size; bit++)
			bytes[bit] = (uint8_t)check_random(&test_state);
		width = 1U + (unsigned)(check_random(&test_state) % 20UL);
		height = 1U + (unsigned)(check_random(&test_state) % 20UL);
		role = (int)(check_random(&test_state) & 1UL);
		total = (size_t)width * height;
		pixels = (uint32_t *)malloc(total * sizeof(uint32_t));
		CHECK(pixels != NULL);
		if (pixels == NULL)
			return;
		error = decode_stream(bytes, size, width, height, role, 1U << 16, pixels, NULL);
		CHECK(error == 0 || error == EINVAL || error == ENOMEM || error == ENOBUFS);
		free(pixels);
	}

	/* 1000 valid streams with 1 to 3 flipped bits and sometimes a cut. */
	memset(&image, 0, sizeof(image));
	image.width = 8;
	image.height = 8;
	image.role = 1;
	image.cache_bits = 3;
	image.prefix_bits = 2;
	image.blocks = blocks;
	image.unused_symbols = 1;
	image.tokens = tokens;
	pixels = (uint32_t *)malloc(64U * sizeof(uint32_t));
	CHECK(pixels != NULL);
	if (pixels == NULL)
		return;
	for (round = 0; round < 1000U; round++) {
		image.token_count = gen_tokens(8, 8, 3, 3U + round % 4U);
		lbitwriter_init(&stream);
		build_image(&stream, &image, expected_pixels, NULL);
		size = lbitwriter_size(&stream);
		for (flips = 1U + round % 3U; flips > 0U; flips--) {
			bit = (unsigned)(check_random(&test_state) % (size * 8U));
			stream.bytes[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
		}
		if (round % 5U == 0U)
			size = (size_t)(check_random(&test_state) % (size + 1U));
		error = decode_stream(stream.bytes, size, 8, 8, 1, 1U << 16, pixels, NULL);
		CHECK(error == 0 || error == EINVAL || error == ENOMEM || error == ENOBUFS);
	}
	free(pixels);
}
