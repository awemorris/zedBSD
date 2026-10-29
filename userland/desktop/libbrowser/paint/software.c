/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CPU reference renderer: a display list drawn into a bitmap in
 * memory, for the headless --render mode and the tests.
 *
 * The coverage rules are the ones the GPU renderer follows: a rectangle
 * covers each pixel by the area of the pixel square it overlaps, and a
 * glyph's coverage bitmap is placed with its pen position rounded to the
 * nearest whole pixel.  Colors are blended over the bitmap as straight
 * (not premultiplied) alpha, and the canvas is opaque.
 */

#include "paint/paint.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The color a translucent canvas is laid over: white. */
#define SOFTWARE_BACKDROP	0xffffffffU

static void software_rect(struct paint_bitmap *bitmap, const struct paint_item *item, layout_unit scroll_y, const struct paint_clip *clip);
static int software_text(struct paint_bitmap *bitmap, struct text_system *text, const struct paint_item *item, layout_unit scroll_y, const struct paint_clip *clip);
static void software_image(struct paint_bitmap *bitmap, const struct paint_item *item, layout_unit scroll_y, const struct paint_clip *clip);
static void software_blend(struct paint_bitmap *bitmap, int x, int y, uint32_t color, float coverage, int nearest);
static float software_overlap(float start, float end, int pixel);

/*
 * Allocates a bitmap of a size, cleared to black.
 */
int
paint_bitmap_create(
	struct paint_bitmap *bitmap,
	int width,
	int height)
{
	/* Refuses a bitmap of no pixels. */
	memset(bitmap, 0, sizeof(*bitmap));
	if (width <= 0 || height <= 0)
		return EINVAL;

	/* The pixels, row after row. */
	bitmap->pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
	if (bitmap->pixels == NULL)
		return ENOMEM;

	/* Succeeded: the bitmap has its size. */
	bitmap->width = width;
	bitmap->height = height;
	return 0;
}

/*
 * Frees a bitmap's pixels.
 */
void
paint_bitmap_release(
	struct paint_bitmap *bitmap)
{
	/* The pixels, and the size that described them. */
	free(bitmap->pixels);
	memset(bitmap, 0, sizeof(*bitmap));
}

/*
 * Draws a display list into a bitmap, the document scrolled up by scroll_y.
 */
int
paint_software(
	const struct paint_list *list,
	struct text_system *text,
	layout_unit scroll_y,
	struct paint_bitmap *bitmap)
{
	const struct paint_item *item;
	struct paint_clips clips;
	uint32_t canvas;
	size_t count;
	size_t index;
	int error;

	/* The canvas: its color laid over white, so the bitmap starts opaque. */
	count = (size_t)bitmap->width * (size_t)bitmap->height;
	canvas = paint_canvas_pixel(list->canvas_color);
	for (index = 0; index < count; index++)
		bitmap->pixels[index] = canvas;

	/* Draws each item in painting order, inside the clips the list starts and ends. */
	paint_clips_init(&clips, bitmap->width, bitmap->height);
	for (index = 0; index < list->items.count; index++) {
		item = wb_vector_at(&list->items, index);

		/* A clip starts. */
		if (item->kind == PAINT_CLIP) {
			paint_clips_push(&clips, item, scroll_y);
			continue;
		}

		/* The end of a clip. */
		if (item->kind == PAINT_UNCLIP) {
			paint_clips_pop(&clips);
			continue;
		}

		/* A rectangle. */
		if (item->kind == PAINT_RECT) {
			software_rect(bitmap, item, scroll_y, paint_clips_top(&clips));
			continue;
		}

		/* An image. */
		if (item->kind == PAINT_IMAGE) {
			software_image(bitmap, item, scroll_y, paint_clips_top(&clips));
			continue;
		}

		/* A run of glyphs. */
		error = software_text(bitmap, text, item, scroll_y, paint_clips_top(&clips));
		if (error != 0)
			return error;
	}

