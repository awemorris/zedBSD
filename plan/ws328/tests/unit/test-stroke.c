/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of vec-stroke: the length of a contour, the shapes of
 * butt, square and round caps and of miter, bevel and round joins (areas
 * of their union from hand calculation, measured with the reference
 * coverage of raster-reference.h, which shares no code with the module),
 * the miter limit, the number of steps of an arc, the device map
 * (mirrored, anisotropic), dashes by the algorithm of SVG 2 section
 * 13.5.7 worked by hand (offsets, odd lists, scale, closed contours, the
 * limit of dashes), zero-length subpaths, joins inside curves, the
 * refusal of out-of-range styles, the limits and the rollback on
 * failure, and a random run under the sanitizers.  Every expected value
 * is a hand calculation or follows from the formulas of the
 * specifications; no other implementation's output is used.
 */

#include "check.h"
#include "raster-reference.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include "vector/vec-stroke.h"

/* The number of random polylines of the random run. */
#define RANDOM_RUNS	4000

/* The pi of the expectations. */
#define TEST_PI		3.14159265358979323846

/* The corner flag on every vertex: a polyline of the original path's own segments. */
#define ALL_CORNERS	VEC_VERTEX_CORNER

/* Makes one contour from n points (x, y pairs) with the given flags on every vertex. */
static void
make_lines(
	struct vec_lines *lines,
	const float *xy,
	size_t n,
	int closed,
	unsigned flags)
{
	size_t i;
	int error;

	vec_lines_init(lines);
	for (i = 0; i < n; i++) {
		if (i == 0)
			error = vec_lines_begin(lines, xy[0], xy[1], flags);
		else
			error = vec_lines_add(lines, xy[2 * i], xy[2 * i + 1], flags);
		CHECK_LONG(error, 0);
	}
	if (closed)
		vec_lines_close(lines);
}

/* Fills a style: no dashes, miter limit 4, tolerance 0.01. */
static void
make_style(
	struct vec_stroke_style *style,
	float width,
	enum vec_cap cap,
	enum vec_join join)
{
	memset(style, 0, sizeof(*style));
	style->width = width;
	style->cap = cap;
	style->join = join;
	style->miter_limit = 4.0f;
	style->dash_scale = 1.0f;
	style->tolerance = 0.01f;
}

/* Gives the signed area (half the shoelace sum) of contour c. */
static double
contour_area(
	const struct vec_lines *lines,
	size_t c)
{
	const struct vec_contour *contour;
	const struct vec_vertex *a;
	const struct vec_vertex *b;
	double sum;
	size_t i;

	contour = &lines->contours[c];
	sum = 0.0;
	for (i = 0; i < contour->count; i++) {
		a = &lines->vertices[contour->first + i];
		b = &lines->vertices[contour->first + (i + 1) % contour->count];
		sum += (double)a->x * (double)b->y - (double)b->x * (double)a->y;
	}
	return sum * 0.5;
}

/* Checks that every contour is closed, has at least three vertices, a positive area and no flags. */
static void
check_pieces(
	const struct vec_lines *lines)
{
	size_t c;
	size_t i;
	const struct vec_contour *contour;

	CHECK_LONG(vec_lines_check(lines), 0);
	for (c = 0; c < lines->contour_count; c++) {
		contour = &lines->contours[c];
		CHECK(contour->closed != 0);
		CHECK(contour->count >= 3);
		CHECK(contour_area(lines, c) > 0.0);
		for (i = 0; i < contour->count; i++)
			CHECK_LONG(lines->vertices[contour->first + i].flags, 0);
	}
}

/* Gives the area of the nonzero fill of lines over a pixel rectangle, by the reference coverage; -1 when the reference refuses. */
static double
fill_area(
	const struct vec_lines *lines,
	int left,
	int top,
	int width,
	int height)
{
	unsigned char *alpha;
	double sum;
	int i;
	int refused;

	alpha = malloc((size_t)width * (size_t)height);
	if (alpha == NULL)
		return -1.0;
	refused = reference_coverage(lines, VEC_FILL_NONZERO, 0.0, 0.0, left, top, width, height, alpha);
	if (refused != 0) {
		free(alpha);
		return -1.0;
	}
	sum = 0.0;
	for (i = 0; i < width * height; i++)
		sum += (double)alpha[i] / 255.0;
	free(alpha);
	return sum;
}

/* Gives the extent of a contour in x. */
static void
contour_x_extent(
	const struct vec_lines *lines,
	size_t c,
	double *low,
	double *high)
{
	const struct vec_contour *contour;
	size_t i;
	double x;

	contour = &lines->contours[c];
	*low = 1e30;
	*high = -1e30;
	for (i = 0; i < contour->count; i++) {
		x = lines->vertices[contour->first + i].x;
		if (x < *low)
			*low = x;
		if (x > *high)
			*high = x;
	}
}

/* Tells whether any vertex of lines is within tolerance of a point. */
static int
has_vertex(
	const struct vec_lines *lines,
	double x,
	double y,
	double tolerance)
{
	size_t i;

	for (i = 0; i < lines->vertex_count; i++) {
		if (fabs((double)lines->vertices[i].x - x) <= tolerance &&
		    fabs((double)lines->vertices[i].y - y) <= tolerance)
			return 1;
	}
	return 0;
}

/* Gives the winding number of the contours of lines around a point. */
static int
winding_at(
	const struct vec_lines *lines,
	double px,
	double py)
{
	const struct vec_contour *contour;
	const struct vec_vertex *a;
	const struct vec_vertex *b;
	double cross;
	size_t c;
	size_t i;
	int winding;

	winding = 0;
	for (c = 0; c < lines->contour_count; c++) {
		contour = &lines->contours[c];
		for (i = 0; i < contour->count; i++) {
			a = &lines->vertices[contour->first + i];
			b = &lines->vertices[contour->first + (i + 1) % contour->count];
			cross = ((double)b->x - a->x) * (py - a->y) - (px - a->x) * ((double)b->y - a->y);
			if (a->y <= py) {
				if (b->y > py && cross > 0.0)
					winding++;
			} else if (b->y <= py && cross < 0.0) {
				winding--;
			}
		}
	}
	return winding;
}

/* The number of steps of an arc by the formula of vec-stroke.h, for a pen radius in device units. */
static int
expected_steps(
	double angle,
	double tolerance,
	double radius)
{
	double steps;

	if (tolerance >= radius)
		steps = angle / (TEST_PI / 2.0);
	else
		steps = angle / (2.0 * acos(1.0 - tolerance / radius));
	steps = ceil(steps);
	if (steps < 1.0)
		steps = 1.0;
	if (steps > 256.0)
		steps = 256.0;
	return (int)steps;
}

/* The area of a polygon of n equal steps over an angle on a circle of a radius, with the center as a vertex (a sector). */
static double
sector_area(
	double radius,
	double angle,
	int n)
{
	return 0.5 * radius * radius * (double)n * sin(angle / (double)n);
}

