/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of vec-path: the builder commands and their sticky errors,
 * the limits, the arcs (each of the four flag combinations, the corrections
 * of the radii, the number of curves), the rectangle, rounded rectangle and
 * ellipse subpaths, the transform, copy and view check, the tight bounding
 * box, and a random run under the sanitizers.  Every expected value is a
 * hand calculation or follows from the spec's equations; no other
 * implementation's output is used.
 */

#include "check.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include "vector/vec-path.h"

/* The ratio of a circle's circumference to its diameter, in double precision, for the expected values. */
#define TEST_PI		3.14159265358979323846

/* The quarter ellipse handle: 4 / 3 * tan(pi / 8) = 4 * (sqrt(2) - 1) / 3, in double precision. */
#define TEST_KAPPA	0.5522847498307935

/* The verb a point of the path array belongs to is not recorded, so the tests keep a small cursor. */
struct test_cursor {
	const struct vec_path *path;
	size_t verb;
	size_t point;
};

/* Builds a point from its two components. */
static struct vec_point
make_point(
	float x,
	float y)
{
	struct vec_point point;

	point.x = x;
	point.y = y;
	return point;
}

/* Checks that a path holds exactly the given verbs. */
static void
check_verbs(
	const struct vec_path *path,
	const unsigned char *expected,
	size_t count)
{
	size_t i;

	CHECK_LONG(path->verb_count, count);
	if (path->verb_count != count)
		return;
	for (i = 0; i < count; i++)
		CHECK_LONG(path->verbs[i], expected[i]);
}

/* Checks the i'th point of a path against an expected pair. */
static void
check_point(
	const struct vec_path *path,
	size_t index,
	double x,
	double y,
	double tolerance)
{
	if (index >= path->point_count) {
		CHECK(index < path->point_count);
		return;
	}
	CHECK_NEAR(path->points[index].x, x, tolerance);
	CHECK_NEAR(path->points[index].y, y, tolerance);
}

/* Evaluates a cubic curve at t from its four control points, in double precision. */
static void
cubic_at(
	struct vec_point p0,
	struct vec_point p1,
	struct vec_point p2,
	struct vec_point p3,
	double t,
	double *x,
	double *y)
{
	double mt;

	mt = 1.0 - t;
	*x = mt * mt * mt * p0.x + 3.0 * mt * mt * t * p1.x + 3.0 * mt * t * t * p2.x + t * t * t * p3.x;
	*y = mt * mt * mt * p0.y + 3.0 * mt * mt * t * p1.y + 3.0 * mt * t * t * p2.y + t * t * t * p3.y;
}

/* Gives twice the signed area of a polygon by the shoelace sum; positive is clockwise with y down. */
static double
shoelace(
	const struct vec_point *points,
	size_t count)
{
	double sum;
	size_t i;
	size_t next;

	sum = 0.0;
	for (i = 0; i < count; i++) {
		next = (i + 1) % count;
		sum += (double)points[i].x * (double)points[next].y - (double)points[next].x * (double)points[i].y;
	}
	return sum;
}

