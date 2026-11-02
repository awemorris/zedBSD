/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boolean entropy decoder of the lossy format and its tree-coded values.
 */

#include "bool.h"

#include <errno.h>

/*
 * Starts a decoder at the first byte of data (size bytes, NULL only when
 * size is 0), as RFC 6386 7.3's init_bool_decoder: range 255, the first two
 * bytes in value (a byte past the end is 0).
 */
void
webpc_bool_init(
	struct webpc_bool *decoder,
	const uint8_t *data,
	size_t size)
{
	UNUSED_PARAMETER(decoder);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);

	/* Not implemented yet. */
}

/*
 * Decodes one bool whose probability of being 0 is prob / 256 (prob 0..255)
 * as RFC 6386 7.3's read_bool: split = 1 + (((range - 1) * prob) >> 8); then
 * the comparison, the new range and value, and the normalisation, bytes past
 * the end of the data shifting in as 0.
 */
int
webpc_bool_read(
	struct webpc_bool *decoder,
	unsigned prob)
{
	UNUSED_PARAMETER(decoder);
	UNUSED_PARAMETER(prob);

	/* Not implemented yet. */
	return 0;
}

/*
 * Decodes an unsigned literal of bits (0..24) bits, most significant first,
 * each with probability 128 (RFC 6386 8, L(n)).
 */
uint32_t
webpc_bool_literal(
	struct webpc_bool *decoder,
	unsigned bits)
{
	UNUSED_PARAMETER(decoder);
	UNUSED_PARAMETER(bits);

	/* Not implemented yet. */
	return 0;
}

/*
 * Decodes a signed value as the frame header codes one: a magnitude of bits
 * (1..24) bits (L(n)), then a sign bit (L(1), 1 for negative) (RFC 6386 9.3,
 * 9.6, 19.2).
 */
int32_t
webpc_bool_signed(
	struct webpc_bool *decoder,
	unsigned bits)
{
	UNUSED_PARAMETER(decoder);
	UNUSED_PARAMETER(bits);

	/* Not implemented yet. */
	return 0;
}

/*
 * Decodes a tree-coded value (RFC 6386 8.1, treed_read) starting at node
 * start (an even index; 0 for the whole tree): at node i, a bool with
 * probability probs[i >> 1] chooses tree[i] or tree[i + 1]; a positive entry
 * is the next node, an entry of 0 or less is the leaf -entry, the value
 * returned.
 */
int
webpc_bool_tree(
	struct webpc_bool *decoder,
	const int8_t *tree,
	const uint8_t *probs,
	unsigned start)
{
	UNUSED_PARAMETER(decoder);
	UNUSED_PARAMETER(tree);
	UNUSED_PARAMETER(probs);
	UNUSED_PARAMETER(start);

	/* Not implemented yet. */
	return 0;
}

/*
 * Tells whether the decoder has read past its data: 1 or 0.
 */
int
webpc_bool_ended(
	const struct webpc_bool *decoder)
{
	UNUSED_PARAMETER(decoder);

	/* Not implemented yet. */
	return 0;
}
