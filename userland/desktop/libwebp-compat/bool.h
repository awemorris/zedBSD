/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boolean entropy decoder of the lossy format (RFC 6386 section 7)
 * and the reading of tree-coded values with it (section 8).
 */

#ifndef LIBWEBP_COMPAT_BOOL_H
#define LIBWEBP_COMPAT_BOOL_H

#include "common.h"

/*
 * A decoder over one partition (a buffer it does not own).  Only bool.c
 * reads or writes the fields; the others use the calls.
 *
 *   data, size  The partition.
 *   next        The index of the next byte not yet taken into value.
 *   value       The bits taken; the 8 bits compared with the split are
 *               the top of the window as RFC 6386 7.3 keeps it.
 *   range       The range, 128..255 between calls.
 *   bit_count   The shifts since the last byte was taken (7.3).
 *   shifts      The normalising shifts made since the start (for ended).
 *   ended       Set, and kept, as webpc_bool_ended says.
 *
 * The implementation may keep more bits in value than 7.3 does, as long
 * as every result and ended are as described.
 */
struct webpc_bool {
	const uint8_t *data;
	size_t size;
	size_t next;
	uint32_t value;
	uint32_t range;
	int bit_count;
	uint64_t shifts;
	int ended;
};

/*
 * Starts a decoder at the first byte of data (size bytes, NULL only when
 * size is 0), as RFC 6386 7.3's init_bool_decoder: range 255, the first
 * two bytes in value (a byte past the end is 0).
 */
void webpc_bool_init(struct webpc_bool *decoder, const uint8_t *data, size_t size);

/*
 * Decodes one bool whose probability of being 0 is prob / 256 (prob
 * 0..255) as RFC 6386 7.3's read_bool: split = 1 + (((range - 1) * prob)
 * >> 8); then the comparison, the new range and value, and the
 * normalisation, bytes past the end of the data shifting in as 0.
 * Returns 0 or 1.
 */
int webpc_bool_read(struct webpc_bool *decoder, unsigned prob);

/*
 * Decodes an unsigned literal of bits (0..24) bits, most significant
 * first, each with probability 128 (RFC 6386 8, L(n)).
 */
uint32_t webpc_bool_literal(struct webpc_bool *decoder, unsigned bits);

/*
 * Decodes a signed value as the frame header codes one: a magnitude of
 * bits (1..24) bits (L(n)), then a sign bit (L(1), 1 for negative)
 * (RFC 6386 9.3, 9.6, 19.2).
 */
int32_t webpc_bool_signed(struct webpc_bool *decoder, unsigned bits);

/*
 * Decodes a tree-coded value (RFC 6386 8.1, treed_read) starting at node
 * start (an even index; 0 for the whole tree): at node i, a bool with
 * probability probs[i >> 1] chooses tree[i] or tree[i + 1]; a positive
 * entry is the next node, an entry of 0 or less is the leaf -entry, the
 * value returned.  tree is one of the constant trees of vp8-tables.h
 * (well formed: every path ends in a leaf), probs has an entry for each
 * pair of nodes.
 */
int webpc_bool_tree(struct webpc_bool *decoder, const int8_t *tree, const uint8_t *probs, unsigned start);

/*
 * Tells whether the decoder has read past its data: 1 or 0.  It becomes 1
 * at webpc_bool_init when size is 0, and when a bool (or a bit of a
 * literal) is decoded while the 8 bits of the window compared with the
 * split reach past the last byte, that is, when the decoding starts with
 * shifts + 8 > 8 * size.  A valid partition never does this (its encoder writes
 * enough bytes); libwebp's decoder fails such a frame, and the lossy
 * decoder here fails it with ENOBUFS.
 */
int webpc_bool_ended(const struct webpc_bool *decoder);

#endif
