/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of vec-flatten: the polyline building steps and their
 * limits, the structure check, flattening of lines (corner flags, closes,
 * zero-length segments, lone moves), the segment counts of quadratic and
 * cubic curves against hand calculations of the formula, the vertices on
 * the curve and the error bound against a dense sample, the limits and
 * the rollback on failure, flattening under a matrix, the in-place
 * transform and the bounding box, and a random run under the sanitizers.
 * Every expected value is a hand calculation or follows from the formulas
 * of the specifications; no other implementation's output is used.
 */

#include "check.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include "vector/vec-flatten.h"

/* The number of parameter steps of the dense sweep that measures the distance between a curve and its polyline. */
#define SWEEP_SAMPLES	2000

/* Flattens a path built with the builder, through its view. */
static int
flatten_path(
	const struct vec_path *path,
	const struct vec_matrix *matrix,
	float tolerance,
	struct vec_lines *out)
{
	struct vec_path_view view;

	vec_path_view_of(path, &view);
	return vec_flatten(&view, matrix, tolerance, out);
}

/* Checks the i'th vertex of polylines against a position and flags. */
static void
check_vertex(
	const struct vec_lines *lines,
	size_t index,
	double x,
	double y,
	unsigned flags,
	double tolerance)
{
	if (index >= lines->vertex_count) {
		CHECK(index < lines->vertex_count);
		return;
	}
	CHECK_NEAR(lines->vertices[index].x, x, tolerance);
	CHECK_NEAR(lines->vertices[index].y, y, tolerance);
	CHECK_LONG(lines->vertices[index].flags, flags);
}

/* Checks the i'th contour of polylines against its first vertex, count and closed state. */
static void
check_contour(
	const struct vec_lines *lines,
	size_t index,
	size_t first,
	size_t count,
	int closed)
{
	if (index >= lines->contour_count) {
		CHECK(index < lines->contour_count);
		return;
	}
	CHECK_LONG(lines->contours[index].first, first);
	CHECK_LONG(lines->contours[index].count, count);
	CHECK_LONG(lines->contours[index].closed != 0, closed);
}

/* Evaluates a quadratic Bezier curve at t from its control points, in the Bernstein form in double precision. */
static void
quad_at(
	const double *px,
	const double *py,
	double t,
	double *x,
	double *y)
{
	double mt;

	mt = 1.0 - t;
	*x = mt * mt * px[0] + 2.0 * mt * t * px[1] + t * t * px[2];
	*y = mt * mt * py[0] + 2.0 * mt * t * py[1] + t * t * py[2];
}

/* Evaluates a cubic Bezier curve at t from its control points, in the Bernstein form in double precision. */
static void
cubic_at(
	const double *px,
	const double *py,
	double t,
	double *x,
	double *y)
{
	double mt;

	mt = 1.0 - t;
	*x = mt * mt * mt * px[0] + 3.0 * mt * mt * t * px[1] + 3.0 * mt * t * t * px[2] + t * t * t * px[3];
	*y = mt * mt * mt * py[0] + 3.0 * mt * mt * t * py[1] + 3.0 * mt * t * t * py[2] + t * t * t * py[3];
}

/* Gives the distance from a point to a segment, in double precision. */
static double
segment_distance(
	double x,
	double y,
	double ax,
	double ay,
	double bx,
	double by)
{
	double dx;
	double dy;
	double length_squared;
	double t;
	double cx;
	double cy;

	dx = bx - ax;
	dy = by - ay;
	length_squared = dx * dx + dy * dy;
	t = 0.0;
	if (length_squared > 0.0)
		t = ((x - ax) * dx + (y - ay) * dy) / length_squared;
	if (t < 0.0)
		t = 0.0;
	if (t > 1.0)
		t = 1.0;
	cx = ax + t * dx;
	cy = ay + t * dy;
	return sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
}

/* Gives the distance from a point to the polyline of the first contour. */
static double
polyline_distance(
	const struct vec_lines *lines,
	double x,
	double y)
{
	size_t i;
	size_t last;
	double best;
	double distance;

	best = 1e30;
	last = lines->contours[0].count;
	for (i = 0; i + 1 < last; i++) {
		distance = segment_distance(x, y, lines->vertices[i].x, lines->vertices[i].y,
		    lines->vertices[i + 1].x, lines->vertices[i + 1].y);
		if (distance < best)
			best = distance;
	}
	return best;
}

/*
 * Gives the largest distance from the curve (sampled densely) to the
 * polyline of the first contour of lines.
 */
static double
curve_error(
	const struct vec_lines *lines,
	const double *px,
	const double *py,
	int degree)
{
	double worst;
	double x;
	double y;
	double distance;
	int i;

	worst = 0.0;
	for (i = 0; i <= SWEEP_SAMPLES; i++) {
		if (degree == 2)
			quad_at(px, py, (double)i / SWEEP_SAMPLES, &x, &y);
		else
			cubic_at(px, py, (double)i / SWEEP_SAMPLES, &x, &y);
		distance = polyline_distance(lines, x, y);
		if (distance > worst)
			worst = distance;
	}
	return worst;
}

