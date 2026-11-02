/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of prefix.c: codes worked out by hand (RFC 1951 3.2.2's
 * example), the refused codes, the second-level tables, the pool's limit,
 * and random complete codes built from frequencies, each read back
 * through streams the lbitwriter makes.
 */

#include "check.h"
#include "lbitwriter.h"
#include "prefix.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The stream the tests write, kept out of the stack (it is 64 KiB). */
static struct lbitwriter stream;

/* The canonical codes of a test's lengths, assigned by the test's own method. */
static uint32_t test_codes[WEBPC_PREFIX_ALPHABET_MAX];

static void test_rfc1951_example(void);
static void test_single_symbol(void);
static void test_refused(void);
static void test_staircase(void);
static void test_equal_lengths(void);
static void test_limit(void);
static void test_pool_growth(void);
static void test_end_of_data(void);
static void test_random_bytes(void);
static void test_random_huffman(void);
static int assign_codes(const uint8_t *lengths, unsigned count);
static unsigned long kraft_sum(const uint8_t *lengths, unsigned count);
static void round_trip(const uint8_t *lengths, unsigned count, unsigned long seed);
static unsigned huffman_lengths(const unsigned long *weights, unsigned count, uint8_t *lengths);
static int compare_leaves(const void *left, const void *right);

/*
 * Runs the tests of prefix.c.
 */
int
main(
	void)
{
	int status;

	/* The codes worked out by hand, then the refusals. */
	test_rfc1951_example();
	test_single_symbol();
	test_refused();

	/* Codes with second-level tables, the pool and the end of the data. */
	test_staircase();
	test_equal_lengths();
	test_limit();
	test_pool_growth();
	test_end_of_data();
	test_random_bytes();

	/* Random complete codes. */
	test_random_huffman();

	/* The summary line. */
	status = check_finish("prefix");
	return status;
}

/*
 * Assigns the canonical codes of lengths into test_codes: the codes of a
 * length are consecutive in the order of the symbols, and the first code of
 * a longer length is the next code after the shorter ones, shifted left
 * (RFC 1951 3.2.2).  Gives the longest length.
 */
static int
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

	/* The longest length. */
	return (int)longest;
}

/* Adds 2^-length of every present symbol in units of 2^-15. */
static unsigned long
kraft_sum(
	const uint8_t *lengths,
	unsigned count)
{
	unsigned long sum;
	unsigned symbol;

	/* Each present symbol adds 2^(15 - length). */
	sum = 0;
	for (symbol = 0; symbol < count; symbol++) {
		if (lengths[symbol] != 0U)
			sum += 1UL << (15U - lengths[symbol]);
	}

	/* The sum, 32768 for a complete code. */
	return sum;
}

/*
 * Builds a code from lengths and reads every symbol back from streams the
 * writer makes: all symbols in order, then random ones, each followed by a
 * 5-bit marker that shows the code used exactly its bits.
 */
