/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8l-codes.c: simple and normal code length codes written
 * with the lbitwriter from RFC 9649 3.7.2.1, the codes they must give
 * (worked out by hand, then read back with codewords the test assigns by
 * RFC 1951 3.2.2), the refusals, cut streams, groups of five and random
 * input.
 */

#include "check.h"
#include "lbitwriter.h"
#include "vp8l-codes.h"

#include <errno.h>
#include <string.h>

/* The marker the tests write after a code, to show the reader used exactly the code's bits. */
#define TEST_MARKER		0xa5U

/* The most tokens (code length symbols with their extra bits) a test writes. */
#define TEST_TOKENS_MAX		2400U

/* One code length symbol of a normal code and the value of its extra bits (16: 2 bits, 17: 3, 18: 7). */
struct test_token {
	uint8_t symbol;
	uint8_t extra;
};

/* The stream a test writes the codes into, and the stream of symbols it reads back with them. */
static struct lbitwriter stream;
static struct lbitwriter symbol_stream;

/* The codewords the test assigns to the symbols of one code. */
static uint32_t test_codes[WEBPC_PREFIX_ALPHABET_MAX];

/* The lengths a test expects the code it reads to have, and a list of tokens it builds. */
static uint8_t expected[WEBPC_PREFIX_ALPHABET_MAX];
static struct test_token tokens[TEST_TOKENS_MAX];

/* The order the code length code's lengths are written in (RFC 9649 3.7.2.1.2). */
static const uint8_t test_order[19] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

static void test_alphabet_range(void);
static void test_simple(void);
static void test_simple_refused(void);
static void test_normal_lengths(void);
static void test_normal_repeats(void);
static void test_normal_max_symbol(void);
static void test_normal_refused(void);
static void test_pool_limit(void);
static void test_group(void);
static void test_group_failure(void);
static void test_random(void);
static void expect_two_lengths(unsigned alphabet_size, unsigned short_length, unsigned short_count);
static void write_two_length_code(unsigned alphabet_size, unsigned short_length, unsigned short_count);
static void write_group(unsigned cache_bits, uint8_t group_lengths[WEBPC_VP8L_GROUP_CODES][WEBPC_PREFIX_ALPHABET_MAX],
	unsigned *alphabet_sizes);
static void assign_codes(const uint8_t *lengths, unsigned count);
static unsigned count_present(const uint8_t *lengths, unsigned count);
static void put_simple(unsigned symbol_count, unsigned first_8bits, unsigned first, unsigned second);
static void put_normal(const uint8_t *length_code_lengths, unsigned num_code_lengths, unsigned max_nbits,
	unsigned max_symbol, const struct test_token *list, unsigned count);
static void put_padding(void);
static void set_expected(unsigned alphabet_size, unsigned fill, unsigned first, unsigned last, unsigned value);
static void verify_code(const struct webpc_prefix_pool *pool, const struct webpc_prefix_code *code,
	const uint8_t *lengths, unsigned alphabet_size);
static void check_stream_valid(unsigned alphabet_size, const uint8_t *lengths);
static void check_stream_error(unsigned alphabet_size, int error_expected);
static void fill_tokens(unsigned first, unsigned count, unsigned symbol, unsigned extra);

/*
 * Runs the tests of vp8l-codes.c.
 */
int
main(
	void)
{
	int status;

	/* The alphabet sizes, then the simple code length code. */
	test_alphabet_range();
	test_simple();
	test_simple_refused();

	/* The normal code length code. */
	test_normal_lengths();
	test_normal_repeats();
	test_normal_max_symbol();
	test_normal_refused();
	test_pool_limit();

	/* Groups and random input. */
	test_group();
	test_group_failure();
	test_random();

	/* The summary line. */
	status = check_finish("vp8l_codes");
	return status;
}

/*
 * Assigns the canonical codes of lengths into test_codes the way RFC 1951
 * 3.2.2 does: the codes of a length are consecutive in the order of the
 * symbols, and the first code of the next length follows them, shifted left.
 */
static void
assign_codes(
	const uint8_t *lengths,
	unsigned count)
{
	unsigned length;
	unsigned symbol;
	unsigned longest;
	uint32_t code;

	/* The longest length present. */
	longest = 0;
	for (symbol = 0; symbol < count; symbol++) {
		if (lengths[symbol] > longest)
			longest = lengths[symbol];
	}

	/* Walks the lengths upward, handing out consecutive codes, shifting at each step. */
	code = 0;
	for (length = 1; length <= longest; length++) {
		for (symbol = 0; symbol < count; symbol++) {
			if (lengths[symbol] != length)
				continue;
			test_codes[symbol] = code;
			code++;
		}
		code <<= 1;
	}
}

/* Counts the symbols with a non-zero length. */
static unsigned
count_present(
	const uint8_t *lengths,
	unsigned count)
{
	unsigned present;
	unsigned symbol;

	/* One for each non-zero length. */
	present = 0;
	for (symbol = 0; symbol < count; symbol++) {
		if (lengths[symbol] != 0U)
			present++;
	}

	/* The count. */
	return present;
}

/*
 * Appends a simple code length code (RFC 9649 3.7.2.1.1) to the stream:
 * symbol_count (1 or 2) symbols, the first of 1 or 8 bits, the second of 8.
 */
static void
put_simple(
	unsigned symbol_count,
	unsigned first_8bits,
	unsigned first,
	unsigned second)
{
	/* is_simple, num_symbols - 1, is_first_8bits. */
	lbitwriter_put(&stream, 1, 1);
	lbitwriter_put(&stream, symbol_count - 1U, 1);
	lbitwriter_put(&stream, first_8bits, 1);

	/* The first symbol, then the second when there is one. */
	lbitwriter_put(&stream, first, first_8bits != 0U ? 8U : 1U);
	if (symbol_count == 2U)
		lbitwriter_put(&stream, second, 8);
}

