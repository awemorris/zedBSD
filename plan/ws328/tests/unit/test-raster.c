/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of vec-raster: the coverage of hand-computed shapes, the
 * coverage of many shapes against the independent brute-force reference
 * (rectangles at fractional places, triangles, a self-intersecting star
 * under both fill rules, holes, overlapping squares, hairlines, shapes
 * across tile edges, offsets), the sorting of tiles into empty, full and
 * partial and the merging of runs, the bounds, the clip, the pixel
 * lookup, the product and the union of coverages, every limit and every
 * error, and random polygons under the sanitizers.  Every expected value
 * is a hand calculation from the definition in vec-raster.h or comes from
 * the reference; no other implementation's output is used.
 */

#include "check.h"
#include "raster-reference.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "vector/vec-raster.h"

/* The most an alpha may differ from the reference's: the reference's sampling (2) and one step of float rounding. */
#define TOLERANCE	3

/* The side of the windows the reference is asked for. */
#define WINDOW		64

/* The number of random polygons per fill rule. */
#define RANDOM_SHAPES	1000

/* Adds a closed contour of n points to polylines with the building steps of vec-flatten. */
static void
add_contour(
	struct vec_lines *lines,
	const float *points,
	size_t n)
{
	size_t i;
	int error;

	error = vec_lines_begin(lines, points[0], points[1], VEC_VERTEX_CORNER);
	CHECK_LONG(error, 0);
	for (i = 1; i < n; i++) {
		error = vec_lines_add(lines, points[2 * i], points[2 * i + 1], VEC_VERTEX_CORNER);
		CHECK_LONG(error, 0);
	}
	vec_lines_close(lines);
}

/* Adds the axis-aligned rectangle [left, right) x [top, bottom) as a contour going clockwise on the screen (down on the right). */
static void
add_rect(
	struct vec_lines *lines,
	float left,
	float top,
	float right,
	float bottom)
{
	float points[8];

	points[0] = left;
	points[1] = top;
	points[2] = right;
	points[3] = top;
	points[4] = right;
	points[5] = bottom;
	points[6] = left;
	points[7] = bottom;
	add_contour(lines, points, 4);
}

/* Adds the same rectangle going the other way. */
static void
add_rect_reversed(
	struct vec_lines *lines,
	float left,
	float top,
	float right,
	float bottom)
{
	float points[8];

	points[0] = left;
	points[1] = top;
	points[2] = left;
	points[3] = bottom;
	points[4] = right;
	points[5] = bottom;
	points[6] = right;
	points[7] = top;
	add_contour(lines, points, 4);
}

/* Adds a 4 x 4 square with its upper left corner at (x, y). */
static void
add_square(
	struct vec_lines *lines,
	int x,
	int y)
{
	add_rect(lines, (float)x, (float)y, (float)(x + 4), (float)(y + 4));
}

/*
 * Checks the invariants of a coverage's structure, independently of the
 * rasterizer: tiles sorted by row then column and not repeated, every
 * partial tile neither empty nor full, runs sorted, not empty and not
 * touching on a row (they would be one run), no tile inside a run, and
 * the bounds the smallest rectangle of the nonzero pixels.
 */
static void
check_structure(
	const struct vec_coverage *cov)
{
	size_t i;
	size_t j;
	int zero;
	int full;
	int k;
	long left;
	long top;
	long right;
	long bottom;
	long x;
	long y;
	int found;

	CHECK(cov->tile_count <= VEC_RASTER_TILES_MAX);
	for (i = 0; i < cov->tile_count; i++) {
		if (i > 0) {
			CHECK(cov->tiles[i - 1].ty < cov->tiles[i].ty ||
			    (cov->tiles[i - 1].ty == cov->tiles[i].ty && cov->tiles[i - 1].tx < cov->tiles[i].tx));
		}
		zero = 0;
		full = 0;
		for (k = 0; k < VEC_TILE_PIXELS; k++) {
			if (cov->tiles[i].alpha[k] == 0)
				zero++;
			if (cov->tiles[i].alpha[k] == 255)
				full++;
		}
		CHECK(zero != VEC_TILE_PIXELS);
		CHECK(full != VEC_TILE_PIXELS);
	}
	for (i = 0; i < cov->run_count; i++) {
		CHECK(cov->runs[i].count > 0);
		if (i > 0) {
			CHECK(cov->runs[i - 1].ty < cov->runs[i].ty ||
			    (cov->runs[i - 1].ty == cov->runs[i].ty &&
			     (long)cov->runs[i - 1].tx + cov->runs[i - 1].count < cov->runs[i].tx));
		}
	}
	for (i = 0; i < cov->tile_count; i++) {
		for (j = 0; j < cov->run_count; j++) {
			if (cov->tiles[i].ty != cov->runs[j].ty)
				continue;
			CHECK(cov->tiles[i].tx < cov->runs[j].tx || cov->tiles[i].tx >= cov->runs[j].tx + cov->runs[j].count);
		}
	}

	/* The smallest rectangle holding every nonzero pixel, from the arrays directly. */
	found = 0;
	left = 0;
	top = 0;
	right = 0;
	bottom = 0;
	for (i = 0; i < cov->tile_count; i++) {
		for (k = 0; k < VEC_TILE_PIXELS; k++) {
			if (cov->tiles[i].alpha[k] == 0)
				continue;
			x = (long)cov->tiles[i].tx * VEC_TILE_SIZE + k % VEC_TILE_SIZE;
			y = (long)cov->tiles[i].ty * VEC_TILE_SIZE + k / VEC_TILE_SIZE;
			if (!found || x < left)
				left = x;
			if (!found || x + 1 > right)
				right = x + 1;
			if (!found || y < top)
				top = y;
			if (!found || y + 1 > bottom)
				bottom = y + 1;
			found = 1;
		}
	}
	for (i = 0; i < cov->run_count; i++) {
		x = (long)cov->runs[i].tx * VEC_TILE_SIZE;
		y = (long)cov->runs[i].ty * VEC_TILE_SIZE;
		if (!found || x < left)
			left = x;
		if (!found || x + (long)cov->runs[i].count * VEC_TILE_SIZE > right)
			right = x + (long)cov->runs[i].count * VEC_TILE_SIZE;
		if (!found || y < top)
			top = y;
		if (!found || y + VEC_TILE_SIZE > bottom)
			bottom = y + VEC_TILE_SIZE;
		found = 1;
	}
	CHECK_LONG(cov->bounds.left, left);
	CHECK_LONG(cov->bounds.top, top);
	CHECK_LONG(cov->bounds.right, right);
	CHECK_LONG(cov->bounds.bottom, bottom);
}

/* Tells whether a coverage is empty: no tile, no run, bounds all 0. */
static int
is_empty(
	const struct vec_coverage *cov)
{
	if (cov->tile_count != 0 || cov->run_count != 0)
		return 0;
	if (cov->bounds.left != 0 || cov->bounds.top != 0 || cov->bounds.right != 0 || cov->bounds.bottom != 0)
		return 0;
	return 1;
}

/*
 * Rasterizes lines and compares every pixel of a window with the
 * reference (a pixel outside the clip is 0 in the expectation), within
 * the tolerance; also checks the structure, that the bounds lie inside
 * the window (so the window held the whole shape), and that nothing
 * outside the window was drawn when the window holds the shape.  Returns
 * the largest difference found, or -1 when the call failed.
 */
static int
compare_window(
	const struct vec_lines *lines,
	enum vec_fill_rule rule,
	float ox,
	float oy,
	const struct vec_irect *clip,
	int left,
	int top,
	int width,
	int height,
	struct vec_coverage *cov)
{
	static unsigned char expected[REFERENCE_WIDTH_MAX * 80];
	int error;
	int worst;
	int x;
	int y;
	int diff;
	int want;
	int got;
	int ref_status;

	error = vec_rasterize(lines, rule, ox, oy, clip, cov);
	CHECK_LONG(error, 0);
	if (error != 0)
		return -1;
	ref_status = reference_coverage(lines, rule, (double)ox, (double)oy, left, top, width, height, expected);
	CHECK_LONG(ref_status, 0);
	if (ref_status != 0)
		return -1;
	check_structure(cov);

