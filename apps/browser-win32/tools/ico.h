/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The writer of Windows icon files (ico.c), for the host tool that makes
 * OmniGrammaton.ico from the browser's icon drawn at each size.
 *
 * An .ico file is a directory (ICONDIR: reserved 0, type 1, count) of
 * entries (ICONDIRENTRY: width, height, colours 0, reserved 0, planes 1,
 * bit count 32, the image's bytes and its offset) followed by the images.
 * An image is either a PNG file (used for 256 x 256) or a 32-bit DIB: a
 * BITMAPINFOHEADER whose height is twice the image's, the pixels bottom row
 * first as blue, green, red, alpha, then the 1-bit AND mask, each row padded
 * to 32 bits.  All integers little-endian.  Pure: the caller encodes PNGs
 * (userland/desktop/picture/png-write.c); no system call.
 */

#ifndef OMNIGRAMMATON_ICO_H
#define OMNIGRAMMATON_ICO_H

#include <stddef.h>
#include <stdint.h>

/* The largest image side an icon holds. */
#define ICO_SIZE_MAX	256U

/*
 * One image of an icon.  Either png (with png_length) is set and the bytes
 * are stored as they are, or pixels is: width * height pixels, rows stride
 * pixels apart, each 0xAARRGGBB with straight (not premultiplied) alpha.
 */
struct ico_image {
	uint32_t width;			/* 1 to ICO_SIZE_MAX */
	uint32_t height;		/* 1 to ICO_SIZE_MAX */
	const uint32_t *pixels;
	size_t stride;			/* in pixels, at least width */
	const unsigned char *png;
	size_t png_length;
};

/*
 * Writes an icon file.
 *
 * images: count images, in the order the directory lists them; count 1 to
 *         255.  Two images of the same size are allowed (Windows picks).
 * out:    receives a malloc'd file the caller frees with free().
 * length: receives its length.
 * The directory's width and height bytes are the size, 0 for 256.  A DIB
 * image's AND mask bit is 1 where alpha is 0, else 0.  The images follow
 * the directory in the order given, with no gaps.
 * Returns 0, EINVAL (NULL arguments, count 0 or above 255, a size out of
 * range, an image with both or neither of pixels and png, stride below
 * width, a PNG not starting with the 8-byte PNG signature, a total above
 * UINT32_MAX), ENOMEM.  On failure *out is NULL and *length 0.
 */
int ico_write(const struct ico_image *images, unsigned count, unsigned char **out, size_t *length);

#endif
