/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The canonical prefix codes of the lossless format: their lookup tables and
 * the reading of a symbol.
 */

#include "prefix.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The entries of a first-level table at its widest: 1 << WEBPC_PREFIX_ROOT_BITS. */
#define PREFIX_ROOT_ENTRIES_MAX		256U

/* The fewest entries a pool grows to when it first allocates. */
#define PREFIX_POOL_CAPACITY_MIN	1024U

/* The sum of 2^-length over the symbols of a complete code, in units of 2^-15. */
#define PREFIX_KRAFT_COMPLETE		(1UL << WEBPC_PREFIX_LENGTH_MAX)

static unsigned prefix_reverse(unsigned code, unsigned length);
static int prefix_reserve(struct webpc_prefix_pool *pool, size_t total);

/*
 * Starts an empty pool whose tables may hold at most limit entries in all.
 */
void
webpc_prefix_pool_init(
	struct webpc_prefix_pool *pool,
	size_t limit)
{
	/* No table yet, and the most entries they may reach. */
	pool->entries = NULL;
	pool->count = 0;
	pool->capacity = 0;
	pool->limit = limit;
}

/*
 * Frees a pool's tables and leaves it empty (and usable again).
 */
void
webpc_prefix_pool_release(
	struct webpc_prefix_pool *pool)
{
	/* Gives the tables back. */
	free(pool->entries);

	/* Leaves the pool empty, with its limit kept. */
	pool->entries = NULL;
	pool->count = 0;
	pool->capacity = 0;
}

/*
 * Builds the canonical prefix code whose code lengths are lengths[0..count -
 * 1] (count 1..WEBPC_PREFIX_ALPHABET_MAX, each length 0..15, 0 for a symbol
 * absent from the code) and appends its tables to pool.
 */
int
webpc_prefix_build(
	struct webpc_prefix_pool *pool,
	const uint8_t *lengths,
	unsigned count,
	struct webpc_prefix_code *code)
{
	unsigned length_count[WEBPC_PREFIX_LENGTH_MAX + 1U];
	unsigned next_code[WEBPC_PREFIX_LENGTH_MAX + 1U];
	uint16_t reversed[WEBPC_PREFIX_ALPHABET_MAX];
	uint8_t deepest[PREFIX_ROOT_ENTRIES_MAX];
	unsigned sub_offset[PREFIX_ROOT_ENTRIES_MAX];
	struct webpc_prefix_entry *table;
	struct webpc_prefix_entry *entry;
	unsigned long kraft;
	unsigned present;
	unsigned single_symbol;
	unsigned longest;
	unsigned root_bits;
	unsigned root_size;
	unsigned length;
	unsigned symbol;
	unsigned first_code;
	unsigned sub_index;
	unsigned index;
	unsigned step;
	unsigned span;
	unsigned sub_bits;
	size_t total;
	size_t base;
	int error;

	/* Refuses an alphabet size the format does not have. */
	if (count == 0U || count > WEBPC_PREFIX_ALPHABET_MAX)
		return EINVAL;

	/*
	 * Counts the symbols of each length, and notes the longest length and the
	 * one present symbol seen last (the one symbol of a single-symbol code).
	 */
	memset(length_count, 0, sizeof(length_count));
	present = 0;
	single_symbol = 0;
	longest = 0;
	for (symbol = 0; symbol < count; symbol++) {
		length = lengths[symbol];

		/* A length above 15 cannot be coded: the stream is broken. */
		if (length > WEBPC_PREFIX_LENGTH_MAX)
			return EINVAL;

		/* A zero length is a symbol the code does not have. */
		if (length == 0U)
			continue;

		/* Counts the symbol under its length and remembers it as the last one present. */
		length_count[length]++;
		present++;
		single_symbol = symbol;

		/* Tracks the longest code length. */
		if (length > longest)
			longest = length;
	}

	/* A code with no symbol cannot read anything. */
	if (present == 0U)
		return EINVAL;

	/*
	 * A code of one symbol, of whatever length, is a complete tree of one
	 * leaf: its symbol is read with no bits and needs no table.
	 */
	if (present == 1U) {
		code->offset = 0;
		code->root_bits = 0;
		code->single = single_symbol;
		return 0;
	}

