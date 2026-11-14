/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Rounded rectangles, their border rings and their blurred shadows drawn by closed formulas.
 */

#include "vector/vec-rrect.h"

#include <math.h>

/*
 * The coefficients of the rational approximation of the error function
 * of vec_erf: erf(x) = 1 - (a1 t + a2 t^2 + a3 t^3 + a4 t^4 + a5 t^5)
 * e^(-x^2) with t = 1 / (1 + p x), for x >= 0.
 *
 * Values referenced: Abramowitz and Stegun, Handbook of Mathematical
 * Functions, formula 7.1.26 (p, a1 to a5; error under 1.5e-7).
 */
#define RRECT_ERF_P		0.3275911f
static const float rrect_erf_coefficients[5] = {
	0.254829592f,
	-0.284496736f,
	1.421413741f,
	-1.453152027f,
	1.061405429f
};

/* The reciprocal of the square root of pi, the factor of the normal density written with k. */
#define RRECT_INV_SQRT_PI	0.56418958354f

/* The square root of 2, which turns a standard deviation sigma into k = 1 / (sigma sqrt 2). */
#define RRECT_SQRT_2		1.41421356237f

/*
 * How many standard deviations around a pixel's centre the shadow's
 * integrals are taken over: the normal density beyond four deviations
 * holds 6.4e-5 of its mass.
 */
#define RRECT_SHADOW_REACH	4.0f

/*
 * The largest border width or spread taken as given.  A coordinate is at
 * most VEC_COORD_MAX in magnitude, so a width beyond it empties every
 * box whatever it is; holding it keeps the edges finite.
 */
#define RRECT_WIDTH_MAX		VEC_COORD_MAX

/*
 * The state of one shadow evaluation: the pixel's centre and the scale
 * of the normal density, shared by the pieces of the integral.
 */
struct rrect_shadow_context {
	/* 1 / (sigma sqrt 2). */
	float k;
	/* The pixel's centre. */
	float cx;
	float cy;
	/* RRECT_SHADOW_REACH standard deviations, the half width of the window of the integrals. */
	float reach;
};

/* The forward declarations of the static functions. */
static float rrect_clamp_unit(float value);
static float rrect_clean_radius(float radius);
static float rrect_clean_width(float width);
static void rrect_zero_radii(struct vec_rrect *rrect);
static void rrect_order_edges(float *low, float *high);
static float rrect_overlap(float low, float high, float position);
static float rrect_corner_coverage(const struct vec_rrect *rrect, int corner, float cx, float cy);
static float rrect_shadow_density(const struct rrect_shadow_context *context, float offset);
static float rrect_shadow_psi(const struct rrect_shadow_context *context, float y);
static float rrect_shadow_edge_value(const struct rrect_shadow_context *context, float x);
static float rrect_shadow_steep(const struct rrect_shadow_context *context, float ex, float ey, float rx, float ry, float dir_x, float dir_y, float theta);
static float rrect_shadow_flat(const struct rrect_shadow_context *context, float ex, float ey, float rx, float ry, float dir_x, float dir_y, float theta);
static float rrect_shadow_corner(const struct rrect_shadow_context *context, float ex, float ey, float rx, float ry, float dir_x, float dir_y);
static float rrect_shadow_edge(const struct rrect_shadow_context *context, const struct vec_rrect *rrect, int right_edge);

/*
 * Makes the radii of a rounded rectangle usable.
 *
 * The radii are cleaned one by one, a corner with a zero radius is made
 * square, and the radii are scaled down together when two along a side
 * would overlap.
 */
void
vec_rrect_normalize(
	struct vec_rrect *rrect)
{
	int corner;
	int axis;
	int finite;
	double width;
	double height;
	double sum;
	double ratio;
	double factor;
	double scaled;

	/* Squares every corner of a rectangle with an edge that is not a number or an infinity. */
	finite = vec_finite(rrect->rect.left);
	if (finite)
		finite = vec_finite(rrect->rect.top);
	if (finite)
		finite = vec_finite(rrect->rect.right);
	if (finite)
		finite = vec_finite(rrect->rect.bottom);
	if (!finite) {
		rrect_zero_radii(rrect);
		return;
	}

	/* Squares every corner of an empty or inverted rectangle, which has no room for a curve. */
	if (!(rrect->rect.right > rrect->rect.left) || !(rrect->rect.bottom > rrect->rect.top)) {
		rrect_zero_radii(rrect);
		return;
	}

