/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inverse trigonometric functions: atan, atan2, asin and acos.
 *
 * All four rest on one kernel, atan u for 0 <= u <= 1 as a double-double.
 * u is written as c + (u - c) with c = i/64, and
 *   atan u = atan c + atan t,  t = (u - c) / (1 + u c),  |t| <= 1/128,
 * with atan c from a table and atan t from its Taylor series to t^13.
 * An argument above one goes through pi/2 - atan(1/u); asin and acos form
 * sqrt((1 - x)(1 + x)) as a double-double and take the arctangent of
 * whichever ratio of it and x is at most one, so no formula subtracts
 * nearly equal values.  The result is rounded once.
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/* The encoding of 2^-27: below it atan x and asin x round to x. */
#define LIBM_ATRIG_TINY UINT64_C(0x3e40000000000000)

/* The encoding of 2^54: above it atan x rounds to pi/2. */
#define LIBM_ATAN_HUGE UINT64_C(0x4350000000000000)

/* The encoding of 1.0. */
#define LIBM_ONE_BITS UINT64_C(0x3ff0000000000000)

static struct libm_dd libm_complement(struct libm_dd angle);
static struct libm_dd libm_negate(struct libm_dd value);
static struct libm_dd libm_root_of_one_minus_square(double magnitude);
static struct libm_dd libm_quotient(double numerator, double denominator);

/*
 * Returns atan u as a double-double for 0 <= u <= 1.
 */
struct libm_dd
__libm_atan_dd(
	struct libm_dd u)
{
	struct libm_dd numerator;
	struct libm_dd denominator;
	struct libm_dd ratio;
	struct libm_dd result;
	const double *coefficients;
	double point;
	double square;
	double tail;
	int index;

	/* The nearest table point c = i/64. */
	index = (int)(u.high * 64.0 + 0.5);
	point = (double)index * 0.015625;

	/*
	 * t = (u - c) / (1 + u c).  u - c is exact next to u; the
	 * denominator is formed from the exact product.
	 */
	numerator = libm_two_sum(u.high - point, u.low);
	denominator = libm_two_product(u.high, point);
	denominator.low += u.low * point;
	denominator = libm_dd_add_double(denominator, 1.0);
	ratio = libm_dd_divide(numerator, denominator);

	/* atan t = t + t^3 (-1/3 + t^2/5 - ... + t^10/13). */
	square = ratio.high * ratio.high;
	coefficients = __libm_atan_coefficients;
	tail = coefficients[5];
	tail = coefficients[4] + square * tail;
	tail = coefficients[3] + square * tail;
	tail = coefficients[2] + square * tail;
	tail = coefficients[1] + square * tail;
	tail = coefficients[0] + square * tail;
	ratio = libm_dd_add_double(ratio, ratio.high * square * tail);

	/* Adds atan c. */
	result = libm_dd_add(__libm_atan_table[index], ratio);

	/* Succeeded: atan u. */
	return result;
}

/*
 * Returns the arctangent of x, in radians.
 */
double
atan(
	double x)
{
	struct libm_dd magnitude;
	struct libm_dd angle;
	uint64_t bits;

	/* A NaN, unequal to itself, is passed on. */
	if (x != x)
		return x + x;

	/* A tiny x is its own arctangent; a subnormal one has underflowed. */
	bits = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (bits < LIBM_ATRIG_TINY) {
		if (bits == 0U)
			return x;

		/* The result is x, inexact. */
		return __libm_check_underflow(x);
	}

	/* A huge x, infinity included, is pi/2 to the last bit. */
	if (bits > LIBM_ATAN_HUGE) {
		if (x < 0.0)
			return -LIBM_HALF_PI_HIGH;

		/* A positive huge x. */
		return LIBM_HALF_PI_HIGH;
	}

	/* Up to one the kernel applies; above it, pi/2 - atan(1/|x|). */
	magnitude.high = libm_from_bits(bits);
	magnitude.low = 0.0;
	if (bits <= LIBM_ONE_BITS) {
		angle = __libm_atan_dd(magnitude);
	} else {
		angle = libm_quotient(1.0, magnitude.high);
		angle = __libm_atan_dd(angle);
		angle = libm_complement(angle);
	}

	/* The arctangent is odd. */
	if (x < 0.0)
		angle = libm_negate(angle);

	/* Succeeded: the pair rounded once. */
	return angle.high + angle.low;
}

/*
 * Returns the angle of the point (x, y), in radians, in [-pi, pi].
 */
