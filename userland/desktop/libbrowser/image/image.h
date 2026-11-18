/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Images (plan/ws074/design.md §10): the kind of an image told from its
 * first bytes, and its decoding into a bitmap the renderers draw.  JPEG
 * goes through libjpeg-compat, PNG through libpng-compat, GIF through
 * libgif-compat (the first frame) and WebP through libwebp-compat (still
 * images); the module depends on nothing of the browser but base.
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
	IMG_GIF,
	IMG_WEBP
};

/*
 * A decoded image: width by height pixels, rows top to bottom, each
 * 0xAARRGGBB with straight (not premultiplied) alpha, as the display list
 * and both renderers take them.  The pixels are the bitmap's own (freed by
 * img_bitmap_release).  serial tells bitmaps apart for the life of the
 * program (a GPU keeps an image's pixels by it).
 *
 * The pixels of a bitmap made by img_bitmap_create are held (hold): a
 * committed frame of the page holds them too (img_hold_retain), so that
 * they outlive the bitmap until the frame lets go; whoever draws into such
 * a bitmap again first makes its pixels its own (img_bitmap_writable).
 * A bitmap whose pixels live in an arena has no hold.
 */
struct img_hold;

struct img_bitmap {
	uint32_t *pixels;
	struct img_hold *hold;
	int width;
	int height;
	uint64_t serial;
	/* Set for a vector picture that has no size and no ratio of its own (the default size stands in). */
	int no_ratio;
	/* A vector picture whose width or height attribute is zero has that natural size though its bitmap has a pixel at least (has_natural). */
	int has_natural;
	int natural_width;
	int natural_height;
	/* Set for a picture drawn from vector markup, which the page can draw again at the size it is shown. */
	int vector;
};

/* The size layout gives a picture: its natural size when it has one apart from its bitmap's. */
static inline int
img_natural_width(
	const struct img_bitmap *bitmap)
{
	return bitmap->has_natural ? bitmap->natural_width : bitmap->width;
}

static inline int
img_natural_height(
	const struct img_bitmap *bitmap)
{
	return bitmap->has_natural ? bitmap->natural_height : bitmap->height;
}

/* The largest image decoded, in pixels (the decoders refuse more). */
#define IMG_PIXELS_MAX		((size_t)64 * 1024U * 1024U)

/* Decoding (decode.c, and one file per kind). */
enum img_kind img_sniff(const unsigned char *bytes, size_t length);
int img_decode(const unsigned char *bytes, size_t length, struct img_bitmap *bitmap);
void img_bitmap_release(struct img_bitmap *bitmap);
int img_bitmap_create(struct img_bitmap *bitmap, int width, int height);
void img_bitmap_renew(struct img_bitmap *bitmap);
int img_bitmap_writable(struct img_bitmap *bitmap);
struct img_hold *img_hold_retain(const struct img_bitmap *bitmap);
void img_hold_release(struct img_hold *hold);
int img_decode_jpeg(const unsigned char *bytes, size_t length, struct img_bitmap *bitmap);
int img_decode_png(const unsigned char *bytes, size_t length, struct img_bitmap *bitmap);
int img_decode_gif(const unsigned char *bytes, size_t length, struct img_bitmap *bitmap);
int img_decode_webp(const unsigned char *bytes, size_t length, struct img_bitmap *bitmap);

#endif
