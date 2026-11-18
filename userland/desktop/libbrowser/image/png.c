/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PNG through libpng-compat's simplified API: the file from memory, read
 * as 8-bit RGBA and packed into the bitmap with its alpha.
 */

#include "image/image.h"

#include <compat/png/png.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Decodes a PNG file into a bitmap; EINVAL when libpng-compat refuses it.
 */
int
img_decode_png(
	const unsigned char *bytes,
	size_t length,
	struct img_bitmap *bitmap)
{
	png_image image;
	unsigned char *rgba;
	const unsigned char *pixel;
	size_t count;
	size_t index;
	int status;
	int read;

	/* The header: the size and the file's own format. */
	memset(bitmap, 0, sizeof(*bitmap));
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	read = png_image_begin_read_from_memory(&image, bytes, length);
	if (!read)
		return EINVAL;

	/* A bitmap of the image's size. */
	status = img_bitmap_create(bitmap, (int)image.width, (int)image.height);
	if (status != 0) {
		png_image_free(&image);
		return status;
	}

	/* A buffer for the pixels as RGBA, 8 bits a component. */
	count = (size_t)image.width * (size_t)image.height;
	rgba = malloc(count * 4U);
	if (rgba == NULL) {
		png_image_free(&image);
		img_bitmap_release(bitmap);
		return ENOMEM;
	}

	/* The pixels read as RGBA. */
	image.format = PNG_FORMAT_RGBA;
	read = png_image_finish_read(&image, NULL, rgba, 0, NULL);
	if (!read) {
		free(rgba);
		img_bitmap_release(bitmap);
		return EINVAL;
	}

	/* Each pixel packed as alpha, red, green, blue from the top byte down. */
	for (index = 0; index < count; index++) {
		pixel = rgba + index * 4U;
		bitmap->pixels[index] = ((uint32_t)pixel[3] << 24) | ((uint32_t)pixel[0] << 16) | ((uint32_t)pixel[1] << 8) | (uint32_t)pixel[2];
	}

	/* The RGBA copy is done with. */
	free(rgba);

	/* Succeeded: the bitmap holds the image. */
	return 0;
}