double
atan2(
	double y,
	double x)
{
	struct libm_dd angle;
	struct libm_dd ratio;
	struct libm_dd pi;
	uint64_t x_bits;
	uint64_t y_bits;
	uint64_t x_magnitude;
	uint64_t y_magnitude;
	double x_scaled;
	double y_scaled;
	double result;
	int x_exponent;
	int y_exponent;
	int scale_exponent;

	/* A NaN in either argument, unequal to itself, is passed on. */
	if (x != x || y != y)
		return x + y;

	/* Reads the classes the special cases of Annex F depend on. */
	x_bits = libm_bits(x);
	y_bits = libm_bits(y);
	x_magnitude = x_bits & LIBM_DOUBLE_MAGNITUDE;
	y_magnitude = y_bits & LIBM_DOUBLE_MAGNITUDE;

	/* On the x axis the angle is 0 or pi, with the sign of y. */
	if (y_magnitude == 0U) {
		if ((x_bits >> 63) == 0U)
			return y;
		if ((y_bits >> 63) != 0U)
			return -LIBM_PI_HIGH;

		/* A positive zero y left of the origin. */
		return LIBM_PI_HIGH;
	}

	/* On the y axis, or towards an infinite y from a finite x, the angle is pi/2. */
	if (x_magnitude == 0U ||
	    (y_magnitude == LIBM_DOUBLE_INFINITY && x_magnitude != LIBM_DOUBLE_INFINITY))
		return copysign(LIBM_HALF_PI_HIGH, y);

	/* Both infinite: the diagonals. */
	if (y_magnitude == LIBM_DOUBLE_INFINITY) {
		if ((x_bits >> 63) == 0U)
			return copysign(LIBM_QUARTER_PI, y);

		/* Towards -infinity in x. */
		return copysign(LIBM_THREE_QUARTER_PI, y);
	}

	/* An infinite x with a finite y: 0 or pi. */
	if (x_magnitude == LIBM_DOUBLE_INFINITY) {
		if ((x_bits >> 63) == 0U)
			return copysign(0.0, y);

		/* Towards -infinity in x. */
		return copysign(LIBM_PI_HIGH, y);
	}

	/*
	 * Brings both magnitudes near one by the same power of two, which
	 * keeps their ratio.  Past a ratio of 2^60 either the angle rounds to
	 * pi/2 or it is y/x itself, or pi.
	 */
	x_exponent = ilogb(x);
	y_exponent = ilogb(y);
	if (y_exponent - x_exponent > 60)
		return copysign(LIBM_HALF_PI_HIGH, y);
	if (x_exponent - y_exponent > 60) {
		if ((x_bits >> 63) != 0U)
			return copysign(LIBM_PI_HIGH, y);

		/* atan(y/x) rounds to y/x, which may underflow. */
		result = y / x;
		if (result == 0.0)
			return __libm_underflow((unsigned int)(y_bits >> 63));

		/* The quotient is the angle. */
		return __libm_check_underflow(result);
	}

	/* Scales both by the larger exponent; the results are normal and exact. */
	scale_exponent = x_exponent;
	if (y_exponent > x_exponent)
		scale_exponent = y_exponent;
	x_scaled = scalbn(libm_from_bits(x_magnitude), -scale_exponent);
	y_scaled = scalbn(libm_from_bits(y_magnitude), -scale_exponent);

	/* The angle in the first quadrant from the smaller ratio. */
	if (y_scaled <= x_scaled) {
		ratio = libm_quotient(y_scaled, x_scaled);
		angle = __libm_atan_dd(ratio);
	} else {
		ratio = libm_quotient(x_scaled, y_scaled);
		angle = __libm_atan_dd(ratio);
		angle = libm_complement(angle);
	}

	/* Left of the y axis the angle is pi minus that. */
	if ((x_bits >> 63) != 0U) {
		pi.high = LIBM_PI_HIGH;
		pi.low = LIBM_PI_LOW;
		angle = libm_dd_add(pi, libm_negate(angle));
	}

	/* Below the x axis the angle is negative. */
	if ((y_bits >> 63) != 0U)
		angle = libm_negate(angle);

	/* Succeeded: the pair rounded once. */
	return angle.high + angle.low;
}

/*
 * Returns the arcsine of x, in radians.
 */
double
asin(
	double x)
{
	struct libm_dd root;
	struct libm_dd side;
	struct libm_dd ratio;
	struct libm_dd angle;
	uint64_t bits;
	double magnitude;

	/* A NaN, unequal to itself, is passed on. */
	if (x != x)
		return x + x;

	/* Outside [-1, 1] there is no arcsine. */
	bits = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (bits > LIBM_ONE_BITS)
		return __libm_invalid();

	/* A tiny x is its own arcsine; a subnormal one has underflowed. */
	if (bits < LIBM_ATRIG_TINY) {
		if (bits == 0U)
			return x;

		/* The result is x, inexact. */
		return __libm_check_underflow(x);
	}

	/*
	 * asin x = atan(x / sqrt(1 - x^2)).  Up to 1/sqrt 2 that ratio is at
	 * most one; beyond, pi/2 - atan(sqrt(1 - x^2) / x) keeps it so.
	 */
	magnitude = libm_from_bits(bits);
	root = libm_root_of_one_minus_square(magnitude);
	side.high = magnitude;
	side.low = 0.0;
	if (root.high >= magnitude) {
		ratio = libm_dd_divide(side, root);
		angle = __libm_atan_dd(ratio);
	} else {
		ratio = libm_dd_divide(root, side);
		angle = __libm_atan_dd(ratio);
		angle = libm_complement(angle);
	}

	/* The arcsine is odd. */
	if (x < 0.0)
		angle = libm_negate(angle);

	/* Succeeded: the pair rounded once. */
	return angle.high + angle.low;
}