/* The building steps: begin, add, close, reset, release, and the refusal of a bad point. */
static void
test_building(void)
{
	struct vec_lines lines;
	size_t capacity;
	float nan;
	int error;

	/* A zeroed structure and an initialized one are both valid and empty. */
	memset(&lines, 0xaa, sizeof(lines));
	vec_lines_init(&lines);
	CHECK_LONG(lines.vertex_count, 0);
	CHECK_LONG(lines.contour_count, 0);
	CHECK(lines.vertices == NULL);
	CHECK(lines.contours == NULL);
	CHECK_LONG(vec_lines_check(&lines), 0);

	/* A close with no contour does nothing. */
	vec_lines_close(&lines);
	CHECK_LONG(lines.contour_count, 0);

	/* An add with no contour begins one, with the flags given. */
	CHECK_LONG(vec_lines_add(&lines, 1.0f, 2.0f, VEC_VERTEX_CORNER), 0);
	CHECK_LONG(lines.contour_count, 1);
	CHECK_LONG(lines.vertex_count, 1);
	check_contour(&lines, 0, 0, 1, 0);
	check_vertex(&lines, 0, 1.0, 2.0, VEC_VERTEX_CORNER, 0.0);

	/* An add extends the last contour; flags are kept as given. */
	CHECK_LONG(vec_lines_add(&lines, 3.0f, 4.0f, 0), 0);
	CHECK_LONG(vec_lines_add(&lines, 5.0f, 6.0f, VEC_VERTEX_CORNER), 0);
	check_contour(&lines, 0, 0, 3, 0);
	check_vertex(&lines, 1, 3.0, 4.0, 0, 0.0);
	check_vertex(&lines, 2, 5.0, 6.0, VEC_VERTEX_CORNER, 0.0);

	/* A close marks the last contour and adds no vertex. */
	vec_lines_close(&lines);
	check_contour(&lines, 0, 0, 3, 1);
	CHECK_LONG(lines.vertex_count, 3);

	/* A begin starts a second contour after the first vertices; the first contour keeps its state. */
	CHECK_LONG(vec_lines_begin(&lines, 7.0f, 8.0f, VEC_VERTEX_CORNER), 0);
	CHECK_LONG(lines.contour_count, 2);
	check_contour(&lines, 0, 0, 3, 1);
	check_contour(&lines, 1, 3, 1, 0);
	check_vertex(&lines, 3, 7.0, 8.0, VEC_VERTEX_CORNER, 0.0);
	CHECK_LONG(vec_lines_check(&lines), 0);

	/* A point that is not valid is refused with EINVAL and nothing changes. */
	nan = NAN;
	CHECK_LONG(vec_lines_begin(&lines, nan, 0.0f, 0), EINVAL);
	CHECK_LONG(vec_lines_add(&lines, 0.0f, INFINITY, 0), EINVAL);
	CHECK_LONG(vec_lines_add(&lines, VEC_COORD_MAX * 2.0f, 0.0f, 0), EINVAL);
	CHECK_LONG(vec_lines_begin(&lines, 0.0f, -VEC_COORD_MAX * 2.0f, 0), EINVAL);
	CHECK_LONG(lines.vertex_count, 4);
	CHECK_LONG(lines.contour_count, 2);

	/* The coordinate limit itself is accepted. */
	CHECK_LONG(vec_lines_add(&lines, VEC_COORD_MAX, -VEC_COORD_MAX, 0), 0);
	CHECK_LONG(lines.vertex_count, 5);

	/* An add refuses a last contour that does not end at the end of the vertex array, and nothing changes. */
	lines.contours[1].count = 1;
	lines.contours[0].count = 3;
	lines.vertex_count = 6;
	CHECK_LONG(vec_lines_add(&lines, 1.0f, 1.0f, 0), EINVAL);
	CHECK_LONG(lines.vertex_count, 6);
	lines.vertex_count = 5;
	lines.contours[1].count = 2;

	/* A reset empties the counts and keeps the arrays. */
	capacity = lines.vertex_capacity;
	vec_lines_reset(&lines);
	CHECK_LONG(lines.vertex_count, 0);
	CHECK_LONG(lines.contour_count, 0);
	CHECK(lines.vertices != NULL);
	CHECK_LONG(lines.vertex_capacity, capacity);

	/* The kept arrays are reused from the start. */
	error = vec_lines_begin(&lines, 9.0f, 9.0f, 0);
	CHECK_LONG(error, 0);
	check_contour(&lines, 0, 0, 1, 0);

	/* A release frees everything and leaves the empty structure. */
	vec_lines_release(&lines);
	CHECK(lines.vertices == NULL);
	CHECK(lines.contours == NULL);
	CHECK_LONG(lines.vertex_count, 0);
	CHECK_LONG(lines.vertex_capacity, 0);
	CHECK_LONG(lines.contour_capacity, 0);
}

/* The limits of one structure: the most vertices and the most contours. */
static void
test_limits_of_lines(void)
{
	struct vec_lines lines;
	size_t i;
	int error;

	/* One contour grows to the most vertices, and the next vertex is refused with ENOBUFS and no change. */
	vec_lines_init(&lines);
	error = vec_lines_begin(&lines, 0.0f, 0.0f, 0);
	CHECK_LONG(error, 0);
	error = 0;
	for (i = 1; i < VEC_LINES_VERTICES_MAX && error == 0; i++)
		error = vec_lines_add(&lines, (float)(i & 1023U), 1.0f, 0);
	CHECK_LONG(error, 0);
	CHECK_LONG(lines.vertex_count, VEC_LINES_VERTICES_MAX);
	CHECK_LONG(lines.contours[0].count, VEC_LINES_VERTICES_MAX);
	CHECK_LONG(vec_lines_add(&lines, 1.0f, 1.0f, 0), ENOBUFS);
	CHECK_LONG(vec_lines_begin(&lines, 1.0f, 1.0f, 0), ENOBUFS);
	CHECK_LONG(lines.vertex_count, VEC_LINES_VERTICES_MAX);
	CHECK_LONG(lines.contour_count, 1);
	CHECK_LONG(lines.contours[0].count, VEC_LINES_VERTICES_MAX);
	CHECK_LONG(vec_lines_check(&lines), 0);
	vec_lines_release(&lines);

	/* Contours grow to the most contours, and the next begin is refused with ENOBUFS and no change. */
	vec_lines_init(&lines);
	error = 0;
	for (i = 0; i < VEC_LINES_CONTOURS_MAX && error == 0; i++)
		error = vec_lines_begin(&lines, 1.0f, (float)(i & 255U), 0);
	CHECK_LONG(error, 0);
	CHECK_LONG(lines.contour_count, VEC_LINES_CONTOURS_MAX);
	CHECK_LONG(vec_lines_begin(&lines, 1.0f, 1.0f, 0), ENOBUFS);
	CHECK_LONG(lines.contour_count, VEC_LINES_CONTOURS_MAX);
	CHECK_LONG(lines.vertex_count, VEC_LINES_CONTOURS_MAX);
	CHECK_LONG(vec_lines_check(&lines), 0);
	vec_lines_release(&lines);
}