	worst = 0;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			want = expected[y * width + x];
			if (clip != NULL && (left + x < clip->left || left + x >= clip->right || top + y < clip->top || top + y >= clip->bottom))
				want = 0;
			got = (int)vec_coverage_at(cov, left + x, top + y);
			diff = got - want;
			if (diff < 0)
				diff = -diff;
			if (diff > worst)
				worst = diff;
			if (diff > TOLERANCE && worst == diff) {
				if (check_failed < CHECK_PRINT_MAX)
					printf("pixel (%d, %d): got %d, reference %d\n", left + x, top + y, got, want);
			}
		}
	}
	CHECK(worst <= TOLERANCE);

	/* The bounds lie inside the window when the window holds the shape. */
	if (cov->bounds.right > cov->bounds.left) {
		CHECK(cov->bounds.left >= left && cov->bounds.top >= top);
		CHECK(cov->bounds.right <= left + width && cov->bounds.bottom <= top + height);
	}
	return worst;
}

/* The shape of a regular star: five points joined every second one, around (cx, cy), radius r. */
static void
add_star(
	struct vec_lines *lines,
	float cx,
	float cy,
	float r)
{
	float points[10];
	int i;
	double angle;

	for (i = 0; i < 5; i++) {
		angle = (double)i * 2.0 * 2.0 * 3.14159265358979323846 / 5.0 - 3.14159265358979323846 / 2.0;
		points[2 * i] = cx + r * (float)cos(angle);
		points[2 * i + 1] = cy + r * (float)sin(angle);
	}
	add_contour(lines, points, 5);
}

/* The rectangle [1.25, 3.75) x [1.5, 3) by hand: every pixel's alpha, the structure and the bounds. */
static void
test_rect_by_hand(void)
{
	struct vec_lines lines;
	struct vec_coverage cov;
	int error;
	int x;
	int y;
	/* Row 0 is above the rectangle.  Row 1 has its lower half (8 of 16 lines), row 2 all lines; the columns have 0.75, 1, 1, 0.75 of the width (x 1.25..4). */
	static const unsigned expected[4][5] = {
		{ 0, 0, 0, 0, 0 },
		{ 0, 96, 128, 96, 0 },
		{ 0, 191, 255, 191, 0 },
		{ 0, 0, 0, 0, 0 }
	};

	vec_lines_init(&lines);
	vec_coverage_init(&cov);
	add_rect(&lines, 1.25f, 1.5f, 3.75f, 3.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	for (y = 0; y < 4; y++) {
		for (x = 0; x < 5; x++)
			CHECK_LONG(vec_coverage_at(&cov, x, y), expected[y][x]);
	}
	CHECK_LONG(cov.tile_count, 1);
	CHECK_LONG(cov.run_count, 0);
	CHECK_LONG(cov.tiles[0].tx, 0);
	CHECK_LONG(cov.tiles[0].ty, 0);
	CHECK_LONG(cov.bounds.left, 1);
	CHECK_LONG(cov.bounds.top, 1);
	CHECK_LONG(cov.bounds.right, 4);
	CHECK_LONG(cov.bounds.bottom, 3);
	check_structure(&cov);

	/* Moved half a pixel right: x 1.75..4.25 gives 0.25, 1, 1, 0.25. Pixel (1, 1) 0.25 * 0.5 = 0.125 -> 32; (1, 2) 0.25 -> 64; (4, 2) 0.25 -> 64. */
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.5f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 1, 1), 32);
	CHECK_LONG(vec_coverage_at(&cov, 1, 2), 64);
	CHECK_LONG(vec_coverage_at(&cov, 2, 2), 255);
	CHECK_LONG(vec_coverage_at(&cov, 4, 2), 64);
	CHECK_LONG(vec_coverage_at(&cov, 0, 2), 0);
	CHECK_LONG(vec_coverage_at(&cov, 5, 2), 0);
	CHECK_LONG(cov.bounds.right, 5);
	check_structure(&cov);

	/* Moved a quarter pixel down: y 1.75..3.25: row 1 has the lower quarter (4 lines), row 3 the upper quarter (4 lines). */
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.25f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 2, 1), 64);
	CHECK_LONG(vec_coverage_at(&cov, 2, 2), 255);
	CHECK_LONG(vec_coverage_at(&cov, 2, 3), 64);
	CHECK_LONG(vec_coverage_at(&cov, 1, 3), 48);
	CHECK_LONG(cov.bounds.bottom, 4);
	check_structure(&cov);

	vec_coverage_release(&cov);
	vec_lines_release(&lines);
}

/* The line a shape starts and stops on is held on the upper end and not on the lower one. */
static void
test_line_ties(void)
{
	struct vec_lines lines;
	struct vec_coverage cov;
	int x;
	int error;

	vec_lines_init(&lines);
	vec_coverage_init(&cov);

	/* The rectangle from y = 1.03125 (the height of line 0 of row 1) to 2.03125 (line 0 of row 2): the 16 lines of row 1 are inside, row 2's first is not. */
	add_rect(&lines, 0.0f, 1.03125f, 4.0f, 2.03125f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	for (x = 0; x < 4; x++) {
		CHECK_LONG(vec_coverage_at(&cov, x, 0), 0);
		CHECK_LONG(vec_coverage_at(&cov, x, 1), 255);
		CHECK_LONG(vec_coverage_at(&cov, x, 2), 0);
	}
	CHECK_LONG(vec_coverage_at(&cov, 4, 1), 0);
	check_structure(&cov);

	/* One line down (+ 0.0625): the lines of row 1 from the second on, and the first of row 2: 15 of 16 in row 1 (alpha 239 = floor(255 * 15 / 16 + 0.5)), 1 of 16 in row 2 (16). */
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0625f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 1, 0), 0);
	CHECK_LONG(vec_coverage_at(&cov, 1, 1), 239);
	CHECK_LONG(vec_coverage_at(&cov, 1, 2), 16);
	check_structure(&cov);

	/* A shape thinner than the space between two lines can fall between them: the height 0.05 from 2.04 holds no line (line heights are 2.03125 and 2.09375), so it is empty. */
	vec_lines_reset(&lines);
	add_rect(&lines, 0.0f, 2.04f, 4.0f, 2.09f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK(is_empty(&cov));

	/* A rectangle thinner in x than a pixel: [2.3, 2.7) over rows 0..9 gives 0.4 of each pixel of column 2. */
	vec_lines_reset(&lines);
	add_rect(&lines, 2.25f, 0.0f, 2.75f, 10.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 2, 0), 128);
	CHECK_LONG(vec_coverage_at(&cov, 2, 9), 128);
	CHECK_LONG(vec_coverage_at(&cov, 1, 5), 0);
	CHECK_LONG(vec_coverage_at(&cov, 3, 5), 0);
	CHECK_LONG(cov.bounds.left, 2);
	CHECK_LONG(cov.bounds.right, 3);
	CHECK_LONG(cov.bounds.top, 0);
	CHECK_LONG(cov.bounds.bottom, 10);
	check_structure(&cov);

	vec_coverage_release(&cov);
	vec_lines_release(&lines);
}

