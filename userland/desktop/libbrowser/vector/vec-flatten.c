/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Paths flattened into polylines within a tolerance.
 */

#include "vector/vec-flatten.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The initial size of the vertex and contour arrays, in elements, before they double. */
#define LINES_CAPACITY_FIRST	16

/*
 * The most control points a curve has (a cubic Bezier curve: two end
 * points and two control points).
 */
#define FLATTEN_CONTROL_MAX	4

/*
 * The degrees of the curves the flattener cuts, which are the n of the
 * segment count N = ceil(sqrt(n * (n - 1) * D / (8 * tolerance))) of
 * vec-flatten.h.
 *
 * Values referenced: SVG 2 §9.3.6 (a cubic Bezier curve has degree 3) and
 * §9.3.7 (a quadratic one has degree 2).
 */
#define FLATTEN_DEGREE_QUAD	2
#define FLATTEN_DEGREE_CUBIC	3

/* The forward declarations of the static functions. */
static int lines_reserve_vertices(struct vec_lines *lines, size_t needed);
static int lines_reserve_contours(struct vec_lines *lines, size_t needed);
static int flatten_read_point(const struct vec_path_view *path, size_t index, const struct vec_matrix *matrix, struct vec_point *mapped);
static size_t flatten_segment_count(const struct vec_point *control, int degree, float tolerance);
static float flatten_lerp(float from, float to, float t);
static struct vec_point flatten_curve_at(const struct vec_point *control, int degree, float t);
static int flatten_curve(struct vec_lines *out, const struct vec_point *control, int degree, float tolerance);
static int flatten_walk(const struct vec_path_view *path, const struct vec_matrix *matrix, float tolerance, struct vec_lines *out);

/*
 * Makes empty polylines.
 */
void
vec_lines_init(
	struct vec_lines *lines)
{
	/* Zeroes the structure, which is the empty polylines with no memory. */
	memset(lines, 0, sizeof(*lines));
}

/*
 * Frees the arrays and leaves the polylines empty.
 */
void
vec_lines_release(
	struct vec_lines *lines)
{
	/* Gives back the vertex array and the contour array. */
	free(lines->vertices);
	free(lines->contours);

	/* Leaves the structure as vec_lines_init does. */
	memset(lines, 0, sizeof(*lines));
}

/*
 * Empties the polylines but keeps their arrays for reuse.
 */
void
vec_lines_reset(
	struct vec_lines *lines)
{
	/* Forgets the vertices and contours while the arrays stay allocated. */
	lines->vertex_count = 0;
	lines->contour_count = 0;
}

/*
 * Starts a new contour at a vertex.
 *
 * The point is checked first, then the limits, then the arrays are made
 * large enough, so that a failure leaves the counts as they were.
 */
int
vec_lines_begin(
	struct vec_lines *lines,
	float x,
	float y,
	unsigned flags)
{
	struct vec_point point;
	struct vec_vertex *vertex;
	struct vec_contour *contour;
	int valid;
	int error;

	/* Refuses a point that is not a usable coordinate. */
	point.x = x;
	point.y = y;
	valid = vec_point_valid(point);
	if (!valid)
		return EINVAL;

	/* Refuses a vertex or a contour beyond the limits of one structure. */
	if (lines->vertex_count >= VEC_LINES_VERTICES_MAX)
		return ENOBUFS;
	if (lines->contour_count >= VEC_LINES_CONTOURS_MAX)
		return ENOBUFS;

	/* Makes room for one more vertex. */
	error = lines_reserve_vertices(lines, lines->vertex_count + 1);
	if (error != 0)
		return error;

	/* Makes room for one more contour. */
	error = lines_reserve_contours(lines, lines->contour_count + 1);
	if (error != 0)
		return error;

	/* Stores the vertex at the end of the vertex array. */
	vertex = &lines->vertices[lines->vertex_count];
	vertex->x = x;
	vertex->y = y;
	vertex->flags = flags;

	/* Stores the contour that holds that one vertex, open until a close says otherwise. */
	contour = &lines->contours[lines->contour_count];
	contour->first = lines->vertex_count;
	contour->count = 1;
	contour->closed = 0;

	/* Publishes both by counting them. */
	lines->vertex_count++;
	lines->contour_count++;

	/* Succeeded: a new contour holds the vertex. */
	return 0;
}

