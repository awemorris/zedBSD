/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The error function and its complement.
 *
 * For |x| <= 1, erf(x) = x E(x^2) with E a minimax polynomial of degree 13
 * (relative error 2^-68).  For x >= 1/2, erfc(x) = exp(-x^2) R(x), where
 * R is approximated on fifteen intervals up to 28 by minimax polynomials
 * (relative error 2^-60) and exp(-x^2) is the exponential's kernel on the
 * exact square.  The last steps of each polynomial and every product are
 * in double-double, and each combination is chosen without cancellation:
 * erfc is 1 - erf only below 1/2, erf is 1 - erfc only above 1, and a
 * negative argument of erfc gives 2 - erfc(|x|).  The coefficients come
 * from sollya's fpminimax (src/libc/math/gen/gen-tables.py).
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/* From here on erfc is formed from its own polynomials. */
#define LIBM_ERFC_START 0.5

/* Past here erfc(x) is below half the smallest subnormal. */
#define LIBM_ERFC_ZERO 28.0

/* Past here erf(x) rounds to one and erfc(-x) to two. */
#define LIBM_ERF_ONE 6.0

/* Below this |x| erf is computed on x lifted by 2^100: 2^-900. */
#define LIBM_ERF_TINY 1.1830521861667747e-271

/* 2^100. */
#define LIBM_TWO_TO_100 1.2676506002282294e30

static struct libm_dd libm_erf_small(double x);
static struct libm_dd libm_erfc_large(double x, int *exponent);
static struct libm_dd libm_erfc_scaled(double x);

/*
 * Returns the error function of x.
 */
double
erf(
	double x)
{
	struct libm_dd value;
	double magnitude;
	double result;

	/* A NaN is passed on, and the infinities give plus or minus one. */
	if (x != x)
		return x + x;
	magnitude = fabs(x);
	if (magnitude > LIBM_ERF_ONE)
		return copysign(1.0, x);

	/* A zero is its own result. */
	if (magnitude == 0.0)
		return x;

	/*
	 * A tiny x is lifted by 2^100 so that the products stay exact, and the
	 * result is lowered again with one rounding, subnormal or not.
	 */
	if (magnitude < LIBM_ERF_TINY) {
		value = libm_erf_small(x * LIBM_TWO_TO_100);
		result = __libm_scale(value, -100);
		return result;
	}

	/* Up to one the polynomial in x^2. */
	if (magnitude <= 1.0) {
		value = libm_erf_small(x);
		return value.high + value.low;
	}

	/* Beyond one, 1 - erfc(|x|) with the sign of x. */
	value = libm_erfc_scaled(magnitude);
	value.high = -value.high;
	value.low = -value.low;
	value = libm_dd_add_double(value, 1.0);
	if (x < 0.0)
		return -value.high;

	/* Succeeded: x is positive. */
	return value.high;
}

/*
 * Returns the complementary error function of x.
 */
double
erfc(
	double x)
{
	struct libm_dd value;
	int exponent;
	double result;

	/* A NaN is passed on; +infinity gives 0, -infinity 2. */
	if (x != x)
		return x + x;
	if (x > LIBM_ERFC_ZERO) {
		if (x == HUGE_VAL)
			return 0.0;

		/* A finite x this large underflows. */
		return __libm_underflow(0U);
	}

	/* Far left erfc rounds to two. */
	if (x < -LIBM_ERF_ONE)
		return 2.0;

	/* From one half on, exp(-x^2) R(x), rounded once. */
	if (x >= LIBM_ERFC_START) {
		value = libm_erfc_large(x, &exponent);
		result = __libm_scale(value, exponent);
		return result;
	}

	/* Around zero, 1 - erf(x), which is at least 0.47. */
	if (x > -LIBM_ERFC_START) {
		value = libm_erf_small(x);
		value.high = -value.high;
		value.low = -value.low;
		value = libm_dd_add_double(value, 1.0);
		return value.high;
	}

	/* Left of -1/2, 2 - erfc(|x|), which is at least 1.52. */
	value = libm_erfc_scaled(-x);
	value.high = -value.high;
	value.low = -value.low;
	value = libm_dd_add_double(value, 2.0);

	/* Succeeded: the renormalized high part is the rounded result. */
	return value.high;
}

