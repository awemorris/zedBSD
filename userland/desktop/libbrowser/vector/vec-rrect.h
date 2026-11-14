/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Rounded rectangles drawn analytically: the coverage of a box with
 * elliptical corners, of its border ring, which side of the border a
 * pixel belongs to, and the coverage of its blurred shadow, each as a
 * closed formula per pixel.
 *
 * These are the formulas the GPU's fragment shader evaluates for CSS
 * backgrounds, borders and box shadows (CSS Backgrounds 3 §4 and §6.1),
 * so that no pixel of them is touched by the CPU in a frame; the CPU
 * reference renderer evaluates the same formulas with these functions,
 * and the two agree up to the rounding of single precision.
 *
 * Pixels.  A function takes a pixel by its top left corner (px, py) in
 * the space of the rectangle (device pixels); the pixel is the square
 * [px, px + 1) x [py, py + 1) and its centre (px + 0.5, py + 0.5).
 */

#ifndef KEILAND_BROWSER_VECTOR_RRECT_H
#define KEILAND_BROWSER_VECTOR_RRECT_H

#include "vector/vec-base.h"

/* The corners, clockwise from the top left, as CSS lists border-radius. */
#define VEC_CORNER_TOP_LEFT	0
#define VEC_CORNER_TOP_RIGHT	1
#define VEC_CORNER_BOTTOM_RIGHT	2
#define VEC_CORNER_BOTTOM_LEFT	3

/* The sides, clockwise from the top, as CSS lists border widths. */
#define VEC_SIDE_TOP		0
#define VEC_SIDE_RIGHT		1
#define VEC_SIDE_BOTTOM		2
#define VEC_SIDE_LEFT		3

/*
 * The smallest standard deviation of a blur that is drawn as a blur; a
 * smaller one (a CSS blur radius under 1 px) is drawn sharp.
 */
#define VEC_SIGMA_MIN		0.5f

/*
 * The nodes of the midpoint rule over each half of a corner's curve in
 * vec_rrect_shadow (two halves per corner, so at most 8 * 16 evaluations
 * for a pixel near every corner).
 */
#define VEC_SHADOW_NODES	8

/*
 * The smallest radius kept: a radius under 1/256 of a pixel is made 0 by
 * vec_rrect_normalize (it draws nothing visible, and dividing by it would
 * overflow the corner formulas).
 */
#define VEC_RADIUS_MIN		(1.0f / 256.0f)

/*
 * A rectangle with elliptical corners: its rectangle and each corner's
 * radii, radii[corner][0] horizontal and radii[corner][1] vertical.
 * After vec_rrect_normalize: every radius is >= 0, a corner with either
 * radius 0 has both 0 (square), and the two radii along each side sum to
 * at most that side's length.
 */
struct vec_rrect {
	struct vec_rect rect;
	float radii[4][2];
};

/*
 * Makes the radii usable (CSS Backgrounds 3 §4.1, §4.5): a radius that is
 * not finite, is negative or is under VEC_RADIUS_MIN becomes 0; a corner
 * with one radius 0 gets both 0; then with f = min over the sides of
 * (side length / sum of the
 * two radii along it), taken over the sides whose sum is positive, all
 * radii are multiplied by f when f < 1.  An empty or inverted rectangle
 * gets all radii 0.
 *
 * Input: rrect not NULL, its rectangle finite.  Never fails.
 */
void vec_rrect_normalize(struct vec_rrect *rrect);

/*
 * Makes the inner edge of a border (the padding edge) from the outer edge
 * (the border edge) and the border widths (top, right, bottom, left):
 * the rectangle moved in by the widths (to zero size at the least), and
 * each radius the outer one less the width of the side it runs along,
 * at least 0 (CSS Backgrounds 3 §4.2): a horizontal radius less the left
 * or right width, a vertical one less the top or bottom width.  The result
 * is normalized.
 *
 * Input: outer normalized; widths finite and >= 0 (others are taken as 0).
 * Output: *inner.  Never fails.
 */
void vec_rrect_inset(const struct vec_rrect *outer, const float widths[4], struct vec_rrect *inner);

