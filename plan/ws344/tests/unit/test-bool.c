/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of bool.c: bools, literals, signed values and trees read back
 * from partitions the test's own encoder writes, hand-worked decodes, and
 * the point at which ended is set, counted by a model kept in the test.
 */

#include "bool.h"
#include "boolwriter.h"
#include "check.h"

#include <stdlib.h>
#include <string.h>

/* The partition being written; it is large, so it is not on the stack. */
static struct boolwriter writer;

/* The random generator's state. */
static unsigned long random_state = 0x2468ace1UL;

static uint8_t *copy_exact(const uint8_t *source, size_t size);
static unsigned random_below(unsigned limit);
static void test_hand_decode(void);
static void test_bools_roundtrip(void);
static void test_literals(void);
static void test_signed(void);
static void test_tree(void);
static void test_tree_cut_off(void);
static void test_ended_hand(void);
static void test_ended_model(void);

/*
 * Runs the tests of bool.c.
 */
int
main(
	void)
{
	int status;

	/* Each helper in turn. */
	test_hand_decode();
	test_bools_roundtrip();
	test_literals();
	test_signed();
	test_tree();
	test_tree_cut_off();
	test_ended_hand();
	test_ended_model();

	/* The summary line. */
	status = check_finish("bool");
	return status;
}

/*
 * Copies size bytes into a block of exactly that size, so a read past the
 * end is an error the sanitizer sees; a size of 0 gives NULL.
 */
static uint8_t *
copy_exact(
	const uint8_t *source,
	size_t size)
{
	uint8_t *copy;

	/* An empty partition has no buffer. */
	if (size == 0U) {
		return NULL;
	}

	/* A block of exactly the partition's size. */
	copy = malloc(size);
	if (copy == NULL) {
		return NULL;
	}
	memcpy(copy, source, size);
	return copy;
}

/*
 * Gives a pseudo-random number below limit (limit is at least 1).
 */
static unsigned
random_below(
	unsigned limit)
{
	unsigned long number;

	/* One step of the generator, reduced to the limit. */
	number = check_random(&random_state);
	return (unsigned)(number % (unsigned long)limit);
}

/* Checks decodes worked out by hand from RFC 6386 7.3. */
static void
test_hand_decode(void)
{
	static const uint8_t data[4] = { 0x80U, 0x00U, 0x00U, 0x00U };
	static const uint8_t low[4] = { 0x00U, 0x00U, 0x00U, 0x00U };
	struct webpc_bool decoder;
	int index;

	/*
	 * Range 255 and prob 128 give split 128, so the window 0x8000 is a 1 and
	 * leaves range 127 and value 0; one shift makes range 254.  Then split
	 * is 1 + (253 * 128 >> 8) = 127, so the window 0 is a 0, which leaves
	 * range 127 and one more shift to 254, every time.
	 */
	webpc_bool_init(&decoder, data, sizeof(data));
	CHECK(webpc_bool_read(&decoder, 128U) == 1);
	for (index = 0; index < 20; index++) {
		CHECK(webpc_bool_read(&decoder, 128U) == 0);
	}

	/* An all-zero window decodes 0 at any probability, the ends 0 and 255 included. */
	webpc_bool_init(&decoder, low, sizeof(low));
	CHECK(webpc_bool_read(&decoder, 0U) == 0);
	CHECK(webpc_bool_read(&decoder, 255U) == 0);
	CHECK(webpc_bool_read(&decoder, 1U) == 0);

	/* A literal of 4 bits from the pattern 1,0,0,0 is 8; of 0 bits it is 0 and takes nothing. */
	webpc_bool_init(&decoder, data, sizeof(data));
	CHECK(webpc_bool_literal(&decoder, 0U) == 0UL);
	CHECK(webpc_bool_literal(&decoder, 4U) == 8UL);
	CHECK(webpc_bool_literal(&decoder, 1U) == 0UL);
}