/* The lengths of contours: open, closed, out of range, short. */
static void
test_length(void)
{
	static const float open_points[] = { 0.0f, 0.0f, 3.0f, 4.0f, 3.0f, 10.0f };
	static const float one_point[] = { 5.0f, 5.0f };
	struct vec_lines lines;

	make_lines(&lines, open_points, 3, 0, ALL_CORNERS);
	CHECK_NEAR(vec_contour_length(&lines, 0), 11.0, 1e-5);
	CHECK_LONG(vec_contour_length(&lines, 1), 0);
	CHECK_LONG(vec_contour_length(&lines, 100), 0);
	vec_lines_close(&lines);
	/* The closing segment from (3, 10) to (0, 0): sqrt(9 + 100). */
	CHECK_NEAR(vec_contour_length(&lines, 0), 11.0 + sqrt(109.0), 1e-5);
	vec_lines_release(&lines);

	make_lines(&lines, one_point, 1, 0, ALL_CORNERS);
	CHECK_LONG(vec_contour_length(&lines, 0), 0);
	vec_lines_close(&lines);
	CHECK_LONG(vec_contour_length(&lines, 0), 0);
	vec_lines_release(&lines);

	/* Empty lines have no contour 0. */
	vec_lines_init(&lines);
	CHECK_LONG(vec_contour_length(&lines, 0), 0);
}

