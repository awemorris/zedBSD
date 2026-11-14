/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Gradients: the colors of linear, radial and conic gradients, as CSS
 * Images 3 and 4 and SVG 2 chapter 14 define them, reduced to one model.
 *
 * Model.  A gradient maps each point to a parameter t, then t through the
 * spread method to [0, 1], then [0, 1] to a color by its stops.
 *
 *   linear  t is the projection of the point on the line from start
 *           (t = 0) to end (t = 1) (CSS Images 3 §3.1, SVG 2 §14.2.2).
 *   radial  two circles, the start circle (focal, focal_radius) and the end
 *           circle (center, radius): t is the largest w for which the
 *           point lies on the circle centred at focal + w (center - focal)
 *           with radius focal_radius + w (radius - focal_radius) >= 0, as
 *           the HTML Standard §4.12.5.1.10 (createRadialGradient) and SVG 2
 *           §14.2.3.2 define it.  A point on no such circle is not painted
 *           (transparent).  CSS's radial gradients are the case
 *           focal = center, focal_radius = 0 (CSS Images 3 §3.2), with an
 *           ellipse made by the matrix that maps the ellipse to a circle.
 *   conic   t is the angle of the point about center, clockwise from up,
 *           less start_angle, over a full turn, in [0, 1) (CSS Images 4
 *           §3.3).
 *
 * The geometry is in gradient space; to_gradient maps the space the
 * caller draws in (device pixels) to it, so one matrix carries SVG's
 * gradientUnits and gradientTransform and CSS's ellipse and placement.
 *
 * Colors.  Stops are straight (not premultiplied) 0xAARRGGBB colors at
 * offsets in [0, 1] after fixup, in order.  Between two stops the color
 * is interpolated per channel in gamma-encoded sRGB: in premultiplied form
 * for CSS (CSS Images 3 §3.4.2, CSS Color 4 §12.1 for legacy sRGB colors
 * and §12.3), or straight for SVG (SVG 2 §14.2.1's formula interpolates
 * the rgba values).  A transition hint (CSS Images 4 §3.5.2) between two
 * stops bends the interpolation.  Before the first stop the color is the
 * first stop's, after the last the last's.
 *
 * Ramps.  A renderer does not evaluate stops per pixel: it samples a ramp,
 * a row of width colors where texel i holds the color at
 * t = (i + 0.5) / width, looked up for a t in [0, 1] at
 * index = min(floor(t * width), width - 1).  The CPU reference renderer and
 * the GPU shader both look colors up this way, so they agree; the ramp is
 * made once when the content changes.
 */

#ifndef KEILAND_BROWSER_VECTOR_GRADIENT_H
#define KEILAND_BROWSER_VECTOR_GRADIENT_H

#include "vector/vec-base.h"

/* The most stops a gradient takes (SVG gives no limit; this is far above use). */
#define VEC_GRADIENT_STOPS_MAX	256

/*
 * The most texels of a ramp: one per device pixel of a gradient line of
 * this length, so a hard stop lands within half a pixel; a longer line
 * shares texels between pixels.  It is the width of the ramp texture.
 */
#define VEC_RAMP_WIDTH_MAX	2048

/* The fewest texels of a ramp. */
#define VEC_RAMP_WIDTH_MIN	2

/* The kinds of gradient. */
enum vec_gradient_kind {
	VEC_GRADIENT_LINEAR = 0,
	VEC_GRADIENT_RADIAL = 1,
	VEC_GRADIENT_CONIC = 2
};

/*
 * What happens to t outside [0, 1] (SVG 2 §14.2.2.1 spreadMethod; CSS's
 * repeating-*-gradient() is REPEAT over the stops' own range, which the
 * CSS caller maps to [0, 1] before):
 *   PAD      t clamped to [0, 1].
 *   REFLECT  t - 2 * floor(t / 2), then 2 - that when it is over 1.
 *   REPEAT   t - floor(t).
 */
enum vec_spread {
	VEC_SPREAD_PAD = 0,
	VEC_SPREAD_REFLECT = 1,
	VEC_SPREAD_REPEAT = 2
};

/*
 * One color stop: its offset on the gradient line, its straight color,
 * whether its offset was given (CSS stops may leave it to the fixup;
 * SVG's always give one), and the transition hint after it: the place in
 * (0, 1) between this stop and the next where the color is halfway, or a
 * value <= 0 for none (the plain linear interpolation).
 */
struct vec_stop {
	float offset;
	uint32_t color;
	int has_offset;
	float hint;
};

/*
 * A gradient.  stops points at count stops owned by the caller (fixed up
 * and in order: vec_gradient_fixup_css or vec_gradient_fixup_svg).
 * premultiplied selects the interpolation (1 for CSS, 0 for SVG).
 * The geometry fields used depend on kind: start and end (linear);
 * focal, focal_radius, center, radius (radial); center and start_angle in
 * radians (conic).
 */
struct vec_gradient {
	enum vec_gradient_kind kind;
	enum vec_spread spread;
	int premultiplied;
	const struct vec_stop *stops;
	size_t count;
	struct vec_matrix to_gradient;
	struct vec_point start;
	struct vec_point end;
	struct vec_point focal;
	float focal_radius;
	struct vec_point center;
	float radius;
	float start_angle;
};

/*
 * Fixes up CSS color stops (CSS Images 3 §3.4.3, with CSS Images 4
 * §3.5.3's hints): offsets are already fractions of the gradient line
 * (the caller resolved lengths and percentages; any value, not only in
 * [0, 1]).
 *   1. A first stop without an offset gets 0, a last one 1.
 *   2. A stop whose offset is less than any before it gets the largest
 *      before it.
 *   3. A run of stops without offsets between two with offsets is spread
 *      evenly between them.
 * Every stop then has has_offset set.  A hint outside the open interval
 * (0, 1) is set to -1 (none).
 *
 * Input: count stops, 1 .. VEC_GRADIENT_STOPS_MAX, each finite where it has
 * an offset.  Output: the stops changed in place.
 * Errors: EINVAL for a count out of range or a given offset or a hint that
 * is not finite; the stops are then unchanged.
 */