	/* Succeeded: the bitmap shows the page. */
	return 0;
}

/*
 * Reports the opaque pixel a canvas color makes over white: the color the
 * CPU renderer starts from and the GPU renderer clears to.
 */
uint32_t
paint_canvas_pixel(
	uint32_t color)
{
	struct paint_bitmap pixel;
	uint32_t value;

	/* A one-pixel bitmap, white, with the color blended over it. */
	value = SOFTWARE_BACKDROP;
	pixel.pixels = &value;
	pixel.width = 1;
	pixel.height = 1;
	software_blend(&pixel, 0, 0, color, 1.0f, 1);

	/* Reports the blended pixel. */
	return value;
}

/*
 * Writes a bitmap as a binary PPM (P6) file.
 */
int
paint_write_ppm(
	const struct paint_bitmap *bitmap,
	const char *path)
{
	struct wb_buffer out;
	uint32_t pixel;
	size_t count;
	size_t index;
	int error;

	/* The header: the magic, the size and the largest sample. */
	wb_buffer_init(&out);
	error = wb_buffer_printf(&out, "P6\n%d %d\n255\n", bitmap->width, bitmap->height);
	if (error != 0) {
		wb_buffer_release(&out);
		return error;
	}

	/* Reserves the samples: three bytes a pixel. */
	count = (size_t)bitmap->width * (size_t)bitmap->height;
	error = wb_buffer_reserve(&out, count * 3U);
	if (error != 0) {
		wb_buffer_release(&out);
		return error;
	}

	/* Each pixel's red, green and blue. */
	for (index = 0; index < count; index++) {
		pixel = bitmap->pixels[index];
		wb_buffer_append_byte(&out, (unsigned char)(pixel >> 16));
		wb_buffer_append_byte(&out, (unsigned char)(pixel >> 8));
		wb_buffer_append_byte(&out, (unsigned char)pixel);
	}

	/* Writes the file. */
	error = wb_file_write(path, out.data, out.length);
	wb_buffer_release(&out);
	if (error != 0)
		return error;

	/* Succeeded: the file holds the picture. */
	return 0;
}

/* Fills a rectangle, its edges covering the pixels they cross by area. */
static void
software_rect(
	struct paint_bitmap *bitmap,
	const struct paint_item *item,
	layout_unit scroll_y,
	const struct paint_clip *clip)
{
	float left;
	float top;
	float right;
	float bottom;
	float coverage_x;
	float coverage_y;
	int first_x;
	int last_x;
	int first_y;
	int last_y;
	int x;
	int y;

	/* The rectangle in pixels, scrolled, and cut to the clip. */
	left = layout_to_px(item->x);
	top = layout_to_px(item->y - scroll_y);
	right = layout_to_px(item->x + item->width);
	bottom = layout_to_px(item->y - scroll_y + item->height);
	if (left < clip->left)
		left = clip->left;
	if (top < clip->top)
		top = clip->top;
	if (right > clip->right)
		right = clip->right;
	if (bottom > clip->bottom)
		bottom = clip->bottom;
	if (right <= left || bottom <= top)
		return;

	/* The pixels it touches, within the bitmap. */
	first_x = (int)floorf(left);
	last_x = (int)ceilf(right);
	first_y = (int)floorf(top);
	last_y = (int)ceilf(bottom);
	if (first_x < 0)
		first_x = 0;
	if (first_y < 0)
		first_y = 0;
	if (last_x > bitmap->width)
		last_x = bitmap->width;
	if (last_y > bitmap->height)
		last_y = bitmap->height;

	/* Each pixel takes the color by the share of its square the rectangle covers. */
	for (y = first_y; y < last_y; y++) {
		coverage_y = software_overlap(top, bottom, y);
		for (x = first_x; x < last_x; x++) {
			coverage_x = software_overlap(left, right, x);
			software_blend(bitmap, x, y, item->color, coverage_x * coverage_y, 1);
		}
	}
}

