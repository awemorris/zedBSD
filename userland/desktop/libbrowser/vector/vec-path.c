/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Paths built one command at a time, arcs turned into cubic curves, and their tight bounding boxes.
 */

#include "vector/vec-path.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * The handle length, in units of the radius, of the cubic Bezier curve
 * that stands for a quarter of an ellipse: 4/3 * tan(theta / 4) for
 * theta = pi / 2, which is 4 * (sqrt(2) - 1) / 3.  It puts the curve's
 * midpoint on the ellipse.
 */
#define PATH_KAPPA		0.55228475f

/*
 * The smallest and the largest radius an arc keeps.  A radius under the
 * smallest is raised to it and one over the largest is lowered to it, so
 * that the squares and ratios of the conversion stay far inside the range
 * of a float; an arc with such radii is a straight line or is scaled up to
 * fit its chord anyway.
 */
#define PATH_ARC_RADIUS_MIN	1e-6f
#define PATH_ARC_RADIUS_MAX	1e9f

/*
 * Under this value of Lambda (SVG 2 B.2.5 eq. 6.2, the squared chord over
 * the squared radii) the chord is under 1e-10 of the radii, the arc is
 * straight to within a float's resolution and is added as a line.
 */
#define PATH_ARC_LAMBDA_MIN	1e-20f

/*
 * How far above a whole number of quarter turns an arc's angle may be and
 * still use that many curves, in quarter turns: it keeps a half circle,
 * whose computed angle is a rounding above pi, from taking three curves.
 */
#define PATH_ARC_QUARTER_SLACK	1e-4f

/* The most curves one arc is split into: four quarter turns make a full turn. */
#define PATH_ARC_SEGMENTS_MAX	4

/*
 * The fraction of a side that the radii along it may exceed it by and
 * still be taken as filling it (the rounding of a caller that normalized
 * them), which vec_path_append_rounded_rect then scales to fit exactly.
 */
#define PATH_RRECT_SLACK	(1.0f / 1024.0f)

/*
 * The most verbs and points a single command adds: a rounded rectangle is
 * a move, four sides, four corners and a close (ten verbs) with at most
 * seventeen points (the start, four side lines and four curves of three).
 */
#define PATH_SCRATCH_VERBS	10
#define PATH_SCRATCH_POINTS	17

/* The initial size of a path's arrays, in elements, before they double. */
#define PATH_CAPACITY_FIRST	16

/*
 * The commands of one builder call, collected before the path is touched,
 * so that a call either adds all of its commands or none of them.
 */
struct path_scratch {
	unsigned char verbs[PATH_SCRATCH_VERBS];
	size_t verb_count;
	struct vec_point points[PATH_SCRATCH_POINTS];
	size_t point_count;
};

/*
 * An elliptical arc in the center parameterization of SVG 2 B.2.4: the
 * ellipse's center, its radii after the correction of B.2.5, the cosine and
 * sine of its rotation, the start angle theta1 and the signed sweep
 * delta (positive towards the y axis), both in radians.
 */
struct path_arc {
	struct vec_point center;
	float rx;
	float ry;
	float cos_phi;
	float sin_phi;
	float theta1;
	float delta;
};

/*
 * How many points each verb takes from the point array, indexed by
 * enum vec_verb: move 1, line 1, quad 2, cubic 3, close 0.
 *
 * Values referenced: SVG 2 chapter 9 (the end points and control points of
 * the commands in sections 9.3.3 to 9.3.7), as the comment of enum vec_verb
 * lists them.
 */
static const unsigned char path_verb_points[5] = { 1, 1, 2, 3, 0 };

/* The forward declarations of the static functions. */
static void path_set_error(struct vec_path *path, int error);
static int path_reserve(struct vec_path *path, size_t verbs, size_t points);
static void scratch_clear(struct path_scratch *scratch);
static void scratch_move(struct path_scratch *scratch, float x, float y);
static void scratch_line(struct path_scratch *scratch, float x, float y);
static void scratch_quad(struct path_scratch *scratch, float cx, float cy, float x, float y);
static void scratch_cubic(struct path_scratch *scratch, float c1x, float c1y, float c2x, float c2y, float x, float y);
static void scratch_close(struct path_scratch *scratch);
static int path_append_scratch(struct vec_path *path, const struct path_scratch *scratch);
static int path_arc_parameters(struct path_arc *arc, struct vec_point from, struct vec_point to, float rx, float ry, int large_arc, int sweep);
static struct vec_point path_arc_map(const struct path_arc *arc, float unit_x, float unit_y);
static void path_arc_emit(struct path_scratch *scratch, const struct path_arc *arc, struct vec_point end);
static int path_cubic_roots(float p0, float p1, float p2, float p3, float roots[2]);
static int path_quad_roots(float p0, float p1, float p2, float roots[1]);
static struct vec_point path_curve_at(const struct vec_point *control, int degree, float t);
static void path_bounds_curve(struct vec_rect *rect, const struct vec_point *control, int degree);
static void path_rrect_collect(struct path_scratch *scratch, float left, float top, float right, float bottom, float radii[4][2]);

/*
 * Makes an empty path.
 */
void
vec_path_init(
	struct vec_path *path)
{
	/* Zeroes the structure, which is the empty path with no memory and no error. */
	memset(path, 0, sizeof(*path));
}

/*
 * Frees a path's arrays and leaves it empty.
 */
void
vec_path_release(
	struct vec_path *path)
{
	/* Gives back the verb array and the point array. */
	free(path->verbs);
	free(path->points);

	/* Leaves the structure as vec_path_init does. */
	memset(path, 0, sizeof(*path));
}

/*
 * Empties a path but keeps its arrays for reuse, and clears its error.
 */
void
vec_path_reset(
	struct vec_path *path)
{
	/* Forgets the commands while the arrays stay allocated. */
	path->verb_count = 0;
	path->point_count = 0;

	/* Forgets the current point, the closed state and any error. */
	path->start.x = 0.0f;
	path->start.y = 0.0f;
	path->current.x = 0.0f;
	path->current.y = 0.0f;
	path->has_current = 0;
	path->closed = 0;
	path->error = 0;
}

/*
 * Reports the first error a command of the path met.
 */
int
vec_path_error(
	const struct vec_path *path)
{
	/* Reports the sticky error, 0 for none. */
	return path->error;
}

/*
 * Starts a new subpath at a point.
 */
int
vec_path_move_to(
	struct vec_path *path,
	float x,
	float y)
{
	struct path_scratch scratch;
	int error;

	/* Does nothing once an earlier command has failed. */
	if (path->error != 0)
		return path->error;

	/* Collects the move. */
	scratch_clear(&scratch);
	scratch_move(&scratch, x, y);

	/* Adds the move to the path. */
	error = path_append_scratch(path, &scratch);
	if (error != 0)
		return error;

	/* Succeeded: a subpath starts at the point. */
	return 0;
}

