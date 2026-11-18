/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Rounding to integral values and conversion to integers.
 *
 * Every function works on the encoding: the fraction bits below the unit
 * are cleared, and a carry out of the kept bits moves into the exponent by
 * itself, so each result is exact.  Only round-to-nearest exists in the
 * floating-point environment, so rint and nearbyint round ties to even.
 */

#include <errno.h>
#include <fenv.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

static double libm_round_integral(double x, int ties_to_even, int report_inexact);
static int libm_fits(double integral, double lower, double upper);

/*
 * Rounds a binary64 value towards zero to an integral value.
 */
double
trunc(
	double x)
{
	uint64_t bits;
	int exponent;
	uint64_t mask;

	/* Reads the unbiased exponent of the value. */
	bits = libm_bits(x);
	exponent = (int)((bits >> 52) & 0x7ffU) - LIBM_DOUBLE_BIAS;

	/* A magnitude below one truncates to a zero of the same sign. */
	if (exponent < 0)
		return libm_from_bits(bits & LIBM_DOUBLE_SIGN);

	/* From 2^52 on every value is integral, and so are infinity and NaN. */
	if (exponent >= 52) {
		/* A NaN is quieted by the addition; the rest is already integral. */
		if (exponent == 0x400 && (bits & LIBM_DOUBLE_FRACTION) != 0U)
			return x + x;

		/* An infinity or a large finite value is its own result. */
		return x;
	}

	/* Clears the fraction bits below the unit. */
	mask = LIBM_DOUBLE_FRACTION >> exponent;

	/* Succeeded: the kept bits are the integral part. */
	return libm_from_bits(bits & ~mask);
}

/*
 * Rounds a binary64 value downwards to an integral value.
 */
double
floor(
	double x)
{
	double whole;

	/* Truncation already rounds a positive value or an integral one down. */
	whole = trunc(x);
	if (whole == x || x > 0.0)
		return whole;

	/* A negative value with a fraction goes one further down, exactly. */
	return whole - 1.0;
}

/*
 * Rounds a binary64 value upwards to an integral value.
 */
double
ceil(
	double x)
{
	double whole;

	/* Truncation already rounds a negative value or an integral one up. */
	whole = trunc(x);
	if (whole == x || x < 0.0)
		return whole;

	/* A positive value with a fraction goes one further up, exactly. */
	return whole + 1.0;
}

/*
 * Rounds a binary64 value to the nearest integral value, ties away from zero.
 */
double
round(
	double x)
{
	double rounded;

	/* Rounds without touching the exception flags. */
	rounded = libm_round_integral(x, 0, 0);

	/* Succeeded: the nearest integral value is the result. */
	return rounded;
}

/*
 * Rounds a binary64 value to the nearest integral value, ties to even,
 * raising the inexact exception when the value changes.
 */
double
rint(
	double x)
{
	double rounded;

	/* Rounds in the current mode, which is always to nearest. */
	rounded = libm_round_integral(x, 1, 1);

	/* Succeeded: the nearest integral value is the result. */
	return rounded;
}

/*
 * Rounds a binary64 value to the nearest integral value, ties to even,
 * without raising the inexact exception.
 */
double
nearbyint(
	double x)
{
	double rounded;

	/* Rounds in the current mode and leaves the flags alone. */
	rounded = libm_round_integral(x, 1, 0);

	/* Succeeded: the nearest integral value is the result. */
	return rounded;
}

/*
 * Rounds a binary64 value to the nearest long, ties to even.
 */
long
lrint(
	double x)
{
	double rounded;
	int fits;

	/* Rounds to an integral double first. */
	rounded = rint(x);

	/*
	 * The bounds are powers of two and exact in double: LONG_MIN itself
	 * and its negation, which is one past LONG_MAX.
	 */
	fits = libm_fits(rounded, (double)LONG_MIN, -(double)LONG_MIN);
	if (!fits) {
		(void)__libm_invalid();
		return LONG_MIN;
	}

	/* Succeeded: the integral value converts exactly. */
	return (long)rounded;
}

/*
 * Rounds a binary64 value to the nearest long long, ties to even.
 */
long long
llrint(
	double x)
{
	double rounded;
	int fits;

	/* Rounds to an integral double first. */
	rounded = rint(x);

	/* The bounds are LLONG_MIN and its negation, both exact in double. */
	fits = libm_fits(rounded, (double)LLONG_MIN, -(double)LLONG_MIN);
	if (!fits) {
		(void)__libm_invalid();
		return LLONG_MIN;
	}

	/* Succeeded: the integral value converts exactly. */
	return (long long)rounded;
}

