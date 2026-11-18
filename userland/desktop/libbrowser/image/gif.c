/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GIF through libgif-compat: the file from memory, and its first image
 * put on a transparent canvas of the logical screen's size with its colour
 * map (the image's own, or the global one) and its transparent colour.
 * Animation (the later images) comes later; a file cut short shows what
 * was decoded of its first image.
 */

#include "image/image.h"

#include <compat/gif_lib.h>

#include <errno.h>
#include <string.h>

/*
 * The bytes libgif-compat reads through the callback, and how far it has
 * read.
 */
struct gif_image_source {
	const unsigned char *bytes;
	size_t length;
	size_t position;
};

static int gif_image_read(GifFileType *gif, GifByteType *buffer, int count);
static void gif_image_place(const GifFileType *gif, const SavedImage *image, int transparent, struct img_bitmap *bitmap);

/*
 * Decodes a GIF file's first image into a bitmap; EINVAL when the file
 * has no image libgif-compat could read.
 */
int
img_decode_gif(
	const unsigned char *bytes,
	size_t length,
	struct img_bitmap *bitmap)
{
	struct gif_image_source source;
	GraphicsControlBlock control;
	const SavedImage *image;
	GifFileType *gif;
	int transparent;
	int width;
	int height;
	int status;
	int error;

	/* The file, read from memory. */
	memset(bitmap, 0, sizeof(*bitmap));
	source.bytes = bytes;
	source.length = length;
	source.position = 0;
	gif = DGifOpen(&source, gif_image_read, &error);
	if (gif == NULL)
		return EINVAL;

	/* Its images; a file cut short keeps what was read. */
	DGifSlurp(gif);
	if (gif->ImageCount == 0) {
		DGifCloseFile(gif, &error);
		return EINVAL;
	}

	/* The canvas: the logical screen, or the first image's reach when the screen is empty. */
	image = &gif->SavedImages[0];
	width = gif->SWidth;
	height = gif->SHeight;
	if (width <= 0 || height <= 0) {
		width = image->ImageDesc.Left + image->ImageDesc.Width;
		height = image->ImageDesc.Top + image->ImageDesc.Height;
	}

	/* The canvas's pixels. */
	status = img_bitmap_create(bitmap, width, height);
	if (status != 0) {
		DGifCloseFile(gif, &error);
		return status;
	}

	/* The first image's transparent colour, when its graphic control extension gives one. */
	transparent = NO_TRANSPARENT_COLOR;
	status = DGifSavedExtensionToGCB(gif, 0, &control);
	if (status == GIF_OK)
		transparent = control.TransparentColor;

	/* The image on the canvas, then the file closed. */
	gif_image_place(gif, image, transparent, bitmap);
	DGifCloseFile(gif, &error);

	/* Succeeded: the bitmap holds the first image. */
	return 0;
}

/* The reading callback: the next bytes of the file in memory (fewer at its end). */
static int
gif_image_read(
	GifFileType *gif,
	GifByteType *buffer,
	int count)
{
	struct gif_image_source *source;
	size_t left;

	/* As many bytes as are asked for and remain. */
	source = gif->UserData;
	left = source->length - source->position;
	if ((size_t)count > left)
		count = (int)left;
	memcpy(buffer, source->bytes + source->position, (size_t)count);
	source->position += (size_t)count;

	/* Succeeded: the bytes given. */
	return count;
}

/*
 * Puts an image's pixels on the canvas: each index's colour from the
 * image's colour map (or the global one), none for the transparent index,
 * an index past the map or a pixel off the canvas.
 */
static void
gif_image_place(
	const GifFileType *gif,
	const SavedImage *image,
	int transparent,
	struct img_bitmap *bitmap)
{
	const ColorMapObject *map;
	const GifColorType *color;
	int index;
	int x;
	int y;
	int canvas_x;
	int canvas_y;

	/* The image's own colour map, or the global one; neither leaves the canvas transparent. */
	map = image->ImageDesc.ColorMap;
	if (map == NULL)
		map = gif->SColorMap;
	if (map == NULL || image->RasterBits == NULL)
		return;

	/* Each pixel of the image. */
	for (y = 0; y < image->ImageDesc.Height; y++) {
		canvas_y = image->ImageDesc.Top + y;
		if (canvas_y >= bitmap->height)
			break;
		for (x = 0; x < image->ImageDesc.Width; x++) {
			canvas_x = image->ImageDesc.Left + x;
			index = image->RasterBits[(size_t)y * (size_t)image->ImageDesc.Width + (size_t)x];

			/* Off the canvas, transparent, or not in the map: nothing drawn. */
			if (canvas_x >= bitmap->width)
				break;
			if (index == transparent || index >= map->ColorCount)
				continue;

			/* The colour, opaque. */
			color = &map->Colors[index];
			bitmap->pixels[(size_t)canvas_y * (size_t)bitmap->width + (size_t)canvas_x] =
			    0xff000000U | ((uint32_t)color->Red << 16) | ((uint32_t)color->Green << 8) | (uint32_t)color->Blue;
		}
	}
}