	/* Cleans each radius: one that is not a number, is negative or is tiny becomes 0. */
	for (corner = 0; corner < 4; corner++) {
		for (axis = 0; axis < 2; axis++)
			rrect->radii[corner][axis] = rrect_clean_radius(rrect->radii[corner][axis]);
	}

	/* Squares a corner that has one radius 0, as CSS Backgrounds 3 section 4.1 asks. */
	for (corner = 0; corner < 4; corner++) {
		if (rrect->radii[corner][0] <= 0.0f || rrect->radii[corner][1] <= 0.0f) {
			rrect->radii[corner][0] = 0.0f;
			rrect->radii[corner][1] = 0.0f;
		}
	}

	/*
	 * Finds the factor f of section 4.5: the least ratio of a side's
	 * length to the sum of the two radii along it, over the sides whose
	 * sum is positive (a sum is made in double, which cannot overflow).
	 */
	width = (double)rrect->rect.right - (double)rrect->rect.left;
	height = (double)rrect->rect.bottom - (double)rrect->rect.top;
	factor = 1.0;

	/* The top side runs between the top left and the top right corners, horizontally. */
	sum = (double)rrect->radii[VEC_CORNER_TOP_LEFT][0] + (double)rrect->radii[VEC_CORNER_TOP_RIGHT][0];
	if (sum > 0.0) {
		ratio = width / sum;
		if (ratio < factor)
			factor = ratio;
	}

	/* The right side runs between the top right and the bottom right corners, vertically. */
	sum = (double)rrect->radii[VEC_CORNER_TOP_RIGHT][1] + (double)rrect->radii[VEC_CORNER_BOTTOM_RIGHT][1];
	if (sum > 0.0) {
		ratio = height / sum;
		if (ratio < factor)
			factor = ratio;
	}

	/* The bottom side runs between the bottom right and the bottom left corners, horizontally. */
	sum = (double)rrect->radii[VEC_CORNER_BOTTOM_RIGHT][0] + (double)rrect->radii[VEC_CORNER_BOTTOM_LEFT][0];
	if (sum > 0.0) {
		ratio = width / sum;
		if (ratio < factor)
			factor = ratio;
	}

	/* The left side runs between the bottom left and the top left corners, vertically. */
	sum = (double)rrect->radii[VEC_CORNER_BOTTOM_LEFT][1] + (double)rrect->radii[VEC_CORNER_TOP_LEFT][1];
	if (sum > 0.0) {
		ratio = height / sum;
		if (ratio < factor)
			factor = ratio;
	}

	/* Scales every radius by f when the radii along a side would overlap. */
	if (factor < 1.0) {
		for (corner = 0; corner < 4; corner++) {
			for (axis = 0; axis < 2; axis++) {
				scaled = (double)rrect->radii[corner][axis] * factor;
				rrect->radii[corner][axis] = (float)scaled;
			}
		}
	}

	/* Squares a corner that the scaling left with a radius under the smallest kept. */
	for (corner = 0; corner < 4; corner++) {
		if (rrect->radii[corner][0] < VEC_RADIUS_MIN || rrect->radii[corner][1] < VEC_RADIUS_MIN) {
			rrect->radii[corner][0] = 0.0f;
			rrect->radii[corner][1] = 0.0f;
		}
	}
}

/*
 * Makes the padding edge from the border edge and the border widths.
 *
 * The rectangle moves in by the widths (an overlap of two edges collapses
 * to their midpoint), and each radius shrinks by the width of the side its
 * axis runs across.
 */
void
vec_rrect_inset(
	const struct vec_rrect *outer,
	const float widths[4],
	struct vec_rrect *inner)
{
	struct vec_rrect result;
	float width[4];
	int side;
	int corner;
	int axis;
	int horizontal;
	int vertical;
	float radius;

	/* Takes the widths, a width that is not usable being 0. */
	for (side = 0; side < 4; side++)
		width[side] = rrect_clean_width(widths[side]);

	/* Moves each edge of the rectangle inwards by the width of its side. */
	result.rect.left = outer->rect.left + width[VEC_SIDE_LEFT];
	result.rect.top = outer->rect.top + width[VEC_SIDE_TOP];
	result.rect.right = outer->rect.right - width[VEC_SIDE_RIGHT];
	result.rect.bottom = outer->rect.bottom - width[VEC_SIDE_BOTTOM];

	/* Makes a zero size rectangle of widths that cross each other. */
	rrect_order_edges(&result.rect.left, &result.rect.right);
	rrect_order_edges(&result.rect.top, &result.rect.bottom);

