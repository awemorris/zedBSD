/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Flattening: a path turned into polylines (contours of straight
 * segments) within a tolerance, which the stroker and the rasterizer
 * read.  The same structure holds the outlines the stroker makes.
 */

#ifndef KEILAND_BROWSER_VECTOR_FLATTEN_H
#define KEILAND_BROWSER_VECTOR_FLATTEN_H

#include "vector/vec-path.h"

/*
 * The most vertices and contours one struct vec_lines holds (8 MiB of
 * vertices at the most).
 */
#define VEC_LINES_VERTICES_MAX	(1UL << 21)
#define VEC_LINES_CONTOURS_MAX	(1UL << 20)

/*
 * The most straight segments one curve is cut into.  Together with the
 * tolerance this bounds the work a hostile curve can cause; a curve that
 * would need more is cut into this many (its error may then exceed the
 * tolerance, which only an absurdly long curve reaches).
 */
#define VEC_FLATTEN_SEGMENTS_MAX	1024

/*
 * The smallest tolerance accepted: below a thousandth of a unit, a curve
 * would need more segments than VEC_FLATTEN_SEGMENTS_MAX anyway.
 */
#define VEC_TOLERANCE_MIN	0.001f

/*
 * The flags of a vertex:
 *   VEC_VERTEX_CORNER  the vertex is the end of a segment of the original
 *                      path (or a subpath's first point), where the
 *                      stroker draws the element's line join; a vertex
 *                      without it lies inside a flattened curve, where
 *                      the stroker joins round so the outline follows the
 *                      curve.
 */
#define VEC_VERTEX_CORNER	1U

/* One vertex of a contour: its position and its flags. */
struct vec_vertex {
	float x;
	float y;
	unsigned flags;
};

/*
 * One contour: vertices first to first + count - 1 of the vertex array, in
 * order, and whether it is closed (the subpath ended with a close; a
 * filled contour is always taken as closed by the rasterizer, a stroked
 * one is closed only when this says so).  A contour of one vertex is a
 * zero-length subpath (SVG 2 §9.5.3), which the stroker draws as caps
 * only.
 */
struct vec_contour {
	size_t first;
	size_t count;
	int closed;
};

/*
 * Polylines: the contours and their vertices, both from realloc and owned
 * by the structure (vec_lines_release frees them).  A zeroed structure is
 * a valid empty one.
 *
 * Readers (the stroker, the rasterizer) check that every contour lies in
 * the vertex array and every vertex is valid (vec_lines_check), so a
 * structure filled by untrusted code is refused, not overrun.
 */
struct vec_lines {
	struct vec_vertex *vertices;
	size_t vertex_count;
	size_t vertex_capacity;
	struct vec_contour *contours;
	size_t contour_count;
	size_t contour_capacity;
};

/* Makes empty polylines.  Input: lines not NULL. */
void vec_lines_init(struct vec_lines *lines);

/* Frees the arrays and leaves the polylines empty. */
void vec_lines_release(struct vec_lines *lines);

/* Empties the polylines but keeps their arrays for reuse. */
void vec_lines_reset(struct vec_lines *lines);

/*
 * Starts a new contour at a vertex, or adds a vertex to the last contour,
 * or marks the last contour closed: the building steps the flattener and
 * the stroker use, and a test may use.
 *
 * vec_lines_begin: a new contour with the one vertex (flags as given).
 * vec_lines_add: the vertex appended to the last contour; with no contour
 *   yet it begins one.
 * vec_lines_close: the last contour marked closed (no vertex added); with
 *   no contour it does nothing.
 *
 * Errors: EINVAL for a point that is not valid; ENOBUFS beyond
 * VEC_LINES_VERTICES_MAX or VEC_LINES_CONTOURS_MAX; ENOMEM.  The lines are
 * unchanged on error.
 */
int vec_lines_begin(struct vec_lines *lines, float x, float y, unsigned flags);
int vec_lines_add(struct vec_lines *lines, float x, float y, unsigned flags);
void vec_lines_close(struct vec_lines *lines);

/*
 * Checks polylines: every contour has count >= 1 and lies inside the
 * vertex array (first + count <= vertex_count, computed without
 * overflow), and every vertex is a valid point.
 *
 * Returns 0, or EINVAL.
 */
int vec_lines_check(const struct vec_lines *lines);

/*
 * Flattens a path into polylines appended to out.
 *
 * Each point of the path is first mapped by matrix (NULL for the
 * identity).  Lines stay single segments.  A quadratic or cubic Bezier
 * curve of degree n (SVG 2 §9.3.6, §9.3.7) is cut into N segments of
 * equal parameter steps, its points evaluated at t = i / N, where
 *
 *   N = ceil(sqrt(n * (n - 1) * D / (8 * tolerance)))
 *
 * and D is the largest length of the second differences of its mapped
 * control points (P[i] - 2 P[i+1] + P[i+2]).  This bound follows from
 * the second derivative of a Bezier curve, n (n - 1) times a convex
 * combination of those differences, and the error of a chord over a
 * parameter step h, at most h^2 / 8 times the second derivative's
 * largest length.  N is at least 1 and at most VEC_FLATTEN_SEGMENTS_MAX.
 *
 * Each subpath becomes one contour: its first vertex and the end of each
 * of its commands carry VEC_VERTEX_CORNER, the vertices inside curves do
 * not.  A segment whose end equals its start (zero length) is kept, so
 * the stroker can see SVG 2 §9.5.3's zero-length subpaths.  A subpath
 * ended by a close has closed set (no vertex is added for the closing
 * line: it is implied); when the subpath's last vertex equals its first,
 * that last vertex stays as it is.
 *
 * Input: path a view (checked here); matrix NULL or valid; tolerance the
 * greatest distance allowed between the curve and its chords, in the
 * units after the matrix, at least VEC_TOLERANCE_MIN.
 * Output: out has the new contours appended after those it had.
 * Errors: EINVAL for a malformed view, a matrix that is not valid, a
 * tolerance that is not finite or below VEC_TOLERANCE_MIN, or a mapped
 * point that is not valid; ENOBUFS; ENOMEM.  On error out is put back to
 * the contours it had before the call.
 */
int vec_flatten(const struct vec_path_view *path, const struct vec_matrix *matrix, float tolerance, struct vec_lines *out);

/*
 * Applies a matrix to every vertex of polylines in place.
 *
 * Errors: EINVAL when the matrix is not valid or a mapped vertex is not;
 * the lines are then left as they were.
 */
int vec_lines_transform(struct vec_lines *lines, const struct vec_matrix *matrix);

/*
 * Computes the bounding box of polylines' vertices.
 *
 * Errors: EINVAL for malformed lines or lines with no vertex; *bounds is
 * then unchanged.
 */
int vec_lines_bounds(const struct vec_lines *lines, struct vec_rect *bounds);

#endif
