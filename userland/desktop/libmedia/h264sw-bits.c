/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bit reader of the software H.264 decoder (7.2, 9.1).
 */

#include "h264sw-bits.h"

#include <errno.h>
#include <stddef.h>

/*
 * Starts a reader at the first bit of an RBSP.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
void
h264sw_bits_init(
	struct h264sw_bits *bits,
	const uint8_t *data,
	size_t size)
{
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);

	/* Does nothing yet. */
	return;
}

/*
 * Reads u(n): count bits, most significant first, as an unsigned number (7.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
uint32_t
h264sw_bits_u(
	struct h264sw_bits *bits,
	unsigned count)
{
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(count);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Reads u(1), the commonest read (flags), as h264sw_bits_u(bits, 1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
uint32_t
h264sw_bits_u1(
	struct h264sw_bits *bits)
{
	UNUSED_PARAMETER(bits);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Returns the next count bits (0 to 32) without moving position.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
uint32_t
h264sw_bits_peek(
	struct h264sw_bits *bits,
	unsigned count)
{
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(count);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Moves position by count bits; moving past size * 8 sets error and leaves position at size * 8.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
void
h264sw_bits_skip(
	struct h264sw_bits *bits,
	size_t count)
{
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(count);

	/* Does nothing yet. */
	return;
}

/*
 * Reads ue(v), an unsigned Exp-Golomb code (9.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
uint32_t
h264sw_bits_ue(
	struct h264sw_bits *bits)
{
	UNUSED_PARAMETER(bits);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Reads se(v), a signed Exp-Golomb code (9.1.1, Table 9-3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
int32_t
h264sw_bits_se(
	struct h264sw_bits *bits)
{
	UNUSED_PARAMETER(bits);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Reads te(v), a truncated Exp-Golomb code whose largest value is range (9.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
uint32_t
h264sw_bits_te(
	struct h264sw_bits *bits,
	uint32_t range)
{
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(range);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Reports whether position is on a byte boundary (byte_aligned(), 7.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
int
h264sw_bits_aligned(
	const struct h264sw_bits *bits)
{
	UNUSED_PARAMETER(bits);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Returns the bits left before size * 8 (0 after an error at the end).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
size_t
h264sw_bits_left(
	const struct h264sw_bits *bits)
{
	UNUSED_PARAMETER(bits);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Reports more_rbsp_data() (7.2): whether position is before stop.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
int
h264sw_bits_more_rbsp_data(
	const struct h264sw_bits *bits)
{
	UNUSED_PARAMETER(bits);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads rbsp_trailing_bits() (7.3.2.11): the stop bit and the zero bits to the byte boundary.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-bits.h.
 */
int
h264sw_bits_trailing(
	struct h264sw_bits *bits)
{
	UNUSED_PARAMETER(bits);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