/* Tiles are sorted into empty, full (in runs that merge along a row) and partial. */
static void
test_tile_classes(void)
{
	struct vec_lines lines;
	struct vec_coverage cov;
	int error;

	vec_lines_init(&lines);
	vec_coverage_init(&cov);

	/* Three whole tiles in a row are one run. */
	add_rect(&lines, 0.0f, 0.0f, 48.0f, 16.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(cov.tile_count, 0);
	CHECK_LONG(cov.run_count, 1);
	if (cov.run_count == 1) {
		CHECK_LONG(cov.runs[0].tx, 0);
		CHECK_LONG(cov.runs[0].ty, 0);
		CHECK_LONG(cov.runs[0].count, 3);
	}
	CHECK_LONG(cov.bounds.left, 0);
	CHECK_LONG(cov.bounds.top, 0);
	CHECK_LONG(cov.bounds.right, 48);
	CHECK_LONG(cov.bounds.bottom, 16);
	CHECK_LONG(vec_coverage_at(&cov, 47, 15), 255);
	CHECK_LONG(vec_coverage_at(&cov, 48, 15), 0);
	CHECK_LONG(vec_coverage_at(&cov, 0, 16), 0);
	check_structure(&cov);

	/* Two tile rows of two tiles: a run on each row, in the order row then column. */
	vec_lines_reset(&lines);
	add_rect(&lines, 0.0f, 0.0f, 32.0f, 32.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(cov.tile_count, 0);
	CHECK_LONG(cov.run_count, 2);
	if (cov.run_count == 2) {
		CHECK_LONG(cov.runs[0].ty, 0);
		CHECK_LONG(cov.runs[0].count, 2);
		CHECK_LONG(cov.runs[1].ty, 1);
		CHECK_LONG(cov.runs[1].tx, 0);
		CHECK_LONG(cov.runs[1].count, 2);
	}
	check_structure(&cov);

	/* A fractional left edge makes the first tile partial and the other two a run. */
	vec_lines_reset(&lines);
	add_rect(&lines, 0.5f, 0.0f, 48.0f, 16.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(cov.tile_count, 1);
	CHECK_LONG(cov.run_count, 1);
	CHECK_LONG(vec_coverage_at(&cov, 0, 0), 128);
	CHECK_LONG(vec_coverage_at(&cov, 1, 0), 255);
	CHECK_LONG(vec_coverage_at(&cov, 15, 15), 255);
	if (cov.run_count == 1) {
		CHECK_LONG(cov.runs[0].tx, 1);
		CHECK_LONG(cov.runs[0].count, 2);
	}
	check_structure(&cov);

	/* Two whole tiles with an empty tile between them are two runs. */
	vec_lines_reset(&lines);
	add_rect(&lines, 0.0f, 0.0f, 16.0f, 16.0f);
	add_rect(&lines, 32.0f, 0.0f, 48.0f, 16.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(cov.tile_count, 0);
	CHECK_LONG(cov.run_count, 2);
	CHECK_LONG(vec_coverage_at(&cov, 20, 5), 0);
	CHECK_LONG(vec_coverage_at(&cov, 40, 5), 255);
	check_structure(&cov);

	/* Two abutting whole tiles from two contours are one run. */
	vec_lines_reset(&lines);
	add_rect(&lines, 0.0f, 0.0f, 16.0f, 16.0f);
	add_rect(&lines, 16.0f, 0.0f, 32.0f, 16.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(cov.run_count, 1);
	if (cov.run_count == 1)
		CHECK_LONG(cov.runs[0].count, 2);
	check_structure(&cov);

	/* A shape in negative coordinates sits in the tiles left of and above the origin. */
	vec_lines_reset(&lines);
	add_rect(&lines, -32.0f, -16.0f, -16.0f, 0.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(cov.run_count, 1);
	if (cov.run_count == 1) {
		CHECK_LONG(cov.runs[0].tx, -2);
		CHECK_LONG(cov.runs[0].ty, -1);
		CHECK_LONG(cov.runs[0].count, 1);
	}
	CHECK_LONG(vec_coverage_at(&cov, -32, -16), 255);
	CHECK_LONG(vec_coverage_at(&cov, -17, -1), 255);
	CHECK_LONG(vec_coverage_at(&cov, -33, -1), 0);
	CHECK_LONG(vec_coverage_at(&cov, -16, -1), 0);
	CHECK_LONG(vec_coverage_at(&cov, -17, 0), 0);
	CHECK_LONG(cov.bounds.left, -32);
	CHECK_LONG(cov.bounds.top, -16);
	CHECK_LONG(cov.bounds.right, -16);
	CHECK_LONG(cov.bounds.bottom, 0);
	check_structure(&cov);

	/* A fractional shape in negative coordinates, against the reference. */
	vec_lines_reset(&lines);
	add_rect(&lines, -20.5f, -18.25f, -3.0f, -2.5f);
	CHECK(compare_window(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, -24, -24, 32, 32, &cov) >= 0);

	vec_coverage_release(&cov);
	vec_lines_release(&lines);
}

/* Shapes against the reference, with offsets, under both fill rules. */
static void
test_against_reference(void)
{
	static const float offsets[3][2] = { { 0.0f, 0.0f }, { 0.25f, 0.5f }, { 0.5f, 0.25f } };
	static const float triangle[] = { 5.0f, 5.0f, 60.0f, 20.0f, 20.0f, 60.0f };
	static const float hairline[] = { 1.0f, 1.0f, 1.3f, 1.0f, 41.3f, 41.0f, 41.0f, 41.0f };
	static const float sliver[] = { 3.0f, 3.0f, 60.0f, 4.0f, 3.0f, 3.5f };
	static const float diamond[] = { 20.0f, 4.0f, 36.0f, 20.0f, 20.0f, 36.0f, 4.0f, 20.0f };
	struct vec_lines lines;
	struct vec_coverage cov;
	int rule;
	int o;

	vec_lines_init(&lines);
	vec_coverage_init(&cov);

	for (rule = 0; rule < 2; rule++) {
		for (o = 0; o < 3; o++) {
			/* A rectangle at fractional places across tile edges. */
			vec_lines_reset(&lines);
			add_rect(&lines, 10.5f, 3.0f, 40.25f, 35.0f);
			CHECK(compare_window(&lines, (enum vec_fill_rule)rule, offsets[o][0], offsets[o][1], NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);

			/* A big triangle across several tiles. */
			vec_lines_reset(&lines);
			add_contour(&lines, triangle, 3);
			CHECK(compare_window(&lines, (enum vec_fill_rule)rule, offsets[o][0], offsets[o][1], NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);

			/* A parallelogram thinner than a pixel across its width, a long sliver, a diamond. */
			vec_lines_reset(&lines);
			add_contour(&lines, hairline, 4);
			CHECK(compare_window(&lines, (enum vec_fill_rule)rule, offsets[o][0], offsets[o][1], NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);
			vec_lines_reset(&lines);
			add_contour(&lines, sliver, 3);
			CHECK(compare_window(&lines, (enum vec_fill_rule)rule, offsets[o][0], offsets[o][1], NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);
			vec_lines_reset(&lines);
			add_contour(&lines, diamond, 4);
			CHECK(compare_window(&lines, (enum vec_fill_rule)rule, offsets[o][0], offsets[o][1], NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);

			/* A star whose points cross each other. */
			vec_lines_reset(&lines);
			add_star(&lines, 30.0f, 30.0f, 26.0f);
			CHECK(compare_window(&lines, (enum vec_fill_rule)rule, offsets[o][0], offsets[o][1], NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);

			/* A square with a hole going the other way, and with a hole going the same way. */
			vec_lines_reset(&lines);
			add_rect(&lines, 4.5f, 4.5f, 50.0f, 50.25f);
			add_rect_reversed(&lines, 12.25f, 12.0f, 40.0f, 38.5f);
			CHECK(compare_window(&lines, (enum vec_fill_rule)rule, offsets[o][0], offsets[o][1], NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);
			vec_lines_reset(&lines);
			add_rect(&lines, 4.5f, 4.5f, 50.0f, 50.25f);
			add_rect(&lines, 12.25f, 12.0f, 40.0f, 38.5f);
			CHECK(compare_window(&lines, (enum vec_fill_rule)rule, offsets[o][0], offsets[o][1], NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);

			/* Two overlapping squares, going the same way and opposite ways. */
			vec_lines_reset(&lines);
			add_rect(&lines, 4.0f, 4.0f, 34.5f, 34.5f);
			add_rect(&lines, 20.25f, 20.25f, 50.0f, 50.0f);
			CHECK(compare_window(&lines, (enum vec_fill_rule)rule, offsets[o][0], offsets[o][1], NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);
			vec_lines_reset(&lines);
			add_rect(&lines, 4.0f, 4.0f, 34.5f, 34.5f);
			add_rect_reversed(&lines, 20.25f, 20.25f, 50.0f, 50.0f);
			CHECK(compare_window(&lines, (enum vec_fill_rule)rule, offsets[o][0], offsets[o][1], NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);
		}
	}

	vec_coverage_release(&cov);
	vec_lines_release(&lines);
}

/* The fill rules by hand: the star's center and the overlap of two squares. */
static void
test_fill_rules(void)
{
	struct vec_lines lines;
	struct vec_coverage cov;
	int error;

	vec_lines_init(&lines);
	vec_coverage_init(&cov);

	/* The star: the pentagon in its middle has winding 2, so it is filled by nonzero and a hole by evenodd; a point of the star has winding 1. */
	add_star(&lines, 32.0f, 32.0f, 28.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 32, 32), 255);
	CHECK_LONG(vec_coverage_at(&cov, 32, 8), 255);
	error = vec_rasterize(&lines, VEC_FILL_EVENODD, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 32, 32), 0);
	CHECK_LONG(vec_coverage_at(&cov, 32, 8), 255);

	/* Two squares of the same direction: the overlap has winding 2 (nonzero fills, evenodd empties it); an opposite direction makes it winding 0 for both. */
	vec_lines_reset(&lines);
	add_rect(&lines, 4.0f, 4.0f, 24.0f, 24.0f);
	add_rect(&lines, 14.0f, 14.0f, 34.0f, 34.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 19, 19), 255);
	CHECK_LONG(vec_coverage_at(&cov, 8, 8), 255);
	CHECK_LONG(vec_coverage_at(&cov, 30, 30), 255);
	CHECK_LONG(vec_coverage_at(&cov, 8, 30), 0);
	error = vec_rasterize(&lines, VEC_FILL_EVENODD, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 19, 19), 0);
	CHECK_LONG(vec_coverage_at(&cov, 8, 8), 255);
	CHECK_LONG(vec_coverage_at(&cov, 30, 30), 255);
	vec_lines_reset(&lines);
	add_rect(&lines, 4.0f, 4.0f, 24.0f, 24.0f);
	add_rect_reversed(&lines, 14.0f, 14.0f, 34.0f, 34.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 19, 19), 0);
	CHECK_LONG(vec_coverage_at(&cov, 8, 8), 255);
	CHECK_LONG(vec_coverage_at(&cov, 30, 30), 255);

	/* A hole going the other way is empty under both rules; going the same way only evenodd empties it. */
	vec_lines_reset(&lines);
	add_rect(&lines, 2.0f, 2.0f, 30.0f, 30.0f);
	add_rect_reversed(&lines, 8.0f, 8.0f, 20.0f, 20.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 12, 12), 0);
	CHECK_LONG(vec_coverage_at(&cov, 5, 5), 255);
	error = vec_rasterize(&lines, VEC_FILL_EVENODD, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 12, 12), 0);
	vec_lines_reset(&lines);
	add_rect(&lines, 2.0f, 2.0f, 30.0f, 30.0f);
	add_rect(&lines, 8.0f, 8.0f, 20.0f, 20.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 12, 12), 255);
	error = vec_rasterize(&lines, VEC_FILL_EVENODD, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 12, 12), 0);
	CHECK_LONG(vec_coverage_at(&cov, 5, 5), 255);

	/* Abutting rectangles have no seam under either rule. */
	vec_lines_reset(&lines);
	add_rect(&lines, 2.0f, 2.0f, 10.5f, 12.0f);
	add_rect(&lines, 10.5f, 2.0f, 20.0f, 12.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 10, 5), 255);
	error = vec_rasterize(&lines, VEC_FILL_EVENODD, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 10, 5), 255);

	vec_coverage_release(&cov);
	vec_lines_release(&lines);
}

/* The clip: pixels outside are 0, a clip off the tile grid cuts a tile partial, an empty or distant clip gives nothing, two aligned windows equal one. */
static void
test_clip(void)
{
	static const float triangle[] = { 2.0f, 3.0f, 62.5f, 9.0f, 20.0f, 61.0f };
	struct vec_lines lines;
	struct vec_coverage cov;
	struct vec_coverage left_half;
	struct vec_coverage right_half;
	struct vec_irect clip;
	int error;
	int x;
	int y;
	int disagree;

	vec_lines_init(&lines);
	vec_coverage_init(&cov);
	vec_coverage_init(&left_half);
	vec_coverage_init(&right_half);

	/* A clip that is not on the tile grid keeps one tile partial, with 0 outside the clip. */
	add_rect(&lines, -10.0f, -10.0f, 100.0f, 100.0f);
	clip.left = 3;
	clip.top = 3;
	clip.right = 13;
	clip.bottom = 13;
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, &clip, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(cov.tile_count, 1);
	CHECK_LONG(cov.run_count, 0);
	CHECK_LONG(vec_coverage_at(&cov, 3, 3), 255);
	CHECK_LONG(vec_coverage_at(&cov, 12, 12), 255);
	CHECK_LONG(vec_coverage_at(&cov, 2, 3), 0);
	CHECK_LONG(vec_coverage_at(&cov, 13, 12), 0);
	CHECK_LONG(vec_coverage_at(&cov, 3, 2), 0);
	CHECK_LONG(vec_coverage_at(&cov, 3, 13), 0);
	CHECK_LONG(cov.bounds.left, 3);
	CHECK_LONG(cov.bounds.top, 3);
	CHECK_LONG(cov.bounds.right, 13);
	CHECK_LONG(cov.bounds.bottom, 13);
	check_structure(&cov);

	/* A clip on the tile grid keeps full tiles full. */
	clip.left = 0;
	clip.top = 0;
	clip.right = 16;
	clip.bottom = 16;
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, &clip, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(cov.tile_count, 0);
	CHECK_LONG(cov.run_count, 1);
	check_structure(&cov);

	/* A clip bigger than the shape changes nothing; the bounds are those of the nonzero pixels. */
	clip.left = -100000;
	clip.top = -100000;
	clip.right = 100000;
	clip.bottom = 100000;
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, &clip, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(cov.bounds.left, -10);
	CHECK_LONG(cov.bounds.top, -10);
	CHECK_LONG(cov.bounds.right, 100);
	CHECK_LONG(cov.bounds.bottom, 100);
	CHECK_LONG(vec_coverage_at(&cov, -10, -10), 255);
	CHECK_LONG(vec_coverage_at(&cov, -11, -10), 0);
	CHECK_LONG(vec_coverage_at(&cov, 99, 99), 255);
	CHECK_LONG(vec_coverage_at(&cov, 100, 99), 0);
	check_structure(&cov);

	/* An empty clip, an inverted clip and a clip away from the shape give nothing, and succeed. */
	clip.left = 5;
	clip.top = 5;
	clip.right = 5;
	clip.bottom = 9;
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, &clip, &cov);
	CHECK_LONG(error, 0);
	CHECK(is_empty(&cov));
	clip.left = 10;
	clip.top = 10;
	clip.right = 2;
	clip.bottom = 2;
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, &clip, &cov);
	CHECK_LONG(error, 0);
	CHECK(is_empty(&cov));
	clip.left = 1000;
	clip.top = 1000;
	clip.right = 1100;
	clip.bottom = 1100;
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, &clip, &cov);
	CHECK_LONG(error, 0);
	CHECK(is_empty(&cov));

	/* A shape cut by an off-grid clip against the reference. */
	vec_lines_reset(&lines);
	add_contour(&lines, triangle, 3);
	clip.left = 5;
	clip.top = 7;
	clip.right = 51;
	clip.bottom = 44;
	CHECK(compare_window(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, &clip, 0, 0, WINDOW, WINDOW, &cov) >= 0);
	CHECK(compare_window(&lines, VEC_FILL_EVENODD, 0.25f, 0.5f, &clip, 0, 0, WINDOW, WINDOW, &cov) >= 0);

	/* Two windows on the tile grid hold the same pixels, and the same partial tiles, as one window. */
	clip.left = 0;
	clip.top = 0;
	clip.right = 32;
	clip.bottom = 64;
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.25f, 0.0f, &clip, &left_half);
	CHECK_LONG(error, 0);
	clip.left = 32;
	clip.right = 64;
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.25f, 0.0f, &clip, &right_half);
	CHECK_LONG(error, 0);
	clip.left = 0;
	clip.right = 64;
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.25f, 0.0f, &clip, &cov);
	CHECK_LONG(error, 0);
	disagree = 0;
	for (y = -2; y < 66; y++) {
		for (x = -2; x < 66; x++) {
			unsigned whole;
			unsigned parts;

			whole = vec_coverage_at(&cov, x, y);
			parts = vec_coverage_at(&left_half, x, y);
			if (parts == 0)
				parts = vec_coverage_at(&right_half, x, y);
			if (whole != parts)
				disagree++;
		}
	}
	CHECK_LONG(disagree, 0);
	CHECK_LONG(cov.tile_count, left_half.tile_count + right_half.tile_count);
	check_structure(&left_half);
	check_structure(&right_half);

	vec_coverage_release(&right_half);
	vec_coverage_release(&left_half);
	vec_coverage_release(&cov);
	vec_lines_release(&lines);
}

