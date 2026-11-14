/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stroking: the outline of the stroke of flattened subpaths, as polygons
 * that filled together with the nonzero rule give the stroke shape of SVG
 * 2 §13.5.7.
 *
 * The outline is a union of simple pieces rather than one offset curve:
 * for each dash, a quadrilateral along each of its segments, a join piece
 * at each vertex between two of its segments, and a cap piece at each of
 * its ends.  Every piece is emitted as its own closed contour with a
 * positive signed area by the shoelace sum in these coordinates (y down,
 * so clockwise on the screen, as vec_path_append_rect goes), so that any
 * point inside one or more pieces has a nonzero
 * winding number and the nonzero fill is exactly their union.  The
 * rasterizer (vector/vec-raster.h) computes coverage from exact spans of
 * the winding number, so pieces that overlap or abut cost no seams.
 */

#ifndef KEILAND_BROWSER_VECTOR_STROKE_H
#define KEILAND_BROWSER_VECTOR_STROKE_H

#include "vector/vec-flatten.h"

/* The most dash lengths a stroke style takes (SVG allows any; real ones have a few). */
#define VEC_DASHES_MAX		64

/*
 * The most dashes one contour is cut into: when a dash pattern far
 * shorter than the path reaches this many dashes, the rest of the contour
 * after the start of the last one is drawn as that one dash, solid to the
 * contour's end (ENOBUFS is not returned for it).
 */
#define VEC_DASH_PIECES_MAX	65536

/* The shape at the ends of an open subpath or a dash (SVG 2 §13.5.4). */
enum vec_cap {
	VEC_CAP_BUTT = 0,
	VEC_CAP_ROUND = 1,
	VEC_CAP_SQUARE = 2
};

/*
 * The shape where two segments meet (SVG 2 §13.5.5).  SVG 2's miter-clip
 * and arcs are drawn as miter by the caller's choice (the stroke style has
 * no value for them).
 */
enum vec_join {
	VEC_JOIN_MITER = 0,
	VEC_JOIN_ROUND = 1,
	VEC_JOIN_BEVEL = 2
};

/*
 * How to stroke, in the units of the center lines (user units):
 *   width        the stroke width (SVG 2 §13.5.3): the outline reaches
 *                width / 2 to each side of the center line.  Must be
 *                finite and positive (a caller does not stroke with 0).
 *   cap, join    as above.
 *   miter_limit  the limit of SVG 2 §13.5.5: a miter join whose miter
 *                length over the width, 1 / sin(theta / 2) for the angle
 *                theta between the segments, exceeds it is drawn as a
 *                bevel.  Must be finite and >= 1.
 *   dashes       dash_count lengths, alternately dash and gap, or NULL
 *                with dash_count 0 for no dashing.  An odd count is
 *                repeated to an even one (SVG 2 §13.5.7).  Each must be
 *                finite and >= 0; when they sum to 0 there is no dashing.
 *   dash_offset  stroke-dashoffset, finite (any sign).
 *   dash_scale   the factor from the author's lengths to the measured
 *                ones for a 'pathLength' (SVG 2 §9.6.1, §13.5.6): every
 *                dash length and dash_offset is multiplied by it before
 *                the dashes are laid out on the measured contour, so
 *                dash_scale = (the measured length of the whole path, all
 *                its subpaths) / pathLength; 1 when there is none.
 *                Finite and positive (pathLength 0 is the caller's case:
 *                svg-picture.h).  SVG 2 §13.5.7's step "scale start and
 *                end by scale" with scale = authorlength / length reads
 *                the other way round; §9.6.1 is followed here (dashes are
 *                in the author's units of length).
 *   tolerance    the greatest distance between a round join or cap's arc
 *                and the polygon that stands for it, in the units of the
 *                output (after to_device).  At least VEC_TOLERANCE_MIN.
 */