/*
 * Appends a normal code length code (RFC 9649 3.7.2.1.2) to the stream: the
 * lengths of the code length code (indexed by symbol) as num_code_lengths of
 * them in kCodeLengthCodeOrder, max_symbol written in max_nbits bits (0 for
 * the form without max_symbol), then the tokens.
 */
static void
put_normal(
	const uint8_t *length_code_lengths,
	unsigned num_code_lengths,
	unsigned max_nbits,
	unsigned max_symbol,
	const struct test_token *list,
	unsigned count)
{
	unsigned index;
	unsigned symbol;
	unsigned present;
	unsigned extra_bits;

	/* Not simple, then the number of code length code lengths less 4, and the lengths. */
	lbitwriter_put(&stream, 0, 1);
	lbitwriter_put(&stream, num_code_lengths - 4U, 4);
	for (index = 0; index < num_code_lengths; index++)
		lbitwriter_put(&stream, length_code_lengths[test_order[index]], 3);

	/* The max_symbol field: absent, or length_nbits and the value less 2. */
	if (max_nbits == 0U) {
		lbitwriter_put(&stream, 0, 1);
	} else {
		lbitwriter_put(&stream, 1, 1);
		lbitwriter_put(&stream, (max_nbits - 2U) / 2U, 3);
		lbitwriter_put(&stream, max_symbol - 2U, max_nbits);
	}

	/* The code length code's codewords; a code of one symbol writes no bits. */
	assign_codes(length_code_lengths, 19);
	present = count_present(length_code_lengths, 19);

	/* Each token: its codeword, then its extra bits when it is a repeat. */
	for (index = 0; index < count; index++) {
		symbol = list[index].symbol;
		if (present > 1U)
			lbitwriter_put_code(&stream, test_codes[symbol], length_code_lengths[symbol]);
		if (symbol < 16U)
			continue;
		extra_bits = 2U;
		if (symbol == 17U)
			extra_bits = 3U;
		if (symbol == 18U)
			extra_bits = 7U;
		lbitwriter_put(&stream, list[index].extra, extra_bits);
	}
}

/* Appends 32 zero bits, so a stream that is valid up to here is not also cut short. */
static void
put_padding(void)
{
	/* Zeros after the code. */
	lbitwriter_put(&stream, 0, 24);
	lbitwriter_put(&stream, 0, 8);
}

/*
 * Sets expected to alphabet_size lengths: all 0, then value for the symbols
 * first..last (when fill is non-zero).
 */
static void
set_expected(
	unsigned alphabet_size,
	unsigned fill,
	unsigned first,
	unsigned last,
	unsigned value)
{
	unsigned symbol;

	/* No symbol present. */
	memset(expected, 0, sizeof(expected));

	/* The range, when there is one. */
	if (fill == 0U)
		return;
	for (symbol = first; symbol <= last && symbol < alphabet_size; symbol++)
		expected[symbol] = (uint8_t)value;
}

/*
 * Writes every present symbol of lengths with the test's codewords and a
 * marker after each, and reads them back with code.
 */
static void
verify_code(
	const struct webpc_prefix_pool *pool,
	const struct webpc_prefix_code *code,
	const uint8_t *lengths,
	unsigned alphabet_size)
{
	struct webpc_lbits reader;
	unsigned present;
	unsigned symbol;
	unsigned read_symbol;
	unsigned marker;
	unsigned index;

	/* The test's own codewords. */
	assign_codes(lengths, alphabet_size);
	present = count_present(lengths, alphabet_size);

	/* A code of one symbol reads no bits; any other writes each codeword. */
	lbitwriter_init(&symbol_stream);
	index = 0;
	for (symbol = 0; symbol < alphabet_size; symbol++) {
		if (lengths[symbol] == 0U)
			continue;
		if (present > 1U)
			lbitwriter_put_code(&symbol_stream, test_codes[symbol], lengths[symbol]);
		lbitwriter_put(&symbol_stream, index & 31U, 5);
		index++;
	}
	CHECK(symbol_stream.overflow == 0);

	/* A one-symbol code is built with no table. */
	if (present == 1U)
		CHECK(code->root_bits == 0U);

	/* Reads them back, each symbol and the marker after it. */
	webpc_lbits_init(&reader, symbol_stream.bytes, lbitwriter_size(&symbol_stream));
	index = 0;
	for (symbol = 0; symbol < alphabet_size; symbol++) {
		if (lengths[symbol] == 0U)
			continue;
		read_symbol = webpc_prefix_read(pool, code, &reader);
		CHECK(read_symbol == symbol);
		marker = webpc_lbits_read(&reader, 5);
		CHECK(marker == (index & 31U));
		index++;
	}
	CHECK(webpc_lbits_ended(&reader) == 0);
}

/*
 * Checks that the stream written so far is the code of expected lengths for
 * alphabet_size symbols: read whole it gives that code and uses exactly its
 * bits and its tables, and cut at any byte before its end it is ENOBUFS and
 * leaves the pool as it was.
 */
