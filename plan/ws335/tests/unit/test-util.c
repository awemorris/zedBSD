/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the helpers of cryc.h: cryc_wipe, the constant-time comparison,
 * masks and selection, and the byte-order readers and writers.  Expected
 * values are the plain C operators on the same values and byte patterns
 * written out by hand.
 */

#include "check.h"

#include "userland/base/libcrypto-compat/cryc.h"

/* The state of the deterministic generator of the test inputs. */
static uint32_t random_state = 0x2545f491U;

/* Returns the next pseudo-random 32-bit value (xorshift; not a secret). */
static uint32_t
random_next(void)
{
	/* The three xorshift steps. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* Fills size bytes at out with pseudo-random bytes. */
static void
random_fill(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random_next() >> 24);
}

/* Checks cryc_wipe over several sizes, leaving the neighbours alone. */
static void
test_wipe(void)
{
	static const size_t sizes[] = { 0, 1, 2, 7, 8, 63, 64, 65, 200 };
	uint8_t buffer[256];
	size_t which;
	size_t size;
	size_t index;
	int clean;
	int intact;

	/* An absent buffer with size 0 is allowed and does nothing. */
	cryc_wipe(NULL, 0);
	CHECK(1);

	/* Each size, in the middle of a buffer of 0xaa. */
	for (which = 0; which < sizeof(sizes) / sizeof(sizes[0]); which++) {
		size = sizes[which];
		memset(buffer, 0xaa, sizeof(buffer));
		cryc_wipe(buffer + 10, size);

		clean = 1;
		for (index = 10; index < 10 + size; index++)
			if (buffer[index] != 0)
				clean = 0;
		intact = 1;
		for (index = 0; index < 10; index++)
			if (buffer[index] != 0xaa)
				intact = 0;
		for (index = 10 + size; index < sizeof(buffer); index++)
			if (buffer[index] != 0xaa)
				intact = 0;
		CHECK(clean);
		CHECK(intact);
	}
}

/* Checks cryc_ct_equal against memcmp. */
static void
test_ct_equal(void)
{
	uint8_t left[80];
	uint8_t right[80];
	size_t size;
	size_t position;
	unsigned a;
	unsigned b;
	unsigned bit;
	int round;

	/* The empty strings are equal even when absent. */
	CHECK(cryc_ct_equal(NULL, NULL, 0) == 1);
	CHECK(cryc_ct_equal(left, right, 0) == 1);

	/* All pairs of one-byte strings: 1 exactly when the bytes are equal. */
	for (a = 0; a < 256U; a++) {
		for (b = 0; b < 256U; b++) {
			left[0] = (uint8_t)a;
			right[0] = (uint8_t)b;
			if (!CHECK(cryc_ct_equal(left, right, 1) == (a == b ? 1 : 0)))
				return;
		}
	}

	/* Equal strings of every size are equal; one flipped bit anywhere makes them different. */
	for (size = 1; size <= sizeof(left); size++) {
		random_fill(left, size);
		memcpy(right, left, size);
		CHECK(cryc_ct_equal(left, right, size) == 1);
		for (position = 0; position < size; position++) {
			for (bit = 0; bit < 8U; bit++) {
				right[position] = (uint8_t)(left[position] ^ (1U << bit));
				CHECK(cryc_ct_equal(left, right, size) == 0);
				right[position] = left[position];
			}
		}
	}

	/* A difference after the compared length does not count. */
	random_fill(left, sizeof(left));
	memcpy(right, left, sizeof(right));
	right[40] = (uint8_t)(right[40] ^ 0xffU);
	CHECK(cryc_ct_equal(left, right, 40) == 1);
	CHECK(cryc_ct_equal(left, right, 41) == 0);

	/* Random strings with random differences agree with memcmp. */
	for (round = 0; round < 2000; round++) {
		size = 1U + (random_next() >> 8) % sizeof(left);
		random_fill(left, size);
		memcpy(right, left, size);
		if ((random_next() & 1U) != 0)
			right[(random_next() >> 8) % size] = (uint8_t)(random_next() >> 24);
		CHECK(cryc_ct_equal(left, right, size) == (memcmp(left, right, size) == 0 ? 1 : 0));
	}
}