/*
 * Adds a straight line from the current point.
 */
int
vec_path_line_to(
	struct vec_path *path,
	float x,
	float y)
{
	struct path_scratch scratch;
	int error;

	/* Does nothing once an earlier command has failed. */
	if (path->error != 0)
		return path->error;

	/* Collects the line. */
	scratch_clear(&scratch);
	scratch_line(&scratch, x, y);

	/* Adds the line, with the move that a close makes necessary. */
	error = path_append_scratch(path, &scratch);
	if (error != 0)
		return error;

	/* Succeeded: the line is in the path. */
	return 0;
}

/*
 * Adds a quadratic Bezier curve from the current point.
 */
int
vec_path_quad_to(
	struct vec_path *path,
	float cx,
	float cy,
	float x,
	float y)
{
	struct path_scratch scratch;
	int error;

	/* Does nothing once an earlier command has failed. */
	if (path->error != 0)
		return path->error;

	/* Collects the curve. */
	scratch_clear(&scratch);
	scratch_quad(&scratch, cx, cy, x, y);

	/* Adds the curve, with the move that a close makes necessary. */
	error = path_append_scratch(path, &scratch);
	if (error != 0)
		return error;

	/* Succeeded: the curve is in the path. */
	return 0;
}

/*
 * Adds a cubic Bezier curve from the current point.
 */
int
vec_path_cubic_to(
	struct vec_path *path,
	float c1x,
	float c1y,
	float c2x,
	float c2y,
	float x,
	float y)
{
	struct path_scratch scratch;
	int error;

	/* Does nothing once an earlier command has failed. */
	if (path->error != 0)
		return path->error;

	/* Collects the curve. */
	scratch_clear(&scratch);
	scratch_cubic(&scratch, c1x, c1y, c2x, c2y, x, y);

	/* Adds the curve, with the move that a close makes necessary. */
	error = path_append_scratch(path, &scratch);
	if (error != 0)
		return error;

	/* Succeeded: the curve is in the path. */
	return 0;
}

/*
 * Adds an elliptical arc from the current point as cubic Bezier curves.
 *
 * The endpoint parameterization is converted to the center
 * parameterization of SVG 2 B.2.4 after the radii are corrected as B.2.5
 * says; the arc is cut into parts of at most a quarter turn, each one cubic
 * curve.  The arc is built in a scratch area first, so a failure leaves the
 * path as it was apart from its error.
 */
int
vec_path_arc_to(
	struct vec_path *path,
	float rx,
	float ry,
	float rotation_degrees,
	int large_arc,
	int sweep,
	float x,
	float y)
{
	struct path_scratch scratch;
	struct path_arc arc;
	struct vec_matrix rotation;
	struct vec_point end;
	int degenerate;
	int finite;
	int valid;
	int error;

	/* Does nothing once an earlier command has failed. */
	if (path->error != 0)
		return path->error;

	/* Refuses a radius that is not a finite number. */
	finite = vec_finite(rx);
	if (!finite) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}
	finite = vec_finite(ry);
	if (!finite) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Refuses an end point that is not a usable coordinate. */
	end.x = x;
	end.y = y;
	valid = vec_point_valid(end);
	if (!valid) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Takes the rotation of the ellipse's axes, which refuses an angle that is not finite. */
	error = vec_matrix_rotate(&rotation, rotation_degrees);
	if (error != 0) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Refuses an arc that has no current point to start from. */
	if (!path->has_current) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Adds nothing when the end point is the current point (SVG 2 9.5.1). */
	if (x == path->current.x && y == path->current.y)
		return 0;

	/* Takes the absolute values of the radii (B.2.5 eq. 6.1). */
	if (rx < 0.0f)
		rx = -rx;
	if (ry < 0.0f)
		ry = -ry;

	/* Adds a straight line when a radius is 0 (SVG 2 9.5.1). */
	if (rx == 0.0f || ry == 0.0f) {
		error = vec_path_line_to(path, x, y);
		if (error != 0)
			return error;

		/* Succeeded: the arc is the straight line. */
		return 0;
	}

	/* Keeps the radii where the conversion's arithmetic stays inside a float. */
	if (rx < PATH_ARC_RADIUS_MIN)
		rx = PATH_ARC_RADIUS_MIN;
	if (rx > PATH_ARC_RADIUS_MAX)
		rx = PATH_ARC_RADIUS_MAX;
	if (ry < PATH_ARC_RADIUS_MIN)
		ry = PATH_ARC_RADIUS_MIN;
	if (ry > PATH_ARC_RADIUS_MAX)
		ry = PATH_ARC_RADIUS_MAX;

	/* Converts the end points to the center parameterization. */
	arc.cos_phi = rotation.a;
	arc.sin_phi = rotation.b;
	degenerate = path_arc_parameters(&arc, path->current, end, rx, ry, large_arc, sweep);

	/* Adds a straight line when the chord is too short against the radii to be told from a line. */
	if (degenerate) {
		error = vec_path_line_to(path, x, y);
		if (error != 0)
			return error;

		/* Succeeded: the arc is the straight line. */
		return 0;
	}

	/* Collects the curves of the arc. */
	scratch_clear(&scratch);
	path_arc_emit(&scratch, &arc, end);

	/* Adds the curves, with the move that a close makes necessary. */
	error = path_append_scratch(path, &scratch);
	if (error != 0)
		return error;

	/* Succeeded: the arc is in the path as cubic curves. */
	return 0;
}

/*
 * Closes the current subpath.
 */
int
vec_path_close(
	struct vec_path *path)
{
	int error;

	/* Does nothing once an earlier command has failed. */
	if (path->error != 0)
		return path->error;

	/* Does nothing for a path with no subpath and for a subpath already closed. */
	if (!path->has_current || path->closed)
		return 0;

	/* Makes room for the close verb. */
	error = path_reserve(path, 1, 0);
	if (error != 0) {
		path_set_error(path, error);
		return error;
	}

	/* Adds the close and makes the subpath's first point the current point. */
	path->verbs[path->verb_count] = (unsigned char)VEC_VERB_CLOSE;
	path->verb_count++;
	path->current = path->start;
	path->closed = 1;

	/* Succeeded: the subpath is closed. */
	return 0;
}

/*
 * Adds a closed rectangle as its own subpath.
 */
int
vec_path_append_rect(
	struct vec_path *path,
	float x,
	float y,
	float width,
	float height)
{
	struct path_scratch scratch;
	int finite;
	int error;

	/* Does nothing once an earlier command has failed. */
	if (path->error != 0)
		return path->error;

	/* Refuses a value that is not a finite number. */
	finite = vec_finite(x);
	if (finite)
		finite = vec_finite(y);
	if (finite)
		finite = vec_finite(width);
	if (finite)
		finite = vec_finite(height);
	if (!finite) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Refuses a negative size. */
	if (width < 0.0f || height < 0.0f) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Collects the subpath: a move to the corner, the three other corners clockwise, and the close. */
	scratch_clear(&scratch);
	scratch_move(&scratch, x, y);
	scratch_line(&scratch, x + width, y);
	scratch_line(&scratch, x + width, y + height);
	scratch_line(&scratch, x, y + height);
	scratch_close(&scratch);

	/* Adds the subpath, which refuses a corner outside the coordinate range. */
	error = path_append_scratch(path, &scratch);
	if (error != 0)
		return error;

	/* Succeeded: the rectangle is in the path. */
	return 0;
}