	/*
	 * Adds up 2^-length of every present symbol in units of 2^-15; the tree
	 * is complete only when they make exactly 1.
	 */
	kraft = 0;
	for (length = 1; length <= WEBPC_PREFIX_LENGTH_MAX; length++)
		kraft += (unsigned long)length_count[length] << (WEBPC_PREFIX_LENGTH_MAX - length);

	/* A tree with leaves to spare, or too many leaves to fit, is broken. */
	if (kraft != PREFIX_KRAFT_COMPLETE)
		return EINVAL;

	/*
	 * Procedure referenced: RFC 1951 (DEFLATE Compressed Data Format
	 * Specification version 1.3) 3.2.2, steps 1 to 3 (the count of codes of
	 * each length, the smallest code of each length, the assignment in the
	 * order of the symbols).
	 *
	 * Finds the first code of each length: the first code of a length is
	 * the end of the codes of the length before it, shifted up one bit (no
	 * length 0 symbol was counted, so length_count[0] is 0).
	 */
	next_code[0] = 0;
	first_code = 0;
	for (length = 1; length <= WEBPC_PREFIX_LENGTH_MAX; length++) {
		first_code = (first_code + length_count[length - 1U]) << 1;
		next_code[length] = first_code;
	}

	/*
	 * Gives each present symbol its code, the shorter lengths first and the
	 * symbols of a length in order, and keeps the code with its bits turned
	 * around: the stream gives the first bit of a code first, and the first
	 * bit read is bit 0 of a peek.
	 */
	for (symbol = 0; symbol < count; symbol++) {
		length = lengths[symbol];

		/* An absent symbol has no code to turn around. */
		if (length == 0U) {
			reversed[symbol] = 0;
			continue;
		}

		/* Takes the symbol's next code of its length, bits turned around. */
		reversed[symbol] = (uint16_t)prefix_reverse(next_code[length], length);
		next_code[length]++;
	}

	/* The first level indexes the bits of the longest code, up to the root width. */
	root_bits = longest;
	if (root_bits > WEBPC_PREFIX_ROOT_BITS)
		root_bits = WEBPC_PREFIX_ROOT_BITS;
	root_size = 1U << root_bits;

	/*
	 * Finds, for each first-level entry, the longest code under it: the
	 * second-level table of that entry is as wide as the bits it needs.
	 */
	memset(deepest, 0, sizeof(deepest));
	for (symbol = 0; symbol < count; symbol++) {
		length = lengths[symbol];

		/* A code that fits the first level has no second-level table. */
		if (length <= root_bits)
			continue;

		/* Notes the code's length under its first-level entry when it is the longest there. */
		index = reversed[symbol] & (root_size - 1U);
		if (length > deepest[index])
			deepest[index] = (uint8_t)length;
	}

	/*
	 * Lays the second-level tables out after the first level, in the order
	 * of their entries, and adds up the size of the code's whole table.
	 */
	total = root_size;
	for (index = 0; index < root_size; index++) {
		sub_offset[index] = 0;

		/* An entry with no long code needs no second-level table. */
		if (deepest[index] == 0U)
			continue;

		/* Places the entry's second-level table at the end of the table so far. */
		sub_offset[index] = (unsigned)total;
		total += (size_t)1 << (deepest[index] - root_bits);
	}

	/* Makes room in the pool, or refuses a table past the pool's limit. */
	error = prefix_reserve(pool, total);
	if (error != 0)
		return error;

	/* Clears the code's table; a complete code fills every entry below. */
	base = pool->count;
	table = pool->entries + base;
	memset(table, 0, total * sizeof(*table));

	/* Links each first-level entry that has a second-level table to it. */
	for (index = 0; index < root_size; index++) {
		/* An entry with no long code has no second-level table to link to. */
		if (deepest[index] == 0U)
			continue;

		/* The first-level entry becomes a link to its second-level table. */
		table[index].kind = 1;
		table[index].value = (uint16_t)sub_offset[index];
		table[index].bits = (uint8_t)(deepest[index] - root_bits);
	}

