/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hyperbolic functions and their inverses.
 *
 * sinh, cosh and tanh are formed from E = exp(|x|) - 1 as a double-double
 * in shapes without cancellation:
 *   sinh = (E + E/(E + 1)) / 2,  cosh = 1 + E^2 / (2 (E + 1)),
 *   tanh = E / (E + 2) with E = exp(2|x|) - 1,
 * and far from zero sinh and cosh are exp(|x|)/2, scaled so that the
 * range just below the overflow of exp does not overflow.  asinh, acosh
 * and atanh take the logarithm of a double-double:
 *   asinh = log(|x| + sqrt(x^2 + 1)),  acosh = log(x + sqrt(x^2 - 1)),
 *   atanh = log((1 + |x|) / (1 - |x|)) / 2,
 * where x^2 - 1 is exact from the exact square and log(h + l) is
 * log h + l/h.  Each result is rounded once.
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/* The encoding of 2^-27: below it sinh, tanh, asinh and atanh round to x. */
#define LIBM_HYPERBOLIC_TINY UINT64_C(0x3e40000000000000)

/* Past this |x| the smaller exponential no longer matters to sinh and cosh. */
#define LIBM_HYPERBOLIC_LARGE 40.0

/* Past this |x| sinh and cosh overflow. */
#define LIBM_HYPERBOLIC_OVERFLOW 711.0

/* Past this |x| tanh rounds to one. */
#define LIBM_TANH_ONE 22.0

/* Past this |x| asinh and acosh are log 2|x| to full precision: 2^28. */
#define LIBM_INVERSE_LARGE 268435456.0

/* The largest |x| for which the expm1 kernel is used directly. */
#define LIBM_EXPM1_DIRECT 0.0027

static struct libm_dd libm_expm1_dd(double magnitude);
static struct libm_dd libm_log_of_dd(struct libm_dd value);
static struct libm_dd libm_log_twice(double magnitude);
static double libm_signed_result(struct libm_dd value, double x);

/*
 * Returns the hyperbolic sine of x.
 */
double
sinh(
	double x)
{
	struct libm_dd growth;
	struct libm_dd denominator;
	struct libm_dd ratio;
	struct libm_dd value;
	struct libm_dd argument;
	uint64_t bits;
	double magnitude;
	double result;
	int exponent;
	double rounded;

	/* A NaN is passed on, and an infinity is its own result. */
	bits = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (bits >= LIBM_DOUBLE_INFINITY)
		return x + x;

	/* A tiny x is its own result; a subnormal one has underflowed. */
	if (bits < LIBM_HYPERBOLIC_TINY) {
		if (bits == 0U)
			return x;

		/* The result is x, inexact. */
		return __libm_check_underflow(x);
	}

	/* Far out sinh overflows. */
	magnitude = libm_from_bits(bits);
	if (magnitude > LIBM_HYPERBOLIC_OVERFLOW)
		return __libm_overflow((unsigned int)(libm_bits(x) >> 63));

	/* Far from zero, sinh |x| = exp(|x|) / 2, scaled once. */
	if (magnitude > LIBM_HYPERBOLIC_LARGE) {
		argument.high = magnitude;
		argument.low = 0.0;
		value = __libm_exp_dd(argument, &exponent);
		if (x < 0.0) {
			value.high = -value.high;
			value.low = -value.low;
		}

		/* Halves while scaling, which also keeps the top range finite. */
		result = __libm_scale(value, exponent - 1);
		return result;
	}

	/* Nearer, (E + E/(E + 1)) / 2 has only positive terms. */
	growth = libm_expm1_dd(magnitude);
	denominator = libm_dd_add_double(growth, 1.0);
	ratio = libm_dd_divide(growth, denominator);
	value = libm_dd_add(growth, ratio);
	value.high *= 0.5;
	value.low *= 0.5;

	/* Rounds the pair and gives it the sign of x. */
	rounded = libm_signed_result(value, x);

	/* Succeeded: the signed result. */
	return rounded;
}

/*
 * Returns the hyperbolic cosine of x.
 */