/*
 * Draws an image item: each pixel it touches takes the image's pixel under
 * the pixel's centre, weighed by the share of its square the item's
 * rectangle (cut to the clip) covers, as a rectangle's pixels are.  The
 * GPU's fragment shader maps the pixels the same way, in the same
 * single-precision steps.
 */
static void
software_image(
	struct paint_bitmap *bitmap,
	const struct paint_item *item,
	layout_unit scroll_y,
	const struct paint_clip *clip)
{
	const struct img_bitmap *image;
	float origin_x;
	float origin_y;
	float scale_x;
	float scale_y;
	float left;
	float top;
	float right;
	float bottom;
	float coverage_x;
	float coverage_y;
	uint32_t texel;
	int first_x;
	int last_x;
	int first_y;
	int last_y;
	int u;
	int v;
	int x;
	int y;

	/* The image's place and its texels to a pixel. */
	image = item->image;
	origin_x = layout_to_px(item->x);
	origin_y = layout_to_px(item->y - scroll_y);
	scale_x = (float)image->width / layout_to_px(item->width);
	scale_y = (float)image->height / layout_to_px(item->height);

	/* The rectangle in pixels, scrolled, and cut to the clip. */
	left = origin_x;
	top = origin_y;
	right = layout_to_px(item->x + item->width);
	bottom = layout_to_px(item->y - scroll_y + item->height);
	if (left < clip->left)
		left = clip->left;
	if (top < clip->top)
		top = clip->top;
	if (right > clip->right)
		right = clip->right;
	if (bottom > clip->bottom)
		bottom = clip->bottom;
	if (right <= left || bottom <= top)
		return;

	/* The pixels it touches, within the bitmap. */
	first_x = (int)floorf(left);
	last_x = (int)ceilf(right);
	first_y = (int)floorf(top);
	last_y = (int)ceilf(bottom);
	if (first_x < 0)
		first_x = 0;
	if (first_y < 0)
		first_y = 0;
	if (last_x > bitmap->width)
		last_x = bitmap->width;
	if (last_y > bitmap->height)
		last_y = bitmap->height;

	/* Each pixel: the texel under its centre, by the share of its square covered. */
	for (y = first_y; y < last_y; y++) {
		coverage_y = software_overlap(top, bottom, y);
		v = (int)floorf(((float)y + 0.5f - origin_y) * scale_y);
		if (v < 0)
			v = 0;
		if (v > image->height - 1)
			v = image->height - 1;
		for (x = first_x; x < last_x; x++) {
			coverage_x = software_overlap(left, right, x);
			u = (int)floorf(((float)x + 0.5f - origin_x) * scale_x);
			if (u < 0)
				u = 0;
			if (u > image->width - 1)
				u = image->width - 1;
			texel = image->pixels[(size_t)v * (size_t)image->width + (size_t)u];
			software_blend(bitmap, x, y, texel, coverage_x * coverage_y, 0);
		}
	}
}