	/* Writes each symbol into every entry its code selects. */
	for (symbol = 0; symbol < count; symbol++) {
		length = lengths[symbol];

		/* An absent symbol is written nowhere. */
		if (length == 0U)
			continue;

		/* A short code takes every first-level entry that starts with its bits. */
		if (length <= root_bits) {
			step = 1U << length;
			for (index = reversed[symbol]; index < root_size; index += step) {
				entry = &table[index];
				entry->kind = 0;
				entry->value = (uint16_t)symbol;
				entry->bits = (uint8_t)length;
			}

			continue;
		}

		/*
		 * A long code takes the entries of its second-level table that
		 * start with the bits after the first level.
		 */
		index = reversed[symbol] & (root_size - 1U);
		sub_bits = (unsigned)deepest[index] - root_bits;
		span = (unsigned)1 << (length - root_bits);
		for (sub_index = reversed[symbol] >> root_bits; sub_index < (1U << sub_bits); sub_index += span) {
			entry = &table[sub_offset[index] + sub_index];
			entry->kind = 0;
			entry->value = (uint16_t)symbol;
			entry->bits = (uint8_t)(length - root_bits);
		}
	}

	/* Publishes the table: the pool now holds it, and the code points at it. */
	pool->count = base + total;
	code->offset = base;
	code->root_bits = root_bits;
	code->single = 0;

	/* Succeeded: the code is built. */
	return 0;
}

/*
 * Reads one symbol with a code built in pool, consuming its bits from
 * reader.
 */
unsigned
webpc_prefix_read(
	const struct webpc_prefix_pool *pool,
	const struct webpc_prefix_code *code,
	struct webpc_lbits *reader)
{
	const struct webpc_prefix_entry *table;
	const struct webpc_prefix_entry *entry;
	uint32_t bits;

	/* A code of one symbol reads no bits, so its symbol is the answer. */
	if (code->root_bits == 0U)
		return code->single;

	/* Looks the next bits up in the first level. */
	table = pool->entries + code->offset;
	bits = webpc_lbits_peek(reader, code->root_bits);
	entry = &table[bits];

	/* A symbol in the first level uses as many bits as its code is long. */
	if (entry->kind == 0U) {
		webpc_lbits_skip(reader, entry->bits);

		/* Succeeded: the first-level entry's symbol. */
		return entry->value;
	}

	/* A link: the first level's bits are used, and the next bits choose within the second level. */
	webpc_lbits_skip(reader, code->root_bits);
	table = table + entry->value;
	bits = webpc_lbits_peek(reader, entry->bits);
	entry = &table[bits];

	/* The symbol of the second level uses the rest of its code. */
	webpc_lbits_skip(reader, entry->bits);

	/* Succeeded: the second-level entry's symbol. */
	return entry->value;
}

/*
 * Turns the low length bits of code around, the first bit becoming the last.
 */
static unsigned
prefix_reverse(
	unsigned code,
	unsigned length)
{
	unsigned result;
	unsigned index;

	/* Moves each bit, from the lowest up, to the matching place from the top. */
	result = 0;
	for (index = 0; index < length; index++) {
		result = (result << 1) | (code & 1U);
		code >>= 1;
	}

	/* Succeeded: the code with its bits in the order the stream gives them. */
	return result;
}

/*
 * Makes room for total more entries in pool, or reports that they would pass its limit.
 */
static int
prefix_reserve(
	struct webpc_prefix_pool *pool,
	size_t total)
{
	struct webpc_prefix_entry *grown;
	size_t needed;
	size_t capacity;

	/* Refuses a table that would take the pool past its limit. */
	if (total > pool->limit || pool->count > pool->limit - total)
		return ENOMEM;

	/* Nothing to grow when the pool already has room. */
	needed = pool->count + total;
	if (needed <= pool->capacity)
		return 0;

	/* Doubles the room (at least a first allocation's worth), but not past what the limit allows. */
	capacity = pool->capacity * 2U;
	if (capacity < PREFIX_POOL_CAPACITY_MIN)
		capacity = PREFIX_POOL_CAPACITY_MIN;
	if (capacity > pool->limit)
		capacity = pool->limit;
	if (capacity < needed)
		capacity = needed;

	/* Allocates the larger table; the pool keeps its old one when this fails. */
	grown = (struct webpc_prefix_entry *)realloc(pool->entries, capacity * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;

	/* Publishes the larger table. */
	pool->entries = grown;
	pool->capacity = capacity;

	/* Succeeded: total more entries fit. */
	return 0;
}