static void
round_trip(
	const uint8_t *lengths,
	unsigned count,
	unsigned long seed)
{
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code code;
	struct webpc_lbits reader;
	unsigned long state;
	unsigned *symbols;
	unsigned total;
	unsigned index;
	unsigned present;
	unsigned symbol;
	unsigned marker;
	unsigned read_symbol;
	int error;

	/* The test's own view: a complete code's Kraft sum is 1, and it assigns the codes. */
	CHECK(kraft_sum(lengths, count) == 32768UL);
	assign_codes(lengths, count);

	/* Builds the code in a pool with room for any code. */
	webpc_prefix_pool_init(&pool, WEBPC_VP8L_TABLE_ENTRIES_MAX);
	error = webpc_prefix_build(&pool, lengths, count, &code);
	CHECK(error == 0);
	if (error != 0) {
		webpc_prefix_pool_release(&pool);
		return;
	}

	/* The symbols to write: every present one in order, then random present ones. */
	symbols = (unsigned *)malloc(sizeof(*symbols) * (count + 3000U));
	CHECK(symbols != NULL);
	if (symbols == NULL) {
		webpc_prefix_pool_release(&pool);
		return;
	}
	state = seed * 2654435761UL + 99UL;
	if ((state & 0xffffffffUL) == 0UL)
		state = 1UL;
	total = 0;
	present = 0;
	for (symbol = 0; symbol < count; symbol++) {
		if (lengths[symbol] != 0U) {
			symbols[total++] = symbol;
			present++;
		}
	}
	for (index = 0; index < 3000U; index++) {
		/* A random present symbol, found by counting to it. */
		read_symbol = (unsigned)(check_random(&state) % present);
		for (symbol = 0; symbol < count; symbol++) {
			if (lengths[symbol] == 0U)
				continue;
			if (read_symbol == 0U)
				break;
			read_symbol--;
		}
		symbols[total++] = symbol;
	}

	/* Writes each symbol's code (most significant bit first) and a marker. */
	lbitwriter_init(&stream);
	for (index = 0; index < total; index++) {
		lbitwriter_put_code(&stream, test_codes[symbols[index]], lengths[symbols[index]]);
		lbitwriter_put(&stream, index & 31U, 5);
	}
	CHECK(stream.overflow == 0);

	/* Reads them back: each symbol and the marker after it. */
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	for (index = 0; index < total; index++) {
		read_symbol = webpc_prefix_read(&pool, &code, &reader);
		CHECK(read_symbol == symbols[index]);
		marker = webpc_lbits_read(&reader, 5);
		CHECK(marker == (index & 31U));
	}
	CHECK(webpc_lbits_ended(&reader) == 0);

	/* The pool's count is the code's table and it is within the limit. */
	CHECK(pool.count <= pool.limit);
	free(symbols);
	webpc_prefix_pool_release(&pool);
}

/* RFC 1951 3.2.2: lengths (3,3,3,3,3,2,4,4) give A=010 B=011 C=100 D=101 E=110 F=00 G=1110 H=1111. */
static void
test_rfc1951_example(void)
{
	static const uint8_t lengths[8] = { 3, 3, 3, 3, 3, 2, 4, 4 };
	static const uint32_t expected[8] = { 2, 3, 4, 5, 6, 0, 14, 15 };
	static const uint8_t sparse[7] = { 0, 2, 0, 2, 2, 0, 2 };
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code code;
	struct webpc_lbits reader;
	unsigned symbol;
	int error;

	/* The test's own assignment agrees with the RFC's table. */
	assign_codes(lengths, 8);
	for (symbol = 0; symbol < 8U; symbol++)
		CHECK(test_codes[symbol] == expected[symbol]);

	/* Builds it: the root is as wide as the longest code, 4 bits, 16 entries, no second level. */
	webpc_prefix_pool_init(&pool, 1000);
	error = webpc_prefix_build(&pool, lengths, 8, &code);
	CHECK(error == 0);
	CHECK(code.root_bits == 4U);
	CHECK(pool.count == 16U);
	CHECK(code.offset == 0U);

	/* Reads each codeword written the way the RFC writes it, most significant bit first. */
	for (symbol = 0; symbol < 8U; symbol++) {
		lbitwriter_init(&stream);
		lbitwriter_put_code(&stream, expected[symbol], lengths[symbol]);
		webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
		CHECK(webpc_prefix_read(&pool, &code, &reader) == symbol);
		CHECK(webpc_lbits_ended(&reader) == 0);
	}

	/* The same code through the generic round trip. */
	webpc_prefix_pool_release(&pool);
	CHECK(pool.entries == NULL);
	CHECK(pool.count == 0U);
	round_trip(lengths, 8, 1);

	/* A zero length between symbols leaves them out: (0,2,0,2,2,0,2) is complete over 4 symbols. */
	round_trip(sparse, 7, 2);
}

