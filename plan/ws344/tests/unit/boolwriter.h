/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A boolean entropy encoder for the tests of the lossy format, written
 * from RFC 6386 7.1 to 7.3 (the encoder is the inverse of the decoder
 * the library implements): tests build partitions with it and read them
 * back with webpc_bool_*.  It never uses the library's code.
 */

#ifndef WEBPC_TEST_BOOLWRITER_H
#define WEBPC_TEST_BOOLWRITER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The most bytes a test partition holds. */
#define BOOLWRITER_BYTES	65536U

/*
 * A partition being written: the bytes written so far (count), the
 * interval's low end (low, 32 bits of which the top byte is next out)
 * and range (128..255), the shifts left before a byte is ready, and
 * overflow when a write passed the end.
 */
struct boolwriter {
	uint8_t bytes[BOOLWRITER_BYTES];
	size_t count;
	uint32_t low;
	uint32_t range;
	int shifts_left;
	int overflow;
};

/* Starts an empty partition (RFC 6386 7.3: range 255, 24 shifts before the first byte). */
static __inline void
boolwriter_init(
	struct boolwriter *writer)
{
	/* An empty buffer and the starting interval. */
	memset(writer, 0, sizeof(*writer));
	writer->range = 255U;
	writer->shifts_left = 24;
}

/* Adds a carry into the bytes already written (it never runs past the first). */
static __inline void
boolwriter_carry(
	struct boolwriter *writer)
{
	size_t index;

	/* Bytes of 255 become 0 until one takes the carry. */
	index = writer->count;
	while (index > 0U) {
		index--;
		if (writer->bytes[index] != 255U) {
			writer->bytes[index]++;
			return;
		}
		writer->bytes[index] = 0U;
	}
}

/* Writes one bool whose probability of being 0 is prob / 256. */
static __inline void
boolwriter_put(
	struct boolwriter *writer,
	unsigned prob,
	int value)
{
	uint32_t split;

	/* The split of the interval, as the decoder will compute it. */
	split = 1U + (((writer->range - 1U) * prob) >> 8);
	if (value) {
		writer->low += split;
		writer->range -= split;
	} else {
		writer->range = split;
	}

	/* Normalises the range, moving out a byte every 8 shifts. */
	while (writer->range < 128U) {
		writer->range <<= 1;
		if ((writer->low & 0x80000000U) != 0U)
			boolwriter_carry(writer);
		writer->low <<= 1;
		writer->shifts_left--;
		if (writer->shifts_left == 0) {
			if (writer->count >= BOOLWRITER_BYTES) {
				writer->overflow = 1;
				return;
			}
			writer->bytes[writer->count] = (uint8_t)(writer->low >> 24);
			writer->count++;
			writer->low &= 0x00ffffffU;
			writer->shifts_left = 8;
		}
	}
}

/* Writes an unsigned literal of count bits, most significant first, each with probability 128. */
static __inline void
boolwriter_literal(
	struct boolwriter *writer,
	uint32_t value,
	unsigned count)
{
	/* From the top bit of the literal down. */
	while (count > 0U) {
		count--;
		boolwriter_put(writer, 128U, (int)((value >> count) & 1U));
	}
}

/* Writes a signed header value: magnitude of count bits, then the sign (1 negative). */
static __inline void
boolwriter_signed(
	struct boolwriter *writer,
	int32_t value,
	unsigned count)
{
	/* The magnitude, then whether it is negative. */
	if (value < 0) {
		boolwriter_literal(writer, (uint32_t)(-value), count);
		boolwriter_put(writer, 128U, 1);
	} else {
		boolwriter_literal(writer, (uint32_t)value, count);
		boolwriter_put(writer, 128U, 0);
	}
}

/*
 * Finds the path from node to the leaf -value in tree (entries as RFC 6386
 * 8.1 writes them): fills nodes[] and branches[] from depth on and gives
 * the path's full length, or -1 when the leaf is not below node.
 */
static __inline int
boolwriter_tree_path(
	const int8_t *tree,
	unsigned tree_size,
	unsigned node,
	int value,
	unsigned *nodes,
	int *branches,
	int depth)
{
	int branch;
	int entry;
	int length;

	/* A path longer than any tree of the format means a malformed tree. */
	if (depth >= 32 || node >= tree_size)
		return -1;

	/* Tries both branches: a leaf that is the value ends the path, a node is searched below. */
	for (branch = 0; branch < 2; branch++) {
		if (node + (unsigned)branch >= tree_size)
			return -1;
		entry = tree[node + (unsigned)branch];
		nodes[depth] = node;
		branches[depth] = branch;
		if (entry <= 0) {
			if (-entry == value)
				return depth + 1;
			continue;
		}
		length = boolwriter_tree_path(tree, tree_size, (unsigned)entry, value, nodes, branches, depth + 1);
		if (length > 0)
			return length;
	}
	return -1;
}

/*
 * Writes a tree-coded value from node start (0 for the whole tree): each
 * branch on the way to the leaf -value with probs[node >> 1].  Returns 0,
 * or 1 when the value is not a leaf below start.
 */
static __inline int
boolwriter_tree(
	struct boolwriter *writer,
	const int8_t *tree,
	unsigned tree_size,
	const uint8_t *probs,
	unsigned start,
	int value)
{
	unsigned nodes[32];
	int branches[32];
	int length;
	int index;

	/* The path, then its branches in order. */
	length = boolwriter_tree_path(tree, tree_size, start, value, nodes, branches, 0);
	if (length <= 0)
		return 1;
	for (index = 0; index < length; index++)
		boolwriter_put(writer, probs[nodes[index] >> 1], branches[index]);
	return 0;
}

/*
 * Ends the partition (RFC 6386 7.3 flush_bool_encoder): the carry, then
 * the low end's remaining bytes, padded so a decoder never needs bytes
 * past the end.  Returns the partition's size.
 */
static __inline size_t
boolwriter_finish(
	struct boolwriter *writer)
{
	uint32_t low;
	int shifts;
	int index;

	/* The pending carry, then the bits still in low moved to its top. */
	shifts = writer->shifts_left;
	low = writer->low;
	if (shifts < 32 && (low & (1UL << (32 - shifts))) != 0U)
		boolwriter_carry(writer);
	low <<= (unsigned)(shifts & 7);
	for (index = shifts >> 3; index > 0; index--)
		low <<= 8;

	/* Four more bytes from the top of low. */
	for (index = 0; index < 4; index++) {
		if (writer->count >= BOOLWRITER_BYTES) {
			writer->overflow = 1;
			break;
		}
		writer->bytes[writer->count] = (uint8_t)(low >> 24);
		writer->count++;
		low <<= 8;
	}
	return writer->count;
}

#endif