/*
 * Makes the edge of a box shadow or a spread outline from a border edge:
 * the rectangle grown by amount on each side (shrunk when amount is
 * negative, to zero size at the least) and each nonzero radius changed
 * per CSS Backgrounds 3 §6.1.1: for a spread s >= 0, a radius r becomes
 * r + s * (1 + (ratio - 1)^3) where ratio = r / s when r < s, or r + s
 * otherwise; for s < 0, max(0, r + s) (for a shrink §6.1.1's proportion
 * only ever gives 0 where it applies, so the plain floor is the same).
 * Each axis of each corner is changed independently.  A zero radius stays
 * 0.
 * The result is normalized.
 *
 * Input: border normalized; amount finite.  Output: *grown.  Never fails.
 */
void vec_rrect_spread(const struct vec_rrect *border, float amount, struct vec_rrect *grown);

/*
 * Computes how much of a pixel a rounded rectangle covers, in [0, 1].
 * The display list gives backgrounds, borders and clips rectangles whose
 * edges are whole pixels (rounded as PAINT_RECT's are, floor(v + 0.5)),
 * so the overlap below is 0 or 1 there and only the corners are
 * anti-aliased; the formula is defined for any edges.
 *
 *   overlap = (overlap of [px, px+1) with [left, right)) *
 *             (overlap of [py, py+1) with [top, bottom))
 *
 * exactly as a plain rectangle covers a pixel (the display list's rule for
 * PAINT_RECT); and when the pixel's centre (cx, cy) lies in a corner's box
 * (the rx x ry rectangle at that corner of the rectangle) with that
 * corner's ellipse centred at (ex, ey) inside it, the corner term
 *
 *   u = (cx - ex) / rx, v = (cy - ey) / ry,  g = u^2 + v^2 - 1,
 *   d = g / (2 * sqrt(u^2 / rx^2 + v^2 / ry^2))   (the distance to the
 *        ellipse in pixels to first order; d = -0.5 * min(rx, ry) when the
 *        root's argument is 0, the centre of the ellipse),
 *   corner = clamp(0.5 - d, 0, 1),
 *
 * and the coverage is min(overlap, corner).  Outside every corner box the
 * coverage is the overlap.
 *
 * Input: rrect normalized.  Returns the coverage.  Never fails; a pixel
 * position that is not finite gives 0.
 */
float vec_rrect_coverage(const struct vec_rrect *rrect, float px, float py);

/*
 * Computes how much of a pixel a border ring covers: the area between an
 * outer and an inner rounded rectangle,
 *   clamp(vec_rrect_coverage(outer) - vec_rrect_coverage(inner), 0, 1).
 *
 * Input: both normalized, inner made by vec_rrect_inset of outer.
 * Returns the coverage.  Never fails.
 */
float vec_rrect_ring_coverage(const struct vec_rrect *outer, const struct vec_rrect *inner, float px, float py);

/*
 * Computes the share of a pixel of a border that belongs to one side
 * (VEC_SIDE_*), so that a border whose sides differ in color is drawn
 * as four rings each weighted by its side's share; the shares of the four
 * sides of a pixel sum to 1 by construction.
 *
 * The outer rectangle (left, top, right, bottom) is cut into four
 * quadrants by its middle lines xm = (left + right) / 2 and
 * ym = (top + bottom) / 2; the pixel's centre (cx, cy) = (px + 0.5,
 * py + 0.5) picks one: cx < xm and cy < ym the top left, cx >= xm and
 * cy < ym the top right, cx >= xm and cy >= ym the bottom right, the rest
 * the bottom left.  Each quadrant has one corner and its two sides (top
 * left: top and left).  Only those two sides share the pixel; the other
 * two get 0.
 *
 * In a quadrant, the transition (CSS Backgrounds 3 §4.4) is the line
 * through the corner of the outer rectangle, O, and the corner of the
 * padding box, I (the outer rectangle moved in by the widths: for the top
 * left, O = (left, top), I = (left + width_left, top + width_top)).  Its
 * slope follows the ratio of the two widths, a monotonic function as §4.4
 * asks.  With s the signed distance of (cx, cy) from that line in pixels,
 * positive towards the horizontal side (top or bottom),
 *   horizontal side's share = clamp(0.5 + s, 0, 1),
 *   vertical side's share = 1 - that.
 * When the horizontal side's width is 0 the vertical side gets 1, when
 * the vertical side's width is 0 the horizontal side gets 1, and when both
 * are 0 the horizontal side gets 1.  (Where a box is thinner than its
 * borders the lines of two quadrants disagree at the middle line; the
 * share then jumps there, which only such degenerate boxes show.)
 *
 * Input: outer normalized; widths as for vec_rrect_inset; side 0 .. 3.
 * Returns the share in [0, 1] (0 for a side out of range).
 */