/*
 * Appends a vertex to the last contour.
 *
 * A structure with no contour begins one.  The last contour has to end at
 * the end of the vertex array, as the building steps leave it; a structure
 * where it does not is refused instead of having a vertex slipped into the
 * middle of another contour.
 */
int
vec_lines_add(
	struct vec_lines *lines,
	float x,
	float y,
	unsigned flags)
{
	struct vec_point point;
	struct vec_vertex *vertex;
	struct vec_contour *last;
	int valid;
	int error;

	/* Begins a contour when there is none to extend. */
	if (lines->contour_count == 0) {
		error = vec_lines_begin(lines, x, y, flags);
		if (error != 0)
			return error;

		/* Succeeded: the vertex began the first contour. */
		return 0;
	}

	/* Refuses a last contour that does not end at the end of the vertex array. */
	last = &lines->contours[lines->contour_count - 1];
	if (last->first > lines->vertex_count)
		return EINVAL;
	if (last->count != lines->vertex_count - last->first)
		return EINVAL;

	/* Refuses a point that is not a usable coordinate. */
	point.x = x;
	point.y = y;
	valid = vec_point_valid(point);
	if (!valid)
		return EINVAL;

	/* Refuses a vertex beyond the limit of one structure. */
	if (lines->vertex_count >= VEC_LINES_VERTICES_MAX)
		return ENOBUFS;

	/* Makes room for one more vertex. */
	error = lines_reserve_vertices(lines, lines->vertex_count + 1);
	if (error != 0)
		return error;

	/* Stores the vertex at the end of the vertex array. */
	vertex = &lines->vertices[lines->vertex_count];
	vertex->x = x;
	vertex->y = y;
	vertex->flags = flags;

	/* Publishes the vertex by counting it into the last contour. */
	lines->vertex_count++;
	last->count++;

	/* Succeeded: the last contour has one more vertex. */
	return 0;
}

/*
 * Marks the last contour closed.
 */
void
vec_lines_close(
	struct vec_lines *lines)
{
	/* Does nothing when there is no contour to close. */
	if (lines->contour_count == 0)
		return;

	/* Marks the last contour closed; no vertex is added, the closing segment is implied. */
	lines->contours[lines->contour_count - 1].closed = 1;
}

/*
 * Checks polylines.
 *
 * The counts are bounded by the limits first, which bounds the work of the
 * loops, and a contour is tested against the vertex array without adding
 * its first vertex to its count, which could overflow.
 */
int
vec_lines_check(
	const struct vec_lines *lines)
{
	const struct vec_contour *contour;
	struct vec_point point;
	size_t i;
	int valid;

	/* Refuses counts beyond the limits of one structure. */
	if (lines->vertex_count > VEC_LINES_VERTICES_MAX)
		return EINVAL;
	if (lines->contour_count > VEC_LINES_CONTOURS_MAX)
		return EINVAL;

	/* Refuses counts that name arrays the structure does not have. */
	if (lines->vertex_count != 0 && lines->vertices == NULL)
		return EINVAL;
	if (lines->contour_count != 0 && lines->contours == NULL)
		return EINVAL;

	/* Checks that every contour is not empty and lies inside the vertex array. */
	for (i = 0;
	     i < lines->contour_count;
	     i++) {
		contour = &lines->contours[i];
		if (contour->count < 1)
			return EINVAL;
		if (contour->first > lines->vertex_count)
			return EINVAL;
		if (contour->count > lines->vertex_count - contour->first)
			return EINVAL;
	}

	/* Checks that every vertex is a usable coordinate. */
	for (i = 0;
	     i < lines->vertex_count;
	     i++) {
		point.x = lines->vertices[i].x;
		point.y = lines->vertices[i].y;
		valid = vec_point_valid(point);
		if (!valid)
			return EINVAL;
	}

	/* Succeeded: the polylines are well formed. */
	return 0;
}

