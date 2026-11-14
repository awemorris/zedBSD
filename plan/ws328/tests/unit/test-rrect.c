/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of vec-rrect: the normalization of radii (the scale
 * factor of CSS Backgrounds 3 section 4.5 by hand), the inner and spread
 * edges, the coverage of a rounded rectangle at points on and off its
 * curve, the border ring, the shares of the four sides, the error
 * function against its tabulated values, and the blurred shadow against a
 * dense numerical integration written here from the definition (the
 * rectangle's rows by the midpoint rule over 4000 steps, the error
 * function of the C library in double precision); a random run under the
 * sanitizers.  Every expected value is a hand calculation, a tabulated
 * value or that integration; no other implementation's output is used.
 */

#include "check.h"

#include <float.h>
#include <math.h>

#include "vector/vec-rrect.h"

/* The ratio of a circle's circumference to its diameter, in double precision. */
#define TEST_PI			3.14159265358979323846

/* The steps of the dense integration of the shadow. */
#define DENSE_STEPS		4000

/* The standard deviations around the pixel the dense integration covers (beyond it the density is under 1e-30). */
#define DENSE_REACH		12.0

/* The largest difference of the shadow from the dense integration that section 6.1.2's 5% allows, with margin. */
#define SHADOW_TOLERANCE	0.015

/* Builds a rounded rectangle whose four corners have the same radii. */
static struct vec_rrect
make_uniform(
	float left,
	float top,
	float right,
	float bottom,
	float rx,
	float ry)
{
	struct vec_rrect rrect;
	int corner;

	rrect.rect.left = left;
	rrect.rect.top = top;
	rrect.rect.right = right;
	rrect.rect.bottom = bottom;
	for (corner = 0; corner < 4; corner++) {
		rrect.radii[corner][0] = rx;
		rrect.radii[corner][1] = ry;
	}
	return rrect;
}

/* Sets the radii of one corner. */
static void
set_corner(
	struct vec_rrect *rrect,
	int corner,
	float rx,
	float ry)
{
	rrect->radii[corner][0] = rx;
	rrect->radii[corner][1] = ry;
}

/* Checks that every rule of a normalized rounded rectangle holds. */
static void
check_normalized(
	const struct vec_rrect *rrect)
{
	int corner;
	double width;
	double height;

	for (corner = 0; corner < 4; corner++) {
		CHECK(rrect->radii[corner][0] >= 0.0f);
		CHECK(rrect->radii[corner][1] >= 0.0f);
		CHECK((rrect->radii[corner][0] == 0.0f) == (rrect->radii[corner][1] == 0.0f));
		CHECK(rrect->radii[corner][0] == 0.0f || rrect->radii[corner][0] >= VEC_RADIUS_MIN);
		CHECK(rrect->radii[corner][1] == 0.0f || rrect->radii[corner][1] >= VEC_RADIUS_MIN);
	}
	width = (double)rrect->rect.right - (double)rrect->rect.left;
	height = (double)rrect->rect.bottom - (double)rrect->rect.top;
	if (!(width > 0.0) || !(height > 0.0)) {
		CHECK(rrect->radii[0][0] == 0.0f && rrect->radii[2][1] == 0.0f);
		return;
	}
	CHECK((double)rrect->radii[VEC_CORNER_TOP_LEFT][0] + rrect->radii[VEC_CORNER_TOP_RIGHT][0] <= width * (1.0 + 1e-6));
	CHECK((double)rrect->radii[VEC_CORNER_BOTTOM_LEFT][0] + rrect->radii[VEC_CORNER_BOTTOM_RIGHT][0] <= width * (1.0 + 1e-6));
	CHECK((double)rrect->radii[VEC_CORNER_TOP_LEFT][1] + rrect->radii[VEC_CORNER_BOTTOM_LEFT][1] <= height * (1.0 + 1e-6));
	CHECK((double)rrect->radii[VEC_CORNER_TOP_RIGHT][1] + rrect->radii[VEC_CORNER_BOTTOM_RIGHT][1] <= height * (1.0 + 1e-6));
}

