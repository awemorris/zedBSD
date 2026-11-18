/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shapes: the equivalent path of each SVG shape element (SVG 2
 * chapter 10 Basic Shapes, and §9.2 for 'path'), in the element's user
 * space.
 */

#ifndef KEILAND_BROWSER_SVG_SHAPE_H
#define KEILAND_BROWSER_SVG_SHAPE_H

#include "svg/svg-values.h"
#include "vector/vec-path.h"

/* What svg_shape_path made of an element. */
enum svg_shape_result {
	/* The element is a shape and out holds its path. */
	SVG_SHAPE_DRAWN = 0,
	/* The element is a shape that renders nothing (a zero or negative size, an empty or invalid 'd'): out is empty. */
	SVG_SHAPE_EMPTY = 1,
	/* The element is not a shape element. */
	SVG_SHAPE_NONE = 2
};

/*
 * Tells whether an element name is one of the shape elements: path, rect,
 * circle, ellipse, line, polyline, polygon.
 *
 * Returns 1 or 0 (0 for NULL).
 */
int svg_shape_is(const char *name);

/*
 * Builds the equivalent path of a shape element into out (appended; out is
 * usually empty).
 *
 *   path      'd' through svg_path_data_parse (an error in the data keeps
 *             the path up to it, §9.5.4).
 *   rect      x, y, width, height, rx, ry as lengths (SVG 2 §10.2):
 *             width or height <= 0 renders nothing; rx and ry auto (absent
 *             or invalid) take each other's value, both auto are 0, a
 *             negative one is an error and treated as auto; each is then
 *             clamped to half the width (rx) or height (ry).  With rx and
 *             ry > 0, vec_path_append_rounded_rect with the same radii at
 *             every corner; else vec_path_append_rect.
 *   circle    cx, cy, r (§10.3): r <= 0 renders nothing; else
 *             vec_path_append_ellipse with rx = ry = r.
 *   ellipse   cx, cy, rx, ry (§10.4): auto radii as for rect (each takes the
 *             other's); a radius <= 0 renders nothing.
 *   line      x1, y1, x2, y2 (§10.5): move to the first point, line to the
 *             second (an open subpath).
 *   polyline  points (§10.6): number pairs through svg_parse_numbers; an odd
 *             count drops the last number; an error keeps the pairs read
 *             before it; fewer than one pair renders nothing; move to the
 *             first pair and lines to the others (open).
 *   polygon   the same, closed (§10.7).
 * Lengths resolve with svg_length_resolve and context (x-axis for x, cx,
 * width, rx, x1, x2; y-axis for y, cy, height, ry, y1, y2; other for r).
 * A missing or invalid length is 0 (except the auto radii above).
 *
 * Input: tree and element (whose name is name, as tree->name gives it);
 * context of the element; out initialized.
 * Output: the result kind in *result.
 * Errors: the path's sticky error (ENOBUFS, ENOMEM, EINVAL for coordinates
 * beyond range); *result is then SVG_SHAPE_EMPTY and the caller draws
 * nothing of the element.
 */
int svg_shape_path(const struct svg_tree *tree, const struct svg_node *element, const char *name, const struct svg_context *context, struct vec_path *out, enum svg_shape_result *result);

#endif