/*
 * Adds a closed rectangle with elliptical corners as its own subpath.
 *
 * The radii are taken as they are unless the two along a side add up to more
 * than the side; then all of them are scaled by the same factor so that the
 * most crowded side is exactly filled (CSS Backgrounds 3 4.5).  The path
 * starts at the end of the top left corner's curve, goes along the top side,
 * and each corner is one cubic curve, or a line to the corner when a radius
 * of that corner is 0.
 */
int
vec_path_append_rounded_rect(
	struct vec_path *path,
	const struct vec_rrect *rrect)
{
	struct path_scratch scratch;
	float radii[4][2];
	float width;
	float height;
	float factor;
	float sum;
	float limit;
	float fit;
	float left;
	float top;
	float right;
	float bottom;
	int finite;
	int corner;
	int axis;
	int side;
	int curved;
	int error;

	/* Does nothing once an earlier command has failed. */
	if (path->error != 0)
		return path->error;

	/* Refuses an edge of the rectangle that is not a finite number. */
	left = rrect->rect.left;
	top = rrect->rect.top;
	right = rrect->rect.right;
	bottom = rrect->rect.bottom;
	finite = vec_finite(left);
	if (finite)
		finite = vec_finite(top);
	if (finite)
		finite = vec_finite(right);
	if (finite)
		finite = vec_finite(bottom);
	if (!finite) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Refuses an inverted rectangle. */
	if (right < left || bottom < top) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}
	width = right - left;
	height = bottom - top;

	/* Copies the radii, refusing one that is not finite or is negative. */
	for (corner = 0;
	     corner < 4;
	     corner++) {
		for (axis = 0;
		     axis < 2;
		     axis++) {
			radii[corner][axis] = rrect->radii[corner][axis];
			finite = vec_finite(radii[corner][axis]);
			if (!finite || radii[corner][axis] < 0.0f) {
				path_set_error(path, EINVAL);
				return EINVAL;
			}
		}
	}

	/* Makes a corner square when either of its radii is 0. */
	for (corner = 0;
	     corner < 4;
	     corner++) {
		if (radii[corner][0] == 0.0f || radii[corner][1] == 0.0f) {
			radii[corner][0] = 0.0f;
			radii[corner][1] = 0.0f;
		}
	}

	/*
	 * Finds the factor that fits the radii along each side: the top side
	 * carries the horizontal radii of the two top corners, the right side
	 * the vertical radii of the right corners, and so on clockwise.
	 */
	factor = 1.0f;
	for (side = 0;
	     side < 4;
	     side++) {
		/* Takes the radii along this side and the side's length. */
		if (side == VEC_SIDE_TOP) {
			sum = radii[VEC_CORNER_TOP_LEFT][0] + radii[VEC_CORNER_TOP_RIGHT][0];
			limit = width;
		} else if (side == VEC_SIDE_RIGHT) {
			sum = radii[VEC_CORNER_TOP_RIGHT][1] + radii[VEC_CORNER_BOTTOM_RIGHT][1];
			limit = height;
		} else if (side == VEC_SIDE_BOTTOM) {
			sum = radii[VEC_CORNER_BOTTOM_RIGHT][0] + radii[VEC_CORNER_BOTTOM_LEFT][0];
			limit = width;
		} else {
			sum = radii[VEC_CORNER_TOP_LEFT][1] + radii[VEC_CORNER_BOTTOM_LEFT][1];
			limit = height;
		}

		/* Refuses radii that overfill the side by more than the slack a caller's rounding may leave. */
		if (sum > limit + limit * PATH_RRECT_SLACK) {
			path_set_error(path, EINVAL);
			return EINVAL;
		}

		/* Keeps the smallest factor that makes the radii fit; sum is positive in the division. */
		if (sum > limit && sum > 0.0f) {
			fit = limit / sum;
			if (fit < factor)
				factor = fit;
		}
	}

	/* Scales the radii by the factor when some side was overfilled. */
	if (factor < 1.0f) {
		for (corner = 0;
		     corner < 4;
		     corner++) {
			radii[corner][0] *= factor;
			radii[corner][1] *= factor;
		}
	}

	/* Counts the corners that are curved. */
	curved = 0;
	for (corner = 0;
	     corner < 4;
	     corner++) {
		if (radii[corner][0] > 0.0f)
			curved++;
	}

	/* Collects the subpath: a plain rectangle when no corner is curved, else the sides and corners. */
	scratch_clear(&scratch);
	if (curved == 0) {
		scratch_move(&scratch, left, top);
		scratch_line(&scratch, right, top);
		scratch_line(&scratch, right, bottom);
		scratch_line(&scratch, left, bottom);
		scratch_close(&scratch);
	} else {
		path_rrect_collect(&scratch, left, top, right, bottom, radii);
	}

	/* Adds the subpath, which refuses a point outside the coordinate range. */
	error = path_append_scratch(path, &scratch);
	if (error != 0)
		return error;

	/* Succeeded: the rounded rectangle is in the path. */
	return 0;
}

/*
 * Adds a closed ellipse as its own subpath.
 *
 * It starts at the right end of the horizontal axis and goes clockwise on
 * the screen, one cubic curve for each quarter.
 */
int
vec_path_append_ellipse(
	struct vec_path *path,
	float cx,
	float cy,
	float rx,
	float ry)
{
	struct path_scratch scratch;
	float handle_x;
	float handle_y;
	int finite;
	int error;

	/* Does nothing once an earlier command has failed. */
	if (path->error != 0)
		return path->error;

	/* Refuses a value that is not a finite number. */
	finite = vec_finite(cx);
	if (finite)
		finite = vec_finite(cy);
	if (finite)
		finite = vec_finite(rx);
	if (finite)
		finite = vec_finite(ry);
	if (!finite) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Refuses a radius that is not positive. */
	if (rx <= 0.0f || ry <= 0.0f) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Finds the handle lengths of the four quarter curves. */
	handle_x = PATH_KAPPA * rx;
	handle_y = PATH_KAPPA * ry;

	/* Collects the subpath: from the right end down to the bottom, to the left, up to the top and back. */
	scratch_clear(&scratch);
	scratch_move(&scratch, cx + rx, cy);
	scratch_cubic(&scratch, cx + rx, cy + handle_y, cx + handle_x, cy + ry, cx, cy + ry);
	scratch_cubic(&scratch, cx - handle_x, cy + ry, cx - rx, cy + handle_y, cx - rx, cy);
	scratch_cubic(&scratch, cx - rx, cy - handle_y, cx - handle_x, cy - ry, cx, cy - ry);
	scratch_cubic(&scratch, cx + handle_x, cy - ry, cx + rx, cy - handle_y, cx + rx, cy);
	scratch_close(&scratch);

	/* Adds the subpath, which refuses a point outside the coordinate range. */
	error = path_append_scratch(path, &scratch);
	if (error != 0)
		return error;

	/* Succeeded: the ellipse is in the path. */
	return 0;
}