/* Checks the three masks against the C operators over edge values and random pairs. */
static void
test_masks(void)
{
	static const uint32_t edge[] = {
		0x00000000U, 0x00000001U, 0x00000002U, 0x0000007fU, 0x00000080U, 0x000000ffU,
		0x00000100U, 0x0000ffffU, 0x00010000U, 0x00ffffffU, 0x01000000U, 0x7ffffffeU,
		0x7fffffffU, 0x80000000U, 0x80000001U, 0xfffffffeU, 0xffffffffU, 0x55555555U,
		0xaaaaaaaaU
	};
	uint32_t left;
	uint32_t right;
	uint32_t expected;
	size_t first;
	size_t second;
	int round;

	/* Every pair of edge values. */
	for (first = 0; first < sizeof(edge) / sizeof(edge[0]); first++) {
		left = edge[first];
		expected = left == 0 ? 0xffffffffU : 0U;
		CHECK(cryc_ct_mask_zero32(left) == expected);
		for (second = 0; second < sizeof(edge) / sizeof(edge[0]); second++) {
			right = edge[second];
			expected = left == right ? 0xffffffffU : 0U;
			CHECK(cryc_ct_mask_eq32(left, right) == expected);
			expected = left < right ? 0xffffffffU : 0U;
			CHECK(cryc_ct_mask_lt32(left, right) == expected);
		}
	}

	/* Random values, and values one apart. */
	for (round = 0; round < 20000; round++) {
		left = random_next();
		right = random_next();
		if (round % 3 == 1)
			right = left + 1U;
		if (round % 3 == 2)
			right = left - 1U;
		if (round % 7 == 0)
			right = left;
		expected = left == 0 ? 0xffffffffU : 0U;
		CHECK(cryc_ct_mask_zero32(left) == expected);
		expected = left == right ? 0xffffffffU : 0U;
		CHECK(cryc_ct_mask_eq32(left, right) == expected);
		expected = left < right ? 0xffffffffU : 0U;
		CHECK(cryc_ct_mask_lt32(left, right) == expected);
	}
}

/* Checks cryc_ct_select_bytes and cryc_ct_copy_if. */
static void
test_select(void)
{
	uint8_t when_set[70];
	uint8_t when_clear[70];
	uint8_t out[72];
	uint8_t expected[70];
	size_t size;
	size_t index;
	uint32_t mask;
	int which;

	/* Zero bytes touch nothing, even with absent buffers. */
	cryc_ct_select_bytes(NULL, NULL, NULL, 0, 0xffffffffU);
	cryc_ct_copy_if(NULL, NULL, 0, 0xffffffffU);
	CHECK(1);

	/* Every size, both masks. */
	for (size = 1; size <= sizeof(when_set); size++) {
		random_fill(when_set, size);
		random_fill(when_clear, size);
		for (which = 0; which < 2; which++) {
			mask = which == 0 ? 0xffffffffU : 0U;
			memset(out, 0x5a, sizeof(out));
			cryc_ct_select_bytes(out, when_set, when_clear, size, mask);
			CHECK_BYTES(out, which == 0 ? when_set : when_clear, size);
			CHECK(out[size] == 0x5a);

			/* The result may be written over either input. */
			memcpy(expected, when_set, size);
			cryc_ct_select_bytes(expected, expected, when_clear, size, mask);
			CHECK_BYTES(expected, which == 0 ? when_set : when_clear, size);
			memcpy(expected, when_clear, size);
			cryc_ct_select_bytes(expected, when_set, expected, size, mask);
			CHECK_BYTES(expected, which == 0 ? when_set : when_clear, size);

			/* The conditional copy: destination takes the source only for the full mask. */
			memcpy(expected, when_clear, size);
			cryc_ct_copy_if(expected, when_set, size, mask);
			CHECK_BYTES(expected, which == 0 ? when_set : when_clear, size);
			for (index = size; index < sizeof(expected); index++)
				expected[index] = 0x77;
			cryc_ct_copy_if(expected, when_set, size, mask);
			CHECK(size == sizeof(expected) || expected[size] == 0x77);
		}
	}
}