/* The structure check: valid, and each way a structure can be malformed. */
static void
test_check(void)
{
	struct vec_vertex vertices[3];
	struct vec_contour contours[2];
	struct vec_lines lines;
	size_t i;

	for (i = 0; i < 3; i++) {
		vertices[i].x = (float)i;
		vertices[i].y = 1.0f;
		vertices[i].flags = 0;
	}
	contours[0].first = 0;
	contours[0].count = 2;
	contours[0].closed = 0;
	contours[1].first = 2;
	contours[1].count = 1;
	contours[1].closed = 1;
	memset(&lines, 0, sizeof(lines));
	lines.vertices = vertices;
	lines.vertex_count = 3;
	lines.vertex_capacity = 3;
	lines.contours = contours;
	lines.contour_count = 2;
	lines.contour_capacity = 2;

	/* A well formed structure passes, and so does an empty one. */
	CHECK_LONG(vec_lines_check(&lines), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(vec_lines_check(&lines), 0);
	lines.vertex_count = 3;
	lines.contour_count = 2;

	/* A contour with no vertex is refused. */
	contours[1].count = 0;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
	contours[1].count = 1;

	/* A contour that runs one past the vertex array is refused; ending exactly at the end is accepted. */
	contours[1].count = 2;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
	contours[1].count = 1;
	CHECK_LONG(vec_lines_check(&lines), 0);

	/* A contour that starts past the vertex array, or exactly at its end with one vertex, is refused. */
	contours[1].first = 4;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
	contours[1].first = 3;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
	contours[1].first = 2;

	/* A contour whose first plus count overflows is refused, not wrapped into range. */
	contours[1].first = (size_t)-1;
	contours[1].count = 2;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
	contours[1].first = 1;
	contours[1].count = (size_t)-1;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
	contours[1].first = 2;
	contours[1].count = 1;
	CHECK_LONG(vec_lines_check(&lines), 0);

	/* A vertex that is not valid is refused. */
	vertices[1].y = NAN;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
	vertices[1].y = VEC_COORD_MAX * 2.0f;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
	vertices[1].y = VEC_COORD_MAX;
	CHECK_LONG(vec_lines_check(&lines), 0);

	/* Counts beyond the limits, or counts with no array, are refused. */
	lines.vertex_count = VEC_LINES_VERTICES_MAX + 1;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
	lines.vertex_count = 3;
	lines.contour_count = VEC_LINES_CONTOURS_MAX + 1;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
	lines.contour_count = 2;
	lines.vertices = NULL;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
	lines.vertices = vertices;
	lines.contours = NULL;
	CHECK_LONG(vec_lines_check(&lines), EINVAL);
}

/* Lines only: corner flags, closes, zero-length segments, lone moves, a close followed by a draw. */
static void
test_straight(void)
{
	static const unsigned char after_close_verbs[] = {
		VEC_VERB_MOVE, VEC_VERB_LINE, VEC_VERB_LINE, VEC_VERB_CLOSE, VEC_VERB_LINE, VEC_VERB_CLOSE
	};
	static const struct vec_point after_close_points[] = {
		{ 1.0f, 1.0f }, { 5.0f, 1.0f }, { 5.0f, 4.0f }, { 2.0f, 6.0f }
	};
	struct vec_path_view view;
	struct vec_path path;
	struct vec_lines lines;

	/* An open polyline of three vertices: every vertex is a corner, not closed. */
	vec_path_init(&path);
	vec_lines_init(&lines);
	CHECK_LONG(vec_path_move_to(&path, 1.0f, 2.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 5.0f, 2.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 5.0f, 6.0f), 0);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.contour_count, 1);
	CHECK_LONG(lines.vertex_count, 3);
	check_contour(&lines, 0, 0, 3, 0);
	check_vertex(&lines, 0, 1.0, 2.0, VEC_VERTEX_CORNER, 0.0);
	check_vertex(&lines, 1, 5.0, 2.0, VEC_VERTEX_CORNER, 0.0);
	check_vertex(&lines, 2, 5.0, 6.0, VEC_VERTEX_CORNER, 0.0);
	CHECK_LONG(vec_lines_check(&lines), 0);

	/* A close marks the contour closed and adds no vertex. */
	CHECK_LONG(vec_path_close(&path), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 3);
	check_contour(&lines, 0, 0, 3, 1);
	vec_path_release(&path);

	/* A last vertex equal to the first stays as it is, and the close still adds none. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 10.0f, 0.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_close(&path), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 3);
	check_contour(&lines, 0, 0, 3, 1);
	check_vertex(&lines, 2, 0.0, 0.0, VEC_VERTEX_CORNER, 0.0);
	vec_path_release(&path);

	/* Two subpaths become two contours, each starting with its own first vertex. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 4.0f, 0.0f), 0);
	CHECK_LONG(vec_path_close(&path), 0);
	CHECK_LONG(vec_path_move_to(&path, 10.0f, 10.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 12.0f, 10.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 12.0f, 14.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.contour_count, 2);
	CHECK_LONG(lines.vertex_count, 5);
	check_contour(&lines, 0, 0, 2, 1);
	check_contour(&lines, 1, 2, 3, 0);
	check_vertex(&lines, 2, 10.0, 10.0, VEC_VERTEX_CORNER, 0.0);
	vec_path_release(&path);

	/* A zero-length segment is kept: M 3 3 L 3 3 has two vertices. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 3.0f, 3.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 3.0f, 3.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 2);
	check_contour(&lines, 0, 0, 2, 0);
	vec_path_release(&path);

	/* A move with nothing after it is a contour of one vertex, open; "M Z" is one vertex, closed. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 7.0f, 8.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.contour_count, 1);
	check_contour(&lines, 0, 0, 1, 0);
	check_vertex(&lines, 0, 7.0, 8.0, VEC_VERTEX_CORNER, 0.0);
	CHECK_LONG(vec_path_close(&path), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	check_contour(&lines, 0, 0, 1, 1);
	vec_path_release(&path);

	/* Two moves in a row are two contours of one vertex each. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 1.0f, 1.0f), 0);
	CHECK_LONG(vec_path_move_to(&path, 2.0f, 2.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.contour_count, 2);
	check_contour(&lines, 0, 0, 1, 0);
	check_contour(&lines, 1, 1, 1, 0);
	vec_path_release(&path);

	/* An empty path adds nothing and succeeds. */
	vec_path_init(&path);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.contour_count, 0);
	CHECK_LONG(lines.vertex_count, 0);
	vec_path_release(&path);

	/*
	 * A view built by hand with a line right after a close: the first
	 * subpath M(1,1) L(5,1) L(5,4) Z is closed, and the line to (2,6)
	 * starts a new contour at (1,1), the closed subpath's first point.
	 */
	view.verbs = after_close_verbs;
	view.verb_count = 6;
	view.points = after_close_points;
	view.point_count = 4;
	vec_lines_reset(&lines);
	CHECK_LONG(vec_flatten(&view, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.contour_count, 2);
	CHECK_LONG(lines.vertex_count, 5);
	check_contour(&lines, 0, 0, 3, 1);
	check_contour(&lines, 1, 3, 2, 1);
	check_vertex(&lines, 3, 1.0, 1.0, VEC_VERTEX_CORNER, 0.0);
	check_vertex(&lines, 4, 2.0, 6.0, VEC_VERTEX_CORNER, 0.0);

	/* The contours are appended after those out already had. */
	CHECK_LONG(vec_flatten(&view, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.contour_count, 4);
	CHECK_LONG(lines.vertex_count, 10);
	check_contour(&lines, 2, 5, 3, 1);
	CHECK_LONG(vec_lines_check(&lines), 0);
	vec_lines_release(&lines);
}

/*
 * The segment counts against the formula by hand:
 *   N = ceil(sqrt(n (n - 1) D / (8 tolerance))),
 *   D the largest length of P[i] - 2 P[i+1] + P[i+2].
 */
static void
test_counts(void)
{
	struct vec_path path;
	struct vec_lines lines;

	vec_lines_init(&lines);

	/*
	 * Quadratic (0,0) (5,10) (10,0), tolerance 0.1: the one second
	 * difference is (0,0) - (10,20) + (10,0) = (0,-20), D = 20, so
	 * N = ceil(sqrt(2 * 1 * 20 / 0.8)) = ceil(sqrt(50)) = ceil(7.07) = 8.
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_quad_to(&path, 5.0f, 10.0f, 10.0f, 0.0f), 0);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 1 + 8);
	vec_path_release(&path);

	/*
	 * Cubic (0,0) (0,10) (10,10) (10,0), tolerance 0.1: the second
	 * differences are (0,0) - (0,20) + (10,10) = (10,-10) and
	 * (0,10) - (20,20) + (10,0) = (-10,-10), both of length 14.142, so
	 * N = ceil(sqrt(3 * 2 * 14.142 / 0.8)) = ceil(sqrt(106.07)) = ceil(10.30) = 11.
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 1 + 11);
	check_contour(&lines, 0, 0, 12, 0);
	vec_path_release(&path);

	/*
	 * The same cubic at tolerance 1: sqrt(6 * 14.142 / 8) = sqrt(10.607)
	 * = 3.257, N = 4; at tolerance 100: sqrt(6 * 14.142 / 800) = 0.326,
	 * N = 1 (the segment count is at least 1).
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 1.0f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 1 + 4);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 100.0f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 1 + 1);
	vec_path_release(&path);

	/*
	 * The count is exact when the formula gives a whole number:
	 * quadratic (0,0) (0,0) (16,0), D = |(0,0) - (0,0) + (16,0)| = 16,
	 * tolerance 0.25: sqrt(2 * 16 / 2) = sqrt(16) = 4, so N = 4, not 5.
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_quad_to(&path, 0.0f, 0.0f, 16.0f, 0.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.25f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 1 + 4);
	vec_path_release(&path);

	/*
	 * Cubic with D from the second of two differences only: (0,0) (0,0)
	 * (0,0) (0,12): the first difference is (0,0), the second is
	 * (0,0) - (0,0) + (0,12) = (0,12), D = 12; tolerance 0.5:
	 * sqrt(6 * 12 / 4) = sqrt(18) = 4.24, N = 5.  A curve whose first
	 * difference only is large, (0,12) (0,0) (0,0) (0,0), gives the same.
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 12.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.5f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 1 + 5);
	vec_path_release(&path);
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 12.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.5f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 1 + 5);
	vec_path_release(&path);

	/* A curve that is a straight line with evenly spaced control points has D = 0 and one segment. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 1.0f, 1.0f, 2.0f, 2.0f, 3.0f, 3.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 2);
	check_vertex(&lines, 0, 0.0, 0.0, VEC_VERTEX_CORNER, 0.0);
	check_vertex(&lines, 1, 3.0, 3.0, VEC_VERTEX_CORNER, 0.0);
	vec_path_release(&path);

	/* A curve with every point equal is one zero-length segment, kept. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 4.0f, 4.0f), 0);
	CHECK_LONG(vec_path_quad_to(&path, 4.0f, 4.0f, 4.0f, 4.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 2);
	vec_path_release(&path);
	vec_lines_release(&lines);
}

/* The vertices of a curve lie on it at t = i / N, interior ones without the corner flag, the end exact. */
static void
test_on_curve(void)
{
	static const double qx[3] = { 0.0, 5.0, 10.0 };
	static const double qy[3] = { 0.0, 10.0, 0.0 };
	static const double cx[4] = { 0.0, 0.0, 10.0, 10.0 };
	static const double cy[4] = { 0.0, 10.0, 10.0, 0.0 };
	struct vec_path path;
	struct vec_lines lines;
	double x;
	double y;
	size_t i;

	vec_lines_init(&lines);

	/* The quadratic of the count test: 8 segments, vertex i is B(i / 8). */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_quad_to(&path, 5.0f, 10.0f, 10.0f, 0.0f), 0);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 9);
	for (i = 0; i < lines.vertex_count && i < 9; i++) {
		quad_at(qx, qy, (double)i / 8.0, &x, &y);
		check_vertex(&lines, i, x, y, (i == 0 || i == 8) ? VEC_VERTEX_CORNER : 0, 1e-5);
	}
	vec_path_release(&path);

	/* The cubic: 11 segments, vertex i is B(i / 11), the end exactly (10, 0). */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 12);
	for (i = 0; i < lines.vertex_count && i < 12; i++) {
		cubic_at(cx, cy, (double)i / 11.0, &x, &y);
		check_vertex(&lines, i, x, y, (i == 0 || i == 11) ? VEC_VERTEX_CORNER : 0, 1e-5);
	}
	check_vertex(&lines, 11, 10.0, 0.0, VEC_VERTEX_CORNER, 0.0);
	vec_path_release(&path);

	/* A curve between two lines: the lines' vertices and the curve's are in order with the right flags. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, -5.0f, 0.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 20.0f, 0.0f), 0);
	CHECK_LONG(vec_path_close(&path), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 2 + 11 + 1);
	check_contour(&lines, 0, 0, 14, 1);
	check_vertex(&lines, 0, -5.0, 0.0, VEC_VERTEX_CORNER, 0.0);
	check_vertex(&lines, 1, 0.0, 0.0, VEC_VERTEX_CORNER, 0.0);
	cubic_at(cx, cy, 1.0 / 11.0, &x, &y);
	check_vertex(&lines, 2, x, y, 0, 1e-5);
	cubic_at(cx, cy, 10.0 / 11.0, &x, &y);
	check_vertex(&lines, 11, x, y, 0, 1e-5);
	check_vertex(&lines, 12, 10.0, 0.0, VEC_VERTEX_CORNER, 0.0);
	check_vertex(&lines, 13, 20.0, 0.0, VEC_VERTEX_CORNER, 0.0);
	vec_path_release(&path);
	vec_lines_release(&lines);
}

/* The error bound: the greatest distance from the curve to the polyline is within the tolerance. */
static void
test_error_bound(void)
{
	static const double qx[3] = { 0.0, 5.0, 10.0 };
	static const double qy[3] = { 0.0, 10.0, 0.0 };
	static const double cx[4] = { 0.0, 0.0, 10.0, 10.0 };
	static const double cy[4] = { 0.0, 10.0, 10.0, 0.0 };
	static const double sx[4] = { 0.0, 30.0, -20.0, 10.0 };
	static const double sy[4] = { 0.0, 20.0, 20.0, 0.0 };
	static const float tolerances[4] = { 2.0f, 0.5f, 0.1f, 0.01f };
	struct vec_path path;
	struct vec_lines lines;
	double worst;
	size_t k;

	vec_lines_init(&lines);
	for (k = 0; k < 4; k++) {
		/* The quadratic. */
		vec_path_init(&path);
		CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
		CHECK_LONG(vec_path_quad_to(&path, 5.0f, 10.0f, 10.0f, 0.0f), 0);
		vec_lines_reset(&lines);
		CHECK_LONG(flatten_path(&path, NULL, tolerances[k], &lines), 0);
		worst = curve_error(&lines, qx, qy, 2);
		CHECK(worst <= (double)tolerances[k] + 1e-4);
		vec_path_release(&path);

		/* The arch cubic. */
		vec_path_init(&path);
		CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
		CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f), 0);
		vec_lines_reset(&lines);
		CHECK_LONG(flatten_path(&path, NULL, tolerances[k], &lines), 0);
		worst = curve_error(&lines, cx, cy, 3);
		CHECK(worst <= (double)tolerances[k] + 1e-4);
		vec_path_release(&path);

		/* A cubic with a loop-like overshoot. */
		vec_path_init(&path);
		CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
		CHECK_LONG(vec_path_cubic_to(&path, 30.0f, 20.0f, -20.0f, 20.0f, 10.0f, 0.0f), 0);
		vec_lines_reset(&lines);
		CHECK_LONG(flatten_path(&path, NULL, tolerances[k], &lines), 0);
		worst = curve_error(&lines, sx, sy, 3);
		CHECK(worst <= (double)tolerances[k] + 1e-4);
		vec_path_release(&path);
	}
	vec_lines_release(&lines);
}