/*
 * Applies a matrix to every point of a path.
 *
 * Every mapped point is checked before any is stored, so a failure leaves
 * the points as they were.
 */
int
vec_path_transform(
	struct vec_path *path,
	const struct vec_matrix *matrix)
{
	struct vec_point mapped;
	size_t i;
	int valid;

	/* Does nothing once an earlier command has failed. */
	if (path->error != 0)
		return path->error;

	/* Refuses a matrix that is not usable. */
	valid = vec_matrix_valid(matrix);
	if (!valid) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Checks that every point maps to a usable coordinate. */
	for (i = 0;
	     i < path->point_count;
	     i++) {
		mapped = vec_matrix_apply(matrix, path->points[i]);
		valid = vec_point_valid(mapped);
		if (!valid) {
			path_set_error(path, EINVAL);
			return EINVAL;
		}
	}

	/* Checks that the subpath's first point and the current point map to usable coordinates. */
	mapped = vec_matrix_apply(matrix, path->start);
	valid = vec_point_valid(mapped);
	if (!valid) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}
	mapped = vec_matrix_apply(matrix, path->current);
	valid = vec_point_valid(mapped);
	if (!valid) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Stores the mapped points; the check above showed none can fail. */
	for (i = 0;
	     i < path->point_count;
	     i++)
		path->points[i] = vec_matrix_apply(matrix, path->points[i]);
	path->start = vec_matrix_apply(matrix, path->start);
	path->current = vec_matrix_apply(matrix, path->current);

	/* Succeeded: the path is mapped. */
	return 0;
}

/*
 * Copies a path into another.
 *
 * The new arrays are allocated before the destination is touched.
 */
int
vec_path_copy(
	struct vec_path *destination,
	const struct vec_path *source)
{
	unsigned char *verbs;
	struct vec_point *points;

	/* Refuses a source that has met an error. */
	if (source->error != 0)
		return EINVAL;

	/* Does nothing when a path is copied onto itself. */
	if (destination == source)
		return 0;

	/* Allocates the copy of the verbs, none for an empty path. */
	verbs = NULL;
	if (source->verb_count != 0) {
		verbs = malloc(source->verb_count * sizeof(*verbs));
		if (verbs == NULL) {
			vec_path_release(destination);
			destination->error = ENOMEM;
			return ENOMEM;
		}
	}

	/* Allocates the copy of the points, none for an empty path. */
	points = NULL;
	if (source->point_count != 0) {
		points = malloc(source->point_count * sizeof(*points));
		if (points == NULL) {
			free(verbs);
			vec_path_release(destination);
			destination->error = ENOMEM;
			return ENOMEM;
		}
	}

	/* Copies the commands into the new arrays. */
	if (source->verb_count != 0)
		memcpy(verbs, source->verbs, source->verb_count * sizeof(*verbs));
	if (source->point_count != 0)
		memcpy(points, source->points, source->point_count * sizeof(*points));

	/* Gives back the destination's old arrays. */
	free(destination->verbs);
	free(destination->points);

	/* Installs the copy together with the source's builder state. */
	destination->verbs = verbs;
	destination->verb_count = source->verb_count;
	destination->verb_capacity = source->verb_count;
	destination->points = points;
	destination->point_count = source->point_count;
	destination->point_capacity = source->point_count;
	destination->start = source->start;
	destination->current = source->current;
	destination->has_current = source->has_current;
	destination->closed = source->closed;
	destination->error = 0;

	/* Succeeded: the destination holds a copy. */
	return 0;
}

/*
 * Gives the read-only view of a path.
 */
void
vec_path_view_of(
	const struct vec_path *path,
	struct vec_path_view *view)
{
	/* Points the view into the path's arrays without copying them. */
	view->verbs = path->verbs;
	view->verb_count = path->verb_count;
	view->points = path->points;
	view->point_count = path->point_count;
}

/*
 * Checks that a view is well formed and every point in it is valid.
 *
 * The view is well formed when its counts are within the path limits, its
 * arrays exist for the counts, it starts with a move, every verb is a
 * known one, and the points the verbs take add up to exactly point_count.
 * A view with no verb and no point is well formed (an empty path).
 */
int
vec_path_view_check(
	const struct vec_path_view *view)
{
	size_t needed;
	size_t i;
	unsigned char verb;
	int valid;

	/* Refuses counts beyond the limits of a path, which bounds the work below. */
	if (view->verb_count > VEC_PATH_VERBS_MAX)
		return EINVAL;
	if (view->point_count > VEC_PATH_POINTS_MAX)
		return EINVAL;

	/* Refuses counts that name arrays the view does not have. */
	if (view->verb_count != 0 && view->verbs == NULL)
		return EINVAL;
	if (view->point_count != 0 && view->points == NULL)
		return EINVAL;

	/* Accepts the empty view, and refuses points that no verb takes. */
	if (view->verb_count == 0) {
		if (view->point_count != 0)
			return EINVAL;

		/* Succeeded: an empty view is well formed. */
		return 0;
	}

	/* Refuses a view that does not start a subpath. */
	if (view->verbs[0] != (unsigned char)VEC_VERB_MOVE)
		return EINVAL;

	/* Adds up the points the verbs take, refusing an unknown verb or a count past the points given. */
	needed = 0;
	for (i = 0;
	     i < view->verb_count;
	     i++) {
		verb = view->verbs[i];
		if (verb > (unsigned char)VEC_VERB_CLOSE)
			return EINVAL;
		needed += path_verb_points[verb];
		if (needed > view->point_count)
			return EINVAL;
	}

	/* Refuses a view whose verbs take fewer points than it holds. */
	if (needed != view->point_count)
		return EINVAL;

	/* Checks that every point is a usable coordinate. */
	for (i = 0;
	     i < view->point_count;
	     i++) {
		valid = vec_point_valid(view->points[i]);
		if (!valid)
			return EINVAL;
	}

	/* Succeeded: the view is well formed. */
	return 0;
}

/*
 * Computes the tight bounding box of a path.
 *
 * Each end point counts, and so does each point of a curve where one of its
 * coordinates is at an extreme (the derivative is zero), found per axis from
 * the derivative's roots; control points that are not on the curve do not
 * count.
 */