float vec_rrect_side_share(const struct vec_rrect *outer, const float widths[4], int side, float px, float py);

/*
 * Computes the error function erf(x) by Abramowitz and Stegun, Handbook
 * of Mathematical Functions, formula 7.1.26 (absolute error under
 * 1.5e-7 for every x), with erf(-x) = -erf(x); its exponential through
 * vec_exp, as the shader computes it.
 *
 * Returns erf(x) in [-1, 1]; 0 for a NaN.
 */
float vec_erf(float x);

/*
 * Computes the coverage of a rounded rectangle blurred by a Gaussian of
 * standard deviation sigma (CSS Backgrounds 3 §6.1.2: half the blur
 * radius), at the pixel's centre (cx, cy) = (px + 0.5, py + 0.5):
 *
 *   S = integral over y in [top, bottom) of G(y - cy) (E(R(y)) - E(L(y))) dy
 *
 * where G is the normal density of deviation sigma, L(y) and R(y) the
 * rounded rectangle's left and right at height y (the corners' ellipses),
 * and with k = 1 / (sigma sqrt 2):
 *   E(x)   = erf((x - cx) k) / 2,
 *   E'(x)  = k / sqrt(pi) * vec_exp(-((x - cx) k)^2),
 *   Psi(y) = (1 + erf((y - cy) k)) / 2   (so integral of G from y0 to y1
 *                                         is Psi(y1) - Psi(y0)),
 *   G(t)   = k / sqrt(pi) * vec_exp(-(t k)^2).
 * S is the right edge's term less the left edge's, each the integral of
 * G(y - cy) E(X(y)) for that edge's X, split into pieces:
 *   - the straight piece between the edge's two corners (from top plus
 *     the vertical radius of its top corner to bottom less that of its
 *     bottom corner), exactly: (Psi(y1) - Psi(y0)) E(X), X the side;
 *   - each corner of the edge with both radii nonzero, its quarter
 *     ellipse parameterized by theta in [0, pi / 2] (0 at the end that
 *     meets the straight piece, pi / 2 at the end on the top or bottom
 *     side), cut at theta45 = vec_atan2(ry, rx) where the curve's slope is
 *     one in these radii's terms, into two halves:
 *       steep half (theta in [0, theta45]): the integral of
 *         G(y - cy) E(x(y)) over its rows met with
 *         [cy - 4 sigma, cy + 4 sigma], by the midpoint rule with
 *         VEC_SHADOW_NODES equal steps in y (x(y) from the ellipse);
 *       flat half (theta in [theta45, pi / 2]), integrated by parts so the
 *         integration runs along x, where the curve is steep: with its
 *         end points (xa, ya) and (xb, yb) ordered so ya <= yb,
 *           Psi(yb) E(xb) - Psi(ya) E(xa) - integral from xa to xb of
 *             Psi(y(x)) E'(x) dx,
 *         the last integral (signed by its direction) over its columns met
 *         with [cx - 4 sigma, cx + 4 sigma] by the midpoint rule with
 *         VEC_SHADOW_NODES equal steps in x (y(x) from the ellipse).
 *     A half whose rows lie wholly outside [cy - 4 sigma, cy + 4 sigma]
 *     is skipped (its term is 0 to within erf's error), which is the
 *     early exit that keeps a large blur's pixels cheap.
 * The result is clamped to [0, 1].  A sigma under VEC_SIGMA_MIN gives
 * vec_rrect_coverage of the pixel.  §6.1.2 asks each pixel to be within
 * 5% of the exact blur: in double precision this scheme stays within
 * 0.009 of a dense numerical integration of the formula above for sigma
 * from 0.5 to 3 and corners from 8 to 100 pixels, round or elliptical.
 *
 * Input: rrect normalized; sigma finite and >= 0.  Returns the coverage in
 * [0, 1].  Never fails (a sigma that is not finite is taken as 0).
 */
float vec_rrect_shadow(const struct vec_rrect *rrect, float sigma, float px, float py);

#endif