int vec_gradient_fixup_css(struct vec_stop *stops, size_t count);

/*
 * Fixes up SVG gradient stops (SVG 2 §14.2.4.1 and §14.2.4.3): each offset
 * clamped to [0, 1], and an offset less than the one before it set to it.
 * Hints are set to -1.
 *
 * Errors: as vec_gradient_fixup_css (every SVG stop has an offset).
 */
int vec_gradient_fixup_svg(struct vec_stop *stops, size_t count);

/*
 * Maps the CSS stops of a repeating gradient onto [0, 1]: the stops span
 * [first offset, last offset]; each offset becomes
 * (offset - first) / (last - first), and *start and *length report first
 * and last - first so the caller scales the geometry (the gradient line
 * then runs over one repetition and the spread is VEC_SPREAD_REPEAT).
 * When last - first is not positive (CSS Images 3 §3.3's degenerate case)
 * nothing is changed and ENOTSUP tells the caller to paint the average
 * color (vec_gradient_average).
 *
 * Input: stops fixed up.  Errors: EINVAL for a count out of range; ENOTSUP.
 */
int vec_gradient_repeat_span(struct vec_stop *stops, size_t count, float *start, float *length);

/*
 * Reports the color at a position on the gradient line, interpolated as
 * the file comment says, premultiplied, as 0xAARRGGBB with each channel
 * rounded to the nearest of 0 .. 255 (and no color channel above alpha).
 *
 * Input: stops fixed up, 1 .. VEC_GRADIENT_STOPS_MAX; position in [0, 1]
 * (a value outside is clamped; a NaN is taken as 0).
 * Returns the color.  Never fails.
 */
uint32_t vec_gradient_color_at(const struct vec_stop *stops, size_t count, int premultiplied, float position);

/*
 * Reports the average color of the stops over [0, 1], weighted by the
 * length of the line each pair of stops colors (the area under the
 * piecewise interpolation, premultiplied, as 0xAARRGGBB): what a
 * degenerate repeating gradient paints (CSS Images 3 §3.3, SVG 2
 * §14.2.3.1's focal point on the circle with repeat).
 *
 * Input as vec_gradient_color_at.  Never fails.
 */
uint32_t vec_gradient_average(const struct vec_stop *stops, size_t count, int premultiplied);

/*
 * Chooses a ramp's width for a gradient line of a length in device
 * pixels: ceil(length) clamped to [VEC_RAMP_WIDTH_MIN, VEC_RAMP_WIDTH_MAX]
 * (a length that is not finite gives VEC_RAMP_WIDTH_MAX).
 */
int vec_gradient_ramp_width(float length);

/*
 * Fills a ramp: width texels, texel i the color at (i + 0.5) / width
 * (vec_gradient_color_at), premultiplied 0xAARRGGBB.
 *
 * Input: gradient with stops fixed up; width in
 * [VEC_RAMP_WIDTH_MIN, VEC_RAMP_WIDTH_MAX]; ramp room for width colors.
 * Errors: EINVAL for a width out of range or stops out of range; the ramp
 * is then not written.
 */
int vec_gradient_ramp(const struct vec_gradient *gradient, int width, uint32_t *ramp);

/*
 * Computes a point's parameter t before the spread (the model in the file
 * comment), the point first mapped by to_gradient.
 *
 * For radial: with cd = center - focal, pd = p - focal, dr = radius -
 * focal_radius, the w on whose circle p lies solve
 *   (cd.cd - dr^2) w^2 - 2 (pd.cd + focal_radius dr) w + (pd.pd - focal_radius^2) = 0;
 * the largest root with focal_radius + w dr >= 0 is t, and none means the
 * point is not painted.  When the quadratic's leading coefficient is under
 * 1e-6 in magnitude it is solved as linear.  Both circles the same
 * (cd = 0 and dr = 0) paint nothing (SVG 2 §14.2.3.2).
 * For conic: angle = atan2(x, -y) about center (0 straight up, growing
 * clockwise on the screen), t = (angle - start_angle) / (2 pi) reduced to
 * [0, 1).
 * For linear with start = end (closer than 1e-6), t is 1 everywhere: SVG 2
 * §14.2.2.2 paints the last stop's color (a CSS gradient line always has a
 * length unless its box is empty, where nothing is painted anyway).
 *
 * Output: *painted is 1 when the point is painted, 0 when it is not
 * (radial only).  Returns t (0 when not painted).  Never fails; a gradient
 * whose numbers are not finite paints nothing.
 */
float vec_gradient_parameter(const struct vec_gradient *gradient, float x, float y, int *painted);

/*
 * Applies a spread method to t (enum vec_spread).  A t that is not finite
 * gives 0.
 *
 * Returns a value in [0, 1].
 */
float vec_gradient_spread(enum vec_spread spread, float t);

/*
 * Reports the color a renderer paints at a point from a ramp: t from
 * vec_gradient_parameter, spread, the ramp texel at
 * min(floor(t * width), width - 1); transparent (0) where not painted.
 * This is the CPU reference renderer's per-pixel color; the GPU shader
 * computes the same steps in single precision.
 *
 * Returns a premultiplied 0xAARRGGBB.  Never fails.
 */
uint32_t vec_gradient_sample(const struct vec_gradient *gradient, const uint32_t *ramp, int width, float x, float y);

#endif
