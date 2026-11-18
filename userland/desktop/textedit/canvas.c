/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The canvas of Text Editor (PDF Viewer's canvas.c): the few shapes its
 * frame is made of, drawn on the CPU into premultiplied 0xAARRGGBB words.
 *
 * Colours are given as 0xAARRGGBB, not premultiplied.  Every shape is
 * clipped to the canvas's clip rectangle (the whole canvas unless
 * te_canvas_clip narrows it, as for the text scrolled under the gutter).
 */

#include "textedit.h"

#include <math.h>

static void clip_span(const struct te_canvas *canvas, int *x, int *y, int *width, int *height);
static int clip_inside(const struct te_canvas *canvas, int x, int y);
static uint32_t over(uint32_t pixel, uint32_t color, unsigned coverage);
static void round_full(uint32_t *pixels, int count, uint32_t color);

/*
 * Clips drawing to a rectangle (within the canvas) until te_canvas_unclip.
 */
void
te_canvas_clip(
	struct te_canvas *canvas,
	int x,
	int y,
	int width,
	int height)
{
	/* The whole canvas first, cut to the rectangle. */
	te_canvas_unclip(canvas);
	clip_span(canvas, &x, &y, &width, &height);
	canvas->clip_x = x;
	canvas->clip_y = y;
	canvas->clip_width = width;
	canvas->clip_height = height;
}

/*
 * Lets drawing reach the whole canvas again.
 */
void
te_canvas_unclip(
	struct te_canvas *canvas)
{
	/* The clip is the canvas. */
	canvas->clip_x = 0;
	canvas->clip_y = 0;
	canvas->clip_width = canvas->width;
	canvas->clip_height = canvas->height;
}

/*
 * Fills a rectangle with a colour, replacing what was there.
 */
void
te_canvas_fill(
	struct te_canvas *canvas,
	int x,
	int y,
	int width,
	int height,
	uint32_t color)
{
	uint32_t premultiplied;
	uint32_t *row;
	unsigned alpha;
	int column;
	int line;

	/* Keeps the rectangle within the canvas. */
	clip_span(canvas, &x, &y, &width, &height);

	/* The colour, premultiplied. */
	alpha = color >> 24;
	premultiplied = (alpha << 24) |
	    ((((color >> 16) & 0xffU) * alpha / 255U) << 16) |
	    ((((color >> 8) & 0xffU) * alpha / 255U) << 8) |
	    ((color & 0xffU) * alpha / 255U);

	/* Writes each pixel. */
	for (line = 0; line < height; line++) {
		row = canvas->pixels + (size_t)(y + line) * canvas->stride + (size_t)x;
		for (column = 0; column < width; column++)
			row[column] = premultiplied;
	}
}

/*
 * Blends a colour over a rectangle by the colour's alpha.
 */
void
te_canvas_blend(
	struct te_canvas *canvas,
	int x,
	int y,
	int width,
	int height,
	uint32_t color)
{
	uint32_t *row;
	int column;
	int line;

	/* Keeps the rectangle within the canvas. */
	clip_span(canvas, &x, &y, &width, &height);

	/* Blends each pixel. */
	for (line = 0; line < height; line++) {
		row = canvas->pixels + (size_t)(y + line) * canvas->stride + (size_t)x;
		for (column = 0; column < width; column++)
			row[column] = over(row[column], color, 255U);
	}
}

/*
 * Blends a rectangle with rounded corners, smoothed at its curved edges.
 *
 * The card is most of the window and is drawn at every frame, so only the
 * part within the clip is walked, the rows between the corners are blended
 * without the corners' test, and a pixel equal to the one blended before it
 * (the even ground under the card) takes the same result without being
 * blended again.  The pixels are the same as when each was blended alone
 * (BUG-143: drawing the frame of a key took 25 ms at 1920x1080 on the
 * host, about 4 ms now).
 */
