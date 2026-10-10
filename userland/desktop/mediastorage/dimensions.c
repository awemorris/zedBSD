/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Extracts raster dimensions from image headers without loading or decoding the media. */
#include "dimensions.h"
#include "userland/desktop/photos/photos.h"
#include <limits.h>
#include <unistd.h>

static unsigned dimensions_be32(const unsigned char *bytes);
static void dimensions_jpeg(int descriptor, int *width, int *height);

/*
 * Records known JPEG/PNG/GIF dimensions; unsupported or incomplete headers remain zero.
 */
void
media_dimensions(
	int descriptor,
	int kind,
	int *width,
	int *height)
{
	unsigned char header[24];
	unsigned horizontal;
	unsigned vertical;
	ssize_t count;

	/* Dimensions are optional metadata, never a reason to reject a retained original. */
	*width = 0;
	*height = 0;
	horizontal = 0U;
	vertical = 0U;
	count = pread(descriptor, header, sizeof(header), 0);
	if (kind == PH_KIND_PNG && count == (ssize_t)sizeof(header)) {
		horizontal = dimensions_be32(header + 16);
		vertical = dimensions_be32(header + 20);
	} else if (kind == PH_KIND_GIF && count >= 10) {
		horizontal = (unsigned)header[6] | ((unsigned)header[7] << 8U);
		vertical = (unsigned)header[8] | ((unsigned)header[9] << 8U);
	} else if (kind == PH_KIND_JPEG) {
		dimensions_jpeg(descriptor, width, height);
		return;
	}

	/* Guard conversion to the existing signed image model. */
	if (horizontal <= INT_MAX && vertical <= INT_MAX) {
		*width = (int)horizontal;
		*height = (int)vertical;
	}
}

/* Reads an unsigned PNG header field. */
static unsigned
dimensions_be32(
	const unsigned char *bytes)
{
	/* PNG dimension fields use network byte order. */
	return ((unsigned)bytes[0] << 24U) | ((unsigned)bytes[1] << 16U) | ((unsigned)bytes[2] << 8U) | bytes[3];
}

/* Walks JPEG marker segments up to the first frame header or compressed scan. */
static void
dimensions_jpeg(
	int descriptor,
	int *width,
	int *height)
{
	unsigned char header[9];
	unsigned marker;
	unsigned length;
	unsigned segments;
	off_t offset;
	ssize_t count;

	/* Metadata segments are length-delimited; no compressed pixels are read. */
	offset = 2;
	for (segments = 0U; segments < 65536U; segments++) {
		count = pread(descriptor, header, 2U, offset);
		if (count != 2 || header[0] != 0xffU)
			return;
		marker = header[1];
		if (marker == 0xffU) {
			offset++;
			continue;
		}

		/* End of image or the first scan ends header discovery. */
		if (marker == 0xd9U || marker == 0xdaU)
			return;
		if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd8U)) {
			offset += 2;
			continue;
		}

		/* Every remaining segment includes its own two-byte length. */
		count = pread(descriptor, header, 4U, offset);
		if (count != 4)
			return;
		length = ((unsigned)header[2] << 8U) | header[3];
		if (length < 2U)
			return;
		if (marker >= 0xc0U && marker <= 0xcfU && marker != 0xc4U && marker != 0xc8U && marker != 0xccU) {
			if (length < 8U)
				return;
			count = pread(descriptor, header, sizeof(header), offset);
			if (count != (ssize_t)sizeof(header))
				return;
			*height = (int)(((unsigned)header[5] << 8U) | header[6]);
			*width = (int)(((unsigned)header[7] << 8U) | header[8]);
			return;
		}

		/* Move past this segment's marker and inclusive length. */
		offset += (off_t)length + 2;
	}
}