/* Checks the normalization on values worked by hand. */
static void
test_normalize(void)
{
	struct vec_rrect rrect;
	struct vec_rrect again;
	float nan_value;
	int corner;

	nan_value = nanf("");

	/*
	 * Width 100, height 50, every radius 50: the top and bottom sides
	 * give 100 / 100 = 1, the left and right sides 50 / 100 = 0.5, so
	 * f = 0.5 and every radius becomes 25.
	 */
	rrect = make_uniform(0.0f, 0.0f, 100.0f, 50.0f, 50.0f, 50.0f);
	vec_rrect_normalize(&rrect);
	for (corner = 0; corner < 4; corner++) {
		CHECK_NEAR(rrect.radii[corner][0], 25.0, 1e-5);
		CHECK_NEAR(rrect.radii[corner][1], 25.0, 1e-5);
	}
	check_normalized(&rrect);

	/* Radii that already fit are left alone. */
	rrect = make_uniform(10.0f, 20.0f, 110.0f, 70.0f, 20.0f, 10.0f);
	vec_rrect_normalize(&rrect);
	for (corner = 0; corner < 4; corner++) {
		CHECK_NEAR(rrect.radii[corner][0], 20.0, 0.0);
		CHECK_NEAR(rrect.radii[corner][1], 10.0, 0.0);
	}

	/*
	 * Width 100: the top has 80 + 80 = 160 -> 100 / 160 = 0.625; the
	 * height 100 has left 30 + 30 = 60 -> 1.67; so f = 0.625 and the
	 * radii (80, 30) become (50, 18.75) at the top corners, while the
	 * bottom corners with (10, 10) become (6.25, 6.25).
	 */
	rrect = make_uniform(0.0f, 0.0f, 100.0f, 100.0f, 10.0f, 10.0f);
	set_corner(&rrect, VEC_CORNER_TOP_LEFT, 80.0f, 30.0f);
	set_corner(&rrect, VEC_CORNER_TOP_RIGHT, 80.0f, 30.0f);
	vec_rrect_normalize(&rrect);
	CHECK_NEAR(rrect.radii[VEC_CORNER_TOP_LEFT][0], 50.0, 1e-5);
	CHECK_NEAR(rrect.radii[VEC_CORNER_TOP_LEFT][1], 18.75, 1e-5);
	CHECK_NEAR(rrect.radii[VEC_CORNER_TOP_RIGHT][0], 50.0, 1e-5);
	CHECK_NEAR(rrect.radii[VEC_CORNER_BOTTOM_LEFT][0], 6.25, 1e-5);
	CHECK_NEAR(rrect.radii[VEC_CORNER_BOTTOM_RIGHT][1], 6.25, 1e-5);

	/* A sum exactly equal to the side does not scale. */
	rrect = make_uniform(0.0f, 0.0f, 100.0f, 100.0f, 50.0f, 50.0f);
	vec_rrect_normalize(&rrect);
	CHECK_NEAR(rrect.radii[0][0], 50.0, 0.0);

	/* A negative, a NaN and an infinite radius become 0, and the corner is made square. */
	rrect = make_uniform(0.0f, 0.0f, 100.0f, 100.0f, 10.0f, 10.0f);
	set_corner(&rrect, VEC_CORNER_TOP_LEFT, -5.0f, 10.0f);
	set_corner(&rrect, VEC_CORNER_TOP_RIGHT, nan_value, 10.0f);
	set_corner(&rrect, VEC_CORNER_BOTTOM_RIGHT, 10.0f, INFINITY);
	vec_rrect_normalize(&rrect);
	CHECK_NEAR(rrect.radii[VEC_CORNER_TOP_LEFT][0], 0.0, 0.0);
	CHECK_NEAR(rrect.radii[VEC_CORNER_TOP_LEFT][1], 0.0, 0.0);
	CHECK_NEAR(rrect.radii[VEC_CORNER_TOP_RIGHT][0], 0.0, 0.0);
	CHECK_NEAR(rrect.radii[VEC_CORNER_TOP_RIGHT][1], 0.0, 0.0);
	CHECK_NEAR(rrect.radii[VEC_CORNER_BOTTOM_RIGHT][0], 0.0, 0.0);
	CHECK_NEAR(rrect.radii[VEC_CORNER_BOTTOM_RIGHT][1], 0.0, 0.0);
	CHECK_NEAR(rrect.radii[VEC_CORNER_BOTTOM_LEFT][0], 10.0, 0.0);

	/* A radius with one axis 0 makes the corner square. */
	rrect = make_uniform(0.0f, 0.0f, 100.0f, 100.0f, 10.0f, 10.0f);
	set_corner(&rrect, VEC_CORNER_BOTTOM_LEFT, 0.0f, 12.0f);
	vec_rrect_normalize(&rrect);
	CHECK_NEAR(rrect.radii[VEC_CORNER_BOTTOM_LEFT][0], 0.0, 0.0);
	CHECK_NEAR(rrect.radii[VEC_CORNER_BOTTOM_LEFT][1], 0.0, 0.0);

	/* A radius under 1/256 is 0, one just at it is kept. */
	rrect = make_uniform(0.0f, 0.0f, 100.0f, 100.0f, 10.0f, 10.0f);
	set_corner(&rrect, VEC_CORNER_TOP_LEFT, 1.0f / 512.0f, 10.0f);
	set_corner(&rrect, VEC_CORNER_TOP_RIGHT, 1.0f / 256.0f, 1.0f / 256.0f);
	vec_rrect_normalize(&rrect);
	CHECK_NEAR(rrect.radii[VEC_CORNER_TOP_LEFT][0], 0.0, 0.0);
	CHECK_NEAR(rrect.radii[VEC_CORNER_TOP_LEFT][1], 0.0, 0.0);
	CHECK_NEAR(rrect.radii[VEC_CORNER_TOP_RIGHT][0], 1.0 / 256.0, 0.0);
	CHECK_NEAR(rrect.radii[VEC_CORNER_TOP_RIGHT][1], 1.0 / 256.0, 0.0);

	/* An empty or inverted rectangle gets all radii 0, so does one with an edge that is not a number. */
	rrect = make_uniform(5.0f, 5.0f, 5.0f, 10.0f, 2.0f, 2.0f);
	vec_rrect_normalize(&rrect);
	CHECK_NEAR(rrect.radii[0][0], 0.0, 0.0);
	rrect = make_uniform(5.0f, 5.0f, 1.0f, 10.0f, 2.0f, 2.0f);
	vec_rrect_normalize(&rrect);
	CHECK_NEAR(rrect.radii[2][1], 0.0, 0.0);
	rrect = make_uniform(5.0f, 5.0f, 10.0f, 5.0f, 2.0f, 2.0f);
	vec_rrect_normalize(&rrect);
	CHECK_NEAR(rrect.radii[1][0], 0.0, 0.0);
	rrect = make_uniform(nan_value, 5.0f, 10.0f, 15.0f, 2.0f, 2.0f);
	vec_rrect_normalize(&rrect);
	CHECK_NEAR(rrect.radii[3][1], 0.0, 0.0);

	/* Normalizing twice gives the same result. */
	rrect = make_uniform(0.0f, 0.0f, 100.0f, 50.0f, 50.0f, 50.0f);
	set_corner(&rrect, VEC_CORNER_TOP_RIGHT, 70.0f, 3.0f);
	vec_rrect_normalize(&rrect);
	again = rrect;
	vec_rrect_normalize(&again);
	for (corner = 0; corner < 4; corner++) {
		CHECK_NEAR(again.radii[corner][0], rrect.radii[corner][0], 1e-5);
		CHECK_NEAR(again.radii[corner][1], rrect.radii[corner][1], 1e-5);
	}
}

