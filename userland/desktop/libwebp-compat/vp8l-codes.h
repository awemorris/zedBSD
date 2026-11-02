/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the prefix codes of the lossless format: one code from its
 * simple or normal code length code, and the group of five codes one
 * block of an image is decoded with.
 */

#ifndef LIBWEBP_COMPAT_VP8L_CODES_H
#define LIBWEBP_COMPAT_VP8L_CODES_H

#include "common.h"
#include "lbits.h"
#include "prefix.h"

/* The literal symbols of a channel, and the length and distance prefix codes (RFC 9649 3.6.2.2, 3.7.2.1.2). */
#define WEBPC_VP8L_LITERALS		256U
#define WEBPC_VP8L_LENGTH_CODES		24U
#define WEBPC_VP8L_DISTANCE_CODES	40U

/* The largest colour cache, in bits of index (RFC 9649 3.6.2.3: 1 to 11). */
#define WEBPC_VP8L_CACHE_BITS_MAX	11U

/* The symbols of the code length code (RFC 9649 3.7.2.1.2: kCodeLengthCodes). */
#define WEBPC_VP8L_CODE_LENGTH_CODES	19U

/* The codes of a group, in their order in the bitstream (RFC 9649 3.7.2). */
#define WEBPC_VP8L_CODE_GREEN		0U
#define WEBPC_VP8L_CODE_RED		1U
#define WEBPC_VP8L_CODE_BLUE		2U
#define WEBPC_VP8L_CODE_ALPHA		3U
#define WEBPC_VP8L_CODE_DISTANCE	4U
#define WEBPC_VP8L_GROUP_CODES		5U

/*
 * The five codes one block is decoded with (a prefix code group, RFC 9649
 * 3.7.2): green with the length prefixes and the cache indices, red,
 * blue, alpha and distance, indexed by WEBPC_VP8L_CODE_*.  Their tables
 * are in the pool they were built in.
 */
struct webpc_vp8l_group {
	struct webpc_prefix_code codes[WEBPC_VP8L_GROUP_CODES];
};

/*
 * Reads one prefix code (RFC 9649 3.7.2.1) for an alphabet of
 * alphabet_size symbols (1..WEBPC_PREFIX_ALPHABET_MAX) and builds it into
 * pool.
 *
 * The first bit chooses the simple code length code (1) or the normal one
 * (0):
 *
 *   - simple (3.7.2.1.1): one or two symbols of length 1; the first is
 *     read with 1 or 8 bits as is_first_8bits says, the second with 8.  A
 *     symbol not below alphabet_size is left out of the code (RFC 9649
 *     does not say; libwebp reads it so); no symbol left is EINVAL.  Two
 *     equal symbols make a code of that one symbol.
 *   - normal (3.7.2.1.2): num_code_lengths (4 + 4 bits) lengths of 3 bits
 *     in kCodeLengthCodeOrder (17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9,
 *     10, 11, 12, 13, 14, 15) build the code length code (it must be a
 *     valid code by webpc_prefix_build's rule, else EINVAL).  Then one bit:
 *     0, max_symbol is alphabet_size; 1, length_nbits = 2 + 2 * (3 bits)
 *     and max_symbol = 2 + (length_nbits bits), EINVAL when it is above
 *     alphabet_size.  Then code length symbols are read, filling the
 *     lengths from symbol 0 up, until alphabet_size lengths are filled or
 *     max_symbol code length symbols have been read: max_symbol counts the
 *     symbols read from the code length code (a repeat counts once,
 *     however many lengths it fills), as libwebp's encoder writes it; RFC
 *     9649's "read up to max_symbol code lengths" is read this way.  0..15
 *     is a length (a non-zero one becomes the length 16 repeats); 16
 *     repeats the last non-zero length (8 when none was read yet) 3 + (2
 *     bits) times; 17 gives 3 + (3 bits) zeros; 18 gives 11 + (7 bits)
 *     zeros.  A repeat that would pass alphabet_size is EINVAL.  The
 *     lengths not filled are 0.
 *
 * The code is then built (webpc_prefix_build: an incomplete code, or one
 * with no symbol, is EINVAL).  The reader running past the end is ENOBUFS.
 * Returns 0 and fills code; on failure code is unspecified and pool is as
 * it was.
 */
int webpc_vp8l_read_code(struct webpc_lbits *reader, struct webpc_prefix_pool *pool, unsigned alphabet_size,
	struct webpc_prefix_code *code);

/*
 * Reads the five codes of one group (RFC 9649 3.7.2.2.2, 3.8.3) for a
 * colour cache of cache_bits bits (0 for none, else 1..11): the green
 * code's alphabet is 256 + 24 + (1 << cache_bits) (256 + 24 without a
 * cache), red, blue and alpha 256, distance 40.  Errors as
 * webpc_vp8l_read_code; on failure group is unspecified and pool is as it
 * was before the call.
 */
int webpc_vp8l_read_group(struct webpc_lbits *reader, struct webpc_prefix_pool *pool, unsigned cache_bits,
	struct webpc_vp8l_group *group);

#endif
