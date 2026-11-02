/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lossless decoder: header, transforms, image and inverse transforms.
 */

#include "vp8l.h"

#include <errno.h>

/*
 * Reads the header of a lossless stream (data from its signature byte, size
 * bytes): signature 0x2f, width and height (14 bits each, plus 1),
 * alpha_is_used (1 bit), version_number (3 bits, which must be 0).
 */
int
webpc_vp8l_read_header(
	const uint8_t *data,
	size_t size,
	struct webpc_vp8l_header *header)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(header);

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Decodes a whole lossless stream (data from its signature byte, size bytes)
 * into a new array of width * height ARGB pixels (as webpc_vp8l_decode_image
 * lays them out), which *pixels receives and the caller frees with free().
 */
int
webpc_vp8l_decode(
	const uint8_t *data,
	size_t size,
	uint32_t **pixels,
	unsigned *width,
	unsigned *height)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(pixels);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Decodes an image-stream without header (RFC 9649 3.8.1: optional
 * transforms then the spatially coded image; an ALPH chunk's compressed
 * data, RFC 9649 2.7.1.2) of width by height pixels (each 1 or more, at most
 * WEBPC_VP8L_SIDE_MAX) from data (size bytes) into pixels (the caller's
 * width * height ARGB values).
 */
int
webpc_vp8l_decode_stream(
	const uint8_t *data,
	size_t size,
	unsigned width,
	unsigned height,
	uint32_t *pixels)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(pixels);

	/* Not implemented yet. */
	return ENOSYS;
}