/* Checks the inner edge of a border and the spread edge on values worked by hand. */
static void
test_inset_spread(void)
{
	struct vec_rrect outer;
	struct vec_rrect inner;
	struct vec_rrect grown;
	float widths[4];
	float nan_value;

	nan_value = nanf("");

	/*
	 * A 100 by 60 box with radii 20 and borders top 5, right 6, bottom 7,
	 * left 8: the rectangle is (8, 5)-(94, 53); the top left radii are
	 * (20 - 8, 20 - 5) = (12, 15), the top right (20 - 6, 20 - 5), the
	 * bottom right (20 - 6, 20 - 7), the bottom left (20 - 8, 20 - 7).
	 */
	outer = make_uniform(0.0f, 0.0f, 100.0f, 60.0f, 20.0f, 20.0f);
	vec_rrect_normalize(&outer);
	widths[VEC_SIDE_TOP] = 5.0f;
	widths[VEC_SIDE_RIGHT] = 6.0f;
	widths[VEC_SIDE_BOTTOM] = 7.0f;
	widths[VEC_SIDE_LEFT] = 8.0f;
	vec_rrect_inset(&outer, widths, &inner);
	CHECK_NEAR(inner.rect.left, 8.0, 0.0);
	CHECK_NEAR(inner.rect.top, 5.0, 0.0);
	CHECK_NEAR(inner.rect.right, 94.0, 0.0);
	CHECK_NEAR(inner.rect.bottom, 53.0, 0.0);
	CHECK_NEAR(inner.radii[VEC_CORNER_TOP_LEFT][0], 12.0, 1e-5);
	CHECK_NEAR(inner.radii[VEC_CORNER_TOP_LEFT][1], 15.0, 1e-5);
	CHECK_NEAR(inner.radii[VEC_CORNER_TOP_RIGHT][0], 14.0, 1e-5);
	CHECK_NEAR(inner.radii[VEC_CORNER_TOP_RIGHT][1], 15.0, 1e-5);
	CHECK_NEAR(inner.radii[VEC_CORNER_BOTTOM_RIGHT][0], 14.0, 1e-5);
	CHECK_NEAR(inner.radii[VEC_CORNER_BOTTOM_RIGHT][1], 13.0, 1e-5);
	CHECK_NEAR(inner.radii[VEC_CORNER_BOTTOM_LEFT][0], 12.0, 1e-5);
	CHECK_NEAR(inner.radii[VEC_CORNER_BOTTOM_LEFT][1], 13.0, 1e-5);

	/* A border wider than the radius gives a square inner corner, even when only the one axis runs out. */
	widths[VEC_SIDE_TOP] = 25.0f;
	widths[VEC_SIDE_RIGHT] = 1.0f;
	widths[VEC_SIDE_BOTTOM] = 1.0f;
	widths[VEC_SIDE_LEFT] = 1.0f;
	vec_rrect_inset(&outer, widths, &inner);
	CHECK_NEAR(inner.radii[VEC_CORNER_TOP_LEFT][0], 0.0, 0.0);
	CHECK_NEAR(inner.radii[VEC_CORNER_TOP_LEFT][1], 0.0, 0.0);
	CHECK_NEAR(inner.radii[VEC_CORNER_TOP_RIGHT][0], 0.0, 0.0);
	CHECK_NEAR(inner.radii[VEC_CORNER_BOTTOM_LEFT][0], 19.0, 1e-5);
	CHECK_NEAR(inner.radii[VEC_CORNER_BOTTOM_LEFT][1], 19.0, 1e-4);

	/* Widths that are negative or not numbers are taken as 0. */
	widths[VEC_SIDE_TOP] = -3.0f;
	widths[VEC_SIDE_RIGHT] = nan_value;
	widths[VEC_SIDE_BOTTOM] = INFINITY;
	widths[VEC_SIDE_LEFT] = 0.0f;
	vec_rrect_inset(&outer, widths, &inner);
	CHECK_NEAR(inner.rect.left, 0.0, 0.0);
	CHECK_NEAR(inner.rect.top, 0.0, 0.0);
	CHECK_NEAR(inner.rect.right, 100.0, 0.0);
	CHECK_NEAR(inner.rect.bottom, 60.0, 0.0);
	CHECK_NEAR(inner.radii[VEC_CORNER_TOP_LEFT][0], 20.0, 0.0);

	/* Borders that meet leave a zero size rectangle, which has no radii. */
	outer = make_uniform(0.0f, 0.0f, 10.0f, 10.0f, 3.0f, 3.0f);
	widths[VEC_SIDE_TOP] = 8.0f;
	widths[VEC_SIDE_RIGHT] = 8.0f;
	widths[VEC_SIDE_BOTTOM] = 8.0f;
	widths[VEC_SIDE_LEFT] = 8.0f;
	vec_rrect_inset(&outer, widths, &inner);
	CHECK(inner.rect.right <= inner.rect.left);
	CHECK(inner.rect.bottom <= inner.rect.top);
	CHECK_NEAR(inner.rect.right - inner.rect.left, 0.0, 0.0);
	CHECK_NEAR(inner.radii[0][0], 0.0, 0.0);

	/* The output may be the input. */
	outer = make_uniform(0.0f, 0.0f, 100.0f, 60.0f, 20.0f, 20.0f);
	widths[VEC_SIDE_TOP] = 5.0f;
	widths[VEC_SIDE_RIGHT] = 5.0f;
	widths[VEC_SIDE_BOTTOM] = 5.0f;
	widths[VEC_SIDE_LEFT] = 5.0f;
	vec_rrect_inset(&outer, widths, &outer);
	CHECK_NEAR(outer.rect.left, 5.0, 0.0);
	CHECK_NEAR(outer.radii[2][0], 15.0, 1e-5);

	/*
	 * Section 6.1.1: radius 10 with spread 20: ratio 0.5, so the radius
	 * becomes 10 + 20 * (1 + (0.5 - 1)^3) = 10 + 20 * 0.875 = 27.5.
	 * Radius 5 with spread 10 is 5 + 10 * 0.875 = 13.75.
	 */
	outer = make_uniform(0.0f, 0.0f, 200.0f, 200.0f, 10.0f, 10.0f);
	set_corner(&outer, VEC_CORNER_TOP_RIGHT, 5.0f, 5.0f);
	vec_rrect_spread(&outer, 20.0f, &grown);
	CHECK_NEAR(grown.rect.left, -20.0, 0.0);
	CHECK_NEAR(grown.rect.top, -20.0, 0.0);
	CHECK_NEAR(grown.rect.right, 220.0, 0.0);
	CHECK_NEAR(grown.rect.bottom, 220.0, 0.0);
	CHECK_NEAR(grown.radii[VEC_CORNER_TOP_LEFT][0], 27.5, 1e-4);
	CHECK_NEAR(grown.radii[VEC_CORNER_TOP_LEFT][1], 27.5, 1e-4);
	CHECK_NEAR(grown.radii[VEC_CORNER_TOP_RIGHT][0], 5.0 + 20.0 * (1.0 + pow(0.25 - 1.0, 3.0)), 1e-4);

	/* A radius at least the spread just grows by it. */
	outer = make_uniform(0.0f, 0.0f, 200.0f, 200.0f, 30.0f, 40.0f);
	vec_rrect_spread(&outer, 20.0f, &grown);
	CHECK_NEAR(grown.radii[VEC_CORNER_TOP_LEFT][0], 50.0, 1e-5);
	CHECK_NEAR(grown.radii[VEC_CORNER_TOP_LEFT][1], 60.0, 1e-5);

	/* Spread 0 changes nothing. */
	vec_rrect_spread(&outer, 0.0f, &grown);
	CHECK_NEAR(grown.radii[VEC_CORNER_TOP_LEFT][0], 30.0, 0.0);
	CHECK_NEAR(grown.rect.right, 200.0, 0.0);

	/* A zero radius stays square under a positive spread. */
	outer = make_uniform(0.0f, 0.0f, 200.0f, 200.0f, 10.0f, 10.0f);
	set_corner(&outer, VEC_CORNER_BOTTOM_LEFT, 0.0f, 0.0f);
	vec_rrect_spread(&outer, 20.0f, &grown);
	CHECK_NEAR(grown.radii[VEC_CORNER_BOTTOM_LEFT][0], 0.0, 0.0);
	CHECK_NEAR(grown.radii[VEC_CORNER_BOTTOM_LEFT][1], 0.0, 0.0);

	/* A negative spread takes the amount off the radius, to 0 at the least, and shrinks the rectangle. */
	outer = make_uniform(0.0f, 0.0f, 200.0f, 100.0f, 10.0f, 10.0f);
	vec_rrect_spread(&outer, -4.0f, &grown);
	CHECK_NEAR(grown.rect.left, 4.0, 0.0);
	CHECK_NEAR(grown.rect.bottom, 96.0, 0.0);
	CHECK_NEAR(grown.radii[0][0], 6.0, 1e-5);
	vec_rrect_spread(&outer, -15.0f, &grown);
	CHECK_NEAR(grown.radii[0][0], 0.0, 0.0);
	CHECK_NEAR(grown.radii[0][1], 0.0, 0.0);

	/* A shrink past the size leaves a zero size rectangle. */
	vec_rrect_spread(&outer, -60.0f, &grown);
	CHECK_NEAR(grown.rect.bottom - grown.rect.top, 0.0, 0.0);
	CHECK(grown.rect.right >= grown.rect.left);

	/* A spread that is not a number is none; the output may be the input. */
	vec_rrect_spread(&outer, nan_value, &grown);
	CHECK_NEAR(grown.rect.left, 0.0, 0.0);
	vec_rrect_spread(&outer, 5.0f, &outer);
	CHECK_NEAR(outer.rect.left, -5.0, 0.0);
}

/* Returns the coverage a plain rectangle gives a pixel, by the overlap of the intervals (the display list's rule). */
static double
plain_coverage(
	const struct vec_rect *rect,
	double px,
	double py)
{
	double x0;
	double x1;
	double y0;
	double y1;

	x0 = px > rect->left ? px : rect->left;
	x1 = px + 1.0 < rect->right ? px + 1.0 : rect->right;
	y0 = py > rect->top ? py : rect->top;
	y1 = py + 1.0 < rect->bottom ? py + 1.0 : rect->bottom;
	if (x1 <= x0 || y1 <= y0)
		return 0.0;
	return (x1 - x0) * (y1 - y0);
}