/* Checks each command's verb, point count and values, and what a close and a later command do. */
static void
test_commands(void)
{
	static const unsigned char first[] = { VEC_VERB_MOVE, VEC_VERB_LINE, VEC_VERB_QUAD, VEC_VERB_CUBIC, VEC_VERB_CLOSE };
	static const unsigned char reopened[] = {
		VEC_VERB_MOVE, VEC_VERB_LINE, VEC_VERB_CLOSE, VEC_VERB_MOVE, VEC_VERB_CUBIC, VEC_VERB_CLOSE,
		VEC_VERB_MOVE, VEC_VERB_LINE
	};
	struct vec_path path;

	/* A new path is empty, with no error and no memory. */
	vec_path_init(&path);
	CHECK_LONG(path.verb_count, 0);
	CHECK_LONG(path.point_count, 0);
	CHECK_LONG(vec_path_error(&path), 0);
	CHECK(path.verbs == NULL);

	/* Each command adds its verb and its points in order: move 1, line 1, quad 2, cubic 3, close 0. */
	CHECK_LONG(vec_path_move_to(&path, 1.0f, 2.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 3.0f, 4.0f), 0);
	CHECK_LONG(vec_path_quad_to(&path, 5.0f, 6.0f, 7.0f, 8.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f), 0);
	CHECK_LONG(vec_path_close(&path), 0);
	check_verbs(&path, first, 5);
	CHECK_LONG(path.point_count, 7);
	check_point(&path, 0, 1.0, 2.0, 0.0);
	check_point(&path, 1, 3.0, 4.0, 0.0);
	check_point(&path, 2, 5.0, 6.0, 0.0);
	check_point(&path, 3, 7.0, 8.0, 0.0);
	check_point(&path, 4, 9.0, 10.0, 0.0);
	check_point(&path, 5, 11.0, 12.0, 0.0);
	check_point(&path, 6, 13.0, 14.0, 0.0);
	CHECK_LONG(vec_path_error(&path), 0);

	/* The close made the first point current: the state says so. */
	CHECK_LONG(path.has_current, 1);
	CHECK_LONG(path.closed, 1);
	CHECK_NEAR(path.current.x, 1.0, 0.0);
	CHECK_NEAR(path.current.y, 2.0, 0.0);

	/* A second close, with the subpath already closed, changes nothing and succeeds. */
	CHECK_LONG(vec_path_close(&path), 0);
	CHECK_LONG(path.verb_count, 5);
	vec_path_release(&path);

	/*
	 * After a close a drawing command first adds a move to the closed
	 * subpath's first point: M(0,0) L(1,0) Z, then a cubic from (0,0)
	 * gets M(0,0) before it, and after the next Z a line gets M(0,0)
	 * again.  A move_to after a close adds nothing extra.
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 1.0f, 0.0f), 0);
	CHECK_LONG(vec_path_close(&path), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 1.0f, 1.0f, 2.0f, 2.0f, 3.0f, 3.0f), 0);
	CHECK_LONG(vec_path_close(&path), 0);
	CHECK_LONG(vec_path_line_to(&path, 5.0f, 5.0f), 0);
	check_verbs(&path, reopened, 8);
	CHECK_LONG(path.point_count, 1 + 1 + 1 + 3 + 1 + 1);
	check_point(&path, 2, 0.0, 0.0, 0.0);
	check_point(&path, 6, 0.0, 0.0, 0.0);
	check_point(&path, 7, 5.0, 5.0, 0.0);
	CHECK_LONG(path.closed, 0);
	CHECK_LONG(vec_path_close(&path), 0);
	CHECK_LONG(vec_path_move_to(&path, 9.0f, 9.0f), 0);
	CHECK_LONG(path.verb_count, 10);
	CHECK_LONG(path.verbs[9], VEC_VERB_MOVE);
	vec_path_release(&path);

	/* Two moves in a row are both kept. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 1.0f, 1.0f), 0);
	CHECK_LONG(vec_path_move_to(&path, 2.0f, 2.0f), 0);
	CHECK_LONG(path.verb_count, 2);
	CHECK_LONG(path.point_count, 2);
	vec_path_release(&path);

	/* A close with no current point does nothing and succeeds. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_close(&path), 0);
	CHECK_LONG(path.verb_count, 0);
	CHECK_LONG(vec_path_error(&path), 0);
	vec_path_release(&path);

	/* Reset keeps the arrays but empties the path and clears the error. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_line_to(&path, 1.0f, 1.0f), EINVAL);
	CHECK_LONG(vec_path_move_to(&path, 1.0f, 1.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_error(&path), 0);
	CHECK_LONG(vec_path_move_to(&path, 1.0f, 1.0f), 0);
	CHECK_LONG(path.verb_count, 1);
	vec_path_reset(&path);
	CHECK_LONG(path.verb_count, 0);
	CHECK_LONG(path.point_count, 0);
	CHECK_LONG(path.has_current, 0);
	CHECK_LONG(path.closed, 0);
	CHECK(path.verbs != NULL);
	vec_path_release(&path);
	CHECK(path.verbs == NULL);
	CHECK(path.points == NULL);
}

/* Checks the refusals and that the first error sticks and later commands do nothing. */
static void
test_errors(void)
{
	struct vec_path path;
	float nan_value;

	nan_value = nanf("");

	/* A line with no current point is EINVAL, and the path keeps it: later valid commands do nothing. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_line_to(&path, 1.0f, 1.0f), EINVAL);
	CHECK_LONG(vec_path_error(&path), EINVAL);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), EINVAL);
	CHECK_LONG(vec_path_append_rect(&path, 0.0f, 0.0f, 1.0f, 1.0f), EINVAL);
	CHECK_LONG(vec_path_close(&path), EINVAL);
	CHECK_LONG(path.verb_count, 0);
	CHECK_LONG(path.point_count, 0);
	vec_path_release(&path);

	/* Quad, cubic and arc with no current point are EINVAL too. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_quad_to(&path, 1.0f, 1.0f, 2.0f, 2.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_cubic_to(&path, 1.0f, 1.0f, 2.0f, 2.0f, 3.0f, 3.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_arc_to(&path, 1.0f, 1.0f, 0.0f, 0, 0, 2.0f, 2.0f), EINVAL);
	vec_path_release(&path);

	/* A NaN, an infinity and a point past 2^24 are EINVAL; the path is unchanged but for its error. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, nan_value, 1.0f), EINVAL);
	CHECK_LONG(path.verb_count, 1);
	CHECK_LONG(path.point_count, 1);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, INFINITY), EINVAL);
	CHECK_LONG(path.verb_count, 0);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_move_to(&path, 16777218.0f, 0.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_move_to(&path, 16777216.0f, -16777216.0f), 0);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_quad_to(&path, 0.0f, 20000000.0f, 1.0f, 1.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 1.0f, 1.0f, nan_value, 2.0f, 3.0f, 3.0f), EINVAL);
	CHECK_LONG(path.verb_count, 1);
	vec_path_release(&path);
}

/* Checks the limits: the count where ENOBUFS starts, that the first error sticks, and that a failed multi-verb command adds nothing. */
static void
test_limits(void)
{
	struct vec_path path;
	size_t i;
	int error;

	/* Moves up to the verb limit all succeed; the next one is ENOBUFS. */
	vec_path_init(&path);
	error = 0;
	for (i = 0; i < VEC_PATH_VERBS_MAX; i++) {
		error = vec_path_move_to(&path, (float)(i % 1000), 1.0f);
		if (error != 0)
			break;
	}
	CHECK_LONG(error, 0);
	CHECK_LONG(path.verb_count, VEC_PATH_VERBS_MAX);
	CHECK_LONG(path.point_count, VEC_PATH_POINTS_MAX);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), ENOBUFS);
	CHECK_LONG(path.verb_count, VEC_PATH_VERBS_MAX);

	/* The first error sticks: a later invalid command reports ENOBUFS, not EINVAL. */
	CHECK_LONG(vec_path_move_to(&path, nanf(""), 0.0f), ENOBUFS);
	CHECK_LONG(vec_path_error(&path), ENOBUFS);
	vec_path_release(&path);

	/* A command that needs five verbs when four remain adds nothing at all. */
	vec_path_init(&path);
	error = 0;
	for (i = 0; i < VEC_PATH_VERBS_MAX - 4; i++) {
		error = vec_path_move_to(&path, 1.0f, 2.0f);
		if (error != 0)
			break;
	}
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_path_append_rect(&path, 0.0f, 0.0f, 1.0f, 1.0f), ENOBUFS);
	CHECK_LONG(path.verb_count, VEC_PATH_VERBS_MAX - 4);
	CHECK_LONG(path.point_count, VEC_PATH_VERBS_MAX - 4);
	vec_path_release(&path);

	/* An arc of three curves with room for one adds nothing. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 5.0f, 0.0f), 0);
	for (i = 1; i < VEC_PATH_VERBS_MAX - 1; i++) {
		error = vec_path_line_to(&path, 5.0f, 0.0f);
		if (error != 0)
			break;
	}
	CHECK_LONG(error, 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 5.0f, 0.0f, 1, 1, 0.0f, -5.0f), ENOBUFS);
	CHECK_LONG(path.verb_count, VEC_PATH_VERBS_MAX - 1);
	vec_path_release(&path);
}

/* Evaluates every cubic of a path whose verbs after the first move are all cubic: the sample points of each. */
static size_t
sample_curves(
	const struct vec_path *path,
	const struct vec_point *start,
	struct vec_point *samples,
	size_t capacity)
{
	struct vec_point current;
	size_t used;
	size_t index;
	size_t i;
	double x;
	double y;
	double t;
	int step;

	used = 0;
	current = *start;
	index = 0;
	for (i = 0; i < path->verb_count; i++) {
		if (path->verbs[i] == VEC_VERB_MOVE) {
			current = path->points[index];
			index++;
		} else if (path->verbs[i] == VEC_VERB_CUBIC) {
			/* Takes samples at t = 1/4, 1/2, 3/4 and 1. */
			for (step = 1; step <= 4; step++) {
				t = (double)step / 4.0;
				cubic_at(current, path->points[index], path->points[index + 1], path->points[index + 2], t, &x, &y);
				if (used < capacity) {
					samples[used].x = (float)x;
					samples[used].y = (float)y;
					used++;
				}
			}
			current = path->points[index + 2];
			index += 3;
		}
	}
	return used;
}