int
vec_path_bounds(
	const struct vec_path_view *view,
	struct vec_rect *bounds)
{
	struct vec_rect rect;
	struct vec_point control[4];
	struct vec_point current;
	size_t point_index;
	size_t i;
	int error;

	/* Refuses a malformed view. */
	error = vec_path_view_check(view);
	if (error != 0)
		return error;

	/* Refuses an empty view, which has no box. */
	if (view->verb_count == 0)
		return EINVAL;

	/* Walks the commands, growing the rectangle by the points of each. */
	vec_rect_empty(&rect);
	point_index = 0;
	current = view->points[0];
	for (i = 0;
	     i < view->verb_count;
	     i++) {
		if (view->verbs[i] == (unsigned char)VEC_VERB_MOVE) {
			/* A move starts a subpath, and a lone move still counts its point. */
			current = view->points[point_index];
			vec_rect_include(&rect, current);
			point_index++;
		} else if (view->verbs[i] == (unsigned char)VEC_VERB_LINE) {
			/* A line adds its end point. */
			current = view->points[point_index];
			vec_rect_include(&rect, current);
			point_index++;
		} else if (view->verbs[i] == (unsigned char)VEC_VERB_QUAD) {
			/* A quadratic curve adds its end point and the extremes between. */
			control[0] = current;
			control[1] = view->points[point_index];
			control[2] = view->points[point_index + 1];
			path_bounds_curve(&rect, control, 2);
			current = control[2];
			vec_rect_include(&rect, current);
			point_index += 2;
		} else if (view->verbs[i] == (unsigned char)VEC_VERB_CUBIC) {
			/* A cubic curve adds its end point and the extremes between. */
			control[0] = current;
			control[1] = view->points[point_index];
			control[2] = view->points[point_index + 1];
			control[3] = view->points[point_index + 2];
			path_bounds_curve(&rect, control, 3);
			current = control[3];
			vec_rect_include(&rect, current);
			point_index += 3;
		}

		/* A close adds nothing: its line returns to a point already counted. */
	}

	/* Publishes the rectangle. */
	*bounds = rect;

	/* Succeeded: *bounds is the tight bounding box. */
	return 0;
}

/*
 * Keeps the first error a command met.
 */
static void
path_set_error(
	struct vec_path *path,
	int error)
{
	/* Records the error unless an earlier one is already kept. */
	if (path->error == 0)
		path->error = error;
}

/*
 * Makes room for more verbs and points, within the limits of a path.
 *
 * Returns ENOBUFS when the counts would pass VEC_PATH_VERBS_MAX or
 * VEC_PATH_POINTS_MAX, ENOMEM when an array cannot grow.  It does not
 * record the error; the caller does.
 */
static int
path_reserve(
	struct vec_path *path,
	size_t verbs,
	size_t points)
{
	unsigned char *new_verbs;
	struct vec_point *new_points;
	size_t capacity;

	/* Refuses counts past the limits; the subtraction cannot wrap because the counts are within them. */
	if (verbs > VEC_PATH_VERBS_MAX - path->verb_count)
		return ENOBUFS;
	if (points > VEC_PATH_POINTS_MAX - path->point_count)
		return ENOBUFS;

	/* Grows the verb array by doubling when the new count does not fit. */
	if (path->verb_count + verbs > path->verb_capacity) {
		capacity = path->verb_capacity;
		if (capacity == 0)
			capacity = PATH_CAPACITY_FIRST;
		while (capacity < path->verb_count + verbs)
			capacity *= 2;
		if (capacity > VEC_PATH_VERBS_MAX)
			capacity = VEC_PATH_VERBS_MAX;
		new_verbs = realloc(path->verbs, capacity * sizeof(*new_verbs));
		if (new_verbs == NULL)
			return ENOMEM;
		path->verbs = new_verbs;
		path->verb_capacity = capacity;
	}

	/* Grows the point array by doubling when the new count does not fit. */
	if (path->point_count + points > path->point_capacity) {
		capacity = path->point_capacity;
		if (capacity == 0)
			capacity = PATH_CAPACITY_FIRST;
		while (capacity < path->point_count + points)
			capacity *= 2;
		if (capacity > VEC_PATH_POINTS_MAX)
			capacity = VEC_PATH_POINTS_MAX;
		new_points = realloc(path->points, capacity * sizeof(*new_points));
		if (new_points == NULL)
			return ENOMEM;
		path->points = new_points;
		path->point_capacity = capacity;
	}

	/* Succeeded: the arrays hold the new counts. */
	return 0;
}

/*
 * Empties a scratch area.
 */
static void
scratch_clear(
	struct path_scratch *scratch)
{
	/* Forgets any commands collected before. */
	scratch->verb_count = 0;
	scratch->point_count = 0;
}

/*
 * Collects a move to a point.
 */
static void
scratch_move(
	struct path_scratch *scratch,
	float x,
	float y)
{
	/* Stops when the scratch area is full, which no caller reaches. */
	if (scratch->verb_count + 1 > PATH_SCRATCH_VERBS || scratch->point_count + 1 > PATH_SCRATCH_POINTS)
		return;

	/* Stores the verb and its point. */
	scratch->verbs[scratch->verb_count] = (unsigned char)VEC_VERB_MOVE;
	scratch->verb_count++;
	scratch->points[scratch->point_count].x = x;
	scratch->points[scratch->point_count].y = y;
	scratch->point_count++;
}

/*
 * Collects a line to a point.
 */
static void
scratch_line(
	struct path_scratch *scratch,
	float x,
	float y)
{
	/* Stops when the scratch area is full, which no caller reaches. */
	if (scratch->verb_count + 1 > PATH_SCRATCH_VERBS || scratch->point_count + 1 > PATH_SCRATCH_POINTS)
		return;

	/* Stores the verb and its point. */
	scratch->verbs[scratch->verb_count] = (unsigned char)VEC_VERB_LINE;
	scratch->verb_count++;
	scratch->points[scratch->point_count].x = x;
	scratch->points[scratch->point_count].y = y;
	scratch->point_count++;
}

/*
 * Collects a quadratic curve.
 */
static void
scratch_quad(
	struct path_scratch *scratch,
	float cx,
	float cy,
	float x,
	float y)
{
	/* Stops when the scratch area is full, which no caller reaches. */
	if (scratch->verb_count + 1 > PATH_SCRATCH_VERBS || scratch->point_count + 2 > PATH_SCRATCH_POINTS)
		return;

	/* Stores the verb, then the control point and the end point. */
	scratch->verbs[scratch->verb_count] = (unsigned char)VEC_VERB_QUAD;
	scratch->verb_count++;
	scratch->points[scratch->point_count].x = cx;
	scratch->points[scratch->point_count].y = cy;
	scratch->points[scratch->point_count + 1].x = x;
	scratch->points[scratch->point_count + 1].y = y;
	scratch->point_count += 2;
}

/*
 * Collects a cubic curve.
 */