/* The caps: areas of the union for butt, square and round, the positions of the round cap's points, and the direction of a slanted cap. */
static void
test_caps(void)
{
	static const float line[] = { 4.0f, 10.0f, 24.0f, 10.0f };
	static const float slanted[] = { 0.0f, 0.0f, 30.0f, 40.0f };
	struct vec_stroke_style style;
	struct vec_lines center;
	struct vec_lines out;
	double area;
	double distance;
	size_t i;
	size_t c;
	int error;

	make_lines(&center, line, 2, 0, ALL_CORNERS);

	/* Butt: the rectangle L * w = 20 * 4. */
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_MITER);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 1);
	check_pieces(&out);
	CHECK_NEAR(fill_area(&out, 0, 0, 30, 20), 80.0, 0.05);
	vec_lines_release(&out);

	/* Square: (L + w) * w. */
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_SQUARE, VEC_JOIN_MITER);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 3);
	check_pieces(&out);
	CHECK_NEAR(fill_area(&out, 0, 0, 30, 20), 96.0, 0.05);
	vec_lines_release(&out);

	/* Round: L * w + pi * (w / 2)^2, less the inscribed polygon's shortfall. */
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_ROUND, VEC_JOIN_MITER);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 3);
	check_pieces(&out);
	area = fill_area(&out, 0, 0, 30, 20);
	CHECK(area <= 80.0 + 4.0 * TEST_PI + 0.05);
	CHECK(area >= 80.0 + 4.0 * TEST_PI - 0.15);
	/* The start cap (contour 1) is a half disc about (4, 10), the end cap (contour 2) about (24, 10). */
	for (c = 1; c <= 2; c++) {
		for (i = 0; i < out.contours[c].count; i++) {
			distance = hypot((double)out.vertices[out.contours[c].first + i].x - (c == 1 ? 4.0 : 24.0),
			    (double)out.vertices[out.contours[c].first + i].y - 10.0);
			CHECK_NEAR(distance, 2.0, 1e-4);
		}
	}
	/* The start cap lies on the left of the start, the end cap on the right of the end. */
	for (i = 0; i < out.contours[1].count; i++)
		CHECK(out.vertices[out.contours[1].first + i].x <= 4.0f + 1e-4f);
	for (i = 0; i < out.contours[2].count; i++)
		CHECK(out.vertices[out.contours[2].first + i].x >= 24.0f - 1e-4f);
	vec_lines_release(&out);
	vec_lines_release(&center);

	/* A slanted line from (0, 0) to (30, 40), direction (0.6, 0.8), normal (-0.8, 0.6), width 2, square caps. */
	make_lines(&center, slanted, 2, 0, ALL_CORNERS);
	vec_lines_init(&out);
	make_style(&style, 2.0f, VEC_CAP_SQUARE, VEC_JOIN_MITER);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	/* End corners: (30, 40) + 1 * (d +- n) = (29.8, 41.4) and (31.4, 40.2). */
	CHECK(has_vertex(&out, 29.8, 41.4, 1e-4));
	CHECK(has_vertex(&out, 31.4, 40.2, 1e-4));
	/* Start corners: (0, 0) + 1 * (-d +- n) = (-1.4, -0.2) and (0.2, -1.4). */
	CHECK(has_vertex(&out, -1.4, -0.2, 1e-4));
	CHECK(has_vertex(&out, 0.2, -1.4, 1e-4));
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* The joins at a right-angle turn, both ways: areas of the union. */
static void
test_joins(void)
{
	static const float right_turn[] = { 2.0f, 10.0f, 12.0f, 10.0f, 12.0f, 20.0f };
	static const float left_turn[] = { 2.0f, 20.0f, 12.0f, 20.0f, 12.0f, 10.0f };
	static const float reversal[] = { 2.0f, 10.0f, 12.0f, 10.0f, 2.0f, 10.0f };
	static const float straight[] = { 2.0f, 10.0f, 7.0f, 10.0f, 12.0f, 10.0f };
	const float *shapes[2];
	struct vec_stroke_style style;
	struct vec_lines center;
	struct vec_lines out;
	double area;
	double expected;
	int s;
	int steps;
	int error;

	shapes[0] = right_turn;
	shapes[1] = left_turn;
	for (s = 0; s < 2; s++) {
		make_lines(&center, shapes[s], 3, 0, ALL_CORNERS);

		/* Two rectangles of 10 * 4 overlapping in 2 * 2: 76; the miter adds the 2 * 2 outer corner. */
		vec_lines_init(&out);
		make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_MITER);
		error = vec_stroke(&center, &style, NULL, &out);
		CHECK_LONG(error, 0);
		CHECK_LONG(out.contour_count, 3);
		check_pieces(&out);
		CHECK_NEAR(fill_area(&out, 0, 0, 20, 30), 80.0, 0.05);
		vec_lines_release(&out);

		/* The bevel adds the triangle of the vertex and the two outer corners: 2 * 2 / 2. */
		vec_lines_init(&out);
		make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_BEVEL);
		error = vec_stroke(&center, &style, NULL, &out);
		CHECK_LONG(error, 0);
		CHECK_LONG(out.contour_count, 3);
		check_pieces(&out);
		CHECK_NEAR(fill_area(&out, 0, 0, 20, 30), 78.0, 0.05);
		vec_lines_release(&out);

		/* The round join adds the quarter sector of radius 2, as an inscribed polygon of M equal steps. */
		vec_lines_init(&out);
		make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_ROUND);
		error = vec_stroke(&center, &style, NULL, &out);
		CHECK_LONG(error, 0);
		CHECK_LONG(out.contour_count, 3);
		check_pieces(&out);
		steps = expected_steps(TEST_PI / 2.0, 0.01, 2.0);
		CHECK_LONG(steps, 8);
		/* The sector contour is the center and the M + 1 arc points. */
		CHECK_LONG(out.contours[1].count, steps + 2);
		expected = 76.0 + sector_area(2.0, TEST_PI / 2.0, steps);
		CHECK_NEAR(fill_area(&out, 0, 0, 20, 30), expected, 0.05);
		vec_lines_release(&out);

		/* A miter limit above sqrt(2) keeps the miter, below makes it a bevel. */
		vec_lines_init(&out);
		make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_MITER);
		style.miter_limit = 1.42f;
		error = vec_stroke(&center, &style, NULL, &out);
		CHECK_LONG(error, 0);
		CHECK_NEAR(fill_area(&out, 0, 0, 20, 30), 80.0, 0.05);
		vec_lines_release(&out);
		vec_lines_init(&out);
		style.miter_limit = 1.41f;
		error = vec_stroke(&center, &style, NULL, &out);
		CHECK_LONG(error, 0);
		CHECK_NEAR(fill_area(&out, 0, 0, 20, 30), 78.0, 0.05);
		vec_lines_release(&out);

		vec_lines_release(&center);
	}

	/* A turn back on itself: a round join is a half disc beyond the end, the others add nothing. */
	make_lines(&center, reversal, 3, 0, ALL_CORNERS);
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_ROUND);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	steps = expected_steps(TEST_PI, 0.01, 2.0);
	area = fill_area(&out, 0, 0, 20, 30);
	expected = 40.0 + sector_area(2.0, TEST_PI, steps);
	CHECK_NEAR(area, expected, 0.05);
	vec_lines_release(&out);
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_MITER);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 2);
	check_pieces(&out);
	CHECK_NEAR(fill_area(&out, 0, 0, 20, 30), 40.0, 0.05);
	vec_lines_release(&out);
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_BEVEL);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 2);
	vec_lines_release(&out);
	vec_lines_release(&center);

	/* Collinear segments: two rectangles and no join piece. */
	make_lines(&center, straight, 3, 0, ALL_CORNERS);
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_ROUND);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 2);
	CHECK_NEAR(fill_area(&out, 0, 0, 20, 30), 40.0, 0.05);
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* The miter point of a sharp turn and the limit that cuts it. */
static void
test_miter_limit(void)
{
	/* A turn of 150 degrees: the interior angle theta is 30 degrees, the miter ratio 1 / sin(15 degrees) = 3.8637. */
	float points[6];
	struct vec_stroke_style style;
	struct vec_lines center;
	struct vec_lines out;
	double angle;
	double distance;
	double farthest;
	size_t i;
	int error;

	angle = 150.0 * TEST_PI / 180.0;
	points[0] = 0.0f;
	points[1] = 0.0f;
	points[2] = 20.0f;
	points[3] = 0.0f;
	points[4] = (float)(20.0 + 10.0 * cos(angle));
	points[5] = (float)(10.0 * sin(angle));
	make_lines(&center, points, 3, 0, ALL_CORNERS);

	/* With limit 4 the miter point is at (width / 2) / sin(theta / 2) = 2 * 3.8637 = 7.7274 from the vertex. */
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_MITER);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	farthest = 0.0;
	for (i = 0; i < out.vertex_count; i++) {
		distance = hypot((double)out.vertices[i].x - 20.0, (double)out.vertices[i].y);
		if (distance < 9.0 && distance > farthest)
			farthest = distance;
	}
	CHECK_NEAR(farthest, 2.0 / sin(15.0 * TEST_PI / 180.0), 1e-3);
	vec_lines_release(&out);

	/* With limit 3.8 (below 3.8637) it is a bevel: nothing nearer than 9 is farther than the pen radius 2. */
	vec_lines_init(&out);
	style.miter_limit = 3.8f;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	farthest = 0.0;
	for (i = 0; i < out.vertex_count; i++) {
		distance = hypot((double)out.vertices[i].x - 20.0, (double)out.vertices[i].y);
		if (distance < 9.0 && distance > farthest)
			farthest = distance;
	}
	CHECK_NEAR(farthest, 2.0, 1e-3);
	vec_lines_release(&out);

	/* Limit 3.9 (above 3.8637) keeps the miter. */
	vec_lines_init(&out);
	style.miter_limit = 3.9f;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	farthest = 0.0;
	for (i = 0; i < out.vertex_count; i++) {
		distance = hypot((double)out.vertices[i].x - 20.0, (double)out.vertices[i].y);
		if (distance < 9.0 && distance > farthest)
			farthest = distance;
	}
	CHECK_NEAR(farthest, 2.0 / sin(15.0 * TEST_PI / 180.0), 1e-3);
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* The number of steps of arcs: the formula, the quarter turn when the tolerance reaches the radius, the upper limit, and the effect of the device scale. */
static void
test_arc_steps(void)
{
	static const float right_turn[] = { 2.0f, 10.0f, 12.0f, 10.0f, 12.0f, 20.0f };
	static const float line[] = { 4.0f, 10.0f, 24.0f, 10.0f };
	struct vec_stroke_style style;
	struct vec_matrix matrix;
	struct vec_lines center;
	struct vec_lines out;
	int error;

	make_lines(&center, right_turn, 3, 0, ALL_CORNERS);

	/* Tolerance 0.01, radius 2, quarter turn: ceil((pi / 2) / (2 acos(0.995))) = 8 steps, 10 vertices. */
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_ROUND);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contours[1].count, expected_steps(TEST_PI / 2.0, 0.01, 2.0) + 2);
	vec_lines_release(&out);

	/* Under scale(2, 2) the radius is 4 device units: 12 steps. */
	vec_matrix_scale(&matrix, 2.0f, 2.0f);
	vec_lines_init(&out);
	error = vec_stroke(&center, &style, &matrix, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(expected_steps(TEST_PI / 2.0, 0.01, 4.0), 12);
	CHECK_LONG(out.contours[1].count, 14);
	vec_lines_release(&out);

	/* A tolerance at or above the radius: ceil(angle / (pi / 2)), one step for a quarter turn, a triangle. */
	vec_lines_init(&out);
	style.tolerance = 2.0f;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contours[1].count, 3);
	vec_lines_release(&out);
	vec_lines_release(&center);

	/* A round cap of a very wide line with a tiny tolerance is held to 256 steps: 257 vertices. */
	make_lines(&center, line, 2, 0, ALL_CORNERS);
	vec_lines_init(&out);
	make_style(&style, 200.0f, VEC_CAP_ROUND, VEC_JOIN_MITER);
	style.tolerance = 0.000001f;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 3);
	CHECK_LONG(out.contours[1].count, 257);
	CHECK_LONG(out.contours[2].count, 257);
	vec_lines_release(&out);

	/* A half disc under a tolerance equal to the radius: ceil(pi / (pi / 2)) = 2 steps, 3 vertices. */
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_ROUND, VEC_JOIN_MITER);
	style.tolerance = 2.0f;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contours[1].count, 3);
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* The device map: a mirror keeps every piece positive, a translation and an anisotropic scale map the pen. */
static void
test_device_map(void)
{
	static const float right_turn[] = { 2.0f, 10.0f, 12.0f, 10.0f, 12.0f, 20.0f };
	static const float line[] = { 4.0f, 10.0f, 24.0f, 10.0f };
	static const float vertical[] = { 10.0f, 2.0f, 10.0f, 12.0f };
	struct vec_stroke_style style;
	struct vec_matrix matrix;
	struct vec_matrix second;
	struct vec_lines center;
	struct vec_lines out;
	struct vec_lines plain;
	double low;
	double high;
	size_t i;
	int error;

	/* A mirror in x: every piece still has a positive area, the shape is the same one moved to negative x. */
	make_lines(&center, right_turn, 3, 0, ALL_CORNERS);
	vec_matrix_scale(&matrix, -1.0f, 1.0f);
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_ROUND, VEC_JOIN_ROUND);
	error = vec_stroke(&center, &style, &matrix, &out);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	vec_lines_init(&plain);
	error = vec_stroke(&center, &style, NULL, &plain);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, plain.contour_count);
	CHECK_LONG(out.vertex_count, plain.vertex_count);
	/* The mirror reverses each piece's order, so look for each mirrored vertex rather than the same index. */
	for (i = 0; i < plain.vertex_count; i++)
		CHECK(has_vertex(&out, -(double)plain.vertices[i].x, plain.vertices[i].y, 1e-5));
	vec_lines_release(&out);
	vec_lines_release(&plain);

	/* A mirror in y and a rotation by 180 degrees (a point reflection), also positive. */
	vec_matrix_scale(&matrix, 1.0f, -1.0f);
	vec_lines_init(&out);
	error = vec_stroke(&center, &style, &matrix, &out);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	vec_lines_release(&out);
	vec_matrix_rotate(&matrix, 180.0f);
	vec_lines_init(&out);
	error = vec_stroke(&center, &style, &matrix, &out);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	vec_lines_release(&out);
	vec_lines_release(&center);

	/* scale(2, 3) of a horizontal butt line of 20 * 4: 40 * 12 = 480 with the edges at whole pixels. */
	make_lines(&center, line, 2, 0, ALL_CORNERS);
	vec_matrix_scale(&matrix, 2.0f, 3.0f);
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_MITER);
	error = vec_stroke(&center, &style, &matrix, &out);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	CHECK_NEAR(fill_area(&out, 0, 0, 60, 40), 480.0, 0.05);
	vec_lines_release(&out);

	/* A translation after a scale: scale(2, 1) then translate(5, 7) maps (4, 10) to (13, 17). */
	vec_matrix_scale(&matrix, 2.0f, 1.0f);
	vec_matrix_translate(&second, 5.0f, 7.0f);
	vec_matrix_multiply(&matrix, &matrix, &second);
	vec_lines_init(&out);
	error = vec_stroke(&center, &style, &matrix, &out);
	CHECK_LONG(error, 0);
	contour_x_extent(&out, 0, &low, &high);
	CHECK_NEAR(low, 13.0, 1e-5);
	CHECK_NEAR(high, 53.0, 1e-5);
	vec_lines_release(&out);
	vec_lines_release(&center);

	/* The pen is stroked before the map: a vertical line of width 4 under scale(3, 1) is 12 wide, 10 tall: 120. */
	make_lines(&center, vertical, 2, 0, ALL_CORNERS);
	vec_matrix_scale(&matrix, 3.0f, 1.0f);
	vec_lines_init(&out);
	error = vec_stroke(&center, &style, &matrix, &out);
	CHECK_LONG(error, 0);
	CHECK_NEAR(fill_area(&out, 0, 0, 60, 20), 120.0, 0.05);
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/*
 * Collects the x extents of the contours of a stroked horizontal line
 * (butt caps, so each dash is one rectangle) into low and high.
 */