/* Checks the arcs: the cases of the spec's 9.5.1, the flags, the number of curves and the midpoints. */
static void
test_arc_cases(void)
{
	struct vec_path path;
	struct vec_path positive;
	double x;
	double y;

	/* The end point equal to the current point adds nothing, with no error. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 3.0f, 4.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 5.0f, 0.0f, 0, 1, 3.0f, 4.0f), 0);
	CHECK_LONG(path.verb_count, 1);
	CHECK_LONG(vec_path_error(&path), 0);

	/* A radius of 0 is a straight line to the end point, for either radius. */
	CHECK_LONG(vec_path_arc_to(&path, 0.0f, 5.0f, 0.0f, 0, 1, 10.0f, 4.0f), 0);
	CHECK_LONG(path.verb_count, 2);
	CHECK_LONG(path.verbs[1], VEC_VERB_LINE);
	check_point(&path, 1, 10.0, 4.0, 0.0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 0.0f, 0.0f, 1, 0, 20.0f, 4.0f), 0);
	CHECK_LONG(path.verb_count, 3);
	CHECK_LONG(path.verbs[2], VEC_VERB_LINE);
	check_point(&path, 2, 20.0, 4.0, 0.0);
	vec_path_release(&path);

	/* Negative radii are their absolute values: the same curves as the positive ones. */
	vec_path_init(&path);
	vec_path_init(&positive);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_move_to(&positive, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, -5.0f, -5.0f, 0.0f, 0, 1, 10.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&positive, 5.0f, 5.0f, 0.0f, 0, 1, 10.0f, 0.0f), 0);
	CHECK_LONG(path.point_count, positive.point_count);
	CHECK(path.point_count == positive.point_count &&
	    memcmp(path.points, positive.points, path.point_count * sizeof(path.points[0])) == 0);
	vec_path_release(&positive);
	vec_path_release(&path);

	/*
	 * The half circle (0, 0) to (10, 0), radius 5, sweep 1 (positive angle,
	 * clockwise on the screen): the center is (5, 0) and the arc goes over
	 * the top, so its midpoint is (5, -5), the end of the first of two
	 * curves.  With sweep 0 it goes below: (5, 5).  The large flag does
	 * not matter for a half circle.
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 5.0f, 0.0f, 0, 1, 10.0f, 0.0f), 0);
	CHECK_LONG(path.verb_count, 3);
	CHECK_LONG(path.verbs[1], VEC_VERB_CUBIC);
	CHECK_LONG(path.verbs[2], VEC_VERB_CUBIC);
	check_point(&path, 3, 5.0, -5.0, 1e-3);
	check_point(&path, 6, 10.0, 0.0, 0.0);
	vec_path_release(&path);

	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 5.0f, 0.0f, 1, 0, 10.0f, 0.0f), 0);
	CHECK_LONG(path.verb_count, 3);
	check_point(&path, 3, 5.0, 5.0, 1e-3);
	check_point(&path, 6, 10.0, 0.0, 0.0);
	vec_path_release(&path);

	/*
	 * Radii too small are scaled up (B.2.5): radius 1 for the chord of
	 * length 10 becomes 5, the same half circle.  An ellipse with rx = 2,
	 * ry = 1 over the same horizontal chord has Lambda = (5 / 2)^2 = 6.25,
	 * so both radii grow by sqrt(6.25) = 2.5 to rx = 5, ry = 2.5, and the
	 * half ellipse's top is at y = -2.5.
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 1.0f, 1.0f, 0.0f, 0, 1, 10.0f, 0.0f), 0);
	CHECK_LONG(path.verb_count, 3);
	check_point(&path, 3, 5.0, -5.0, 1e-3);
	vec_path_release(&path);

	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 2.0f, 1.0f, 0.0f, 0, 1, 10.0f, 0.0f), 0);
	CHECK_LONG(path.verb_count, 3);
	check_point(&path, 3, 5.0, -2.5, 1e-3);
	vec_path_release(&path);

	/*
	 * The same chord with radius 10 has two circles through the end
	 * points, with centers (5, +-8.660254) (the offset is
	 * sqrt(10^2 - 5^2)).  sweep 1 with the small arc takes the center
	 * below the chord, (5, 8.660254): the arc's midpoint is
	 * (5, 8.660254 - 10) = (5, -1.339746), a single 60 degree curve.
	 * The cubic's own midpoint is checked against the circle at t = 1/2.
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 10.0f, 10.0f, 0.0f, 0, 1, 10.0f, 0.0f), 0);
	CHECK_LONG(path.verb_count, 2);
	cubic_at(make_point(0.0f, 0.0f), path.points[1], path.points[2], path.points[3], 0.5, &x, &y);
	CHECK_NEAR(x, 5.0, 1e-3);
	CHECK_NEAR(y, -1.339746, 1e-3);
	vec_path_release(&path);

	/* sweep 0 with the small arc takes the center above the chord: the midpoint is (5, 1.339746). */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 10.0f, 10.0f, 0.0f, 0, 0, 10.0f, 0.0f), 0);
	CHECK_LONG(path.verb_count, 2);
	cubic_at(make_point(0.0f, 0.0f), path.points[1], path.points[2], path.points[3], 0.5, &x, &y);
	CHECK_NEAR(x, 5.0, 1e-3);
	CHECK_NEAR(y, 1.339746, 1e-3);
	vec_path_release(&path);

	/*
	 * The large arcs sweep 360 - 60 = 300 degrees, four curves of 75
	 * degrees.  large 1, sweep 1: the center is (5, -8.660254) and the
	 * arc passes the far point (5, -18.660254) at its middle, the end of
	 * the second curve.  large 1, sweep 0: the mirror, (5, 18.660254).
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 10.0f, 10.0f, 0.0f, 1, 1, 10.0f, 0.0f), 0);
	CHECK_LONG(path.verb_count, 5);
	check_point(&path, 6, 5.0, -18.660254, 1e-2);
	check_point(&path, 12, 10.0, 0.0, 0.0);
	vec_path_release(&path);

	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 10.0f, 10.0f, 0.0f, 1, 0, 10.0f, 0.0f), 0);
	CHECK_LONG(path.verb_count, 5);
	check_point(&path, 6, 5.0, 18.660254, 1e-2);
	check_point(&path, 12, 10.0, 0.0, 0.0);
	vec_path_release(&path);

	/* Any nonzero flag value means 1. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 10.0f, 10.0f, 0.0f, 7, -3, 10.0f, 0.0f), 0);
	CHECK_LONG(path.verb_count, 5);
	check_point(&path, 6, 5.0, -18.660254, 1e-2);
	vec_path_release(&path);

	/* A rotated ellipse: rx = 5, ry = 10 turned by 90 degrees is 10 wide and 5 tall, so the half ellipse from (0, 0) to (20, 0) peaks at y = -5. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 10.0f, 90.0f, 0, 1, 20.0f, 0.0f), 0);
	CHECK_LONG(path.verb_count, 3);
	check_point(&path, 3, 10.0, -5.0, 1e-3);
	vec_path_release(&path);

	/* The same ellipse without the rotation: rx = 10, ry = 5. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 10.0f, 5.0f, 0.0f, 0, 1, 20.0f, 0.0f), 0);
	check_point(&path, 3, 10.0, -5.0, 1e-3);
	vec_path_release(&path);

	/* An arc after a close starts at the closed subpath's first point and adds the move first. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, 4.0f, 0.0f), 0);
	CHECK_LONG(vec_path_close(&path), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 5.0f, 0.0f, 0, 1, 10.0f, 0.0f), 0);
	CHECK_LONG(path.verbs[3], VEC_VERB_MOVE);
	CHECK_LONG(path.verbs[4], VEC_VERB_CUBIC);
	check_point(&path, 2, 0.0, 0.0, 0.0);
	vec_path_release(&path);

	/* A NaN or infinite radius or rotation is EINVAL and sticks. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, nanf(""), 5.0f, 0.0f, 0, 0, 10.0f, 0.0f), EINVAL);
	CHECK_LONG(path.verb_count, 1);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, INFINITY, 0.0f, 0, 0, 10.0f, 0.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 5.0f, nanf(""), 0, 0, 10.0f, 0.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 5.0f, 0.0f, 0, 0, nanf(""), 0.0f), EINVAL);
	vec_path_release(&path);
}

/* Checks the number of curves by the sweep, and the control points of a quarter circle against the hand-computed handle. */
static void
test_arc_segments(void)
{
	struct vec_path path;
	double handle;

	/*
	 * The quarter circle of radius 5 from (5, 0) to (0, 5), sweep 1: the
	 * center is (0, 0), one curve, and the handles are kappa * 5 =
	 * 2.7614237 long along the tangents: (5, 2.7614237) and (2.7614237, 5).
	 */
	handle = 5.0 * TEST_KAPPA;
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 5.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 5.0f, 0.0f, 0, 1, 0.0f, 5.0f), 0);
	CHECK_LONG(path.verb_count, 2);
	check_point(&path, 1, 5.0, handle, 1e-4);
	check_point(&path, 2, handle, 5.0, 1e-4);
	check_point(&path, 3, 0.0, 5.0, 0.0);
	vec_path_release(&path);

	/* The same quarter taken the other way round (sweep 0, large 1) is 270 degrees: three curves, ending at the end point. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 5.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 5.0f, 0.0f, 1, 0, 0.0f, 5.0f), 0);
	CHECK_LONG(path.verb_count, 4);
	check_point(&path, 9, 0.0, 5.0, 0.0);
	vec_path_release(&path);

	/* The 270 degree arc (5, 0) to (0, -5), sweep 1, large 1: center (0, 0), three curves ending at (0, 5) and (-5, 0). */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 5.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 5.0f, 0.0f, 1, 1, 0.0f, -5.0f), 0);
	CHECK_LONG(path.verb_count, 4);
	check_point(&path, 3, 0.0, 5.0, 1e-3);
	check_point(&path, 6, -5.0, 0.0, 1e-3);
	check_point(&path, 9, 0.0, -5.0, 0.0);
	vec_path_release(&path);

	/* An arc of a few degrees is one curve; one just over a quarter is two. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 100.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 100.0f, 100.0f, 0.0f, 0, 1, 99.9847695f, 1.7452406f), 0);
	CHECK_LONG(path.verb_count, 2);
	vec_path_release(&path);

	/* A 100 degree arc of the unit circle: endpoints (1, 0) and (cos 100, sin 100) = (-0.17364818, 0.98480775). */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 1.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 1.0f, 1.0f, 0.0f, 0, 1, -0.17364818f, 0.98480775f), 0);
	CHECK_LONG(path.verb_count, 3);
	vec_path_release(&path);
}

/*
 * Finds the circle through three points: its center and radius.  Returns 0
 * for points in a line.
 */
