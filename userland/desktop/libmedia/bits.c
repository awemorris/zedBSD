/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Borrowed, MSB-first spans for AAC and H.264; no allocation or global state. */

#include "bits.h"

#include <errno.h>
#include <limits.h>

static int bits_golomb(struct media_bits *bits, uint64_t limit, uint64_t *number);

/*
 * Initialize a reader over a borrowed byte span.
 */
void
media_bits_init(
	struct media_bits *bits,
	const void *data,
	size_t size)
{
	/* An invalid span starts failed rather than overflowing its bit count. */
	bits->data = data;
	bits->count = 0U;
	bits->position = 0U;
	bits->error = 0;
	if (size > SIZE_MAX / 8U) {
		bits->error = 1;
		return;
	}

	/* Nonempty input must name actual bytes. */
	if (size != 0U && data == NULL) {
		bits->error = 1;
		return;
	}

	/* Succeeded: the cursor covers exactly the supplied bytes. */
	bits->count = size * 8U;
	return;
}

/*
 * Read one to thirty-two bits, or retain a sticky error on overrun.
 */
uint32_t
media_bits_read(
	struct media_bits *bits,
	unsigned count)
{
	uint32_t number;
	unsigned remaining;
	size_t position;

	/* Once failed, a reader never exposes subsequent input. */
	if (bits->error != 0)
		return 0U;

	/* A scalar read cannot exceed its result width or read an empty field. */
	if (count == 0U || count > 32U) {
		bits->error = 1;
		return 0U;
	}

	/* A failed read does not advance into a partially available field. */
	if (count > bits->count - bits->position) {
		bits->error = 1;
		return 0U;
	}

	/* Accumulate syntax bits in their wire order, most significant first. */
	number = 0U;
	position = bits->position;
	for (remaining = count; remaining != 0U; remaining--) {
		number = (number << 1U) | ((bits->data[position / 8U] >> (7U - position % 8U)) & 1U);
		position++;
	}

	/* Succeeded: publish the entire consumed field. */
	bits->position = position;
	return number;
}

/*
 * Read one flag using the same overrun contract as scalar fields.
 */
uint32_t
media_bits_read1(
	struct media_bits *bits)
{
	uint32_t number;

	/* Consume a flag, retaining any existing or new read error. */
	number = media_bits_read(bits, 1U);
	if (bits->error != 0)
		return 0U;

	/* Succeeded: the flag is available. */
	return number;
}

/*
 * Skip a bounded number of bits without touching their bytes.
 */
void
media_bits_skip(
	struct media_bits *bits,
	size_t count)
{
	/* Failed input cannot be made valid by a later skip. */
	if (bits->error != 0)
		return;

	/* Skipping beyond the span poisons the reader without advancing it. */
	if (count > bits->count - bits->position) {
		bits->error = 1;
		return;
	}

	/* Succeeded: even a zero-bit skip stays within the span. */
	bits->position += count;
	return;
}

/*
 * Advance to the next byte boundary inside this span.
 */
void
media_bits_align(
	struct media_bits *bits)
{
	size_t padding;

	/* The current byte contributes at most seven alignment bits. */
	padding = (8U - bits->position % 8U) % 8U;
	media_bits_skip(bits, padding);

	/* Succeeded or failed as the bounded skip reports in the reader. */
	return;
}

/*
 * Report the unconsumed bit count, or zero for a failed reader.
 */
size_t
media_bits_left(
	const struct media_bits *bits)
{
	/* A failed reader supplies no more syntax. */
	if (bits->error != 0)
		return 0U;

	/* Succeeded: the cursor cannot extend beyond its initialized span. */
	return bits->count - bits->position;
}

/*
 * Read an unsigned Exp-Golomb number within uint32_t's entire range.
 */
uint32_t
media_bits_ue(
	struct media_bits *bits)
{
	uint64_t number;
	int error;

	/* Decode before narrowing, including the sixty-five-bit UINT32_MAX code. */
	error = bits_golomb(bits, UINT32_MAX, &number);
	if (error != 0)
		return 0U;

	/* Succeeded: the decoded number fits the public result type. */
	return (uint32_t)number;
}

/*
 * Read a signed Exp-Golomb number, including INT32_MIN without signed overflow.
 */