double
cosh(
	double x)
{
	struct libm_dd growth;
	struct libm_dd square;
	struct libm_dd denominator;
	struct libm_dd value;
	struct libm_dd argument;
	uint64_t bits;
	double magnitude;
	double result;
	int exponent;

	/* A NaN is passed on, and an infinity gives +infinity. */
	bits = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (bits >= LIBM_DOUBLE_INFINITY)
		return x * x;

	/* A tiny x rounds to one. */
	if (bits < LIBM_HYPERBOLIC_TINY)
		return 1.0;

	/* Far out cosh overflows. */
	magnitude = libm_from_bits(bits);
	if (magnitude > LIBM_HYPERBOLIC_OVERFLOW)
		return __libm_overflow(0U);

	/* Far from zero, cosh |x| = exp(|x|) / 2, scaled once. */
	if (magnitude > LIBM_HYPERBOLIC_LARGE) {
		argument.high = magnitude;
		argument.low = 0.0;
		value = __libm_exp_dd(argument, &exponent);
		result = __libm_scale(value, exponent - 1);
		return result;
	}

	/* Nearer, 1 + E^2 / (2 (E + 1)). */
	growth = libm_expm1_dd(magnitude);
	square = libm_dd_multiply(growth, growth);
	denominator = libm_dd_add_double(growth, 1.0);
	denominator.high *= 2.0;
	denominator.low *= 2.0;
	value = libm_dd_divide(square, denominator);
	value = libm_dd_add_double(value, 1.0);

	/* Succeeded: the renormalized high part is the rounded result. */
	return value.high;
}

/*
 * Returns the hyperbolic tangent of x.
 */
double
tanh(
	double x)
{
	struct libm_dd growth;
	struct libm_dd denominator;
	struct libm_dd value;
	uint64_t bits;
	double magnitude;
	double rounded;

	/* A NaN is passed on. */
	if (x != x)
		return x + x;

	/* A tiny x is its own result; a subnormal one has underflowed. */
	bits = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (bits < LIBM_HYPERBOLIC_TINY) {
		if (bits == 0U)
			return x;

		/* The result is x, inexact. */
		return __libm_check_underflow(x);
	}

	/* Far out, infinity included, tanh rounds to one of its sign. */
	magnitude = libm_from_bits(bits);
	if (magnitude > LIBM_TANH_ONE)
		return copysign(1.0, x);

	/* E / (E + 2) with E = exp(2|x|) - 1. */
	growth = libm_expm1_dd(2.0 * magnitude);
	denominator = libm_dd_add_double(growth, 2.0);
	value = libm_dd_divide(growth, denominator);

	/* Rounds the pair and gives it the sign of x. */
	rounded = libm_signed_result(value, x);

	/* Succeeded: the signed result. */
	return rounded;
}

/*
 * Returns the inverse hyperbolic sine of x.
 */
double
asinh(
	double x)
{
	struct libm_dd square;
	struct libm_dd root;
	struct libm_dd sum;
	struct libm_dd value;
	uint64_t bits;
	double magnitude;
	double rounded;

	/* A NaN is passed on, and an infinity is its own result. */
	bits = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (bits >= LIBM_DOUBLE_INFINITY)
		return x + x;

	/* A tiny x is its own result; a subnormal one has underflowed. */
	if (bits < LIBM_HYPERBOLIC_TINY) {
		if (bits == 0U)
			return x;

		/* The result is x, inexact. */
		return __libm_check_underflow(x);
	}

	/* Far out, asinh |x| = log |x| + ln 2. */
	magnitude = libm_from_bits(bits);
	if (magnitude > LIBM_INVERSE_LARGE) {
		value = libm_log_twice(magnitude);
		return libm_signed_result(value, x);
	}

	/* log(|x| + sqrt(x^2 + 1)) in double-double. */
	square = libm_two_product(magnitude, magnitude);
	square = libm_dd_add_double(square, 1.0);
	root = libm_dd_sqrt(square);
	sum = libm_dd_add_double(root, magnitude);
	value = libm_log_of_dd(sum);

	/* Rounds the pair and gives it the sign of x. */
	rounded = libm_signed_result(value, x);

	/* Succeeded: the signed result. */
	return rounded;
}

/*
 * Returns the inverse hyperbolic cosine of x.
 */
double
acosh(
	double x)
{
	struct libm_dd square;
	struct libm_dd root;
	struct libm_dd sum;
	struct libm_dd value;

	/* A NaN is passed on; below one there is no result, at one it is zero. */
	if (x != x)
		return x + x;
	if (x < 1.0)
		return __libm_invalid();
	if (x == 1.0)
		return 0.0;

	/* +infinity is its own result. */
	if (x == HUGE_VAL)
		return x;

	/* Far out, acosh x = log x + ln 2. */
	if (x > LIBM_INVERSE_LARGE) {
		value = libm_log_twice(x);
		return value.high + value.low;
	}

	/*
	 * log(x + sqrt(x^2 - 1)).  The square is exact as a pair, and near
	 * x = 1 taking one from its high part is exact as well.
	 */
	square = libm_two_product(x, x);
	square = libm_dd_add_double(square, -1.0);
	root = libm_dd_sqrt(square);
	sum = libm_dd_add_double(root, x);
	value = libm_log_of_dd(sum);

	/* Succeeded: the pair rounded once. */
	return value.high + value.low;
}