static int
circumcircle(
	struct vec_point a,
	struct vec_point b,
	struct vec_point c,
	double *cx,
	double *cy,
	double *radius)
{
	double d;
	double a2;
	double b2;
	double c2;

	d = 2.0 * ((double)a.x * ((double)b.y - (double)c.y) + (double)b.x * ((double)c.y - (double)a.y) +
	    (double)c.x * ((double)a.y - (double)b.y));
	if (fabs(d) < 1e-9)
		return 0;
	a2 = (double)a.x * (double)a.x + (double)a.y * (double)a.y;
	b2 = (double)b.x * (double)b.x + (double)b.y * (double)b.y;
	c2 = (double)c.x * (double)c.x + (double)c.y * (double)c.y;
	*cx = (a2 * ((double)b.y - (double)c.y) + b2 * ((double)c.y - (double)a.y) + c2 * ((double)a.y - (double)b.y)) / d;
	*cy = (a2 * ((double)c.x - (double)b.x) + b2 * ((double)a.x - (double)c.x) + c2 * ((double)b.x - (double)a.x)) / d;
	*radius = sqrt(((double)a.x - *cx) * ((double)a.x - *cx) + ((double)a.y - *cy) * ((double)a.y - *cy));
	return 1;
}

/*
 * Checks random circular arcs against the geometry of the flags alone: every
 * sample of the curves lies on one circle of the given radius through both
 * end points, the sweep is positive (the angle from the x axis towards the y
 * axis grows) exactly when the sweep flag is set, and the arc is the larger
 * of the two exactly when the large arc flag is set.
 */
static void
test_arc_random_circles(void)
{
	struct vec_path path;
	struct vec_point samples[16];
	struct vec_point start;
	uint32_t seed;
	double cx;
	double cy;
	double radius;
	double distance;
	double angle;
	double previous;
	double step;
	double total;
	size_t count;
	size_t k;
	float x0;
	float y0;
	float x1;
	float y1;
	float r;
	int large;
	int sweep;
	int i;

	seed = 20260101;
	for (i = 0; i < 3000; i++) {
		x0 = check_random_float(&seed, -100.0f, 100.0f);
		y0 = check_random_float(&seed, -100.0f, 100.0f);
		x1 = check_random_float(&seed, -100.0f, 100.0f);
		y1 = check_random_float(&seed, -100.0f, 100.0f);
		distance = sqrt(((double)x1 - x0) * ((double)x1 - x0) + ((double)y1 - y0) * ((double)y1 - y0));
		if (distance < 5.0)
			continue;
		r = (float)(distance * 0.5 * (1.05 + 2.0 * (double)check_random_float(&seed, 0.0f, 1.0f)));
		large = (int)(check_random(&seed) & 1U);
		sweep = (int)(check_random(&seed) & 1U);

		vec_path_init(&path);
		CHECK_LONG(vec_path_move_to(&path, x0, y0), 0);
		CHECK_LONG(vec_path_arc_to(&path, r, r, 0.0f, large, sweep, x1, y1), 0);
		start = make_point(x0, y0);
		count = sample_curves(&path, &start, samples, 16);
		CHECK(count >= 4);

		/* The circle through the start, the first sample at t = 1/2 of the first curve, and the end. */
		if (!circumcircle(start, samples[1], make_point(x1, y1), &cx, &cy, &radius)) {
			vec_path_release(&path);
			continue;
		}
		CHECK_NEAR(radius / r, 1.0, 2e-3);

		/* Every sample is on that circle, within a relative 1e-3. */
		for (k = 0; k < count; k++) {
			distance = sqrt(((double)samples[k].x - cx) * ((double)samples[k].x - cx) +
			    ((double)samples[k].y - cy) * ((double)samples[k].y - cy));
			CHECK_NEAR(distance / r, 1.0, 1e-3);
		}

		/* Walks the angle around the center through the start, the samples and the end. */
		total = 0.0;
		previous = atan2((double)y0 - cy, (double)x0 - cx);
		for (k = 0; k < count; k++) {
			angle = atan2((double)samples[k].y - cy, (double)samples[k].x - cx);
			step = angle - previous;
			while (step > TEST_PI)
				step -= 2.0 * TEST_PI;
			while (step < -TEST_PI)
				step += 2.0 * TEST_PI;
			total += step;
			previous = angle;
		}

		/* The sweep is positive exactly when the flag is set, and over half a turn exactly when the large flag is set. */
		if (sweep)
			CHECK(total > 0.0);
		else
			CHECK(total < 0.0);
		if (large)
			CHECK(fabs(total) > TEST_PI);
		else
			CHECK(fabs(total) < TEST_PI);
		vec_path_release(&path);
	}
}