/*
 * Returns the arccosine of x, in radians.
 */
double
acos(
	double x)
{
	struct libm_dd root;
	struct libm_dd side;
	struct libm_dd ratio;
	struct libm_dd angle;
	struct libm_dd pi;
	uint64_t bits;
	double magnitude;

	/* A NaN, unequal to itself, is passed on. */
	if (x != x)
		return x + x;

	/* Outside [-1, 1] there is no arccosine; at 1 it is exactly zero. */
	bits = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (bits > LIBM_ONE_BITS)
		return __libm_invalid();
	if (x == 1.0)
		return 0.0;

	/*
	 * acos |x| = atan(sqrt(1 - x^2) / |x|): the ratio is at most one from
	 * 1/sqrt 2 on, and below that pi/2 - atan(|x| / sqrt(1 - x^2)).
	 */
	magnitude = libm_from_bits(bits);
	root = libm_root_of_one_minus_square(magnitude);
	side.high = magnitude;
	side.low = 0.0;
	if (root.high <= magnitude) {
		ratio = libm_dd_divide(root, side);
		angle = __libm_atan_dd(ratio);
	} else {
		ratio = libm_dd_divide(side, root);
		angle = __libm_atan_dd(ratio);
		angle = libm_complement(angle);
	}

	/* A negative x gives pi - acos |x|. */
	if (x < 0.0) {
		pi.high = LIBM_PI_HIGH;
		pi.low = LIBM_PI_LOW;
		angle = libm_dd_add(pi, libm_negate(angle));
	}

	/* Succeeded: the pair rounded once. */
	return angle.high + angle.low;
}

/*
 * Returns the arctangent of x in binary32.
 */
float
atanf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = atan((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns the angle of the point (x, y) in binary32.
 */
float
atan2f(
	float y,
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = atan2((double)y, (double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns the arcsine of x in binary32.
 */
float
asinf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = asin((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns the arccosine of x in binary32.
 */
float
acosf(
	float x)
{
	double value;

	/* Computes in double. */
	value = acos((double)x);

	/* Succeeded: an arccosine is inside the float range. */
	return (float)value;
}

/*
 * Returns pi/2 minus an angle.
 */
static struct libm_dd
libm_complement(
	struct libm_dd angle)
{
	struct libm_dd half_pi;
	struct libm_dd result;

	/* Subtracts in double-double; the angle is at most pi/4 here. */
	half_pi.high = LIBM_HALF_PI_HIGH;
	half_pi.low = LIBM_HALF_PI_LOW;
	result = libm_dd_add(half_pi, libm_negate(angle));

	/* Succeeded: the complementary angle. */
	return result;
}

/*
 * Returns the negation of a double-double.
 */
static struct libm_dd
libm_negate(
	struct libm_dd value)
{
	struct libm_dd result;

	/* Negating both parts is exact. */
	result.high = -value.high;
	result.low = -value.low;

	/* Succeeded: the negated pair. */
	return result;
}

/*
 * Returns sqrt((1 - x)(1 + x)) as a double-double for 0 <= x <= 1.
 */
static struct libm_dd
libm_root_of_one_minus_square(
	double magnitude)
{
	struct libm_dd below;
	struct libm_dd above;
	struct libm_dd product;
	struct libm_dd root;

	/* Both factors exactly, then their product in double-double. */
	below = libm_two_sum(1.0, -magnitude);
	above = libm_two_sum(1.0, magnitude);
	product = libm_dd_multiply(below, above);

	/* The root of the product. */
	root = libm_dd_sqrt(product);

	/* Succeeded: sqrt(1 - x^2) with no cancellation near x = 1. */
	return root;
}

/*
 * Returns numerator / denominator as a double-double.
 */
static struct libm_dd
libm_quotient(
	double numerator,
	double denominator)
{
	struct libm_dd top;
	struct libm_dd bottom;
	struct libm_dd result;

	/* Divides the two doubles in double-double. */
	top.high = numerator;
	top.low = 0.0;
	bottom.high = denominator;
	bottom.low = 0.0;
	result = libm_dd_divide(top, bottom);

	/* Succeeded: the quotient to about 104 bits. */
	return result;
}