void
te_canvas_round(
	struct te_canvas *canvas,
	int x,
	int y,
	int width,
	int height,
	int radius,
	uint32_t color)
{
	uint32_t *pixel;
	uint32_t blended_from;
	uint32_t blended_to;
	double centre_x;
	double centre_y;
	double distance;
	double coverage;
	unsigned blended_coverage;
	unsigned level;
	int clip_x;
	int clip_y;
	int clip_width;
	int clip_height;
	int first_column;
	int end_column;
	int first_line;
	int end_line;
	int column;
	int line;

	/* The radius fits the rectangle. */
	if (radius * 2 > width)
		radius = width / 2;
	if (radius * 2 > height)
		radius = height / 2;

	/* The rows and columns of the rectangle within the clip. */
	clip_x = x;
	clip_y = y;
	clip_width = width;
	clip_height = height;
	clip_span(canvas, &clip_x, &clip_y, &clip_width, &clip_height);
	first_column = clip_x - x;
	end_column = first_column + clip_width;
	first_line = clip_y - y;
	end_line = first_line + clip_height;

	/* No pixel has been blended yet: the remembered coverage is none a pixel can have. */
	blended_from = 0;
	blended_to = 0;
	blended_coverage = 256U;

	/* Each pixel within the clip, covered fully except in the corners. */
	for (line = first_line; line < end_line; line++) {
		/* A row between the corners is covered fully all along. */
		if (line >= radius && line < height - radius) {
			pixel = canvas->pixels + (size_t)(y + line) * canvas->stride + (size_t)(x + first_column);
			round_full(pixel, end_column - first_column, color);
			continue;
		}

		/* A row of the corners: each pixel by the circle of its corner. */
		for (column = first_column; column < end_column; column++) {
			/* The corner's circle decides a corner pixel's coverage. */
			coverage = 1.0;
			centre_x = -1.0;
			centre_y = -1.0;
			if (column < radius)
				centre_x = (double)radius;
			if (column >= width - radius)
				centre_x = (double)(width - radius);
			if (line < radius)
				centre_y = (double)radius;
			if (line >= height - radius)
				centre_y = (double)(height - radius);
			if (centre_x >= 0.0 && centre_y >= 0.0) {
				distance = sqrt(((double)column + 0.5 - centre_x) * ((double)column + 0.5 - centre_x) +
				    ((double)line + 0.5 - centre_y) * ((double)line + 0.5 - centre_y));
				coverage = (double)radius + 0.5 - distance;
				if (coverage <= 0.0)
					continue;
				if (coverage > 1.0)
					coverage = 1.0;
			}

			/* The same pixel at the same coverage as the last one blended takes its result. */
			pixel = canvas->pixels + (size_t)(y + line) * canvas->stride + (size_t)(x + column);
			level = (unsigned)(coverage * 255.0 + 0.5);
			if (*pixel == blended_from && level == blended_coverage) {
				*pixel = blended_to;
				continue;
			}

			/* Blends the pixel by its coverage, and remembers it for the next. */
			blended_from = *pixel;
			blended_coverage = level;
			blended_to = over(blended_from, color, level);
			*pixel = blended_to;
		}
	}
}

/*
 * Blends a colour through an 8-bit coverage mask (a glyph), stride bytes a
 * row, with its top left at a place.
 */
void
te_canvas_mask(
	struct te_canvas *canvas,
	int x,
	int y,
	const unsigned char *mask,
	int width,
	int height,
	size_t stride,
	uint32_t color)
{
	uint32_t *pixel;
	unsigned coverage;
	int line;
	int column;
	int inside;

	/* Each covered pixel within the clip. */
	for (line = 0; line < height; line++) {
		for (column = 0; column < width; column++) {
			inside = clip_inside(canvas, x + column, y + line);
			if (!inside)
				continue;
			coverage = mask[(size_t)line * stride + (size_t)column];
			if (coverage == 0U)
				continue;
			pixel = canvas->pixels + (size_t)(y + line) * canvas->stride + (size_t)(x + column);
			*pixel = over(*pixel, color, coverage);
		}
	}
}

/*
 * Blends premultiplied 0xAARRGGBB pixels (a colour glyph, ws102-p019),
 * width a row, with their top left at a place.
 */