/* Checks the readers and writers against hand-written byte patterns. */
static void
test_byte_order(void)
{
	static const uint8_t pattern[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
	static const uint8_t high[8] = { 0xff, 0xfe, 0x80, 0x81, 0xf0, 0x0f, 0xc3, 0x3c };
	static const uint8_t le64_expected[8] = { 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
	uint8_t bytes[12];
	uint8_t again[8];
	uint64_t wide;
	uint32_t narrow;
	int round;

	/* Readers, ascending bytes: the first byte is the most significant of the big-endian ones. */
	CHECK(cryc_load_be16(pattern) == 0x0102U);
	CHECK(cryc_load_be24(pattern) == 0x010203UL);
	CHECK(cryc_load_be32(pattern) == 0x01020304UL);
	CHECK(cryc_load_be64(pattern) == CRYC_U64(0x01020304, 0x05060708));
	CHECK(cryc_load_le32(pattern) == 0x04030201UL);

	/* Readers, bytes with the top bit set (a shift into the sign bit must not go wrong). */
	CHECK(cryc_load_be16(high) == 0xfffeU);
	CHECK(cryc_load_be24(high) == 0xfffe80UL);
	CHECK(cryc_load_be32(high) == 0xfffe8081UL);
	CHECK(cryc_load_be64(high) == CRYC_U64(0xfffe8081, 0xf00fc33c));
	CHECK(cryc_load_le32(high) == 0x8180feffUL);

	/* Writers: only the named number of bytes are touched; higher bits of the value are dropped. */
	memset(bytes, 0xee, sizeof(bytes));
	cryc_store_be16(bytes + 1, 0xabcd1234UL);
	CHECK(bytes[0] == 0xee && bytes[1] == 0x12 && bytes[2] == 0x34 && bytes[3] == 0xee);
	memset(bytes, 0xee, sizeof(bytes));
	cryc_store_be24(bytes + 1, 0xab123456UL);
	CHECK(bytes[0] == 0xee && bytes[1] == 0x12 && bytes[2] == 0x34 && bytes[3] == 0x56 && bytes[4] == 0xee);
	memset(bytes, 0xee, sizeof(bytes));
	cryc_store_be32(bytes + 1, 0x89abcdefUL);
	CHECK(bytes[0] == 0xee && bytes[1] == 0x89 && bytes[2] == 0xab && bytes[3] == 0xcd && bytes[4] == 0xef && bytes[5] == 0xee);
	memset(bytes, 0xee, sizeof(bytes));
	cryc_store_le32(bytes + 1, 0x89abcdefUL);
	CHECK(bytes[0] == 0xee && bytes[1] == 0xef && bytes[2] == 0xcd && bytes[3] == 0xab && bytes[4] == 0x89 && bytes[5] == 0xee);
	memset(bytes, 0xee, sizeof(bytes));
	cryc_store_be64(bytes + 1, CRYC_U64(0x01020304, 0x05060708));
	CHECK(bytes[0] == 0xee && bytes[9] == 0xee);
	CHECK_BYTES(bytes + 1, pattern, 8);
	memset(bytes, 0xee, sizeof(bytes));
	cryc_store_le64(bytes + 1, CRYC_U64(0x01020304, 0x05060708));
	CHECK(bytes[0] == 0xee && bytes[9] == 0xee);
	CHECK_BYTES(bytes + 1, le64_expected, 8);

	/* Writers of values with the top bit set. */
	cryc_store_be64(bytes, CRYC_U64(0xfffe8081, 0xf00fc33c));
	CHECK_BYTES(bytes, high, 8);

	/* Round trips of random values. */
	for (round = 0; round < 2000; round++) {
		narrow = random_next();
		wide = CRYC_U64(random_next(), random_next());
		cryc_store_be32(again, narrow);
		CHECK(cryc_load_be32(again) == narrow);
		cryc_store_le32(again, narrow);
		CHECK(cryc_load_le32(again) == narrow);
		cryc_store_be16(again, narrow);
		CHECK(cryc_load_be16(again) == (narrow & 0xffffU));
		cryc_store_be24(again, narrow);
		CHECK(cryc_load_be24(again) == (narrow & 0xffffffUL));
		cryc_store_be64(again, wide);
		CHECK(cryc_load_be64(again) == wide);
		cryc_store_le64(again, wide);
		CHECK(cryc_load_le32(again) == (uint32_t)(wide & 0xffffffffUL));
		CHECK(cryc_load_le32(again + 4) == (uint32_t)(wide >> 32));
	}
}

/* Runs the checks. */
int
main(void)
{
	/* Each group of helpers. */
	test_wipe();
	test_ct_equal();
	test_masks();
	test_select();
	test_byte_order();

	/* The result. */
	return check_finish("test-util");
}
