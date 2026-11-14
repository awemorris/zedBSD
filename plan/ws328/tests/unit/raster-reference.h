/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An independent reference of the coverage definition of
 * vector/vec-raster.h, for the tests only: written from the definition by
 * brute force, sharing no code with the rasterizer.
 *
 * For each pixel and each of the VEC_SUBLINES sample lines, the winding
 * number is evaluated at REFERENCE_SAMPLES points evenly inside the pixel
 * along the line (x = px + (k + 0.5) / REFERENCE_SAMPLES), by counting the
 * directed crossings of the line left of the point; the covered length is
 * the share of samples where the rule holds.  The sampling in x differs
 * from the exact length by at most 1 / REFERENCE_SAMPLES per crossing in
 * the pixel, so with a shape crossing a sample line at most twice in a
 * pixel the alpha may differ from the exact one by 2 (8-bit steps); tests
 * compare with a tolerance of 2 plus one step for the rasterizer's own
 * float rounding.  It reads struct vec_lines' fields directly and calls no
 * function of the vector modules.
 */

#ifndef WS328_TEST_RASTER_REFERENCE_H
#define WS328_TEST_RASTER_REFERENCE_H

#include "vector/vec-raster.h"

#include <stdlib.h>

/* The samples along one sample line of one pixel. */
#define REFERENCE_SAMPLES	256

/* The most crossings of one sample line the reference keeps (more make it refuse, so a test cannot pass on a truncated reference). */
#define REFERENCE_CROSSINGS	4096

/* The widest rectangle the reference computes (a wider one is refused). */
#define REFERENCE_WIDTH_MAX	1024

/* One crossing of a sample line: where, and +1 for a segment going down, -1 going up. */
struct reference_crossing {
	double x;
	int direction;
};

/*
 * Finds where the closed contours of lines cross the horizontal line at
 * height y: a segment from (x0, y0) to (x1, y1) crosses it when
 * min(y0, y1) <= y < max(y0, y1) (its upper end held, its lower not; a
 * horizontal one never), at the x of its line there.  The vertices are
 * moved by (dx, dy) first.  Returns the number of crossings, or
 * REFERENCE_CROSSINGS + 1 when there are more than REFERENCE_CROSSINGS
 * (only the first REFERENCE_CROSSINGS are stored).
 */
CHECK_UNUSED static size_t
reference_crossings(
	const struct vec_lines *lines,
	double dx,
	double dy,
	double y,
	struct reference_crossing *crossings)
{
	const struct vec_contour *contour;
	const struct vec_vertex *a;
	const struct vec_vertex *b;
	double x0;
	double y0;
	double x1;
	double y1;
	size_t count;
	size_t c;
	size_t i;

	/* Every segment of every contour, with the closing one. */
	count = 0;
	for (c = 0; c < lines->contour_count; c++) {
		contour = &lines->contours[c];
		for (i = 0; i < contour->count; i++) {
			a = &lines->vertices[contour->first + i];
			b = &lines->vertices[contour->first + (i + 1) % contour->count];
			x0 = (double)a->x + dx;
			y0 = (double)a->y + dy;
			x1 = (double)b->x + dx;
			y1 = (double)b->y + dy;
			if (y0 == y1)
				continue;
			if (y0 < y1 && (y < y0 || y >= y1))
				continue;
			if (y1 < y0 && (y < y1 || y >= y0))
				continue;
			if (count >= REFERENCE_CROSSINGS)
				return REFERENCE_CROSSINGS + 1;
			crossings[count].x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
			crossings[count].direction = y1 > y0 ? 1 : -1;
			count++;
		}
	}

	/* Reports how many were found. */
	return count;
}

/*
 * Computes the reference alpha of every pixel of a rectangle of whole
 * pixels (left, top, width, height) into alpha (width * height bytes, row
 * by row), for lines moved by (dx, dy) and filled by rule.
 *
 * Returns 0, or -1 when the reference cannot compute it (width over
 * REFERENCE_WIDTH_MAX, or a sample line with more than
 * REFERENCE_CROSSINGS crossings); a test treats -1 as a failure.
 */
CHECK_UNUSED static int
reference_coverage(
	const struct vec_lines *lines,
	enum vec_fill_rule rule,
	double dx,
	double dy,
	int left,
	int top,
	int width,
	int height,
	unsigned char *alpha)
{
	static struct reference_crossing crossings[REFERENCE_CROSSINGS];
	double covered[REFERENCE_WIDTH_MAX];
	double y;
	double x;
	size_t count;
	size_t i;
	int winding;
	int row;
	int column;
	int sub;
	int k;
	int inside;
	double value;

	/* A rectangle wider than the row buffer is refused. */
	if (width < 0 || height < 0 || width > REFERENCE_WIDTH_MAX)
		return -1;

	/* Each pixel row, its sample lines summed per pixel. */
	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++)
			covered[column] = 0.0;
		for (sub = 0; sub < VEC_SUBLINES; sub++) {
			y = (double)(top + row) + ((double)sub + 0.5) / (double)VEC_SUBLINES;
			count = reference_crossings(lines, dx, dy, y, crossings);
			if (count > REFERENCE_CROSSINGS)
				return -1;
			for (column = 0; column < width; column++) {
				for (k = 0; k < REFERENCE_SAMPLES; k++) {
					x = (double)(left + column) + ((double)k + 0.5) / (double)REFERENCE_SAMPLES;
					winding = 0;
					for (i = 0; i < count; i++) {
						if (crossings[i].x < x)
							winding += crossings[i].direction;
					}
					if (rule == VEC_FILL_EVENODD)
						inside = (winding & 1) != 0;
					else
						inside = winding != 0;
					if (inside)
						covered[column] += 1.0 / (double)REFERENCE_SAMPLES;
				}
			}
		}
		for (column = 0; column < width; column++) {
			value = covered[column] / (double)VEC_SUBLINES;
			alpha[row * width + column] = (unsigned char)(int)(value * 255.0 + 0.5);
		}
	}

	/* Every pixel was computed. */
	return 0;
}

#endif