/* Checks the coverage of a rectangle without corners, of circles and of ellipses. */
static void
test_coverage(void)
{
	struct vec_rrect rrect;
	struct vec_rrect square;
	uint32_t seed;
	int i;
	int px;
	int py;
	float radial;
	float previous;
	float fx;
	float fy;
	float value;

	/* Without radii the coverage is the plain rectangle's overlap, for whole-pixel and fractional edges. */
	rrect = make_uniform(2.0f, 3.0f, 10.0f, 9.0f, 0.0f, 0.0f);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 5.0f, 5.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 2.0f, 3.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 9.0f, 8.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 10.0f, 5.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 1.0f, 5.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 5.0f, 9.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 5.0f, 2.0f), 0.0, 0.0);
	rrect = make_uniform(2.25f, 3.5f, 10.5f, 9.0f, 0.0f, 0.0f);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 2.0f, 5.0f), 0.75, 1e-6);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 10.0f, 5.0f), 0.5, 1e-6);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 2.0f, 3.0f), 0.375, 1e-6);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 10.0f, 8.0f), 0.5, 1e-6);

	/* Random fractional rectangles and pixels agree with the plain overlap everywhere. */
	seed = 12345;
	for (i = 0; i < 20000; i++) {
		rrect = make_uniform(check_random_float(&seed, -20.0f, 20.0f), check_random_float(&seed, -20.0f, 20.0f), 0.0f, 0.0f,
		    0.0f, 0.0f);
		rrect.rect.right = rrect.rect.left + check_random_float(&seed, 0.0f, 30.0f);
		rrect.rect.bottom = rrect.rect.top + check_random_float(&seed, 0.0f, 30.0f);
		fx = (float)(int)check_random_float(&seed, -30.0f, 30.0f);
		fy = (float)(int)check_random_float(&seed, -30.0f, 30.0f);
		CHECK_NEAR(vec_rrect_coverage(&rrect, fx, fy), plain_coverage(&rrect.rect, fx, fy), 1e-5);
	}

	/*
	 * A 40 by 40 box with circular corners of radius 20 is a circle: the
	 * centre (20, 20).  The pixel centre (8, 4), at distance
	 * sqrt(12^2 + 16^2) = 20 from it, is on the curve and covers half.
	 */
	rrect = make_uniform(0.0f, 0.0f, 40.0f, 40.0f, 20.0f, 20.0f);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 7.5f, 3.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 20.0f - 0.5f + 12.0f, 3.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 7.5f, 40.0f - 4.0f - 0.5f), 0.5, 1e-5);

	/* Deep inside a corner is covered, far outside it is not, and the box's own corner pixel is empty. */
	CHECK_NEAR(vec_rrect_coverage(&rrect, 18.0f, 18.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 10.0f, 10.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 0.0f, 0.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 39.0f, 0.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 39.0f, 39.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 0.0f, 39.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 20.0f, 20.0f), 1.0, 0.0);

	/* Half a pixel inside the curve is full, half a pixel outside is nothing (the ramp is one pixel wide at 45 degrees too). */
	CHECK_NEAR(vec_rrect_coverage(&rrect, 7.8f, 3.9f), 1.0, 1e-6);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 7.2f, 3.1f), 0.5 - 0.4939024, 2e-4);

	/* Along a ray out of the circle's centre, while the pixel is wholly in the box, the coverage never increases. */
	previous = 1.0f;
	for (i = 0; i < 140; i++) {
		radial = (float)i * 0.1f;
		value = vec_rrect_coverage(&rrect, 20.0f - 0.6f * (10.0f + radial) - 0.5f, 20.0f - 0.8f * (10.0f + radial) - 0.5f);
		CHECK(value <= previous + 1e-6f);
		previous = value;
	}
	CHECK_NEAR(previous, 0.0, 0.0);

	/* The four corners of a symmetric box are mirror images of one another, for every pixel. */
	for (px = 0; px < 20; px++) {
		for (py = 0; py < 20; py++) {
			value = vec_rrect_coverage(&rrect, (float)px, (float)py);
			CHECK_NEAR(vec_rrect_coverage(&rrect, (float)(39 - px), (float)py), value, 1e-6);
			CHECK_NEAR(vec_rrect_coverage(&rrect, (float)px, (float)(39 - py)), value, 1e-6);
			CHECK_NEAR(vec_rrect_coverage(&rrect, (float)(39 - px), (float)(39 - py)), value, 1e-6);
		}
	}

	/*
	 * An ellipse: the box 60 by 40 with corners rx 30, ry 10 has the top
	 * left centre (30, 10).  (12, 2) is at u = -0.6, v = -0.8 and (6, 4)
	 * at u = -0.8, v = -0.6, both on the curve: coverage one half.
	 */
	rrect = make_uniform(0.0f, 0.0f, 60.0f, 40.0f, 30.0f, 10.0f);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 11.5f, 1.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 5.5f, 3.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 60.0f - 12.0f - 0.5f, 40.0f - 2.0f - 0.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 12.0f - 0.5f, 40.0f - 2.0f - 0.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 0.0f, 0.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 30.0f, 20.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 0.0f, 12.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 1.0f, 1.0f), 0.0, 0.0);

	/* A corner with a different radius at each corner uses its own: a square corner is full right to the box's pixel. */
	rrect = make_uniform(0.0f, 0.0f, 40.0f, 40.0f, 20.0f, 20.0f);
	set_corner(&rrect, VEC_CORNER_TOP_LEFT, 0.0f, 0.0f);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 0.0f, 0.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 39.0f, 0.0f), 0.0, 0.0);
	square = rrect;
	CHECK_NEAR(vec_rrect_coverage(&square, 20.0f, 0.0f), 0.99985, 1e-4);

	/* A position that is not finite covers nothing. */
	CHECK_NEAR(vec_rrect_coverage(&rrect, nanf(""), 5.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, 5.0f, INFINITY), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_coverage(&rrect, -INFINITY, 5.0f), 0.0, 0.0);
}