/* Draws a run of glyphs, each bitmap placed at its pen position rounded to a pixel. */
static int
software_text(
	struct paint_bitmap *bitmap,
	struct text_system *text,
	const struct paint_item *item,
	layout_unit scroll_y,
	const struct paint_clip *clip)
{
	struct text_glyph glyph;
	float coverage;
	int baseline;
	int origin_x;
	int origin_y;
	int column;
	int row;
	int pixel_x;
	int pixel_y;
	size_t index;
	int error;

	/* The baseline, on a whole pixel. */
	baseline = (int)floorf(layout_to_px(item->y - scroll_y) + 0.5f);

	/* Each glyph's coverage bitmap. */
	for (index = 0; index < item->glyph_count; index++) {
		error = text_glyph(text, &item->font, item->glyphs[index].code_point, 1, &glyph);
		if (error != 0)
			return error;

		/* A glyph without ink (a space) draws nothing. */
		if (glyph.bitmap == NULL)
			continue;

		/* The bitmap's top left, from the pen and the baseline. */
		if (item->vertical) {
			origin_x = (int)floorf(layout_to_px(item->x) + 0.5f) + glyph.top - glyph.height;
			/* The pen runs down from the run's start, rounded once as a horizontal run's is. */
			origin_y = (int)floorf(layout_to_px(item->y - scroll_y + item->glyphs[index].x) + 0.5f) + glyph.left;
		} else {
			origin_x = (int)floorf(layout_to_px(item->x + item->glyphs[index].x) + 0.5f) + glyph.left;
			origin_y = baseline - glyph.top;
		}

		/* Each covered pixel inside the clip (on whole pixels) takes the text color by its coverage. */
		for (row = 0; row < glyph.height; row++) {
			for (column = 0; column < glyph.width; column++) {
				pixel_x = origin_x + column;
				pixel_y = origin_y + row;
				if (item->vertical) {
					pixel_x = origin_x + glyph.height - 1 - row;
					pixel_y = origin_y + column;
				}
				if (pixel_x < clip->pixel_left || pixel_x >= clip->pixel_right ||
				    pixel_y < clip->pixel_top || pixel_y >= clip->pixel_bottom)
					continue;
				coverage = (float)glyph.bitmap[(size_t)row * (size_t)glyph.width + (size_t)column] / 255.0f;
				if (coverage > 0.0f)
					software_blend(bitmap, pixel_x, pixel_y, item->color, coverage, 1);
			}
		}
	}

	/* Succeeded: the run is drawn. */
	return 0;
}

/* Blends a color over one pixel by a coverage, with the color's own alpha. */
static void
software_blend(
	struct paint_bitmap *bitmap,
	int x,
	int y,
	uint32_t color,
	float coverage,
	int nearest)
{
	uint32_t *pixel;
	float alpha;
	float source;
	float target;
	uint32_t alpha_byte;
	uint32_t source_byte;
	uint32_t target_byte;
	uint32_t blended;
	int shift;

	/* A pixel outside the bitmap is not drawn. */
	if (x < 0 || y < 0 || x >= bitmap->width || y >= bitmap->height)
		return;

	/* The share of the color that shows: its alpha times the coverage. */
	alpha = (float)(color >> 24) / 255.0f * coverage;
	if (alpha <= 0.0f)
		return;
	if (alpha > 1.0f)
		alpha = 1.0f;

	/* Mixes each channel, red, green and blue, and keeps the pixel opaque. */
	pixel = &bitmap->pixels[(size_t)y * (size_t)bitmap->width + (size_t)x];
	blended = 0xff000000U;
	for (shift = 0; shift <= 16; shift += 8) {
		/* Whole image texels follow the integer premultiplied blend used by image compositors. */
		if (!nearest && coverage >= 1.0f) {
			alpha_byte = color >> 24;
			source_byte = (color >> shift) & 0xffU;
			target_byte = (*pixel >> shift) & 0xffU;
			source_byte = source_byte * alpha_byte / 255U;
			target_byte = target_byte * (255U - alpha_byte) / 255U;
			blended |= (source_byte + target_byte) << shift;
			continue;
		}

		/* Shapes and partial image coverage use the renderer's nearest integer coverage. */
		source = (float)((color >> shift) & 0xffU);
		target = (float)((*pixel >> shift) & 0xffU);
		target = source * alpha + target * (1.0f - alpha);
		target += 0.5f;
		blended |= (uint32_t)target << shift;
	}

	/* Stores the mixed pixel. */
	*pixel = blended;
}

/* Measures how much of a pixel's span [pixel, pixel + 1) lies in [start, end). */
static float
software_overlap(
	float start,
	float end,
	int pixel)
{
	float low;
	float high;

	/* The overlap's two ends. */
	low = (float)pixel;
	if (start > low)
		low = start;
	high = (float)pixel + 1.0f;
	if (end < high)
		high = end;

	/* No overlap is no coverage. */
	if (high <= low)
		return 0.0f;

	/* Reports the overlap, at most the whole pixel. */
	return high - low;
}
