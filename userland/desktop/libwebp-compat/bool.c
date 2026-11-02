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

/* The most bits one literal or one magnitude holds. */
#define BOOL_LITERAL_BITS_MAX	24U

/* The most nodes a tree walk visits before it gives up (the formats' trees are far shallower). */
#define BOOL_TREE_STEPS_MAX	64U

static uint32_t bool_byte_at(const struct webpc_bool *decoder, size_t index);
static void bool_update_ended(struct webpc_bool *decoder);

/*
 * Gives the byte at index of the partition, or 0 past its end.
 */
static uint32_t
bool_byte_at(
	const struct webpc_bool *decoder,
	size_t index)
{
	uint32_t byte;

	/* A byte past the end reads as 0, so no read leaves the partition. */
	if (index >= decoder->size) {
		return 0U;
	}

	/* A byte inside the partition. */
	byte = (uint32_t)decoder->data[index];
	return byte;
}

/*
 * Sets ended when the 8 bits of the window about to be compared reach past
 * the last byte (shifts + 8 > 8 * size).  Once set it stays set.
 */
static void
bool_update_ended(
	struct webpc_bool *decoder)
{
	uint64_t window_end;
	uint64_t data_bits;

	/* The first bit after the window and the number of bits the data holds. */
	window_end = decoder->shifts + 8U;
	data_bits = (uint64_t)decoder->size * 8U;

	/* The window needs a bit the data does not have: the decoding has run out. */
	if (window_end > data_bits) {
		decoder->ended = 1;
	}
}

/*
 * Starts a decoder at the first byte of data (size bytes, NULL only when
 * size is 0), as RFC 6386 7.3's init_bool_decoder: range 255, the first two
 * bytes in value (a byte past the end is 0).
 */
/*
 * Procedure referenced: RFC 6386 (November 2011) section 7.3
 * (init_bool_decoder).
 */
void
webpc_bool_init(
	struct webpc_bool *decoder,
	const uint8_t *data,
	size_t size)
{
	/* The partition and the interval's full range. */
	decoder->data = data;
	decoder->size = size;
	decoder->range = 255U;
	decoder->bit_count = 0;
	decoder->shifts = 0U;
	decoder->ended = 0;

	/* The first two bytes make the window; the next byte to take is the third. */
	decoder->value = (bool_byte_at(decoder, 0U) << 8) | bool_byte_at(decoder, 1U);
	decoder->next = 2U;

	/*
	 * An empty partition has nothing to decode.  A one-byte partition is
	 * not ended yet: its first window is still inside the data.
	 */
	if (size == 0U) {
		decoder->ended = 1;
	}
}

/*
 * Decodes one bool whose probability of being 0 is prob / 256 (prob 0..255)
 * as RFC 6386 7.3's read_bool: split = 1 + (((range - 1) * prob) >> 8); then
 * the comparison, the new range and value, and the normalisation, bytes past
 * the end of the data shifting in as 0.
 */
/*
 * Procedure referenced: RFC 6386 (November 2011) section 7.3 (read_bool).
 */
int
webpc_bool_read(
	struct webpc_bool *decoder,
	unsigned prob)
{
	uint32_t split;
	uint32_t split_value;
	int result;

	/* The decoding that starts with its window past the data has ended. */
	bool_update_ended(decoder);

	/* The split of the interval, as the encoder computed it (prob is 0..255). */
	split = 1U + (((decoder->range - 1U) * (prob & 255U)) >> 8);
	split_value = split << 8;

	/* A value at or above the split is a 1: the interval moves up past the split. */
	if (decoder->value >= split_value) {
		result = 1;
		decoder->range -= split;
		decoder->value -= split_value;
	} else {
		result = 0;
		decoder->range = split;
	}

	/*
	 * Normalises: shifts the window until the range is back to 128..255,
	 * taking a new byte in after every 8 shifts.
	 */
	while (decoder->range < 128U) {
		decoder->value <<= 1;
		decoder->range <<= 1;
		decoder->shifts++;
		decoder->bit_count++;
		if (decoder->bit_count == 8) {
			decoder->bit_count = 0;
			decoder->value |= bool_byte_at(decoder, decoder->next);
			if (decoder->next < decoder->size) {
				decoder->next++;
			}
		}
	}

	/* The decoded bool. */
	return result;
}