static size_t
dash_extents(
	const struct vec_lines *lines,
	double *low,
	double *high,
	size_t room)
{
	size_t c;

	for (c = 0; c < lines->contour_count && c < room; c++)
		contour_x_extent(lines, c, &low[c], &high[c]);
	return lines->contour_count;
}

/* Strokes a horizontal line of the given length with butt caps and a dash list, into out. */
static int
stroke_dashed_line(
	struct vec_lines *out,
	float length,
	const float *dashes,
	size_t count,
	float offset,
	float scale,
	enum vec_cap cap)
{
	float points[4];
	struct vec_stroke_style style;
	struct vec_lines center;
	int error;

	points[0] = 0.0f;
	points[1] = 10.0f;
	points[2] = length;
	points[3] = 10.0f;
	make_lines(&center, points, 2, 0, ALL_CORNERS);
	vec_lines_init(out);
	make_style(&style, 2.0f, cap, VEC_JOIN_MITER);
	style.dashes = dashes;
	style.dash_count = count;
	style.dash_offset = offset;
	style.dash_scale = scale;
	error = vec_stroke(&center, &style, NULL, out);
	vec_lines_release(&center);
	return error;
}

/* Checks the dashes of a line against a list of expected [start, end] pairs. */
static void
check_dashes(
	const struct vec_lines *lines,
	const double *expected,
	size_t pairs)
{
	double low[32];
	double high[32];
	size_t n;
	size_t i;

	n = dash_extents(lines, low, high, 32);
	CHECK_LONG(n, pairs);
	for (i = 0; i < n && i < pairs; i++) {
		CHECK_NEAR(low[i], expected[2 * i], 1e-4);
		CHECK_NEAR(high[i], expected[2 * i + 1], 1e-4);
	}
}