/* Checks the border ring. */
static void
test_ring(void)
{
	struct vec_rrect outer;
	struct vec_rrect inner;
	float widths[4];
	int px;
	int py;
	float ring;
	float whole;

	/* A 40 by 40 box with radius 10 and a 4 pixel border. */
	outer = make_uniform(0.0f, 0.0f, 40.0f, 40.0f, 10.0f, 10.0f);
	widths[0] = 4.0f;
	widths[1] = 4.0f;
	widths[2] = 4.0f;
	widths[3] = 4.0f;
	vec_rrect_inset(&outer, widths, &inner);

	/* A pixel inside the border is full, one in the padding is empty, one outside the box is empty. */
	CHECK_NEAR(vec_rrect_ring_coverage(&outer, &inner, 20.0f, 1.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_ring_coverage(&outer, &inner, 1.0f, 20.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_ring_coverage(&outer, &inner, 38.0f, 20.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_ring_coverage(&outer, &inner, 20.0f, 36.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_ring_coverage(&outer, &inner, 20.0f, 4.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_ring_coverage(&outer, &inner, 20.0f, 20.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_ring_coverage(&outer, &inner, -1.0f, 20.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_ring_coverage(&outer, &inner, 0.0f, 0.0f), 0.0, 0.0);

	/* The ring is the outer coverage less the inner one for every pixel, and lies in [0, 1]. */
	for (px = -2; px < 42; px++) {
		for (py = -2; py < 42; py++) {
			ring = vec_rrect_ring_coverage(&outer, &inner, (float)px, (float)py);
			whole = vec_rrect_coverage(&outer, (float)px, (float)py) - vec_rrect_coverage(&inner, (float)px, (float)py);
			if (whole < 0.0f)
				whole = 0.0f;
			CHECK_NEAR(ring, whole, 1e-6);
			CHECK(ring >= 0.0f && ring <= 1.0f);
		}
	}

	/* A border wider than half the box leaves no padding: the ring is the whole box. */
	widths[0] = 30.0f;
	widths[1] = 30.0f;
	widths[2] = 30.0f;
	widths[3] = 30.0f;
	vec_rrect_inset(&outer, widths, &inner);
	CHECK_NEAR(vec_rrect_ring_coverage(&outer, &inner, 20.0f, 20.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_ring_coverage(&outer, &inner, 5.0f, 5.0f), vec_rrect_coverage(&outer, 5.0f, 5.0f), 1e-6);
}

/* Checks the shares of the four sides. */
static void
test_side_share(void)
{
	struct vec_rrect outer;
	float widths[4];
	float sum;
	int side;
	int i;
	uint32_t seed;
	float px;
	float py;

	/*
	 * A 100 by 60 box with a 10 pixel border on every side: in the top
	 * left quadrant (centre x < 50, y < 30) the transition is the
	 * diagonal from (0, 0) to (10, 10).
	 */
	outer = make_uniform(0.0f, 0.0f, 100.0f, 60.0f, 0.0f, 0.0f);
	widths[VEC_SIDE_TOP] = 10.0f;
	widths[VEC_SIDE_RIGHT] = 10.0f;
	widths[VEC_SIDE_BOTTOM] = 10.0f;
	widths[VEC_SIDE_LEFT] = 10.0f;

	/* On the diagonal the two sides share the pixel equally: the centre (10, 10) and (20, 20). */
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 9.5f, 9.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_LEFT, 9.5f, 9.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 19.5f, 19.5f), 0.5, 1e-5);

	/* Inside a side's band the side has everything: the top band at (30, 5), the left at (5, 25). */
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 29.5f, 4.5f), 1.0, 1e-6);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_LEFT, 29.5f, 4.5f), 0.0, 1e-6);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_LEFT, 4.5f, 24.5f), 1.0, 1e-6);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 4.5f, 24.5f), 0.0, 1e-6);

	/* The two sides outside the quadrant get 0, wherever the pixel is in it. */
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_RIGHT, 29.5f, 4.5f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_BOTTOM, 29.5f, 4.5f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_RIGHT, 95.0f, 55.0f), 1.0 - vec_rrect_side_share(&outer, widths, VEC_SIDE_BOTTOM, 95.0f, 55.0f), 1e-6);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_LEFT, 95.0f, 55.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 95.0f, 55.0f), 0.0, 0.0);

	/* The quadrants are cut at the middle lines: x = 50 and y = 30, the centre at the line belonging to the later quadrant. */
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_RIGHT, 49.5f, 4.5f), 0.0, 1e-6);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 49.5f, 4.5f), 1.0, 1e-6);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 50.0f, 4.5f), 1.0, 1e-6);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_BOTTOM, 20.0f, 29.5f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_LEFT, 4.5f, 29.5f), 1.0, 1e-6);

	/* Out of range sides give 0. */
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, -1, 9.5f, 9.5f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, 4, 9.5f, 9.5f), 0.0, 0.0);

	/*
	 * Different widths: top 10, left 30.  The transition in the top left
	 * quadrant runs from (0, 0) through (30, 10); the centre (30, 10) is
	 * on it, (60 is beyond the quadrant).
	 */
	widths[VEC_SIDE_TOP] = 10.0f;
	widths[VEC_SIDE_LEFT] = 30.0f;
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 29.5f, 9.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_LEFT, 29.5f, 9.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 19.5f, 6.0f), 0.5 + (10.0 * 20.0 - 30.0 * 6.5) / sqrt(1000.0), 1e-5);

	/* Top right: right 20, top 10: the line from (100, 0) through (80, 10); the centre (80, 10) is on it. */
	widths[VEC_SIDE_TOP] = 10.0f;
	widths[VEC_SIDE_RIGHT] = 20.0f;
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 79.5f, 9.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_RIGHT, 79.5f, 9.5f), 0.5, 1e-5);

	/* Bottom left: bottom 15, left 15: the diagonal from (0, 60) to (15, 45); the centre (15, 45). */
	widths[VEC_SIDE_BOTTOM] = 15.0f;
	widths[VEC_SIDE_LEFT] = 15.0f;
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_BOTTOM, 14.5f, 44.5f), 0.5, 1e-5);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_LEFT, 14.5f, 44.5f), 0.5, 1e-5);

	/* A side without width gives everything to the other: top 10 with left 0, then top 0 with left 10. */
	widths[VEC_SIDE_TOP] = 10.0f;
	widths[VEC_SIDE_RIGHT] = 10.0f;
	widths[VEC_SIDE_BOTTOM] = 10.0f;
	widths[VEC_SIDE_LEFT] = 0.0f;
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 2.5f, 25.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_LEFT, 2.5f, 25.0f), 0.0, 0.0);
	widths[VEC_SIDE_TOP] = 0.0f;
	widths[VEC_SIDE_LEFT] = 10.0f;
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_LEFT, 40.0f, 3.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 40.0f, 3.0f), 0.0, 0.0);

	/* Both widths of a quadrant 0: the horizontal side gets 1. */
	widths[VEC_SIDE_TOP] = 0.0f;
	widths[VEC_SIDE_LEFT] = 0.0f;
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 20.0f, 3.0f), 1.0, 0.0);
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_LEFT, 20.0f, 3.0f), 0.0, 0.0);

	/* A width that is negative or not a number is taken as 0. */
	widths[VEC_SIDE_TOP] = -5.0f;
	widths[VEC_SIDE_LEFT] = nanf("");
	CHECK_NEAR(vec_rrect_side_share(&outer, widths, VEC_SIDE_TOP, 20.0f, 3.0f), 1.0, 0.0);

	/* The four shares of a pixel sum to 1, for random boxes, widths and pixels. */
	seed = 4242;
	for (i = 0; i < 20000; i++) {
		outer = make_uniform(check_random_float(&seed, -30.0f, 30.0f), check_random_float(&seed, -30.0f, 30.0f), 0.0f, 0.0f,
		    0.0f, 0.0f);
		outer.rect.right = outer.rect.left + check_random_float(&seed, 1.0f, 80.0f);
		outer.rect.bottom = outer.rect.top + check_random_float(&seed, 1.0f, 80.0f);
		for (side = 0; side < 4; side++) {
			widths[side] = check_random_float(&seed, 0.0f, 25.0f);
			if (check_random(&seed) % 5 == 0)
				widths[side] = 0.0f;
		}
		px = check_random_float(&seed, outer.rect.left - 5.0f, outer.rect.right + 5.0f);
		py = check_random_float(&seed, outer.rect.top - 5.0f, outer.rect.bottom + 5.0f);
		sum = 0.0f;
		for (side = 0; side < 4; side++) {
			sum += vec_rrect_side_share(&outer, widths, side, px, py);
			CHECK(vec_rrect_side_share(&outer, widths, side, px, py) >= 0.0f);
			CHECK(vec_rrect_side_share(&outer, widths, side, px, py) <= 1.0f);
		}
		CHECK_NEAR(sum, 1.0, 1e-5);
	}
}