static void
scratch_cubic(
	struct path_scratch *scratch,
	float c1x,
	float c1y,
	float c2x,
	float c2y,
	float x,
	float y)
{
	/* Stops when the scratch area is full, which no caller reaches. */
	if (scratch->verb_count + 1 > PATH_SCRATCH_VERBS || scratch->point_count + 3 > PATH_SCRATCH_POINTS)
		return;

	/* Stores the verb, then the two control points and the end point. */
	scratch->verbs[scratch->verb_count] = (unsigned char)VEC_VERB_CUBIC;
	scratch->verb_count++;
	scratch->points[scratch->point_count].x = c1x;
	scratch->points[scratch->point_count].y = c1y;
	scratch->points[scratch->point_count + 1].x = c2x;
	scratch->points[scratch->point_count + 1].y = c2y;
	scratch->points[scratch->point_count + 2].x = x;
	scratch->points[scratch->point_count + 2].y = y;
	scratch->point_count += 3;
}

/*
 * Collects a close.
 */
static void
scratch_close(
	struct path_scratch *scratch)
{
	/* Stops when the scratch area is full, which no caller reaches. */
	if (scratch->verb_count + 1 > PATH_SCRATCH_VERBS)
		return;

	/* Stores the verb, which takes no point. */
	scratch->verbs[scratch->verb_count] = (unsigned char)VEC_VERB_CLOSE;
	scratch->verb_count++;
}

/*
 * Adds the commands of a scratch area to a path as one step.
 *
 * A scratch area that starts with a move begins a new subpath.  Any other
 * continues the current one: it needs a current point, and after a close
 * it first adds the move to the closed subpath's first point.  Every point
 * is checked and room is made before anything is stored, so a failure
 * changes nothing but the path's error.  The error is recorded here.
 */
static int
path_append_scratch(
	struct vec_path *path,
	const struct path_scratch *scratch)
{
	size_t extra;
	size_t i;
	int continues;
	int valid;
	int error;

	/* Checks that every point is a usable coordinate (a NaN or a far point is refused). */
	for (i = 0;
	     i < scratch->point_count;
	     i++) {
		valid = vec_point_valid(scratch->points[i]);
		if (!valid) {
			path_set_error(path, EINVAL);
			return EINVAL;
		}
	}

	/* Tells whether the commands continue a subpath instead of starting one. */
	continues = 1;
	if (scratch->verbs[0] == (unsigned char)VEC_VERB_MOVE)
		continues = 0;

	/* Refuses commands that continue a subpath when there is none. */
	if (continues && !path->has_current) {
		path_set_error(path, EINVAL);
		return EINVAL;
	}

	/* Counts the move that a close makes necessary before a drawing command. */
	extra = 0;
	if (continues && path->closed)
		extra = 1;

	/* Makes room for the move and the commands together. */
	error = path_reserve(path, scratch->verb_count + extra, scratch->point_count + extra);
	if (error != 0) {
		path_set_error(path, error);
		return error;
	}

	/* Adds the move to the closed subpath's first point (SVG 2 9.3.4). */
	if (extra != 0) {
		path->verbs[path->verb_count] = (unsigned char)VEC_VERB_MOVE;
		path->verb_count++;
		path->points[path->point_count] = path->start;
		path->point_count++;
	}

	/* Stores the commands and their points. */
	memcpy(path->verbs + path->verb_count, scratch->verbs, scratch->verb_count * sizeof(path->verbs[0]));
	path->verb_count += scratch->verb_count;
	if (scratch->point_count != 0) {
		memcpy(path->points + path->point_count, scratch->points, scratch->point_count * sizeof(path->points[0]));
		path->point_count += scratch->point_count;
	}

	/* A move makes its point the subpath's first point. */
	if (!continues) {
		path->start = scratch->points[0];
		path->has_current = 1;
	}

	/* Moves the current point to the end of the last command, which a close sends back to the first point. */
	if (scratch->verbs[scratch->verb_count - 1] == (unsigned char)VEC_VERB_CLOSE) {
		path->current = path->start;
		path->closed = 1;
	} else {
		path->current = scratch->points[scratch->point_count - 1];
		path->closed = 0;
	}

	/* Succeeded: the commands are in the path. */
	return 0;
}

/*
 * Collects the subpath of a rectangle with at least one curved corner.
 *
 * It starts at the end of the top left corner's curve and goes clockwise:
 * each side is a line to where the next corner's curve begins, and each
 * curved corner is one cubic curve whose handles are (1 - kappa) of the
 * radius short of the corner (the quarter ellipse of path_arc_emit's
 * construction).  A square corner adds nothing, because the side's line
 * already ends at the corner.  The caller has made every corner square
 * (both radii 0) or curved (both positive) and fitted the radii to the sides.
 */
static void
path_rrect_collect(
	struct path_scratch *scratch,
	float left,
	float top,
	float right,
	float bottom,
	float radii[4][2])
{
	float near;

	/* Finds the handle's distance from the corner along a radius. */
	near = 1.0f - PATH_KAPPA;

	/* Starts the subpath at the end of the top left corner's curve. */
	scratch_move(scratch, left + radii[VEC_CORNER_TOP_LEFT][0], top);

	/* Adds the top side and the top right corner. */
	scratch_line(scratch, right - radii[VEC_CORNER_TOP_RIGHT][0], top);
	if (radii[VEC_CORNER_TOP_RIGHT][0] > 0.0f) {
		scratch_cubic(scratch,
		    right - near * radii[VEC_CORNER_TOP_RIGHT][0], top,
		    right, top + near * radii[VEC_CORNER_TOP_RIGHT][1],
		    right, top + radii[VEC_CORNER_TOP_RIGHT][1]);
	}

	/* Adds the right side and the bottom right corner. */
	scratch_line(scratch, right, bottom - radii[VEC_CORNER_BOTTOM_RIGHT][1]);
	if (radii[VEC_CORNER_BOTTOM_RIGHT][0] > 0.0f) {
		scratch_cubic(scratch,
		    right, bottom - near * radii[VEC_CORNER_BOTTOM_RIGHT][1],
		    right - near * radii[VEC_CORNER_BOTTOM_RIGHT][0], bottom,
		    right - radii[VEC_CORNER_BOTTOM_RIGHT][0], bottom);
	}

	/* Adds the bottom side and the bottom left corner. */
	scratch_line(scratch, left + radii[VEC_CORNER_BOTTOM_LEFT][0], bottom);
	if (radii[VEC_CORNER_BOTTOM_LEFT][0] > 0.0f) {
		scratch_cubic(scratch,
		    left + near * radii[VEC_CORNER_BOTTOM_LEFT][0], bottom,
		    left, bottom - near * radii[VEC_CORNER_BOTTOM_LEFT][1],
		    left, bottom - radii[VEC_CORNER_BOTTOM_LEFT][1]);
	}

	/* Adds the left side and the top left corner, which ends where the subpath started. */
	scratch_line(scratch, left, top + radii[VEC_CORNER_TOP_LEFT][1]);
	if (radii[VEC_CORNER_TOP_LEFT][0] > 0.0f) {
		scratch_cubic(scratch,
		    left, top + near * radii[VEC_CORNER_TOP_LEFT][1],
		    left + near * radii[VEC_CORNER_TOP_LEFT][0], top,
		    left + radii[VEC_CORNER_TOP_LEFT][0], top);
	}

	/* Closes the subpath. */
	scratch_close(scratch);
}