/* Checks the rectangle, rounded rectangle and ellipse subpaths. */
static void
test_shapes(void)
{
	static const unsigned char rect_verbs[] = { VEC_VERB_MOVE, VEC_VERB_LINE, VEC_VERB_LINE, VEC_VERB_LINE, VEC_VERB_CLOSE };
	static const unsigned char round_verbs[] = {
		VEC_VERB_MOVE, VEC_VERB_LINE, VEC_VERB_CUBIC, VEC_VERB_LINE, VEC_VERB_CUBIC, VEC_VERB_LINE, VEC_VERB_CUBIC,
		VEC_VERB_LINE, VEC_VERB_CUBIC, VEC_VERB_CLOSE
	};
	static const unsigned char ellipse_verbs[] = {
		VEC_VERB_MOVE, VEC_VERB_CUBIC, VEC_VERB_CUBIC, VEC_VERB_CUBIC, VEC_VERB_CUBIC, VEC_VERB_CLOSE
	};
	static const unsigned char mixed_verbs[] = {
		VEC_VERB_MOVE, VEC_VERB_LINE, VEC_VERB_LINE, VEC_VERB_CUBIC, VEC_VERB_LINE, VEC_VERB_LINE, VEC_VERB_CLOSE
	};
	struct vec_path path;
	struct vec_rrect rrect;
	struct vec_point corners[4];
	double x;
	double y;
	double ratio;
	double near;
	int corner;
	int step;

	/* A rectangle: move, three lines clockwise, close; four points; the area 3 * 4 = 12 in the shoelace sense. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rect(&path, 1.0f, 2.0f, 3.0f, 4.0f), 0);
	check_verbs(&path, rect_verbs, 5);
	CHECK_LONG(path.point_count, 4);
	check_point(&path, 0, 1.0, 2.0, 0.0);
	check_point(&path, 1, 4.0, 2.0, 0.0);
	check_point(&path, 2, 4.0, 6.0, 0.0);
	check_point(&path, 3, 1.0, 6.0, 0.0);
	CHECK_NEAR(shoelace(path.points, 4), 24.0, 1e-9);
	CHECK_LONG(path.closed, 1);
	CHECK_NEAR(path.current.x, 1.0, 0.0);
	CHECK_NEAR(path.current.y, 2.0, 0.0);

	/* A command after the rectangle starts from its first point, with the move. */
	CHECK_LONG(vec_path_line_to(&path, 9.0f, 9.0f), 0);
	CHECK_LONG(path.verb_count, 7);
	CHECK_LONG(path.verbs[5], VEC_VERB_MOVE);
	check_point(&path, 4, 1.0, 2.0, 0.0);
	vec_path_release(&path);

	/* A zero width or height gives the degenerate rectangle as given; a negative or non-finite value is refused. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rect(&path, 1.0f, 2.0f, 0.0f, 4.0f), 0);
	check_point(&path, 1, 1.0, 2.0, 0.0);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_append_rect(&path, 1.0f, 2.0f, -1.0f, 4.0f), EINVAL);
	CHECK_LONG(path.verb_count, 0);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_append_rect(&path, 1.0f, 2.0f, 3.0f, -0.5f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_append_rect(&path, nanf(""), 2.0f, 3.0f, 4.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_append_rect(&path, 1.0f, 2.0f, INFINITY, 4.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_append_rect(&path, 16000000.0f, 0.0f, 3000000.0f, 1.0f), EINVAL);
	CHECK_LONG(path.verb_count, 0);
	vec_path_release(&path);

	/*
	 * A rounded rectangle (10, 20) to (110, 70) with every corner 10 by
	 * 5: it starts at (20, 20), runs the top to (100, 20), curves to
	 * (110, 25), runs down to (110, 65), curves to (100, 70), runs to
	 * (20, 70), curves to (10, 65), runs up to (10, 25) and curves back to
	 * (20, 20).  Ten verbs; 17 points.
	 */
	memset(&rrect, 0, sizeof(rrect));
	rrect.rect.left = 10.0f;
	rrect.rect.top = 20.0f;
	rrect.rect.right = 110.0f;
	rrect.rect.bottom = 70.0f;
	for (corner = 0; corner < 4; corner++) {
		rrect.radii[corner][0] = 10.0f;
		rrect.radii[corner][1] = 5.0f;
	}
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), 0);
	check_verbs(&path, round_verbs, 10);
	CHECK_LONG(path.point_count, 17);
	check_point(&path, 0, 20.0, 20.0, 0.0);
	check_point(&path, 1, 100.0, 20.0, 0.0);
	near = 1.0 - TEST_KAPPA;
	check_point(&path, 2, 110.0 - near * 10.0, 20.0, 1e-4);
	check_point(&path, 3, 110.0, 20.0 + near * 5.0, 1e-4);
	check_point(&path, 4, 110.0, 25.0, 0.0);
	check_point(&path, 5, 110.0, 65.0, 0.0);
	check_point(&path, 8, 100.0, 70.0, 0.0);
	check_point(&path, 9, 20.0, 70.0, 0.0);
	check_point(&path, 12, 10.0, 65.0, 0.0);
	check_point(&path, 13, 10.0, 25.0, 0.0);
	check_point(&path, 16, 20.0, 20.0, 0.0);
	CHECK_NEAR(path.current.x, 20.0, 0.0);
	CHECK_NEAR(path.current.y, 20.0, 0.0);

	/* The outline is clockwise: the polygon of the points is positive in the shoelace sense. */
	CHECK(shoelace(path.points, path.point_count) > 0.0);
	vec_path_release(&path);

	/* One square corner (top left, a radius 0) is a line only: it starts at the corner itself. */
	rrect.radii[VEC_CORNER_TOP_LEFT][0] = 0.0f;
	rrect.radii[VEC_CORNER_TOP_LEFT][1] = 5.0f;
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), 0);
	CHECK_LONG(path.verb_count, 10 - 1);
	check_point(&path, 0, 10.0, 20.0, 0.0);
	CHECK_LONG(path.point_count, 17 - 3);
	vec_path_release(&path);

	/* Three square corners and the bottom right one curved: move, two lines, curve, two lines, close. */
	memset(&rrect.radii, 0, sizeof(rrect.radii));
	rrect.radii[VEC_CORNER_BOTTOM_RIGHT][0] = 10.0f;
	rrect.radii[VEC_CORNER_BOTTOM_RIGHT][1] = 5.0f;
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), 0);
	check_verbs(&path, mixed_verbs, 7);
	check_point(&path, 0, 10.0, 20.0, 0.0);
	check_point(&path, 1, 110.0, 20.0, 0.0);
	check_point(&path, 2, 110.0, 65.0, 0.0);
	check_point(&path, 5, 100.0, 70.0, 0.0);
	check_point(&path, 6, 10.0, 70.0, 0.0);
	check_point(&path, 7, 10.0, 20.0, 0.0);
	vec_path_release(&path);

	/* No curved corner at all is the plain rectangle. */
	memset(&rrect.radii, 0, sizeof(rrect.radii));
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), 0);
	check_verbs(&path, rect_verbs, 5);
	check_point(&path, 0, 10.0, 20.0, 0.0);
	check_point(&path, 2, 110.0, 70.0, 0.0);
	vec_path_release(&path);

	/* A pill: radii exactly half the height at the left and right (rx = 25, ry = 25 on a 100 x 50 box) fill the sides. */
	for (corner = 0; corner < 4; corner++) {
		rrect.radii[corner][0] = 25.0f;
		rrect.radii[corner][1] = 25.0f;
	}
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), 0);
	check_point(&path, 0, 35.0, 20.0, 0.0);
	check_point(&path, 4, 110.0, 45.0, 0.0);
	vec_path_release(&path);

	/* Radii that overfill a side by a rounding (sum 100.08 on 100) are scaled to fit: the start is at x = 10 + 50. */
	memset(&rrect.radii, 0, sizeof(rrect.radii));
	rrect.radii[VEC_CORNER_TOP_LEFT][0] = 50.04f;
	rrect.radii[VEC_CORNER_TOP_LEFT][1] = 5.0f;
	rrect.radii[VEC_CORNER_TOP_RIGHT][0] = 50.04f;
	rrect.radii[VEC_CORNER_TOP_RIGHT][1] = 5.0f;
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), 0);
	check_point(&path, 0, 60.0, 20.0, 2e-3);
	check_point(&path, 1, 60.0, 20.0, 2e-3);
	vec_path_release(&path);

	/* Radii that overfill a side by more than the slack are refused. */
	rrect.radii[VEC_CORNER_TOP_LEFT][0] = 60.0f;
	rrect.radii[VEC_CORNER_TOP_RIGHT][0] = 60.0f;
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), EINVAL);
	CHECK_LONG(path.verb_count, 0);
	vec_path_release(&path);

	/* A vertical overfill is refused as well. */
	memset(&rrect.radii, 0, sizeof(rrect.radii));
	rrect.radii[VEC_CORNER_TOP_RIGHT][0] = 5.0f;
	rrect.radii[VEC_CORNER_TOP_RIGHT][1] = 30.0f;
	rrect.radii[VEC_CORNER_BOTTOM_RIGHT][0] = 5.0f;
	rrect.radii[VEC_CORNER_BOTTOM_RIGHT][1] = 30.0f;
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), EINVAL);
	vec_path_release(&path);

	/* A negative radius, a NaN radius and an inverted rectangle are refused. */
	memset(&rrect.radii, 0, sizeof(rrect.radii));
	rrect.radii[VEC_CORNER_TOP_LEFT][0] = -1.0f;
	rrect.radii[VEC_CORNER_TOP_LEFT][1] = 1.0f;
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), EINVAL);
	vec_path_reset(&path);
	rrect.radii[VEC_CORNER_TOP_LEFT][0] = nanf("");
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), EINVAL);
	vec_path_reset(&path);
	memset(&rrect.radii, 0, sizeof(rrect.radii));
	rrect.rect.right = 5.0f;
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), EINVAL);
	vec_path_reset(&path);
	rrect.rect.right = 110.0f;
	rrect.rect.bottom = 10.0f;
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), EINVAL);
	vec_path_reset(&path);
	rrect.rect.bottom = nanf("");
	CHECK_LONG(vec_path_append_rounded_rect(&path, &rrect), EINVAL);
	vec_path_release(&path);

	/*
	 * An ellipse at (10, 20) with radii 5 and 3: from (15, 20) clockwise
	 * to (10, 23), (5, 20), (10, 17) and back; the handles are kappa times
	 * the radii.  Six verbs, 13 points, the polygon of the four extreme
	 * points has twice the area 2 * (10 * 6 / 2) = 60 in the shoelace sense.
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_ellipse(&path, 10.0f, 20.0f, 5.0f, 3.0f), 0);
	check_verbs(&path, ellipse_verbs, 6);
	CHECK_LONG(path.point_count, 13);
	check_point(&path, 0, 15.0, 20.0, 0.0);
	check_point(&path, 1, 15.0, 20.0 + TEST_KAPPA * 3.0, 1e-4);
	check_point(&path, 2, 10.0 + TEST_KAPPA * 5.0, 23.0, 1e-4);
	check_point(&path, 3, 10.0, 23.0, 0.0);
	check_point(&path, 4, 10.0 - TEST_KAPPA * 5.0, 23.0, 1e-4);
	check_point(&path, 5, 5.0, 20.0 + TEST_KAPPA * 3.0, 1e-4);
	check_point(&path, 6, 5.0, 20.0, 0.0);
	check_point(&path, 7, 5.0, 20.0 - TEST_KAPPA * 3.0, 1e-4);
	check_point(&path, 8, 10.0 - TEST_KAPPA * 5.0, 17.0, 1e-4);
	check_point(&path, 9, 10.0, 17.0, 0.0);
	check_point(&path, 10, 10.0 + TEST_KAPPA * 5.0, 17.0, 1e-4);
	check_point(&path, 11, 15.0, 20.0 - TEST_KAPPA * 3.0, 1e-4);
	check_point(&path, 12, 15.0, 20.0, 0.0);
	corners[0] = path.points[0];
	corners[1] = path.points[3];
	corners[2] = path.points[6];
	corners[3] = path.points[9];
	CHECK_NEAR(shoelace(corners, 4), 60.0, 1e-4);

	/* Each curve's midpoint is on the ellipse (x - 10)^2 / 25 + (y - 20)^2 / 9 = 1 within a relative 1e-3. */
	for (step = 0; step < 4; step++) {
		cubic_at(path.points[step * 3], path.points[step * 3 + 1], path.points[step * 3 + 2], path.points[step * 3 + 3],
		    0.5, &x, &y);
		ratio = (x - 10.0) * (x - 10.0) / 25.0 + (y - 20.0) * (y - 20.0) / 9.0;
		CHECK_NEAR(ratio, 1.0, 1e-3);
	}
	vec_path_release(&path);

	/* A circle is an ellipse with equal radii; a zero, negative or non-finite radius or a point out of range is refused. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_ellipse(&path, 0.0f, 0.0f, 2.0f, 2.0f), 0);
	CHECK_LONG(path.verb_count, 6);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_append_ellipse(&path, 0.0f, 0.0f, 0.0f, 2.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_append_ellipse(&path, 0.0f, 0.0f, 2.0f, -2.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_append_ellipse(&path, 0.0f, 0.0f, nanf(""), 2.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_append_ellipse(&path, nanf(""), 0.0f, 2.0f, 2.0f), EINVAL);
	vec_path_reset(&path);
	CHECK_LONG(vec_path_append_ellipse(&path, 16777216.0f, 0.0f, 10.0f, 2.0f), EINVAL);
	CHECK_LONG(path.verb_count, 0);
	vec_path_release(&path);
}

/* Checks the transform of a path. */
static void
test_transform(void)
{
	struct vec_path path;
	struct vec_matrix matrix;
	struct vec_path_view view;
	struct vec_rect bounds;

	/* Translating by (10, 20) moves every point and the current and first points. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 1.0f, 2.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f), 0);
	CHECK_LONG(vec_matrix_translate(&matrix, 10.0f, 20.0f), 0);
	CHECK_LONG(vec_path_transform(&path, &matrix), 0);
	check_point(&path, 0, 11.0, 22.0, 0.0);
	check_point(&path, 3, 17.0, 28.0, 0.0);
	CHECK_NEAR(path.start.x, 11.0, 0.0);
	CHECK_NEAR(path.current.x, 17.0, 0.0);
	CHECK_NEAR(path.current.y, 28.0, 0.0);

	/* A later command continues from the moved current point (the close returns to the moved first point). */
	CHECK_LONG(vec_path_close(&path), 0);
	CHECK_NEAR(path.current.x, 11.0, 0.0);
	CHECK_LONG(vec_path_line_to(&path, 0.0f, 0.0f), 0);
	check_point(&path, 4, 11.0, 22.0, 0.0);

	/* A scale that sends a point beyond the range is refused, the points stay and the error sticks. */
	CHECK_LONG(vec_matrix_scale(&matrix, 1.0e7f, 1.0f), 0);
	CHECK_LONG(vec_path_transform(&path, &matrix), EINVAL);
	check_point(&path, 0, 11.0, 22.0, 0.0);
	check_point(&path, 3, 17.0, 28.0, 0.0);
	CHECK_LONG(vec_path_error(&path), EINVAL);
	CHECK_LONG(vec_matrix_translate(&matrix, 1.0f, 1.0f), 0);
	CHECK_LONG(vec_path_transform(&path, &matrix), EINVAL);
	check_point(&path, 0, 11.0, 22.0, 0.0);
	vec_path_release(&path);

	/* An invalid matrix is refused. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 1.0f, 2.0f), 0);
	matrix.a = nanf("");
	matrix.b = 0.0f;
	matrix.c = 0.0f;
	matrix.d = 1.0f;
	matrix.e = 0.0f;
	matrix.f = 0.0f;
	CHECK_LONG(vec_path_transform(&path, &matrix), EINVAL);
	check_point(&path, 0, 1.0, 2.0, 0.0);
	vec_path_release(&path);

	/* A rotation of an ellipse by 45 degrees: the tight box has the half width sqrt((25 + 9) / 2) = 4.1231056 on both axes. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_ellipse(&path, 0.0f, 0.0f, 5.0f, 3.0f), 0);
	CHECK_LONG(vec_matrix_rotate(&matrix, 45.0f), 0);
	CHECK_LONG(vec_path_transform(&path, &matrix), 0);
	vec_path_view_of(&path, &view);
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.left, -4.1231056, 5e-3);
	CHECK_NEAR(bounds.right, 4.1231056, 5e-3);
	CHECK_NEAR(bounds.top, -4.1231056, 5e-3);
	CHECK_NEAR(bounds.bottom, 4.1231056, 5e-3);
	vec_path_release(&path);
}

/* Checks the copy of a path. */
static void
test_copy(void)
{
	struct vec_path source;
	struct vec_path copy;

	/* A copy has the same commands, and the source's builder state: building continues from its current point. */
	vec_path_init(&source);
	vec_path_init(&copy);
	CHECK_LONG(vec_path_move_to(&source, 1.0f, 2.0f), 0);
	CHECK_LONG(vec_path_line_to(&source, 3.0f, 4.0f), 0);
	CHECK_LONG(vec_path_copy(&copy, &source), 0);
	CHECK_LONG(copy.verb_count, 2);
	CHECK_LONG(copy.point_count, 2);
	check_point(&copy, 1, 3.0, 4.0, 0.0);
	CHECK(copy.verbs != source.verbs);
	CHECK(copy.points != source.points);
	CHECK_LONG(vec_path_line_to(&copy, 5.0f, 6.0f), 0);
	CHECK_LONG(copy.verb_count, 3);
	CHECK_LONG(source.verb_count, 2);

	/* A change to the source after the copy does not reach the copy. */
	CHECK_LONG(vec_path_line_to(&source, 7.0f, 8.0f), 0);
	CHECK_LONG(copy.verb_count, 3);
	check_point(&copy, 2, 5.0, 6.0, 0.0);

	/* Copying again replaces the old contents. */
	CHECK_LONG(vec_path_copy(&copy, &source), 0);
	CHECK_LONG(copy.verb_count, 3);
	check_point(&copy, 2, 7.0, 8.0, 0.0);

	/* A path copied onto itself is left alone. */
	CHECK_LONG(vec_path_copy(&copy, &copy), 0);
	CHECK_LONG(copy.verb_count, 3);

	/* A source with an error is refused. */
	CHECK_LONG(vec_path_line_to(&source, nanf(""), 0.0f), EINVAL);
	CHECK_LONG(vec_path_copy(&copy, &source), EINVAL);
	CHECK_LONG(copy.verb_count, 3);
	vec_path_release(&source);

	/* An empty path copies to an empty path with no memory. */
	vec_path_init(&source);
	CHECK_LONG(vec_path_copy(&copy, &source), 0);
	CHECK_LONG(copy.verb_count, 0);
	CHECK_LONG(copy.point_count, 0);
	CHECK_LONG(vec_path_error(&copy), 0);
	vec_path_release(&source);

	/* The closed state is copied: a line after the copied close adds its move. */
	vec_path_init(&source);
	CHECK_LONG(vec_path_append_rect(&source, 0.0f, 0.0f, 2.0f, 2.0f), 0);
	CHECK_LONG(vec_path_copy(&copy, &source), 0);
	CHECK_LONG(vec_path_line_to(&copy, 9.0f, 9.0f), 0);
	CHECK_LONG(copy.verbs[5], VEC_VERB_MOVE);
	vec_path_release(&source);
	vec_path_release(&copy);
}

/* Checks the view check against well formed and broken views. */
static void
test_view(void)
{
	static const unsigned char good[] = { VEC_VERB_MOVE, VEC_VERB_LINE, VEC_VERB_QUAD, VEC_VERB_CUBIC, VEC_VERB_CLOSE };
	static const unsigned char starts_with_line[] = { VEC_VERB_LINE, VEC_VERB_MOVE };
	static const unsigned char bad_verb[] = { VEC_VERB_MOVE, 5 };
	static const unsigned char big_verb[] = { VEC_VERB_MOVE, 255 };
	static const unsigned char two_moves[] = { VEC_VERB_MOVE, VEC_VERB_MOVE };
	struct vec_point points[8];
	struct vec_path_view view;
	struct vec_path path;
	size_t i;

	for (i = 0; i < 8; i++) {
		points[i].x = (float)i;
		points[i].y = (float)(i * 2);
	}

	/* The path built by the commands is well formed. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 1.0f, 2.0f), 0);
	CHECK_LONG(vec_path_quad_to(&path, 3.0f, 4.0f, 5.0f, 6.0f), 0);
	vec_path_view_of(&path, &view);
	CHECK_LONG(view.verb_count, 2);
	CHECK_LONG(view.point_count, 3);
	CHECK(view.verbs == path.verbs);
	CHECK(view.points == path.points);
	CHECK_LONG(vec_path_view_check(&view), 0);
	vec_path_release(&path);

	/* A view with exactly the points its verbs take (1 + 1 + 2 + 3 + 0 = 7) is well formed. */
	view.verbs = good;
	view.verb_count = 5;
	view.points = points;
	view.point_count = 7;
	CHECK_LONG(vec_path_view_check(&view), 0);

	/* One point short or one too many is refused. */
	view.point_count = 6;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);
	view.point_count = 8;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);

	/* The empty view is well formed; points with no verb are not. */
	view.verb_count = 0;
	view.point_count = 0;
	CHECK_LONG(vec_path_view_check(&view), 0);
	view.point_count = 1;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);

	/* A first verb that is not a move is refused. */
	view.verbs = starts_with_line;
	view.verb_count = 2;
	view.point_count = 2;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);

	/* A verb out of range is refused, whatever the points. */
	view.verbs = bad_verb;
	view.verb_count = 2;
	view.point_count = 1;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);
	view.verbs = big_verb;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);

	/* Two moves with their two points are fine. */
	view.verbs = two_moves;
	view.verb_count = 2;
	view.point_count = 2;
	CHECK_LONG(vec_path_view_check(&view), 0);

	/* A NaN point, an infinite point and a point beyond the range are refused. */
	points[1].x = nanf("");
	CHECK_LONG(vec_path_view_check(&view), EINVAL);
	points[1].x = 1.0f;
	points[0].y = INFINITY;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);
	points[0].y = 0.0f;
	points[1].x = 16777218.0f;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);
	points[1].x = 1.0f;
	CHECK_LONG(vec_path_view_check(&view), 0);

	/* Missing arrays for the counts are refused. */
	view.points = NULL;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);
	view.points = points;
	view.verbs = NULL;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);

	/* Counts past the limits are refused without reading the arrays. */
	view.verbs = two_moves;
	view.verb_count = VEC_PATH_VERBS_MAX + 1;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);
	view.verb_count = 2;
	view.point_count = VEC_PATH_POINTS_MAX + 1;
	CHECK_LONG(vec_path_view_check(&view), EINVAL);
}

