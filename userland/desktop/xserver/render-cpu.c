/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing layer in memory: each operation of render.h drawn pixel by
 * pixel into an image by the X rules.
 *
 * Every operation draws only inside its clip region and the image.  The
 * source a fill paints with is the foreground, a tile's pixel, or the
 * foreground and background where a stipple is set and clear; it is
 * joined with the pixel already there by the graphics context's function
 * (one of the sixteen boolean functions of source and destination), and
 * only the planes the plane mask names change.
 */

#include "userland/desktop/xserver/render.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static uint32_t render_depth_mask(unsigned depth);
static uint32_t render_apply(unsigned function, uint32_t source, uint32_t target);
static void render_store(struct x11_image *target, int x, int y, uint32_t source, unsigned function, uint32_t mask);
static int render_source(const struct x11_paint *paint, int x, int y, uint32_t *source);
static int render_wrap(int value, int size);
static void render_fill_box(struct x11_image *target, const struct x11_paint *paint, const struct x11_box *box);
static int render_clip_box(const struct x11_image *target, const struct x11_box *clip, const struct x11_box *box, struct x11_box *result);

/*
 * Fills boxes (on the target's coordinates) with a paint, inside a clip.
 * A box that overlaps another is painted again where they overlap.
 */
void
x11_render_boxes(
	struct x11_image *target,
	const struct x11_region *clip,
	const struct x11_paint *paint,
	const struct x11_box *boxes,
	size_t count)
{
	struct x11_box part;
	size_t index;
	size_t clip_index;
	int inside;

	/* Each box, in each box of the clip it meets. */
	for (index = 0U; index < count; index++) {
		for (clip_index = 0U; clip_index < clip->count; clip_index++) {
			inside = render_clip_box(target, &clip->boxes[clip_index], &boxes[index], &part);
			if (!inside)
				continue;
			render_fill_box(target, paint, &part);
		}
	}
}

/*
 * Fills spans (rows of pixels on the target's coordinates) with a paint,
 * inside a clip.
 */
void
x11_render_spans(
	struct x11_image *target,
	const struct x11_region *clip,
	const struct x11_paint *paint,
	const struct x11_span *spans,
	size_t count)
{
	struct x11_box box;
	size_t index;

	/* Each span is a box one pixel high. */
	for (index = 0U; index < count; index++) {
		box.x0 = spans[index].x0;
		box.y0 = spans[index].y;
		box.x1 = spans[index].x1;
		box.y1 = spans[index].y + 1;
		x11_render_boxes(target, clip, paint, &box, 1U);
	}
}

/*
 * Fills the pixels of a rectangle where a bitmap is set (one bit a
 * pixel, the most significant bit first, rows stride bytes apart) with a
 * paint, inside a clip: what a glyph of the core font draws.
 */
void
x11_render_mask(
	struct x11_image *target,
	const struct x11_region *clip,
	const struct x11_paint *paint,
	int x,
	int y,
	int width,
	int height,
	const uint8_t *bits,
	size_t stride)
{
	struct x11_box rectangle;
	struct x11_box part;
	struct x11_box pixel;
	size_t clip_index;
	unsigned byte;
	int inside;
	int row;
	int column;

	/* The rectangle of the bitmap on the target. */
	rectangle.x0 = x;
	rectangle.y0 = y;
	rectangle.x1 = x + width;
	rectangle.y1 = y + height;

	/* Each part of it inside a box of the clip, pixel by pixel where the bitmap is set. */
	for (clip_index = 0U; clip_index < clip->count; clip_index++) {
		inside = render_clip_box(target, &clip->boxes[clip_index], &rectangle, &part);
		if (!inside)
			continue;
		for (row = part.y0; row < part.y1; row++) {
			for (column = part.x0; column < part.x1; column++) {
				byte = bits[(size_t)(row - y) * stride + (size_t)(column - x) / 8U];
				if ((byte & (0x80U >> ((unsigned)(column - x) & 7U))) == 0U)
					continue;
				pixel.x0 = column;
				pixel.y0 = row;
				pixel.x1 = column + 1;
				pixel.y1 = row + 1;
				render_fill_box(target, paint, &pixel);
			}
		}
	}
}

/*
 * Copies pixels of a source image onto the target inside a clip (on the
 * target's coordinates): the target's pixel (x + i, y + j) takes the
 * source's (source_x + i, source_y + j), joined by a function and a plane
 * mask.  The caller keeps the clip to where the source can be read.  The
 * source may be the target itself, overlapping.  Returns 0, or ENOMEM
 * when an overlapping copy cannot be made.
 */