static void
check_stream_valid(
	unsigned alphabet_size,
	const uint8_t *lengths)
{
	struct webpc_prefix_pool pool;
	struct webpc_prefix_pool reference;
	struct webpc_prefix_code code;
	struct webpc_prefix_code reference_code;
	struct webpc_prefix_code first_code;
	struct webpc_lbits reader;
	uint8_t first_lengths[2];
	size_t code_bytes;
	size_t cut;
	size_t before;
	unsigned marker;
	int error;

	/* A pool that already holds a code, so a restored count is not just 0. */
	webpc_prefix_pool_init(&pool, WEBPC_VP8L_TABLE_ENTRIES_MAX);
	first_lengths[0] = 1;
	first_lengths[1] = 1;
	error = webpc_prefix_build(&pool, first_lengths, 2, &first_code);
	CHECK(error == 0);
	before = pool.count;

	/* The bytes the code takes. */
	CHECK(stream.overflow == 0);
	code_bytes = lbitwriter_size(&stream);

	/* Cut short at every byte before the end: the data ends inside the code. */
	for (cut = 0; cut < code_bytes; cut++) {
		webpc_lbits_init(&reader, stream.bytes, cut);
		error = webpc_vp8l_read_code(&reader, &pool, alphabet_size, &code);
		CHECK(error == ENOBUFS);
		CHECK(pool.count == before);
	}

	/* The marker after the code: the reader must stop right before it. */
	lbitwriter_put(&stream, TEST_MARKER, 8);
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	error = webpc_vp8l_read_code(&reader, &pool, alphabet_size, &code);
	CHECK(error == 0);
	if (error == 0) {
		marker = webpc_lbits_read(&reader, 8);
		CHECK(marker == TEST_MARKER);
		CHECK(webpc_lbits_ended(&reader) == 0);
		verify_code(&pool, &code, lengths, alphabet_size);

		/* The pool holds the code's table and nothing of the code length code. */
		webpc_prefix_pool_init(&reference, WEBPC_VP8L_TABLE_ENTRIES_MAX);
		error = webpc_prefix_build(&reference, lengths, alphabet_size, &reference_code);
		CHECK(error == 0);
		CHECK(pool.count == before + reference.count);
		CHECK(code.root_bits == reference_code.root_bits);
		webpc_prefix_pool_release(&reference);
	}
	webpc_prefix_pool_release(&pool);
}

/*
 * Checks that the stream written so far (with padding after it) is refused
 * with error_expected and leaves the pool as it was.
 */
static void
check_stream_error(
	unsigned alphabet_size,
	int error_expected)
{
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code code;
	struct webpc_prefix_code first_code;
	struct webpc_lbits reader;
	uint8_t first_lengths[2];
	size_t before;
	int error;

	/* A pool that already holds a code. */
	webpc_prefix_pool_init(&pool, WEBPC_VP8L_TABLE_ENTRIES_MAX);
	first_lengths[0] = 1;
	first_lengths[1] = 1;
	error = webpc_prefix_build(&pool, first_lengths, 2, &first_code);
	CHECK(error == 0);
	before = pool.count;

	/* Reads it: the error is the one expected and the pool is as it was. */
	put_padding();
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	error = webpc_vp8l_read_code(&reader, &pool, alphabet_size, &code);
	CHECK(error == error_expected);
	CHECK(pool.count == before);
	webpc_prefix_pool_release(&pool);
}

/* Sets count tokens from index first to symbol with the extra bits value. */
static void
fill_tokens(
	unsigned first,
	unsigned count,
	unsigned symbol,
	unsigned extra)
{
	unsigned index;

	/* The same token repeated. */
	for (index = 0; index < count; index++) {
		tokens[first + index].symbol = (uint8_t)symbol;
		tokens[first + index].extra = (uint8_t)extra;
	}
}

/* An alphabet of no symbol, or above the largest, is refused before anything is read. */
static void
test_alphabet_range(void)
{
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code code;
	struct webpc_lbits reader;
	uint8_t bytes[4] = { 0, 0, 0, 0 };

	/* Zero and one past the largest alphabet. */
	webpc_prefix_pool_init(&pool, 1000);
	webpc_lbits_init(&reader, bytes, sizeof(bytes));
	CHECK(webpc_vp8l_read_code(&reader, &pool, 0, &code) == EINVAL);
	CHECK(webpc_vp8l_read_code(&reader, &pool, WEBPC_PREFIX_ALPHABET_MAX + 1U, &code) == EINVAL);
	CHECK(pool.count == 0U);
	webpc_prefix_pool_release(&pool);
}

/* The simple code length code: one or two symbols, 1-bit and 8-bit forms, equal symbols. */
static void
test_simple(void)
{
	static const unsigned single_8[4] = { 0, 3, 200, 255 };
	unsigned index;
	unsigned symbol;

	/* One symbol in the 1-bit form: 0 and 1. */
	for (symbol = 0; symbol < 2U; symbol++) {
		lbitwriter_init(&stream);
		put_simple(1, 0, symbol, 0);
		set_expected(280, 1, symbol, symbol, 1);
		check_stream_valid(280, expected);
	}

	/* One symbol in the 8-bit form, with a literal alphabet and the green one. */
	for (index = 0; index < 4U; index++) {
		lbitwriter_init(&stream);
		put_simple(1, 1, single_8[index], 0);
		set_expected(256, 1, single_8[index], single_8[index], 1);
		check_stream_valid(256, expected);
	}
	lbitwriter_init(&stream);
	put_simple(1, 1, 255, 0);
	set_expected(280, 1, 255, 255, 1);
	check_stream_valid(280, expected);

	/* Two symbols, 8 bits each: the lower symbol gets the code 0, whatever the order written. */
	lbitwriter_init(&stream);
	put_simple(2, 1, 3, 200);
	set_expected(256, 0, 0, 0, 0);
	expected[3] = 1;
	expected[200] = 1;
	check_stream_valid(256, expected);
	lbitwriter_init(&stream);
	put_simple(2, 1, 200, 3);
	check_stream_valid(256, expected);

	/* Two symbols, the first in 1 bit: {0, 1} and {1, 0}. */
	lbitwriter_init(&stream);
	put_simple(2, 0, 0, 1);
	set_expected(40, 1, 0, 1, 1);
	check_stream_valid(40, expected);
	lbitwriter_init(&stream);
	put_simple(2, 0, 1, 0);
	check_stream_valid(40, expected);

	/* Two symbols with the first of 1 bit and the second large. */
	lbitwriter_init(&stream);
	put_simple(2, 0, 1, 255);
	set_expected(256, 0, 0, 0, 0);
	expected[1] = 1;
	expected[255] = 1;
	check_stream_valid(256, expected);

	/* Two equal symbols are the code of that one symbol. */
	lbitwriter_init(&stream);
	put_simple(2, 1, 7, 7);
	set_expected(256, 1, 7, 7, 1);
	check_stream_valid(256, expected);
	lbitwriter_init(&stream);
	put_simple(2, 0, 1, 1);
	set_expected(256, 1, 1, 1, 1);
	check_stream_valid(256, expected);
	lbitwriter_init(&stream);
	put_simple(2, 1, 0, 0);
	set_expected(256, 1, 0, 0, 1);
	check_stream_valid(256, expected);

	/* An alphabet of 1 symbol: symbol 0 is the whole code. */
	lbitwriter_init(&stream);
	put_simple(1, 0, 0, 0);
	set_expected(1, 1, 0, 0, 1);
	check_stream_valid(1, expected);

	/* The highest symbol of the distance alphabet (39) is in; the next two symbols are out. */
	lbitwriter_init(&stream);
	put_simple(2, 1, 39, 38);
	set_expected(40, 1, 38, 39, 1);
	check_stream_valid(40, expected);
	lbitwriter_init(&stream);
	put_simple(1, 1, 39, 0);
	set_expected(40, 1, 39, 39, 1);
	check_stream_valid(40, expected);

	/* A symbol of the distance alphabet's 40 or more is left out: {3, 200} is the code of 3 alone. */
	lbitwriter_init(&stream);
	put_simple(2, 1, 3, 200);
	set_expected(40, 1, 3, 3, 1);
	check_stream_valid(40, expected);
	lbitwriter_init(&stream);
	put_simple(2, 1, 200, 3);
	check_stream_valid(40, expected);
	lbitwriter_init(&stream);
	put_simple(2, 1, 39, 40);
	set_expected(40, 1, 39, 39, 1);
	check_stream_valid(40, expected);
}

