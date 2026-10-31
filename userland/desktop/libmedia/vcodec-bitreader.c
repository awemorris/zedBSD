/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bit reader of libmedia's software video codecs (H.264 7.2 and 9.1,
 * H.265 7.2 and 9.2).
 */

#include "vcodec-bitreader.h"

#include <errno.h>
#include <stddef.h>

/* The most bits one fixed-width read or peek returns. */
#define BITS_READ_MAX 32U

/*
 * The most leading zero bits an Exp-Golomb code may have for the value to fit
 * in 32 bits: codeNum is at most 2^32 - 2, which has 31 leading zero bits.
 */
#define BITS_LEADING_ZERO_MAX 31U

/* The number of bytes a 32-bit field starting inside a byte can touch. */
#define BITS_FETCH_BYTES 5U

/* Static function declarations. */
static uint32_t bits_fetch(const struct vcodec_bitreader *bits, size_t position, unsigned count);
static size_t bits_stop_find(const uint8_t *data, size_t size);

/*
 * Starts a reader at the first bit of an RBSP.
 *
 * The stop bit is the last bit equal to 1, so it is found by scanning back
 * over the trailing zero bytes and then over the zero bits of the last
 * nonzero byte.
 */
void
vcodec_bitreader_init(
	struct vcodec_bitreader *bits,
	const uint8_t *data,
	size_t size)
{
	/* A missing buffer is an empty RBSP, whatever size the caller gave. */
	if (data == NULL)
		size = 0U;

	/* Starts the reader at the first bit with no error. */
	bits->data = data;
	bits->size = size;
	bits->position = 0U;
	bits->error = 0;

	/* Finds the position of the rbsp_stop_one_bit. */
	bits->stop = bits_stop_find(data, size);
}

/*
 * Reads u(n): count bits, most significant first, as an unsigned number (7.2).
 *
 * A failed read leaves position where it was, so the caller can tell how far
 * the reader got.
 */
uint32_t
vcodec_bitreader_u(
	struct vcodec_bitreader *bits,
	unsigned count)
{
	uint32_t value;
	size_t left;

	/* An error stays: every read after it returns 0. */
	if (bits->error != 0)
		return 0U;

	/* A width above 32 cannot be returned in a uint32_t. */
	if (count > BITS_READ_MAX) {
		bits->error = EINVAL;
		return 0U;
	}

	/* A zero-width field reads nothing. */
	if (count == 0U)
		return 0U;

	/* Refuses a read that would pass the end of the RBSP. */
	left = vcodec_bitreader_left(bits);
	if ((size_t)count > left) {
		bits->error = EINVAL;
		return 0U;
	}

	/* Takes the bits at the current position and moves past them. */
	value = bits_fetch(bits, bits->position, count);
	bits->position += (size_t)count;

	/* Reports the number read. */
	return value;
}

/* Reads u(1), the commonest read (flags). */
uint32_t
vcodec_bitreader_u1(
	struct vcodec_bitreader *bits)
{
	uint32_t value;

	/* Reads one bit through the general read. */
	value = vcodec_bitreader_u(bits, 1U);

	/* Reports the bit. */
	return value;
}

/*
 * Returns the next count bits (0 to 32) without moving position.
 *
 * Bits beyond the end of the RBSP read as 0.
 */
uint32_t
vcodec_bitreader_peek(
	struct vcodec_bitreader *bits,
	unsigned count)
{
	uint32_t value;

	/* A broken reader, or a width that cannot be returned, gives 0. */
	if (bits->error != 0)
		return 0U;
	if (count > BITS_READ_MAX)
		return 0U;

	/* Takes the bits without moving the position. */
	value = bits_fetch(bits, bits->position, count);

	/* Reports the bits that follow the position. */
	return value;
}

/* Moves position by count bits; moving past size * 8 sets error and leaves position at size * 8. */
void
vcodec_bitreader_skip(
	struct vcodec_bitreader *bits,
	size_t count)
{
	size_t left;

	/* An error stays: nothing moves after it. */
	if (bits->error != 0)
		return;

	/* Moving past the end parks the reader at the end and marks the error. */
	left = vcodec_bitreader_left(bits);
	if (count > left) {
		bits->position = bits->size * 8U;
		bits->error = EINVAL;
		return;
	}

	/* Moves over the skipped bits. */
	bits->position += count;
}