/*
 * Returns the inverse hyperbolic tangent of x.
 */
double
atanh(
	double x)
{
	struct libm_dd numerator;
	struct libm_dd denominator;
	struct libm_dd ratio;
	struct libm_dd value;
	uint64_t bits;
	double magnitude;
	double rounded;

	/* A NaN is passed on. */
	if (x != x)
		return x + x;

	/* Past one there is no result, and at one it is a pole. */
	bits = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	magnitude = libm_from_bits(bits);
	if (magnitude > 1.0)
		return __libm_invalid();
	if (magnitude == 1.0)
		return __libm_pole((unsigned int)(libm_bits(x) >> 63));

	/* A tiny x is its own result; a subnormal one has underflowed. */
	if (bits < LIBM_HYPERBOLIC_TINY) {
		if (bits == 0U)
			return x;

		/* The result is x, inexact. */
		return __libm_check_underflow(x);
	}

	/* log((1 + |x|) / (1 - |x|)) / 2, both factors exact as pairs. */
	numerator = libm_two_sum(1.0, magnitude);
	denominator = libm_two_sum(1.0, -magnitude);
	ratio = libm_dd_divide(numerator, denominator);
	value = libm_log_of_dd(ratio);
	value.high *= 0.5;
	value.low *= 0.5;

	/* Rounds the pair and gives it the sign of x. */
	rounded = libm_signed_result(value, x);

	/* Succeeded: the signed result. */
	return rounded;
}

/*
 * Returns the hyperbolic sine of x in binary32.
 */
float
sinhf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = sinh((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns the hyperbolic cosine of x in binary32.
 */
float
coshf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = cosh((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns the hyperbolic tangent of x in binary32.
 */
float
tanhf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = tanh((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns the inverse hyperbolic sine of x in binary32.
 */
float
asinhf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = asinh((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns the inverse hyperbolic cosine of x in binary32.
 */
float
acoshf(
	float x)
{
	double value;

	/* Computes in double. */
	value = acosh((double)x);

	/* Succeeded: an inverse cosine of a float is inside the float range. */
	return (float)value;
}

/*
 * Returns the inverse hyperbolic tangent of x in binary32.
 */
float
atanhf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = atanh((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns exp(x) - 1 as a double-double for 0 < x <= 2 * LIBM_TANH_ONE
 * or LIBM_HYPERBOLIC_LARGE, whichever is larger.
 */
static struct libm_dd
libm_expm1_dd(
	double magnitude)
{
	struct libm_dd argument;
	struct libm_dd value;
	double power;
	int exponent;

	/* Near zero the kernel gives exp(x) - 1 with its relative precision. */
	argument.high = magnitude;
	argument.low = 0.0;
	if (magnitude < LIBM_EXPM1_DIRECT) {
		value = __libm_expm1_kernel(argument);
		return value;
	}

	/* Elsewhere exp(x) is scaled exactly, well inside the range, less one. */
	value = __libm_exp_dd(argument, &exponent);
	power = libm_from_bits((uint64_t)(exponent + LIBM_DOUBLE_BIAS) << 52);
	value.high *= power;
	value.low *= power;
	value = libm_dd_add_double(value, -1.0);

	/* Succeeded: exp(x) - 1 as a pair. */
	return value;
}

/*
 * Returns log(high + low) for a positive double-double.
 *
 * log(h + l) = log h + log(1 + l/h), and l/h is below 2^-52, so the
 * logarithm of the correction is the correction to within 2^-105.
 */
static struct libm_dd
libm_log_of_dd(
	struct libm_dd value)
{
	struct libm_dd result;

	/* The logarithm of the high part, then the correction. */
	result = __libm_log_dd(value.high);
	result = libm_dd_add_double(result, value.low / value.high);

	/* Succeeded: the logarithm of the pair. */
	return result;
}

/*
 * Returns log(2 |x|) = log |x| + ln 2 as a double-double.
 */
static struct libm_dd
libm_log_twice(
	double magnitude)
{
	struct libm_dd ln2;
	struct libm_dd result;

	/* Adds ln 2 in double-double. */
	ln2.high = LIBM_LN2_HIGH;
	ln2.low = LIBM_LN2_LOW;
	result = __libm_log_dd(magnitude);
	result = libm_dd_add(result, ln2);

	/* Succeeded: log 2|x|. */
	return result;
}

/*
 * Rounds a pair computed for |x| and gives it the sign of x.
 */
static double
libm_signed_result(
	struct libm_dd value,
	double x)
{
	double rounded;

	/* Rounding is symmetric, so the magnitude is rounded first. */
	rounded = value.high + value.low;
	if (x < 0.0)
		return -rounded;

	/* Succeeded: x is positive. */
	return rounded;
}