/* The simple codes with no symbol left in the alphabet. */
static void
test_simple_refused(void)
{
	/* Two symbols, both out of the distance alphabet. */
	lbitwriter_init(&stream);
	put_simple(2, 1, 40, 41);
	check_stream_error(40, EINVAL);
	lbitwriter_init(&stream);
	put_simple(2, 1, 200, 255);
	check_stream_error(40, EINVAL);

	/* One symbol out of the alphabet. */
	lbitwriter_init(&stream);
	put_simple(1, 1, 40, 0);
	check_stream_error(40, EINVAL);

	/* An alphabet of one symbol and the symbol 1. */
	lbitwriter_init(&stream);
	put_simple(1, 0, 1, 0);
	check_stream_error(1, EINVAL);
	lbitwriter_init(&stream);
	put_simple(2, 0, 1, 1);
	check_stream_error(1, EINVAL);
}

/*
 * The normal code length code with a code length code of one symbol, 16
 * repeating the default 8, and every literal length 0 to 15.
 */
static void
test_normal_lengths(void)
{
	uint8_t cl[19];
	unsigned index;

	/* A code length code of the one symbol 8: 256 tokens read no bits and give 256 lengths of 8. */
	memset(cl, 0, sizeof(cl));
	cl[8] = 1;
	fill_tokens(0, 256, 8, 0);
	lbitwriter_init(&stream);
	put_normal(cl, 12, 0, 0, tokens, 256);
	set_expected(256, 1, 0, 255, 8);
	check_stream_valid(256, expected);
	lbitwriter_init(&stream);
	put_normal(cl, 19, 0, 0, tokens, 256);
	check_stream_valid(256, expected);

	/*
	 * A code length code of the one symbol 16, whose first repeat is of 8:
	 * 6 + 41 * 6 + 4 = 256 lengths of 8.
	 */
	memset(cl, 0, sizeof(cl));
	cl[16] = 1;
	fill_tokens(0, 42, 16, 3);
	tokens[42].symbol = 16;
	tokens[42].extra = 1;
	lbitwriter_init(&stream);
	put_normal(cl, 9, 0, 0, tokens, 43);
	check_stream_valid(256, expected);

	/*
	 * Every literal length: lengths 0, 1, 2, ..., 15, 15 (17 symbols, Kraft
	 * sum 1) written with a code length code of 16 symbols of 4 bits.
	 */
	memset(cl, 0, sizeof(cl));
	for (index = 0; index < 16U; index++)
		cl[index] = 4;
	for (index = 0; index < 16U; index++) {
		tokens[index].symbol = (uint8_t)index;
		tokens[index].extra = 0;
	}
	tokens[16].symbol = 15;
	tokens[16].extra = 0;
	set_expected(17, 0, 0, 0, 0);
	for (index = 0; index < 16U; index++)
		expected[index] = (uint8_t)index;
	expected[16] = 15;
	lbitwriter_init(&stream);
	put_normal(cl, 19, 0, 0, tokens, 17);
	check_stream_valid(17, expected);

	/* Lengths 0, 0, 3, 0: one present symbol of any length is the code of that symbol. */
	memset(cl, 0, sizeof(cl));
	cl[0] = 1;
	cl[3] = 1;
	tokens[0].symbol = 0;
	tokens[1].symbol = 0;
	tokens[2].symbol = 3;
	tokens[3].symbol = 0;
	set_expected(4, 1, 2, 2, 3);
	lbitwriter_init(&stream);
	put_normal(cl, 7, 0, 0, tokens, 4);
	check_stream_valid(4, expected);
}