struct vec_stroke_style {
	float width;
	enum vec_cap cap;
	enum vec_join join;
	float miter_limit;
	const float *dashes;
	size_t dash_count;
	float dash_offset;
	float dash_scale;
	float tolerance;
};

/*
 * Strokes polylines: appends to out the pieces of the stroke shape of
 * every contour of center, mapped by to_device.
 *
 * The stroke shape follows SVG 2 §13.5.7 with these choices:
 *   - Dashes: the dash positions of each contour by the algorithm of
 *     §13.5.7 ("The dash positions for a given subpath") applied to the
 *     dash lengths and offset multiplied by dash_scale, measured along the
 *     contour's polyline in center's units; a closed contour's length
 *     includes its closing segment.  For a
 *     closed contour whose first dash starts at 0 and last dash ends at
 *     the length, the two are one dash joined at the first vertex.
 *   - A segment piece: the rectangle of width `width` centred on the
 *     segment.  A zero-length segment contributes no rectangle.
 *   - A join at a vertex between two segments of one dash (and at the
 *     first vertex of a closed contour that is not dashed), the two
 *     segments being the nearest of nonzero length before and after the
 *     vertex (zero-length segments are passed over; a vertex with no such
 *     segment on one side has no join): when the
 *     vertex carries VEC_VERTEX_CORNER, the style's join; otherwise a
 *     round join.  Joins are drawn on the outer side of the turn only:
 *     bevel, the triangle between the vertex and the two outer corners;
 *     miter, that triangle and the triangle out to the miter point when
 *     the miter ratio is within miter_limit; round, the circular sector
 *     of radius width / 2 between the two outer corners.  Where the two
 *     segments are collinear (the turn is under 1e-6 radians) no join is
 *     drawn; where they turn back on themselves (180 degrees) a round
 *     join is a half disc and the others nothing.
 *   - Caps at each end of an open contour and of each dash: butt nothing;
 *     square the rectangle width / 2 long beyond the end; round the half
 *     disc.  The direction at an end is that of the nearest segment of
 *     nonzero length.  A contour of one vertex that is not closed (a
 *     moveto alone) draws nothing (§13.5.1).  A contour of one vertex that
 *     is closed, or of several whose segments all have zero length, is a
 *     zero-length subpath (§9.5.3): with round or square caps it draws the
 *     disc or the axis-aligned square of side width centred on the point
 *     (the direction of the positive x axis), with butt nothing.
 *   - Arcs (round joins and caps) are polygons with
 *       M = ceil(angle / (2 * acos(1 - tolerance / r)))
 *     equal steps, r = width / 2 measured after to_device by
 *     vec_matrix_scale_max, M at least 1 and at most 256; with
 *     tolerance >= r, M = ceil(angle / (pi / 2)).
 *   - Every piece goes to out as a closed contour of positive signed area
 *     after to_device (its vertices reversed when the map mirrors).  Its
 *     vertices carry no flags.
 *
 * Input: center checked here (vec_lines_check); style as described;
 * to_device NULL for the identity or a valid matrix.
 * Output: out has the pieces appended.
 * Errors: EINVAL for malformed lines, a style value out of its range, a
 * dash_count over VEC_DASHES_MAX with a non-NULL dashes or a NULL dashes
 * with a count, a matrix that is not valid, or a vertex that maps out of
 * range; ENOBUFS when out would pass its limits; ENOMEM.  On error out is
 * put back to the contours it had before the call.
 */
int vec_stroke(const struct vec_lines *center, const struct vec_stroke_style *style, const struct vec_matrix *to_device, struct vec_lines *out);

/*
 * Measures the length of a contour of polylines (the sum of its segments'
 * lengths, with the closing segment when it is closed): what SVG 2
 * §13.5.7 calls the user agent's computed length, from which a caller
 * finds dash_scale for a 'pathLength'.
 *
 * Returns the length (0 for a contour index out of range).
 */
float vec_contour_length(const struct vec_lines *lines, size_t contour);

#endif