	/*
	 * Shrinks each radius by the border it runs across: the horizontal
	 * radius of a corner by the width of the left or right side that
	 * corner is on, the vertical one by the top or bottom width.
	 */
	for (corner = 0; corner < 4; corner++) {
		horizontal = VEC_SIDE_LEFT;
		if (corner == VEC_CORNER_TOP_RIGHT || corner == VEC_CORNER_BOTTOM_RIGHT)
			horizontal = VEC_SIDE_RIGHT;
		vertical = VEC_SIDE_TOP;
		if (corner == VEC_CORNER_BOTTOM_RIGHT || corner == VEC_CORNER_BOTTOM_LEFT)
			vertical = VEC_SIDE_BOTTOM;

		for (axis = 0; axis < 2; axis++) {
			radius = outer->radii[corner][axis];
			if (axis == 0)
				radius = radius - width[horizontal];
			else
				radius = radius - width[vertical];
			if (!(radius > 0.0f))
				radius = 0.0f;
			result.radii[corner][axis] = radius;
		}
	}

	/* Makes the result usable and gives it to the caller. */
	vec_rrect_normalize(&result);
	*inner = result;
}

/*
 * Makes the edge of a shadow or an outline from a border edge and a spread.
 *
 * The rectangle grows (or shrinks) by the spread; a radius grows by the
 * proportional rule of section 6.1.1 and shrinks by the plain floor.
 */
void
vec_rrect_spread(
	const struct vec_rrect *border,
	float amount,
	struct vec_rrect *grown)
{
	struct vec_rrect result;
	int finite;
	int corner;
	int axis;
	float radius;
	float ratio;
	float cube;

	/* Takes an amount that is not a number as no spread, and holds a huge one. */
	finite = vec_finite(amount);
	if (!finite)
		amount = 0.0f;
	if (amount > RRECT_WIDTH_MAX)
		amount = RRECT_WIDTH_MAX;
	if (amount < -RRECT_WIDTH_MAX)
		amount = -RRECT_WIDTH_MAX;

	/* Moves each edge of the rectangle outwards by the amount. */
	result.rect.left = border->rect.left - amount;
	result.rect.top = border->rect.top - amount;
	result.rect.right = border->rect.right + amount;
	result.rect.bottom = border->rect.bottom + amount;

	/* Makes a zero size rectangle of edges that cross each other, as a shrink may. */
	rrect_order_edges(&result.rect.left, &result.rect.right);
	rrect_order_edges(&result.rect.top, &result.rect.bottom);

	/* Changes each radius, a square axis staying square. */
	for (corner = 0; corner < 4; corner++) {
		for (axis = 0; axis < 2; axis++) {
			radius = border->radii[corner][axis];
			if (radius > 0.0f) {
				if (amount < 0.0f) {
					/* A shrink takes the amount off, to 0 at the least. */
					radius = radius + amount;
					if (!(radius > 0.0f))
						radius = 0.0f;
				} else if (radius < amount) {
					/* A radius under the spread grows by the cubic of section 6.1.1, so that the corner stays a curve. */
					ratio = radius / amount - 1.0f;
					cube = ratio * ratio * ratio;
					radius = radius + amount * (1.0f + cube);
				} else {
					/* A radius at least the spread grows by it. */
					radius = radius + amount;
				}
			}
			result.radii[corner][axis] = radius;
		}
	}

	/* Makes the result usable and gives it to the caller. */
	vec_rrect_normalize(&result);
	*grown = result;
}

/*
 * Computes how much of a pixel a rounded rectangle covers.
 *
 * The rectangle's overlap with the pixel is cut by the corner's
 * anti-aliased ellipse when the pixel's centre is in a corner's box.
 */
float
vec_rrect_coverage(
	const struct vec_rrect *rrect,
	float px,
	float py)
{
	float overlap_x;
	float overlap_y;
	float coverage;
	float corner_coverage;
	float cx;
	float cy;
	int finite;
	int corner;

	/* Reports 0 for a pixel that has no position. */
	finite = vec_finite(px);
	if (!finite)
		return 0.0f;
	finite = vec_finite(py);
	if (!finite)
		return 0.0f;

	/* Takes the overlap of the pixel with the rectangle, as a plain rectangle covers a pixel. */
	overlap_x = rrect_overlap(rrect->rect.left, rrect->rect.right, px);
	overlap_y = rrect_overlap(rrect->rect.top, rrect->rect.bottom, py);
	coverage = overlap_x * overlap_y;

	/* Reports 0 for a pixel the rectangle does not touch. */
	if (!(coverage > 0.0f))
		return 0.0f;

	/* Cuts the coverage by each corner whose box holds the pixel's centre. */
	cx = px + 0.5f;
	cy = py + 0.5f;
	for (corner = 0; corner < 4; corner++) {
		corner_coverage = rrect_corner_coverage(rrect, corner, cx, cy);
		if (corner_coverage < coverage)
			coverage = corner_coverage;
	}

	/* Reports the coverage, kept in range. */
	return rrect_clamp_unit(coverage);
}