/*
 * Decodes an unsigned literal of bits (0..24) bits, most significant first,
 * each with probability 128 (RFC 6386 8, L(n)).
 */
/*
 * Procedure referenced: RFC 6386 (November 2011) section 7.3
 * (read_literal).
 */
uint32_t
webpc_bool_literal(
	struct webpc_bool *decoder,
	unsigned bits)
{
	uint32_t value;
	unsigned count;
	int bit;

	/* A width past 24 is not a width any field has; it is cut to 24. */
	if (bits > BOOL_LITERAL_BITS_MAX) {
		bits = BOOL_LITERAL_BITS_MAX;
	}

	/* Collects the bits from the most significant down. */
	value = 0U;
	for (count = 0U; count < bits; count++) {
		bit = webpc_bool_read(decoder, 128U);
		value = (value << 1) | (uint32_t)bit;
	}

	/* The literal's value. */
	return value;
}

/*
 * Decodes a signed value as the frame header codes one: a magnitude of bits
 * (1..24) bits (L(n)), then a sign bit (L(1), 1 for negative) (RFC 6386 9.3,
 * 9.6, 19.2).
 */
/*
 * Procedure referenced: RFC 6386 (November 2011) sections 9.3 and 19.2
 * (a magnitude, then a sign flag).
 */
int32_t
webpc_bool_signed(
	struct webpc_bool *decoder,
	unsigned bits)
{
	uint32_t magnitude;
	int negative;
	int32_t value;

	/* The magnitude comes first, then the sign. */
	magnitude = webpc_bool_literal(decoder, bits);
	negative = webpc_bool_read(decoder, 128U);

	/* A magnitude of at most 24 bits fits int32_t, so negating it cannot overflow. */
	value = (int32_t)magnitude;
	if (negative) {
		value = -value;
	}

	/* The signed value. */
	return value;
}

/*
 * Decodes a tree-coded value (RFC 6386 8.1, treed_read) starting at node
 * start (an even index; 0 for the whole tree): at node i, a bool with
 * probability probs[i >> 1] chooses tree[i] or tree[i + 1]; a positive entry
 * is the next node, an entry of 0 or less is the leaf -entry, the value
 * returned.
 */
/*
 * Procedure referenced: RFC 6386 (November 2011) section 8.1 (treed_read).
 */
int
webpc_bool_tree(
	struct webpc_bool *decoder,
	const int8_t *tree,
	const uint8_t *probs,
	unsigned start)
{
	unsigned node;
	unsigned steps;
	int branch;
	int entry;

	/* Nodes come in pairs, so a walk starts on an even index. */
	node = start & ~1U;

	/*
	 * Follows the branches down: each bool picks one of the node's two
	 * entries, and an entry above 0 is the next node.  A tree that never
	 * reaches a leaf is cut off after a fixed number of steps.
	 */
	entry = 0;
	for (steps = 0U; steps < BOOL_TREE_STEPS_MAX; steps++) {
		branch = webpc_bool_read(decoder, probs[node >> 1]);
		entry = tree[node + (unsigned)branch];
		if (entry <= 0) {
			break;
		}
		node = (unsigned)entry;
	}

	/* The leaf's value (0 when the walk was cut off before a leaf). */
	if (entry > 0) {
		return 0;
	}
	return -entry;
}

/*
 * Tells whether the decoder has read past its data: 1 or 0.
 */
int
webpc_bool_ended(
	const struct webpc_bool *decoder)
{
	/* The flag set by webpc_bool_init and webpc_bool_read stays set. */
	return decoder->ended;
}