int32_t
media_bits_se(
	struct media_bits *bits)
{
	uint64_t number;
	int error;

	/* INT32_MIN uses codeNum 2^32, one beyond the unsigned reader's range. */
	error = bits_golomb(bits, (uint64_t)UINT32_MAX + 1U, &number);
	if (error != 0)
		return 0;

	/* The largest odd code would produce an unrepresentable positive number. */
	if (number == UINT32_MAX) {
		bits->error = 1;
		return 0;
	}

	/* The negative endpoint is represented directly, without negating it. */
	if (number == (uint64_t)UINT32_MAX + 1U)
		return INT32_MIN;

	/* Odd codes are positive, and even codes negative or zero. */
	if ((number & 1U) != 0U)
		return (int32_t)((number + 1U) / 2U);

	/* Succeeded: the negative magnitude fits before it is negated. */
	return -(int32_t)(number / 2U);
}

/*
 * Remove H.264 emulation prevention, allowing source and destination to coincide.
 * On failure the destination is unspecified and written is zero.
 */
int
media_rbsp_unescape(
	const uint8_t *source,
	size_t size,
	uint8_t *destination,
	size_t capacity,
	size_t *written)
{
	size_t cursor;
	size_t used;
	unsigned zeros;
	uint8_t byte;

	/* A caller must be able to receive the successful span length. */
	if (written == NULL)
		return EINVAL;
	*written = 0U;

	/* Empty spans need no storage, but nonempty spans do. */
	if (size != 0U && source == NULL)
		return EINVAL;

	/* A destination is required whenever its advertised capacity is nonempty. */
	if (capacity != 0U && destination == NULL)
		return EINVAL;

	/* Consume escaped input once; output never overtakes an in-place source. */
	used = 0U;
	zeros = 0U;
	for (cursor = 0U; cursor < size; cursor++) {
		byte = source[cursor];

		/* An escape protects one byte in the range zero through three. */
		if (zeros == 2U && byte == 3U) {
			/* A dangling escape or one protecting an impossible byte is malformed. */
			if (cursor + 1U == size)
				return EINVAL;

			/* Prevention is defined only for a following byte from zero through three. */
			if (source[cursor + 1U] > 3U)
				return EINVAL;

			/* Restart the input-zero count after the consumed prevention byte. */
			zeros = 0U;
			continue;
		}

		/* These bytes require prevention when they follow two zeros. */
		if (zeros == 2U && byte < 3U)
			return EINVAL;

		/* A small destination never receives a partial byte beyond its capacity. */
		if (used == capacity)
			return ENOBUFS;
		destination[used] = byte;
		used++;

		/* Track preceding input zeros for the next escape decision. */
		if (byte == 0U) {
			zeros++;
		} else {
			zeros = 0U;
		}
	}

	/* Succeeded: publish the unescaped length only after the whole span is valid. */
	*written = used;
	return 0;
}

/* Decode an Exp-Golomb code in wide arithmetic before applying the caller's limit. */
static int
bits_golomb(
	struct media_bits *bits,
	uint64_t limit,
	uint64_t *number)
{
	uint64_t decoded;
	uint32_t suffix;
	uint32_t bit;
	unsigned zeros;

	/* Find the first one; at most thirty-two leading zeros can fit either result. */
	zeros = 0U;
	for (;;) {
		bit = media_bits_read1(bits);
		if (bits->error != 0)
			return EINVAL;

		/* The separator ends the prefix without contributing another zero. */
		if (bit != 0U)
			break;
		zeros++;

		/* Longer prefixes cannot fit the supported integer widths. */
		if (zeros > 32U) {
			bits->error = 1;
			return EINVAL;
		}
	}

	/* A prefix without zeros has number zero and no suffix field. */
	suffix = 0U;
	if (zeros != 0U) {
		suffix = media_bits_read(bits, zeros);
		if (bits->error != 0)
			return EINVAL;
	}

	/* Wide arithmetic keeps both integer endpoints independent of wraparound. */
	decoded = ((uint64_t)1U << zeros) - 1U + suffix;
	if (decoded > limit) {
		bits->error = 1;
		return EINVAL;
	}

	/* Succeeded: the caller may narrow within its checked domain. */
	*number = decoded;
	return 0;
}