/* Checks the error function against tabulated values and the structure of the function. */
static void
test_erf(void)
{
	float x;
	int i;
	double exact;

	/* erf(0) = 0, to the approximation's error. */
	CHECK_NEAR(vec_erf(0.0f), 0.0, 2e-7);

	/* Values tabulated in Abramowitz and Stegun, table 7.1, and common references. */
	CHECK_NEAR(vec_erf(0.1f), 0.1124629, 5e-7);
	CHECK_NEAR(vec_erf(0.5f), 0.5204999, 5e-7);
	CHECK_NEAR(vec_erf(1.0f), 0.8427008, 5e-7);
	CHECK_NEAR(vec_erf(1.5f), 0.9661051, 5e-7);
	CHECK_NEAR(vec_erf(2.0f), 0.9953223, 5e-7);
	CHECK_NEAR(vec_erf(3.0f), 0.9999779, 5e-7);

	/* Large arguments reach 1, and the function is odd. */
	CHECK_NEAR(vec_erf(6.0f), 1.0, 5e-7);
	CHECK_NEAR(vec_erf(1.0e10f), 1.0, 0.0);
	CHECK_NEAR(vec_erf(FLT_MAX), 1.0, 0.0);
	CHECK_NEAR(vec_erf(INFINITY), 1.0, 0.0);
	CHECK_NEAR(vec_erf(-INFINITY), -1.0, 0.0);
	CHECK_NEAR(vec_erf(-1.0e10f), -1.0, 0.0);
	CHECK_NEAR(vec_erf(-0.5f), -0.5204999, 5e-7);
	CHECK_NEAR(vec_erf(-1.0f), -0.8427008, 5e-7);

	/* A NaN gives 0. */
	CHECK_NEAR(vec_erf(nanf("")), 0.0, 0.0);

	/*
	 * Across a sweep the function is odd exactly, within [-1, 1], and
	 * within 5e-7 of the mathematical function (the C library's erf in
	 * double precision stands for the mathematical function here).
	 */
	for (i = -1000; i <= 1000; i++) {
		x = (float)i * 0.005f;
		exact = erf((double)x);
		CHECK_NEAR(vec_erf(x), exact, 5e-7);
		CHECK_NEAR(vec_erf(-x), -vec_erf(x), 0.0);
		CHECK(vec_erf(x) >= -1.0f && vec_erf(x) <= 1.0f);
	}
}

/* The right or left edge of a rounded rectangle at height y, by the definition of the rounded corners. */
static double
dense_edge(
	const struct vec_rrect *rrect,
	int right_edge,
	double y)
{
	double x_side;
	double direction;
	double rx_top;
	double ry_top;
	double rx_bottom;
	double ry_bottom;
	double ey;
	double v;
	double root;

	x_side = rrect->rect.left;
	direction = -1.0;
	rx_top = rrect->radii[VEC_CORNER_TOP_LEFT][0];
	ry_top = rrect->radii[VEC_CORNER_TOP_LEFT][1];
	rx_bottom = rrect->radii[VEC_CORNER_BOTTOM_LEFT][0];
	ry_bottom = rrect->radii[VEC_CORNER_BOTTOM_LEFT][1];
	if (right_edge) {
		x_side = rrect->rect.right;
		direction = 1.0;
		rx_top = rrect->radii[VEC_CORNER_TOP_RIGHT][0];
		ry_top = rrect->radii[VEC_CORNER_TOP_RIGHT][1];
		rx_bottom = rrect->radii[VEC_CORNER_BOTTOM_RIGHT][0];
		ry_bottom = rrect->radii[VEC_CORNER_BOTTOM_RIGHT][1];
	}

	/* In the top corner's rows the edge follows its ellipse. */
	if (ry_top > 0.0 && y < rrect->rect.top + ry_top) {
		ey = rrect->rect.top + ry_top;
		v = (y - ey) / ry_top;
		root = 1.0 - v * v;
		if (root < 0.0)
			root = 0.0;
		return x_side - direction * rx_top + direction * rx_top * sqrt(root);
	}

	/* In the bottom corner's rows too. */
	if (ry_bottom > 0.0 && y > rrect->rect.bottom - ry_bottom) {
		ey = rrect->rect.bottom - ry_bottom;
		v = (y - ey) / ry_bottom;
		root = 1.0 - v * v;
		if (root < 0.0)
			root = 0.0;
		return x_side - direction * rx_bottom + direction * rx_bottom * sqrt(root);
	}

	return x_side;
}

/*
 * The blurred coverage of a rounded rectangle at (cx, cy) by dense
 * numerical integration of the definition: the sum over rows of the
 * normal density along y times the integral of the density along x
 * between the row's left and right edge, by the midpoint rule over
 * DENSE_STEPS equal steps of the rows within DENSE_REACH deviations.
 */
static double
dense_shadow(
	const struct vec_rrect *rrect,
	double sigma,
	double cx,
	double cy)
{
	double low;
	double high;
	double step;
	double y;
	double density;
	double left;
	double right;
	double sum;
	double root2sigma;
	int i;

	low = rrect->rect.top;
	high = rrect->rect.bottom;
	if (low < cy - DENSE_REACH * sigma)
		low = cy - DENSE_REACH * sigma;
	if (high > cy + DENSE_REACH * sigma)
		high = cy + DENSE_REACH * sigma;
	if (high <= low)
		return 0.0;
	step = (high - low) / DENSE_STEPS;
	root2sigma = sigma * sqrt(2.0);
	sum = 0.0;
	for (i = 0; i < DENSE_STEPS; i++) {
		y = low + ((double)i + 0.5) * step;
		density = exp(-(y - cy) * (y - cy) / (2.0 * sigma * sigma)) / (sigma * sqrt(2.0 * TEST_PI));
		right = 0.5 * erf((dense_edge(rrect, 1, y) - cx) / root2sigma);
		left = 0.5 * erf((dense_edge(rrect, 0, y) - cx) / root2sigma);
		sum += density * (right - left) * step;
	}
	if (sum < 0.0)
		sum = 0.0;
	if (sum > 1.0)
		sum = 1.0;
	return sum;
}

/*
 * Compares the shadow with the dense integration at the pixels of a grid
 * around the rectangle (every stride'th pixel, within 4 sigma + 2 of it),
 * checks every difference and returns the largest.
 */