/* Checks the tight bounding box on curves worked by hand. */
static void
test_bounds(void)
{
	static const unsigned char verbs_move_only[] = { VEC_VERB_MOVE, VEC_VERB_MOVE };
	static const unsigned char verbs_bad[] = { VEC_VERB_MOVE, VEC_VERB_LINE };
	static const unsigned char verbs_range[] = { VEC_VERB_MOVE, 77 };
	struct vec_path path;
	struct vec_path_view view;
	struct vec_rect bounds;
	struct vec_rect sentinel;
	struct vec_point points[3];

	sentinel.left = 1.0f;
	sentinel.top = 2.0f;
	sentinel.right = 3.0f;
	sentinel.bottom = 4.0f;

	/* A line: the box of its two points, whichever way it runs. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 5.0f, 1.0f), 0);
	CHECK_LONG(vec_path_line_to(&path, -3.0f, 7.0f), 0);
	vec_path_view_of(&path, &view);
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.left, -3.0, 0.0);
	CHECK_NEAR(bounds.top, 1.0, 0.0);
	CHECK_NEAR(bounds.right, 5.0, 0.0);
	CHECK_NEAR(bounds.bottom, 7.0, 0.0);
	vec_path_release(&path);

	/*
	 * A quadratic curve (0, 0), (5, 10), (10, 0): y(t) = 2 (1 - t) t 10,
	 * largest at t = 1/2 where y = 5, and x is monotonic.  The control
	 * point's y = 10 is not on the curve, so the box is (0, 0) to (10, 5).
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_quad_to(&path, 5.0f, 10.0f, 10.0f, 0.0f), 0);
	vec_path_view_of(&path, &view);
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.left, 0.0, 1e-6);
	CHECK_NEAR(bounds.top, 0.0, 1e-6);
	CHECK_NEAR(bounds.right, 10.0, 1e-6);
	CHECK_NEAR(bounds.bottom, 5.0, 1e-5);
	vec_path_release(&path);

	/* A quadratic curve with the extreme in x: (0, 0), (10, 5), (0, 10) reaches x = 5 at t = 1/2. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_quad_to(&path, 10.0f, 5.0f, 0.0f, 10.0f), 0);
	vec_path_view_of(&path, &view);
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.right, 5.0, 1e-5);
	CHECK_NEAR(bounds.left, 0.0, 1e-6);
	CHECK_NEAR(bounds.bottom, 10.0, 1e-6);
	vec_path_release(&path);

	/*
	 * A cubic (0, 0), (10, 10), (10, 10), (20, 0)... instead the plain
	 * case (0, 0), (0, 10), (10, 10), (10, 0): y(t) = 30 t (1 - t) peaks at
	 * t = 1/2 with 7.5 (the control points at 10 are not on the curve), and
	 * x(t) = 30 t^2 - 20 t^3 stays between 0 and 10.
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 0.0f, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f), 0);
	vec_path_view_of(&path, &view);
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.left, 0.0, 1e-6);
	CHECK_NEAR(bounds.top, 0.0, 1e-6);
	CHECK_NEAR(bounds.right, 10.0, 1e-6);
	CHECK_NEAR(bounds.bottom, 7.5, 1e-5);
	vec_path_release(&path);

	/*
	 * An S curve (0, 0), (10, 30), (20, -30), (30, 0): x is linear in t, y
	 * is 90 t (1 - t) (1 - 2 t), whose derivative 90 (1 - 6 t + 6 t^2)
	 * vanishes at t = (3 +- sqrt(3)) / 6 = 0.2113 and 0.7887 where y =
	 * +-5 sqrt(3) = +-8.660254.  The control points at +-30 are not on the
	 * curve.
	 */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 10.0f, 30.0f, 20.0f, -30.0f, 30.0f, 0.0f), 0);
	vec_path_view_of(&path, &view);
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.left, 0.0, 1e-5);
	CHECK_NEAR(bounds.right, 30.0, 1e-5);
	CHECK_NEAR(bounds.top, -8.660254, 1e-4);
	CHECK_NEAR(bounds.bottom, 8.660254, 1e-4);
	vec_path_release(&path);

	/* A cubic whose derivative has a double root and a straight-line cubic have no inside extreme. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_cubic_to(&path, 1.0f, 1.0f, 2.0f, 2.0f, 3.0f, 3.0f), 0);
	vec_path_view_of(&path, &view);
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.left, 0.0, 1e-6);
	CHECK_NEAR(bounds.top, 0.0, 1e-6);
	CHECK_NEAR(bounds.right, 3.0, 1e-6);
	CHECK_NEAR(bounds.bottom, 3.0, 1e-6);
	vec_path_release(&path);

	/* A move-only subpath counts its point: (1, 1) and (5, 7) alone make the box from (1, 1) to (5, 7). */
	points[0].x = 1.0f;
	points[0].y = 1.0f;
	points[1].x = 5.0f;
	points[1].y = 7.0f;
	view.verbs = verbs_move_only;
	view.verb_count = 2;
	view.points = points;
	view.point_count = 2;
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.left, 1.0, 0.0);
	CHECK_NEAR(bounds.top, 1.0, 0.0);
	CHECK_NEAR(bounds.right, 5.0, 0.0);
	CHECK_NEAR(bounds.bottom, 7.0, 0.0);

	/* A lone point is a zero size box at the point. */
	view.verb_count = 1;
	view.point_count = 1;
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.left, 1.0, 0.0);
	CHECK_NEAR(bounds.right, 1.0, 0.0);
	CHECK_NEAR(bounds.top, 1.0, 0.0);
	CHECK_NEAR(bounds.bottom, 1.0, 0.0);

	/* The closed rectangle's box is the rectangle. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_rect(&path, 2.0f, 3.0f, 4.0f, 5.0f), 0);
	vec_path_view_of(&path, &view);
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.left, 2.0, 0.0);
	CHECK_NEAR(bounds.top, 3.0, 0.0);
	CHECK_NEAR(bounds.right, 6.0, 0.0);
	CHECK_NEAR(bounds.bottom, 8.0, 0.0);
	vec_path_release(&path);

	/* The half circle arc (0, 0) to (10, 0), radius 5, over the top: the box has top -5 (the cubics' extreme). */
	vec_path_init(&path);
	CHECK_LONG(vec_path_move_to(&path, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_path_arc_to(&path, 5.0f, 5.0f, 0.0f, 0, 1, 10.0f, 0.0f), 0);
	vec_path_view_of(&path, &view);
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.left, 0.0, 1e-4);
	CHECK_NEAR(bounds.right, 10.0, 1e-4);
	CHECK_NEAR(bounds.top, -5.0, 1e-3);
	CHECK_NEAR(bounds.bottom, 0.0, 1e-4);
	vec_path_release(&path);

	/* The ellipse's tight box is its axes' extent. */
	vec_path_init(&path);
	CHECK_LONG(vec_path_append_ellipse(&path, 10.0f, 20.0f, 5.0f, 3.0f), 0);
	vec_path_view_of(&path, &view);
	CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
	CHECK_NEAR(bounds.left, 5.0, 1e-4);
	CHECK_NEAR(bounds.right, 15.0, 1e-4);
	CHECK_NEAR(bounds.top, 17.0, 1e-4);
	CHECK_NEAR(bounds.bottom, 23.0, 1e-4);
	vec_path_release(&path);

	/* An empty view is refused and the box is left alone. */
	bounds = sentinel;
	view.verbs = NULL;
	view.verb_count = 0;
	view.points = NULL;
	view.point_count = 0;
	CHECK_LONG(vec_path_bounds(&view, &bounds), EINVAL);
	CHECK(memcmp(&bounds, &sentinel, sizeof(bounds)) == 0);

	/* A broken view (a line with no point to go to, a verb out of range) is refused and the box is left alone. */
	view.verbs = verbs_bad;
	view.verb_count = 2;
	view.points = points;
	view.point_count = 1;
	CHECK_LONG(vec_path_bounds(&view, &bounds), EINVAL);
	CHECK(memcmp(&bounds, &sentinel, sizeof(bounds)) == 0);
	view.verbs = verbs_range;
	view.point_count = 1;
	CHECK_LONG(vec_path_bounds(&view, &bounds), EINVAL);
	CHECK(memcmp(&bounds, &sentinel, sizeof(bounds)) == 0);
}

