/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A bit writer for the tests of the lossless format: the inverse of RFC
 * 9649 3.2's ReadBits, written from that section alone (bytes in order,
 * the bits of a byte from the least significant).  Tests build their
 * lossless streams with it: codes, transforms and pixels as the RFC's
 * syntax lays them out.
 */

#ifndef WEBPC_TEST_LBITWRITER_H
#define WEBPC_TEST_LBITWRITER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The most bytes a test stream holds. */
#define LBITWRITER_BYTES	65536U

/*
 * A stream being written: bytes, the bits written so far (bit n is bit
 * n % 8 of byte n / 8), and overflow, set when a write passed the end.
 */
struct lbitwriter {
	uint8_t bytes[LBITWRITER_BYTES];
	size_t bits;
	int overflow;
};

/* Starts an empty stream. */
static __inline void
lbitwriter_init(
	struct lbitwriter *writer)
{
	/* Every byte zero, so a bit is written by setting it. */
	memset(writer, 0, sizeof(*writer));
}

/*
 * Writes the low count bits (0..32) of value as ReadBits(count) reads
 * them: bit 0 of value first.
 */
static __inline void
lbitwriter_put(
	struct lbitwriter *writer,
	uint32_t value,
	unsigned count)
{
	unsigned index;

	/* One bit at a time, from bit 0 of value. */
	for (index = 0; index < count; index++) {
		if (writer->bits >= (size_t)LBITWRITER_BYTES * 8U) {
			writer->overflow = 1;
			return;
		}
		if (((value >> index) & 1U) != 0U)
			writer->bytes[writer->bits / 8U] |= (uint8_t)(1U << (writer->bits % 8U));
		writer->bits++;
	}
}

/*
 * Writes a prefix code's codeword: code (length bits) with its most
 * significant bit first, as a canonical code is read.
 */
static __inline void
lbitwriter_put_code(
	struct lbitwriter *writer,
	uint32_t code,
	unsigned length)
{
	unsigned index;

	/* From the most significant bit of the codeword down. */
	for (index = length; index > 0U; index--)
		lbitwriter_put(writer, (code >> (index - 1U)) & 1U, 1U);
}

/* Gives the bytes the stream fills (the last one partly). */
static __inline size_t
lbitwriter_size(
	const struct lbitwriter *writer)
{
	/* Whole bytes, the partial last one included. */
	return (writer->bits + 7U) / 8U;
}

#endif
