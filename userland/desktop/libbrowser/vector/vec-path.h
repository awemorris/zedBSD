/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A path: subpaths of straight lines and quadratic and cubic Bezier
 * curves, as SVG 2 chapter 9 (Paths) defines them, built one command at
 * a time and read back as a view.
 *
 * Elliptical arcs are not kept as arcs: vec_path_arc_to turns each into
 * cubic curves (SVG 2 Appendix B.2), so that every reader of a path
 * deals with three kinds of segment only.
 *
 * Builder errors stick: once a command fails (ENOMEM, ENOBUFS, EINVAL),
 * the path keeps that first error, later commands do nothing and return
 * it, and vec_path_error reports it.  A caller may therefore issue a run
 * of commands and check once at the end.
 */

#ifndef KEILAND_BROWSER_VECTOR_PATH_H
#define KEILAND_BROWSER_VECTOR_PATH_H

#include "vector/vec-base.h"

/*
 * The most points and the most verbs one path holds.  A million points is
 * far beyond any drawing a page shows (a detailed map is some tens of
 * thousands) and keeps the arrays under 8 MiB.
 */
#define VEC_PATH_POINTS_MAX	(1UL << 20)
#define VEC_PATH_VERBS_MAX	(1UL << 20)

/*
 * The commands of a path, as kept in its verb array, and how many points
 * each takes from its point array:
 *   VEC_VERB_MOVE   1: starts a subpath at the point.
 *   VEC_VERB_LINE   1: a straight line to the point.
 *   VEC_VERB_QUAD   2: a quadratic Bezier curve, its control point then
 *                      its end point (SVG 2 §9.3.7).
 *   VEC_VERB_CUBIC  3: a cubic Bezier curve, its two control points then
 *                      its end point (SVG 2 §9.3.6).
 *   VEC_VERB_CLOSE  0: a straight line back to the subpath's first point,
 *                      which ends the subpath as a closed one (SVG 2
 *                      §9.3.4).
 */
enum vec_verb {
	VEC_VERB_MOVE = 0,
	VEC_VERB_LINE = 1,
	VEC_VERB_QUAD = 2,
	VEC_VERB_CUBIC = 3,
	VEC_VERB_CLOSE = 4
};

/*
 * A path under construction, owned by its builder.
 *
 * verbs holds verb_count enum vec_verb values (one byte each) and points
 * point_count points, both from realloc and freed by vec_path_release.
 * Every subpath begins with a VEC_VERB_MOVE: a drawing command issued
 * right after a VEC_VERB_CLOSE first adds a VEC_VERB_MOVE to the closed
 * subpath's first point (SVG 2 §9.3.4).  current is the end of the last
 * command and start the first point of the subpath it belongs to; both are
 * meaningful only when has_current is set (after the first move_to).
 * closed is set while the last subpath has been closed and no command has
 * followed the close.  error is the first error a command met (0 for none).
 *
 * A zeroed structure is a valid empty path (vec_path_init gives the same).
 */
struct vec_path {
	unsigned char *verbs;
	size_t verb_count;
	size_t verb_capacity;
	struct vec_point *points;
	size_t point_count;
	size_t point_capacity;
	struct vec_point start;
	struct vec_point current;
	int has_current;
	int closed;
	int error;
};

/*
 * A read-only path: the verbs and the points of a struct vec_path, or
 * copies of them the caller keeps elsewhere (an arena).  The readers
 * (vec_flatten, vec_path_bounds) take views.  A view whose counts do not
 * agree (the points the verbs take are not exactly point_count, or the
 * first verb is not VEC_VERB_MOVE, or a verb is out of range) is refused
 * by every reader with EINVAL, so a view built from untrusted numbers is
 * safe to pass.
 */
struct vec_path_view {
	const unsigned char *verbs;
	size_t verb_count;
	const struct vec_point *points;
	size_t point_count;
};

/*
 * Makes an empty path.  Input: path not NULL.  Output: *path is empty with
 * no memory and no error.
 */
void vec_path_init(struct vec_path *path);