/* Runs a hundred thousand random commands and checks the invariants after each batch, for the sanitizers to watch. */
static void
test_random(void)
{
	static const float specials[] = {
		0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 100.0f, -100.0f, 16777216.0f, -16777216.0f, 16777218.0f, 1.0e20f, -1.0e20f,
		1.0e-30f, 3.0e38f
	};
	struct vec_path path;
	struct vec_path copy;
	struct vec_path_view view;
	struct vec_rect bounds;
	struct vec_matrix matrix;
	struct vec_rrect rrect;
	unsigned char random_verbs[16];
	struct vec_point random_points[16];
	uint32_t seed;
	float v[8];
	size_t n;
	int command;
	int k;
	int i;
	int error;

	seed = 31337;
	vec_path_init(&path);
	vec_path_init(&copy);
	for (n = 0; n < 100000; n++) {
		/* Chooses seven values, a few of them awkward ones (NaN, infinity, huge, out of range). */
		for (k = 0; k < 8; k++) {
			switch (check_random(&seed) % 8) {
			case 0:
				v[k] = specials[check_random(&seed) % (sizeof(specials) / sizeof(specials[0]))];
				break;
			case 1:
				v[k] = ((check_random(&seed) & 1U) != 0) ? nanf("") : INFINITY;
				break;
			default:
				v[k] = check_random_float(&seed, -200.0f, 200.0f);
				break;
			}
		}

		/* Issues one command. */
		command = (int)(check_random(&seed) % 13);
		if (command == 0)
			error = vec_path_move_to(&path, v[0], v[1]);
		else if (command == 1 || command == 2)
			error = vec_path_line_to(&path, v[0], v[1]);
		else if (command == 3)
			error = vec_path_quad_to(&path, v[0], v[1], v[2], v[3]);
		else if (command == 4)
			error = vec_path_cubic_to(&path, v[0], v[1], v[2], v[3], v[4], v[5]);
		else if (command == 5 || command == 6)
			error = vec_path_arc_to(&path, v[0], v[1], v[2], (int)(check_random(&seed) & 3U),
			    (int)(check_random(&seed) & 3U), v[3], v[4]);
		else if (command == 7)
			error = vec_path_close(&path);
		else if (command == 8)
			error = vec_path_append_rect(&path, v[0], v[1], v[2], v[3]);
		else if (command == 9) {
			rrect.rect.left = v[0];
			rrect.rect.top = v[1];
			rrect.rect.right = v[2];
			rrect.rect.bottom = v[3];
			for (i = 0; i < 4; i++) {
				rrect.radii[i][0] = v[4 + (i & 1)];
				rrect.radii[i][1] = v[6 + (i >> 1)];
			}
			error = vec_path_append_rounded_rect(&path, &rrect);
		} else if (command == 10)
			error = vec_path_append_ellipse(&path, v[0], v[1], v[2], v[3]);
		else if (command == 11) {
			matrix.a = v[0] * 0.01f;
			matrix.b = v[1] * 0.01f;
			matrix.c = v[2] * 0.01f;
			matrix.d = v[3] * 0.01f;
			matrix.e = v[4];
			matrix.f = v[5];
			error = vec_path_transform(&path, &matrix);
		} else
			error = vec_path_copy(&copy, &path);

		/* A command that failed left the sticky error; the path is reset so the run goes on. */
		CHECK(error == 0 || error == EINVAL || error == ENOBUFS || error == ENOMEM);
		if (vec_path_error(&path) != 0 || path.verb_count > 3000)
			vec_path_reset(&path);

		/* Every so often the path must read back as a well formed view with a box that holds its points. */
		if ((n % 50) == 0 && path.verb_count > 0) {
			vec_path_view_of(&path, &view);
			CHECK_LONG(vec_path_view_check(&view), 0);
			CHECK_LONG(vec_path_bounds(&view, &bounds), 0);
			CHECK(bounds.left <= bounds.right && bounds.top <= bounds.bottom);
		}
	}
	vec_path_release(&path);
	vec_path_release(&copy);

	/* Random bytes as a view: the check and the box must stay inside the arrays whatever the verbs and counts are. */
	for (n = 0; n < 100000; n++) {
		for (k = 0; k < 16; k++) {
			random_verbs[k] = (unsigned char)(check_random(&seed) % 7);
			random_points[k].x = check_random_float(&seed, -50.0f, 50.0f);
			random_points[k].y = check_random_float(&seed, -50.0f, 50.0f);
		}
		if ((check_random(&seed) & 7U) != 0)
			random_verbs[0] = (unsigned char)VEC_VERB_MOVE;
		view.verbs = random_verbs;
		view.verb_count = check_random(&seed) % 17;
		view.points = random_points;
		view.point_count = check_random(&seed) % 17;
		error = vec_path_view_check(&view);
		CHECK(error == 0 || error == EINVAL);
		error = vec_path_bounds(&view, &bounds);
		CHECK(error == 0 || error == EINVAL);
	}
}

int
main(void)
{
	/* Runs every group of checks. */
	test_commands();
	test_errors();
	test_limits();
	test_arc_cases();
	test_arc_segments();
	test_arc_random_circles();
	test_shapes();
	test_transform();
	test_copy();
	test_view();
	test_bounds();
	test_random();

	/* Reports the result. */
	return check_finish("path");
}