/* Checks 20000 random bools at random probabilities, the ends included, read back exactly. */
static void
test_bools_roundtrip(void)
{
	static unsigned probs[20000];
	static int values[20000];
	struct webpc_bool decoder;
	uint8_t *copy;
	size_t size;
	unsigned index;
	unsigned mismatches;
	unsigned round;

	/* Three streams: any probability, probability 0 and 255 only, and bools that mostly follow their probability. */
	for (round = 0U; round < 3U; round++) {
		/* The bools to write. */
		for (index = 0U; index < 20000U; index++) {
			if (round == 1U) {
				probs[index] = (random_below(2U) == 0U) ? 0U : 255U;
				values[index] = (int)random_below(2U);
			} else {
				probs[index] = random_below(256U);
				values[index] = (int)random_below(2U);
				if (round == 2U) {
					values[index] = (random_below(256U) >= probs[index]) ? 1 : 0;
				}
			}
		}

		/* Writes them, then closes the partition. */
		boolwriter_init(&writer);
		for (index = 0U; index < 20000U; index++) {
			boolwriter_put(&writer, probs[index], values[index]);
		}
		size = boolwriter_finish(&writer);
		CHECK(writer.overflow == 0);

		/* Reads them back from an exactly sized copy. */
		copy = copy_exact(writer.bytes, size);
		CHECK(copy != NULL);
		if (copy == NULL) {
			continue;
		}
		webpc_bool_init(&decoder, copy, size);
		CHECK(webpc_bool_ended(&decoder) == 0);
		mismatches = 0U;
		for (index = 0U; index < 20000U; index++) {
			if (webpc_bool_read(&decoder, probs[index]) != values[index]) {
				mismatches++;
			}
		}
		CHECK(mismatches == 0U);

		/* The bytes the encoder wrote were enough: the decoder never ran past them. */
		CHECK(webpc_bool_ended(&decoder) == 0);
		free(copy);
	}
}

/* Checks literals of 1 to 24 bits, the largest and zero values included. */
static void
test_literals(void)
{
	static unsigned widths[3000];
	static uint32_t values[3000];
	struct webpc_bool decoder;
	uint8_t *copy;
	size_t size;
	unsigned index;
	unsigned mismatches;
	uint32_t mask;
	uint32_t marker;

	/* Widths 1..24 in turn with random values, the ends of each width first. */
	for (index = 0U; index < 3000U; index++) {
		widths[index] = 1U + (index % 24U);
		mask = (uint32_t)((1UL << widths[index]) - 1UL);
		values[index] = (uint32_t)check_random(&random_state) & mask;
		if (index < 48U) {
			values[index] = (index < 24U) ? 0U : mask;
		}
	}

	/* Writes them as literals and reads them back. */
	boolwriter_init(&writer);
	for (index = 0U; index < 3000U; index++) {
		boolwriter_literal(&writer, values[index], widths[index]);
	}
	size = boolwriter_finish(&writer);
	copy = copy_exact(writer.bytes, size);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}
	webpc_bool_init(&decoder, copy, size);
	mismatches = 0U;
	for (index = 0U; index < 3000U; index++) {
		if (webpc_bool_literal(&decoder, widths[index]) != values[index]) {
			mismatches++;
		}
	}
	CHECK(mismatches == 0U);
	CHECK(webpc_bool_ended(&decoder) == 0);
	free(copy);

	/* A width above 24 is cut to 24: the next literal starts after 24 bits. */
	boolwriter_init(&writer);
	boolwriter_literal(&writer, 0xabcdefUL, 24U);
	boolwriter_literal(&writer, 21U, 5U);
	size = boolwriter_finish(&writer);
	copy = copy_exact(writer.bytes, size);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}
	webpc_bool_init(&decoder, copy, size);
	CHECK(webpc_bool_literal(&decoder, 40U) == 0xabcdefUL);
	marker = webpc_bool_literal(&decoder, 5U);
	CHECK(marker == 21UL);
	free(copy);
}