/* The repeats 16, 17 and 18 and the previous length they use. */
static void
test_normal_repeats(void)
{
	uint8_t cl[19];

	/*
	 * Tokens 5, then 16 five times (6 repeats of 5 each), then 5: 32 lengths
	 * of 5 (a complete code), with a code length code of 5 and 16, one bit
	 * each.
	 */
	memset(cl, 0, sizeof(cl));
	cl[5] = 1;
	cl[16] = 1;
	tokens[0].symbol = 5;
	tokens[0].extra = 0;
	fill_tokens(1, 5, 16, 3);
	tokens[6].symbol = 5;
	tokens[6].extra = 0;
	lbitwriter_init(&stream);
	put_normal(cl, 9, 0, 0, tokens, 7);
	set_expected(32, 1, 0, 31, 5);
	check_stream_valid(32, expected);

	/*
	 * Tokens 2, 17 (3 zeros), 16 (3 repeats): a 17 does not change the
	 * length a 16 repeats, so the lengths are 2 0 0 0 2 2 2 (complete).  The
	 * code length code has 2 (1 bit), 16 and 17 (2 bits).
	 */
	memset(cl, 0, sizeof(cl));
	cl[2] = 1;
	cl[16] = 2;
	cl[17] = 2;
	tokens[0].symbol = 2;
	tokens[0].extra = 0;
	tokens[1].symbol = 17;
	tokens[1].extra = 0;
	tokens[2].symbol = 16;
	tokens[2].extra = 0;
	set_expected(7, 0, 0, 0, 0);
	expected[0] = 2;
	expected[4] = 2;
	expected[5] = 2;
	expected[6] = 2;
	lbitwriter_init(&stream);
	put_normal(cl, 9, 0, 0, tokens, 3);
	check_stream_valid(7, expected);
	lbitwriter_init(&stream);
	put_normal(cl, 19, 0, 0, tokens, 3);
	check_stream_valid(7, expected);

	/* Tokens 1, 18 (27: 38 zeros), 1: lengths 1, 38 zeros, 1 for the 40 symbols of the distance alphabet. */
	memset(cl, 0, sizeof(cl));
	cl[1] = 1;
	cl[18] = 1;
	tokens[0].symbol = 1;
	tokens[0].extra = 0;
	tokens[1].symbol = 18;
	tokens[1].extra = 27;
	tokens[2].symbol = 1;
	tokens[2].extra = 0;
	set_expected(40, 0, 0, 0, 0);
	expected[0] = 1;
	expected[39] = 1;
	lbitwriter_init(&stream);
	put_normal(cl, 4, 0, 0, tokens, 3);
	check_stream_valid(40, expected);

	/* The longest 18 (127 extra: 138 zeros) and the longest 17 (7 extra: 10 zeros). */
	tokens[1].extra = 127;
	set_expected(140, 0, 0, 0, 0);
	expected[0] = 1;
	expected[139] = 1;
	lbitwriter_init(&stream);
	put_normal(cl, 4, 0, 0, tokens, 3);
	check_stream_valid(140, expected);
	memset(cl, 0, sizeof(cl));
	cl[1] = 1;
	cl[17] = 1;
	tokens[1].symbol = 17;
	tokens[1].extra = 7;
	set_expected(12, 0, 0, 0, 0);
	expected[0] = 1;
	expected[11] = 1;
	lbitwriter_init(&stream);
	put_normal(cl, 4, 0, 0, tokens, 3);
	check_stream_valid(12, expected);
}

/* The max_symbol field: the forms of length_nbits, and what it counts. */
static void
test_normal_max_symbol(void)
{
	uint8_t cl[19];

	/*
	 * Seven tokens (5, then five 16, then 5) fill 32 lengths.  max_symbol 7
	 * counts tokens, so all seven are read; counted in lengths it would be
	 * read as 7 lengths and the code would not be complete.
	 */
	memset(cl, 0, sizeof(cl));
	cl[5] = 1;
	cl[16] = 1;
	tokens[0].symbol = 5;
	tokens[0].extra = 0;
	fill_tokens(1, 5, 16, 3);
	tokens[6].symbol = 5;
	tokens[6].extra = 0;
	set_expected(32, 1, 0, 31, 5);
	lbitwriter_init(&stream);
	put_normal(cl, 9, 4, 7, tokens, 7);
	check_stream_valid(32, expected);

	/* The same 7 tokens in a 40-symbol alphabet: the 8 lengths not read are 0 and the code is still complete. */
	lbitwriter_init(&stream);
	put_normal(cl, 9, 2 + 2 * 3, 7, tokens, 7);
	set_expected(40, 1, 0, 31, 5);
	check_stream_valid(40, expected);

	/* One token fewer than the code needs: 6 tokens give 31 lengths, an incomplete code. */
	lbitwriter_init(&stream);
	put_normal(cl, 9, 4, 6, tokens, 6);
	check_stream_error(32, EINVAL);

	/* max_symbol 3 with length_nbits 2: lengths 1, 38 zeros, 1. */
	memset(cl, 0, sizeof(cl));
	cl[1] = 1;
	cl[18] = 1;
	tokens[0].symbol = 1;
	tokens[0].extra = 0;
	tokens[1].symbol = 18;
	tokens[1].extra = 27;
	tokens[2].symbol = 1;
	tokens[2].extra = 0;
	set_expected(40, 0, 0, 0, 0);
	expected[0] = 1;
	expected[39] = 1;
	lbitwriter_init(&stream);
	put_normal(cl, 4, 2, 3, tokens, 3);
	check_stream_valid(40, expected);

	/* The smallest max_symbol, 2, with two tokens: lengths 1 and 1, the other symbols 0. */
	memset(cl, 0, sizeof(cl));
	cl[1] = 1;
	cl[2] = 1;
	tokens[0].symbol = 1;
	tokens[1].symbol = 1;
	set_expected(40, 1, 0, 1, 1);
	lbitwriter_init(&stream);
	put_normal(cl, 5, 2, 2, tokens, 2);
	check_stream_valid(40, expected);

	/* length_nbits 16 with max_symbol 256 for an alphabet of 256 (all lengths 8, one-symbol code length code). */
	memset(cl, 0, sizeof(cl));
	cl[8] = 1;
	fill_tokens(0, 256, 8, 0);
	set_expected(256, 1, 0, 255, 8);
	lbitwriter_init(&stream);
	put_normal(cl, 12, 16, 256, tokens, 256);
	check_stream_valid(256, expected);

	/* One more than the alphabet is refused, with 16 bits and with 6. */
	lbitwriter_init(&stream);
	put_normal(cl, 12, 16, 257, tokens, 256);
	check_stream_error(256, EINVAL);
	lbitwriter_init(&stream);
	put_normal(cl, 12, 6, 41, tokens, 41);
	check_stream_error(40, EINVAL);

	/* The alphabet's size itself is allowed: 40 tokens of 8 do not make a complete code, but are read as 40. */
	lbitwriter_init(&stream);
	put_normal(cl, 12, 6, 40, tokens, 40);
	check_stream_error(40, EINVAL);
}