int
x11_render_copy(
	struct x11_image *target,
	const struct x11_region *clip,
	int x,
	int y,
	const struct x11_image *source,
	int source_x,
	int source_y,
	unsigned function,
	uint32_t plane_mask)
{
	struct x11_box readable;
	struct x11_box limit;
	struct x11_box part;
	uint32_t *copy;
	uint32_t mask;
	uint32_t all_planes;
	size_t clip_index;
	size_t width;
	size_t height;
	int dx;
	int dy;
	int inside;
	int row;
	int column;

	/* From the target to the source, and the part of the target whose source is in the source image. */
	dx = source_x - x;
	dy = source_y - y;
	readable.x0 = -dx;
	readable.y0 = -dy;
	readable.x1 = source->width - dx;
	readable.y1 = source->height - dy;
	all_planes = render_depth_mask(target->depth);
	mask = plane_mask & all_planes;

	/* Each box of the clip inside both images. */
	for (clip_index = 0U; clip_index < clip->count; clip_index++) {
		inside = x11_box_intersect(&clip->boxes[clip_index], &readable, &limit);
		if (!inside)
			continue;
		inside = render_clip_box(target, &limit, &limit, &part);
		if (!inside)
			continue;

		/* The source's pixels of the box, taken first: the target may be the source. */
		width = (size_t)(part.x1 - part.x0);
		height = (size_t)(part.y1 - part.y0);
		copy = malloc(width * height * sizeof(*copy));
		if (copy == NULL)
			return ENOMEM;
		for (row = 0; row < (int)height; row++)
			memcpy(copy + (size_t)row * width, source->pixels + (size_t)(part.y0 + dy + row) * source->stride + (size_t)(part.x0 + dx), width * sizeof(*copy));

		/* Each pixel joined with the target's; a copy into every plane is stored row by row. */
		if (function == X11_FUNCTION_COPY && mask == all_planes) {
			for (row = part.y0; row < part.y1; row++)
				memcpy(target->pixels + (size_t)row * target->stride + (size_t)part.x0, copy + (size_t)(row - part.y0) * width, width * sizeof(*copy));
			free(copy);
			continue;
		}
		for (row = part.y0; row < part.y1; row++) {
			for (column = part.x0; column < part.x1; column++)
				render_store(target, column, row, copy[(size_t)(row - part.y0) * width + (size_t)(column - part.x0)], function, mask);
		}
		free(copy);
	}

	/* Succeeded: copied. */
	return 0;
}

/*
 * Copies one plane of a source image onto the target inside a clip (on
 * the target's coordinates) as a bitmap: the target's pixel (x + i,
 * y + j) takes the paint's foreground where the source's pixel
 * (source_x + i, source_y + j) has the plane set and its background where
 * it is clear, joined by the paint's function and plane mask.  The caller
 * keeps the clip to where the source can be read.  The source may be the
 * target itself, overlapping.  Returns 0, or ENOMEM.
 */
int
x11_render_copy_plane(
	struct x11_image *target,
	const struct x11_region *clip,
	int x,
	int y,
	const struct x11_image *source,
	int source_x,
	int source_y,
	uint32_t plane,
	const struct x11_paint *paint)
{
	struct x11_box readable;
	struct x11_box limit;
	struct x11_box part;
	uint32_t *copy;
	uint32_t mask;
	uint32_t pixel;
	size_t clip_index;
	size_t width;
	size_t height;
	int dx;
	int dy;
	int inside;
	int row;
	int column;

	/* From the target to the source, and the part of the target whose source is in the source image. */
	dx = source_x - x;
	dy = source_y - y;
	readable.x0 = -dx;
	readable.y0 = -dy;
	readable.x1 = source->width - dx;
	readable.y1 = source->height - dy;
	mask = paint->plane_mask & render_depth_mask(target->depth);

	/* Each box of the clip inside both images. */
	for (clip_index = 0U; clip_index < clip->count; clip_index++) {
		inside = x11_box_intersect(&clip->boxes[clip_index], &readable, &limit);
		if (!inside)
			continue;
		inside = render_clip_box(target, &limit, &limit, &part);
		if (!inside)
			continue;

		/* The source's pixels of the box, taken first: the target may be the source. */
		width = (size_t)(part.x1 - part.x0);
		height = (size_t)(part.y1 - part.y0);
		copy = malloc(width * height * sizeof(*copy));
		if (copy == NULL)
			return ENOMEM;
		for (row = 0; row < (int)height; row++)
			memcpy(copy + (size_t)row * width, source->pixels + (size_t)(part.y0 + dy + row) * source->stride + (size_t)(part.x0 + dx), width * sizeof(*copy));

		/* Each pixel the foreground or the background by its plane, joined with the target's. */
		for (row = part.y0; row < part.y1; row++) {
			for (column = part.x0; column < part.x1; column++) {
				pixel = paint->background;
				if ((copy[(size_t)(row - part.y0) * width + (size_t)(column - part.x0)] & plane) != 0U)
					pixel = paint->foreground;
				render_store(target, column, row, pixel, paint->function, mask);
			}
		}
		free(copy);
	}

	/* Succeeded: copied. */
	return 0;
}