/*
 * Computes how much of a pixel a border ring covers.
 *
 * The inner rectangle's coverage is taken off the outer one's.
 */
float
vec_rrect_ring_coverage(
	const struct vec_rrect *outer,
	const struct vec_rrect *inner,
	float px,
	float py)
{
	float outer_coverage;
	float inner_coverage;

	/* Takes the coverage of the border edge, the outside of the ring. */
	outer_coverage = vec_rrect_coverage(outer, px, py);

	/* Takes the coverage of the padding edge, the hole of the ring. */
	inner_coverage = vec_rrect_coverage(inner, px, py);

	/* Reports what the border edge covers beyond the padding edge. */
	return rrect_clamp_unit(outer_coverage - inner_coverage);
}

/*
 * Computes the share of a pixel of a border that belongs to one side.
 *
 * The pixel's quadrant of the outer rectangle names its two sides, and the
 * distance from the diagonal of the two borders there divides it.
 */
float
vec_rrect_side_share(
	const struct vec_rrect *outer,
	const float widths[4],
	int side,
	float px,
	float py)
{
	float width[4];
	float middle_x;
	float middle_y;
	float cx;
	float cy;
	float inward_x;
	float inward_y;
	float horizontal_width;
	float vertical_width;
	float length;
	float distance;
	float horizontal_share;
	int finite;
	int index;
	int lower;
	int right;
	int horizontal_side;
	int vertical_side;

	/* Reports 0 for a side that does not exist. */
	if (side < 0 || side > 3)
		return 0.0f;

	/* Reports 0 for a pixel that has no position. */
	finite = vec_finite(px);
	if (!finite)
		return 0.0f;
	finite = vec_finite(py);
	if (!finite)
		return 0.0f;

	/* Takes the widths, a width that is not usable being 0. */
	for (index = 0; index < 4; index++)
		width[index] = rrect_clean_width(widths[index]);

	/* Picks the quadrant of the pixel's centre, cut by the middle lines of the rectangle. */
	middle_x = outer->rect.left * 0.5f + outer->rect.right * 0.5f;
	middle_y = outer->rect.top * 0.5f + outer->rect.bottom * 0.5f;
	cx = px + 0.5f;
	cy = py + 0.5f;
	right = 0;
	if (cx >= middle_x)
		right = 1;
	lower = 0;
	if (cy >= middle_y)
		lower = 1;

	/* Names the two sides of the quadrant. */
	horizontal_side = VEC_SIDE_TOP;
	if (lower)
		horizontal_side = VEC_SIDE_BOTTOM;
	vertical_side = VEC_SIDE_LEFT;
	if (right)
		vertical_side = VEC_SIDE_RIGHT;

	/* Gives 0 to a side that is not one of the quadrant's. */
	if (side != horizontal_side && side != vertical_side)
		return 0.0f;

	/* Measures how far the centre is from the quadrant's corner of the outer rectangle, inwards. */
	inward_x = cx - outer->rect.left;
	if (right)
		inward_x = outer->rect.right - cx;
	inward_y = cy - outer->rect.top;
	if (lower)
		inward_y = outer->rect.bottom - cy;
	horizontal_width = width[horizontal_side];
	vertical_width = width[vertical_side];

	/*
	 * Divides the pixel between the two sides: the transition runs along
	 * the line from the outer corner to the padding corner, (vertical
	 * width, horizontal width) inwards, and the share follows the
	 * distance from that line.  A side without width gives all to the
	 * other, and two such sides give all to the horizontal one.
	 */
	if (horizontal_width <= 0.0f && vertical_width <= 0.0f) {
		horizontal_share = 1.0f;
	} else if (horizontal_width <= 0.0f) {
		horizontal_share = 0.0f;
	} else if (vertical_width <= 0.0f) {
		horizontal_share = 1.0f;
	} else {
		length = sqrtf(horizontal_width * horizontal_width + vertical_width * vertical_width);
		distance = (horizontal_width * inward_x - vertical_width * inward_y) / length;
		horizontal_share = rrect_clamp_unit(0.5f + distance);
	}

	/* Reports the horizontal side's share to it, the rest to the vertical side. */
	if (side == horizontal_side)
		return horizontal_share;
	return 1.0f - horizontal_share;
}