/* The lookup: partial tiles, runs, outside, negative and extreme coordinates. */
static void
test_lookup(void)
{
	struct vec_coverage empty;
	struct vec_coverage cov;
	struct vec_lines lines;
	int error;

	vec_coverage_init(&empty);
	vec_coverage_init(&cov);
	vec_lines_init(&lines);

	/* An empty coverage covers nothing, wherever it is asked. */
	CHECK_LONG(vec_coverage_at(&empty, 0, 0), 0);
	CHECK_LONG(vec_coverage_at(&empty, -1, -1), 0);
	CHECK_LONG(vec_coverage_at(&empty, INT32_MAX, INT32_MAX), 0);
	CHECK_LONG(vec_coverage_at(&empty, INT32_MIN, INT32_MIN), 0);
	CHECK_LONG(vec_coverage_at(&empty, INT32_MIN + 1, INT32_MIN + 7), 0);

	/* A run of three tiles with a partial tile after it, on rows 1 and 2. */
	add_rect(&lines, 16.0f, 16.0f, 64.5f, 32.0f);
	add_rect(&lines, 16.0f, 40.5f, 40.0f, 48.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 16, 16), 255);
	CHECK_LONG(vec_coverage_at(&cov, 63, 31), 255);
	CHECK_LONG(vec_coverage_at(&cov, 64, 20), 128);
	CHECK_LONG(vec_coverage_at(&cov, 64, 32), 0);
	CHECK_LONG(vec_coverage_at(&cov, 15, 20), 0);
	CHECK_LONG(vec_coverage_at(&cov, 20, 15), 0);
	CHECK_LONG(vec_coverage_at(&cov, 20, 40), 128);
	CHECK_LONG(vec_coverage_at(&cov, 20, 41), 255);
	CHECK_LONG(vec_coverage_at(&cov, 20, 47), 255);
	CHECK_LONG(vec_coverage_at(&cov, 20, 48), 0);
	CHECK_LONG(vec_coverage_at(&cov, 40, 41), 0);
	CHECK_LONG(vec_coverage_at(&cov, 100000, 100000), 0);
	CHECK_LONG(vec_coverage_at(&cov, INT32_MAX, 20), 0);
	CHECK_LONG(vec_coverage_at(&cov, INT32_MIN, 20), 0);
	CHECK_LONG(vec_coverage_at(&cov, 20, INT32_MIN + 1), 0);
	check_structure(&cov);

	vec_lines_release(&lines);
	vec_coverage_release(&cov);
	vec_coverage_release(&empty);
}

