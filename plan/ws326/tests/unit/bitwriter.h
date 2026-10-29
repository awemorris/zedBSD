/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A bit writer for the host unit tests of the software H.264 decoder:
 * the tests build their inputs (RBSPs, NAL units, coded blocks) from
 * the syntax of the specification with it, so no input is copied from
 * another decoder or encoder.
 */

#ifndef H264SW_TEST_BITWRITER_H
#define H264SW_TEST_BITWRITER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The largest RBSP a test builds. */
#define BITWRITER_BYTES 65536U

/*
 * An RBSP being written: bits written so far, most significant bit of
 * each byte first, and overflow set when a write did not fit.
 */
struct bitwriter {
	uint8_t data[BITWRITER_BYTES];
	size_t bits;
	int overflow;
};

/* Empties a writer. */
static __inline void
bitwriter_init(
	struct bitwriter *writer)
{
	/* Every byte starts at 0 so writes only need to set bits. */
	memset(writer, 0, sizeof(*writer));
}

/* Writes the count (0..32) low bits of value, most significant first: u(n). */
static __inline void
bitwriter_u(
	struct bitwriter *writer,
	uint32_t value,
	unsigned count)
{
	unsigned index;
	uint32_t bit;

	/* Each bit lands at the next position; a full buffer marks overflow. */
	for (index = count; index != 0U; index--) {
		if (writer->bits >= BITWRITER_BYTES * 8U) {
			writer->overflow = 1;
			return;
		}

		bit = (value >> (index - 1U)) & 1U;
		if (bit != 0U)
			writer->data[writer->bits / 8U] |= (uint8_t)(0x80U >> (writer->bits % 8U));
		writer->bits++;
	}
}

/* Writes ue(v) (9.1): leadingZeroBits zeros, a 1, then the suffix. */
static __inline void
bitwriter_ue(
	struct bitwriter *writer,
	uint32_t value)
{
	uint64_t code;
	unsigned length;

	/* codeNum + 1 written in 2 * length + 1 bits, length its bits after the top one. */
	code = (uint64_t)value + 1U;
	length = 0U;
	while ((code >> (length + 1U)) != 0U)
		length++;

	/* The prefix zeros, then the code itself. */
	bitwriter_u(writer, 0U, length);
	bitwriter_u(writer, 1U, 1U);
	bitwriter_u(writer, (uint32_t)(code & ((1U << length) - 1U)), length);
}

/* Writes se(v) (9.1.1, Table 9-3): positive k as 2k - 1, negative k as -2k. */
static __inline void
bitwriter_se(
	struct bitwriter *writer,
	int32_t value)
{
	/* The mapping of Table 9-3. */
	if (value > 0) {
		bitwriter_ue(writer, (uint32_t)value * 2U - 1U);
	} else {
		bitwriter_ue(writer, (uint32_t)(-(int64_t)value) * 2U);
	}
}

/* Writes rbsp_trailing_bits() (7.3.2.11): a 1 and zeros to the byte boundary. */
static __inline void
bitwriter_trailing(
	struct bitwriter *writer)
{
	/* The stop bit, then the alignment zeros. */
	bitwriter_u(writer, 1U, 1U);
	while (writer->bits % 8U != 0U)
		bitwriter_u(writer, 0U, 1U);
}

/* Returns the bytes written so far (the last partial byte counts). */
static __inline size_t
bitwriter_bytes(
	const struct bitwriter *writer)
{
	/* A partial byte is padded with zeros. */
	return (writer->bits + 7U) / 8U;
}

/*
 * Makes a NAL unit from an RBSP: the header byte, then the RBSP with an
 * emulation_prevention_three_byte inserted after each two zero bytes
 * followed by a byte of 0..3 (7.4.1).  Returns the bytes written to
 * out (capacity bytes), or 0 when they do not fit.
 */
static __inline size_t
bitwriter_nal(
	unsigned ref_idc,
	unsigned type,
	const uint8_t *rbsp,
	size_t size,
	uint8_t *out,
	size_t capacity)
{
	size_t used;
	size_t index;
	unsigned zeros;

	/* forbidden_zero_bit 0, nal_ref_idc, nal_unit_type. */
	if (capacity == 0U)
		return 0U;
	out[0] = (uint8_t)((ref_idc << 5) | type);
	used = 1U;

	/* The RBSP bytes, escaped where three-byte patterns would appear. */
	zeros = 0U;
	for (index = 0U; index < size; index++) {
		if (zeros == 2U && rbsp[index] <= 3U) {
			if (used == capacity)
				return 0U;
			out[used] = 3U;
			used++;
			zeros = 0U;
		}

		if (used == capacity)
			return 0U;
		out[used] = rbsp[index];
		used++;
		if (rbsp[index] == 0U) {
			zeros++;
		} else {
			zeros = 0U;
		}
	}

	/* A final zero byte (a cabac_zero_word) is followed by 0x03 (7.4.1). */
	if (size != 0U && rbsp[size - 1U] == 0U) {
		if (used == capacity)
			return 0U;
		out[used] = 3U;
		used++;
	}

	/* Succeeded: the NAL unit's length. */
	return used;
}

#endif
