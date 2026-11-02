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

/*
 * Starts an empty pool whose tables may hold at most limit entries in all.
 */
void
webpc_prefix_pool_init(
	struct webpc_prefix_pool *pool,
	size_t limit)
{
	UNUSED_PARAMETER(pool);
	UNUSED_PARAMETER(limit);

	/* Not implemented yet. */
}

/*
 * Frees a pool's tables and leaves it empty (and usable again).
 */
void
webpc_prefix_pool_release(
	struct webpc_prefix_pool *pool)
{
	UNUSED_PARAMETER(pool);

	/* Not implemented yet. */
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
	UNUSED_PARAMETER(pool);
	UNUSED_PARAMETER(lengths);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(code);

	/* Not implemented yet. */
	return ENOSYS;
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
	UNUSED_PARAMETER(pool);
	UNUSED_PARAMETER(code);
	UNUSED_PARAMETER(reader);

	/* Not implemented yet. */
	return 0;
}
