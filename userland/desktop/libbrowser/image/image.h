/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Images (plan/ws074/design.md §10): the kind of an image told from its
 * first bytes, and its decoding into a bitmap the renderers draw.  JPEG
 * goes through libjpeg-compat, PNG through libpng-compat and GIF through
 * libgif-compat (the first frame); the module depends on nothing of the
 * browser but base.
 */

#ifndef KEILAND_BROWSER_IMAGE_H
#define KEILAND_BROWSER_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/* The kinds of image the browser decodes. */
enum img_kind {
	IMG_UNKNOWN,
	IMG_JPEG,
	IMG_PNG,
	IMG_GIF
};

/*
 * A decoded image: width by height pixels, rows top to bottom, each
 * 0xAARRGGBB with straight (not premultiplied) alpha, as the display list
 * and both renderers take them.  The pixels are the bitmap's own (freed by
 * img_bitmap_release).  serial tells bitmaps apart for the life of the
 * program (a GPU keeps an image's pixels by it).
 */
struct img_bitmap {
	uint32_t *pixels;
	int width;
	int height;
	uint64_t serial;
	/* Set for a vector picture that has no size and no ratio of its own (the default size stands in). */
	int no_ratio;
};

/* The largest image decoded, in pixels (the decoders refuse more). */
#define IMG_PIXELS_MAX		((size_t)64 * 1024U * 1024U)

/* Decoding (decode.c, and one file per kind). */
enum img_kind img_sniff(const unsigned char *bytes, size_t length);
int img_decode(const unsigned char *bytes, size_t length, struct img_bitmap *bitmap);
void img_bitmap_release(struct img_bitmap *bitmap);
int img_bitmap_create(struct img_bitmap *bitmap, int width, int height);
void img_bitmap_renew(struct img_bitmap *bitmap);
int img_decode_jpeg(const unsigned char *bytes, size_t length, struct img_bitmap *bitmap);
int img_decode_png(const unsigned char *bytes, size_t length, struct img_bitmap *bitmap);
int img_decode_gif(const unsigned char *bytes, size_t length, struct img_bitmap *bitmap);

#endif
