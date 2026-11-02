/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reading of the lossless format's prefix codes and of a group of five.
 */

#include "vp8l-codes.h"

#include <errno.h>
#include <string.h>

/* The most symbols the first read of a simple code length code may name: 8 bits, then 8 bits. */
#define CODES_SIMPLE_SYMBOLS_MAX	2U

/* The width in bits of one length of the code length code, and of the num_code_lengths field. */
#define CODES_LENGTH_CODE_BITS		3U
#define CODES_NUM_LENGTHS_BITS		4U

/* The fewest code length code lengths a normal code gives (RFC 9649 3.7.2.1.2: 4 + ReadBits(4)). */
#define CODES_NUM_LENGTHS_MIN		4U

/* The length a repeat of code 16 uses when no non-zero length has been read yet. */
#define CODES_REPEAT_DEFAULT_LENGTH	8U

/* The first code length symbol that is a repeat rather than a length. */
#define CODES_FIRST_REPEAT		16U

/*
 * The order the code length code's own lengths come in.
 *
 * Values referenced: RFC 9649 (WebP Image Format, 2024-11) 3.7.2.1.2
 * (kCodeLengthCodeOrder).
 */
static const uint8_t codes_length_code_order[WEBPC_VP8L_CODE_LENGTH_CODES] = {
	17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

/*
 * For the repeat symbols 16, 17 and 18: the extra bits read after the
 * symbol, and the repeat count when those bits are 0.
 *
 * Values referenced: RFC 9649 (WebP Image Format, 2024-11) 3.7.2.1.2 (code
 * 16: 3 + ReadBits(2); code 17: 3 + ReadBits(3); code 18: 11 + ReadBits(7)).
 */
static const uint8_t codes_repeat_bits[3] = { 2, 3, 7 };
static const uint8_t codes_repeat_base[3] = { 3, 3, 11 };

static int codes_read_unchecked(struct webpc_lbits *reader, struct webpc_prefix_pool *pool, unsigned alphabet_size,
	struct webpc_prefix_code *code);
static int codes_read_simple(struct webpc_lbits *reader, struct webpc_prefix_pool *pool, unsigned alphabet_size,
	struct webpc_prefix_code *code);
static int codes_read_normal(struct webpc_lbits *reader, struct webpc_prefix_pool *pool, unsigned alphabet_size,
	struct webpc_prefix_code *code);
static int codes_read_lengths(struct webpc_lbits *reader, const struct webpc_prefix_pool *pool,
	const struct webpc_prefix_code *length_code, unsigned alphabet_size, unsigned max_symbol, uint8_t *lengths);

/*
 * Reads one prefix code (RFC 9649 3.7.2.1) for an alphabet of alphabet_size
 * symbols (1..WEBPC_PREFIX_ALPHABET_MAX) and builds it into pool.
 */
int
webpc_vp8l_read_code(
	struct webpc_lbits *reader,
	struct webpc_prefix_pool *pool,
	unsigned alphabet_size,
	struct webpc_prefix_code *code)
{
	size_t saved_count;
	int ended;
	int error;

	/* Refuses an alphabet the format does not have. */
	if (alphabet_size == 0U || alphabet_size > WEBPC_PREFIX_ALPHABET_MAX)
		return EINVAL;

	/* Remembers how much of the pool is in use, to give it back on a failure. */
	saved_count = pool->count;

	/* Reads and builds the code. */
	error = codes_read_unchecked(reader, pool, alphabet_size, code);

	/*
	 * A reader that ran past its data read zeros for the missing bits, so
	 * whatever the code came out as (or failed as) is not what the stream
	 * says: the stream is cut short.
	 */
	ended = webpc_lbits_ended(reader);
	if (ended != 0)
		error = ENOBUFS;

	/* Gives back the tables of a code that did not make it. */
	if (error != 0) {
		pool->count = saved_count;
		return error;
	}

	/* Succeeded: the code is built and its bits are consumed. */
	return 0;
}

/*
 * Reads the five codes of one group (RFC 9649 3.7.2.2.2, 3.8.3) for a colour
 * cache of cache_bits bits (0 for none, else 1..11): the green code's
 * alphabet is 256 + 24 + (1 << cache_bits) (256 + 24 without a cache), red,
 * blue and alpha 256, distance 40.
 */
int
webpc_vp8l_read_group(
	struct webpc_lbits *reader,
	struct webpc_prefix_pool *pool,
	unsigned cache_bits,
	struct webpc_vp8l_group *group)
{
	unsigned alphabet_sizes[WEBPC_VP8L_GROUP_CODES];
	size_t saved_count;
	unsigned index;
	int error;

	/* Refuses a colour cache wider than the format allows. */
	if (cache_bits > WEBPC_VP8L_CACHE_BITS_MAX)
		return EINVAL;

	/*
	 * Fixes the alphabet of each code: green has the literals, the length
	 * prefixes and the cache indices; red, blue and alpha have the 256
	 * values of a channel; distance has the distance prefixes.
	 */
	alphabet_sizes[WEBPC_VP8L_CODE_GREEN] = WEBPC_VP8L_LITERALS + WEBPC_VP8L_LENGTH_CODES;
	if (cache_bits != 0U)
		alphabet_sizes[WEBPC_VP8L_CODE_GREEN] += 1U << cache_bits;
	alphabet_sizes[WEBPC_VP8L_CODE_RED] = WEBPC_VP8L_LITERALS;
	alphabet_sizes[WEBPC_VP8L_CODE_BLUE] = WEBPC_VP8L_LITERALS;
	alphabet_sizes[WEBPC_VP8L_CODE_ALPHA] = WEBPC_VP8L_LITERALS;
	alphabet_sizes[WEBPC_VP8L_CODE_DISTANCE] = WEBPC_VP8L_DISTANCE_CODES;

	/* Remembers the pool's use, to give back the tables if a later code fails. */
	saved_count = pool->count;

	/* Reads the five codes in their order in the stream. */
	for (index = 0; index < WEBPC_VP8L_GROUP_CODES; index++) {
		error = webpc_vp8l_read_code(reader, pool, alphabet_sizes[index], &group->codes[index]);
		if (error != 0) {
			/* The codes already built are given back too. */
			pool->count = saved_count;
			return error;
		}
	}

	/* Succeeded: all five codes of the group are built. */
	return 0;
}

/*
 * Reads and builds one code by its first bit, without looking at whether
 * the reader ran out of data.
 */
static int
codes_read_unchecked(
	struct webpc_lbits *reader,
	struct webpc_prefix_pool *pool,
	unsigned alphabet_size,
	struct webpc_prefix_code *code)
{
	uint32_t is_simple;
	int error;

	/* The first bit says whether this is a simple code length code. */
	is_simple = webpc_lbits_read(reader, 1U);
	if (is_simple != 0U) {
		/* Reads the one or two symbols of a simple code. */
		error = codes_read_simple(reader, pool, alphabet_size, code);
	} else {
		/* Reads the lengths of a normal code. */
		error = codes_read_normal(reader, pool, alphabet_size, code);
	}

	/* Reports a code that could not be read or built. */
	if (error != 0)
		return error;

	/* Succeeded: the code is built. */
	return 0;
}

/*
 * Reads a simple code length code (RFC 9649 3.7.2.1.1) after its first bit:
 * one or two symbols, each of length 1.
 */
static int
codes_read_simple(
	struct webpc_lbits *reader,
	struct webpc_prefix_pool *pool,
	unsigned alphabet_size,
	struct webpc_prefix_code *code)
{
	uint8_t lengths[WEBPC_PREFIX_ALPHABET_MAX];
	unsigned symbols[CODES_SIMPLE_SYMBOLS_MAX];
	unsigned symbol_count;
	unsigned first_bits;
	uint32_t is_first_8bits;
	unsigned index;
	int error;

	/*
	 * Reads how many symbols there are (1 or 2), and how wide the first
	 * symbol is: 1 bit, or 8 bits when is_first_8bits is set.  The second
	 * symbol always takes 8 bits.
	 */
	symbol_count = 1U + webpc_lbits_read(reader, 1U);
	is_first_8bits = webpc_lbits_read(reader, 1U);
	first_bits = 1U;
	if (is_first_8bits != 0U)
		first_bits = 8U;
	symbols[0] = webpc_lbits_read(reader, first_bits);
	symbols[1] = 0;
	if (symbol_count == 2U)
		symbols[1] = webpc_lbits_read(reader, 8U);

	/*
	 * Gives each symbol that is inside the alphabet the length 1; one that
	 * is not is left out of the code.  Two equal symbols set the same
	 * entry, which is the code of that one symbol.
	 */
	memset(lengths, 0, alphabet_size);
	for (index = 0; index < symbol_count; index++) {
		if (symbols[index] < alphabet_size)
			lengths[symbols[index]] = 1;
	}

	/* Builds the code; no symbol left, or an unbuildable code, is refused. */
	error = webpc_prefix_build(pool, lengths, alphabet_size, code);
	if (error != 0)
		return error;

	/* Succeeded: the simple code is built. */
	return 0;
}

/*
 * Reads a normal code length code (RFC 9649 3.7.2.1.2) after its first bit:
 * the code length code, how many code length symbols to read, then the
 * symbols that give the lengths.
 */
static int
codes_read_normal(
	struct webpc_lbits *reader,
	struct webpc_prefix_pool *pool,
	unsigned alphabet_size,
	struct webpc_prefix_code *code)
{
	uint8_t length_code_lengths[WEBPC_VP8L_CODE_LENGTH_CODES];
	uint8_t lengths[WEBPC_PREFIX_ALPHABET_MAX];
	struct webpc_prefix_code length_code;
	size_t saved_count;
	unsigned num_code_lengths;
	unsigned length_nbits;
	unsigned max_symbol;
	uint32_t has_max_symbol;
	unsigned index;
	int error;

	/* Reads how many of the code length code's lengths follow, and the lengths in their order. */
	num_code_lengths = CODES_NUM_LENGTHS_MIN + webpc_lbits_read(reader, CODES_NUM_LENGTHS_BITS);
	memset(length_code_lengths, 0, sizeof(length_code_lengths));
	for (index = 0; index < num_code_lengths; index++)
		length_code_lengths[codes_length_code_order[index]] = (uint8_t)webpc_lbits_read(reader, CODES_LENGTH_CODE_BITS);

	/*
	 * Reads how many code length symbols may be read: the whole alphabet,
	 * or the number the stream gives.
	 */
	max_symbol = alphabet_size;
	has_max_symbol = webpc_lbits_read(reader, 1U);
	if (has_max_symbol != 0U) {
		length_nbits = 2U + 2U * webpc_lbits_read(reader, 3U);
		max_symbol = 2U + webpc_lbits_read(reader, length_nbits);

		/* More symbols than the alphabet has lengths is a broken stream. */
		if (max_symbol > alphabet_size)
			return EINVAL;
	}

	/*
	 * Builds the code length code in the pool.  Its table is only needed
	 * while the lengths are read, so the pool is given back after that.
	 */
	saved_count = pool->count;
	error = webpc_prefix_build(pool, length_code_lengths, WEBPC_VP8L_CODE_LENGTH_CODES, &length_code);
	if (error != 0)
		return error;

	/* Reads the lengths of the code's symbols. */
	memset(lengths, 0, alphabet_size);
	error = codes_read_lengths(reader, pool, &length_code, alphabet_size, max_symbol, lengths);
	pool->count = saved_count;
	if (error != 0)
		return error;

	/* Builds the code from its lengths; an incomplete code is refused. */
	error = webpc_prefix_build(pool, lengths, alphabet_size, code);
	if (error != 0)
		return error;

	/* Succeeded: the normal code is built. */
	return 0;
}

/*
 * Reads code length symbols with length_code and fills lengths[0..
 * alphabet_size - 1] from symbol 0 up, until the alphabet is full or
 * max_symbol symbols have been read.
 */
static int
codes_read_lengths(
	struct webpc_lbits *reader,
	const struct webpc_prefix_pool *pool,
	const struct webpc_prefix_code *length_code,
	unsigned alphabet_size,
	unsigned max_symbol,
	uint8_t *lengths)
{
	unsigned symbol;
	unsigned previous_length;
	unsigned token;
	unsigned repeat_kind;
	unsigned repeat;
	unsigned fill_length;
	unsigned index;

	/*
	 * Reads one code length symbol at a time.  max_symbol counts these
	 * symbols, so a repeat that fills many lengths counts once.  The
	 * length a code 16 repeats starts as 8 and follows the last non-zero
	 * length read.
	 */
	symbol = 0;
	previous_length = CODES_REPEAT_DEFAULT_LENGTH;
	while (symbol < alphabet_size && max_symbol > 0U) {
		max_symbol--;
		token = webpc_prefix_read(pool, length_code, reader);

		/* A symbol below 16 is a length itself. */
		if (token < CODES_FIRST_REPEAT) {
			lengths[symbol] = (uint8_t)token;
			symbol++;

			/* Only a non-zero length is what a later code 16 repeats. */
			if (token != 0U)
				previous_length = token;
			continue;
		}

		/* Otherwise it is a repeat: 16 repeats the last length, 17 and 18 give zeros. */
		repeat_kind = token - CODES_FIRST_REPEAT;
		repeat = codes_repeat_base[repeat_kind] + webpc_lbits_read(reader, codes_repeat_bits[repeat_kind]);
		fill_length = 0;
		if (token == CODES_FIRST_REPEAT)
			fill_length = previous_length;

		/* A repeat running past the alphabet is a broken stream. */
		if (repeat > alphabet_size - symbol)
			return EINVAL;

		/* Writes the repeated length into the next lengths. */
		for (index = 0; index < repeat; index++)
			lengths[symbol + index] = (uint8_t)fill_length;
		symbol += repeat;
	}

	/* Succeeded: the lengths read are in place, the rest are zero. */
	return 0;
}
