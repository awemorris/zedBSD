/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A colour glyph decoded and scaled (ws102-p019; color-glyph.h): the PNG
 * libtruetype finds is read with libpng-compat into premultiplied pixels,
 * then made the size asked for, each pixel the average of the source
 * pixels it covers (the emoji fonts store large images, which text shows
 * small).
 */

#include "color-glyph.h"

#include <compat/png/png.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The largest source image read, a side (Noto Color Emoji's are 136 by 128). */
#define COLOR_SOURCE_MAX	512U

static int color_decode(const struct truetype_color_glyph *glyph, uint32_t **pixels, unsigned *width, unsigned *height);
static int color_size(const struct truetype_color_glyph *glyph, unsigned *width, unsigned *height);
static void color_geometry(const struct truetype_color_glyph *found, unsigned source_width, unsigned source_height, unsigned pixels, struct kl_color_image *out);
static void color_scale(const uint32_t *source, unsigned source_width, unsigned source_height, uint32_t *target, int width, int height);
static int color_round(float value);

/*
 * Draws a face's colour glyph at a size in pixels per em.  Returns 0 with
 * the image in out (its pixels the caller's to free), ENOENT when the glyph
 * has no colour image, or another errno value.
 */
int
kl_color_glyph(
	struct truetype_face *face,
	unsigned glyph,
	unsigned pixels,
	struct kl_color_image *out)
{
	struct truetype_color_glyph found;
	uint32_t *source;
	unsigned source_width;
	unsigned source_height;
	int error;

	/* The image at the strike nearest the size. */
	memset(out, 0, sizeof(*out));
	error = truetype_set_pixel_size(face, pixels);
	if (error != 0)
		return error;
	error = truetype_color_glyph(face, glyph, &found);
	if (error != 0)
		return error;

	/* Its pixels. */
	error = color_decode(&found, &source, &source_width, &source_height);
	if (error != 0)
		return error;

	/* The size asked for over the strike's. */
	color_geometry(&found, source_width, source_height, pixels, out);

	/* The scaled pixels. */
	out->pixels = malloc((size_t)out->width * (size_t)out->height * sizeof(uint32_t));
	if (out->pixels == NULL) {
		free(source);
		return ENOMEM;
	}

	/* Each target pixel from the source pixels under it. */
	color_scale(source, source_width, source_height, out->pixels, out->width, out->height);
	free(source);

	/* Succeeded: the glyph at the size. */
	return 0;
}

/*
 * Measures a face's colour glyph at a size in pixels per em without
 * decoding it: the box, the place and the advance kl_color_glyph gives,
 * from the PNG's header alone (out's pixels stay NULL).  Returns 0, ENOENT
 * when the glyph has no colour image, or another errno value.
 */
int
kl_color_glyph_metrics(
	struct truetype_face *face,
	unsigned glyph,
	unsigned pixels,
	struct kl_color_image *out)
{
	struct truetype_color_glyph found;
	unsigned source_width;
	unsigned source_height;
	int error;

	/* The image at the strike nearest the size. */
	memset(out, 0, sizeof(*out));
	error = truetype_set_pixel_size(face, pixels);
	if (error != 0)
		return error;
	error = truetype_color_glyph(face, glyph, &found);
	if (error != 0)
		return error;

	/* Its size, from the PNG's header. */
	error = color_size(&found, &source_width, &source_height);
	if (error != 0)
		return error;

	/* Succeeded: the box at the size asked for over the strike's. */
	color_geometry(&found, source_width, source_height, pixels, out);
	return 0;
}

/* Reads a colour glyph's PNG header for its size (as color_decode bounds it); 0 or EINVAL. */
static int
color_size(
	const struct truetype_color_glyph *glyph,
	unsigned *width,
	unsigned *height)
{
	png_image png;
	int ok;

	/* The header, of a size worth reading. */
	memset(&png, 0, sizeof(png));
	png.version = PNG_IMAGE_VERSION;
	ok = png_image_begin_read_from_memory(&png, glyph->png, glyph->png_size);
	if (!ok)
		return EINVAL;
	*width = png.width;
	*height = png.height;
	png_image_free(&png);
	if (*width == 0U || *height == 0U || *width > COLOR_SOURCE_MAX || *height > COLOR_SOURCE_MAX)
		return EINVAL;

	/* Succeeded: the source's size. */
	return 0;
}

/* Gives the box, the place and the advance of a colour glyph's image of a size at the size asked for. */
static void
color_geometry(
	const struct truetype_color_glyph *found,
	unsigned source_width,
	unsigned source_height,
	unsigned pixels,
	struct kl_color_image *out)
{
	float scale;

