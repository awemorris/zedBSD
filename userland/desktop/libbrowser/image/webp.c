/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * WebP through libwebp-compat: a still image (lossy, lossless, with or
 * without alpha) decoded straight into the bitmap's pixels.  An animated
 * file is refused (libwebp-compat decodes still images only).
 */

#include "image/image.h"

#include <compat/webp/decode.h>

#include <errno.h>
#include <string.h>

static int webp_little_endian(void);

/*
 * Decodes a WebP file into a bitmap; EINVAL when libwebp-compat refuses
 * it (an animation, a damaged file, a size the bitmap refuses).
 */
int
img_decode_webp(
	const unsigned char *bytes,
	size_t length,
	struct img_bitmap *bitmap)
{
	unsigned char *pixels;
	unsigned char *decoded;
	size_t size;
	int width;
	int height;
	int known;
	int status;

	/* The size from the headers. */
	memset(bitmap, 0, sizeof(*bitmap));
	known = WebPGetInfo(bytes, length, &width, &height);
	if (!known)
		return EINVAL;

	/* A bitmap of that size (which refuses an empty or too large one). */
	status = img_bitmap_create(bitmap, width, height);
	if (status != 0)
		return EINVAL;

	/*
	 * The pixels straight into the bitmap: each is 0xAARRGGBB in the
	 * machine's order, which is BGRA in memory on a little-endian machine
	 * and ARGB on a big-endian one; alpha is straight in both.
	 */
	pixels = (unsigned char *)bitmap->pixels;
	size = (size_t)width * (size_t)height * 4U;
	if (webp_little_endian())
		decoded = WebPDecodeBGRAInto(bytes, length, pixels, size, width * 4);
	else
		decoded = WebPDecodeARGBInto(bytes, length, pixels, size, width * 4);
	if (decoded == NULL) {
		img_bitmap_release(bitmap);
		return EINVAL;
	}

	/* Succeeded: the bitmap holds the image. */
	return 0;
}

/* Tells whether the machine stores the low byte of a word first. */
static int
webp_little_endian(void)
{
	uint32_t word;
	unsigned char first;

	/* The first byte of a word whose low byte is 1. */
	word = 1U;
	memcpy(&first, &word, 1);
	if (first == 1U)
		return 1;

	/* Big-endian: the high byte first. */
	return 0;
}