/* The refused normal codes. */
static void
test_normal_refused(void)
{
	uint8_t cl[19];

	/* A repeat past the end of the alphabet: 2, 2, then three zeros in an alphabet of 4. */
	memset(cl, 0, sizeof(cl));
	cl[2] = 1;
	cl[17] = 1;
	tokens[0].symbol = 2;
	tokens[1].symbol = 2;
	tokens[2].symbol = 17;
	tokens[2].extra = 0;
	lbitwriter_init(&stream);
	put_normal(cl, 5, 0, 0, tokens, 3);
	check_stream_error(4, EINVAL);

	/* A 16 past the end: 8 then 6 repeats in an alphabet of 5. */
	memset(cl, 0, sizeof(cl));
	cl[8] = 1;
	cl[16] = 1;
	tokens[0].symbol = 8;
	tokens[1].symbol = 16;
	tokens[1].extra = 3;
	lbitwriter_init(&stream);
	put_normal(cl, 12, 0, 0, tokens, 2);
	check_stream_error(5, EINVAL);

	/* An incomplete code: lengths 2, 2, 2 and a 0. */
	memset(cl, 0, sizeof(cl));
	cl[2] = 1;
	fill_tokens(0, 3, 2, 0);
	lbitwriter_init(&stream);
	put_normal(cl, 5, 2, 3, tokens, 3);
	check_stream_error(4, EINVAL);

	/* An over-subscribed code: lengths 1, 1, 1. */
	memset(cl, 0, sizeof(cl));
	cl[1] = 1;
	fill_tokens(0, 3, 1, 0);
	lbitwriter_init(&stream);
	put_normal(cl, 5, 0, 0, tokens, 3);
	check_stream_error(3, EINVAL);

	/* No length at all: every length 0. */
	memset(cl, 0, sizeof(cl));
	cl[0] = 1;
	fill_tokens(0, 4, 0, 0);
	lbitwriter_init(&stream);
	put_normal(cl, 4, 0, 0, tokens, 4);
	check_stream_error(4, EINVAL);

	/* A code length code that is incomplete: lengths 1 and 2 for two symbols. */
	memset(cl, 0, sizeof(cl));
	cl[1] = 1;
	cl[2] = 2;
	fill_tokens(0, 4, 1, 0);
	lbitwriter_init(&stream);
	put_normal(cl, 5, 0, 0, tokens, 4);
	check_stream_error(4, EINVAL);

	/* A code length code with no symbol (all 3-bit lengths 0). */
	memset(cl, 0, sizeof(cl));
	lbitwriter_init(&stream);
	put_normal(cl, 4, 0, 0, tokens, 0);
	check_stream_error(4, EINVAL);

	/* A code length code that is over-subscribed: three symbols of length 1. */
	memset(cl, 0, sizeof(cl));
	cl[0] = 1;
	cl[1] = 1;
	cl[2] = 1;
	lbitwriter_init(&stream);
	put_normal(cl, 5, 0, 0, tokens, 0);
	check_stream_error(4, EINVAL);
}

/* A pool whose limit is too small for the table is ENOMEM and is left as it was. */
static void
test_pool_limit(void)
{
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code code;
	struct webpc_lbits reader;
	uint8_t cl[19];
	int error;

	/* A two-symbol simple code needs 2 entries, and the pool allows 1. */
	lbitwriter_init(&stream);
	put_simple(2, 1, 3, 200);
	put_padding();
	webpc_prefix_pool_init(&pool, 1);
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	error = webpc_vp8l_read_code(&reader, &pool, 256, &code);
	CHECK(error == ENOMEM);
	CHECK(pool.count == 0U);
	webpc_prefix_pool_release(&pool);

	/* A normal code of 256 symbols needs 256 entries, and the pool allows 20. */
	memset(cl, 0, sizeof(cl));
	cl[8] = 1;
	fill_tokens(0, 256, 8, 0);
	lbitwriter_init(&stream);
	put_normal(cl, 12, 0, 0, tokens, 256);
	put_padding();
	webpc_prefix_pool_init(&pool, 20);
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	error = webpc_vp8l_read_code(&reader, &pool, 256, &code);
	CHECK(error == ENOMEM);
	CHECK(pool.count == 0U);
	webpc_prefix_pool_release(&pool);

	/* A code length code of two symbols needs 2 entries, and the pool allows 1. */
	memset(cl, 0, sizeof(cl));
	cl[0] = 1;
	cl[1] = 1;
	tokens[0].symbol = 1;
	tokens[1].symbol = 1;
	lbitwriter_init(&stream);
	put_normal(cl, 5, 0, 0, tokens, 2);
	put_padding();
	webpc_prefix_pool_init(&pool, 1);
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	error = webpc_vp8l_read_code(&reader, &pool, 2, &code);
	CHECK(error == ENOMEM);
	CHECK(pool.count == 0U);
	webpc_prefix_pool_release(&pool);
}