	/* The size asked for over the strike's; a box is at least one pixel a side. */
	scale = (float)pixels / (float)found->ppem;
	out->width = color_round((float)source_width * scale);
	out->height = color_round((float)source_height * scale);
	if (out->width < 1)
		out->width = 1;
	if (out->height < 1)
		out->height = 1;
	out->left = color_round((float)found->left * scale);
	out->top = color_round((float)found->top * scale);
	out->advance = color_round((float)found->advance * scale);
}

/* Decodes a colour glyph's PNG into premultiplied 0xAARRGGBB pixels. */
static int
color_decode(
	const struct truetype_color_glyph *glyph,
	uint32_t **pixels,
	unsigned *width,
	unsigned *height)
{
	png_image png;
	uint8_t *bytes;
	uint32_t alpha;
	size_t count;
	size_t index;
	int ok;

	/* The header, of a size worth reading. */
	memset(&png, 0, sizeof(png));
	png.version = PNG_IMAGE_VERSION;
	ok = png_image_begin_read_from_memory(&png, glyph->png, glyph->png_size);
	if (!ok)
		return EINVAL;
	if (png.width == 0U || png.height == 0U || png.width > COLOR_SOURCE_MAX || png.height > COLOR_SOURCE_MAX) {
		png_image_free(&png);
		return EINVAL;
	}

	/* The pixels as B, G, R, A bytes: one 0xAARRGGBB word each on a little-endian machine. */
	png.format = PNG_FORMAT_BGRA;
	count = (size_t)png.width * (size_t)png.height;
	bytes = malloc(count * 4U);
	if (bytes == NULL) {
		png_image_free(&png);
		return ENOMEM;
	}

	/* The decoding. */
	ok = png_image_finish_read(&png, NULL, bytes, 0, NULL);
	if (!ok) {
		free(bytes);
		return EINVAL;
	}

	/* Each colour multiplied by its alpha. */
	for (index = 0; index < count; index++) {
		alpha = bytes[index * 4U + 3U];
		bytes[index * 4U] = (uint8_t)((bytes[index * 4U] * alpha + 127U) / 255U);
		bytes[index * 4U + 1U] = (uint8_t)((bytes[index * 4U + 1U] * alpha + 127U) / 255U);
		bytes[index * 4U + 2U] = (uint8_t)((bytes[index * 4U + 2U] * alpha + 127U) / 255U);
	}

	/* Succeeded: the pixels (their bytes are the words). */
	*pixels = (uint32_t *)(void *)bytes;
	*width = png.width;
	*height = png.height;
	return 0;
}

/* Makes a picture another size: each target pixel the average of the source area it covers. */
static void
color_scale(
	const uint32_t *source,
	unsigned source_width,
	unsigned source_height,
	uint32_t *target,
	int width,
	int height)
{
	uint32_t sum[4];
	uint32_t pixel;
	unsigned x0;
	unsigned x1;
	unsigned y0;
	unsigned y1;
	unsigned sx;
	unsigned sy;
	unsigned count;
	unsigned channel;
	int x;
	int y;

	/* Each target pixel. */
	for (y = 0; y < height; y++) {
		y0 = (unsigned)y * source_height / (unsigned)height;
		y1 = ((unsigned)y + 1U) * source_height / (unsigned)height;
		if (y1 <= y0)
			y1 = y0 + 1U;
		for (x = 0; x < width; x++) {
			x0 = (unsigned)x * source_width / (unsigned)width;
			x1 = ((unsigned)x + 1U) * source_width / (unsigned)width;
			if (x1 <= x0)
				x1 = x0 + 1U;

			/* The source pixels under it, summed by channel. */
			memset(sum, 0, sizeof(sum));
			count = 0;
			for (sy = y0; sy < y1 && sy < source_height; sy++) {
				for (sx = x0; sx < x1 && sx < source_width; sx++) {
					pixel = source[(size_t)sy * source_width + sx];
					for (channel = 0; channel < 4U; channel++)
						sum[channel] += (pixel >> (channel * 8U)) & 0xffU;
					count++;
				}
			}

			/* Their average. */
			pixel = 0;
			for (channel = 0; count != 0U && channel < 4U; channel++)
				pixel |= ((sum[channel] + count / 2U) / count) << (channel * 8U);
			target[(size_t)y * (size_t)width + (size_t)x] = pixel;
		}
	}
}

/* Rounds to the nearest whole number, halves away from zero. */
static int
color_round(
	float value)
{
	/* Down for a negative value, up for a positive one. */
	if (value < 0.0f)
		return (int)(value - 0.5f);
	return (int)(value + 0.5f);
}