static double
compare_shadow(
	const struct vec_rrect *rrect,
	float sigma,
	int stride)
{
	int x0;
	int x1;
	int y0;
	int y1;
	int px;
	int py;
	double difference;
	double worst;
	float value;
	double expected;
	int margin;

	margin = (int)(4.0f * sigma) + 2;
	x0 = (int)rrect->rect.left - margin;
	x1 = (int)rrect->rect.right + margin;
	y0 = (int)rrect->rect.top - margin;
	y1 = (int)rrect->rect.bottom + margin;
	worst = 0.0;
	for (py = y0; py < y1; py += stride) {
		for (px = x0; px < x1; px += stride) {
			value = vec_rrect_shadow(rrect, sigma, (float)px, (float)py);
			expected = dense_shadow(rrect, sigma, px + 0.5, py + 0.5);
			difference = (double)value - expected;
			if (difference < 0.0)
				difference = -difference;
			if (difference > worst)
				worst = difference;
			CHECK_NEAR(value, expected, SHADOW_TOLERANCE);
		}
	}
	return worst;
}

/* The product of the two erf factors of a rectangle without corners, by hand: the density's mass in the rectangle. */
static double
box_blur(
	const struct vec_rect *rect,
	double sigma,
	double cx,
	double cy)
{
	double k;
	double in_x;
	double in_y;

	k = 1.0 / (sigma * sqrt(2.0));
	in_x = 0.5 * (erf((rect->right - cx) * k) - erf((rect->left - cx) * k));
	in_y = 0.5 * (erf((rect->bottom - cy) * k) - erf((rect->top - cy) * k));
	return in_x * in_y;
}

/* Checks the shadow: the sharp case, the rectangle without corners in closed form, the limits, and the dense comparison. */
static void
test_shadow(void)
{
	struct vec_rrect rrect;
	struct vec_rrect big;
	uint32_t seed;
	int i;
	int px;
	int py;
	double worst;
	float sigma;
	float fx;
	float fy;

	/* A blur under the smallest sigma is the sharp coverage, also for a sigma that is negative or not a number. */
	rrect = make_uniform(0.0f, 0.0f, 40.0f, 40.0f, 15.0f, 15.0f);
	for (px = -2; px < 42; px += 3) {
		for (py = -2; py < 42; py += 3) {
			CHECK_NEAR(vec_rrect_shadow(&rrect, 0.0f, (float)px, (float)py), vec_rrect_coverage(&rrect, (float)px, (float)py), 0.0);
			CHECK_NEAR(vec_rrect_shadow(&rrect, 0.49f, (float)px, (float)py), vec_rrect_coverage(&rrect, (float)px, (float)py), 0.0);
			CHECK_NEAR(vec_rrect_shadow(&rrect, -1.0f, (float)px, (float)py), vec_rrect_coverage(&rrect, (float)px, (float)py), 0.0);
			CHECK_NEAR(vec_rrect_shadow(&rrect, nanf(""), (float)px, (float)py), vec_rrect_coverage(&rrect, (float)px, (float)py), 0.0);
			CHECK_NEAR(vec_rrect_shadow(&rrect, INFINITY, (float)px, (float)py) >= 0.0f, 1.0, 0.0);
		}
	}

	/* A rectangle without corners: the shadow is the product of two error function factors, within 1e-4. */
	rrect = make_uniform(20.0f, 30.0f, 90.0f, 70.0f, 0.0f, 0.0f);
	seed = 99;
	for (i = 0; i < 2000; i++) {
		sigma = check_random_float(&seed, 0.5f, 12.0f);
		fx = (float)(int)check_random_float(&seed, -20.0f, 130.0f);
		fy = (float)(int)check_random_float(&seed, -20.0f, 110.0f);
		CHECK_NEAR(vec_rrect_shadow(&rrect, sigma, fx, fy), box_blur(&rrect.rect, sigma, fx + 0.5, fy + 0.5), 1e-4);
	}

	/* The centre of a large box is 1, far away is 0, on an edge far from the corners it is a half. */
	big = make_uniform(0.0f, 0.0f, 200.0f, 200.0f, 0.0f, 0.0f);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, 100.0f, 100.0f), 1.0, 1e-4);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, 1000.0f, 100.0f), 0.0, 1e-6);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, -50.0f, -50.0f), 0.0, 1e-6);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, -0.5f, 100.0f), 0.5, 1e-4);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, 199.5f, 100.0f), 0.5, 1e-4);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, 100.0f, -0.5f), 0.5, 1e-4);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, 100.0f, 199.5f), 0.5, 1e-4);
	big = make_uniform(0.0f, 0.0f, 200.0f, 200.0f, 60.0f, 60.0f);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, 100.0f, 100.0f), 1.0, 1e-4);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, 1000.0f, 100.0f), 0.0, 1e-6);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, -0.5f, 100.0f), 0.5, 1e-4);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, 100.0f, 199.5f), 0.5, 1e-4);

	/* The rectangle's corner pixel at the corner of a round box is almost 0, at the middle of the corner's arc about a half. */
	CHECK(vec_rrect_shadow(&big, 1.0f, 0.0f, 0.0f) < 0.01f);

	/* A position that is not finite, an empty rectangle, and a sigma beyond any range give a value in range. */
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, nanf(""), 4.0f), 0.0, 0.0);
	CHECK_NEAR(vec_rrect_shadow(&big, 3.0f, 4.0f, INFINITY), 0.0, 0.0);
	rrect = make_uniform(5.0f, 5.0f, 5.0f, 9.0f, 0.0f, 0.0f);
	CHECK_NEAR(vec_rrect_shadow(&rrect, 3.0f, 5.0f, 6.0f), 0.0, 0.0);
	CHECK(vec_rrect_shadow(&big, 1.0e30f, 5.0f, 6.0f) >= 0.0f);
	CHECK(vec_rrect_shadow(&big, 1.0e30f, 5.0f, 6.0f) <= 1.0f);
	CHECK(vec_rrect_shadow(&big, FLT_MAX, 5.0f, 6.0f) <= 1.0f);

	/* Dense comparison: a circle of radius 8, sigma 0.5 to 3, every pixel. */
	rrect = make_uniform(10.0f, 10.0f, 74.0f, 58.0f, 8.0f, 8.0f);
	worst = compare_shadow(&rrect, 0.5f, 1);
	CHECK(worst < SHADOW_TOLERANCE);
	worst = compare_shadow(&rrect, 1.0f, 1);
	CHECK(worst < SHADOW_TOLERANCE);
	worst = compare_shadow(&rrect, 2.0f, 1);
	CHECK(worst < SHADOW_TOLERANCE);
	worst = compare_shadow(&rrect, 3.0f, 1);
	CHECK(worst < SHADOW_TOLERANCE);

	/* Circles with radius 20, 24 (half the height). */
	rrect = make_uniform(10.0f, 10.0f, 90.0f, 58.0f, 20.0f, 20.0f);
	worst = compare_shadow(&rrect, 0.5f, 1);
	CHECK(worst < SHADOW_TOLERANCE);
	worst = compare_shadow(&rrect, 1.5f, 1);
	CHECK(worst < SHADOW_TOLERANCE);
	worst = compare_shadow(&rrect, 3.0f, 1);
	CHECK(worst < SHADOW_TOLERANCE);
	rrect = make_uniform(10.0f, 10.0f, 90.0f, 58.0f, 24.0f, 24.0f);
	worst = compare_shadow(&rrect, 2.5f, 1);
	CHECK(worst < SHADOW_TOLERANCE);

	/* Elliptical corners 92 by 30 (rx != ry), also 30 by 92 and a mixed set of radii. */
	rrect = make_uniform(10.0f, 10.0f, 210.0f, 74.0f, 92.0f, 30.0f);
	worst = compare_shadow(&rrect, 0.5f, 3);
	CHECK(worst < SHADOW_TOLERANCE);
	worst = compare_shadow(&rrect, 1.0f, 3);
	CHECK(worst < SHADOW_TOLERANCE);
	worst = compare_shadow(&rrect, 3.0f, 3);
	CHECK(worst < SHADOW_TOLERANCE);
	rrect = make_uniform(10.0f, 10.0f, 74.0f, 210.0f, 30.0f, 92.0f);
	worst = compare_shadow(&rrect, 1.0f, 3);
	CHECK(worst < SHADOW_TOLERANCE);
	rrect = make_uniform(10.0f, 10.0f, 150.0f, 130.0f, 0.0f, 0.0f);
	set_corner(&rrect, VEC_CORNER_TOP_LEFT, 60.0f, 10.0f);
	set_corner(&rrect, VEC_CORNER_TOP_RIGHT, 10.0f, 50.0f);
	set_corner(&rrect, VEC_CORNER_BOTTOM_RIGHT, 40.0f, 40.0f);
	vec_rrect_normalize(&rrect);
	worst = compare_shadow(&rrect, 2.0f, 2);
	CHECK(worst < SHADOW_TOLERANCE);
	worst = compare_shadow(&rrect, 0.7f, 2);
	CHECK(worst < SHADOW_TOLERANCE);

	/* Corners of 100 pixels: the rows of one half lie far outside the window at sigma 0.5 and are skipped. */
	rrect = make_uniform(10.0f, 10.0f, 250.0f, 230.0f, 100.0f, 100.0f);
	worst = compare_shadow(&rrect, 0.5f, 5);
	CHECK(worst < SHADOW_TOLERANCE);
	worst = compare_shadow(&rrect, 1.0f, 5);
	CHECK(worst < SHADOW_TOLERANCE);
	worst = compare_shadow(&rrect, 3.0f, 5);
	CHECK(worst < SHADOW_TOLERANCE);

	/* Large blurs. */
	rrect = make_uniform(20.0f, 20.0f, 220.0f, 180.0f, 60.0f, 50.0f);
	worst = compare_shadow(&rrect, 10.0f, 9);
	CHECK(worst < SHADOW_TOLERANCE);
	worst = compare_shadow(&rrect, 30.0f, 17);
	CHECK(worst < SHADOW_TOLERANCE);

	/* Pixels far from a corner but near the long edge of a box with 100 pixel corners, where the window skips the far rows. */
	rrect = make_uniform(10.0f, 10.0f, 250.0f, 230.0f, 100.0f, 100.0f);
	for (py = 100; py < 140; py += 3) {
		for (px = 6; px < 16; px++) {
			CHECK_NEAR(vec_rrect_shadow(&rrect, 0.5f, (float)px, (float)py), dense_shadow(&rrect, 0.5, px + 0.5, py + 0.5), SHADOW_TOLERANCE);
		}
	}
}