/*
 * Converts an arc from endpoint to center parameterization.
 *
 * Follows SVG 2 B.2.4 (eq. 5.1 to 5.6) after B.2.5 (eq. 6.1 to 6.3), in
 * the form that divides by the radii first so that no square of a radius
 * is formed: with u = (x1' / rx, y1' / ry), Lambda = |u|^2, and the center
 * (cx', cy') = +-sqrt((1 - Lambda) / Lambda) * (rx * u.y, -ry * u.x).
 * arc->cos_phi and arc->sin_phi are set by the caller; radii are positive.
 *
 * Returns 1 when the chord is too short against the radii for the
 * conversion (the arc is a line), 0 with *arc filled in otherwise.
 */
static int
path_arc_parameters(
	struct path_arc *arc,
	struct vec_point from,
	struct vec_point to,
	float rx,
	float ry,
	int large_arc,
	int sweep)
{
	float half_dx;
	float half_dy;
	float x1p;
	float y1p;
	float ux;
	float uy;
	float lambda;
	float scale;
	float coefficient;
	float cxp;
	float cyp;
	float start_x;
	float start_y;
	float end_x;
	float end_y;
	float cross;
	float dot;

	/* Moves the origin to the chord's midpoint and turns the axes with the ellipse (eq. 5.1). */
	half_dx = (from.x - to.x) * 0.5f;
	half_dy = (from.y - to.y) * 0.5f;
	x1p = arc->cos_phi * half_dx + arc->sin_phi * half_dy;
	y1p = -arc->sin_phi * half_dx + arc->cos_phi * half_dy;

	/* Finds Lambda of eq. 6.2, the squared chord over the squared radii. */
	ux = x1p / rx;
	uy = y1p / ry;
	lambda = ux * ux + uy * uy;

	/* Reports a chord that is too short to tell from a line, which also guards the division below. */
	if (!(lambda >= PATH_ARC_LAMBDA_MIN))
		return 1;

	/* Scales the radii up when they are too small (eq. 6.3), which puts the center on the chord. */
	if (lambda > 1.0f) {
		scale = sqrtf(lambda);
		rx *= scale;
		ry *= scale;
		ux = x1p / rx;
		uy = y1p / ry;
		coefficient = 0.0f;
	} else {
		coefficient = sqrtf((1.0f - lambda) / lambda);
	}

	/* Takes the sign of the center: plus when the flags differ, minus when they agree (eq. 5.2). */
	if ((large_arc != 0) == (sweep != 0))
		coefficient = -coefficient;

	/* Finds the center in the primed system (eq. 5.2). */
	cxp = coefficient * rx * uy;
	cyp = -coefficient * ry * ux;

	/* Finds the center in the user system (eq. 5.3). */
	arc->center.x = arc->cos_phi * cxp - arc->sin_phi * cyp + (from.x + to.x) * 0.5f;
	arc->center.y = arc->sin_phi * cxp + arc->cos_phi * cyp + (from.y + to.y) * 0.5f;
	arc->rx = rx;
	arc->ry = ry;

	/* Finds the vectors from the center to the two end points, in units of the radii (eq. 5.5, 5.6). */
	start_x = (x1p - cxp) / rx;
	start_y = (y1p - cyp) / ry;
	end_x = (-x1p - cxp) / rx;
	end_y = (-y1p - cyp) / ry;

	/* Finds the start angle (eq. 5.5) and the angle between the vectors, signed by their cross product (eq. 5.4). */
	cross = start_x * end_y - start_y * end_x;
	dot = start_x * end_x + start_y * end_y;
	arc->theta1 = atan2f(start_y, start_x);
	arc->delta = atan2f(cross, dot);

	/* Fixes the sweep's sign by the sweep flag (the text under eq. 5.6). */
	if (sweep == 0 && arc->delta > 0.0f)
		arc->delta -= 2.0f * VEC_PI;
	if (sweep != 0 && arc->delta < 0.0f)
		arc->delta += 2.0f * VEC_PI;

	/* Succeeded: the arc is in center parameterization. */
	return 0;
}

/*
 * Maps a point of the unit circle to the arc's ellipse.
 */
static struct vec_point
path_arc_map(
	const struct path_arc *arc,
	float unit_x,
	float unit_y)
{
	struct vec_point mapped;

	/* Stretches by the radii, turns by the rotation and moves to the center. */
	mapped.x = arc->center.x + arc->rx * arc->cos_phi * unit_x - arc->ry * arc->sin_phi * unit_y;
	mapped.y = arc->center.y + arc->rx * arc->sin_phi * unit_x + arc->ry * arc->cos_phi * unit_y;

	/* Reports the point on the ellipse. */
	return mapped;
}

/*
 * Collects the cubic curves of an arc, one for each part of at most a
 * quarter turn.
 *
 * For a part from angle a to b on the unit circle, the handles are the
 * tangents at its ends, of length 4/3 * tan((b - a) / 4), which puts the
 * curve's midpoint on the circle; the ellipse is the image of the unit
 * circle under path_arc_map.  The last curve ends at exactly the arc's end
 * point so that rounding does not move the next command's start.
 */
static void
path_arc_emit(
	struct path_scratch *scratch,
	const struct path_arc *arc,
	struct vec_point end)
{
	struct vec_point control1;
	struct vec_point control2;
	struct vec_point point;
	float quarters;
	float step;
	float handle;
	float angle0;
	float angle1;
	float sin0;
	float cos0;
	float sin1;
	float cos1;
	int segments;
	int i;

	/* Finds how many curves of at most a quarter turn cover the sweep. */
	quarters = fabsf(arc->delta) / (VEC_PI * 0.5f);
	segments = (int)ceilf(quarters - PATH_ARC_QUARTER_SLACK);
	if (segments < 1)
		segments = 1;
	if (segments > PATH_ARC_SEGMENTS_MAX)
		segments = PATH_ARC_SEGMENTS_MAX;

	/* Finds the sweep of each curve and the handle length that fits it. */
	step = arc->delta / (float)segments;
	handle = (4.0f / 3.0f) * tanf(step * 0.25f);

