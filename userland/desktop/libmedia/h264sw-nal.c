/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The NAL units of the software H.264 decoder: the byte stream (Annex B),
 * the NAL unit header (7.3.1) and the emulation prevention (7.4.1).
 */

#include "h264sw-nal.h"
#include "bits.h"

#include <errno.h>
#include <stddef.h>

/*
 * Finds the next NAL unit of a byte stream (B.1.1, B.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-nal.h.
 */
int
h264sw_nal_find(
	const uint8_t *data,
	size_t size,
	size_t *cursor,
	const uint8_t **nal,
	size_t *nal_size)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(cursor);
	UNUSED_PARAMETER(nal);
	UNUSED_PARAMETER(nal_size);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a NAL unit's header and copies its RBSP into buffer (7.3.1, 7.4.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-nal.h.
 */
int
h264sw_nal_parse(
	const uint8_t *nal,
	size_t size,
	uint8_t *buffer,
	size_t capacity,
	struct h264sw_nal *out)
{
	UNUSED_PARAMETER(nal);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(capacity);
	UNUSED_PARAMETER(out);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