/*
 * Computes the error function by the rational approximation of
 * Abramowitz and Stegun 7.1.26.
 *
 * The magnitude is evaluated for x >= 0 and the sign restored, as erf is
 * odd.
 */
float
vec_erf(
	float x)
{
	float magnitude;
	float t;
	float polynomial;
	float value;
	int index;

	/* Reports 0 for a NaN. */
	if (x != x)
		return 0.0f;

	/* Works on the magnitude, the function being odd. */
	magnitude = x;
	if (magnitude < 0.0f)
		magnitude = -magnitude;

	/* Evaluates the polynomial in t by Horner's rule, from the highest power down. */
	t = 1.0f / (1.0f + RRECT_ERF_P * magnitude);
	polynomial = 0.0f;
	for (index = 4; index >= 0; index--)
		polynomial = polynomial * t + rrect_erf_coefficients[index];
	polynomial = polynomial * t;

	/* Takes erf of the magnitude, the Gaussian factor through vec_exp as the shader does. */
	value = 1.0f - polynomial * vec_exp(-(magnitude * magnitude));
	if (value > 1.0f)
		value = 1.0f;
	if (value < 0.0f)
		value = 0.0f;

	/* Restores the sign. */
	if (x < 0.0f)
		value = -value;

	/* Reports erf(x). */
	return value;
}

/*
 * Computes the coverage of a rounded rectangle blurred by a Gaussian.
 *
 * The blurred coverage is the integral over the rows of the density along
 * y times the density's integral along x between the left and right edges
 * of the row; each edge's integral is exact on its straight run and by the
 * midpoint rule on the curves of its corners.
 */
float
vec_rrect_shadow(
	const struct vec_rrect *rrect,
	float sigma,
	float px,
	float py)
{
	struct rrect_shadow_context context;
	float right_term;
	float left_term;
	float coverage;
	int finite;

	/* Takes a sigma that is not a number as no blur. */
	finite = vec_finite(sigma);
	if (!finite)
		sigma = 0.0f;

	/* Reports a sharp coverage for a blur too small to show. */
	if (sigma < VEC_SIGMA_MIN)
		return vec_rrect_coverage(rrect, px, py);

	/* Reports 0 for a pixel that has no position. */
	finite = vec_finite(px);
	if (!finite)
		return 0.0f;
	finite = vec_finite(py);
	if (!finite)
		return 0.0f;

	/* Reports 0 for a rectangle without area. */
	if (!(rrect->rect.right > rrect->rect.left) || !(rrect->rect.bottom > rrect->rect.top))
		return 0.0f;

	/* Sets up the density for this pixel's centre. */
	context.k = 1.0f / (sigma * RRECT_SQRT_2);
	context.cx = px + 0.5f;
	context.cy = py + 0.5f;
	context.reach = RRECT_SHADOW_REACH * sigma;

	/* Integrates along the right edge, then the left edge; the blur is the difference. */
	right_term = rrect_shadow_edge(&context, rrect, 1);
	left_term = rrect_shadow_edge(&context, rrect, 0);
	coverage = right_term - left_term;

	/* Reports the coverage, kept in range. */
	return rrect_clamp_unit(coverage);
}

/*
 * Keeps a value in [0, 1], a NaN becoming 0.
 */
static float
rrect_clamp_unit(
	float value)
{
	/* Takes a NaN and anything under 0 as 0. */
	if (!(value > 0.0f))
		return 0.0f;

	/* Holds anything above 1. */
	if (value > 1.0f)
		return 1.0f;

	/* Succeeded: the value was in range. */
	return value;
}

/*
 * Cleans a radius: one that is not finite, is negative or is under the smallest kept becomes 0.
 */
static float
rrect_clean_radius(
	float radius)
{
	int finite;

	/* Refuses a NaN or an infinity. */
	finite = vec_finite(radius);
	if (!finite)
		return 0.0f;

	/* Refuses a negative radius and one too small to draw. */
	if (radius < VEC_RADIUS_MIN)
		return 0.0f;

	/* Succeeded: the radius is usable. */
	return radius;
}

/*
 * Cleans a border width or spread: one that is not finite or is negative is 0, a huge one is held.
 */
static float
rrect_clean_width(
	float width)
{
	int finite;

	/* Refuses a NaN or an infinity. */
	finite = vec_finite(width);
	if (!finite)
		return 0.0f;

	/* Refuses a negative width. */
	if (!(width > 0.0f))
		return 0.0f;

	/* Holds a width beyond any coordinate. */
	if (width > RRECT_WIDTH_MAX)
		return RRECT_WIDTH_MAX;

	/* Succeeded: the width is usable. */
	return width;
}