/* Checks signed values: positive, negative and zero, in widths 1 to 24, magnitude first and sign last. */
static void
test_signed(void)
{
	static unsigned widths[2400];
	static int32_t values[2400];
	struct webpc_bool decoder;
	uint8_t *copy;
	size_t size;
	unsigned index;
	unsigned mismatches;
	int32_t magnitude;
	uint32_t mask;

	/* Values in each width: the extremes first, then random of either sign. */
	for (index = 0U; index < 2400U; index++) {
		widths[index] = 1U + (index % 24U);
		mask = (uint32_t)((1UL << widths[index]) - 1UL);
		magnitude = (int32_t)((uint32_t)check_random(&random_state) & mask);
		if (index < 24U) {
			magnitude = 0;
		} else if (index < 48U) {
			magnitude = (int32_t)mask;
		}
		values[index] = (random_below(2U) == 0U || magnitude == 0) ? magnitude : -magnitude;
		if (index >= 24U && index < 48U) {
			values[index] = -magnitude;
		}
	}

	/* Writes them with the writer's signed form and reads them back. */
	boolwriter_init(&writer);
	for (index = 0U; index < 2400U; index++) {
		boolwriter_signed(&writer, values[index], widths[index]);
	}
	size = boolwriter_finish(&writer);
	copy = copy_exact(writer.bytes, size);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}
	webpc_bool_init(&decoder, copy, size);
	mismatches = 0U;
	for (index = 0U; index < 2400U; index++) {
		if (webpc_bool_signed(&decoder, widths[index]) != values[index]) {
			mismatches++;
		}
	}
	CHECK(mismatches == 0U);
	CHECK(webpc_bool_ended(&decoder) == 0);
	free(copy);

	/* A hand-built value: magnitude 5 in 3 bits then sign 1 is -5; magnitude 5 then sign 0 is 5. */
	boolwriter_init(&writer);
	boolwriter_literal(&writer, 5UL, 3U);
	boolwriter_put(&writer, 128U, 1);
	boolwriter_literal(&writer, 5UL, 3U);
	boolwriter_put(&writer, 128U, 0);
	boolwriter_literal(&writer, 0UL, 3U);
	boolwriter_put(&writer, 128U, 1);
	size = boolwriter_finish(&writer);
	copy = copy_exact(writer.bytes, size);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}
	webpc_bool_init(&decoder, copy, size);
	CHECK(webpc_bool_signed(&decoder, 3U) == -5);
	CHECK(webpc_bool_signed(&decoder, 3U) == 5);
	CHECK(webpc_bool_signed(&decoder, 3U) == 0);
	free(copy);
}