/* The dash positions of SVG 2 section 13.5.7 worked by hand. */
static void
test_dashes(void)
{
	static const float even[] = { 5.0f, 5.0f };
	static const float odd_one[] = { 5.0f };
	static const float odd_three[] = { 4.0f, 2.0f, 1.0f };
	static const float zeros[] = { 0.0f, 0.0f };
	static const float tiny[] = { 0.5f };
	/* [5, 5] on 22: dashes [0, 5], [10, 15], [20, 22] (the last cut by the length). */
	static const double pair_basic[] = { 0.0, 5.0, 10.0, 15.0, 20.0, 22.0 };
	/* Offset 3 into the pattern: index 0 has 2 left: [0, 2], gap to 7, [7, 12], gap to 17, [17, 22]. */
	static const double pair_offset3[] = { 0.0, 2.0, 7.0, 12.0, 17.0, 22.0 };
	/* Offset -3 is 7 into the pattern of sum 10: index 1 (a gap) has 3 left: gap to 3, [3, 8], gap to 13, [13, 18], gap to 22. */
	static const double pair_negative[] = { 3.0, 8.0, 13.0, 18.0 };
	/* Offset 5 is the end of the first dash: a dash of length 0 at 0, then gap [0, 5], [5, 10], [15, 20]. */
	static const double pair_boundary[] = { 5.0, 10.0, 15.0, 20.0 };
	/* An offset of a whole number of periods is the same as none: 23 mod 10 = 3. */
	static const double pair_periods[] = { 0.0, 2.0, 7.0, 12.0, 17.0, 22.0 };
	/*
	 * [4, 2, 1] repeated is [4, 2, 1, 4, 2, 1] (sum 14) on 30:
	 * dash 0-4, gap 4-6, dash 6-7, gap 7-11, dash 11-13, gap 13-14,
	 * dash 14-18, gap 18-20, dash 20-21, gap 21-25, dash 25-27, gap 27-28,
	 * dash 28-30 (cut from 28-32).
	 */
	static const double pair_odd[] = { 0.0, 4.0, 6.0, 7.0, 11.0, 13.0, 14.0, 18.0, 20.0, 21.0, 25.0, 27.0, 28.0, 30.0 };
	/* pathLength 80 on a line of 40: scale 0.5 turns [10, 10] into [5, 5]: dashes at 0, 10, 20, 30, each 5 long. */
	static const double pair_scaled[] = { 0.0, 5.0, 10.0, 15.0, 20.0, 25.0, 30.0, 35.0 };
	/* The offset is scaled too: offset 4 * 0.5 = 2 into [5, 5]: [0, 3], gap to 8, [8, 13], gap to 18, [18, 23], gap to 28, [28, 33], gap to 38, [38, 40]. */
	static const double pair_scaled_offset[] = { 0.0, 3.0, 8.0, 13.0, 18.0, 23.0, 28.0, 33.0, 38.0, 40.0 };
	static const float scaled_dashes[] = { 10.0f, 10.0f };
	struct vec_lines out;
	int error;

	error = stroke_dashed_line(&out, 22.0f, even, 2, 0.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	check_dashes(&out, pair_basic, 3);
	vec_lines_release(&out);

	/* An odd list of one is the same as two of it. */
	error = stroke_dashed_line(&out, 22.0f, odd_one, 1, 0.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	check_dashes(&out, pair_basic, 3);
	vec_lines_release(&out);

	error = stroke_dashed_line(&out, 22.0f, even, 2, 3.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	check_dashes(&out, pair_offset3, 3);
	vec_lines_release(&out);

	error = stroke_dashed_line(&out, 22.0f, even, 2, -3.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	check_dashes(&out, pair_negative, 2);
	vec_lines_release(&out);

	error = stroke_dashed_line(&out, 22.0f, even, 2, 5.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	check_dashes(&out, pair_boundary, 2);
	vec_lines_release(&out);

	error = stroke_dashed_line(&out, 22.0f, even, 2, 23.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	check_dashes(&out, pair_periods, 3);
	vec_lines_release(&out);

	/* A negative offset of more than a period: -13 is 7 into the pattern, the same as -3. */
	error = stroke_dashed_line(&out, 22.0f, even, 2, -13.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	check_dashes(&out, pair_negative, 2);
	vec_lines_release(&out);

	error = stroke_dashed_line(&out, 30.0f, odd_three, 3, 0.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	check_dashes(&out, pair_odd, 7);
	vec_lines_release(&out);

	/* Lengths that sum to 0 are no dashing: one solid rectangle, offset or not. */
	error = stroke_dashed_line(&out, 22.0f, zeros, 2, 3.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 1);
	CHECK_NEAR(fill_area(&out, 0, 0, 30, 20), 44.0, 0.05);
	vec_lines_release(&out);

	/* The same with no list at all. */
	error = stroke_dashed_line(&out, 22.0f, NULL, 0, 0.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 1);
	vec_lines_release(&out);

	/* The scale multiplies the lengths. */
	error = stroke_dashed_line(&out, 40.0f, scaled_dashes, 2, 0.0f, 0.5f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	check_dashes(&out, pair_scaled, 4);
	vec_lines_release(&out);

	/* A single dash [10] is [10, 10]: with scale 0.5 on 40, the dashes are 5 long at 0, 10, 20, 30. */
	error = stroke_dashed_line(&out, 40.0f, (const float[]){ 10.0f }, 1, 0.0f, 0.5f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	check_dashes(&out, pair_scaled, 4);
	vec_lines_release(&out);

	/* And the offset. */
	error = stroke_dashed_line(&out, 40.0f, (const float[]){ 10.0f, 10.0f }, 2, 4.0f, 0.5f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	check_dashes(&out, pair_scaled_offset, 5);
	vec_lines_release(&out);

	/* A dash that is a half of the pattern's tiny length and no cap: 0.5 on 4 gives 4 dashes [0, 0.5], [1, 1.5], [2, 2.5], [3, 3.5]. */
	error = stroke_dashed_line(&out, 4.0f, tiny, 1, 0.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 4);
	vec_lines_release(&out);
}

/* Dashes with caps: each dash has its own caps, with the area and the count by hand. */
static void
test_dash_caps(void)
{
	static const float even[] = { 5.0f, 5.0f };
	static const float dots[] = { 0.0f, 10.0f };
	struct vec_lines out;
	int error;

	/* Square caps, width 2: dashes [0, 5], [10, 15], [20, 22] grow by 1 on each side: 7 * 2 + 7 * 2 + 4 * 2 = 36. */
	error = stroke_dashed_line(&out, 22.0f, even, 2, 0.0f, 1.0f, VEC_CAP_SQUARE);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	CHECK_LONG(out.contour_count, 9);
	CHECK_NEAR(fill_area(&out, -2, 0, 30, 20), 36.0, 0.05);
	vec_lines_release(&out);

	/* Round caps: a rectangle and two half discs per dash. */
	error = stroke_dashed_line(&out, 22.0f, even, 2, 0.0f, 1.0f, VEC_CAP_ROUND);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	CHECK_LONG(out.contour_count, 9);
	vec_lines_release(&out);

	/* Dots: dashes [0, 0] with round caps are discs at 0, 10, 20 (the position 30 is not before the length 30). */
	error = stroke_dashed_line(&out, 30.0f, dots, 2, 0.0f, 1.0f, VEC_CAP_ROUND);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	CHECK_LONG(out.contour_count, 3);
	CHECK_NEAR(fill_area(&out, -2, 0, 40, 20), 3.0 * TEST_PI, 0.15);
	vec_lines_release(&out);

	/* The same with square caps: squares of side 2. */
	error = stroke_dashed_line(&out, 30.0f, dots, 2, 0.0f, 1.0f, VEC_CAP_SQUARE);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 3);
	CHECK_NEAR(fill_area(&out, -2, 0, 40, 20), 12.0, 0.05);
	vec_lines_release(&out);

	/* With butt caps they are nothing. */
	error = stroke_dashed_line(&out, 30.0f, dots, 2, 0.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 0);
	vec_lines_release(&out);

	/* The dash that ends the offset exactly, with round caps: the dash of length 0 at the start is a disc. */
	error = stroke_dashed_line(&out, 22.0f, even, 2, 5.0f, 1.0f, VEC_CAP_ROUND);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 1 + 3 + 3);
	vec_lines_release(&out);
}

/* Dashes across a corner get a join, dashes that stop at the corner do not. */
static void
test_dash_joins(void)
{
	static const float corner[] = { 0.0f, 10.0f, 10.0f, 10.0f, 10.0f, 20.0f };
	static const float across[] = { 15.0f, 100.0f };
	static const float stop[] = { 5.0f, 5.0f };
	struct vec_stroke_style style;
	struct vec_lines center;
	struct vec_lines out;
	int error;

	make_lines(&center, corner, 3, 0, ALL_CORNERS);

	/* One dash [0, 15] over the corner: rectangle, join, rectangle. */
	vec_lines_init(&out);
	make_style(&style, 2.0f, VEC_CAP_BUTT, VEC_JOIN_MITER);
	style.dashes = across;
	style.dash_count = 2;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 3);
	check_pieces(&out);
	vec_lines_release(&out);

	/* Dashes [0, 5] and [10, 15]: the second starts at the corner, so no join and two rectangles per dash... one each. */
	vec_lines_init(&out);
	style.dashes = stop;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 2);
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* Closed contours: the join at the first vertex, a dash that wraps, and a dash over the whole contour. */
static void
test_closed(void)
{
	static const float square[] = { 0.0f, 0.0f, 10.0f, 0.0f, 10.0f, 10.0f, 0.0f, 10.0f };
	static const float around[] = { 100.0f };
	static const float wrap[] = { 15.0f, 5.0f };
	static const float whole[] = { 100.0f, 0.0f };
	struct vec_stroke_style style;
	struct vec_lines center;
	struct vec_lines out;
	int error;

	make_lines(&center, square, 4, 1, ALL_CORNERS);

	/* Not dashed: the ring of width 2 round a 10 square, 12 * 12 - 8 * 8 = 80, from four rectangles and four miters. */
	vec_lines_init(&out);
	make_style(&style, 2.0f, VEC_CAP_BUTT, VEC_JOIN_MITER);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 8);
	check_pieces(&out);
	CHECK_NEAR(fill_area(&out, -4, -4, 20, 20), 80.0, 0.05);
	vec_lines_release(&out);

	/* With round caps (which a loop does not have) the count and area stay. */
	vec_lines_init(&out);
	style.cap = VEC_CAP_ROUND;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 8);
	vec_lines_release(&out);
	style.cap = VEC_CAP_BUTT;

	/* One dash longer than the contour covers all of it: the same loop. */
	vec_lines_init(&out);
	style.dashes = around;
	style.dash_count = 1;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 8);
	CHECK_NEAR(fill_area(&out, -4, -4, 20, 20), 80.0, 0.05);
	vec_lines_release(&out);

	/* A dash of 100 and a gap of 0: also the whole contour. */
	vec_lines_init(&out);
	style.dashes = whole;
	style.dash_count = 2;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 8);
	vec_lines_release(&out);

	/*
	 * Dashes [15, 5] with offset 5 on the perimeter 40: [0, 10], [15, 30], [35, 40].
	 * The first starts at 0 and the last ends at 40, so they are one dash
	 * from (0, 5) round the first vertex to (10, 0): the vertical part is
	 * 2 * 5, the horizontal 10 * 2, they overlap in 1, and the miter at the
	 * first vertex adds the outer square 1: 30.  [15, 30] is the vertical
	 * 5 and the top 10 of the square, 30 as well, the two apart: 60, from
	 * rectangle, join, rectangle for each (6 pieces; unjoined there would
	 * be 5).
	 */
	vec_lines_init(&out);
	style.dashes = wrap;
	style.dash_count = 2;
	style.dash_offset = 5.0f;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	CHECK_LONG(out.contour_count, 6);
	CHECK_NEAR(fill_area(&out, -4, -4, 20, 20), 60.0, 0.05);
	vec_lines_release(&out);

	/* A closed contour whose dashes [0, 15] and [20, 35] do not meet at the first vertex: two dashes of 30 each (a corner each). */
	vec_lines_init(&out);
	style.dash_offset = 0.0f;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 6);
	check_pieces(&out);
	CHECK_NEAR(fill_area(&out, -4, -4, 20, 20), 60.0, 0.05);
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* The wrapped dash of a closed contour: its area shows the join at the first vertex. */
static void
test_closed_wrap_area(void)
{
	static const float square[] = { 0.0f, 0.0f, 10.0f, 0.0f, 10.0f, 10.0f, 0.0f, 10.0f };
	static const float list[] = { 35.0f, 5.0f };
	struct vec_stroke_style style;
	struct vec_lines center;
	struct vec_lines out;
	int error;

	/*
	 * Dashes [35, 5] with offset 5: index 0 has 30 left, so [0, 30], gap
	 * [30, 35], dash [35, 40].  The last ends at the perimeter 40, so
	 * [35, 40] and [0, 30] are one dash from (0, 5) over the first vertex
	 * and the next two corners to (0, 10).  It is 4 rectangles (5, 10, 10 and
	 * 10 long, width 2: 10 + 20 + 20 + 20), 3 joins, and the overlaps
	 * of consecutive rectangles in 1 each (3), which the 3 outer corner
	 * squares of the miters fill again: 70.  Were the two dashes separate,
	 * the corner at the first vertex would be missing: 69.
	 */
	make_lines(&center, square, 4, 1, ALL_CORNERS);
	vec_lines_init(&out);
	make_style(&style, 2.0f, VEC_CAP_BUTT, VEC_JOIN_MITER);
	style.dashes = list;
	style.dash_count = 2;
	style.dash_offset = 5.0f;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	check_pieces(&out);
	CHECK_LONG(out.contour_count, 7);
	CHECK_NEAR(fill_area(&out, -4, -4, 20, 20), 70.0, 0.05);
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* The limit of dashes per contour: the rest is one solid dash. */
static void
test_dash_limit(void)
{
	static const float tiny[] = { 0.01f, 0.01f };
	struct vec_lines out;
	double low;
	double high;
	int error;

	/* 0.02 per period on a line of 2000: the dash k starts at 0.02 k; the 65536'th (k = 65535) starts at 1310.7 and runs to 2000. */
	error = stroke_dashed_line(&out, 2000.0f, tiny, 2, 0.0f, 1.0f, VEC_CAP_BUTT);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, VEC_DASH_PIECES_MAX);
	contour_x_extent(&out, out.contour_count - 1, &low, &high);
	CHECK_NEAR(low, 0.02 * 65535.0, 0.01);
	CHECK_NEAR(high, 2000.0, 1e-3);
	check_pieces(&out);
	vec_lines_release(&out);
}

/* Zero-length subpaths: discs, axis-aligned squares and nothing. */
static void
test_zero_length(void)
{
	static const float one[] = { 5.0f, 5.0f };
	static const float two[] = { 5.0f, 5.0f, 5.0f, 5.0f };
	static const float three[] = { 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f };
	struct vec_stroke_style style;
	struct vec_lines center;
	struct vec_lines out;
	double low;
	double high;
	double area;
	int variant;
	int error;

	for (variant = 0; variant < 4; variant++) {
		if (variant == 0)
			make_lines(&center, one, 1, 1, ALL_CORNERS);
		else if (variant == 1)
			make_lines(&center, two, 2, 0, ALL_CORNERS);
		else if (variant == 2)
			make_lines(&center, three, 3, 1, ALL_CORNERS);
		else
			make_lines(&center, three, 3, 0, ALL_CORNERS);

		/* Round caps: a disc of radius 2 about (5, 5). */
		vec_lines_init(&out);
		make_style(&style, 4.0f, VEC_CAP_ROUND, VEC_JOIN_MITER);
		error = vec_stroke(&center, &style, NULL, &out);
		CHECK_LONG(error, 0);
		CHECK_LONG(out.contour_count, 1);
		check_pieces(&out);
		contour_x_extent(&out, 0, &low, &high);
		CHECK(low >= 3.0 - 1e-4);
		CHECK(high <= 7.0 + 1e-4);
		CHECK(high - low > 3.9);
		area = fill_area(&out, 0, 0, 12, 12);
		CHECK(area <= 4.0 * TEST_PI + 0.05);
		CHECK(area >= 4.0 * TEST_PI - 0.2);
		vec_lines_release(&out);

		/* Square caps: the square [3, 7] * [3, 7] with its sides on the axes. */
		vec_lines_init(&out);
		style.cap = VEC_CAP_SQUARE;
		error = vec_stroke(&center, &style, NULL, &out);
		CHECK_LONG(error, 0);
		CHECK_LONG(out.contour_count, 1);
		CHECK_LONG(out.contours[0].count, 4);
		check_pieces(&out);
		CHECK(has_vertex(&out, 3.0, 3.0, 1e-5));
		CHECK(has_vertex(&out, 7.0, 3.0, 1e-5));
		CHECK(has_vertex(&out, 7.0, 7.0, 1e-5));
		CHECK(has_vertex(&out, 3.0, 7.0, 1e-5));
		vec_lines_release(&out);

		/* Butt caps: nothing. */
		vec_lines_init(&out);
		style.cap = VEC_CAP_BUTT;
		error = vec_stroke(&center, &style, NULL, &out);
		CHECK_LONG(error, 0);
		CHECK_LONG(out.contour_count, 0);
		vec_lines_release(&out);

		vec_lines_release(&center);
	}

	/* A moveto alone (one vertex, not closed) draws nothing whatever the cap. */
	make_lines(&center, one, 1, 0, ALL_CORNERS);
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_ROUND, VEC_JOIN_ROUND);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 0);
	CHECK_LONG(out.vertex_count, 0);
	vec_lines_release(&out);
	vec_lines_release(&center);

	/* A zero-length subpath with a dash on it: the first dash (at 0) is of length 0, a disc; a gap there is nothing. */
	make_lines(&center, two, 2, 0, ALL_CORNERS);
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_ROUND, VEC_JOIN_ROUND);
	{
		static const float list[] = { 3.0f, 3.0f };

		style.dashes = list;
		style.dash_count = 2;
		error = vec_stroke(&center, &style, NULL, &out);
		CHECK_LONG(error, 0);
		CHECK_LONG(out.contour_count, 1);
		vec_lines_release(&out);
		vec_lines_init(&out);
		style.dash_offset = 4.0f;
		error = vec_stroke(&center, &style, NULL, &out);
		CHECK_LONG(error, 0);
		CHECK_LONG(out.contour_count, 0);
	}
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* A zero-length segment between two segments does not change the join, and gives no NaN. */
static void
test_zero_length_segment(void)
{
	static const float points[] = { 2.0f, 10.0f, 12.0f, 10.0f, 12.0f, 10.0f, 12.0f, 20.0f };
	struct vec_stroke_style style;
	struct vec_lines center;
	struct vec_lines out;
	size_t i;
	int error;

	make_lines(&center, points, 4, 0, ALL_CORNERS);
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_MITER);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	/* Two rectangles and one join, the same as without the repeated vertex. */
	CHECK_LONG(out.contour_count, 3);
	check_pieces(&out);
	for (i = 0; i < out.vertex_count; i++) {
		CHECK(vec_finite(out.vertices[i].x));
		CHECK(vec_finite(out.vertices[i].y));
	}
	CHECK_NEAR(fill_area(&out, 0, 0, 20, 30), 80.0, 0.05);
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* A vertex inside a flattened curve (no corner flag) is joined round whatever the style says. */
static void
test_curve_vertices(void)
{
	static const float right_turn[] = { 2.0f, 10.0f, 12.0f, 10.0f, 12.0f, 20.0f };
	struct vec_stroke_style style;
	struct vec_lines center;
	struct vec_lines out;
	int error;

	/* Only the middle vertex lacks the corner flag. */
	make_lines(&center, right_turn, 3, 0, VEC_VERTEX_CORNER);
	center.vertices[1].flags = 0;
	vec_lines_init(&out);
	make_style(&style, 4.0f, VEC_CAP_BUTT, VEC_JOIN_MITER);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contour_count, 3);
	/* The join is a round sector of 10 vertices (8 steps), not a miter kite of 4. */
	CHECK_LONG(out.contours[1].count, 10);
	CHECK_NEAR(fill_area(&out, 0, 0, 20, 30), 76.0 + sector_area(2.0, TEST_PI / 2.0, 8), 0.05);
	vec_lines_release(&out);

	/* With the corner flag it is the miter kite. */
	center.vertices[1].flags = VEC_VERTEX_CORNER;
	vec_lines_init(&out);
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	CHECK_LONG(out.contours[1].count, 4);
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* A style or a matrix out of its range is refused with nothing appended. */
static void
test_invalid(void)
{
	static const float line[] = { 4.0f, 10.0f, 24.0f, 10.0f };
	static const float dashes[] = { 5.0f, 5.0f };
	static const float bad_dashes_negative[] = { 5.0f, -1.0f };
	float bad_dashes_nan[2];
	float big[4];
	struct vec_stroke_style style;
	struct vec_matrix matrix;
	struct vec_lines center;
	struct vec_lines out;
	struct vec_lines malformed;
	struct vec_contour contour;
	struct vec_vertex vertex;
	float zero;
	int variant;
	int error;

	zero = 0.0f;
	bad_dashes_nan[0] = 5.0f;
	bad_dashes_nan[1] = zero / zero;
	make_lines(&center, line, 2, 0, ALL_CORNERS);

	for (variant = 0; variant < 22; variant++) {
		vec_lines_init(&out);
		vec_lines_begin(&out, 1.0f, 1.0f, 0);
		vec_lines_add(&out, 2.0f, 1.0f, 0);
		vec_lines_add(&out, 2.0f, 2.0f, 0);
		vec_lines_close(&out);
		make_style(&style, 4.0f, VEC_CAP_ROUND, VEC_JOIN_MITER);
		switch (variant) {
		case 0:
			style.width = 0.0f;
			break;
		case 1:
			style.width = -1.0f;
			break;
		case 2:
			style.width = zero / zero;
			break;
		case 3:
			style.width = 1.0f / zero;
			break;
		case 4:
			style.miter_limit = 0.5f;
			break;
		case 5:
			style.miter_limit = zero / zero;
			break;
		case 6:
			style.dashes = NULL;
			style.dash_count = 2;
			break;
		case 7:
			style.dashes = dashes;
			style.dash_count = VEC_DASHES_MAX + 1;
			break;
		case 8:
			style.dashes = bad_dashes_negative;
			style.dash_count = 2;
			break;
		case 9:
			style.dashes = bad_dashes_nan;
			style.dash_count = 2;
			break;
		case 10:
			style.dash_scale = 0.0f;
			break;
		case 11:
			style.dash_scale = zero / zero;
			break;
		case 12:
			style.tolerance = 0.0f;
			break;
		case 13:
			style.tolerance = zero / zero;
			break;
		case 14:
			style.dash_offset = 1.0f / zero;
			break;
		case 15:
			style.cap = (enum vec_cap)7;
			break;
		case 16:
			style.join = (enum vec_join)9;
			break;
		case 17:
			style.dash_scale = -1.0f;
			break;
		case 18:
			style.miter_limit = 1.0f / zero;
			break;
		case 19:
			style.dashes = NULL;
			style.dash_count = VEC_DASHES_MAX + 1;
			break;
		case 20:
			style.tolerance = 0.0000001f;
			break;
		default:
			break;
		}
		error = vec_stroke(&center, &style, NULL, &out);
		if (variant == 21)
			CHECK_LONG(error, 0);
		else
			CHECK_LONG(error, EINVAL);
		if (variant != 21) {
			CHECK_LONG(out.contour_count, 1);
			CHECK_LONG(out.vertex_count, 3);
		}
		vec_lines_release(&out);
	}

	/* A matrix that is not valid. */
	make_style(&style, 4.0f, VEC_CAP_ROUND, VEC_JOIN_MITER);
	vec_matrix_identity(&matrix);
	matrix.a = zero / zero;
	vec_lines_init(&out);
	error = vec_stroke(&center, &style, &matrix, &out);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(out.contour_count, 0);
	vec_lines_release(&out);

	/* A vertex that maps out of range: scale 1e7 takes 24 to 2.4e8, beyond 2^24; nothing stays appended, even the pieces before it. */
	vec_matrix_scale(&matrix, 10000000.0f, 1.0f);
	matrix.a = 1000000.0f;
	vec_lines_init(&out);
	error = vec_stroke(&center, &style, &matrix, &out);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(out.contour_count, 0);
	CHECK_LONG(out.vertex_count, 0);
	vec_lines_release(&out);

	/* Two contours where the second fails: the first one's pieces are taken back too. */
	big[0] = 0.0f;
	big[1] = 0.0f;
	big[2] = 10.0f;
	big[3] = 0.0f;
	vec_lines_release(&center);
	make_lines(&center, big, 2, 0, ALL_CORNERS);
	vec_lines_begin(&center, 5.0f, 5.0f, VEC_VERTEX_CORNER);
	vec_lines_add(&center, 16000000.0f, 5.0f, VEC_VERTEX_CORNER);
	vec_matrix_scale(&matrix, 2.0f, 1.0f);
	vec_lines_init(&out);
	error = vec_stroke(&center, &style, &matrix, &out);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(out.contour_count, 0);
	CHECK_LONG(out.vertex_count, 0);
	vec_lines_release(&out);
	vec_lines_release(&center);

	/* Malformed polylines: a contour that is not inside the vertex array. */
	vertex.x = 0.0f;
	vertex.y = 0.0f;
	vertex.flags = 0;
	contour.first = 0;
	contour.count = 5;
	contour.closed = 0;
	memset(&malformed, 0, sizeof(malformed));
	malformed.vertices = &vertex;
	malformed.vertex_count = 1;
	malformed.contours = &contour;
	malformed.contour_count = 1;
	vec_lines_init(&out);
	error = vec_stroke(&malformed, &style, NULL, &out);
	CHECK_LONG(error, EINVAL);
	vec_lines_release(&out);

	/* A NULL list with a count of 0 is fine, and a dash list under the limit with a non-NULL pointer and count 0 too. */
	make_lines(&center, line, 2, 0, ALL_CORNERS);
	vec_lines_init(&out);
	style.dashes = dashes;
	style.dash_count = 0;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, 0);
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* The limits of the output: ENOBUFS, with out put back. */
static void
test_limits(void)
{
	static const float line[] = { 4.0f, 10.0f, 24.0f, 10.0f };
	struct vec_stroke_style style;
	struct vec_lines center;
	struct vec_lines out;
	size_t before;
	int error;

	make_lines(&center, line, 2, 0, ALL_CORNERS);
	make_style(&style, 4.0f, VEC_CAP_SQUARE, VEC_JOIN_MITER);

	/* The output has room for 6 more vertices of the limit: the rectangle (4) fits, the first cap (4) does not. */
	vec_lines_init(&out);
	vec_lines_begin(&out, 0.0f, 0.0f, 0);
	vec_lines_add(&out, 1.0f, 0.0f, 0);
	vec_lines_add(&out, 1.0f, 1.0f, 0);
	vec_lines_close(&out);
	out.vertex_count = VEC_LINES_VERTICES_MAX - 6;
	out.contours[0].count = VEC_LINES_VERTICES_MAX - 6;
	before = out.contour_count;
	error = vec_stroke(&center, &style, NULL, &out);
	CHECK_LONG(error, ENOBUFS);
	CHECK_LONG(out.contour_count, before);
	CHECK_LONG(out.vertex_count, VEC_LINES_VERTICES_MAX - 6);
	vec_lines_release(&out);
	vec_lines_release(&center);
}

/* A random run: random polylines and styles must never fail unexpectedly or leave a malformed piece, and the center line lies in the fill. */
static void
test_random(void)
{
	struct vec_stroke_style style;
	struct vec_matrix matrix;
	struct vec_lines center;
	struct vec_lines out;
	const struct vec_vertex *a;
	const struct vec_vertex *b;
	float dashes[6];
	float width;
	float limit;
	uint32_t state;
	double mx;
	double my;
	double dx;
	double dy;
	double determinant;
	size_t run;
	size_t i;
	size_t n;
	size_t segments;
	int closed;
	int use_dashes;
	int use_matrix;
	int error;
	int pick;

	state = 20260415U;
	for (run = 0; run < RANDOM_RUNS; run++) {
		vec_lines_init(&center);
		n = 1 + (size_t)(check_random(&state) % 8U);
		for (i = 0; i < n; i++) {
			pick = (int)(check_random(&state) % 5U);
			if (i > 0 && pick == 0) {
				/* A repeated vertex: a zero-length segment. */
				error = vec_lines_add(&center, center.vertices[center.vertex_count - 1].x,
				    center.vertices[center.vertex_count - 1].y, (check_random(&state) & 1U) ? VEC_VERTEX_CORNER : 0U);
			} else if (i == 0) {
				error = vec_lines_begin(&center, check_random_float(&state, -50.0f, 50.0f), check_random_float(&state, -50.0f, 50.0f), VEC_VERTEX_CORNER);
			} else {
				error = vec_lines_add(&center, check_random_float(&state, -50.0f, 50.0f), check_random_float(&state, -50.0f, 50.0f),
				    (check_random(&state) & 1U) ? VEC_VERTEX_CORNER : 0U);
			}
			CHECK_LONG(error, 0);
		}
		closed = (int)(check_random(&state) % 3U) == 0;
		if (closed)
			vec_lines_close(&center);

		width = check_random_float(&state, 0.5f, 12.0f);
		limit = check_random_float(&state, 1.0f, 8.0f);
		make_style(&style, width, (enum vec_cap)(check_random(&state) % 3U), (enum vec_join)(check_random(&state) % 3U));
		style.miter_limit = limit;
		style.tolerance = check_random_float(&state, 0.01f, 1.0f);
		use_dashes = (int)(check_random(&state) % 3U) == 0;
		if (use_dashes) {
			style.dash_count = 1 + (size_t)(check_random(&state) % 6U);
			for (i = 0; i < style.dash_count; i++)
				dashes[i] = check_random_float(&state, 0.0f, 12.0f);
			dashes[0] += 0.5f;
			style.dashes = dashes;
			style.dash_offset = check_random_float(&state, -30.0f, 30.0f);
			style.dash_scale = check_random_float(&state, 0.2f, 3.0f);
		}
		use_matrix = (int)(check_random(&state) % 2U);
		if (use_matrix) {
			do {
				matrix.a = check_random_float(&state, -2.0f, 2.0f);
				matrix.b = check_random_float(&state, -2.0f, 2.0f);
				matrix.c = check_random_float(&state, -2.0f, 2.0f);
				matrix.d = check_random_float(&state, -2.0f, 2.0f);
				determinant = (double)matrix.a * matrix.d - (double)matrix.b * matrix.c;
			} while (fabs(determinant) < 0.2);
			matrix.e = check_random_float(&state, -20.0f, 20.0f);
			matrix.f = check_random_float(&state, -20.0f, 20.0f);
		}

		vec_lines_init(&out);
		error = vec_stroke(&center, &style, use_matrix ? &matrix : NULL, &out);
		CHECK_LONG(error, 0);
		check_pieces(&out);

		/* Without dashes the middle of every segment of some length is inside the fill (nonzero winding). */
		if (error == 0 && !use_dashes && center.contour_count == 1 && center.contours[0].count >= 2) {
			segments = center.contours[0].count - 1;
			for (i = 0; i < segments; i++) {
				a = &center.vertices[i];
				b = &center.vertices[i + 1];
				dx = (double)b->x - a->x;
				dy = (double)b->y - a->y;
				if (hypot(dx, dy) < 0.05)
					continue;
				mx = ((double)a->x + b->x) * 0.5;
				my = ((double)a->y + b->y) * 0.5;
				if (use_matrix) {
					double tx;
					double ty;

					tx = (double)matrix.a * mx + (double)matrix.c * my + matrix.e;
					ty = (double)matrix.b * mx + (double)matrix.d * my + matrix.f;
					mx = tx;
					my = ty;
				}
				CHECK(winding_at(&out, mx, my) != 0);
			}
		}
		vec_lines_release(&out);
		vec_lines_release(&center);
	}
}

int
main(void)
{
	test_length();
	test_caps();
	test_joins();
	test_miter_limit();
	test_arc_steps();
	test_device_map();
	test_dashes();
	test_dash_caps();
	test_dash_joins();
	test_closed();
	test_closed_wrap_area();
	test_dash_limit();
	test_zero_length();
	test_zero_length_segment();
	test_curve_vertices();
	test_invalid();
	test_limits();
	test_random();

	return check_finish("stroke");
}
