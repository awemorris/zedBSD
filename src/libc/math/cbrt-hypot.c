/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cube root and the hypotenuse.
 *
 * cbrt reduces x to v 2^(3q) with v in [1, 8), starts from the cube root of
 * the middle of v's unit interval, takes four Newton steps in double and
 * one more against the exact residual v - y^3 formed in double-double, so
 * a cube is found exactly and anything else is rounded once.  hypot scales
 * both arguments by the same power of two, forms x^2 + y^2 exactly as a
 * double-double, takes its double-double root and scales back rounding
 * once.
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/* 2^54, which lifts a subnormal into the normal range exactly. */
#define LIBM_TWO_TO_54 18014398509481984.0

/*
 * The cube roots of 1.5, 2.5, ..., 7.5: the starting points of the Newton
 * steps for v in [k, k + 1).  Four quadratic steps from within 11% reach
 * double precision.
 */
static const double libm_cbrt_start[7] = {
	1.1447142425533319,
	1.3572088082974532,
	1.5182944859378313,
	1.6509636244473134,
	1.7651741676630317,
	1.8662555867686201,
	1.9574338205844317
};

/*
 * Returns the real cube root of x.
 */
double
cbrt(
	double x)
{
	struct libm_dd square;
	struct libm_dd cube;
	struct libm_dd residual;
	uint64_t bits;
	uint64_t magnitude;
	double value;
	double root;
	double correction;
	double result;
	int exponent;
	int remainder;
	int thirds;
	int step;
	int shift;

	/* Zero, infinity and NaN are their own cube roots. */
	bits = libm_bits(x);
	magnitude = bits & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude == 0U || magnitude >= LIBM_DOUBLE_INFINITY)
		return x + x;

	/* A subnormal is lifted by 2^54, whose cube root is 2^18. */
	shift = 0;
	if (magnitude < LIBM_DOUBLE_HIDDEN) {
		magnitude = libm_bits(libm_from_bits(magnitude) * LIBM_TWO_TO_54);
		shift = -18;
	}

	/*
	 * |x| = m 2^e with m in [1, 2); e = 3q + r with r in {0, 1, 2}, and
	 * v = m 2^r in [1, 8).
	 */
	exponent = (int)(magnitude >> 52) - LIBM_DOUBLE_BIAS;
	remainder = exponent % 3;
	if (remainder < 0)
		remainder += 3;
	thirds = (exponent - remainder) / 3;
	value = libm_from_bits((magnitude & LIBM_DOUBLE_FRACTION) |
	    ((uint64_t)(remainder + LIBM_DOUBLE_BIAS) << 52));

	/* Newton's steps y <- y - (y^3 - v) / (3 y^2) from the table's start. */
	root = libm_cbrt_start[(int)value - 1];
	for (step = 0; step < 4; step++)
		root = root - (root * root * root - value) / (3.0 * root * root);

	/*
	 * One more step against the residual v - y^3, which the exact square
	 * and a double-double product make accurate to about 2^-104.
	 */
	square = libm_two_product(root, root);
	cube = libm_dd_multiply_double(square, root);
	residual = libm_dd_add_double(cube, -value);
	correction = -(residual.high + residual.low) / (3.0 * square.high);
	result = root + correction;

	/* Puts back the power of two and the sign; both are exact. */
	result = scalbn(result, thirds + shift);
	if ((bits >> 63) != 0U)
		return -result;

	/* Succeeded: x is positive. */
	return result;
}

/*
 * Returns sqrt(x^2 + y^2) without undue overflow or underflow.
 */
double
hypot(
	double x,
	double y)
{
	struct libm_dd square;
	struct libm_dd other;
	struct libm_dd root;
	uint64_t x_magnitude;
	uint64_t y_magnitude;
	uint64_t swap;
	double larger;
	double smaller;
	double result;
	int exponent;
	int smaller_exponent;

	/* An infinity wins even over a NaN; otherwise a NaN is passed on. */
	x_magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	y_magnitude = libm_bits(y) & LIBM_DOUBLE_MAGNITUDE;
	if (x_magnitude == LIBM_DOUBLE_INFINITY || y_magnitude == LIBM_DOUBLE_INFINITY)
		return HUGE_VAL;
	if (x != x || y != y)
		return x + y;

	/* Orders the magnitudes, whose encodings order like their values. */
	if (x_magnitude < y_magnitude) {
		swap = x_magnitude;
		x_magnitude = y_magnitude;
		y_magnitude = swap;
	}

	/* The two legs as values, the larger first. */
	larger = libm_from_bits(x_magnitude);
	smaller = libm_from_bits(y_magnitude);

	/* A zero leg leaves the other as the hypotenuse. */
	if (y_magnitude == 0U)
		return larger;

	/*
	 * Past a ratio of 2^60 the smaller leg changes the result by less than
	 * 2^-121 of it, which rounds away.
	 */
	exponent = ilogb(larger);
	smaller_exponent = ilogb(smaller);
	if (exponent - smaller_exponent > 60)
		return larger + smaller;

	/* Scales both legs near one, exactly. */
	larger = scalbn(larger, -exponent);
	smaller = scalbn(smaller, -exponent);

	/* x^2 + y^2 exactly as double-doubles, summed and square-rooted. */
	square = libm_two_product(larger, larger);
	other = libm_two_product(smaller, smaller);
	square = libm_dd_add(square, other);
	root = libm_dd_sqrt(square);

	/* Scales back and rounds once, reporting overflow or underflow. */
	result = __libm_scale(root, exponent);

	/* Succeeded: the hypotenuse. */
	return result;
}

/*
 * Returns the real cube root of x in binary32.
 */
float
cbrtf(
	float x)
{
	double value;

	/* Computes in double. */
	value = cbrt((double)x);

	/* Succeeded: a cube root of a float is inside the float range. */
	return (float)value;
}

/*
 * Returns sqrt(x^2 + y^2) in binary32.
 */
float
hypotf(
	float x,
	float y)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = hypot((double)x, (double)y);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}