/*
 * Sets expected to alphabet_size lengths: short_count symbols of
 * short_length, then the rest of short_length + 1.
 */
static void
expect_two_lengths(
	unsigned alphabet_size,
	unsigned short_length,
	unsigned short_count)
{
	unsigned symbol;

	/* The short lengths first, then the long ones. */
	memset(expected, 0, sizeof(expected));
	for (symbol = 0; symbol < alphabet_size; symbol++) {
		if (symbol < short_count)
			expected[symbol] = (uint8_t)short_length;
		else
			expected[symbol] = (uint8_t)(short_length + 1U);
	}
}

/*
 * Appends a normal code of alphabet_size symbols with short_count lengths
 * of short_length and the rest of short_length + 1 (a complete code when
 * the counts are chosen so), with a code length code of just those two
 * symbols, and sets expected to its lengths.
 */
static void
write_two_length_code(
	unsigned alphabet_size,
	unsigned short_length,
	unsigned short_count)
{
	uint8_t cl[19];

	/* The code length code has the two lengths, 1 bit each. */
	memset(cl, 0, sizeof(cl));
	cl[short_length] = 1;
	cl[short_length + 1U] = 1;

	/* One token per symbol. */
	fill_tokens(0, short_count, short_length, 0);
	fill_tokens(short_count, alphabet_size - short_count, short_length + 1U, 0);
	put_normal(cl, 19, 0, 0, tokens, alphabet_size);
	expect_two_lengths(alphabet_size, short_length, short_count);
}

/*
 * Appends one group of five codes for cache_bits to the stream and records
 * each code's lengths and alphabet: green a normal code with two lengths, red
 * a simple code of two symbols, blue a simple code of one symbol, alpha a
 * normal code of 256 lengths of 8, distance a simple code of two symbols.
 */
static void
write_group(
	unsigned cache_bits,
	uint8_t group_lengths[WEBPC_VP8L_GROUP_CODES][WEBPC_PREFIX_ALPHABET_MAX],
	unsigned *alphabet_sizes)
{
	uint8_t cl[19];
	unsigned green_size;

	/* The green alphabet and the complete code over it: with 8 and 9 bits, or 11 and 12 bits. */
	green_size = 280U + (cache_bits != 0U ? (1U << cache_bits) : 0U);
	alphabet_sizes[0] = green_size;
	alphabet_sizes[1] = 256;
	alphabet_sizes[2] = 256;
	alphabet_sizes[3] = 256;
	alphabet_sizes[4] = 40;
	if (green_size == 280U)
		write_two_length_code(green_size, 8, 232);
	else if (green_size == 282U)
		write_two_length_code(green_size, 8, 230);
	else
		write_two_length_code(green_size, 11, 1768);
	memcpy(group_lengths[0], expected, sizeof(expected));

	/* Red: symbols 10 and 20. */
	put_simple(2, 1, 10, 20);
	memset(group_lengths[1], 0, WEBPC_PREFIX_ALPHABET_MAX);
	group_lengths[1][10] = 1;
	group_lengths[1][20] = 1;

	/* Blue: the one symbol 33. */
	put_simple(1, 1, 33, 0);
	memset(group_lengths[2], 0, WEBPC_PREFIX_ALPHABET_MAX);
	group_lengths[2][33] = 1;

	/* Alpha: 256 lengths of 8 from a code length code of one symbol. */
	memset(cl, 0, sizeof(cl));
	cl[8] = 1;
	fill_tokens(0, 256, 8, 0);
	put_normal(cl, 12, 0, 0, tokens, 256);
	memset(group_lengths[3], 0, WEBPC_PREFIX_ALPHABET_MAX);
	memset(group_lengths[3], 8, 256);

	/* Distance: symbols 0 and 39. */
	put_simple(2, 1, 0, 39);
	memset(group_lengths[4], 0, WEBPC_PREFIX_ALPHABET_MAX);
	group_lengths[4][0] = 1;
	group_lengths[4][39] = 1;
}

/* The group for cache_bits 0, 1 and 11: the five codes in their order and the alphabets of each. */
static void
test_group(void)
{
	static uint8_t group_lengths[WEBPC_VP8L_GROUP_CODES][WEBPC_PREFIX_ALPHABET_MAX];
	static const unsigned cache_choices[3] = { 0, 1, 11 };
	unsigned alphabet_sizes[WEBPC_VP8L_GROUP_CODES];
	struct webpc_prefix_pool pool;
	struct webpc_vp8l_group group;
	struct webpc_lbits reader;
	size_t group_bytes;
	size_t cut;
	size_t before;
	unsigned choice;
	unsigned index;
	unsigned marker;
	int error;

	/* Each cache size. */
	for (choice = 0; choice < 3U; choice++) {
		lbitwriter_init(&stream);
		write_group(cache_choices[choice], group_lengths, alphabet_sizes);
		CHECK(stream.overflow == 0);
		group_bytes = lbitwriter_size(&stream);

		/* Starts a pool that already holds something. */
		webpc_prefix_pool_init(&pool, WEBPC_VP8L_TABLE_ENTRIES_MAX);
		before = 0;

		/* Cut short at each byte: ENOBUFS, and the pool as it was. */
		for (cut = 0; cut < group_bytes; cut++) {
			webpc_lbits_init(&reader, stream.bytes, cut);
			error = webpc_vp8l_read_group(&reader, &pool, cache_choices[choice], &group);
			CHECK(error == ENOBUFS);
			CHECK(pool.count == before);
		}

		/* The whole group, then the marker. */
		lbitwriter_put(&stream, TEST_MARKER, 8);
		webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
		error = webpc_vp8l_read_group(&reader, &pool, cache_choices[choice], &group);
		CHECK(error == 0);
		if (error == 0) {
			marker = webpc_lbits_read(&reader, 8);
			CHECK(marker == TEST_MARKER);
			CHECK(webpc_lbits_ended(&reader) == 0);

			/* Each of the five codes reads its own symbols, in the order green, red, blue, alpha, distance. */
			for (index = 0; index < WEBPC_VP8L_GROUP_CODES; index++)
				verify_code(&pool, &group.codes[index], group_lengths[index], alphabet_sizes[index]);
		}
		webpc_prefix_pool_release(&pool);
	}

	/* A cache of more than 11 bits is refused and reads nothing. */
	webpc_prefix_pool_init(&pool, 1000);
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	error = webpc_vp8l_read_group(&reader, &pool, 12, &group);
	CHECK(error == EINVAL);
	CHECK(pool.count == 0U);
	webpc_prefix_pool_release(&pool);
}

