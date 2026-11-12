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

#if __GCC_ATOMIC_LLONG_LOCK_FREE != 2
/*
 * Where an 8-byte atomic is not lock-free (i386), the C library's runtime
 * serializes the image serials under a lock.  Clang notes that such an
 * operation is slower than a lock-free one, which that target accepts.  The
 * serials stay within this process.
 */
#pragma clang diagnostic ignored "-Watomic-alignment"
#endif

/*
 * The serial the next bitmap made gets.  It only increases (from 1, so 0
 * is no bitmap), for the life of the program; any thread takes one with
 * an atomic increment.
 */
static uint64_t img_next_serial = 1;

/*
 * The pixels of a bitmap and how many hold them (the bitmap, and each
 * committed frame that draws it): the last to let go frees them.  refs is
 * changed atomically (frames are let go on other threads than the page's).
 */
struct img_hold {
	unsigned refs;
	uint32_t *pixels;
};

/*
 * Tells an image's kind from its first bytes: JPEG starts with FF D8 FF,
 * PNG with its eight-byte signature, GIF with "GIF87a" or "GIF89a", WebP
 * with "RIFF", four bytes of size and "WEBPVP" (the MIME Sniffing
 * standard's image pattern).
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

	/* WebP's RIFF header and the start of its first chunk's name. */
	if (length >= 14) {
		differs = memcmp(bytes, "RIFF", 4);
		if (differs == 0) {
			differs = memcmp(bytes + 8, "WEBPVP", 6);
			if (differs == 0)
				return IMG_WEBP;
		}
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
	case IMG_WEBP:
		error = img_decode_webp(bytes, length, bitmap);
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
	/* The pixels (held ones when nobody else holds them), and nothing left behind. */
	if (bitmap->hold != NULL)
		img_hold_release(bitmap->hold);
	else
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

	/* The pixels, cleared, and their hold. */
	bitmap->hold = malloc(sizeof(*bitmap->hold));
	if (bitmap->hold == NULL)
		return ENOMEM;
	bitmap->pixels = calloc(count, sizeof(uint32_t));
	if (bitmap->pixels == NULL) {
		free(bitmap->hold);
		bitmap->hold = NULL;
		return ENOMEM;
	}
	bitmap->hold->refs = 1U;
	bitmap->hold->pixels = bitmap->pixels;

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

/*
 * Makes a bitmap's pixels its own before it is drawn into again: pixels a
 * committed frame holds too are copied into new ones (the frame keeps the
 * old).  Reports 0, or ENOMEM (the bitmap is unchanged).
 */
int
img_bitmap_writable(
	struct img_bitmap *bitmap)
{
	struct img_hold *hold;
	uint32_t *pixels;
	size_t count;
	unsigned refs;

	/* Pixels nobody else holds are the bitmap's already. */
	if (bitmap->hold == NULL)
		return 0;
	refs = __atomic_load_n(&bitmap->hold->refs, __ATOMIC_ACQUIRE);
	if (refs == 1U)
		return 0;

	/* New pixels with the same picture, and their hold. */
	count = (size_t)bitmap->width * (size_t)bitmap->height;
	hold = malloc(sizeof(*hold));
	pixels = malloc(count * sizeof(uint32_t));
	if (hold == NULL || pixels == NULL) {
		free(hold);
		free(pixels);
		return ENOMEM;
	}
	memcpy(pixels, bitmap->pixels, count * sizeof(uint32_t));
	hold->refs = 1U;
	hold->pixels = pixels;

	/* The bitmap's hold on the old pixels ends. */
	img_hold_release(bitmap->hold);
	bitmap->hold = hold;
	bitmap->pixels = pixels;

	/* Succeeded: the pixels are the bitmap's alone. */
	return 0;
}

/* Holds a bitmap's pixels (NULL for a bitmap without a hold, whose pixels live as long as their arena). */
struct img_hold *
img_hold_retain(
	const struct img_bitmap *bitmap)
{
	/* An arena's bitmap has nothing to hold. */
	if (bitmap->hold == NULL)
		return NULL;

	/* One hold more. */
	__atomic_fetch_add(&bitmap->hold->refs, 1U, __ATOMIC_RELAXED);
	return bitmap->hold;
}

/* Lets go of a hold on pixels (NULL is nothing); the last frees them. */
void
img_hold_release(
	struct img_hold *hold)
{
	unsigned refs;

	/* Nothing held. */
	if (hold == NULL)
		return;

	/* One hold less; others still hold them. */
	refs = __atomic_sub_fetch(&hold->refs, 1U, __ATOMIC_ACQ_REL);
	if (refs != 0U)
		return;

	/* The last: the pixels go. */
	free(hold->pixels);
	free(hold);
}