/*
 * Flattens a path into polylines appended to out.
 *
 * The inputs are checked before anything is appended.  The walk then
 * appends contours; when it fails, the counts of out are put back to what
 * they were, which drops everything the call added (the arrays may stay
 * larger).
 */
int
vec_flatten(
	const struct vec_path_view *path,
	const struct vec_matrix *matrix,
	float tolerance,
	struct vec_lines *out)
{
	size_t vertices_before;
	size_t contours_before;
	int valid;
	int error;

	/* Refuses a view that is malformed or holds a point that is not valid. */
	error = vec_path_view_check(path);
	if (error != 0)
		return error;

	/* Refuses a matrix that is not valid, when one is given. */
	if (matrix != NULL) {
		valid = vec_matrix_valid(matrix);
		if (!valid)
			return EINVAL;
	}

	/* Refuses a tolerance that is not finite or is below the smallest accepted. */
	valid = vec_finite(tolerance);
	if (!valid)
		return EINVAL;
	if (tolerance < VEC_TOLERANCE_MIN)
		return EINVAL;

	/* Remembers how much out held, to put it back on failure. */
	vertices_before = out->vertex_count;
	contours_before = out->contour_count;

	/* Walks the commands, appending the contours. */
	error = flatten_walk(path, matrix, tolerance, out);
	if (error != 0) {
		out->vertex_count = vertices_before;
		out->contour_count = contours_before;
		return error;
	}

	/* Succeeded: out has the new contours after those it had. */
	return 0;
}

/*
 * Applies a matrix to every vertex of polylines in place.
 *
 * Every mapped vertex is checked before any is stored, so that a failure
 * leaves the lines as they were.
 */
int
vec_lines_transform(
	struct vec_lines *lines,
	const struct vec_matrix *matrix)
{
	struct vec_point point;
	struct vec_point mapped;
	size_t i;
	int valid;

	/* Refuses a matrix that is not valid. */
	valid = vec_matrix_valid(matrix);
	if (!valid)
		return EINVAL;

	/* Refuses a vertex count that is beyond the limit or names no array. */
	if (lines->vertex_count > VEC_LINES_VERTICES_MAX)
		return EINVAL;
	if (lines->vertex_count != 0 && lines->vertices == NULL)
		return EINVAL;

	/* Checks that every vertex maps to a usable coordinate, changing nothing yet. */
	for (i = 0;
	     i < lines->vertex_count;
	     i++) {
		point.x = lines->vertices[i].x;
		point.y = lines->vertices[i].y;
		mapped = vec_matrix_apply(matrix, point);
		valid = vec_point_valid(mapped);
		if (!valid)
			return EINVAL;
	}

	/* Stores the mapped position of every vertex; the flags stay. */
	for (i = 0;
	     i < lines->vertex_count;
	     i++) {
		point.x = lines->vertices[i].x;
		point.y = lines->vertices[i].y;
		mapped = vec_matrix_apply(matrix, point);
		lines->vertices[i].x = mapped.x;
		lines->vertices[i].y = mapped.y;
	}

	/* Succeeded: every vertex is mapped. */
	return 0;
}

/*
 * Computes the bounding box of the vertices of polylines.
 *
 * The box covers every vertex of the vertex array, so a vertex that no
 * contour names still counts.
 */
int
vec_lines_bounds(
	const struct vec_lines *lines,
	struct vec_rect *bounds)
{
	struct vec_rect rect;
	struct vec_point point;
	size_t i;
	int error;

	/* Refuses lines that are malformed. */
	error = vec_lines_check(lines);
	if (error != 0)
		return error;

	/* Refuses lines with no vertex, which have no box. */
	if (lines->vertex_count == 0)
		return EINVAL;

	/* Grows a rectangle over every vertex. */
	vec_rect_empty(&rect);
	for (i = 0;
	     i < lines->vertex_count;
	     i++) {
		point.x = lines->vertices[i].x;
		point.y = lines->vertices[i].y;
		vec_rect_include(&rect, point);
	}

	/* Publishes the rectangle. */
	*bounds = rect;

