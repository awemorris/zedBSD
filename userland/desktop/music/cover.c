/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The picture of an album's cover (ws120-p009): the JPEG (libjpeg-compat,
 * through the decoding Image Viewer and Files share, userland/desktop/picture)
 * or the PNG (libpng-compat) an m4a holds, decoded and made a square of a
 * side for the view to draw at any size: a cover that is not square gives
 * its middle square (ws177-p020), as a record sleeve shows it.
 */

#include "music.h"

#include "userland/desktop/picture/picture.h"

#include <compat/png/png.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The largest cover decoded: its pixels a side, and in all. */
#define COVER_SIDE_MAX		4096U
#define COVER_PIXELS_MAX	(4096UL * 4096UL)

static int cover_jpeg(const unsigned char *data, size_t size, struct kl_image *image);
static int cover_png(const unsigned char *data, size_t size, struct kl_image *image);

/*
 * Makes the picture of a cover: a square of a side, the cover's middle
 * square scaled into it.  Returns 0 (the image's pixels are the caller's, kl_image_release),
 * EINVAL for bytes that are not a JPEG or a PNG it reads, EFBIG, ENOMEM.
 */
int
mu_cover_picture(
	const unsigned char *data,
	size_t size,
	int side,
	struct kl_image *image)
{
	struct kl_image decoded;
	struct kl_image middle;
	int shorter;
	int error;

	/* The cover as stored: a JPEG starts with FF D8, a PNG with its signature. */
	memset(image, 0, sizeof(*image));
	memset(&decoded, 0, sizeof(decoded));
	error = EINVAL;
	if (size >= 2U && data[0] == 0xffU && data[1] == 0xd8U)
		error = cover_jpeg(data, size, &decoded);
	else if (size >= 8U && data[0] == 0x89U && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
		error = cover_png(data, size, &decoded);
	if (error != 0)
		return error;

	/* The square it is scaled into. */
	error = kl_image_create(image, side, side);
	if (error != 0) {
		kl_image_release(&decoded);
		return error;
	}

	/* The middle square of the decoded cover (its rows and stride as they are), scaled. */
	shorter = decoded.width;
	if (decoded.height < shorter)
		shorter = decoded.height;
	middle = decoded;
	middle.pixels = decoded.pixels + (size_t)((decoded.height - shorter) / 2) * decoded.stride + (size_t)((decoded.width - shorter) / 2);
	middle.width = shorter;
	middle.height = shorter;
	kl_image_scale(&middle, image);

	/* Succeeded: the decoded cover is not needed after. */
	kl_image_release(&decoded);
	return 0;
}

/* Decodes a JPEG, turned as its EXIF orientation says; 0 or an errno value. */
static int
cover_jpeg(
	const unsigned char *data,
	size_t size,
	struct kl_image *image)
{
	struct kl_picture picture;
	int orientation;
	int error;

	/* Decoded within the sizes taken. */
	error = kl_picture_jpeg(NULL, data, size, COVER_SIDE_MAX, COVER_PIXELS_MAX, &picture, &orientation);
	if (error != 0)
		return error;

	/* Turned. */
	error = kl_picture_orient(&picture, orientation);
	if (error != 0) {
		free(picture.pixels);
		return error;
	}

	/* The image takes the pixels (no padding between the rows). */
	image->pixels = picture.pixels;
	image->width = picture.width;
	image->height = picture.height;
	image->stride = (size_t)picture.width;
	return 0;
}

/* Decodes a PNG into premultiplied pixels; 0 or an errno value. */
static int
cover_png(
	const unsigned char *data,
	size_t size,
	struct kl_image *image)
{
	png_image png;
	unsigned char *pixels;
	const unsigned char *pixel;
	uint32_t *row;
	uint32_t alpha;
	int x;
	int y;
	int ok;
	int error;

	/* The header: a size taken. */
	memset(&png, 0, sizeof(png));
	png.version = PNG_IMAGE_VERSION;
	ok = png_image_begin_read_from_memory(&png, data, size);
	if (!ok)
		return EINVAL;
	if (png.width == 0U || png.height == 0U || png.width > COVER_SIDE_MAX || png.height > COVER_SIDE_MAX ||
	    (unsigned long)png.width * png.height > COVER_PIXELS_MAX) {
		png_image_free(&png);
		return EFBIG;
	}

	/* The pixels, 8-bit BGRA. */
	png.format = PNG_FORMAT_BGRA;
	pixels = malloc(PNG_IMAGE_SIZE(png));
	if (pixels == NULL) {
		png_image_free(&png);
		return ENOMEM;
	}

	/* Decoded. */
	ok = png_image_finish_read(&png, NULL, pixels, 0, NULL);
	if (!ok) {
		free(pixels);
		return EINVAL;
	}

	/* The image. */
	error = kl_image_create(image, (int)png.width, (int)png.height);
	if (error != 0) {
		free(pixels);
		return error;
	}

	/* Each pixel, its colour multiplied by its alpha. */
	for (y = 0; y < image->height; y++) {
		row = image->pixels + (size_t)y * image->stride;
		for (x = 0; x < image->width; x++) {
			pixel = pixels + ((size_t)y * png.width + (size_t)x) * 4U;
			alpha = pixel[3];
			row[x] = (alpha << 24) |
			    (((uint32_t)pixel[2] * alpha / 255U) << 16) |
			    (((uint32_t)pixel[1] * alpha / 255U) << 8) |
			    ((uint32_t)pixel[0] * alpha / 255U);
		}
	}

	/* Succeeded: the decoded bytes go. */
	free(pixels);
	return 0;
}
