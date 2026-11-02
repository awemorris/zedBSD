/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The canonical prefix codes of the lossless format: building a code's
 * lookup table from its code lengths, and reading one symbol with it.
 * The tables of all the codes of one image live in one pool, so their
 * memory is counted and limited in one place.
 */

#ifndef LIBWEBP_COMPAT_PREFIX_H
#define LIBWEBP_COMPAT_PREFIX_H

#include "common.h"
#include "lbits.h"

/* The longest code (RFC 9649 3.7.2.1.2: lengths 1 to 15). */
#define WEBPC_PREFIX_LENGTH_MAX		15U

/* The most bits the first-level table of a code is indexed by. */
#define WEBPC_PREFIX_ROOT_BITS		8U

/*
 * The largest alphabet: the green alphabet with the largest colour cache,
 * 256 + 24 + 2048 (RFC 9649 3.7.2.1.2).
 */
#define WEBPC_PREFIX_ALPHABET_MAX	2328U

/*
 * One entry of a lookup table.  Only prefix.c writes or interprets the
 * entries; their meaning is:
 *
 *   kind 0, a symbol: value is the symbol, bits is the number of bits of
 *     the code that this table level consumes (for a first-level entry the
 *     code's length; for a second-level entry the length minus the first
 *     level's bits);
 *   kind 1, a link from the first level: value is the offset, in entries
 *     from the start of the code's first-level table, of a second-level
 *     table indexed by the next bits bits.
 */
struct webpc_prefix_entry {
	uint16_t value;
	uint8_t bits;
	uint8_t kind;
};

/*
 * The storage of the tables of one image's codes.  entries holds count
 * entries in use of capacity allocated (realloc'ed as codes are built, so
 * a code refers to its table by offset, never by pointer); limit is the
 * most count may reach (WEBPC_VP8L_TABLE_ENTRIES_MAX or less).  The pool
 * owns entries; webpc_prefix_pool_release frees it.
 */
struct webpc_prefix_pool {
	struct webpc_prefix_entry *entries;
	size_t count;
	size_t capacity;
	size_t limit;
};

/*
 * One built code.  A code of one symbol reads no bits: root_bits is 0 and
 * single is the symbol (offset unused).  Otherwise root_bits is 1 to
 * WEBPC_PREFIX_ROOT_BITS and offset is the index in the pool of its
 * first-level table of 1 << root_bits entries, followed by its
 * second-level tables.  The code does not own its table; the pool does.
 */
struct webpc_prefix_code {
	size_t offset;
	unsigned root_bits;
	unsigned single;
};

/* Starts an empty pool whose tables may hold at most limit entries in all. */
void webpc_prefix_pool_init(struct webpc_prefix_pool *pool, size_t limit);

/* Frees a pool's tables and leaves it empty (and usable again). */
void webpc_prefix_pool_release(struct webpc_prefix_pool *pool);

/*
 * Builds the canonical prefix code whose code lengths are lengths[0..count
 * - 1] (count 1..WEBPC_PREFIX_ALPHABET_MAX, each length 0..15, 0 for a
 * symbol absent from the code) and appends its tables to pool.
 *
 * The codes are assigned as RFC 1951 3.2.2 assigns them (the format
 * names the code canonical and libwebp's encoder writes it so; RFC 9649
 * does not spell the assignment out): shorter codes before longer ones,
 * and within a length in the order of the symbols.  The first bit of a
 * code read from the stream (webpc_lbits_read(reader, 1)) is its most
 * significant bit.
 *
 * The code must be complete (the sum of 2^-length over the present
 * symbols is exactly 1), or have exactly one present symbol, of any length
 * 1 to 15: that symbol is then read with no bits (RFC 9649 3.7.2.1).  No
 * present symbol, a length above 15, an over-subscribed or an incomplete
 * code is EINVAL.  A table that would take pool past its limit, or a
 * failed allocation, is ENOMEM.
 *
 * Returns 0 and fills code; on failure code is unspecified and pool is as
 * it was before the call (count restored).
 */
int webpc_prefix_build(struct webpc_prefix_pool *pool, const uint8_t *lengths, unsigned count,
	struct webpc_prefix_code *code);

/*
 * Reads one symbol with a code built in pool, consuming its bits from
 * reader.  The result is always one of the code's present symbols (a read
 * past the end of the data gives the symbol its zero bits select, and
 * sets the reader's consumed_end).
 */
unsigned webpc_prefix_read(const struct webpc_prefix_pool *pool, const struct webpc_prefix_code *code,
	struct webpc_lbits *reader);

#endif