/* A code of one symbol, of any length, reads no bits. */
static void
test_single_symbol(void)
{
	static const uint8_t bytes[4] = { 0x5aU, 0xc3U, 0x0fU, 0x81U };
	static uint8_t lengths[WEBPC_PREFIX_ALPHABET_MAX];
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code code;
	struct webpc_lbits reader;
	unsigned length;
	unsigned position;
	unsigned count;
	int error;

	/* Every length 1 to 15, the symbol at the start, the middle and the end of an alphabet. */
	for (length = 1; length <= 15U; length++) {
		for (position = 0; position < 3U; position++) {
			count = 2328U;
			memset(lengths, 0, sizeof(lengths));
			if (position == 0U)
				lengths[0] = (uint8_t)length;
			else if (position == 1U)
				lengths[1000] = (uint8_t)length;
			else
				lengths[count - 1U] = (uint8_t)length;

			webpc_prefix_pool_init(&pool, 0);
			error = webpc_prefix_build(&pool, lengths, count, &code);
			CHECK(error == 0);
			CHECK(code.root_bits == 0U);
			CHECK(pool.count == 0U);
			CHECK(pool.entries == NULL);
			if (position == 0U)
				CHECK(code.single == 0U);
			else if (position == 1U)
				CHECK(code.single == 1000U);
			else
				CHECK(code.single == count - 1U);

			/* Reads symbols with no bits used: the next 24 bits of the stream are unchanged. */
			webpc_lbits_init(&reader, bytes, sizeof(bytes));
			CHECK(webpc_prefix_read(&pool, &code, &reader) == code.single);
			CHECK(webpc_prefix_read(&pool, &code, &reader) == code.single);
			CHECK(webpc_lbits_peek(&reader, 24) == 0x0fc35aUL);
			CHECK(webpc_lbits_ended(&reader) == 0);

			/* Over no data it still reads the symbol and does not note the end. */
			webpc_lbits_init(&reader, NULL, 0);
			CHECK(webpc_prefix_read(&pool, &code, &reader) == code.single);
			CHECK(webpc_lbits_ended(&reader) == 0);
			webpc_prefix_pool_release(&pool);
		}
	}

	/* An alphabet of one symbol of length 1. */
	lengths[0] = 1;
	webpc_prefix_pool_init(&pool, 10);
	error = webpc_prefix_build(&pool, lengths, 1, &code);
	CHECK(error == 0);
	CHECK(code.single == 0U);
	webpc_prefix_pool_release(&pool);
}