/* Checks trees read from the root and from node 2, with a leaf of value 0 and an unbalanced shape. */
static void
test_tree(void)
{
	/*
	 * Root: 0 is the leaf 0, 1 goes to node 2.  Node 2: 0 is the leaf 1,
	 * 1 goes to node 4.  Node 4: leaves 2 and 3.
	 */
	static const int8_t tree[6] = { 0, 2, -1, 4, -2, -3 };

	/*
	 * Root: node 2 or node 4.  Node 2: leaves 0 and 1.  Node 4: node 6 or
	 * leaf 4.  Node 6: leaves 2 and 3 (a tree whose root has no leaf).
	 */
	static const int8_t tree_two[8] = { 2, 4, 0, -1, 6, -4, -2, -3 };
	static uint8_t probs[4];
	static int values[4000];
	static unsigned starts[4000];
	struct webpc_bool decoder;
	uint8_t *copy;
	size_t size;
	unsigned index;
	unsigned mismatches;
	unsigned start;
	int value;
	int status;

	/* Probabilities for each node pair, the ends 0 and 255 among them. */
	probs[0] = 77U;
	probs[1] = 0U;
	probs[2] = 255U;
	probs[3] = 128U;

	/* The first tree: values are written from the root, or from node 2 where the leaf is not 0. */
	for (index = 0U; index < 4000U; index++) {
		starts[index] = (random_below(3U) == 0U) ? 2U : 0U;
		if (starts[index] == 2U) {
			values[index] = 1 + (int)random_below(3U);
		} else {
			values[index] = (int)random_below(4U);
		}
	}
	boolwriter_init(&writer);
	for (index = 0U; index < 4000U; index++) {
		status = boolwriter_tree(&writer, tree, 6U, probs, starts[index], values[index]);
		CHECK(status == 0);
	}
	size = boolwriter_finish(&writer);
	copy = copy_exact(writer.bytes, size);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}
	webpc_bool_init(&decoder, copy, size);
	mismatches = 0U;
	for (index = 0U; index < 4000U; index++) {
		value = webpc_bool_tree(&decoder, tree, probs, starts[index]);
		if (value != values[index]) {
			mismatches++;
		}
	}
	CHECK(mismatches == 0U);
	CHECK(webpc_bool_ended(&decoder) == 0);
	free(copy);

	/* The second tree, whose root has no leaf, from the root and from node 4. */
	for (index = 0U; index < 4000U; index++) {
		starts[index] = (random_below(3U) == 0U) ? 4U : 0U;
		if (starts[index] == 4U) {
			values[index] = (int)(2U + random_below(3U));
		} else {
			values[index] = (int)random_below(5U);
		}
	}
	boolwriter_init(&writer);
	for (index = 0U; index < 4000U; index++) {
		status = boolwriter_tree(&writer, tree_two, 8U, probs, starts[index], values[index]);
		CHECK(status == 0);
	}
	size = boolwriter_finish(&writer);
	copy = copy_exact(writer.bytes, size);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}
	webpc_bool_init(&decoder, copy, size);
	mismatches = 0U;
	for (index = 0U; index < 4000U; index++) {
		start = starts[index];
		value = webpc_bool_tree(&decoder, tree_two, probs, start);
		if (value != values[index]) {
			mismatches++;
		}
	}
	CHECK(mismatches == 0U);
	CHECK(webpc_bool_ended(&decoder) == 0);
	free(copy);
}

/* Checks that a tree with no leaf on its path ends the walk instead of looping. */
static void
test_tree_cut_off(void)
{
	/* Nodes 2 and 4 point at each other; no path reaches a leaf. */
	static const int8_t tree[6] = { 0, 2, 4, 4, 2, 2 };
	static const uint8_t probs[3] = { 128U, 128U, 128U };
	static const uint8_t data[8] = { 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU };
	struct webpc_bool decoder;
	int value;

	/* The walk from node 2 stops and gives 0 (the formats' own trees never do this). */
	webpc_bool_init(&decoder, data, sizeof(data));
	value = webpc_bool_tree(&decoder, tree, probs, 2U);
	CHECK(value == 0);
}

/* Checks the point where ended is set against partitions of 1 and 2 bytes worked out by hand. */
static void
test_ended_hand(void)
{
	/* Window 0x8000 gives the bools 1,0,0,... each followed by exactly one shift. */
	static const uint8_t data[2] = { 0x80U, 0x00U };
	struct webpc_bool decoder;
	int index;

	/* An empty partition is ended at init and stays so; its bools read 0. */
	webpc_bool_init(&decoder, NULL, 0U);
	CHECK(webpc_bool_ended(&decoder) == 1);
	CHECK(webpc_bool_read(&decoder, 128U) == 0);
	CHECK(webpc_bool_ended(&decoder) == 1);

	/* A one-byte partition: the first bool is read inside the data, and it is 0 with the missing byte as 0. */
	webpc_bool_init(&decoder, data, 1U);
	CHECK(webpc_bool_ended(&decoder) == 0);
	CHECK(webpc_bool_read(&decoder, 128U) == 1);
	CHECK(webpc_bool_ended(&decoder) == 0);
	CHECK(webpc_bool_read(&decoder, 128U) == 0);
	CHECK(webpc_bool_ended(&decoder) == 1);

	/*
	 * A two-byte partition holds 16 bits.  Bool k starts with k - 1 shifts,
	 * so its window reaches bit k + 7: the 9th bool still fits, the 10th
	 * does not.
	 */
	webpc_bool_init(&decoder, data, sizeof(data));
	for (index = 1; index <= 9; index++) {
		webpc_bool_read(&decoder, 128U);
		CHECK(webpc_bool_ended(&decoder) == 0);
	}
	webpc_bool_read(&decoder, 128U);
	CHECK(webpc_bool_ended(&decoder) == 1);

	/* Once set it stays set. */
	for (index = 0; index < 50; index++) {
		webpc_bool_read(&decoder, 128U);
		CHECK(webpc_bool_ended(&decoder) == 1);
	}
}