/*
 * Squares all four corners.
 */
static void
rrect_zero_radii(
	struct vec_rrect *rrect)
{
	int corner;

	/* Clears both radii of every corner. */
	for (corner = 0; corner < 4; corner++) {
		rrect->radii[corner][0] = 0.0f;
		rrect->radii[corner][1] = 0.0f;
	}
}

/*
 * Makes two crossed edges (high below low) one edge at their midpoint.
 */
static void
rrect_order_edges(
	float *low,
	float *high)
{
	float middle;

	/* Leaves edges that are in order. */
	if (!(*high < *low))
		return;

	/* Puts both at the middle of the two, so that the size is 0. */
	middle = *low * 0.5f + *high * 0.5f;
	*low = middle;
	*high = middle;
}

/*
 * Computes the length of [position, position + 1) inside [low, high), in [0, 1].
 */
static float
rrect_overlap(
	float low,
	float high,
	float position)
{
	float start;
	float end;

	/* Takes the later start and the earlier end of the two intervals. */
	start = position;
	if (low > start)
		start = low;
	end = position + 1.0f;
	if (high < end)
		end = high;

	/* Reports their common length, 0 when they do not meet. */
	return rrect_clamp_unit(end - start);
}

/*
 * Computes the coverage factor of one corner at a pixel centre: 1 outside its box, the anti-aliased ellipse inside.
 */
static float
rrect_corner_coverage(
	const struct vec_rrect *rrect,
	int corner,
	float cx,
	float cy)
{
	float rx;
	float ry;
	float ex;
	float ey;
	float u;
	float v;
	float g;
	float q;
	float d;
	float smaller;
	int far_x;
	int far_y;
	int inside;

	/* Gives 1 for a square corner. */
	rx = rrect->radii[corner][0];
	ry = rrect->radii[corner][1];
	if (!(rx > 0.0f) || !(ry > 0.0f))
		return 1.0f;

	/* Places the ellipse's centre by which of the sides the corner is on. */
	far_x = 0;
	if (corner == VEC_CORNER_TOP_RIGHT || corner == VEC_CORNER_BOTTOM_RIGHT)
		far_x = 1;
	far_y = 0;
	if (corner == VEC_CORNER_BOTTOM_RIGHT || corner == VEC_CORNER_BOTTOM_LEFT)
		far_y = 1;
	ex = rrect->rect.left + rx;
	if (far_x)
		ex = rrect->rect.right - rx;
	ey = rrect->rect.top + ry;
	if (far_y)
		ey = rrect->rect.bottom - ry;

	/* Tells whether the pixel's centre lies in the corner's box, the rx by ry rectangle at the corner. */
	inside = 1;
	if (far_x) {
		if (cx < ex || cx >= rrect->rect.right)
			inside = 0;
	} else {
		if (cx < rrect->rect.left || cx >= ex)
			inside = 0;
	}
	if (far_y) {
		if (cy < ey || cy >= rrect->rect.bottom)
			inside = 0;
	} else {
		if (cy < rrect->rect.top || cy >= ey)
			inside = 0;
	}

	/* Gives 1 for a pixel outside the corner's box. */
	if (!inside)
		return 1.0f;

	/* Takes the position in the unit circle the ellipse maps to and the implicit function g. */
	u = (cx - ex) / rx;
	v = (cy - ey) / ry;
	g = u * u + v * v - 1.0f;

	/*
	 * Divides g by the length of its gradient to get the distance to the
	 * ellipse in pixels; at the ellipse's centre, where the gradient is
	 * 0, the distance is minus half the smaller radius.
	 */
	q = u * u / (rx * rx) + v * v / (ry * ry);
	if (q > 0.0f) {
		d = g / (2.0f * sqrtf(q));
	} else {
		smaller = rx;
		if (ry < smaller)
			smaller = ry;
		d = -0.5f * smaller;
	}

	/* Reports the half pixel ramp across the curve, 1 inside it and 0 outside. */
	return rrect_clamp_unit(0.5f - d);
}

/*
 * Computes the normal density of the blur at an offset from the pixel's centre, written with k.
 */
static float
rrect_shadow_density(
	const struct rrect_shadow_context *context,
	float offset)
{
	float scaled;

	/* Evaluates k / sqrt(pi) * e^(-(offset k)^2). */
	scaled = offset * context->k;
	return context->k * RRECT_INV_SQRT_PI * vec_exp(-(scaled * scaled));
}

/*
 * Computes the integral of the density from minus infinity to y, (1 + erf((y - cy) k)) / 2.
 */