/* Codes that are refused leave the pool as it was. */
static void
test_refused(void)
{
	static uint8_t lengths[WEBPC_PREFIX_ALPHABET_MAX];
	static const uint8_t incomplete3[3] = { 3, 3, 3 };
	static const uint8_t incomplete2[3] = { 1, 2, 0 };
	static const uint8_t over3[3] = { 1, 1, 1 };
	static const uint8_t over4[4] = { 1, 2, 2, 2 };
	static const uint8_t one_missing[8] = { 3, 3, 3, 3, 3, 3, 3, 0 };
	static const uint8_t zeros[5] = { 0, 0, 0, 0, 0 };
	static const uint8_t too_long[2] = { 16, 16 };
	static const uint8_t very_long[3] = { 1, 255, 1 };
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code code;
	size_t before;
	int error;

	/* Some entries are already in the pool so a change would show. */
	webpc_prefix_pool_init(&pool, 1000);
	error = webpc_prefix_build(&pool, over4 + 1, 3, &code);
	CHECK(error == EINVAL);
	lengths[0] = 1;
	lengths[1] = 1;
	error = webpc_prefix_build(&pool, lengths, 2, &code);
	CHECK(error == 0);
	before = pool.count;
	CHECK(before == 2U);

	/* Codes whose lengths leave leaves unused. */
	error = webpc_prefix_build(&pool, incomplete3, 3, &code);
	CHECK(error == EINVAL);
	error = webpc_prefix_build(&pool, incomplete2, 3, &code);
	CHECK(error == EINVAL);
	error = webpc_prefix_build(&pool, one_missing, 8, &code);
	CHECK(error == EINVAL);

	/* Codes with too many leaves. */
	error = webpc_prefix_build(&pool, over3, 3, &code);
	CHECK(error == EINVAL);
	error = webpc_prefix_build(&pool, over4, 4, &code);
	CHECK(error == EINVAL);

	/* No symbol at all. */
	error = webpc_prefix_build(&pool, zeros, 5, &code);
	CHECK(error == EINVAL);

	/* Lengths above 15, even in a code that would otherwise be one symbol. */
	error = webpc_prefix_build(&pool, too_long, 2, &code);
	CHECK(error == EINVAL);
	error = webpc_prefix_build(&pool, very_long, 3, &code);
	CHECK(error == EINVAL);
	lengths[0] = 16;
	lengths[1] = 0;
	error = webpc_prefix_build(&pool, lengths, 2, &code);
	CHECK(error == EINVAL);

	/* An alphabet size the format does not have. */
	lengths[0] = 1;
	lengths[1] = 1;
	error = webpc_prefix_build(&pool, lengths, 0, &code);
	CHECK(error == EINVAL);
	error = webpc_prefix_build(&pool, lengths, WEBPC_PREFIX_ALPHABET_MAX + 1U, &code);
	CHECK(error == EINVAL);

	/* The pool was not changed by any of them. */
	CHECK(pool.count == before);
	webpc_prefix_pool_release(&pool);
}

/* Staircase codes: lengths 1, 2, ..., k-1, k, k reach each length 2 to 15, past the first level above 8. */
static void
test_staircase(void)
{
	uint8_t lengths[16];
	uint8_t reversed[16];
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code code;
	unsigned longest;
	unsigned index;
	unsigned count;
	int error;

	/* Each longest length in turn; the symbols run in order of length, then in reverse order. */
	for (longest = 2; longest <= 15U; longest++) {
		count = longest + 1U;
		for (index = 0; index + 1U < longest; index++)
			lengths[index] = (uint8_t)(index + 1U);
		lengths[longest - 1U] = (uint8_t)longest;
		lengths[longest] = (uint8_t)longest;
		round_trip(lengths, count, longest);

		/* The first level is as wide as the longest code, up to 8 bits. */
		webpc_prefix_pool_init(&pool, WEBPC_VP8L_TABLE_ENTRIES_MAX);
		error = webpc_prefix_build(&pool, lengths, count, &code);
		CHECK(error == 0);
		if (longest <= 8U) {
			CHECK(code.root_bits == longest);
			CHECK(pool.count == (size_t)1 << longest);
		} else {
			/*
			 * Only the entry of the all-ones prefix has a second level, as
			 * wide as the bits past the first level (longest - 8).
			 */
			CHECK(code.root_bits == 8U);
			CHECK(pool.count == 256U + ((size_t)1 << (longest - 8U)));
		}
		webpc_prefix_pool_release(&pool);

		/* The same lengths with the symbols in the opposite order. */
		for (index = 0; index < count; index++)
			reversed[index] = lengths[count - 1U - index];
		round_trip(reversed, count, longest + 100UL);
	}
}

/* Codes of equal lengths over big alphabets: 256 of length 8, 512 of 9, 1024 of 10, 2048 of 11. */
static void
test_equal_lengths(void)
{
	static uint8_t lengths[WEBPC_PREFIX_ALPHABET_MAX];
	unsigned length;
	unsigned count;
	unsigned index;

	/* All the symbols the same length: 2^length of them. */
	for (length = 1; length <= 11U; length++) {
		count = 1U << length;
		for (index = 0; index < count; index++)
			lengths[index] = (uint8_t)length;
		round_trip(lengths, count, length);
	}

	/* One short symbol and the rest long: 1 of length 1 and 1024 of length 11 (1/2 + 1024/2048 = 1). */
	lengths[0] = 1;
	for (index = 1; index <= 1024U; index++)
		lengths[index] = 11;
	round_trip(lengths, 1025, 7);
}