	/* Collects each curve from its start angle to its end angle. */
	for (i = 0;
	     i < segments;
	     i++) {
		/* Finds the unit circle's points and tangent directions at the two ends. */
		angle0 = arc->theta1 + step * (float)i;
		angle1 = arc->theta1 + step * (float)(i + 1);
		sin0 = sinf(angle0);
		cos0 = cosf(angle0);
		sin1 = sinf(angle1);
		cos1 = cosf(angle1);

		/* Maps the two control points and the end point to the ellipse. */
		control1 = path_arc_map(arc, cos0 - handle * sin0, sin0 + handle * cos0);
		control2 = path_arc_map(arc, cos1 + handle * sin1, sin1 - handle * cos1);
		point = path_arc_map(arc, cos1, sin1);

		/* Ends the last curve at the exact end point. */
		if (i == segments - 1)
			point = end;

		/* Collects the curve. */
		scratch_cubic(scratch, control1.x, control1.y, control2.x, control2.y, point.x, point.y);
	}
}

/*
 * Finds where the derivative of a quadratic Bezier coordinate is zero.
 *
 * With the control values p0, p1, p2 the derivative is zero at
 * t = (p0 - p1) / (p0 - 2 p1 + p2).  Stores the root in roots[0] and
 * returns 1 when it lies strictly between 0 and 1, otherwise returns 0.
 */
static int
path_quad_roots(
	float p0,
	float p1,
	float p2,
	float roots[1])
{
	float denominator;
	float t;

	/* Gives no root when the coordinate is linear in t, which also guards the division. */
	denominator = p0 - 2.0f * p1 + p2;
	if (denominator == 0.0f)
		return 0;

	/* Keeps the root only when it lies inside the curve (a NaN fails both comparisons). */
	t = (p0 - p1) / denominator;
	if (!(t > 0.0f && t < 1.0f))
		return 0;

	/* Succeeded: one root inside the curve. */
	roots[0] = t;
	return 1;
}

/*
 * Finds where the derivative of a cubic Bezier coordinate is zero.
 *
 * With the control values p0 to p3 the derivative over 3 is
 * a t^2 + b t + c, with a = -p0 + 3 p1 - 3 p2 + p3, b = 2 (p0 - 2 p1 + p2)
 * and c = p1 - p0.  The roots come from the numerically stable form of the
 * quadratic formula (q = -(b + sign(b) sqrt(b^2 - 4 a c)) / 2, roots q / a
 * and c / q).  Stores the roots strictly between 0 and 1 in roots and
 * returns how many (0 to 2).
 */
static int
path_cubic_roots(
	float p0,
	float p1,
	float p2,
	float p3,
	float roots[2])
{
	float a;
	float b;
	float c;
	float discriminant;
	float root_sqrt;
	float q;
	float candidate[2];
	int candidates;
	int count;
	int i;

	/* Forms the coefficients of the derivative. */
	a = -p0 + 3.0f * p1 - 3.0f * p2 + p3;
	b = 2.0f * (p0 - 2.0f * p1 + p2);
	c = p1 - p0;

	/* Finds the candidates: the root of a line when a is 0, else the roots of the quadratic. */
	candidates = 0;
	if (a == 0.0f) {
		if (b != 0.0f) {
			candidate[0] = -c / b;
			candidates = 1;
		}
	} else {
		discriminant = b * b - 4.0f * a * c;
		if (discriminant >= 0.0f) {
			root_sqrt = sqrtf(discriminant);
			if (b < 0.0f)
				root_sqrt = -root_sqrt;
			q = -0.5f * (b + root_sqrt);
			candidate[0] = q / a;
			candidates = 1;
			if (q != 0.0f) {
				candidate[1] = c / q;
				candidates = 2;
			}
		}
	}

	/* Keeps the candidates strictly inside the curve (a NaN or an infinity fails the comparison). */
	count = 0;
	for (i = 0;
	     i < candidates;
	     i++) {
		if (candidate[i] > 0.0f && candidate[i] < 1.0f) {
			roots[count] = candidate[i];
			count++;
		}
	}

	/* Reports how many roots lie inside the curve. */
	return count;
}

/*
 * Evaluates a Bezier curve of degree 2 or 3 at a parameter by repeated
 * linear interpolation, then holds the point inside the box of the control
 * points, which the curve cannot leave, so that rounding never takes it out
 * of the coordinate range.
 */
static struct vec_point
path_curve_at(
	const struct vec_point *control,
	int degree,
	float t)
{
	struct vec_point work[4];
	struct vec_point result;
	float low_x;
	float high_x;
	float low_y;
	float high_y;
	int level;
	int i;

	/* Copies the control points and finds their box. */
	low_x = control[0].x;
	high_x = control[0].x;
	low_y = control[0].y;
	high_y = control[0].y;
	for (i = 0;
	     i <= degree;
	     i++) {
		work[i] = control[i];
		if (control[i].x < low_x)
			low_x = control[i].x;
		if (control[i].x > high_x)
			high_x = control[i].x;
		if (control[i].y < low_y)
			low_y = control[i].y;
		if (control[i].y > high_y)
			high_y = control[i].y;
	}

	/* Interpolates between neighbours until one point is left. */
	for (level = degree;
	     level > 0;
	     level--) {
		for (i = 0;
		     i < level;
		     i++) {
			work[i].x = work[i].x + (work[i + 1].x - work[i].x) * t;
			work[i].y = work[i].y + (work[i + 1].y - work[i].y) * t;
		}
	}
	result = work[0];

	/* Holds the point inside the box of the control points. */
	if (result.x < low_x)
		result.x = low_x;
	if (result.x > high_x)
		result.x = high_x;
	if (result.y < low_y)
		result.y = low_y;
	if (result.y > high_y)
		result.y = high_y;

	/* Reports the point on the curve. */
	return result;
}

/*
 * Grows a rectangle by the extreme points inside a Bezier curve.
 *
 * control holds degree + 1 points, the start first.  For each axis the
 * parameters where the coordinate's derivative is zero are found, and the
 * curve's point at each is included; the end points are the caller's.
 */
static void
path_bounds_curve(
	struct vec_rect *rect,
	const struct vec_point *control,
	int degree)
{
	struct vec_point point;
	float roots[2];
	float values[4];
	int count;
	int axis;
	int i;

	/* Finds the roots of each axis in turn. */
	for (axis = 0;
	     axis < 2;
	     axis++) {
		/* Takes the axis's coordinate of each control point. */
		for (i = 0;
		     i <= degree;
		     i++) {
			if (axis == 0)
				values[i] = control[i].x;
			else
				values[i] = control[i].y;
		}

		/* Finds where the coordinate is at an extreme. */
		if (degree == 2)
			count = path_quad_roots(values[0], values[1], values[2], roots);
		else
			count = path_cubic_roots(values[0], values[1], values[2], values[3], roots);

		/* Includes the curve's point at each extreme. */
		for (i = 0;
		     i < count;
		     i++) {
			point = path_curve_at(control, degree, roots[i]);
			vec_rect_include(rect, point);
		}
	}
}