/*
 * Frees a path's arrays and leaves it empty (as vec_path_init does).
 * Input: path not NULL, initialized.
 */
void vec_path_release(struct vec_path *path);

/*
 * Empties a path but keeps its arrays for reuse, and clears its error.
 * Input: path not NULL, initialized.
 */
void vec_path_reset(struct vec_path *path);

/*
 * Reports the first error a command of the path met: 0, EINVAL, ENOBUFS
 * or ENOMEM.
 */
int vec_path_error(const struct vec_path *path);

/*
 * Starts a new subpath at a point (SVG 2 §9.3.3).  A move_to right after
 * another move_to leaves both (the first is a subpath of one point, which
 * the stroker draws as a zero-length subpath only when SVG 2 §9.5.3 says
 * so; vec_flatten keeps it).
 *
 * Errors (sticky, see the file comment): EINVAL when the point is not valid
 * (vec_point_valid); ENOBUFS when the path would pass VEC_PATH_POINTS_MAX
 * or VEC_PATH_VERBS_MAX; ENOMEM.  On error the path is not changed apart
 * from its error.
 */
int vec_path_move_to(struct vec_path *path, float x, float y);

/*
 * Adds a straight line from the current point (SVG 2 §9.3.5).
 *
 * Precondition: the path has a current point; a path without one keeps
 * EINVAL.  After a close, a VEC_VERB_MOVE to the closed subpath's first
 * point is added first (SVG 2 §9.3.4).
 * Errors: as vec_path_move_to, and EINVAL with no current point.
 */
int vec_path_line_to(struct vec_path *path, float x, float y);

/*
 * Adds a quadratic Bezier curve from the current point through the control
 * point (cx, cy) to (x, y) (SVG 2 §9.3.7).  Precondition and errors as
 * vec_path_line_to.
 */
int vec_path_quad_to(struct vec_path *path, float cx, float cy, float x, float y);

/*
 * Adds a cubic Bezier curve from the current point with the control points
 * (c1x, c1y) and (c2x, c2y) to (x, y) (SVG 2 §9.3.6).  Precondition and
 * errors as vec_path_line_to.
 */
int vec_path_cubic_to(struct vec_path *path, float c1x, float c1y, float c2x, float c2y, float x, float y);

/*
 * Adds an elliptical arc from the current point to (x, y), given in the
 * endpoint parameterization of SVG 2 §9.3.8: radii rx and ry, the
 * rotation of the ellipse's x axis in degrees, and the two flags (0 or
 * any other value for 1).  The arc is converted to the center
 * parameterization by SVG 2 Appendix B.2.4 (eq. 5.1 to 5.6)
 * after the radii are corrected by B.2.5, and is added as cubic Bezier
 * curves, one for each part of at most 90 degrees, whose control points
 * put the curve's midpoint on the ellipse (the handle length
 * 4/3 * tan(theta / 4) of the unit circle for a part of angle theta).
 *
 * The out-of-range cases of SVG 2 §9.5.1:
 *   - (x, y) equal to the current point: nothing is added (returns 0);
 *   - rx = 0 or ry = 0: a straight line to (x, y) is added;
 *   - negative radii: their absolute values are used;
 *   - radii too small: scaled up by B.2.5 (eq. 6.1 to 6.3).
 *
 * Precondition and errors as vec_path_line_to; EINVAL also when a radius
 * or the rotation is not finite.
 */
int vec_path_arc_to(struct vec_path *path, float rx, float ry, float rotation_degrees, int large_arc, int sweep, float x, float y);

/*
 * Closes the current subpath (SVG 2 §9.3.4): adds VEC_VERB_CLOSE, and the
 * current point becomes the subpath's first point.  Closing a subpath
 * that is already closed, or a path with no current point, does nothing
 * and returns 0.
 * Errors: ENOBUFS, ENOMEM (sticky).
 */
int vec_path_close(struct vec_path *path);

