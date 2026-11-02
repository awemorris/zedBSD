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
	UNUSED_PARAMETER(reader);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);

	/* Not implemented yet. */
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
	UNUSED_PARAMETER(reader);
	UNUSED_PARAMETER(bits);

	/* Not implemented yet. */
	return 0;
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
	UNUSED_PARAMETER(reader);
	UNUSED_PARAMETER(bits);

	/* Not implemented yet. */
	return 0;
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
	UNUSED_PARAMETER(reader);
	UNUSED_PARAMETER(bits);

	/* Not implemented yet. */
}

/*
 * Tells whether a bit past the end was consumed: 1 or 0.
 */
int
webpc_lbits_ended(
	const struct webpc_lbits *reader)
{
	UNUSED_PARAMETER(reader);

	/* Not implemented yet. */
	return 0;
}
