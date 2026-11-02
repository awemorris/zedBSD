/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The WebP container: the RIFF header, the VP8X chunk, the optional chunks
 * and the bitstream chunk.
 */

#include "riff.h"

#include <errno.h>

/*
 * Parses a file's container.
 */
int
webpc_riff_parse(
	const uint8_t *data,
	size_t size,
	int complete,
	struct webpc_container *out)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(complete);
	UNUSED_PARAMETER(out);

	/* Not implemented yet. */
	return ENOSYS;
}