/*
 * Checks ended for every partition size from 0 to 40 bytes, of written
 * and of random bytes, against a model that follows the range from the
 * bools the decoder returns and counts the shifts itself.
 */
static void
test_ended_model(void)
{
	static uint8_t source[64];
	struct webpc_bool decoder;
	uint8_t *copy;
	size_t size;
	size_t byte;
	unsigned kind;
	unsigned count;
	unsigned prob;
	unsigned model_range;
	unsigned split;
	uint64_t model_shifts;
	int model_ended;
	int bit;
	int mismatches;
	unsigned reads;

	/* Partitions cut from a written stream (kind 0), of random bytes (kind 1), and of 0xff bytes (kind 2). */
	for (kind = 0U; kind < 3U; kind++) {
		for (size = 0U; size <= 40U; size++) {
			/* The bytes of this partition. */
			if (kind == 0U) {
				boolwriter_init(&writer);
				for (count = 0U; count < 400U; count++) {
					boolwriter_put(&writer, random_below(256U), (int)random_below(2U));
				}
				boolwriter_finish(&writer);
				memcpy(source, writer.bytes, sizeof(source));
			} else {
				for (byte = 0U; byte < sizeof(source); byte++) {
					source[byte] = (kind == 1U) ? (uint8_t)random_below(256U) : 0xffU;
				}
			}
			copy = copy_exact(source, size);
			if (size != 0U && copy == NULL) {
				CHECK(copy != NULL);
				continue;
			}

			/* The model starts as the decoder does: range 255, no shifts, ended when empty. */
			webpc_bool_init(&decoder, copy, size);
			model_range = 255U;
			model_shifts = 0U;
			model_ended = (size == 0U) ? 1 : 0;
			mismatches = 0;
			if (webpc_bool_ended(&decoder) != model_ended) {
				mismatches++;
			}

			/*
			 * Reads past the end of the data, comparing ended after every
			 * bool: random probabilities first, then probability 128, which
			 * shifts at least once a bool, so the end is certainly passed.
			 */
			for (reads = 0U; reads < (unsigned)size * 16U + 16U; reads++) {
				prob = (reads < (unsigned)size * 8U) ? random_below(256U) : 128U;
				bit = webpc_bool_read(&decoder, prob);

				/* The window of this bool reaches past the data: ended, for good. */
				if (model_shifts + 8U > (uint64_t)size * 8U) {
					model_ended = 1;
				}

				/* The model's own range, from the returned bool. */
				split = 1U + (((model_range - 1U) * prob) >> 8);
				if (bit) {
					model_range -= split;
				} else {
					model_range = split;
				}
				while (model_range < 128U) {
					model_range <<= 1;
					model_shifts++;
				}
				if (webpc_bool_ended(&decoder) != model_ended) {
					mismatches++;
				}
			}
			CHECK(mismatches == 0);

			/* The 8 * size + 16 shifts of the second phase always pass the end of the data. */
			CHECK(model_ended == 1);
			free(copy);
		}
	}
}