/*
 * Reads ue(v), an unsigned Exp-Golomb code (9.1).
 *
 * The code is leadingZeroBits zeros, a 1, and leadingZeroBits suffix bits;
 * codeNum is 2^leadingZeroBits - 1 plus the suffix (9-1, 9-2).  The whole
 * code is at most 63 bits, so the zeros are counted in a 32-bit peek.
 */
uint32_t
vcodec_bitreader_ue(
	struct vcodec_bitreader *bits)
{
	uint32_t window;
	uint32_t suffix;
	uint32_t code_num;
	unsigned zeros;
	size_t left;

	/* An error stays: every read after it returns 0. */
	if (bits->error != 0)
		return 0U;

	/*
	 * Takes the next 32 bits.  A window of all zeros has 32 or more
	 * leading zero bits (or the RBSP ends in zeros), which no codeNum of
	 * 32 bits has.
	 */
	window = bits_fetch(bits, bits->position, BITS_READ_MAX);
	if (window == 0U) {
		bits->error = EINVAL;
		return 0U;
	}

	/* Counts the leading zero bits, which are 31 at most here. */
	zeros = 0U;
	while ((window & 0x80000000UL) == 0U) {
		window = (window << 1) & 0xFFFFFFFFUL;
		zeros++;
	}

	/* Refuses a code whose prefix, stop bit and suffix pass the end of the RBSP. */
	left = vcodec_bitreader_left(bits);
	if ((size_t)(2U * zeros + 1U) > left) {
		bits->error = EINVAL;
		return 0U;
	}

	/* Moves over the prefix zeros and the 1 that ends the prefix. */
	bits->position += (size_t)(zeros + 1U);

	/* Reads the suffix; a zero-length suffix is 0. */
	suffix = vcodec_bitreader_u(bits, zeros);

	/* Assembles codeNum = 2^leadingZeroBits - 1 + suffix, at most 2^32 - 2. */
	code_num = ((uint32_t)1U << zeros) - 1U + suffix;

	/* Reports codeNum. */
	return code_num;
}

/*
 * Reads se(v), a signed Exp-Golomb code (9.1.1, Table 9-3).
 *
 * An odd codeNum k is the positive value (k + 1) / 2 and an even one is the
 * negative value -(k / 2); both fit in int32_t for codeNum up to 2^32 - 2.
 */
int32_t
vcodec_bitreader_se(
	struct vcodec_bitreader *bits)
{
	uint32_t code_num;
	uint32_t magnitude;
	int32_t value;

	/* Reads the codeNum of the signed code. */
	code_num = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return 0;

	/* Maps the codeNum to a signed value by its parity. */
	if ((code_num & 1U) != 0U) {
		/* An odd codeNum is positive: the magnitude is Ceil(k / 2). */
		magnitude = (code_num >> 1) + 1U;
		value = (int32_t)magnitude;
	} else {
		/* An even codeNum is zero or negative: the magnitude is k / 2. */
		magnitude = code_num >> 1;
		value = -(int32_t)magnitude;
	}

	/* Reports the signed value. */
	return value;
}

/*
 * Reads te(v), a truncated Exp-Golomb code whose largest value is range (9.1).
 *
 * With a range of 1 the element is one bit, inverted (9-3); with a larger
 * range it is an ue(v).
 */
uint32_t
vcodec_bitreader_te(
	struct vcodec_bitreader *bits,
	uint32_t range)
{
	uint32_t bit;
	uint32_t value;

	/* A range of 0 means the element is not present, so reading it is a mistake. */
	if (range == 0U) {
		bits->error = EINVAL;
		return 0U;
	}

	/* A range above 1 is coded like ue(v). */
	if (range > 1U) {
		value = vcodec_bitreader_ue(bits);
		return value;
	}

	/* A range of 1 is one bit whose value is the inverse of the bit read. */
	bit = vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return 0U;

	/* Inverts the bit read (9-3). */
	value = 1U - bit;

	/* Reports the value. */
	return value;
}

/* Reports whether position is on a byte boundary (byte_aligned(), 7.2). */
int
vcodec_bitreader_aligned(
	const struct vcodec_bitreader *bits)
{
	/* A position that is not a multiple of 8 is inside a byte. */
	if ((bits->position & 7U) != 0U)
		return 0;

	/* Succeeded: the position is on a byte boundary. */
	return 1;
}