/*
 * Puts pixels (width by height, rows stride pixels apart, already of the
 * target's depth) onto the target at (x, y), joined by a function and a
 * plane mask, inside a clip.
 */
void
x11_render_put(
	struct x11_image *target,
	const struct x11_region *clip,
	int x,
	int y,
	int width,
	int height,
	const uint32_t *pixels,
	size_t stride,
	unsigned function,
	uint32_t plane_mask)
{
	struct x11_box rectangle;
	struct x11_box part;
	uint32_t mask;
	uint32_t value;
	size_t clip_index;
	int inside;
	int row;
	int column;

	/* The rectangle of the pixels on the target, and the planes that change. */
	rectangle.x0 = x;
	rectangle.y0 = y;
	rectangle.x1 = x + width;
	rectangle.y1 = y + height;
	mask = plane_mask & render_depth_mask(target->depth);

	/* Each part of it inside a box of the clip. */
	for (clip_index = 0U; clip_index < clip->count; clip_index++) {
		inside = render_clip_box(target, &clip->boxes[clip_index], &rectangle, &part);
		if (!inside)
			continue;
		for (row = part.y0; row < part.y1; row++) {
			for (column = part.x0; column < part.x1; column++) {
				value = pixels[(size_t)(row - y) * stride + (size_t)(column - x)];
				render_store(target, column, row, value, function, mask);
			}
		}
	}
}

/*
 * Reads a rectangle of an image into pixels (rows stride pixels apart).
 * A pixel outside the image reads as 0.
 */
void
x11_render_read(
	const struct x11_image *source,
	int x,
	int y,
	int width,
	int height,
	uint32_t *pixels,
	size_t stride)
{
	int row;
	int column;
	int from_x;
	int from_y;

	/* Each pixel, from the image when it is inside. */
	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++) {
			from_x = x + column;
			from_y = y + row;
			pixels[(size_t)row * stride + (size_t)column] = 0U;
			if (from_x < 0 || from_y < 0)
				continue;
			if (from_x >= source->width || from_y >= source->height)
				continue;
			pixels[(size_t)row * stride + (size_t)column] = source->pixels[(size_t)from_y * source->stride + (size_t)from_x];
		}
	}
}

/* Returns the planes a pixel of a depth has. */
static uint32_t
render_depth_mask(
	unsigned depth)
{
	/* Every plane of a 32-bit pixel. */
	if (depth >= 32U)
		return 0xffffffffU;

	/* The low planes. */
	return ((uint32_t)1U << depth) - 1U;
}

/* Joins a source pixel with a target pixel by one of the sixteen functions. */
static uint32_t
render_apply(
	unsigned function,
	uint32_t source,
	uint32_t target)
{
	/* The function's number names its truth table. */
	switch (function) {
	case 0U:
		return 0U;
	case 1U:
		return source & target;
	case 2U:
		return source & ~target;
	case 3U:
		return source;
	case 4U:
		return ~source & target;
	case 5U:
		return target;
	case 6U:
		return source ^ target;
	case 7U:
		return source | target;
	case 8U:
		return ~source & ~target;
	case 9U:
		return ~source ^ target;
	case 10U:
		return ~target;
	case 11U:
		return source | ~target;
	case 12U:
		return ~source;
	case 13U:
		return ~source | target;
	case 14U:
		return ~source | ~target;
	default:
		break;
	}

	/* Set. */
	return 0xffffffffU;
}

