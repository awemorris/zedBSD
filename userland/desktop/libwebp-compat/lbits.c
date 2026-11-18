/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The least-significant-bit-first bit reader of the lossless format.
 */

#include "lbits.h"

#include <errno.h>

/* The most bits the reader's value holds: a byte is taken in only below this. */
#define LBITS_VALUE_BITS	64U

static void lbits_refill(struct webpc_lbits *reader, unsigned bits);
static void lbits_consume(struct webpc_lbits *reader, unsigned bits);

/*
 * Starts a reader at the first bit of data (size bytes; data may be NULL
 * when size is 0).
 */
void
webpc_lbits_init(
	struct webpc_lbits *reader,
	const uint8_t *data,
	size_t size)
{
	/* Points the reader at the buffer, with no bit taken in yet. */
	reader->data = data;
	reader->size = size;
	reader->next = 0;
	reader->value = 0;
	reader->count = 0;

	/* Nothing has been consumed, so nothing is past the end. */
	reader->consumed_end = 0;
}

/*
 * Reads bits (0..WEBPC_LBITS_MAX) bits as RFC 9649's ReadBits(bits): the
 * first bit read is bit 0 of the result.
 */
uint32_t
webpc_lbits_read(
	struct webpc_lbits *reader,
	unsigned bits)
{
	uint32_t mask;
	uint32_t result;

	/* Makes sure the bytes that are left are in the value, up to bits bits. */
	lbits_refill(reader, bits);

	/*
	 * Procedure referenced: RFC 9649 3.2 (ReadBits: the bits of each byte
	 * from the least significant, the first bit read being bit 0 of the
	 * result).
	 *
	 * Takes the low bits bits.  The value holds zeros above its count, so a
	 * bit past the end of the data reads as 0.
	 */
	mask = (uint32_t)((1UL << bits) - 1UL);
	result = (uint32_t)(reader->value & mask);

	/* Drops the bits that were read, and notes any that were past the end. */
	lbits_consume(reader, bits);

	/* Succeeded: the bits read, the first one in bit 0. */
	return result;
}

/*
 * Gives the next bits (0..WEBPC_LBITS_MAX) bits as webpc_lbits_read would,
 * without consuming them; bits past the end are 0 and do not set
 * consumed_end.
 */
uint32_t
webpc_lbits_peek(
	struct webpc_lbits *reader,
	unsigned bits)
{
	uint32_t mask;
	uint32_t result;

	/* Makes sure the bytes that are left are in the value, up to bits bits. */
	lbits_refill(reader, bits);

	/* Takes the low bits bits, zeros past the end, and leaves the reader as it is. */
	mask = (uint32_t)((1UL << bits) - 1UL);
	result = (uint32_t)(reader->value & mask);

	/* Succeeded: the bits that follow, not consumed. */
	return result;
}

/*
 * Consumes bits (0..WEBPC_LBITS_MAX) bits, as a read whose result is dropped
 * (a bit past the end sets consumed_end).
 */
void
webpc_lbits_skip(
	struct webpc_lbits *reader,
	unsigned bits)
{
	/* Makes sure the bytes that are left are in the value, up to bits bits. */
	lbits_refill(reader, bits);

	/* Drops the bits, and notes any that were past the end. */
	lbits_consume(reader, bits);
}

/*
 * Tells whether a bit past the end was consumed: 1 or 0.
 */
int
webpc_lbits_ended(
	const struct webpc_lbits *reader)
{
	/* 1 once any read or skip went past the last byte. */
	return reader->consumed_end;
}

/*
 * Takes whole bytes into the value, while the value has room for them, until it holds at least bits bits or the data is used up.
 */
static void
lbits_refill(
	struct webpc_lbits *reader,
	unsigned bits)
{
	uint64_t byte;

	/*
	 * Fills up from the data, a byte at a time above the bits already held:
	 * the value stays zero above its count, which is what makes a bit past
	 * the end read as 0.
	 */
	while (reader->count < bits &&
	       reader->count + 8U <= LBITS_VALUE_BITS &&
	       reader->next < reader->size) {
		byte = (uint64_t)reader->data[reader->next];
		reader->value |= byte << reader->count;
		reader->count += 8U;
		reader->next++;
	}
}

/*
 * Drops bits bits from the value, and sets consumed_end when the value held fewer (the rest were past the end).
 */
static void
lbits_consume(
	struct webpc_lbits *reader,
	unsigned bits)
{
	/*
	 * Fewer bits held than consumed after a refill means the data ran out:
	 * the reader is left empty and the end is noted for good.
	 */
	if (reader->count < bits) {
		reader->value = 0;
		reader->count = 0;
		reader->consumed_end = 1;
		return;
	}

	/* Enough bits were held: drops them, the next one is now bit 0. */
	reader->value >>= bits;
	reader->count -= bits;
}