/* The pool's limit: ENOMEM, the count restored, and a smaller code still fits afterwards. */
static void
test_limit(void)
{
	static const uint8_t example[8] = { 3, 3, 3, 3, 3, 2, 4, 4 };
	static const uint8_t two[2] = { 1, 1 };
	static const uint8_t lone[4] = { 0, 0, 7, 0 };
	uint8_t staircase[16];
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code first;
	struct webpc_prefix_code second;
	struct webpc_lbits reader;
	unsigned index;
	int error;

	/* A limit of 20: the example's 16 entries fit, 16 more do not. */
	webpc_prefix_pool_init(&pool, 20);
	error = webpc_prefix_build(&pool, example, 8, &first);
	CHECK(error == 0);
	CHECK(pool.count == 16U);
	error = webpc_prefix_build(&pool, example, 8, &second);
	CHECK(error == ENOMEM);
	CHECK(pool.count == 16U);

	/* A smaller code still fits afterwards: 2 more entries, and the first code still reads. */
	error = webpc_prefix_build(&pool, two, 2, &second);
	CHECK(error == 0);
	CHECK(pool.count == 18U);
	CHECK(second.offset == 16U);
	lbitwriter_init(&stream);
	lbitwriter_put_code(&stream, 14, 4);
	lbitwriter_put_code(&stream, 1, 1);
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	CHECK(webpc_prefix_read(&pool, &first, &reader) == 6U);
	CHECK(webpc_prefix_read(&pool, &second, &reader) == 1U);

	/* A code of one symbol takes no entries, so it fits in a full pool. */
	error = webpc_prefix_build(&pool, lone, 4, &second);
	CHECK(error == 0);
	CHECK(pool.count == 18U);
	webpc_prefix_pool_release(&pool);

	/* Exactly the limit fits: the limit equal to the table's size. */
	webpc_prefix_pool_init(&pool, 16);
	error = webpc_prefix_build(&pool, example, 8, &first);
	CHECK(error == 0);
	CHECK(pool.count == 16U);
	webpc_prefix_pool_release(&pool);

	/* One entry short does not. */
	webpc_prefix_pool_init(&pool, 15);
	error = webpc_prefix_build(&pool, example, 8, &first);
	CHECK(error == ENOMEM);
	CHECK(pool.count == 0U);
	webpc_prefix_pool_release(&pool);

	/* A limit of 0 refuses every code with a table. */
	webpc_prefix_pool_init(&pool, 0);
	error = webpc_prefix_build(&pool, two, 2, &first);
	CHECK(error == ENOMEM);
	CHECK(pool.count == 0U);
	webpc_prefix_pool_release(&pool);

	/* A code with second-level tables is counted whole: the staircase to 15 takes more than 256 entries. */
	for (index = 0; index < 14U; index++)
		staircase[index] = (uint8_t)(index + 1U);
	staircase[14] = 15;
	staircase[15] = 15;
	webpc_prefix_pool_init(&pool, 256);
	error = webpc_prefix_build(&pool, staircase, 16, &first);
	CHECK(error == ENOMEM);
	CHECK(pool.count == 0U);
	webpc_prefix_pool_release(&pool);
	webpc_prefix_pool_init(&pool, 10000);
	error = webpc_prefix_build(&pool, staircase, 16, &first);
	CHECK(error == 0);
	CHECK(pool.count > 256U);
	CHECK(pool.count <= 10000U);
	webpc_prefix_pool_release(&pool);
}

