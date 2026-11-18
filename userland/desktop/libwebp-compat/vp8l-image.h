/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The entropy-coded images of the lossless format (RFC 9649 3.6, 3.7):
 * the colour cache, the meta prefix codes and the entropy image, and the
 * pixels as literals, backward references and cache indices.  The same
 * decoding serves the ARGB image and every subresolution image (the
 * predictor, colour transform, colour indexing and entropy images).
 */

#ifndef LIBWEBP_COMPAT_VP8L_IMAGE_H
#define LIBWEBP_COMPAT_VP8L_IMAGE_H

#include "common.h"
#include "lbits.h"
#include "vp8l-codes.h"

/* The neighbourhood distance codes (RFC 9649 3.6.2.2.1: codes 1 to 120). */
#define WEBPC_VP8L_NEAR_CODES		120U

/* The longest backward reference (RFC 9649 3.6.2.2: 4096 pixels). */
#define WEBPC_VP8L_LENGTH_MAX		4096U

/* The multiplier of the colour cache's hash (RFC 9649 3.6.2.3). */
#define WEBPC_VP8L_CACHE_MULTIPLIER	0x1e35a7bdU

/*
 * Decodes one entropy-coded image of width by height pixels (each 1 or
 * more; width * height at most WEBPC_VP8L_SIDE_MAX squared) from reader
 * into pixels (width * height ARGB values, alpha in bits 31..24, red
 * 23..16, green 15..8, blue 7..0, rows top to bottom; the caller's
 * buffer), as RFC 9649 3.8.3 lays it out:
 *
 *   - color-cache-info: 1 bit, then when set 4 bits of cache_bits, which
 *     must be 1..11 (else EINVAL);
 *   - meta-prefix, only when argb_role is 1 (the main ARGB image, RFC
 *     9649 3.7.2.2): 1 bit; when set, prefix_bits = 2 + (3 bits) and an
 *     entropy image of DIV_ROUND_UP(width, 1 << prefix_bits) by
 *     DIV_ROUND_UP(height, 1 << prefix_bits) pixels decoded by this same
 *     call with argb_role 0; each of its pixels names the group of its
 *     block, (pixel >> 8) & 0xffff, and the groups are as many as the
 *     largest of those plus one.  Without it (and always for argb_role 0)
 *     one group serves the whole image.
 *   - the groups (webpc_vp8l_read_group), in order.  Every group is read;
 *     a group no block names is read into a scratch pool of its own (with
 *     the same limit, emptied after each such group), so the memory kept
 *     is that of the groups in use, which are counted against limit table
 *     entries (more is ENOMEM); the entropy image decoded inside has a
 *     limit of its own, the same value.
 *   - the pixels in scan-line order (3.7.2.3): a green symbol S below 256
 *     is a literal (red, blue, alpha follow with their codes); 256..279 a
 *     backward reference, length from webpc_vp8l_lz77_value(S - 256),
 *     then a distance symbol and webpc_vp8l_lz77_value of it, made a
 *     distance in pixels by webpc_vp8l_plane_distance; 280 and above an
 *     index into the colour cache.  A reference whose distance reaches
 *     before the first pixel, or whose length passes the last pixel, is
 *     EINVAL.  Every pixel produced (by a literal, a copy or the cache) is
 *     put into the cache in order (3.6.2.3), the cache starting all 0.
 *     Each symbol is read with the group of the block of the pixel it
 *     starts at (after a copy, the block of the pixel after the copy).
 *
 * The reader running past the end is ENOBUFS (checked at least after the
 * codes, after each row and at the end).  Returns 0 with every pixel
 * written; on failure the pixels are unspecified.  Everything allocated
 * (cache, entropy image, groups, pools) is freed before returning.
 */
int webpc_vp8l_decode_image(struct webpc_lbits *reader, unsigned width, unsigned height, int argb_role,
	size_t limit, uint32_t *pixels);

/*
 * Reads the value a length or distance prefix code prefix_code (0..39)
 * stands for, with its extra bits from reader (RFC 9649 3.6.2.2): codes 0
 * to 3 are 1 to 4; above, extra_bits = (prefix_code - 2) >> 1 and the
 * value is ((2 + (prefix_code & 1)) << extra_bits) + (extra_bits bits) + 1.
 * The result is 1..1048576.
 */
uint32_t webpc_vp8l_lz77_value(struct webpc_lbits *reader, unsigned prefix_code);

/*
 * Turns a distance code (1 or more) into a distance in pixels for an
 * image width pixels wide (RFC 9649 3.6.2.2.1): codes 1..120 name the
 * neighbour (xi, yi) of Figure 20 and give xi + yi * width, at least 1;
 * a larger code gives code - 120.
 */
size_t webpc_vp8l_plane_distance(uint32_t distance_code, unsigned width);

/*
 * Gives the colour cache index of argb for a cache of cache_bits bits
 * (1..11): (0x1e35a7bd * argb) >> (32 - cache_bits), the product taken
 * modulo 2^32 (RFC 9649 3.6.2.3).
 */
unsigned webpc_vp8l_cache_index(uint32_t argb, unsigned cache_bits);

#endif
