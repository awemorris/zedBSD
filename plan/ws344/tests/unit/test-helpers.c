/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of the test helpers themselves: lbitwriter.h against bytes
 * worked out by hand from RFC 9649 3.2, and boolwriter.h against a
 * decoder written here from RFC 6386 7.3 (not the library's).
 */

#include "check.h"
#include "lbitwriter.h"
#include "boolwriter.h"

/* A plain decoder of RFC 6386 7.3, only for checking the writer. */
struct plain_decoder {
	const uint8_t *data;
	size_t size;
	size_t next;
	uint32_t value;
	uint32_t range;
	int bit_count;
};

static void plain_init(struct plain_decoder *decoder, const uint8_t *data, size_t size);
static int plain_read(struct plain_decoder *decoder, unsigned prob);
static void test_lbitwriter(void);
static void test_boolwriter(void);
static void test_tree(void);

/*
 * Runs the helper tests.
 */
int
main(
	void)
{
	int status;

	/* Each helper in turn. */
	test_lbitwriter();
	test_boolwriter();
	test_tree();

	/* The summary line. */
	status = check_finish("helpers");
	return status;
}

/* Starts the plain decoder as init_bool_decoder does (bytes past the end are 0). */
static void
plain_init(
	struct plain_decoder *decoder,
	const uint8_t *data,
	size_t size)
{
	unsigned index;

	/* The first two bytes into value. */
	decoder->data = data;
	decoder->size = size;
	decoder->next = 0;
	decoder->value = 0;
	for (index = 0; index < 2U; index++) {
		decoder->value <<= 8;
		if (decoder->next < size)
			decoder->value |= data[decoder->next];
		decoder->next++;
	}
	decoder->range = 255U;
	decoder->bit_count = 0;
}

/* Reads one bool as read_bool does. */
static int
plain_read(
	struct plain_decoder *decoder,
	unsigned prob)
{
	uint32_t split;
	int bit;

	/* The comparison with the split, shifted to the value's top byte. */
	split = 1U + (((decoder->range - 1U) * prob) >> 8);
	if (decoder->value >= (split << 8)) {
		bit = 1;
		decoder->range -= split;
		decoder->value -= split << 8;
	} else {
		bit = 0;
		decoder->range = split;
	}

	/* The normalisation, a byte every 8 shifts. */
	while (decoder->range < 128U) {
		decoder->value <<= 1;
		decoder->range <<= 1;
		decoder->bit_count++;
		if (decoder->bit_count == 8) {
			decoder->bit_count = 0;
			if (decoder->next < decoder->size)
				decoder->value |= decoder->data[decoder->next];
			decoder->next++;
		}
	}
	return bit;
}

/* Checks lbitwriter's bit order and codewords against bytes worked out by hand. */
static void
test_lbitwriter(void)
{
	static struct lbitwriter writer;

	/* ReadBits(2) of value 2 then ReadBits(3) of 5: bits 0,1 then 1,0,1 -> byte 0b10110 = 0x16. */
	lbitwriter_init(&writer);
	lbitwriter_put(&writer, 2U, 2U);
	lbitwriter_put(&writer, 5U, 3U);
	CHECK(lbitwriter_size(&writer) == 1U);
	CHECK(writer.bytes[0] == 0x16U);

	/* 14 bits of 0x3fff then 1 bit: the first byte 0xff, the second 0x7f (14 - 8 = 6 ones and the seventh). */
	lbitwriter_init(&writer);
	lbitwriter_put(&writer, 0x3fffU, 14U);
	lbitwriter_put(&writer, 1U, 1U);
	CHECK(writer.bytes[0] == 0xffU);
	CHECK(writer.bytes[1] == 0x7fU);
	CHECK(lbitwriter_size(&writer) == 2U);

	/* A codeword 110 (3 bits) is written 1, 1, 0 from bit 0: byte 0x03. */
	lbitwriter_init(&writer);
	lbitwriter_put_code(&writer, 6U, 3U);
	CHECK(writer.bytes[0] == 0x03U);
	CHECK(writer.bits == 3U);

	/* The VP8L header of a 1x1 image, no alpha, version 0: 0x2f, then 28 bits of zero sizes and 4 bits 0. */
	lbitwriter_init(&writer);
	lbitwriter_put(&writer, 0x2fU, 8U);
	lbitwriter_put(&writer, 0U, 14U);
	lbitwriter_put(&writer, 0U, 14U);
	lbitwriter_put(&writer, 0U, 1U);
	lbitwriter_put(&writer, 0U, 3U);
	CHECK(lbitwriter_size(&writer) == 5U);
	CHECK(writer.bytes[0] == 0x2fU && writer.bytes[1] == 0U && writer.bytes[4] == 0U);
	CHECK(writer.overflow == 0);
}

/* Checks that what boolwriter writes reads back with the plain decoder, at every probability. */
static void
test_boolwriter(void)
{
	static struct boolwriter writer;
	static int bits[20000];
	static unsigned probs[20000];
	struct plain_decoder decoder;
	unsigned long state;
	unsigned index;
	unsigned count;
	size_t size;
	int mismatches;
	uint32_t value;
	int sign;

	/* Random bools at random probabilities, some at the extremes. */
	state = 12345UL;
	count = 20000U;
	boolwriter_init(&writer);
	for (index = 0; index < count; index++) {
		probs[index] = (unsigned)(check_random(&state) & 255UL);
		if (index % 97U == 0U)
			probs[index] = 1U;
		if (index % 89U == 0U)
			probs[index] = 255U;
		bits[index] = (check_random(&state) & 3UL) == 0UL;
		boolwriter_put(&writer, probs[index], bits[index]);
	}
	size = boolwriter_finish(&writer);
	CHECK(writer.overflow == 0);
	CHECK(size > 0U);

	/* Each bool reads back as written. */
	plain_init(&decoder, writer.bytes, size);
	mismatches = 0;
	for (index = 0; index < count; index++) {
		if (plain_read(&decoder, probs[index]) != bits[index])
			mismatches++;
	}
	CHECK(mismatches == 0);

	/* The decoder never needed a byte the writer did not write (the window stayed within the data). */
	CHECK(decoder.next <= size + 1U);

	/* Literals and signed values in the header's form. */
	boolwriter_init(&writer);
	boolwriter_literal(&writer, 0x5aU, 8U);
	boolwriter_signed(&writer, -37, 7U);
	boolwriter_signed(&writer, 12, 4U);
	size = boolwriter_finish(&writer);

	/* The literal, then the two signed values. */
	plain_init(&decoder, writer.bytes, size);
	value = 0;
	for (index = 0; index < 8U; index++)
		value = (value << 1) | (uint32_t)plain_read(&decoder, 128U);
	CHECK(value == 0x5aU);
	value = 0;
	for (index = 0; index < 7U; index++)
		value = (value << 1) | (uint32_t)plain_read(&decoder, 128U);
	sign = plain_read(&decoder, 128U);
	CHECK(value == 37U && sign == 1);
	value = 0;
	for (index = 0; index < 4U; index++)
		value = (value << 1) | (uint32_t)plain_read(&decoder, 128U);
	sign = plain_read(&decoder, 128U);
	CHECK(value == 12U && sign == 0);
}

/* Checks the tree writer with RFC 6386 8.1's example tree shape (a leaf of 0 included). */
static void
test_tree(void)
{
	/* A tree of four leaves 0..3: "0" -> 0, "10" -> 1, "110" -> 2, "111" -> 3. */
	static const int8_t tree[6] = { -0, 2, -1, 4, -2, -3 };
	static const uint8_t probs[3] = { 100U, 150U, 200U };
	static struct boolwriter writer;
	struct plain_decoder decoder;
	int value;
	int read;
	int node;
	size_t size;

	/* Every leaf written, then read back by walking the tree with the plain decoder. */
	boolwriter_init(&writer);
	for (value = 0; value < 4; value++)
		CHECK(boolwriter_tree(&writer, tree, 6U, probs, 0U, value) == 0);
	CHECK(boolwriter_tree(&writer, tree, 6U, probs, 0U, 7) == 1);
	size = boolwriter_finish(&writer);
	plain_init(&decoder, writer.bytes, size);
	for (value = 0; value < 4; value++) {
		node = 0;
		do {
			node = tree[node + plain_read(&decoder, probs[node >> 1])];
		} while (node > 0);
		read = -node;
		CHECK(read == value);
	}
}