/* Codes built one after another keep working while the pool grows (and moves). */
static void
test_pool_growth(void)
{
	static const uint8_t example[8] = { 3, 3, 3, 3, 3, 2, 4, 4 };
	static uint8_t big[256];
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code first;
	struct webpc_prefix_code later;
	struct webpc_lbits reader;
	unsigned index;
	int error;

	/* A small code first, then many 256-entry codes that make the pool reallocate. */
	webpc_prefix_pool_init(&pool, WEBPC_VP8L_TABLE_ENTRIES_MAX);
	error = webpc_prefix_build(&pool, example, 8, &first);
	CHECK(error == 0);
	for (index = 0; index < 256U; index++)
		big[index] = 8;
	for (index = 0; index < 100U; index++) {
		error = webpc_prefix_build(&pool, big, 256, &later);
		CHECK(error == 0);
		CHECK(later.offset == 16U + (size_t)index * 256U);
	}
	CHECK(pool.count == 16U + 100U * 256U);
	CHECK(pool.capacity >= pool.count);

	/* The first code and the last one still read as built. */
	lbitwriter_init(&stream);
	lbitwriter_put_code(&stream, 15, 4);
	lbitwriter_put_code(&stream, 77, 8);
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	CHECK(webpc_prefix_read(&pool, &first, &reader) == 7U);
	CHECK(webpc_prefix_read(&pool, &later, &reader) == 77U);

	/* A released pool is empty and works again. */
	webpc_prefix_pool_release(&pool);
	CHECK(pool.entries == NULL);
	CHECK(pool.count == 0U);
	CHECK(pool.capacity == 0U);
	error = webpc_prefix_build(&pool, example, 8, &first);
	CHECK(error == 0);
	CHECK(pool.count == 16U);
	webpc_prefix_pool_release(&pool);
	webpc_prefix_pool_release(&pool);
}

/* A read past the end of the data gives the symbol the zero bits select, and notes the end. */
static void
test_end_of_data(void)
{
	static const uint8_t example[8] = { 3, 3, 3, 3, 3, 2, 4, 4 };
	static const uint8_t cut[1] = { 0x07U };
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code code;
	struct webpc_lbits reader;
	int error;

	/* No data: the zero bits select F (00), and the 2 bits of its code are past the end. */
	webpc_prefix_pool_init(&pool, 100);
	error = webpc_prefix_build(&pool, example, 8, &code);
	CHECK(error == 0);
	webpc_lbits_init(&reader, NULL, 0);
	CHECK(webpc_prefix_read(&pool, &code, &reader) == 5U);
	CHECK(webpc_lbits_ended(&reader) == 1);

	/*
	 * One byte 0x07 = bits 1,1,1,0,0,0,0,0: the code 111 then 0 is G (1110),
	 * and then the 4 zeros read F twice, using the last bit exactly: the end
	 * is noted only by the read after that.
	 */
	webpc_lbits_init(&reader, cut, sizeof(cut));
	CHECK(webpc_prefix_read(&pool, &code, &reader) == 6U);
	CHECK(webpc_prefix_read(&pool, &code, &reader) == 5U);
	CHECK(webpc_prefix_read(&pool, &code, &reader) == 5U);
	CHECK(webpc_lbits_ended(&reader) == 0);
	CHECK(webpc_prefix_read(&pool, &code, &reader) == 5U);
	CHECK(webpc_lbits_ended(&reader) == 1);
	webpc_prefix_pool_release(&pool);
}