/*
 * Rounds a binary64 value to the nearest long, ties away from zero.
 */
long
lround(
	double x)
{
	double rounded;
	int fits;

	/* Rounds to an integral double first. */
	rounded = round(x);

	/* The bounds are LONG_MIN and its negation, both exact in double. */
	fits = libm_fits(rounded, (double)LONG_MIN, -(double)LONG_MIN);
	if (!fits) {
		(void)__libm_invalid();
		return LONG_MIN;
	}

	/* Succeeded: the integral value converts exactly. */
	return (long)rounded;
}

/*
 * Rounds a binary64 value to the nearest long long, ties away from zero.
 */
long long
llround(
	double x)
{
	double rounded;
	int fits;

	/* Rounds to an integral double first. */
	rounded = round(x);

	/* The bounds are LLONG_MIN and its negation, both exact in double. */
	fits = libm_fits(rounded, (double)LLONG_MIN, -(double)LLONG_MIN);
	if (!fits) {
		(void)__libm_invalid();
		return LLONG_MIN;
	}

	/* Succeeded: the integral value converts exactly. */
	return (long long)rounded;
}

/*
 * Rounds a binary64 value to the nearest integral value.
 *
 * Ties go to the even neighbour or away from zero.  The inexact exception
 * is raised on request when the result differs from the argument.
 */
static double
libm_round_integral(
	double x,
	int ties_to_even,
	int report_inexact)
{
	uint64_t bits;
	int exponent;
	uint64_t unit;
	uint64_t half;
	uint64_t fraction;
	uint64_t sign;

	/* Reads the unbiased exponent of the value. */
	bits = libm_bits(x);
	sign = bits & LIBM_DOUBLE_SIGN;
	exponent = (int)((bits >> 52) & 0x7ffU) - LIBM_DOUBLE_BIAS;

	/* From 2^52 on every value is integral, and so are infinity and NaN. */
	if (exponent >= 52) {
		/* A NaN is quieted by the addition; the rest is already integral. */
		if (exponent == 0x400 && (bits & LIBM_DOUBLE_FRACTION) != 0U)
			return x + x;

		/* An infinity or a large finite value is its own result. */
		return x;
	}

	/* A zero stays itself. */
	if ((bits & LIBM_DOUBLE_MAGNITUDE) == 0U)
		return x;

	/* A magnitude below one rounds to zero or to one of its sign. */
	if (exponent < 0) {
		if (report_inexact)
			(void)feraiseexcept(FE_INEXACT);

		/* Only a magnitude of one half or more can reach one. */
		if (exponent < -1)
			return libm_from_bits(sign);

		/* One half is a tie: even sends it to zero, away sends it to one. */
		if ((bits & LIBM_DOUBLE_FRACTION) == 0U && ties_to_even)
			return libm_from_bits(sign);

		/* Everything else in [0.5, 1) rounds to one. */
		return libm_from_bits(sign | UINT64_C(0x3ff0000000000000));
	}

	/* Names the unit bit, the half below it, and the fraction under it. */
	unit = LIBM_DOUBLE_HIDDEN >> exponent;
	half = unit >> 1;
	fraction = bits & (unit - 1U);

	/* An integral value is returned as it is. */
	if (fraction == 0U)
		return x;

	/* Clears the fraction and decides whether to step one unit away. */
	bits &= ~(unit - 1U);
	if (fraction > half) {
		bits += unit;
	} else if (fraction == half) {
		/* A tie steps away unless ties go to an even value that is even. */
		if (!ties_to_even || (bits & unit) != 0U)
			bits += unit;
	}

	/* The changed value is inexact when the caller wants that known. */
	if (report_inexact)
		(void)feraiseexcept(FE_INEXACT);

	/* Succeeded: the carry, if any, has moved into the exponent. */
	return libm_from_bits(bits);
}

/*
 * Reports whether an integral double lies in [lower, upper).
 *
 * A NaN fails both comparisons and so never fits.
 */
static int
libm_fits(
	double integral,
	double lower,
	double upper)
{
	/* Below the smallest integer of the type. */
	if (!(integral >= lower))
		return 0;

	/* At or past one more than the largest integer of the type. */
	if (!(integral < upper))
		return 0;

	/* Succeeded: the value converts exactly. */
	return 1;
}