void
te_canvas_pixels(
	struct te_canvas *canvas,
	int x,
	int y,
	const uint32_t *pixels,
	int width,
	int height)
{
	uint32_t *pixel;
	uint32_t source;
	uint32_t result;
	unsigned keep;
	unsigned shift;
	int line;
	int column;
	int inside;

	/* Each pixel within the clip: the source over what its alpha leaves of the canvas. */
	for (line = 0; line < height; line++) {
		for (column = 0; column < width; column++) {
			inside = clip_inside(canvas, x + column, y + line);
			if (!inside)
				continue;
			source = pixels[(size_t)line * (size_t)width + (size_t)column];
			if ((source >> 24) == 0U)
				continue;

			/* Each channel of the premultiplied source over the premultiplied pixel. */
			pixel = canvas->pixels + (size_t)(y + line) * canvas->stride + (size_t)(x + column);
			keep = 255U - (source >> 24);
			result = 0;
			for (shift = 0; shift < 32U; shift += 8U)
				result |= (((source >> shift) & 0xffU) + (((*pixel >> shift) & 0xffU) * keep + 127U) / 255U) << shift;
			*pixel = result;
		}
	}
}

/* Cuts a rectangle to the clip; an empty result has no width or height. */
static void
clip_span(
	const struct te_canvas *canvas,
	int *x,
	int *y,
	int *width,
	int *height)
{
	int right;
	int bottom;

	/* The left edge. */
	if (*x < canvas->clip_x) {
		*width -= canvas->clip_x - *x;
		*x = canvas->clip_x;
	}

	/* The top edge. */
	if (*y < canvas->clip_y) {
		*height -= canvas->clip_y - *y;
		*y = canvas->clip_y;
	}

	/* The right and bottom edges. */
	right = canvas->clip_x + canvas->clip_width;
	bottom = canvas->clip_y + canvas->clip_height;
	if (*x + *width > right)
		*width = right - *x;
	if (*y + *height > bottom)
		*height = bottom - *y;

	/* Nothing left is an empty rectangle. */
	if (*width < 0)
		*width = 0;
	if (*height < 0)
		*height = 0;
}

/* Tells whether a pixel is within the clip. */
static int
clip_inside(
	const struct te_canvas *canvas,
	int x,
	int y)
{
	/* Left of or above the clip. */
	if (x < canvas->clip_x || y < canvas->clip_y)
		return 0;

	/* Right of or below it. */
	if (x >= canvas->clip_x + canvas->clip_width)
		return 0;
	if (y >= canvas->clip_y + canvas->clip_height)
		return 0;

	/* Inside. */
	return 1;
}

/* Blends a colour fully over a run of pixels, a pixel equal to the one before it taking its result. */
static void
round_full(
	uint32_t *pixels,
	int count,
	uint32_t color)
{
	uint32_t blended_from;
	uint32_t blended_to;
	int index;

	/* A run cut away by the clip has nothing to blend. */
	if (count <= 0)
		return;

	/* The first pixel is blended; each later one only when it differs from the one before. */
	blended_from = pixels[0];
	blended_to = over(blended_from, color, 255U);
	for (index = 0; index < count; index++) {
		/* A pixel unlike the one before is blended anew. */
		if (pixels[index] != blended_from) {
			blended_from = pixels[index];
			blended_to = over(blended_from, color, 255U);
		}

		/* The pixel takes the blended colour. */
		pixels[index] = blended_to;
	}
}

/* Blends a colour (not premultiplied) at a coverage (0 to 255) over a premultiplied pixel. */
static uint32_t
over(
	uint32_t pixel,
	uint32_t color,
	unsigned coverage)
{
	unsigned alpha;
	unsigned keep;
	unsigned channel;
	unsigned result[4];
	unsigned source[4];
	unsigned shift;
	int index;

	/* The source's alpha by the coverage, and what of the pixel stays. */
	alpha = (color >> 24) * coverage / 255U;
	keep = 255U - alpha;

	/* The source premultiplied, alpha first. */
	source[0] = alpha;
	source[1] = ((color >> 16) & 0xffU) * alpha / 255U;
	source[2] = ((color >> 8) & 0xffU) * alpha / 255U;
	source[3] = (color & 0xffU) * alpha / 255U;

	/* Each channel: the source over what stays of the pixel. */
	for (index = 0; index < 4; index++) {
		shift = (unsigned)(24 - index * 8);
		channel = (pixel >> shift) & 0xffU;
		result[index] = source[index] + channel * keep / 255U;
		if (result[index] > 255U)
			result[index] = 255U;
	}

	/* Reports the blended pixel. */
	return (result[0] << 24) | (result[1] << 16) | (result[2] << 8) | result[3];
}
