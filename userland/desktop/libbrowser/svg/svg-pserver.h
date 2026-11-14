/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Paint servers: a 'linearGradient' or 'radialGradient' element resolved
 * into a vector gradient for one shape (SVG 2 §14.2).  Patterns (§14.3)
 * are not drawn.
 */

#ifndef KEILAND_BROWSER_SVG_PSERVER_H
#define KEILAND_BROWSER_SVG_PSERVER_H

#include "svg/svg-style.h"
#include "vector/vec-gradient.h"

/* The longest chain of href templates followed (SVG 2 §14.1.1); a longer one or a cycle stops there. */
#define SVG_TEMPLATE_DEPTH	16

/* What a paint server gives a shape. */
enum svg_server_kind {
	/* Nothing is painted: the server has no stops (§14.2.4.3), or its bounding box units meet a shape with no width or height (§8.11). */
	SVG_SERVER_NONE = 0,
	/* One color everywhere (one stop, or a linear gradient whose two points are the same: §14.2.2.2's last stop). */
	SVG_SERVER_COLOR = 1,
	/* A gradient. */
	SVG_SERVER_GRADIENT = 2
};

/*
 * A resolved paint server.  For SVG_SERVER_COLOR, color is the straight
 * color (stop-color with stop-opacity applied to its alpha).  For
 * SVG_SERVER_GRADIENT, gradient is complete except for to_gradient's
 * final composition: gradient.to_gradient maps the shape's user space to
 * gradient space (the inverse of gradientTransform after the
 * objectBoundingBox mapping), and the drawer composes it with the inverse
 * of user-to-device; gradient.stops points at stops (count of them),
 * owned here, freed by svg_server_release.  premultiplied is 0 (SVG's
 * interpolation of rgba values, §14.2.1).
 */
struct svg_server {
	enum svg_server_kind kind;
	uint32_t color;
	struct vec_gradient gradient;
	struct vec_stop *stops;
};

/* Makes an empty server (kind NONE, no stops). */
void svg_server_init(struct svg_server *server);

/* Frees the stops and leaves the server empty. */
void svg_server_release(struct svg_server *server);

/*
 * Resolves a gradient element for a shape.
 *
 * Attributes (SVG 2 §14.2.2.1, §14.2.3.1), each taken from the element
 * or, when it does not have it, from the template its href names, and so
 * on along the chain (§14.1.1; at most SVG_TEMPLATE_DEPTH elements, a
 * cycle ending the chain where it repeats): gradientUnits
 * (objectBoundingBox by default), gradientTransform, spreadMethod (pad by
 * default), and for linear x1, y1, x2, y2 (0%, 0%, 100%, 0%), for radial
 * cx, cy, r (50%, 50%, 50%), fx, fy (cx and cy), fr (0%).  A template of
 * the other gradient kind still gives the attributes both kinds have.
 * The stops are the element's own 'stop' children, or when it has none
 * those of the first template along the chain that has any (§14.2.4.3).
 *
 * Each stop (§14.2.4): offset a number or a percentage, clamped to
 * [0, 1]; stop-color and stop-opacity from its properties (svg_style_apply
 * on the stop, inheriting from the gradient element's style: the parent
 * passed here as gradient_style; currentColor taking the stop's color
 * property); the stops fixed up by vec_gradient_fixup_svg.
 *
 * Units (§8.11): with objectBoundingBox, lengths are fractions (a
 * percentage over 100, a number as is) of the box, and the box maps to
 * user space by translate(bbox.left, bbox.top) scale(width, height);
 * with userSpaceOnUse, lengths resolve against context (percentages of
 * the viewport).  The matrix chain: user space <- bbox mapping <-
 * gradientTransform <- gradient space; to_gradient is its inverse.  A
 * matrix that cannot be inverted paints nothing.
 *
 * Input: tree; element named "linearGradient" or "radialGradient";
 * gradient_style the style of the gradient element; bbox the shape's tight
 * bounding box in user space; context the shape's (for userSpaceOnUse
 * lengths); server initialized (its old contents are released).
 * Output: *server.
 * Errors: EINVAL when element is not a gradient element; ENOMEM.  The
 * server is then NONE.
 */
int svg_server_resolve(const struct svg_tree *tree, const struct svg_node *element, const struct svg_style *gradient_style, const struct vec_rect *bbox, const struct svg_context *context, struct svg_server *server);

#endif
