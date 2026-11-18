/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scaling of keiland-preview (WS168 p003; preview.h): the size a
 * picture is made (within the size asked, its shape kept and never larger
 * than it is: contain; or the size asked itself: cover, the picture cut
 * to its shape from the middle), and the scaling itself, each pixel of
 * the result the average of the source pixels under it (weighed by how
 * much of each it covers; a pixel larger than the source's takes the one
 * under its middle).
 */

#include "preview.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void scale_row_span(double from, double to, int limit, int *first, int *last);

/*
 * Works out the size a picture of a size is made: within the size asked
 * (contain, never larger than the picture) or the size asked (cover).
 */
void
preview_fit(
	int width,
	int height,
	const struct preview_request *request,
	int *fitted_width,
	int *fitted_height)
{
	double across;
	double down;
	double scale;

	/* Cover: the size asked. */
	if (request->cover) {
		*fitted_width = request->width;
		*fitted_height = request->height;
		return;
	}

	/* Contain: the smaller of the two scales, at most 1. */
	across = (double)request->width / (double)width;
	down = (double)request->height / (double)height;
	scale = across;
	if (down < scale)
		scale = down;
	if (scale > 1.0)
		scale = 1.0;
	*fitted_width = (int)((double)width * scale + 0.5);
	*fitted_height = (int)((double)height * scale + 0.5);
	if (*fitted_width < 1)
		*fitted_width = 1;
	if (*fitted_height < 1)
		*fitted_height = 1;
}

/*
 * Scales a picture as asked (preview_fit's size; for cover, the middle of
 * the picture of the size's shape).  Returns 0, or ENOMEM.
 */
int
preview_scale(
	const struct preview_image *source,
	const struct preview_request *request,
	struct preview_image *scaled)
{
	double left;
	double top;
	double area_width;
	double area_height;
	double step_x;
	double step_y;
	double weight;
	double sums[4];
	double cover_x;
	double cover_y;
	double from_x;
	double from_y;
	double to_x;
	double to_y;
	uint32_t pixel;
	int width;
	int height;
	int x;
	int y;
	int sx;
	int sy;
	int first_x;
	int last_x;
	int first_y;
	int last_y;
	int channel;

	/* The size, and the part of the source used: all of it, or for cover the middle of the result's shape. */
	preview_fit(source->width, source->height, request, &width, &height);
	left = 0.0;
	top = 0.0;
	area_width = (double)source->width;
	area_height = (double)source->height;
	if (request->cover && (double)source->width * (double)height > (double)source->height * (double)width) {
		area_width = (double)source->height * (double)width / (double)height;
		left = ((double)source->width - area_width) / 2.0;
	} else if (request->cover) {
		area_height = (double)source->width * (double)height / (double)width;
		top = ((double)source->height - area_height) / 2.0;
	}

	/* The result. */
	scaled->width = width;
	scaled->height = height;
	scaled->pixels = malloc((size_t)width * (size_t)height * sizeof(scaled->pixels[0]));
	if (scaled->pixels == NULL)
		return ENOMEM;

	/* Each result pixel: the source pixels its square covers, each weighed by its share. */
	step_x = area_width / (double)width;
	step_y = area_height / (double)height;
	for (y = 0; y < height; y++) {
		from_y = top + (double)y * step_y;
		to_y = from_y + step_y;
		scale_row_span(from_y, to_y, source->height, &first_y, &last_y);
		for (x = 0; x < width; x++) {
			from_x = left + (double)x * step_x;
			to_x = from_x + step_x;
			scale_row_span(from_x, to_x, source->width, &first_x, &last_x);
			memset(sums, 0, sizeof(sums));
			weight = 0.0;
			for (sy = first_y; sy <= last_y; sy++) {
				cover_y = 1.0;
				if ((double)sy < from_y)
					cover_y -= from_y - (double)sy;
				if ((double)(sy + 1) > to_y)
					cover_y -= (double)(sy + 1) - to_y;
				if (cover_y <= 0.0)
					cover_y = 1.0;
				for (sx = first_x; sx <= last_x; sx++) {
					cover_x = 1.0;
					if ((double)sx < from_x)
						cover_x -= from_x - (double)sx;
					if ((double)(sx + 1) > to_x)
						cover_x -= (double)(sx + 1) - to_x;
					if (cover_x <= 0.0)
						cover_x = 1.0;
					pixel = source->pixels[(size_t)sy * (size_t)source->width + (size_t)sx];
					for (channel = 0; channel < 4; channel++)
						sums[channel] += cover_x * cover_y * (double)((pixel >> (channel * 8)) & 0xffU);
					weight += cover_x * cover_y;
				}
			}

			/* The average. */
			pixel = 0;
			for (channel = 0; channel < 4; channel++)
				pixel |= (uint32_t)(sums[channel] / weight + 0.5) << (channel * 8);
			scaled->pixels[(size_t)y * (size_t)width + (size_t)x] = pixel;
		}
	}

	/* Scaled. */
	return 0;
}

/*
 * Frees a picture's pixels.
 */
void
preview_image_release(
	struct preview_image *image)
{
	/* The pixels. */
	free(image->pixels);
	image->pixels = NULL;
	image->width = 0;
	image->height = 0;
}

/* Finds the source pixels a span covers (at least the one under its middle), within the source. */
static void
scale_row_span(
	double from,
	double to,
	int limit,
	int *first,
	int *last)
{
	double middle;

	/* The whole pixels it touches. */
	*first = (int)from;
	*last = (int)to;
	if ((double)*last >= to && *last > *first)
		(*last)--;

	/* Smaller than a pixel: the one under its middle. */
	if (to - from < 1.0) {
		middle = (from + to) / 2.0;
		*first = (int)middle;
		*last = *first;
	}

	/* Within the source. */
	if (*first < 0)
		*first = 0;
	if (*last >= limit)
		*last = limit - 1;
	if (*first > *last)
		*first = *last;
}
