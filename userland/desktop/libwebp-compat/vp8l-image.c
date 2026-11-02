/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The entropy-coded images of the lossless format: colour cache, meta prefix
 * codes and the pixels.
 */

#include "vp8l-image.h"

#include <errno.h>

/*
 * Decodes one entropy-coded image of width by height pixels (each 1 or more;
 * width * height at most WEBPC_VP8L_SIDE_MAX squared) from reader into
 * pixels (width * height ARGB values, alpha in bits 31..24, red 23..16,
 * green 15..8, blue 7..0, rows top to bottom; the caller's buffer), as RFC
 * 9649 3.8.3 lays it out: - color-cache-info: 1 bit, then when set 4 bits of
 * cache_bits, which must be 1..11 (else EINVAL); - meta-prefix, only when
 * argb_role is 1 (the main ARGB image, RFC 9649 3.7.2.2): 1 bit; when set,
 * prefix_bits = 2 + (3 bits) and an entropy image of DIV_ROUND_UP(width, 1
 * << prefix_bits) by DIV_ROUND_UP(height, 1 << prefix_bits) pixels decoded
 * by this same call with argb_role 0; each of its pixels names the group of
 * its block, (pixel >> 8) & 0xffff, and the groups are as many as the
 * largest of those plus one.
 */
int
webpc_vp8l_decode_image(
	struct webpc_lbits *reader,
	unsigned width,
	unsigned height,
	int argb_role,
	size_t limit,
	uint32_t *pixels)
{
	UNUSED_PARAMETER(reader);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(argb_role);
	UNUSED_PARAMETER(limit);
	UNUSED_PARAMETER(pixels);

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Reads the value a length or distance prefix code prefix_code (0..39)
 * stands for, with its extra bits from reader (RFC 9649 3.6.2.2): codes 0 to
 * 3 are 1 to 4; above, extra_bits = (prefix_code - 2) >> 1 and the value is
 * ((2 + (prefix_code & 1)) << extra_bits) + (extra_bits bits) + 1.
 */
uint32_t
webpc_vp8l_lz77_value(
	struct webpc_lbits *reader,
	unsigned prefix_code)
{
	UNUSED_PARAMETER(reader);
	UNUSED_PARAMETER(prefix_code);

	/* Not implemented yet. */
	return 0;
}

/*
 * Turns a distance code (1 or more) into a distance in pixels for an image
 * width pixels wide (RFC 9649 3.6.2.2.1): codes 1..120 name the neighbour
 * (xi, yi) of Figure 20 and give xi + yi * width, at least 1; a larger code
 * gives code - 120.
 */
size_t
webpc_vp8l_plane_distance(
	uint32_t distance_code,
	unsigned width)
{
	UNUSED_PARAMETER(distance_code);
	UNUSED_PARAMETER(width);

	/* Not implemented yet. */
	return 0;
}

/*
 * Gives the colour cache index of argb for a cache of cache_bits bits
 * (1..11): (0x1e35a7bd * argb) >> (32 - cache_bits), the product taken
 * modulo 2^32 (RFC 9649 3.6.2.3).
 */
unsigned
webpc_vp8l_cache_index(
	uint32_t argb,
	unsigned cache_bits)
{
	UNUSED_PARAMETER(argb);
	UNUSED_PARAMETER(cache_bits);

	/* Not implemented yet. */
	return 0;
}
