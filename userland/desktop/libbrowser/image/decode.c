/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An image's kind from its signature (the image part of the MIME Sniffing
 * standard: the bytes decide, not the Content-Type), the decoder of that
 * kind, and the bitmaps they fill.
 */

#include "image/image.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * The serial the next bitmap made gets.  It only increases (from 1, so 0
 * is no bitmap), for the life of the program; any thread takes one with
 * an atomic increment.
 */
static uint64_t img_next_serial = 1;

/*
 * Tells an image's kind from its first bytes: JPEG starts with FF D8 FF,
 * PNG with its eight-byte signature, GIF with "GIF87a" or "GIF89a".
 */
enum img_kind
img_sniff(
	const unsigned char *bytes,
	size_t length)
{
	static const unsigned char png_signature[8] = { 0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU };
	int differs;

	/* JPEG's start of image and the first marker's 0xFF. */
	if (length >= 3 && bytes[0] == 0xffU && bytes[1] == 0xd8U && bytes[2] == 0xffU)
		return IMG_JPEG;

	/* PNG's signature. */
	if (length >= sizeof(png_signature)) {
		differs = memcmp(bytes, png_signature, sizeof(png_signature));
		if (differs == 0)
			return IMG_PNG;
	}

	/* GIF's two versions. */
	if (length >= 6) {
		differs = memcmp(bytes, "GIF87a", 6);
		if (differs == 0)
			return IMG_GIF;
		differs = memcmp(bytes, "GIF89a", 6);
		if (differs == 0)
			return IMG_GIF;
	}

	/* Anything else is not an image the browser decodes. */
	return IMG_UNKNOWN;
}

/*
 * Decodes an image of any kind the browser knows into a bitmap; ENOTSUP
 * for bytes of another kind, EINVAL for an image that cannot be decoded.
 */
int
img_decode(
	const unsigned char *bytes,
	size_t length,
	struct img_bitmap *bitmap)
{
	enum img_kind kind;
	int error;

	/* The kind decides the decoder. */
	memset(bitmap, 0, sizeof(*bitmap));
	kind = img_sniff(bytes, length);

	/* Decide by the kind. */
	switch (kind) {
	case IMG_JPEG:
		error = img_decode_jpeg(bytes, length, bitmap);
		break;
	case IMG_PNG:
		error = img_decode_png(bytes, length, bitmap);
		break;
	case IMG_GIF:
		error = img_decode_gif(bytes, length, bitmap);
		break;
	default:
		error = ENOTSUP;
		break;
	}

	/* Reports why the image could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the bitmap holds the image. */
	return 0;
}

/*
 * Frees a bitmap's pixels.
 */
void
img_bitmap_release(
	struct img_bitmap *bitmap)
{
	/* The pixels, and nothing left behind. */
	free(bitmap->pixels);
	memset(bitmap, 0, sizeof(*bitmap));
}

/*
 * Allocates a bitmap of a size, every pixel transparent; EINVAL for a
 * size that is empty or larger than the decoders take.
 */
int
img_bitmap_create(
	struct img_bitmap *bitmap,
	int width,
	int height)
{
	size_t count;

	/* A size with pixels, and not too many. */
	memset(bitmap, 0, sizeof(*bitmap));
	if (width <= 0 || height <= 0)
		return EINVAL;
	count = (size_t)width * (size_t)height;
	if (count > IMG_PIXELS_MAX)
		return EINVAL;

	/* The pixels, cleared. */
	bitmap->pixels = calloc(count, sizeof(uint32_t));
	if (bitmap->pixels == NULL)
		return ENOMEM;

	/* Succeeded: the bitmap is transparent, with a serial no other bitmap has. */
	bitmap->width = width;
	bitmap->height = height;
	bitmap->serial = __atomic_fetch_add(&img_next_serial, 1U, __ATOMIC_RELAXED);
	return 0;
}

/*
 * Gives a bitmap whose pixels were drawn again a new serial (a video's
 * picture, ws121-p004), so that a GPU takes its pixels anew.
 */
void
img_bitmap_renew(
	struct img_bitmap *bitmap)
{
	/* A serial no other bitmap has. */
	bitmap->serial = __atomic_fetch_add(&img_next_serial, 1U, __ATOMIC_RELAXED);
}