/*
 * Returns the error function of x in binary32.
 */
float
erff(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = erf((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns the complementary error function of x in binary32.
 */
float
erfcf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = erfc((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns erf(x) as a double-double for |x| <= 1.
 *
 * z = x^2 is exact as a pair; the polynomial's high terms are summed in
 * double on its high part, and its last three steps in double-double.
 */
static struct libm_dd
libm_erf_small(
	double x)
{
	struct libm_dd square;
	struct libm_dd value;
	struct libm_dd coefficient;
	const double *coefficients;
	double tail;
	int index;

	/* x^2 exactly. */
	square = libm_two_product(x, x);

	/* The terms from z^3 up, by Horner's rule in double. */
	coefficients = __libm_erf_coefficients;
	tail = coefficients[15];
	for (index = 14; index >= 5; index--)
		tail = coefficients[index] + square.high * tail;

	/* z (tail) + c2, then z (...) + c1 and z (...) + c0, in double-double. */
	value.high = tail;
	value.low = 0.0;
	value = libm_dd_multiply(value, square);
	value = libm_dd_add_double(value, coefficients[4]);
	value = libm_dd_multiply(value, square);
	coefficient.high = coefficients[2];
	coefficient.low = coefficients[3];
	value = libm_dd_add(value, coefficient);
	value = libm_dd_multiply(value, square);
	coefficient.high = coefficients[0];
	coefficient.low = coefficients[1];
	value = libm_dd_add(value, coefficient);

	/* erf(x) = x E(x^2). */
	value = libm_dd_multiply_double(value, x);

	/* Succeeded: erf(x). */
	return value;
}

/*
 * Returns erfc(x) for 1/2 <= x <= 28 as a pair and a power of two: the
 * value is the pair times 2^exponent.
 */
static struct libm_dd
libm_erfc_large(
	double x,
	int *exponent)
{
	const struct libm_polynomial_interval *interval;
	const double *coefficients;
	struct libm_dd square;
	struct libm_dd value;
	struct libm_dd coefficient;
	struct libm_dd decay;
	double offset;
	double tail;
	int index;

	/* Finds the interval and the exact reduced variable t. */
	interval = __libm_erfc_intervals;
	while (x > interval->upper)
		interval++;
	offset = (x - interval->centre) * interval->inverse_half_width;

	/* The terms from t^2 up, by Horner's rule in double. */
	coefficients = __libm_erfc_coefficients + interval->first;
	tail = coefficients[interval->degree + 2];
	for (index = interval->degree - 1; index >= 2; index--)
		tail = coefficients[index + 2] + offset * tail;

	/* t (tail) + c1, then t (...) + c0, in double-double. */
	value = libm_two_product(tail, offset);
	coefficient.high = coefficients[2];
	coefficient.low = coefficients[3];
	value = libm_dd_add(value, coefficient);
	value = libm_dd_multiply_double(value, offset);
	coefficient.high = coefficients[0];
	coefficient.low = coefficients[1];
	value = libm_dd_add(value, coefficient);

	/* exp(-x^2) on the exact square, as a pair and a power of two. */
	square = libm_two_product(x, x);
	square.high = -square.high;
	square.low = -square.low;
	decay = __libm_exp_dd(square, exponent);
	value = libm_dd_multiply(decay, value);

	/* Succeeded: erfc(x) = exp(-x^2) R(x). */
	return value;
}

/*
 * Returns erfc(x) as a plain double-double for 1/2 <= x <= 6, where its
 * power of two is small enough to apply exactly.
 */
static struct libm_dd
libm_erfc_scaled(
	double x)
{
	struct libm_dd value;
	double power;
	int exponent;

	/* The pair and its power of two, which is at least 2^-60 here. */
	value = libm_erfc_large(x, &exponent);
	power = libm_from_bits((uint64_t)(exponent + LIBM_DOUBLE_BIAS) << 52);
	value.high *= power;
	value.low *= power;

	/* Succeeded: erfc(x). */
	return value;
}