/* The life of a coverage: init, reset keeping the arrays, release, and a result replacing an earlier one. */
static void
test_life(void)
{
	struct vec_coverage cov;
	struct vec_lines lines;
	size_t capacity;
	int error;

	vec_lines_init(&lines);
	vec_coverage_init(&cov);
	CHECK(is_empty(&cov));
	CHECK(cov.tiles == NULL && cov.runs == NULL);

	add_rect(&lines, 1.5f, 1.5f, 40.5f, 40.5f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK(cov.tile_count > 0);
	CHECK(cov.run_count > 0);
	CHECK(!is_empty(&cov));

	/* Reset empties it and keeps the memory. */
	capacity = cov.tile_capacity;
	vec_coverage_reset(&cov);
	CHECK(is_empty(&cov));
	CHECK(cov.tiles != NULL);
	CHECK(cov.tile_capacity == capacity);

	/* A second call replaces what was there (it does not append). */
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	check_structure(&cov);
	CHECK_LONG(cov.run_count, 1);

	/* Rasterizing nothing replaces it with the empty coverage. */
	vec_lines_reset(&lines);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK(is_empty(&cov));

	/* Release frees and leaves it as init does. */
	vec_coverage_release(&cov);
	CHECK(is_empty(&cov));
	CHECK(cov.tiles == NULL && cov.runs == NULL);
	CHECK_LONG(cov.tile_capacity, 0);
	CHECK_LONG(cov.run_capacity, 0);
	vec_lines_release(&lines);
}

/* Inputs that cover nothing succeed with the empty coverage. */
static void
test_degenerate(void)
{
	struct vec_lines lines;
	struct vec_coverage cov;
	float line_there_and_back[] = { 2.0f, 2.0f, 30.0f, 20.0f };
	float horizontal[] = { 2.0f, 5.0f, 30.0f, 5.0f, 12.0f, 5.0f };
	float three_same[] = { 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f };
	float whole_lines[] = { 2.0f, 2.0f, 30.0f, 2.0f, 30.0f, 2.0f };
	int error;
	int rule;

	vec_lines_init(&lines);
	vec_coverage_init(&cov);

	for (rule = 0; rule < 2; rule++) {
		/* No contour at all. */
		vec_lines_reset(&lines);
		error = vec_rasterize(&lines, (enum vec_fill_rule)rule, 0.0f, 0.0f, NULL, &cov);
		CHECK_LONG(error, 0);
		CHECK(is_empty(&cov));

		/* One vertex, a segment to itself. */
		vec_lines_reset(&lines);
		error = vec_lines_begin(&lines, 5.5f, 5.5f, 0);
		CHECK_LONG(error, 0);
		error = vec_rasterize(&lines, (enum vec_fill_rule)rule, 0.0f, 0.0f, NULL, &cov);
		CHECK_LONG(error, 0);
		CHECK(is_empty(&cov));

		/* A segment and its way back: the crossings cancel, nothing is inside. */
		vec_lines_reset(&lines);
		add_contour(&lines, line_there_and_back, 2);
		error = vec_rasterize(&lines, (enum vec_fill_rule)rule, 0.0f, 0.0f, NULL, &cov);
		CHECK_LONG(error, 0);
		CHECK(is_empty(&cov));

		/* Segments that are all horizontal cross no line. */
		vec_lines_reset(&lines);
		add_contour(&lines, horizontal, 3);
		error = vec_rasterize(&lines, (enum vec_fill_rule)rule, 0.0f, 0.0f, NULL, &cov);
		CHECK_LONG(error, 0);
		CHECK(is_empty(&cov));
		vec_lines_reset(&lines);
		add_contour(&lines, whole_lines, 3);
		error = vec_rasterize(&lines, (enum vec_fill_rule)rule, 0.0f, 0.0f, NULL, &cov);
		CHECK_LONG(error, 0);
		CHECK(is_empty(&cov));

		/* Three equal vertices. */
		vec_lines_reset(&lines);
		add_contour(&lines, three_same, 3);
		error = vec_rasterize(&lines, (enum vec_fill_rule)rule, 0.0f, 0.0f, NULL, &cov);
		CHECK_LONG(error, 0);
		CHECK(is_empty(&cov));
	}

	vec_coverage_release(&cov);
	vec_lines_release(&lines);
}

/* The errors and the empty result they leave. */
static void
test_errors(void)
{
	struct vec_lines lines;
	struct vec_lines hand;
	struct vec_vertex vertices[4];
	struct vec_contour contours[2];
	struct vec_coverage cov;
	float nan_value;
	float infinity;
	int error;

	nan_value = nanf("");
	infinity = INFINITY;
	vec_lines_init(&lines);
	vec_coverage_init(&cov);
	add_rect(&lines, 2.0f, 2.0f, 30.0f, 30.0f);

	/* An unknown fill rule. */
	error = vec_rasterize(&lines, (enum vec_fill_rule)2, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	CHECK(is_empty(&cov));
	error = vec_rasterize(&lines, (enum vec_fill_rule)-1, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);

	/* Offsets that are not finite, each leaving a coverage that had pixels empty. */
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK(!is_empty(&cov));
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, nan_value, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	CHECK(is_empty(&cov));
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, nan_value, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, infinity, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, -infinity, NULL, &cov);
	CHECK_LONG(error, EINVAL);

	/* A vertex moved beyond the coordinate limit: by a huge offset, by an offset that just crosses it. */
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 1.0e30f, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, -16777300.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	CHECK(is_empty(&cov));

	/* A vertex exactly at the limit moved back inside is accepted, and a clip lets a far shape pass. */
	vec_lines_reset(&lines);
	add_rect(&lines, 16777216.0f, 0.0f, 16777216.0f, 4.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 1.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);

	/* Missing polylines. */
	error = vec_rasterize(NULL, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	CHECK(is_empty(&cov));

	/* Malformed polylines: a contour past the vertex array, an empty contour, a vertex that is not a number. */
	memset(&hand, 0, sizeof(hand));
	vertices[0].x = 0.0f;
	vertices[0].y = 0.0f;
	vertices[1].x = 10.0f;
	vertices[1].y = 0.0f;
	vertices[2].x = 10.0f;
	vertices[2].y = 10.0f;
	vertices[3].x = 0.0f;
	vertices[3].y = 10.0f;
	vertices[0].flags = 0;
	vertices[1].flags = 0;
	vertices[2].flags = 0;
	vertices[3].flags = 0;
	contours[0].first = 0;
	contours[0].count = 4;
	contours[0].closed = 1;
	hand.vertices = vertices;
	hand.vertex_count = 4;
	hand.contours = contours;
	hand.contour_count = 1;
	error = vec_rasterize(&hand, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK(!is_empty(&cov));
	contours[0].count = 5;
	error = vec_rasterize(&hand, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	CHECK(is_empty(&cov));
	contours[0].count = 0;
	error = vec_rasterize(&hand, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	contours[0].count = 3;
	contours[0].first = 2;
	error = vec_rasterize(&hand, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	contours[0].first = 0;
	vertices[2].x = nan_value;
	error = vec_rasterize(&hand, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	vertices[2].x = 1.0e30f;
	error = vec_rasterize(&hand, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, EINVAL);
	CHECK(is_empty(&cov));

	vec_coverage_release(&cov);
	vec_lines_release(&lines);
}

/* The limits: the side, the number of segments, the crossings, the partial tiles. */
static void
test_limits(void)
{
	struct vec_lines lines;
	struct vec_coverage cov;
	struct vec_irect clip;
	struct vec_vertex *vertices;
	struct vec_contour contours[2];
	struct vec_lines hand;
	clock_t start;
	double seconds;
	size_t i;
	int error;
	int row;
	int column;
	int count;
	int variant;
	size_t vertex_count;

	vec_lines_init(&lines);
	vec_coverage_init(&cov);

	/* The widest and the tallest area allowed, and one more. */
	add_rect(&lines, 0.0f, 0.0f, (float)VEC_RASTER_SIDE_MAX, 1.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(cov.tile_count, VEC_RASTER_SIDE_MAX / VEC_TILE_SIZE);
	CHECK_LONG(vec_coverage_at(&cov, VEC_RASTER_SIDE_MAX - 1, 0), 255);
	CHECK_LONG(vec_coverage_at(&cov, VEC_RASTER_SIDE_MAX - 1, 1), 0);
	check_structure(&cov);
	vec_lines_reset(&lines);
	add_rect(&lines, 0.0f, 0.0f, 1.0f, (float)VEC_RASTER_SIDE_MAX);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(cov.tile_count, VEC_RASTER_SIDE_MAX / VEC_TILE_SIZE);
	CHECK_LONG(vec_coverage_at(&cov, 0, VEC_RASTER_SIDE_MAX - 1), 255);
	CHECK_LONG(vec_coverage_at(&cov, 0, VEC_RASTER_SIDE_MAX), 0);
	check_structure(&cov);
	vec_lines_reset(&lines);
	add_rect(&lines, 0.0f, 0.0f, (float)VEC_RASTER_SIDE_MAX + 1.0f, 1.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, ENOBUFS);
	CHECK(is_empty(&cov));
	vec_lines_reset(&lines);
	add_rect(&lines, 0.0f, 0.0f, 1.0f, (float)VEC_RASTER_SIDE_MAX + 1.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, ENOBUFS);
	CHECK(is_empty(&cov));

	/* A shape too wide by a fraction of a pixel on each end (the area is rounded outward). */
	vec_lines_reset(&lines);
	add_rect(&lines, 0.5f, 0.0f, (float)VEC_RASTER_SIDE_MAX + 0.5f, 1.0f);
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	CHECK_LONG(error, ENOBUFS);

	/* The same shape with a clip that keeps the area small is accepted. */
	clip.left = 0;
	clip.top = 0;
	clip.right = 100;
	clip.bottom = 100;
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, &clip, &cov);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&cov, 50, 0), 255);
	CHECK_LONG(vec_coverage_at(&cov, 0, 0), 128);
	check_structure(&cov);

	/* More segments than one call takes: two contours over one array of vertices. */
	vertex_count = VEC_RASTER_SEGMENTS_MAX / 2 + 1;
	vertices = calloc(vertex_count, sizeof(*vertices));
	CHECK(vertices != NULL);
	if (vertices != NULL) {
		for (i = 0; i < vertex_count; i++) {
			vertices[i].x = 5.0f;
			vertices[i].y = 5.0f;
			vertices[i].flags = 0;
		}
		memset(&hand, 0, sizeof(hand));
		contours[0].first = 0;
		contours[0].count = vertex_count;
		contours[0].closed = 1;
		contours[1].first = 0;
		contours[1].count = vertex_count;
		contours[1].closed = 1;
		hand.vertices = vertices;
		hand.vertex_count = vertex_count;
		hand.contours = contours;
		hand.contour_count = 2;
		error = vec_rasterize(&hand, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
		CHECK_LONG(error, ENOBUFS);
		CHECK(is_empty(&cov));

		/* Exactly the number of segments allowed passes (all of them are one point, so there is nothing to draw). */
		contours[1].first = 0;
		contours[1].count = VEC_RASTER_SEGMENTS_MAX - vertex_count;
		error = vec_rasterize(&hand, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
		CHECK_LONG(error, 0);
		CHECK(is_empty(&cov));
		free(vertices);
	}

	/*
	 * More crossings than one call computes: 400 segments each running
	 * the whole height of the tallest area (262128 lines) are 104.8
	 * million crossings, more than 2^26.  It has to be refused before
	 * the scan, so quickly.
	 */
	vec_lines_reset(&lines);
	error = vec_lines_begin(&lines, 0.0f, 0.0f, 0);
	CHECK_LONG(error, 0);
	for (i = 1; i < 400; i++) {
		if (i % 2 == 1)
			error = vec_lines_add(&lines, 16.0f, 16383.0f, 0);
		else
			error = vec_lines_add(&lines, 0.0f, 0.0f, 0);
		CHECK_LONG(error, 0);
	}
	vec_lines_close(&lines);
	start = clock();
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
	seconds = (double)(clock() - start) / (double)CLOCKS_PER_SEC;
	CHECK_LONG(error, ENOBUFS);
	CHECK(is_empty(&cov));
	CHECK(seconds < 2.0);

	/* The same shape through a small clip makes few crossings and passes. */
	clip.left = 0;
	clip.top = 0;
	clip.right = 16;
	clip.bottom = 16;
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, &clip, &cov);
	CHECK_LONG(error, 0);
	check_structure(&cov);

	/*
	 * The partial tiles: 256 x 256 squares, one per tile, are exactly the
	 * most; one more square (65537 tiles) and one more column and row
	 * (66049 tiles) are too many.
	 */
	for (variant = 0; variant < 3; variant++) {
		count = variant == 2 ? 257 : 256;
		vec_lines_reset(&lines);
		for (row = 0; row < count; row++) {
			for (column = 0; column < count; column++) {
				add_square(&lines, column * VEC_TILE_SIZE + 2, row * VEC_TILE_SIZE + 2);
			}
		}

		/* The middle variant has one more square, in the next tile column of the first row. */
		if (variant == 1)
			add_square(&lines, 256 * VEC_TILE_SIZE + 2, 2);
		error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &cov);
		if (variant == 0) {
			CHECK_LONG(error, 0);
			CHECK_LONG(cov.tile_count, VEC_RASTER_TILES_MAX);
			CHECK_LONG(vec_coverage_at(&cov, 3, 3), 255);
			CHECK_LONG(vec_coverage_at(&cov, 255 * 16 + 3, 255 * 16 + 5), 255);
			CHECK_LONG(vec_coverage_at(&cov, 255 * 16 + 8, 255 * 16 + 5), 0);
			check_structure(&cov);
		} else {
			CHECK_LONG(error, ENOBUFS);
			CHECK(is_empty(&cov));
		}
	}

	vec_coverage_release(&cov);
	vec_lines_release(&lines);
}

/* A coverage built by hand: partial tiles and runs given directly. */
static void
make_hand_coverage(
	struct vec_coverage *cov,
	size_t tile_count,
	size_t run_count)
{
	vec_coverage_init(cov);
	cov->tiles = calloc(tile_count + 1, sizeof(*cov->tiles));
	cov->runs = calloc(run_count + 1, sizeof(*cov->runs));
	cov->tile_capacity = tile_count + 1;
	cov->run_capacity = run_count + 1;
	cov->tile_count = tile_count;
	cov->run_count = run_count;
}

/* The product and the union against the per-pixel definitions, with runs and partial tiles of both. */
static void
test_combine(void)
{
	struct vec_lines lines_a;
	struct vec_lines lines_b;
	struct vec_coverage a;
	struct vec_coverage b;
	struct vec_coverage product;
	struct vec_coverage uni;
	struct vec_coverage empty;
	struct vec_coverage hand;
	int error;
	int x;
	int y;
	int bad_product;
	int bad_union;
	unsigned va;
	unsigned vb;
	unsigned expected_product;
	unsigned expected_union;
	size_t i;

	vec_lines_init(&lines_a);
	vec_lines_init(&lines_b);
	vec_coverage_init(&a);
	vec_coverage_init(&b);
	vec_coverage_init(&product);
	vec_coverage_init(&uni);
	vec_coverage_init(&empty);

	/* Two overlapping fractional rectangles and a triangle, across tile edges. */
	add_rect(&lines_a, 3.5f, 3.25f, 70.75f, 50.5f);
	add_rect(&lines_a, 80.0f, 10.0f, 120.0f, 20.0f);
	add_contour(&lines_b, (const float[]){ 10.0f, 60.0f, 100.0f, 6.0f, 130.0f, 70.0f }, 3);
	error = vec_rasterize(&lines_a, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &a);
	CHECK_LONG(error, 0);
	error = vec_rasterize(&lines_b, VEC_FILL_NONZERO, 0.25f, 0.0f, NULL, &b);
	CHECK_LONG(error, 0);
	CHECK(a.run_count > 0 && a.tile_count > 0);
	CHECK(b.run_count > 0 && b.tile_count > 0);
	error = vec_coverage_intersect(&product, &a, &b);
	CHECK_LONG(error, 0);
	error = vec_coverage_unite(&uni, &a, &b);
	CHECK_LONG(error, 0);
	check_structure(&product);
	check_structure(&uni);
	bad_product = 0;
	bad_union = 0;
	for (y = -3; y < 100; y++) {
		for (x = -3; x < 140; x++) {
			va = vec_coverage_at(&a, x, y);
			vb = vec_coverage_at(&b, x, y);
			expected_product = (va * vb + 127U) / 255U;
			expected_union = va > vb ? va : vb;
			if (vec_coverage_at(&product, x, y) != expected_product)
				bad_product++;
			if (vec_coverage_at(&uni, x, y) != expected_union)
				bad_union++;
		}
	}
	CHECK_LONG(bad_product, 0);
	CHECK_LONG(bad_union, 0);

	/* The operands in the other order give the same. */
	error = vec_coverage_intersect(&uni, &b, &a);
	CHECK_LONG(error, 0);
	bad_product = 0;
	for (y = -3; y < 100; y++) {
		for (x = -3; x < 140; x++) {
			if (vec_coverage_at(&uni, x, y) != vec_coverage_at(&product, x, y))
				bad_product++;
		}
	}
	CHECK_LONG(bad_product, 0);

	/* 255 is the identity of the product and 0 absorbs. */
	vec_lines_reset(&lines_b);
	add_rect(&lines_b, -40.0f, -40.0f, 300.0f, 300.0f);
	error = vec_rasterize(&lines_b, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &b);
	CHECK_LONG(error, 0);
	error = vec_coverage_intersect(&product, &a, &b);
	CHECK_LONG(error, 0);
	bad_product = 0;
	for (y = -3; y < 100; y++) {
		for (x = -3; x < 140; x++) {
			if (vec_coverage_at(&product, x, y) != vec_coverage_at(&a, x, y))
				bad_product++;
		}
	}
	CHECK_LONG(bad_product, 0);
	CHECK_LONG(product.tile_count, a.tile_count);
	CHECK_LONG(product.run_count, a.run_count);
	check_structure(&product);
	error = vec_coverage_intersect(&product, &a, &empty);
	CHECK_LONG(error, 0);
	CHECK(is_empty(&product));
	error = vec_coverage_intersect(&product, &empty, &a);
	CHECK_LONG(error, 0);
	CHECK(is_empty(&product));
	error = vec_coverage_intersect(&product, &empty, &empty);
	CHECK_LONG(error, 0);
	CHECK(is_empty(&product));

	/* The union with the empty one is the other, and with a covering one is the covering one. */
	error = vec_coverage_unite(&uni, &a, &empty);
	CHECK_LONG(error, 0);
	CHECK_LONG(uni.tile_count, a.tile_count);
	CHECK_LONG(uni.run_count, a.run_count);
	check_structure(&uni);
	error = vec_coverage_unite(&uni, &empty, &a);
	CHECK_LONG(error, 0);
	CHECK_LONG(uni.tile_count, a.tile_count);
	CHECK_LONG(uni.run_count, a.run_count);
	error = vec_coverage_unite(&uni, &empty, &empty);
	CHECK_LONG(error, 0);
	CHECK(is_empty(&uni));
	error = vec_coverage_unite(&uni, &a, &b);
	CHECK_LONG(error, 0);
	CHECK_LONG(uni.tile_count, b.tile_count);
	CHECK_LONG(uni.run_count, b.run_count);
	check_structure(&uni);

	/* The output is the input: refused, and untouched. */
	error = vec_coverage_intersect(&a, &a, &b);
	CHECK_LONG(error, EINVAL);
	CHECK(a.tile_count > 0);
	error = vec_coverage_unite(&b, &a, &b);
	CHECK_LONG(error, EINVAL);
	CHECK(b.run_count > 0);

	/* The product of rounding: 100 * 100 -> (10000 + 127) / 255 = 39; 255 * 3 -> 3; 1 * 1 -> (1 + 127) / 255 = 0; 128 * 128 -> 64.  A hand-built tile of every kind. */
	make_hand_coverage(&hand, 1, 0);
	for (i = 0; i < VEC_TILE_PIXELS; i++)
		hand.tiles[0].alpha[i] = 100;
	hand.tiles[0].alpha[0] = 255;
	hand.tiles[0].alpha[1] = 1;
	hand.tiles[0].alpha[2] = 128;
	hand.tiles[0].tx = 2;
	hand.tiles[0].ty = 1;
	vec_coverage_release(&a);
	make_hand_coverage(&a, 1, 0);
	for (i = 0; i < VEC_TILE_PIXELS; i++)
		a.tiles[0].alpha[i] = 100;
	a.tiles[0].alpha[0] = 3;
	a.tiles[0].alpha[1] = 1;
	a.tiles[0].alpha[2] = 128;
	a.tiles[0].tx = 2;
	a.tiles[0].ty = 1;
	error = vec_coverage_intersect(&product, &hand, &a);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&product, 2 * 16 + 0, 16), 3);
	CHECK_LONG(vec_coverage_at(&product, 2 * 16 + 1, 16), 0);
	CHECK_LONG(vec_coverage_at(&product, 2 * 16 + 2, 16), 64);
	CHECK_LONG(vec_coverage_at(&product, 2 * 16 + 3, 16), 39);
	CHECK_LONG(vec_coverage_at(&product, 2 * 16 + 3, 17), 39);
	CHECK_LONG(product.tile_count, 1);
	error = vec_coverage_unite(&uni, &hand, &a);
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_coverage_at(&uni, 2 * 16 + 0, 16), 255);
	CHECK_LONG(vec_coverage_at(&uni, 2 * 16 + 1, 16), 1);
	CHECK_LONG(vec_coverage_at(&uni, 2 * 16 + 2, 16), 128);
	CHECK_LONG(vec_coverage_at(&uni, 2 * 16 + 3, 16), 100);

	/* A product that comes out all 0 is dropped, and one that comes out all 255 becomes a run. */
	vec_coverage_release(&hand);
	make_hand_coverage(&hand, 1, 0);
	for (i = 0; i < VEC_TILE_PIXELS; i++)
		hand.tiles[0].alpha[i] = 255;
	hand.tiles[0].tx = 0;
	hand.tiles[0].ty = 0;
	vec_coverage_release(&b);
	make_hand_coverage(&b, 0, 1);
	b.runs[0].tx = -1;
	b.runs[0].ty = 0;
	b.runs[0].count = 3;
	error = vec_coverage_intersect(&product, &hand, &b);
	CHECK_LONG(error, 0);
	CHECK_LONG(product.tile_count, 0);
	CHECK_LONG(product.run_count, 1);
	CHECK_LONG(vec_coverage_at(&product, 5, 5), 255);
	CHECK_LONG(vec_coverage_at(&product, 17, 5), 0);
	check_structure(&product);
	vec_coverage_release(&a);
	make_hand_coverage(&a, 1, 0);
	for (i = 0; i < VEC_TILE_PIXELS; i++)
		a.tiles[0].alpha[i] = (i % 2) ? 255 : 0;
	a.tiles[0].tx = 0;
	a.tiles[0].ty = 0;
	vec_coverage_release(&hand);
	make_hand_coverage(&hand, 1, 0);
	for (i = 0; i < VEC_TILE_PIXELS; i++)
		hand.tiles[0].alpha[i] = (i % 2) ? 0 : 255;
	hand.tiles[0].tx = 0;
	hand.tiles[0].ty = 0;
	error = vec_coverage_intersect(&product, &a, &hand);
	CHECK_LONG(error, 0);
	CHECK(is_empty(&product));
	error = vec_coverage_unite(&uni, &a, &hand);
	CHECK_LONG(error, 0);
	CHECK_LONG(uni.tile_count, 0);
	CHECK_LONG(uni.run_count, 1);
	check_structure(&uni);

	vec_coverage_release(&hand);
	vec_coverage_release(&empty);
	vec_coverage_release(&uni);
	vec_coverage_release(&product);
	vec_coverage_release(&b);
	vec_coverage_release(&a);
	vec_lines_release(&lines_b);
	vec_lines_release(&lines_a);
}

/* Random polygons, fixed seeds, against the reference, under both rules; the sanitizers watch the memory. */
static void
test_random(void)
{
	struct vec_lines lines;
	struct vec_coverage cov;
	float points[16];
	uint32_t state;
	int rule;
	int shape;
	int n;
	int i;
	int worst;
	int shape_worst;
	float ox;
	float oy;
	static const float phases[4] = { 0.0f, 0.25f, 0.5f, 0.75f };

	vec_lines_init(&lines);
	vec_coverage_init(&cov);
	for (rule = 0; rule < 2; rule++) {
		state = rule == 0 ? 0x1234abcdU : 0x9e3779b9U;
		worst = 0;
		for (shape = 0; shape < RANDOM_SHAPES; shape++) {
			n = 3 + (int)(check_random(&state) % 6U);
			for (i = 0; i < n; i++) {
				points[2 * i] = check_random_float(&state, 0.0f, 8.0f);
				points[2 * i + 1] = check_random_float(&state, 0.0f, 8.0f);
			}
			/* The offset puts the shape across the tile edge at 16, at a few fractional phases and at random ones. */
			if (shape % 3 == 0) {
				ox = 12.0f + phases[check_random(&state) % 4U];
				oy = 12.0f + phases[check_random(&state) % 4U];
			} else {
				ox = check_random_float(&state, 12.0f, 16.0f);
				oy = check_random_float(&state, 12.0f, 16.0f);
			}
			vec_lines_reset(&lines);
			add_contour(&lines, points, (size_t)n);
			shape_worst = compare_window(&lines, (enum vec_fill_rule)rule, ox, oy, NULL, 12, 12, 13, 13, &cov);
			CHECK(shape_worst >= 0);
			if (shape_worst > worst)
				worst = shape_worst;
		}
		printf("random polygons, rule %d: worst difference %d\n", rule, worst);
	}
	vec_coverage_release(&cov);
	vec_lines_release(&lines);
}

/* Many long segments all crossing each other still sort in bounded time and give the same coverage in any order. */
static void
test_crossing_fan(void)
{
	struct vec_lines lines;
	struct vec_lines reversed;
	struct vec_coverage nonzero;
	struct vec_coverage evenodd;
	struct vec_coverage again;
	struct vec_coverage cov;
	float points[6];
	float x;
	int error;
	int i;
	int px;
	int py;
	int worst;
	int difference;
	int evenodd_over;
	unsigned a;
	unsigned b;

	vec_lines_init(&lines);
	vec_lines_init(&reversed);
	vec_coverage_init(&nonzero);
	vec_coverage_init(&evenodd);
	vec_coverage_init(&again);
	vec_coverage_init(&cov);

	/* A few slim triangles whose long edges cross each other, against the reference. */
	for (i = 0; i < 6; i++) {
		x = (float)i * 7.5f;
		points[0] = x;
		points[1] = 0.0f;
		points[2] = 59.0f - x;
		points[3] = 63.0f;
		points[4] = 60.5f - x;
		points[5] = 63.0f;
		add_contour(&lines, points, 3);
	}
	CHECK(compare_window(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);
	CHECK(compare_window(&lines, VEC_FILL_EVENODD, 0.0f, 0.0f, NULL, 0, 0, WINDOW, WINDOW, &cov) >= 0);

	/*
	 * 2000 slim triangles whose edges all meet at one height: the order of
	 * their crossings reverses on one sample line, far over the insertion
	 * sort's budget, so the heap sort finishes it.  The contours in the
	 * opposite order give the same pixels (within a step of rounding), and
	 * the evenodd coverage never exceeds the nonzero one.
	 */
	vec_lines_reset(&lines);
	for (i = 0; i < 2000; i++) {
		x = (float)i * 0.025f;
		points[0] = x;
		points[1] = 0.0f;
		points[2] = 63.0f - x;
		points[3] = 63.0f;
		points[4] = 63.5f - x;
		points[5] = 63.0f;
		add_contour(&lines, points, 3);
	}
	for (i = 1999; i >= 0; i--) {
		x = (float)i * 0.025f;
		points[0] = x;
		points[1] = 0.0f;
		points[2] = 63.0f - x;
		points[3] = 63.0f;
		points[4] = 63.5f - x;
		points[5] = 63.0f;
		add_contour(&reversed, points, 3);
	}
	error = vec_rasterize(&lines, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &nonzero);
	CHECK_LONG(error, 0);
	check_structure(&nonzero);
	error = vec_rasterize(&lines, VEC_FILL_EVENODD, 0.0f, 0.0f, NULL, &evenodd);
	CHECK_LONG(error, 0);
	check_structure(&evenodd);
	error = vec_rasterize(&reversed, VEC_FILL_NONZERO, 0.0f, 0.0f, NULL, &again);
	CHECK_LONG(error, 0);
	worst = 0;
	evenodd_over = 0;
	for (py = -1; py < 65; py++) {
		for (px = -1; px < 70; px++) {
			a = vec_coverage_at(&nonzero, px, py);
			b = vec_coverage_at(&again, px, py);
			difference = (int)a - (int)b;
			if (difference < 0)
				difference = -difference;
			if (difference > worst)
				worst = difference;
			if (vec_coverage_at(&evenodd, px, py) > a)
				evenodd_over++;
		}
	}
	CHECK(worst <= 1);
	CHECK_LONG(evenodd_over, 0);
	CHECK(nonzero.bounds.right > nonzero.bounds.left);

	vec_coverage_release(&cov);
	vec_coverage_release(&again);
	vec_coverage_release(&evenodd);
	vec_coverage_release(&nonzero);
	vec_lines_release(&reversed);
	vec_lines_release(&lines);
}

int
main(void)
{
	test_rect_by_hand();
	test_line_ties();
	test_tile_classes();
	test_against_reference();
	test_fill_rules();
	test_clip();
	test_lookup();
	test_life();
	test_degenerate();
	test_errors();
	test_limits();
	test_combine();
	test_crossing_fan();
	test_random();
	return check_finish("raster");
}