/* The most segments of one curve, the smallest tolerance, and the refused tolerances. */
static void
test_segments_max_and_tolerance(void)
{
	struct vec_path path;
	struct vec_lines lines;
	struct vec_rect box;
	float tolerance;

	vec_lines_init(&lines);

	/* A curve that would need far more is cut into the most, VEC_FLATTEN_SEGMENTS_MAX, and still ends on its end point. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 1000000.0f, 1000000.0f, 1000000.0f, 1000000.0f, 0.0f), 0);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 1 + VEC_FLATTEN_SEGMENTS_MAX);
	check_vertex(&lines, VEC_FLATTEN_SEGMENTS_MAX, 1000000.0, 0.0, VEC_VERTEX_CORNER, 0.0);
	CHECK_LONG(vec_lines_check(&lines), 0);
	CHECK_LONG(vec_lines_bounds(&lines, &box), 0);
	CHECK(box.top >= 0.0f && box.bottom <= 1000000.0f);
	vec_path_release(&path);

	/* The smallest tolerance is accepted, and the count of a small curve stays at the most. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, VEC_TOLERANCE_MIN, &lines), 0);
	CHECK_LONG(lines.vertex_count, 1 + VEC_FLATTEN_SEGMENTS_MAX);

	/* A quadratic at the smallest tolerance with the largest coordinates is cut into the most too. */
	vec_path_release(&path);
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, -VEC_COORD_MAX, -VEC_COORD_MAX), 0);
	CHECK_LONG(vec_path_quad_to(&path, VEC_COORD_MAX, VEC_COORD_MAX, -VEC_COORD_MAX, VEC_COORD_MAX), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, NULL, VEC_TOLERANCE_MIN, &lines), 0);
	CHECK_LONG(lines.vertex_count, 1 + VEC_FLATTEN_SEGMENTS_MAX);
	CHECK_LONG(vec_lines_check(&lines), 0);

	/* A tolerance below the smallest, zero, negative, a NaN or an infinity is refused, and out stays empty. */
	vec_lines_reset(&lines);
	tolerance = VEC_TOLERANCE_MIN * 0.5f;
	CHECK_LONG(flatten_path(&path, NULL, tolerance, &lines), EINVAL);
	CHECK_LONG(flatten_path(&path, NULL, 0.0f, &lines), EINVAL);
	CHECK_LONG(flatten_path(&path, NULL, -1.0f, &lines), EINVAL);
	CHECK_LONG(flatten_path(&path, NULL, NAN, &lines), EINVAL);
	CHECK_LONG(flatten_path(&path, NULL, INFINITY, &lines), EINVAL);
	CHECK_LONG(flatten_path(&path, NULL, -INFINITY, &lines), EINVAL);
	CHECK_LONG(lines.vertex_count, 0);
	CHECK_LONG(lines.contour_count, 0);

	/* A huge tolerance is accepted and gives one segment. */
	CHECK_LONG(flatten_path(&path, NULL, 3.0e38f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 2);
	vec_path_release(&path);
	vec_lines_release(&lines);
}