/* Random bytes through random codes: always a present symbol, never outside the table. */
static void
test_random_bytes(void)
{
	static uint8_t lengths[WEBPC_PREFIX_ALPHABET_MAX];
	static unsigned long weights[WEBPC_PREFIX_ALPHABET_MAX];
	static uint8_t data[512];
	struct webpc_prefix_pool pool;
	struct webpc_prefix_code code;
	struct webpc_lbits reader;
	unsigned long state;
	unsigned long round;
	unsigned count;
	unsigned index;
	unsigned symbol;
	int error;

	/* Rounds of a random code and random data read until its end passes. */
	state = 4242UL;
	for (round = 0; round < 40UL; round++) {
		count = 2U + (unsigned)(check_random(&state) % 2327UL);
		for (index = 0; index < count; index++)
			weights[index] = 1UL + (check_random(&state) % 1000UL);
		huffman_lengths(weights, count, lengths);
		for (index = 0; index < sizeof(data); index++)
			data[index] = (uint8_t)(check_random(&state) & 0xffUL);

		webpc_prefix_pool_init(&pool, WEBPC_VP8L_TABLE_ENTRIES_MAX);
		error = webpc_prefix_build(&pool, lengths, count, &code);
		CHECK(error == 0);
		webpc_lbits_init(&reader, data, sizeof(data));
		for (index = 0; index < 5000U; index++) {
			symbol = webpc_prefix_read(&pool, &code, &reader);
			CHECK(symbol < count);
			CHECK(symbol >= count || lengths[symbol] != 0U);
		}
		CHECK(webpc_lbits_ended(&reader) == 1);
		webpc_prefix_pool_release(&pool);
	}
}

/* One entry of the leaves the Huffman builder sorts. */
struct leaf {
	unsigned long weight;
	unsigned symbol;
};

/* Orders leaves by weight, then by symbol so the order is the same everywhere. */
static int
compare_leaves(
	const void *left,
	const void *right)
{
	const struct leaf *first;
	const struct leaf *second;

	/* Lighter first; equal weights by symbol. */
	first = (const struct leaf *)left;
	second = (const struct leaf *)right;
	if (first->weight < second->weight)
		return -1;
	if (first->weight > second->weight)
		return 1;
	if (first->symbol < second->symbol)
		return -1;
	if (first->symbol > second->symbol)
		return 1;
	return 0;
}

/*
 * Makes the code lengths of a Huffman code for weights (0 weight = absent;
 * at least two present).  The tree is built with two queues; a length above
 * 15 is cut to 15 and the Kraft sum is brought back to 1 by lengthening
 * shorter codes while it is over and shortening codes while it is under.
 * Gives 1 when the cut was needed.
 */