static float
rrect_shadow_psi(
	const struct rrect_shadow_context *context,
	float y)
{
	float erf_value;

	/* Takes the error function at the scaled offset. */
	erf_value = vec_erf((y - context->cy) * context->k);

	/* Reports the cumulative distribution. */
	return (1.0f + erf_value) * 0.5f;
}

/*
 * Computes E(x) = erf((x - cx) k) / 2, the density's integral along x up to a vertical edge at x, less a half.
 */
static float
rrect_shadow_edge_value(
	const struct rrect_shadow_context *context,
	float x)
{
	float erf_value;

	/* Takes the error function at the scaled offset. */
	erf_value = vec_erf((x - context->cx) * context->k);

	/* Reports half of it. */
	return erf_value * 0.5f;
}

/*
 * Integrates the steep half of a corner's curve along y.
 *
 * The half runs from the end of the straight piece (theta 0) to theta;
 * its rows are met with the window around the pixel and the product of the
 * density and E(x(y)) is summed by the midpoint rule.
 */
static float
rrect_shadow_steep(
	const struct rrect_shadow_context *context,
	float ex,
	float ey,
	float rx,
	float ry,
	float dir_x,
	float dir_y,
	float theta)
{
	float y_end;
	float low;
	float high;
	float step;
	float y;
	float v;
	float root;
	float x;
	float sum;
	int node;

	/* Finds the rows of the half, from the ellipse's centre line to the cut. */
	y_end = ey + dir_y * ry * sinf(theta);
	low = ey;
	high = y_end;
	if (y_end < ey) {
		low = y_end;
		high = ey;
	}

	/* Meets the rows with the window of four deviations around the pixel. */
	if (low < context->cy - context->reach)
		low = context->cy - context->reach;
	if (high > context->cy + context->reach)
		high = context->cy + context->reach;

	/* Skips a half with no rows in the window. */
	if (!(high > low))
		return 0.0f;

	/* Sums the product at the middle of each equal step. */
	step = (high - low) / (float)VEC_SHADOW_NODES;
	sum = 0.0f;
	for (node = 0; node < VEC_SHADOW_NODES; node++) {
		y = low + ((float)node + 0.5f) * step;
		v = (y - ey) / ry;
		root = 1.0f - v * v;
		if (root < 0.0f)
			root = 0.0f;
		x = ex + dir_x * rx * sqrtf(root);
		sum = sum + rrect_shadow_density(context, y - context->cy) * rrect_shadow_edge_value(context, x);
	}

	/* Reports the sum times the step. */
	return sum * step;
}

/*
 * Integrates the flat half of a corner's curve by parts, along x.
 *
 * The half runs from theta to the end on the top or bottom side
 * (theta pi / 2).  The integral of density times E over its rows equals
 * the boundary term Psi E at the two ends less the integral of Psi times
 * E' along x, which the midpoint rule sums over the columns in the window.
 */
static float
rrect_shadow_flat(
	const struct rrect_shadow_context *context,
	float ex,
	float ey,
	float rx,
	float ry,
	float dir_x,
	float dir_y,
	float theta)
{
	float cut_x;
	float cut_y;
	float end_y;
	float xa;
	float ya;
	float xb;
	float yb;
	float low;
	float high;
	float step;
	float x;
	float u;
	float root;
	float y;
	float sum;
	float direction;
	float boundary;
	int node;

	/* Finds the two ends: the cut and the point on the top or bottom side. */
	cut_x = ex + dir_x * rx * cosf(theta);
	cut_y = ey + dir_y * ry * sinf(theta);
	end_y = ey + dir_y * ry;

	/* Skips a half whose rows are wholly outside the window, where its term is 0. */
	low = cut_y;
	high = end_y;
	if (end_y < cut_y) {
		low = end_y;
		high = cut_y;
	}
	if (high < context->cy - context->reach || low > context->cy + context->reach)
		return 0.0f;

	/* Orders the ends by y, the upper first. */
	if (dir_y < 0.0f) {
		xa = ex;
		ya = end_y;
		xb = cut_x;
		yb = cut_y;
	} else {
		xa = cut_x;
		ya = cut_y;
		xb = ex;
		yb = end_y;
	}

	/* Takes the boundary term Psi(yb) E(xb) - Psi(ya) E(xa). */
	boundary = rrect_shadow_psi(context, yb) * rrect_shadow_edge_value(context, xb) -
	    rrect_shadow_psi(context, ya) * rrect_shadow_edge_value(context, xa);

	/* Takes the columns of the half, met with the window around the pixel. */
	direction = 1.0f;
	low = xa;
	high = xb;
	if (xb < xa) {
		direction = -1.0f;
		low = xb;
		high = xa;
	}
	if (low < context->cx - context->reach)
		low = context->cx - context->reach;
	if (high > context->cx + context->reach)
		high = context->cx + context->reach;

	/* Reports the boundary term alone when no column is in the window. */
	if (!(high > low))
		return boundary;

	/* Sums Psi(y(x)) E'(x) at the middle of each equal step in x. */
	step = (high - low) / (float)VEC_SHADOW_NODES;
	sum = 0.0f;
	for (node = 0; node < VEC_SHADOW_NODES; node++) {
		x = low + ((float)node + 0.5f) * step;
		u = (x - ex) / rx;
		root = 1.0f - u * u;
		if (root < 0.0f)
			root = 0.0f;
		y = ey + dir_y * ry * sqrtf(root);
		sum = sum + rrect_shadow_psi(context, y) * rrect_shadow_density(context, x - context->cx);
	}

	/* Reports the boundary term less the signed integral. */
	return boundary - direction * sum * step;
}

