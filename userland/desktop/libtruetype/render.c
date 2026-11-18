/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Turning an outline into coverage.
 *
 * Each row of pixels is sampled TRUETYPE_SAMPLES times.  For one sample row
 * the scanline crosses the outline at a set of x positions; sorted, they
 * pair up into spans that are inside the shape.  A pixel's coverage is how
 * much of it those spans cover, summed over the sample rows.
 *
 * The rule for inside is non-zero winding, which is what TrueType uses: a
 * counter-clockwise contour inside a clockwise one is a hole, which is how
 * the middle of an "o" stays empty.
 */

#include "internal.h"

#include <errno.h>
#include <math.h>
#include <string.h>

/* How many times one sample row may cross the outline. */
#define CROSSINGS_MAX	128U

struct crossing {
	float x;
	int direction;
};

static void sort_crossings(struct crossing *crossings, unsigned count);
static void add_span(float *coverage, unsigned width, float start, float end,
		     float weight);

/*
 * Orders the crossings of one sample row by x.
 *
 * Insertion sort: a scanline crosses a letter a handful of times, and the
 * list is almost always already in order.
 */
static void
sort_crossings(
	struct crossing *crossings,
	unsigned count)
{
	struct crossing held;
	unsigned index, place;

	/* Process each crossing after the first. */
	for (index = 1; index < count; index++) {
		held = crossings[index];
		place = index;

		/* Moves the crossings that belong after this one. */
		while (place > 0 && crossings[place - 1U].x > held.x) {
			crossings[place] = crossings[place - 1U];
			place--;
		}
		crossings[place] = held;
	}
}

/*
 * Adds one horizontal span to a row of coverage.
 *
 * A span rarely lands on pixel boundaries, so the two end pixels take the
 * fraction they are covered by and the ones between take all of it.
 */
static void
add_span(
	float *coverage,
	unsigned width,
	float start,
	float end,
	float weight)
{
	unsigned first, last, index;
	float left, right;

	/* Handles a span outside the bitmap or with nothing in it. */
	if (end <= 0.0f || start >= (float)width || end <= start)
		return;

	/* Handles a span reaching past either edge. */
	if (start < 0.0f)
		start = 0.0f;
	if (end > (float)width)
		end = (float)width;
	first = (unsigned)start;
	last = (unsigned)end;

	/* Handles a span inside one pixel. */
	if (first == last) {
		coverage[first] += (end - start) * weight;
		return;
	}
	left = (float)(first + 1U) - start;
	coverage[first] += left * weight;

	/* Process each pixel the span covers completely. */
	for (index = first + 1U; index < last; index++)
		coverage[index] += weight;

	/* Handles the last pixel, which the span may not reach the end of. */
	if (last < width) {
		right = end - (float)last;
		coverage[last] += right * weight;
	}
}

/*
 * Implements the truetype rasterize operation.
 *
 * The outline is in pixels with y growing upward; the bitmap has y growing
 * downward from the top of the glyph's box, which metrics->top names.
 */
void
truetype_rasterize(
	const struct truetype_outline *outline,
	const struct truetype_glyph *metrics,
	uint8_t *bitmap,
	size_t stride)
{
	struct crossing crossings[CROSSINGS_MAX];
	float coverage[TRUETYPE_PIXELS_MAX];
	const struct truetype_point *a, *b;
	float y, x, value, weight;
	float origin_x, origin_y;
	unsigned row, sample, contour, index, first, count, crossing_count;
	int winding;

	memset(bitmap, 0, stride * metrics->height);

	/* Handles a glyph with no area to fill. */
	if (metrics->width == 0 || metrics->height == 0 ||
	    outline->point_count == 0)
		return;
	origin_x = (float)metrics->left;
	origin_y = (float)metrics->top;
	weight = 1.0f / (float)TRUETYPE_SAMPLES;

	/* Process each row of pixels. */
	for (row = 0; row < metrics->height; row++) {
		memset(coverage, 0, sizeof(coverage[0]) * metrics->width);

		/* Process each sample row inside the pixel row. */
		for (sample = 0; sample < TRUETYPE_SAMPLES; sample++) {
			/*
			 * The sample sits in the middle of its slice of the
			 * row, and the row is measured down from the top of
			 * the box, which is where the outline's y is highest.
			 */
			y = origin_y - ((float)row +
				((float)sample + 0.5f) / (float)TRUETYPE_SAMPLES);
			crossing_count = 0;
			first = 0;

			/* Process each contour. */
			for (contour = 0; contour < outline->contour_count;
			     contour++) {
				count = outline->ends[contour];

				/* Process each segment of the contour. */
				for (index = first; index + 1U < count;
				     index++) {
					a = &outline->points[index];
					b = &outline->points[index + 1U];

					/* Skips a segment this row misses. */
					if ((a->y <= y && b->y <= y) ||
					    (a->y > y && b->y > y))
						continue;

					/* Skips a segment with no height. */
					if (a->y == b->y)
						continue;

					/* Stops rather than overrunning. */
					if (crossing_count >= CROSSINGS_MAX)
						break;
					x = a->x + (y - a->y) *
						(b->x - a->x) / (b->y - a->y);
					crossings[crossing_count].x =
						x - origin_x;
					crossings[crossing_count].direction =
						b->y > a->y ? 1 : -1;
					crossing_count++;
				}
				first = count;
			}
			sort_crossings(crossings, crossing_count);
			winding = 0;

			/* Process each crossing, filling where the winding is not zero. */
			for (index = 0; index + 1U <= crossing_count; index++) {
				/* Handles the last crossing, which opens nothing. */
				if (index + 1U == crossing_count)
					break;
				winding += crossings[index].direction;

				/* Skips the stretch that is outside the shape. */
				if (winding == 0)
					continue;
				add_span(coverage, metrics->width,
					 crossings[index].x,
					 crossings[index + 1U].x, weight);
			}
		}

		/* Writes the row, turning coverage into 0..255. */
		for (index = 0; index < metrics->width; index++) {
			value = coverage[index];

			/* Handles coverage past one, which rounding can leave. */
			if (value > 1.0f)
				value = 1.0f;

			/* Handles coverage below zero, which cannot be drawn. */
			if (value < 0.0f)
				value = 0.0f;
			bitmap[(size_t)row * stride + index] =
				(uint8_t)(value * 255.0f + 0.5f);
		}
	}
}