/* Stores a source pixel into the target, joined by a function, changing only the planes of a mask. */
static void
render_store(
	struct x11_image *target,
	int x,
	int y,
	uint32_t source,
	unsigned function,
	uint32_t mask)
{
	uint32_t *pixel;
	uint32_t joined;

	/* The pixel, and the source joined with it. */
	pixel = &target->pixels[(size_t)y * target->stride + (size_t)x];
	joined = render_apply(function, source, *pixel);

	/* Only the planes of the mask change. */
	*pixel = (*pixel & ~mask) | (joined & mask);
}

/*
 * Gives the source a paint has at a pixel of the target.  Returns
 * nonzero when the pixel is painted, 0 when a stipple leaves it.
 */
static int
render_source(
	const struct x11_paint *paint,
	int x,
	int y,
	uint32_t *source)
{
	const struct x11_image *pattern;
	uint32_t bit;
	int pattern_x;
	int pattern_y;

	/* The foreground. */
	if (paint->fill_style == X11_FILL_SOLID) {
		*source = paint->foreground;
		return 1;
	}

	/* The tile's pixel, the tile repeated from its origin. */
	if (paint->fill_style == X11_FILL_TILED) {
		pattern = paint->tile;
		pattern_x = render_wrap(x - paint->origin_x, pattern->width);
		pattern_y = render_wrap(y - paint->origin_y, pattern->height);
		*source = pattern->pixels[(size_t)pattern_y * pattern->stride + (size_t)pattern_x];
		return 1;
	}

	/* The stipple's bit, the stipple repeated from its origin. */
	pattern = paint->stipple;
	pattern_x = render_wrap(x - paint->origin_x, pattern->width);
	pattern_y = render_wrap(y - paint->origin_y, pattern->height);
	bit = pattern->pixels[(size_t)pattern_y * pattern->stride + (size_t)pattern_x] & 1U;

	/* Set: the foreground. */
	if (bit != 0U) {
		*source = paint->foreground;
		return 1;
	}

	/* Clear: the background for an opaque stipple, nothing for another. */
	if (paint->fill_style == X11_FILL_OPAQUE_STIPPLED) {
		*source = paint->background;
		return 1;
	}

	return 0;
}

/* Returns a value modulo a size, never negative. */
static int
render_wrap(
	int value,
	int size)
{
	int remainder;

	/* C's remainder takes the sign of the value. */
	remainder = value % size;
	if (remainder < 0)
		remainder += size;

	/* The place within the size. */
	return remainder;
}

/* Paints every pixel of a box that is inside the target. */
static void
render_fill_box(
	struct x11_image *target,
	const struct x11_paint *paint,
	const struct x11_box *box)
{
	uint32_t source;
	uint32_t mask;
	uint32_t all_planes;
	int painted;
	int row;
	int column;

	/* The planes that change. */
	all_planes = render_depth_mask(target->depth);
	mask = paint->plane_mask & all_planes;

	/* The foreground copied into every plane is stored as it is (most fills). */
	if (paint->fill_style == X11_FILL_SOLID &&
	    paint->function == X11_FUNCTION_COPY &&
	    mask == all_planes) {
		source = paint->foreground & mask;
		for (row = box->y0; row < box->y1; row++) {
			for (column = box->x0; column < box->x1; column++)
				target->pixels[(size_t)row * target->stride + (size_t)column] = source;
		}
		return;
	}

	/* Each pixel, with the source the paint has there. */
	for (row = box->y0; row < box->y1; row++) {
		for (column = box->x0; column < box->x1; column++) {
			painted = render_source(paint, column, row, &source);
			if (!painted)
				continue;
			render_store(target, column, row, source, paint->function, mask);
		}
	}
}

/*
 * Gives the part of a box inside a box of a clip and inside the target
 * image.  Returns nonzero when that part is not empty.
 */
static int
render_clip_box(
	const struct x11_image *target,
	const struct x11_box *clip,
	const struct x11_box *box,
	struct x11_box *result)
{
	struct x11_box bounds;
	struct x11_box clipped;
	int inside;

	/* Inside the clip's box. */
	inside = x11_box_intersect(clip, box, &clipped);
	if (!inside)
		return 0;

	/* Inside the image. */
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = target->width;
	bounds.y1 = target->height;
	inside = x11_box_intersect(&clipped, &bounds, result);
	if (!inside)
		return 0;

	/* Succeeded: the part. */
	return 1;
}
