/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The canvas of PDF Viewer: the few shapes its frame is made of, drawn on
 * the CPU into premultiplied 0xAARRGGBB words.
 *
 * Colours are given as 0xAARRGGBB, not premultiplied.  Every shape is
 * clipped to the canvas.
 */

#include "viewer.h"

#include <math.h>

static void clip_span(const struct pv_canvas *canvas, int *x, int *y, int *width, int *height);
static uint32_t over(uint32_t pixel, uint32_t color, unsigned coverage);

/*
 * Fills a rectangle with a colour, replacing what was there.
 */
void
pv_canvas_fill(
	struct pv_canvas *canvas,
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
pv_canvas_blend(
	struct pv_canvas *canvas,
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
 */
void
pv_canvas_round(
	struct pv_canvas *canvas,
	int x,
	int y,
	int width,
	int height,
	int radius,
	uint32_t color)
{
	double centre_x;
	double centre_y;
	double distance;
	double coverage;
	int column;
	int line;
	int pixel_x;
	int pixel_y;

	/* The radius fits the rectangle. */
	if (radius * 2 > width)
		radius = width / 2;
	if (radius * 2 > height)
		radius = height / 2;

	/* Each pixel of the rectangle, covered fully except in the corners. */
	for (line = 0; line < height; line++) {
		pixel_y = y + line;
		if (pixel_y < 0 || pixel_y >= canvas->height)
			continue;
		for (column = 0; column < width; column++) {
			pixel_x = x + column;
			if (pixel_x < 0 || pixel_x >= canvas->width)
				continue;

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

			/* Blends the pixel by its coverage. */
			canvas->pixels[(size_t)pixel_y * canvas->stride + (size_t)pixel_x] =
			    over(canvas->pixels[(size_t)pixel_y * canvas->stride + (size_t)pixel_x], color, (unsigned)(coverage * 255.0 + 0.5));
		}
	}
}

/*
 * Copies opaque pixels (a page's raster, width words a row) with their
 * top left at a place.
 */
void
pv_canvas_copy(
	struct pv_canvas *canvas,
	int x,
	int y,
	const uint32_t *pixels,
	int width,
	int height)
{
	uint32_t *row;
	const uint32_t *source;
	int first_x;
	int first_y;
	int shown_width;
	int shown_height;
	int line;
	int column;

	/* The part of the pixels within the canvas. */
	first_x = x;
	first_y = y;
	shown_width = width;
	shown_height = height;
	clip_span(canvas, &first_x, &first_y, &shown_width, &shown_height);

	/* Copies each row of that part. */
	for (line = 0; line < shown_height; line++) {
		row = canvas->pixels + (size_t)(first_y + line) * canvas->stride + (size_t)first_x;
		source = pixels + (size_t)(first_y + line - y) * (size_t)width + (size_t)(first_x - x);
		for (column = 0; column < shown_width; column++)
			row[column] = source[column];
	}
}

/*
 * Copies opaque pixels (source_width words a row) stretched or shrunk to a
 * width and a height with their top left at a place: each pixel takes the
 * source pixel under its centre (ws081-p012, a page while two fingers
 * zoom, until it is drawn again at the new scale).
 */
void
pv_canvas_stretch(
	struct pv_canvas *canvas,
	int x,
	int y,
	int width,
	int height,
	const uint32_t *pixels,
	int source_width,
	int source_height)
{
	uint32_t *row;
	const uint32_t *source;
	long source_x;
	long source_y;
	long first_source_x;
	long first_left;
	long left;
	long step;
	long span;
	int first_x;
	int first_y;
	int shown_width;
	int shown_height;
	int line;
	int column;

	/* Nothing to stretch from or to. */
	if (width <= 0 ||
	    height <= 0 ||
	    source_width <= 0 ||
	    source_height <= 0)
		return;

	/* The part of the stretched pixels within the canvas. */
	first_x = x;
	first_y = y;
	shown_width = width;
	shown_height = height;
	clip_span(canvas, &first_x, &first_y, &shown_width, &shown_height);

	/*
	 * The source column under the first pixel's centre, (2c + 1) * source
	 * width / (2 * width), and what is left over of that division; each
	 * next pixel adds 2 * source width to it, so the columns follow without
	 * a division a pixel (BUG-259: a frame while the window is resized
	 * stretches every page).
	 */
	span = 2L * (long)width;
	step = 2L * (long)source_width;
	first_source_x = ((long)(first_x - x) * 2L + 1L) * (long)source_width / span;
	first_left = ((long)(first_x - x) * 2L + 1L) * (long)source_width % span;

	/* Each pixel of that part takes the source pixel under its centre. */
	for (line = 0; line < shown_height; line++) {
		source_y = ((long)(first_y + line - y) * 2L + 1L) * (long)source_height / (2L * (long)height);
		row = canvas->pixels + (size_t)(first_y + line) * canvas->stride + (size_t)first_x;
		source = pixels + (size_t)source_y * (size_t)source_width;
		source_x = first_source_x;
		left = first_left;
		for (column = 0; column < shown_width; column++) {
			row[column] = source[source_x];

			/* The next pixel's centre, as many source columns on as it passes. */
			left += step;
			while (left >= span) {
				left -= span;
				source_x++;
			}
		}
	}
}

/*
 * Blends a colour through an 8-bit coverage mask (a glyph), width bytes a
 * row, with its top left at a place.
 */
void
pv_canvas_mask(
	struct pv_canvas *canvas,
	int x,
	int y,
	const unsigned char *mask,
	int width,
	int height,
	uint32_t color)
{
	uint32_t *pixel;
	unsigned coverage;
	int line;
	int column;

	/* Each covered pixel within the canvas. */
	for (line = 0; line < height; line++) {
		if (y + line < 0 || y + line >= canvas->height)
			continue;
		for (column = 0; column < width; column++) {
			if (x + column < 0 || x + column >= canvas->width)
				continue;
			coverage = mask[(size_t)line * (size_t)width + (size_t)column];
			if (coverage == 0U)
				continue;
			pixel = canvas->pixels + (size_t)(y + line) * canvas->stride + (size_t)(x + column);
			*pixel = over(*pixel, color, coverage);
		}
	}
}

/* Cuts a rectangle to the canvas; an empty result has no width or height. */
static void
clip_span(
	const struct pv_canvas *canvas,
	int *x,
	int *y,
	int *width,
	int *height)
{
	/* The left and top edges. */
	if (*x < 0) {
		*width += *x;
		*x = 0;
	}

	/* The top edge. */
	if (*y < 0) {
		*height += *y;
		*y = 0;
	}

	/* The right and bottom edges. */
	if (*x + *width > canvas->width)
		*width = canvas->width - *x;
	if (*y + *height > canvas->height)
		*height = canvas->height - *y;

	/* Nothing left is an empty rectangle. */
	if (*width < 0)
		*width = 0;
	if (*height < 0)
		*height = 0;
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