static unsigned
huffman_lengths(
	const unsigned long *weights,
	unsigned count,
	uint8_t *lengths)
{
	static struct leaf leaves[WEBPC_PREFIX_ALPHABET_MAX];
	static unsigned long node_weight[2U * WEBPC_PREFIX_ALPHABET_MAX];
	static unsigned node_parent[2U * WEBPC_PREFIX_ALPHABET_MAX];
	static unsigned depth[2U * WEBPC_PREFIX_ALPHABET_MAX];
	unsigned present;
	unsigned nodes;
	unsigned leaf_next;
	unsigned merged_next;
	unsigned index;
	unsigned pick[2];
	unsigned which;
	unsigned long total;
	unsigned long slack;
	unsigned cut;
	unsigned changed;
	unsigned symbol;
	unsigned level;

	/* The present symbols, sorted by weight. */
	present = 0;
	for (symbol = 0; symbol < count; symbol++) {
		lengths[symbol] = 0;
		if (weights[symbol] == 0UL)
			continue;
		leaves[present].weight = weights[symbol];
		leaves[present].symbol = symbol;
		present++;
	}
	qsort(leaves, present, sizeof(leaves[0]), compare_leaves);

	/* The leaves are nodes 0..present-1; merged nodes follow, made from the two lightest each time. */
	for (index = 0; index < present; index++)
		node_weight[index] = leaves[index].weight;
	nodes = present;
	leaf_next = 0;
	merged_next = present;
	while (merged_next - present + 1U < present || leaf_next < present) {
		/* Stops when one root remains: the leaves are used and one merged node is left. */
		if (leaf_next >= present && nodes - merged_next <= 1U)
			break;
		for (which = 0; which < 2U; which++) {
			/* The lighter of the next leaf and the oldest merged node. */
			if (leaf_next < present && (merged_next >= nodes || node_weight[leaf_next] <= node_weight[merged_next])) {
				pick[which] = leaf_next;
				leaf_next++;
			} else {
				pick[which] = merged_next;
				merged_next++;
			}
		}
		node_weight[nodes] = node_weight[pick[0]] + node_weight[pick[1]];
		node_parent[pick[0]] = nodes;
		node_parent[pick[1]] = nodes;
		nodes++;
	}

	/* Depths from the root (the last node) down: parents come after their children. */
	depth[nodes - 1U] = 0;
	for (index = nodes - 1U; index > 0U; index--) {
		/* Node index - 1 has its parent already given a depth, since parents have larger numbers. */
		depth[index - 1U] = depth[node_parent[index - 1U]] + 1U;
	}
	cut = 0;
	for (index = 0; index < present; index++) {
		level = depth[index];
		if (level > 15U) {
			level = 15U;
			cut = 1;
		}
		lengths[leaves[index].symbol] = (uint8_t)level;
	}

	/* Lengthens the longest code under 15 while the Kraft sum is over 1. */
	total = kraft_sum(lengths, count);
	while (total > 32768UL) {
		symbol = count;
		level = 0;
		for (index = 0; index < count; index++) {
			if (lengths[index] != 0U && lengths[index] < 15U && lengths[index] > level) {
				level = lengths[index];
				symbol = index;
			}
		}
		lengths[symbol]++;
		total = kraft_sum(lengths, count);
	}

	/* Shortens a code while the Kraft sum is under 1 and a code whose shortening fits exists. */
	changed = 1;
	while (total < 32768UL && changed != 0U) {
		changed = 0;
		slack = 32768UL - total;
		for (index = 0; index < count; index++) {
			if (lengths[index] > 1U && (1UL << (15U - lengths[index])) <= slack) {
				lengths[index]--;
				changed = 1;
				break;
			}
		}
		total = kraft_sum(lengths, count);
	}
	return cut;
}

/* Random alphabets with skewed frequencies: complete Huffman codes, some cut to length 15, 100 of them. */
static void
test_random_huffman(void)
{
	static uint8_t lengths[WEBPC_PREFIX_ALPHABET_MAX];
	static unsigned long weights[WEBPC_PREFIX_ALPHABET_MAX];
	unsigned long state;
	unsigned long round;
	unsigned count;
	unsigned index;
	unsigned cut;
	unsigned cuts;
	unsigned deep;
	unsigned maximum;
	unsigned style;

	/* Each round: an alphabet size, then weights of one of three styles. */
	state = 987654321UL;
	cuts = 0;
	deep = 0;
	for (round = 0; round < 100UL; round++) {
		if (round == 0UL)
			count = WEBPC_PREFIX_ALPHABET_MAX;
		else
			count = 2U + (unsigned)(check_random(&state) % 2327UL);
		style = (unsigned)(round % 3UL);

		/* Style 0: even; 1: exponential in size (long codes, cut at 15); 2: mostly absent symbols. */
		for (index = 0; index < count; index++) {
			if (style == 0U)
				weights[index] = 1UL + (check_random(&state) % 100UL);
			else if (style == 1U)
				weights[index] = 1UL << (check_random(&state) % 24UL);
			else if (check_random(&state) % 4UL == 0UL)
				weights[index] = 1UL + (check_random(&state) % 5000UL);
			else
				weights[index] = 0UL;
		}

		/* At least two symbols are present. */
		weights[0] = 1UL;
		weights[count - 1U] = 3UL;
		cut = huffman_lengths(weights, count, lengths);
		cuts += cut;
		maximum = 0;
		for (index = 0; index < count; index++) {
			if (lengths[index] > maximum)
				maximum = lengths[index];
		}
		if (maximum > 8U)
			deep++;
		round_trip(lengths, count, round);
	}

	/* The set reached the second level, and the cut to length 15 happened, in some codes. */
	CHECK(deep > 10U);
	CHECK(cuts > 3U);
}