/* A group whose later code fails gives back the tables of the codes before it. */
static void
test_group_failure(void)
{
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code first_code;
	struct webpc_vp8l_group group;
	struct webpc_lbits reader;
	uint8_t first_lengths[2];
	uint8_t cl[19];
	size_t before;
	int error;

	/* A pool that already holds a code. */
	webpc_prefix_pool_init(&pool, WEBPC_VP8L_TABLE_ENTRIES_MAX);
	first_lengths[0] = 1;
	first_lengths[1] = 1;
	error = webpc_prefix_build(&pool, first_lengths, 2, &first_code);
	CHECK(error == 0);
	before = pool.count;

	/* Green, red and blue are good; alpha is an incomplete code (2, 2, 2 and zeros). */
	lbitwriter_init(&stream);
	put_simple(2, 1, 1, 2);
	put_simple(2, 1, 10, 20);
	put_simple(2, 1, 5, 6);
	memset(cl, 0, sizeof(cl));
	cl[2] = 1;
	fill_tokens(0, 3, 2, 0);
	put_normal(cl, 4, 2, 3, tokens, 3);
	put_simple(2, 1, 0, 39);
	put_padding();
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	error = webpc_vp8l_read_group(&reader, &pool, 0, &group);
	CHECK(error == EINVAL);
	CHECK(pool.count == before);
	webpc_prefix_pool_release(&pool);
}

/*
 * Random bytes and mutated groups: the answer is 0, EINVAL or ENOBUFS, a
 * failure leaves the pool as it was, and a code that was built reads
 * symbols of its alphabet.
 */
static void
test_random(void)
{
	static uint8_t group_lengths[WEBPC_VP8L_GROUP_CODES][WEBPC_PREFIX_ALPHABET_MAX];
	static const unsigned alphabets[6] = { 1, 2, 40, 256, 280, 2328 };
	static uint8_t bytes[1024];
	unsigned alphabet_sizes[WEBPC_VP8L_GROUP_CODES];
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code code;
	struct webpc_prefix_code first_code;
	struct webpc_vp8l_group group;
	struct webpc_lbits reader;
	unsigned long state;
	uint8_t first_lengths[2];
	size_t before;
	size_t length;
	size_t base_length;
	unsigned iteration;
	unsigned index;
	unsigned alphabet;
	unsigned symbol;
	unsigned cache_bits;
	int error;

	/* A pool that already holds a code, shared by the runs (each success is given back). */
	state = 12345UL;
	webpc_prefix_pool_init(&pool, WEBPC_VP8L_TABLE_ENTRIES_MAX);
	first_lengths[0] = 1;
	first_lengths[1] = 1;
	error = webpc_prefix_build(&pool, first_lengths, 2, &first_code);
	CHECK(error == 0);
	before = pool.count;

	/* Random bytes of random length, for each alphabet. */
	for (iteration = 0; iteration < 6000U; iteration++) {
		length = check_random(&state) % 48UL;
		for (index = 0; index < length; index++)
			bytes[index] = (uint8_t)(check_random(&state) & 0xffUL);
		alphabet = alphabets[iteration % 6U];
		webpc_lbits_init(&reader, bytes, length);
		error = webpc_vp8l_read_code(&reader, &pool, alphabet, &code);
		CHECK(error == 0 || error == EINVAL || error == ENOBUFS);
		if (error != 0) {
			CHECK(pool.count == before);
			continue;
		}

		/* A built code reads symbols of its alphabet, whatever the data. */
		for (index = 0; index < 32U; index++) {
			symbol = webpc_prefix_read(&pool, &code, &reader);
			CHECK(symbol < alphabet);
		}
		pool.count = before;
	}

	/* A valid group (cache 11) with bytes changed and cut. */
	lbitwriter_init(&stream);
	write_group(11, group_lengths, alphabet_sizes);
	base_length = lbitwriter_size(&stream);
	for (iteration = 0; iteration < 3000U; iteration++) {
		memcpy(bytes, stream.bytes, base_length);
		for (index = 0; index < 1U + iteration % 4U; index++)
			bytes[check_random(&state) % base_length] = (uint8_t)(check_random(&state) & 0xffUL);
		length = base_length;
		if (iteration % 3U == 0U)
			length = check_random(&state) % (base_length + 1U);
		cache_bits = 11U;
		if (iteration % 5U == 0U)
			cache_bits = iteration % 12U;
		webpc_lbits_init(&reader, bytes, length);
		error = webpc_vp8l_read_group(&reader, &pool, cache_bits, &group);
		CHECK(error == 0 || error == EINVAL || error == ENOBUFS);
		if (error != 0) {
			CHECK(pool.count == before);
			continue;
		}

		/* A group that was built reads symbols from every code. */
		for (index = 0; index < WEBPC_VP8L_GROUP_CODES; index++) {
			symbol = webpc_prefix_read(&pool, &group.codes[index], &reader);
			CHECK(symbol < 2328U);
		}
		pool.count = before;
	}
	webpc_prefix_pool_release(&pool);
}
