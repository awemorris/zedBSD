/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bit reader of the lossless format: bytes in their order, the bits of
 * each byte from the least significant (RFC 9649 3.2).
 */

#ifndef LIBWEBP_COMPAT_LBITS_H
#define LIBWEBP_COMPAT_LBITS_H

#include "common.h"

/* The most bits one read, peek or skip takes. */
#define WEBPC_LBITS_MAX		24U

/*
 * A reader over a buffer it does not own (the buffer must outlive it).
 * Only lbits.c reads or writes the fields; the others use the calls.
 *
 *   data, size     The bytes read.
 *   next           The index of the next byte not yet taken into value.
 *   value, count   The bits taken but not consumed, the next one in bit 0;
 *                  count of them (0..64).
 *   consumed_end   Set once a read or skip consumed a bit past the last
 *                  byte (such bits read as 0); it stays set.
 */
struct webpc_lbits {
	const uint8_t *data;
	size_t size;
	size_t next;
	uint64_t value;
	unsigned count;
	int consumed_end;
};

/*
 * Starts a reader at the first bit of data (size bytes; data may be NULL
 * when size is 0).
 */
void webpc_lbits_init(struct webpc_lbits *reader, const uint8_t *data, size_t size);

/*
 * Reads bits (0..WEBPC_LBITS_MAX) bits as RFC 9649's ReadBits(bits): the
 * first bit read is bit 0 of the result.  0 bits gives 0 and consumes
 * nothing.  A bit past the end reads as 0 and sets consumed_end.
 */
uint32_t webpc_lbits_read(struct webpc_lbits *reader, unsigned bits);

/*
 * Gives the next bits (0..WEBPC_LBITS_MAX) bits as webpc_lbits_read would,
 * without consuming them; bits past the end are 0 and do not set
 * consumed_end.
 */
uint32_t webpc_lbits_peek(struct webpc_lbits *reader, unsigned bits);

/*
 * Consumes bits (0..WEBPC_LBITS_MAX) bits, as a read whose result is
 * dropped (a bit past the end sets consumed_end).
 */
void webpc_lbits_skip(struct webpc_lbits *reader, unsigned bits);

/*
 * Tells whether a bit past the end was consumed: 1 or 0.  The decoders
 * check it at least after the prefix codes of an image, after each row of
 * pixels and at the end of an image, and fail with ENOBUFS once it is 1
 * (a failure found first, such as EINVAL for a copy from before the
 * start, may be reported instead).
 */
int webpc_lbits_ended(const struct webpc_lbits *reader);

#endif