/*
 * Integrates one corner's quarter ellipse, in two halves cut where the curve's slope is one.
 */
static float
rrect_shadow_corner(
	const struct rrect_shadow_context *context,
	float ex,
	float ey,
	float rx,
	float ry,
	float dir_x,
	float dir_y)
{
	float theta;
	float steep;
	float flat;

	/* Finds the angle where the slope of the curve is one for these radii. */
	theta = vec_atan2(ry, rx);

	/* Integrates the steep half, from the straight piece to the cut. */
	steep = rrect_shadow_steep(context, ex, ey, rx, ry, dir_x, dir_y, theta);

	/* Integrates the flat half, from the cut to the top or bottom side. */
	flat = rrect_shadow_flat(context, ex, ey, rx, ry, dir_x, dir_y, theta);

	/* Reports the corner's integral. */
	return steep + flat;
}

/*
 * Integrates the density of the rows times E of one edge: its straight run and both corners.
 */
static float
rrect_shadow_edge(
	const struct rrect_shadow_context *context,
	const struct vec_rrect *rrect,
	int right_edge)
{
	float x_side;
	float dir_x;
	float rx_top;
	float ry_top;
	float rx_bottom;
	float ry_bottom;
	float y_start;
	float y_end;
	float total;
	float ex;
	float ey;

	/* Takes the edge's side and the radii of the two corners it runs between. */
	x_side = rrect->rect.left;
	dir_x = -1.0f;
	rx_top = rrect->radii[VEC_CORNER_TOP_LEFT][0];
	ry_top = rrect->radii[VEC_CORNER_TOP_LEFT][1];
	rx_bottom = rrect->radii[VEC_CORNER_BOTTOM_LEFT][0];
	ry_bottom = rrect->radii[VEC_CORNER_BOTTOM_LEFT][1];
	if (right_edge) {
		x_side = rrect->rect.right;
		dir_x = 1.0f;
		rx_top = rrect->radii[VEC_CORNER_TOP_RIGHT][0];
		ry_top = rrect->radii[VEC_CORNER_TOP_RIGHT][1];
		rx_bottom = rrect->radii[VEC_CORNER_BOTTOM_RIGHT][0];
		ry_bottom = rrect->radii[VEC_CORNER_BOTTOM_RIGHT][1];
	}

	/* Integrates the straight piece between the corners exactly. */
	y_start = rrect->rect.top + ry_top;
	y_end = rrect->rect.bottom - ry_bottom;
	total = 0.0f;
	if (y_end > y_start)
		total = (rrect_shadow_psi(context, y_end) - rrect_shadow_psi(context, y_start)) *
		    rrect_shadow_edge_value(context, x_side);

	/* Adds the top corner's curve when it has one. */
	if (rx_top > 0.0f && ry_top > 0.0f) {
		ex = x_side - dir_x * rx_top;
		ey = rrect->rect.top + ry_top;
		total = total + rrect_shadow_corner(context, ex, ey, rx_top, ry_top, dir_x, -1.0f);
	}

	/* Adds the bottom corner's curve when it has one. */
	if (rx_bottom > 0.0f && ry_bottom > 0.0f) {
		ex = x_side - dir_x * rx_bottom;
		ey = rrect->rect.bottom - ry_bottom;
		total = total + rrect_shadow_corner(context, ex, ey, rx_bottom, ry_bottom, dir_x, 1.0f);
	}

	/* Reports the edge's integral. */
	return total;
}