/* A failed call puts out back: bad views, bad matrices, mapped points out of range, the vertex limit. */
static void
test_failures_roll_back(void)
{
	static const unsigned char not_move_verbs[] = { VEC_VERB_LINE, VEC_VERB_LINE };
	static const unsigned char move_line_verbs[] = { VEC_VERB_MOVE, VEC_VERB_LINE };
	static const struct vec_point two_points[] = { { 0.0f, 0.0f }, { 5.0f, 0.0f } };
	static const struct vec_point nan_points[] = { { 0.0f, 0.0f }, { NAN, 0.0f } };
	struct vec_path_view view;
	struct vec_matrix matrix;
	struct vec_path path;
	struct vec_lines lines;
	size_t i;
	int error;

	vec_lines_init(&lines);

	/* Out starts with one contour of two vertices, which every failure below keeps. */
	CHECK_LONG(vec_lines_begin(&lines, 1.0f, 1.0f, VEC_VERTEX_CORNER), 0);
	CHECK_LONG(vec_lines_add(&lines, 2.0f, 2.0f, VEC_VERTEX_CORNER), 0);

	/* A view that starts with a line, or whose point count is off, or that holds a NaN, is refused. */
	view.verbs = not_move_verbs;
	view.verb_count = 2;
	view.points = two_points;
	view.point_count = 2;
	CHECK_LONG(vec_flatten(&view, NULL, 0.1f, &lines), EINVAL);
	view.verbs = move_line_verbs;
	view.point_count = 1;
	CHECK_LONG(vec_flatten(&view, NULL, 0.1f, &lines), EINVAL);
	view.point_count = 2;
	view.points = nan_points;
	CHECK_LONG(vec_flatten(&view, NULL, 0.1f, &lines), EINVAL);
	CHECK_LONG(lines.contour_count, 1);
	CHECK_LONG(lines.vertex_count, 2);

	/* A matrix that is not valid is refused. */
	view.points = two_points;
	matrix.a = NAN;
	matrix.b = 0.0f;
	matrix.c = 0.0f;
	matrix.d = 1.0f;
	matrix.e = 0.0f;
	matrix.f = 0.0f;
	CHECK_LONG(vec_flatten(&view, &matrix, 0.1f, &lines), EINVAL);
	CHECK_LONG(lines.vertex_count, 2);

	/*
	 * A matrix whose product with a point is out of range: a = 1e7 is a
	 * valid entry, but (5, 0) maps to 5e7, beyond VEC_COORD_MAX.  The
	 * first point (0, 0) maps fine and has been appended when the
	 * second fails; out is put back to its two vertices.
	 */
	matrix.a = 10000000.0f;
	matrix.d = 1.0f;
	CHECK_LONG(vec_flatten(&view, &matrix, 0.1f, &lines), EINVAL);
	CHECK_LONG(lines.contour_count, 1);
	CHECK_LONG(lines.vertex_count, 2);
	check_vertex(&lines, 1, 2.0, 2.0, VEC_VERTEX_CORNER, 0.0);

	/* A mapped control point out of range in the middle of a curve fails the same way. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 0.5f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 1.0f, 1.0f, 1.0f, 1.0f, 2.0f, 0.0f), 0);
	CHECK_LONG(flatten_path(&path, &matrix, 0.1f, &lines), EINVAL);
	CHECK_LONG(lines.contour_count, 1);
	CHECK_LONG(lines.vertex_count, 2);
	vec_path_release(&path);

	/*
	 * The vertex limit in the middle of a curve: with room for five
	 * more vertices, M L then a cubic of 11 segments fails with
	 * ENOBUFS, and out is back at the vertices it held.
	 */
	vec_lines_reset(&lines);
	CHECK_LONG(vec_lines_begin(&lines, 0.0f, 0.0f, 0), 0);
	error = 0;
	for (i = 1; i < VEC_LINES_VERTICES_MAX - 5 && error == 0; i++)
		error = vec_lines_add(&lines, 1.0f, 1.0f, 0);
	CHECK_LONG(error, 0);
	CHECK_LONG(lines.vertex_count, VEC_LINES_VERTICES_MAX - 5);
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 1.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f), 0);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), ENOBUFS);
	CHECK_LONG(lines.vertex_count, VEC_LINES_VERTICES_MAX - 5);
	CHECK_LONG(lines.contour_count, 1);
	CHECK_LONG(lines.contours[0].count, VEC_LINES_VERTICES_MAX - 5);
	CHECK_LONG(vec_lines_check(&lines), 0);
	vec_path_release(&path);

	/* A path that fits into the room left still succeeds afterwards. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 1.0f, 0.0f), 0);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, VEC_LINES_VERTICES_MAX - 3);
	CHECK_LONG(lines.contour_count, 2);
	vec_path_release(&path);
	vec_lines_release(&lines);
}

/* Flattening under a matrix: more segments when scaled up, positions mapped, rotation and translation. */
static void
test_matrix(void)
{
	static const double cx[4] = { 0.0, 0.0, 10.0, 10.0 };
	static const double cy[4] = { 0.0, 10.0, 10.0, 0.0 };
	double sx[4];
	double sy[4];
	struct vec_matrix matrix;
	struct vec_matrix identity;
	struct vec_path path;
	struct vec_lines plain;
	struct vec_lines lines;
	double x;
	double y;
	size_t i;
	int k;

	vec_lines_init(&plain);
	vec_lines_init(&lines);
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f), 0);
	CHECK_LONG(flatten_path(&path, NULL, 0.1f, &plain), 0);

	/* The identity matrix gives the same vertices as no matrix. */
	vec_matrix_identity(&identity);
	CHECK_LONG(flatten_path(&path, &identity, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, plain.vertex_count);
	for (i = 0; i < lines.vertex_count && i < plain.vertex_count; i++)
		check_vertex(&lines, i, plain.vertices[i].x, plain.vertices[i].y, plain.vertices[i].flags, 0.0);

	/*
	 * A scale of 10: the second differences are 10 times as long,
	 * D = 141.42, N = ceil(sqrt(6 * 141.42 / 0.8)) = ceil(sqrt(1060.7))
	 * = ceil(32.57) = 33, and vertex i is 10 * B(i / 33).
	 */
	CHECK_LONG(vec_matrix_scale(&matrix, 10.0f, 10.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, &matrix, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 34);
	for (k = 0; k < 4; k++) {
		sx[k] = cx[k] * 10.0;
		sy[k] = cy[k] * 10.0;
	}
	for (i = 0; i < lines.vertex_count && i < 34; i++) {
		cubic_at(sx, sy, (double)i / 33.0, &x, &y);
		check_vertex(&lines, i, x, y, (i == 0 || i == 33) ? VEC_VERTEX_CORNER : 0, 1e-4);
	}

	/* A rotation by 90 degrees keeps the count (11) and maps (x, y) to (-y, x): the end (10, 0) goes to (0, 10). */
	CHECK_LONG(vec_matrix_rotate(&matrix, 90.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, &matrix, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 12);
	check_vertex(&lines, 11, 0.0, 10.0, VEC_VERTEX_CORNER, 1e-4);
	cubic_at(cx, cy, 5.0 / 11.0, &x, &y);
	check_vertex(&lines, 5, -y, x, 0, 1e-4);

	/* A translation keeps the count and offsets every vertex. */
	CHECK_LONG(vec_matrix_translate(&matrix, 5.0f, 7.0f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, &matrix, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 12);
	for (i = 0; i < lines.vertex_count && i < 12; i++)
		check_vertex(&lines, i, plain.vertices[i].x + 5.0, plain.vertices[i].y + 7.0, plain.vertices[i].flags, 1e-4);

	/* A shrink to a tenth gives fewer segments: D = 1.414, N = ceil(sqrt(6 * 1.414 / 0.8)) = ceil(3.26) = 4. */
	CHECK_LONG(vec_matrix_scale(&matrix, 0.1f, 0.1f), 0);
	vec_lines_reset(&lines);
	CHECK_LONG(flatten_path(&path, &matrix, 0.1f, &lines), 0);
	CHECK_LONG(lines.vertex_count, 5);
	vec_path_release(&path);
	vec_lines_release(&plain);
	vec_lines_release(&lines);
}

/* The in-place transform and its atomic refusal. */
static void
test_transform(void)
{
	struct vec_matrix matrix;
	struct vec_lines lines;

	vec_lines_init(&lines);
	CHECK_LONG(vec_lines_begin(&lines, 1.0f, 2.0f, VEC_VERTEX_CORNER), 0);
	CHECK_LONG(vec_lines_add(&lines, 3.0f, 4.0f, 0), 0);
	CHECK_LONG(vec_lines_begin(&lines, -1.0f, 5.0f, VEC_VERTEX_CORNER), 0);

	/* Scale then translate: x' = 2 x + 10, y' = -3 y + 1, flags and contours unchanged. */
	matrix.a = 2.0f;
	matrix.b = 0.0f;
	matrix.c = 0.0f;
	matrix.d = -3.0f;
	matrix.e = 10.0f;
	matrix.f = 1.0f;
	CHECK_LONG(vec_lines_transform(&lines, &matrix), 0);
	check_vertex(&lines, 0, 12.0, -5.0, VEC_VERTEX_CORNER, 0.0);
	check_vertex(&lines, 1, 16.0, -11.0, 0, 0.0);
	check_vertex(&lines, 2, 8.0, -14.0, VEC_VERTEX_CORNER, 0.0);
	check_contour(&lines, 0, 0, 2, 0);
	check_contour(&lines, 1, 2, 1, 0);

	/* A skew uses the off-diagonal entries: x' = x + y, y' = y; (12, -5) becomes (7, -5), (16, -11) becomes (5, -11). */
	matrix.a = 1.0f;
	matrix.b = 0.0f;
	matrix.c = 1.0f;
	matrix.d = 1.0f;
	matrix.e = 0.0f;
	matrix.f = 0.0f;
	CHECK_LONG(vec_lines_transform(&lines, &matrix), 0);
	check_vertex(&lines, 0, 7.0, -5.0, VEC_VERTEX_CORNER, 0.0);
	check_vertex(&lines, 1, 5.0, -11.0, 0, 0.0);
	check_vertex(&lines, 2, -6.0, -14.0, VEC_VERTEX_CORNER, 0.0);

	/* A matrix that is not valid is refused and nothing changes. */
	matrix.a = NAN;
	CHECK_LONG(vec_lines_transform(&lines, &matrix), EINVAL);
	check_vertex(&lines, 0, 7.0, -5.0, VEC_VERTEX_CORNER, 0.0);

	/* A matrix that carries the last vertex out of range is refused before the first is stored. */
	matrix.a = 1.0e6f;
	matrix.b = 0.0f;
	matrix.c = 0.0f;
	matrix.d = 1.0f;
	matrix.e = 0.0f;
	matrix.f = 0.0f;
	lines.vertices[0].x = 1.0f;
	lines.vertices[1].x = 1.0f;
	lines.vertices[2].x = 100.0f;
	CHECK_LONG(vec_lines_transform(&lines, &matrix), EINVAL);
	check_vertex(&lines, 0, 1.0, -5.0, VEC_VERTEX_CORNER, 0.0);
	check_vertex(&lines, 1, 1.0, -11.0, 0, 0.0);
	check_vertex(&lines, 2, 100.0, -14.0, VEC_VERTEX_CORNER, 0.0);

	/* Lines with no vertex transform to nothing and succeed. */
	vec_lines_reset(&lines);
	CHECK_LONG(vec_lines_transform(&lines, &matrix), 0);
	vec_lines_release(&lines);
}

/* The bounding box: the extremes of the vertices, and the refusals that leave *bounds alone. */
static void
test_bounds(void)
{
	struct vec_vertex vertices[2];
	struct vec_contour contours[1];
	struct vec_rect rect;
	struct vec_lines lines;

	vec_lines_init(&lines);

	/* Lines with no vertex have no box: EINVAL, and the rectangle is not written. */
	rect.left = 11.0f;
	rect.top = 12.0f;
	rect.right = 13.0f;
	rect.bottom = 14.0f;
	CHECK_LONG(vec_lines_bounds(&lines, &rect), EINVAL);
	CHECK_NEAR(rect.left, 11.0, 0.0);
	CHECK_NEAR(rect.bottom, 14.0, 0.0);

	/* One vertex gives the zero size rectangle at it. */
	CHECK_LONG(vec_lines_begin(&lines, -3.0f, 4.0f, 0), 0);
	CHECK_LONG(vec_lines_bounds(&lines, &rect), 0);
	CHECK_NEAR(rect.left, -3.0, 0.0);
	CHECK_NEAR(rect.right, -3.0, 0.0);
	CHECK_NEAR(rect.top, 4.0, 0.0);
	CHECK_NEAR(rect.bottom, 4.0, 0.0);

	/* Several vertices of two contours give the extremes of each axis. */
	CHECK_LONG(vec_lines_add(&lines, 6.0f, -2.0f, 0), 0);
	CHECK_LONG(vec_lines_begin(&lines, 1.0f, 9.0f, 0), 0);
	CHECK_LONG(vec_lines_add(&lines, -8.0f, 0.5f, 0), 0);
	CHECK_LONG(vec_lines_bounds(&lines, &rect), 0);
	CHECK_NEAR(rect.left, -8.0, 0.0);
	CHECK_NEAR(rect.top, -2.0, 0.0);
	CHECK_NEAR(rect.right, 6.0, 0.0);
	CHECK_NEAR(rect.bottom, 9.0, 0.0);
	vec_lines_release(&lines);

	/* Malformed lines are refused and the rectangle is not written. */
	vertices[0].x = 0.0f;
	vertices[0].y = 0.0f;
	vertices[0].flags = 0;
	vertices[1].x = 1.0f;
	vertices[1].y = 1.0f;
	vertices[1].flags = 0;
	contours[0].first = 1;
	contours[0].count = 2;
	contours[0].closed = 0;
	memset(&lines, 0, sizeof(lines));
	lines.vertices = vertices;
	lines.vertex_count = 2;
	lines.contours = contours;
	lines.contour_count = 1;
	rect.left = 11.0f;
	CHECK_LONG(vec_lines_bounds(&lines, &rect), EINVAL);
	CHECK_NEAR(rect.left, 11.0, 0.0);
	contours[0].first = 0;
	CHECK_LONG(vec_lines_bounds(&lines, &rect), 0);
	CHECK_NEAR(rect.right, 1.0, 0.0);
}

/* A random run under the sanitizers: random paths and matrices, and random bytes as a view. */
static void
test_random(void)
{
	unsigned char verbs[24];
	struct vec_point points[48];
	struct vec_path_view view;
	struct vec_matrix matrix;
	struct vec_matrix turn;
	struct vec_path path;
	struct vec_lines lines;
	struct vec_rect bounds;
	uint32_t seed;
	size_t before_vertices;
	size_t before_contours;
	size_t moves;
	size_t drawn;
	size_t k;
	size_t n;
	unsigned pick;
	float tolerance;
	int error;

	seed = 0x2468ace1U;
	vec_lines_init(&lines);

	/* Random paths built by the builder, flattened with a random matrix and tolerance. */
	for (n = 0; n < 600; n++) {
		vec_path_init(&path);
		CHECK_LONG(vec_path_move_to(&path, check_random_float(&seed, -100.0f, 100.0f),
		    check_random_float(&seed, -100.0f, 100.0f)), 0);
		for (k = 0; k < 12; k++) {
			pick = check_random(&seed) % 6U;
			if (pick == 0) {
				CHECK_LONG(vec_path_line_to(&path, check_random_float(&seed, -100.0f, 100.0f),
				    check_random_float(&seed, -100.0f, 100.0f)), 0);
			} else if (pick == 1) {
				CHECK_LONG(vec_path_quad_to(&path, check_random_float(&seed, -100.0f, 100.0f),
				    check_random_float(&seed, -100.0f, 100.0f), check_random_float(&seed, -100.0f, 100.0f),
				    check_random_float(&seed, -100.0f, 100.0f)), 0);
			} else if (pick == 2) {
				CHECK_LONG(vec_path_cubic_to(&path, check_random_float(&seed, -100.0f, 100.0f),
				    check_random_float(&seed, -100.0f, 100.0f), check_random_float(&seed, -100.0f, 100.0f),
				    check_random_float(&seed, -100.0f, 100.0f), check_random_float(&seed, -100.0f, 100.0f),
				    check_random_float(&seed, -100.0f, 100.0f)), 0);
			} else if (pick == 3) {
				CHECK_LONG(vec_path_close(&path), 0);
			} else if (pick == 4) {
				CHECK_LONG(vec_path_move_to(&path, check_random_float(&seed, -100.0f, 100.0f),
				    check_random_float(&seed, -100.0f, 100.0f)), 0);
			} else {
				CHECK_LONG(vec_path_close(&path), 0);
				CHECK_LONG(vec_path_line_to(&path, check_random_float(&seed, -100.0f, 100.0f),
				    check_random_float(&seed, -100.0f, 100.0f)), 0);
			}
		}

		/* Counts the subpaths (each move starts one contour) and the commands that put a vertex. */
		moves = 0;
		drawn = 0;
		for (k = 0; k < path.verb_count; k++) {
			if (path.verbs[k] == VEC_VERB_MOVE)
				moves++;
			if (path.verbs[k] != VEC_VERB_CLOSE)
				drawn++;
		}

		/* Makes a random matrix (a scale and a turn) and a random tolerance. */
		CHECK_LONG(vec_matrix_scale(&matrix, check_random_float(&seed, 0.1f, 5.0f),
		    check_random_float(&seed, 0.1f, 5.0f)), 0);
		CHECK_LONG(vec_matrix_rotate(&turn, check_random_float(&seed, -180.0f, 180.0f)), 0);
		vec_matrix_multiply(&matrix, &matrix, &turn);
		tolerance = check_random_float(&seed, 0.002f, 5.0f);

		/* The flattening succeeds, is well formed and has one contour per subpath. */
		vec_lines_reset(&lines);
		error = flatten_path(&path, &matrix, tolerance, &lines);
		CHECK_LONG(error, 0);
		CHECK_LONG(vec_lines_check(&lines), 0);
		CHECK_LONG(lines.contour_count, moves);
		CHECK(lines.vertex_count >= drawn);
		if (lines.vertex_count > 0)
			CHECK_LONG(vec_lines_bounds(&lines, &bounds), 0);
		vec_path_release(&path);
	}
	vec_lines_release(&lines);

	/* Random bytes as a view, with NaN and huge points mixed in: the result is 0 or EINVAL and out stays consistent. */
	vec_lines_init(&lines);
	for (n = 0; n < 20000; n++) {
		for (k = 0; k < 24; k++)
			verbs[k] = (unsigned char)(check_random(&seed) % 7U);
		for (k = 0; k < 48; k++) {
			points[k].x = check_random_float(&seed, -50.0f, 50.0f);
			points[k].y = check_random_float(&seed, -50.0f, 50.0f);
			pick = check_random(&seed) % 64U;
			if (pick == 0)
				points[k].x = NAN;
			if (pick == 1)
				points[k].y = VEC_COORD_MAX * 4.0f;
		}
		if ((check_random(&seed) & 3U) != 0)
			verbs[0] = (unsigned char)VEC_VERB_MOVE;
		view.verbs = verbs;
		view.verb_count = check_random(&seed) % 25U;
		view.points = points;
		view.point_count = check_random(&seed) % 49U;
		before_vertices = lines.vertex_count;
		before_contours = lines.contour_count;
		error = vec_flatten(&view, NULL, check_random_float(&seed, 0.01f, 2.0f), &lines);
		CHECK(error == 0 || error == EINVAL);
		if (error != 0) {
			CHECK_LONG(lines.vertex_count, before_vertices);
			CHECK_LONG(lines.contour_count, before_contours);
		}
		CHECK_LONG(vec_lines_check(&lines), 0);
		if (lines.vertex_count > 4000)
			vec_lines_reset(&lines);
	}
	vec_lines_release(&lines);
}

int
main(void)
{
	/* Runs every group of checks. */
	test_building();
	test_limits_of_lines();
	test_check();
	test_straight();
	test_counts();
	test_on_curve();
	test_error_bound();
	test_segments_max_and_tolerance();
	test_failures_roll_back();
	test_matrix();
	test_transform();
	test_bounds();
	test_random();

	/* Reports the result. */
	return check_finish("flatten");
}
