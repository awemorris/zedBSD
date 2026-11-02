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
	UNUSED_PARAMETER(reader);
	UNUSED_PARAMETER(pool);
	UNUSED_PARAMETER(alphabet_size);
	UNUSED_PARAMETER(code);

	/* Not implemented yet. */
	return ENOSYS;
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
	UNUSED_PARAMETER(reader);
	UNUSED_PARAMETER(pool);
	UNUSED_PARAMETER(cache_bits);
	UNUSED_PARAMETER(group);

	/* Not implemented yet. */
	return ENOSYS;
}