/* Runs the functions on random and hostile input, where only the range of the results and the sanitizers matter. */
static void
test_random(void)
{
	struct vec_rrect rrect;
	struct vec_rrect inner;
	struct vec_rrect grown;
	float widths[4];
	float values[12];
	uint32_t seed;
	int i;
	int j;
	int side;
	float sigma;
	float px;
	float py;
	float result;
	float amount;

	/* A pool of ordinary and hostile values. */
	values[0] = 0.0f;
	values[1] = -1.0f;
	values[2] = nanf("");
	values[3] = INFINITY;
	values[4] = -INFINITY;
	values[5] = FLT_MAX;
	values[6] = -FLT_MAX;
	values[7] = 1.0e-30f;
	values[8] = 16777216.0f;
	values[9] = 0.001f;
	values[10] = 1.0e20f;
	values[11] = 1.0f / 256.0f;

	seed = 2026;
	for (i = 0; i < 4000; i++) {
		/* A rectangle and radii, each from the pool a third of the time. */
		rrect.rect.left = check_random_float(&seed, -100.0f, 100.0f);
		rrect.rect.top = check_random_float(&seed, -100.0f, 100.0f);
		rrect.rect.right = rrect.rect.left + check_random_float(&seed, -10.0f, 150.0f);
		rrect.rect.bottom = rrect.rect.top + check_random_float(&seed, -10.0f, 150.0f);
		if (check_random(&seed) % 12 == 0)
			rrect.rect.left = values[check_random(&seed) % 12];
		if (check_random(&seed) % 12 == 0)
			rrect.rect.bottom = values[check_random(&seed) % 12];
		for (j = 0; j < 8; j++) {
			rrect.radii[j / 2][j % 2] = check_random_float(&seed, -5.0f, 120.0f);
			if (check_random(&seed) % 3 == 0)
				rrect.radii[j / 2][j % 2] = values[check_random(&seed) % 12];
		}
		vec_rrect_normalize(&rrect);
		if (vec_finite(rrect.rect.left) && vec_finite(rrect.rect.right) && vec_finite(rrect.rect.top) && vec_finite(rrect.rect.bottom))
			check_normalized(&rrect);

		/* Widths, spread, sigma and pixel. */
		for (side = 0; side < 4; side++) {
			widths[side] = check_random_float(&seed, -2.0f, 40.0f);
			if (check_random(&seed) % 6 == 0)
				widths[side] = values[check_random(&seed) % 12];
		}
		amount = check_random_float(&seed, -60.0f, 60.0f);
		if (check_random(&seed) % 8 == 0)
			amount = values[check_random(&seed) % 12];
		sigma = check_random_float(&seed, 0.0f, 20.0f);
		if (check_random(&seed) % 8 == 0)
			sigma = values[check_random(&seed) % 12];
		px = check_random_float(&seed, -150.0f, 250.0f);
		py = check_random_float(&seed, -150.0f, 250.0f);
		if (check_random(&seed) % 10 == 0)
			px = values[check_random(&seed) % 12];
		if (check_random(&seed) % 10 == 0)
			py = values[check_random(&seed) % 12];

		/* Every function, whose results must be numbers in range. */
		vec_rrect_inset(&rrect, widths, &inner);
		vec_rrect_spread(&rrect, amount, &grown);
		result = vec_rrect_coverage(&rrect, px, py);
		CHECK(result >= 0.0f && result <= 1.0f);
		result = vec_rrect_ring_coverage(&rrect, &inner, px, py);
		CHECK(result >= 0.0f && result <= 1.0f);
		result = vec_rrect_ring_coverage(&grown, &rrect, px, py);
		CHECK(result >= 0.0f && result <= 1.0f);
		result = vec_rrect_shadow(&rrect, sigma, px, py);
		CHECK(result >= 0.0f && result <= 1.0f);
		result = vec_rrect_shadow(&grown, sigma, px, py);
		CHECK(result >= 0.0f && result <= 1.0f);
		for (side = 0; side < 4; side++) {
			result = vec_rrect_side_share(&rrect, widths, side, px, py);
			CHECK(result >= 0.0f && result <= 1.0f);
		}
		result = vec_erf(px);
		CHECK(result >= -1.0f && result <= 1.0f);
	}
}

int
main(void)
{
	/* Runs every group of checks. */
	test_normalize();
	test_inset_spread();
	test_coverage();
	test_ring();
	test_side_share();
	test_erf();
	test_shadow();
	test_random();

	/* Reports the result. */
	return check_finish("rrect");
}