/* Returns the bits left before size * 8. */
size_t
vcodec_bitreader_left(
	const struct vcodec_bitreader *bits)
{
	size_t total;
	size_t left;

	/* The position never passes the end, so the difference is not negative. */
	total = bits->size * 8U;
	left = total - bits->position;

	/* Reports the bits left. */
	return left;
}

/*
 * Reports more_rbsp_data() (7.2): whether position is before stop.
 *
 * The stop bit is the last bit equal to 1, so any bit before it belongs to
 * the data and the stop bit itself does not.
 */
int
vcodec_bitreader_more_rbsp_data(
	const struct vcodec_bitreader *bits)
{
	/* A broken reader has no more data, so a loop on it ends. */
	if (bits->error != 0)
		return 0;

	/* At or after the stop bit, the data is over. */
	if (bits->position >= bits->stop)
		return 0;

	/* Succeeded: data bits remain before the stop bit. */
	return 1;
}

/*
 * Reads rbsp_trailing_bits() (7.3.2.11): the stop bit and the zero bits to the byte boundary.
 *
 * Because stop is the last bit equal to 1, the bits after it are zero
 * already; what is checked is that the reader stands on that bit.
 */
int
vcodec_bitreader_trailing(
	struct vcodec_bitreader *bits)
{
	/* A broken reader cannot be at the trailing bits. */
	if (bits->error != 0)
		return EINVAL;

	/* An RBSP with no bit equal to 1 has no stop bit. */
	if (bits->stop >= bits->size * 8U)
		return EINVAL;

	/* Data, or another bit equal to 1, remains before the stop bit; or the stop bit was passed. */
	if (bits->position != bits->stop)
		return EINVAL;

	/* Moves over the stop bit and the zero bits up to the next byte boundary. */
	bits->position = (bits->stop / 8U + 1U) * 8U;

	/* Succeeded: the trailing bits are in their valid form. */
	return 0;
}

/*
 * Returns count bits (0 to 32) from the bit at position, reading 0 for any
 * bit beyond the end; the reader is not changed.
 *
 * Up to five bytes cover a 32-bit field that starts inside a byte, so they
 * are gathered most significant first and the field is cut out of them.
 */
static uint32_t
bits_fetch(
	const struct vcodec_bitreader *bits,
	size_t position,
	unsigned count)
{
	uint64_t gathered;
	size_t byte_index;
	unsigned bit_offset;
	unsigned index;
	unsigned shift;
	uint64_t mask;
	uint32_t value;

	/* A zero-width field is 0, and a start beyond the end is all zero bits. */
	if (count == 0U)
		return 0U;
	if (position >= bits->size * 8U)
		return 0U;

	/* Locates the byte and the bit within it where the field starts. */
	byte_index = position >> 3;
	bit_offset = (unsigned)(position & 7U);

	/* Gathers the bytes that hold the field, with zero bytes past the end. */
	gathered = 0U;
	for (index = 0U; index < BITS_FETCH_BYTES; index++) {
		gathered = gathered << 8;
		if (bits->size - byte_index > (size_t)index)
			gathered |= (uint64_t)bits->data[byte_index + (size_t)index];
	}

	/* Cuts the field out: drops the bits after it, then keeps the low count bits. */
	shift = BITS_FETCH_BYTES * 8U - bit_offset - count;
	mask = (((uint64_t)1U) << count) - 1U;
	value = (uint32_t)((gathered >> shift) & mask);

	/* Reports the field. */
	return value;
}

/*
 * Finds the bit position of the last bit equal to 1 in data[0..size), or
 * size * 8 when there is none.
 */
static size_t
bits_stop_find(
	const uint8_t *data,
	size_t size)
{
	size_t end;
	unsigned bit;
	uint8_t last;
	size_t stop;

	/* Skips the trailing zero bytes, which hold no bit equal to 1. */
	end = size;
	while (end > 0U && data[end - 1U] == 0U)
		end--;

	/* An RBSP of zero bytes has no stop bit. */
	if (end == 0U)
		return size * 8U;

	/* The lowest set bit of the last nonzero byte is the last bit equal to 1. */
	last = data[end - 1U];
	bit = 0U;
	while (((last >> bit) & 1U) == 0U)
		bit++;

	/* Converts the position inside the byte, counted from its most significant bit. */
	stop = (end - 1U) * 8U + (size_t)(7U - bit);

	/* Reports the stop bit's position. */
	return stop;
}