/*
 * Adds a closed rectangle as its own subpath: move to (x, y), lines to
 * (x + width, y), (x + width, y + height), (x, y + height), close.  This is
 * the equivalent path of an SVG 'rect' with no rounding (SVG 2 §10.2),
 * clockwise on the screen.
 *
 * Errors: EINVAL when a value is not finite, width or height is negative,
 * or a corner is not a valid point; ENOBUFS; ENOMEM (sticky).  A zero width
 * or height adds the degenerate rectangle as given.
 */
int vec_path_append_rect(struct vec_path *path, float x, float y, float width, float height);

/*
 * Adds a closed rectangle with elliptical corners as its own subpath,
 * clockwise from the end of the top left corner's curve: the equivalent
 * path of SVG 2 §10.2 when every corner has the same radii, and the shape
 * of CSS Backgrounds 3 §4.2 in general.
 *
 * radii[corner][axis]: corner 0 top left, 1 top right, 2 bottom right,
 * 3 bottom left; axis 0 horizontal, 1 vertical.  The caller has already
 * reduced them (vec_rrect_normalize, or SVG 2 §10.2's clamping): the two
 * radii along each side sum to at most that side's length.  A corner with
 * either radius 0 is square.  Each curved corner is a quarter ellipse
 * added as vec_path_arc_to would (one cubic Bezier curve).
 *
 * Errors: EINVAL when a value is not finite, width, height or a radius is
 * negative, or the radii along a side sum to more than its length plus
 * 1/1024 of it; ENOBUFS; ENOMEM (sticky).
 */
int vec_path_append_rounded_rect(struct vec_path *path, float x, float y, float width, float height, const float radii[4][2]);

/*
 * Adds a closed ellipse as its own subpath, the equivalent path of SVG 2
 * §10.4: it starts at (cx + rx, cy) and goes clockwise on the screen
 * through (cx, cy + ry), (cx - rx, cy) and (cx, cy - ry), each quarter one
 * cubic Bezier curve as in vec_path_arc_to.  A circle is rx = ry.
 *
 * Errors: EINVAL when a value is not finite or a radius is not positive;
 * ENOBUFS; ENOMEM (sticky).
 */
int vec_path_append_ellipse(struct vec_path *path, float cx, float cy, float rx, float ry);

/*
 * Applies a matrix to every point of a path (the curves stay curves: an
 * affine map of a Bezier curve's control points is the map of the curve).
 *
 * Errors: EINVAL when the matrix is not valid or a transformed point is
 * not (vec_point_valid); the path is then left as it was and keeps EINVAL.
 * A path that already has an error is left as it is and that error is
 * returned.
 */
int vec_path_transform(struct vec_path *path, const struct vec_matrix *matrix);

/*
 * Copies a path into another (source's verbs, points and current state;
 * not its error, which must be 0).
 *
 * Input: destination initialized (its old contents are replaced), source
 * a path with no error.
 * Errors: EINVAL when source has an error; ENOMEM, the destination then
 * being empty with ENOMEM as its error.
 */
int vec_path_copy(struct vec_path *destination, const struct vec_path *source);

/*
 * Gives the read-only view of a path (no copy: the view points into the
 * path's arrays, valid until the path is changed or released).
 */
void vec_path_view_of(const struct vec_path *path, struct vec_path_view *view);

/*
 * Checks that a view is well formed (the rules of struct vec_path_view)
 * and every point in it is valid.
 *
 * Returns 0, or EINVAL.
 */
int vec_path_view_check(const struct vec_path_view *view);

/*
 * Computes the tight bounding box of a path: the smallest rectangle that
 * holds every point of its geometry (SVG 2 §8.10, the "tight" bounding
 * box), from the end points and the extrema of each curve (the parameters
 * where a coordinate's derivative is zero, found from the derivative
 * polynomial of degree one or two; Bezier curves as in SVG 2 §9.3.6 and
 * §9.3.7).  The control points do not count unless they lie on the curve.
 * A subpath of a single move counts its point.
 *
 * Input: view checked or not (it is checked).  Output: *bounds on success.
 * Errors: EINVAL for a malformed view or an empty one (no verb); *bounds
 * is then unchanged.
 */
int vec_path_bounds(const struct vec_path_view *view, struct vec_rect *bounds);

#endif