	/* Succeeded: *bounds is the bounding box. */
	return 0;
}

/*
 * Makes the vertex array hold at least the given number of vertices.
 *
 * The array doubles from a first size, and is never made larger than the
 * limit of one structure; the caller has already refused a count beyond it.
 */
static int
lines_reserve_vertices(
	struct vec_lines *lines,
	size_t needed)
{
	struct vec_vertex *grown;
	size_t capacity;

	/* Keeps the array when it is already large enough. */
	if (needed <= lines->vertex_capacity)
		return 0;

	/* Doubles the capacity until it holds the needed count, but not past the limit. */
	capacity = lines->vertex_capacity;
	if (capacity < LINES_CAPACITY_FIRST)
		capacity = LINES_CAPACITY_FIRST;
	while (capacity < needed)
		capacity *= 2;
	if (capacity > VEC_LINES_VERTICES_MAX)
		capacity = VEC_LINES_VERTICES_MAX;

	/* Grows the array; the old one stays valid when this fails. */
	grown = realloc(lines->vertices, capacity * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;

	/* Publishes the larger array. */
	lines->vertices = grown;
	lines->vertex_capacity = capacity;

	/* Succeeded: the array holds the needed vertices. */
	return 0;
}

/*
 * Makes the contour array hold at least the given number of contours.
 */
static int
lines_reserve_contours(
	struct vec_lines *lines,
	size_t needed)
{
	struct vec_contour *grown;
	size_t capacity;

	/* Keeps the array when it is already large enough. */
	if (needed <= lines->contour_capacity)
		return 0;

	/* Doubles the capacity until it holds the needed count, but not past the limit. */
	capacity = lines->contour_capacity;
	if (capacity < LINES_CAPACITY_FIRST)
		capacity = LINES_CAPACITY_FIRST;
	while (capacity < needed)
		capacity *= 2;
	if (capacity > VEC_LINES_CONTOURS_MAX)
		capacity = VEC_LINES_CONTOURS_MAX;

	/* Grows the array; the old one stays valid when this fails. */
	grown = realloc(lines->contours, capacity * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;

	/* Publishes the larger array. */
	lines->contours = grown;
	lines->contour_capacity = capacity;

	/* Succeeded: the array holds the needed contours. */
	return 0;
}

/*
 * Reads a point of the view and maps it by the matrix.
 *
 * The mapped point has to be a usable coordinate: a matrix that scales a
 * valid point beyond the range is refused here.
 */
static int
flatten_read_point(
	const struct vec_path_view *path,
	size_t index,
	const struct vec_matrix *matrix,
	struct vec_point *mapped)
{
	struct vec_point point;
	int valid;

	/* Maps the point, or takes it as it is when there is no matrix. */
	point = path->points[index];
	if (matrix != NULL)
		point = vec_matrix_apply(matrix, point);

	/* Refuses a mapped point that is not a usable coordinate. */
	valid = vec_point_valid(point);
	if (!valid)
		return EINVAL;

	/* Publishes the mapped point. */
	*mapped = point;

	/* Succeeded: *mapped is the mapped point. */
	return 0;
}

/*
 * Computes how many segments a curve is cut into.
 *
 * N = ceil(sqrt(n * (n - 1) * D / (8 * tolerance))), the formula of
 * vec-flatten.h, kept between 1 and VEC_FLATTEN_SEGMENTS_MAX.  The
 * comparison with the limit is made on the float before it is converted,
 * as converting a float beyond the range of the integer is undefined.
 */
static size_t
flatten_segment_count(
	const struct vec_point *control,
	int degree,
	float tolerance)
{
	float largest;
	float dx;
	float dy;
	float length;
	float divisor;
	float ratio;
	float steps;
	size_t count;
	int finite;
	int i;

	/* Finds D, the largest length of the second differences P[i] - 2 P[i + 1] + P[i + 2]. */
	largest = 0.0f;
	for (i = 0;
	     i < degree - 1;
	     i++) {
		dx = control[i].x - 2.0f * control[i + 1].x + control[i + 2].x;
		dy = control[i].y - 2.0f * control[i + 1].y + control[i + 2].y;
		length = sqrtf(dx * dx + dy * dy);
		if (length > largest)
			largest = length;
	}

	/* Takes one segment when the divisor overflows, which a huge tolerance makes. */
	divisor = 8.0f * tolerance;
	finite = vec_finite(divisor);
	if (!finite)
		return 1;

	/* Computes n * (n - 1) * D / (8 * tolerance) and its square root. */
	ratio = (float)(degree * (degree - 1)) * largest / divisor;
	steps = sqrtf(ratio);

	/* Takes the most segments when the count is beyond the limit or not a number. */
	if (!(steps < (float)VEC_FLATTEN_SEGMENTS_MAX))
		return VEC_FLATTEN_SEGMENTS_MAX;

	/* Rounds up to a whole number of segments, at least one. */
	count = (size_t)ceilf(steps);
	if (count < 1)
		count = 1;
	if (count > VEC_FLATTEN_SEGMENTS_MAX)
		count = VEC_FLATTEN_SEGMENTS_MAX;

	/* Reports the number of segments. */
	return count;
}

/*
 * Interpolates between two numbers: from at t = 0, to at t = 1.
 */
static float
flatten_lerp(
	float from,
	float to,
	float t)
{
	/* Reports the number a fraction t of the way from the first to the second. */
	return from + (to - from) * t;
}

/*
 * Evaluates a Bezier curve at a parameter by repeated interpolation of
 * its control points (de Casteljau's construction, equal to the Bernstein
 * form of SVG 2 §9.3.6 and §9.3.7).
 *
 * The result is kept inside the box of the control points: the curve lies
 * there exactly, and rounding must not carry a point past the range the
 * control points were checked to be in.
 */
static struct vec_point
flatten_curve_at(
	const struct vec_point *control,
	int degree,
	float t)
{
	struct vec_point work[FLATTEN_CONTROL_MAX];
	struct vec_point result;
	float low_x;
	float high_x;
	float low_y;
	float high_y;
	int level;
	int j;

	/* Copies the control points, which the interpolation overwrites. */
	for (j = 0;
	     j <= degree;
	     j++)
		work[j] = control[j];

	/* Interpolates neighbours level by level until one point is left. */
	for (level = 1;
	     level <= degree;
	     level++) {
		for (j = 0;
		     j <= degree - level;
		     j++) {
			work[j].x = flatten_lerp(work[j].x, work[j + 1].x, t);
			work[j].y = flatten_lerp(work[j].y, work[j + 1].y, t);
		}
	}
	result = work[0];

	/* Finds the box of the control points. */
	low_x = control[0].x;
	high_x = control[0].x;
	low_y = control[0].y;
	high_y = control[0].y;
	for (j = 1;
	     j <= degree;
	     j++) {
		if (control[j].x < low_x)
			low_x = control[j].x;
		if (control[j].x > high_x)
			high_x = control[j].x;
		if (control[j].y < low_y)
			low_y = control[j].y;
		if (control[j].y > high_y)
			high_y = control[j].y;
	}

	/* Keeps the result inside that box. */
	if (result.x < low_x)
		result.x = low_x;
	if (result.x > high_x)
		result.x = high_x;
	if (result.y < low_y)
		result.y = low_y;
	if (result.y > high_y)
		result.y = high_y;

	/* Reports the point of the curve. */
	return result;
}

/*
 * Appends the vertices of one curve to the last contour: the points at
 * t = i / N inside the curve without the corner flag, and the end point
 * with it.
 *
 * The end point is the control point itself, not an evaluation, so the
 * curve ends exactly where the next command starts.  The start point is
 * already the last vertex of the contour.
 */
static int
flatten_curve(
	struct vec_lines *out,
	const struct vec_point *control,
	int degree,
	float tolerance)
{
	struct vec_point point;
	size_t count;
	size_t i;
	float t;
	int error;

	/* Decides how many segments the curve is cut into. */
	count = flatten_segment_count(control, degree, tolerance);

	/* Appends the points inside the curve, which end no segment of the original path. */
	for (i = 1;
	     i < count;
	     i++) {
		t = (float)i / (float)count;
		point = flatten_curve_at(control, degree, t);
		error = vec_lines_add(out, point.x, point.y, 0);
		if (error != 0)
			return error;
	}

	/* Appends the end point, where the original path's segment ends. */
	point = control[degree];
	error = vec_lines_add(out, point.x, point.y, VEC_VERTEX_CORNER);
	if (error != 0)
		return error;

	/* Succeeded: the curve is appended. */
	return 0;
}

/*
 * Walks the commands of a checked view and appends their contours.
 *
 * A subpath begins at a move.  A drawing command that follows a close
 * without a move (a view built by hand may have one) begins a new
 * contour at the closed subpath's first point, as SVG 2 §9.3.4 says.
 */
static int
flatten_walk(
	const struct vec_path_view *path,
	const struct vec_matrix *matrix,
	float tolerance,
	struct vec_lines *out)
{
	struct vec_point control[FLATTEN_CONTROL_MAX];
	struct vec_point current;
	struct vec_point start;
	size_t point_index;
	size_t i;
	int after_close;
	int error;

	/* Starts with no current point and no close pending. */
	current.x = 0.0f;
	current.y = 0.0f;
	start = current;
	point_index = 0;
	after_close = 0;

	/* Appends the contours of the commands in order. */
	for (i = 0;
	     i < path->verb_count;
	     i++) {
		/* A drawing command after a close starts a new contour at the closed subpath's first point. */
		if (after_close &&
		    path->verbs[i] != (unsigned char)VEC_VERB_MOVE &&
		    path->verbs[i] != (unsigned char)VEC_VERB_CLOSE) {
			error = vec_lines_begin(out, start.x, start.y, VEC_VERTEX_CORNER);
			if (error != 0)
				return error;
			after_close = 0;
		}

		/* Appends what this command draws. */
		switch (path->verbs[i]) {
		case VEC_VERB_MOVE:
			/* A move starts a contour at its point. */
			error = flatten_read_point(path, point_index, matrix, &current);
			if (error != 0)
				return error;
			start = current;
			error = vec_lines_begin(out, current.x, current.y, VEC_VERTEX_CORNER);
			if (error != 0)
				return error;
			point_index++;
			after_close = 0;
			break;
		case VEC_VERB_LINE:
			/* A line adds one vertex, the end of the segment. */
			error = flatten_read_point(path, point_index, matrix, &current);
			if (error != 0)
				return error;
			error = vec_lines_add(out, current.x, current.y, VEC_VERTEX_CORNER);
			if (error != 0)
				return error;
			point_index++;
			break;
		case VEC_VERB_QUAD:
			/* A quadratic curve is cut into segments between its mapped control points. */
			control[0] = current;
			error = flatten_read_point(path, point_index, matrix, &control[1]);
			if (error != 0)
				return error;
			error = flatten_read_point(path, point_index + 1, matrix, &control[2]);
			if (error != 0)
				return error;
			error = flatten_curve(out, control, FLATTEN_DEGREE_QUAD, tolerance);
			if (error != 0)
				return error;
			current = control[2];
			point_index += 2;
			break;
		case VEC_VERB_CUBIC:
			/* A cubic curve is cut into segments between its mapped control points. */
			control[0] = current;
			error = flatten_read_point(path, point_index, matrix, &control[1]);
			if (error != 0)
				return error;
			error = flatten_read_point(path, point_index + 1, matrix, &control[2]);
			if (error != 0)
				return error;
			error = flatten_read_point(path, point_index + 2, matrix, &control[3]);
			if (error != 0)
				return error;
			error = flatten_curve(out, control, FLATTEN_DEGREE_CUBIC, tolerance);
			if (error != 0)
				return error;
			current = control[3];
			point_index += 3;
			break;
		case VEC_VERB_CLOSE:
			/* A close marks the contour closed; the closing segment is implied, no vertex is added. */
			vec_lines_close(out);
			current = start;
			after_close = 1;
			break;
		default:
			/* The view check refused every other verb, so none reaches here. */
			return EINVAL;
		}
	}

	/* Succeeded: every command is appended. */
	return 0;
}
