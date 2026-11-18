/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The logarithms: log, log2, log10 and log1p.
 *
 * The method is Tang's table-driven one.  x is written as 2^e m with m in
 * [0.709, 1.418), and m falls in one of 128 intervals with a table value
 * c close to its reciprocal.  Then log x = e ln2 - log c + log(1 + r) with
 * r = m c - 1, which the exact product makes a double-double, and
 * |r| <= 2^-8.  The interval around 1 has c = 1, so an x near 1 gives a
 * small r with nothing to cancel against.  log(1 + r) is its Taylor series
 * to r^11, the first three terms in double-double; the whole is within
 * about 2^-78 of the true value, which pow depends on.
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/*
 * The encoding where the reduced significands begin, about 0.709.
 *
 * It is 74.5 intervals of 2^45 encodings below the encoding of 1.0, so
 * interval 74 is centred on 1.
 */
#define LIBM_LOG_OFFSET UINT64_C(0x3fe6b00000000000)

/* 2^54, which lifts a subnormal into the normal range exactly. */
#define LIBM_TWO_TO_54 18014398509481984.0

/*
 * The magnitude below which log1p(x) is x - x^2/2 + x^3/3 to full
 * precision: 2^-27.
 */
#define LIBM_LOG1P_SERIES UINT64_C(0x3e40000000000000)

/* The magnitude below which log1p(x) rounds to x: 2^-54. */
#define LIBM_LOG1P_TINY UINT64_C(0x3c90000000000000)

static struct libm_dd libm_log1p_series(struct libm_dd reduced);
static double libm_log_special(double x);

/*
 * Returns log(m) for x = 2^exponent m, with m in [0.709, 1.418).
 *
 * x must be positive and finite.
 */
struct libm_dd
__libm_log_parts(
	double x,
	int *exponent)
{
	struct libm_dd product;
	struct libm_dd reduced;
	struct libm_dd series;
	struct libm_dd result;
	uint64_t bits;
	uint64_t significand_bits;
	int shift;
	int binade;
	int index;

	/* A subnormal is lifted into the normal range first. */
	bits = libm_bits(x);
	shift = 0;
	if (bits < LIBM_DOUBLE_HIDDEN) {
		bits = libm_bits(x * LIBM_TWO_TO_54);
		shift = -54;
	}

	/*
	 * The significand takes the biased exponent 0x3fe when its fraction
	 * is at or above that of the offset, and 0x3ff otherwise, which keeps
	 * its encoding in [offset, offset + 2^52).
	 */
	binade = (int)(bits >> 52) - 0x3fe;
	if ((bits & LIBM_DOUBLE_FRACTION) < (LIBM_LOG_OFFSET & LIBM_DOUBLE_FRACTION))
		binade--;
	significand_bits = bits - ((uint64_t)binade << 52);
	*exponent = binade + shift;

	/* The interval is the next seven bits of the encoding past the offset. */
	index = (int)((significand_bits - LIBM_LOG_OFFSET) >> 45);

	/*
	 * r = m c - 1.  The product is exact as a double-double, and its high
	 * part is within 2^-8 of one, so taking one away is exact too.
	 */
	product = libm_two_product(libm_from_bits(significand_bits),
	    __libm_log_inverse[index]);
	reduced = libm_fast_two_sum(product.high - 1.0, product.low);

	/* log m = -log c + log(1 + r). */
	series = libm_log1p_series(reduced);
	result = libm_dd_add(__libm_log_table[index], series);

	/* Succeeded: the logarithm of the reduced significand. */
	return result;
}

/*
 * Returns log(x) as a double-double for a positive finite x.
 */
struct libm_dd
__libm_log_dd(
	double x)
{
	struct libm_dd reduced;
	struct libm_dd power;
	struct libm_dd result;
	int exponent;

	/* The logarithm of the significand. */
	reduced = __libm_log_parts(x, &exponent);

	/*
	 * e ln2 in two pieces: ln2 to 42 bits, whose product with e is exact,
	 * and the next 53 bits.
	 */
	power = libm_fast_two_sum((double)exponent * LIBM_LN2_SPLIT_HIGH,
	    (double)exponent * LIBM_LN2_SPLIT_LOW);
	result = libm_dd_add(power, reduced);

	/* Succeeded: log x = e ln2 + log m. */
	return result;
}

/*
 * Returns the natural logarithm of x.
 */
double
log(
	double x)
{
	struct libm_dd value;

	/* Zero, negative values, infinity and NaN have fixed answers. */
	if (!(x > 0.0) || x == HUGE_VAL)
		return libm_log_special(x);

	/* The double-double logarithm, rounded once. */
	value = __libm_log_dd(x);

	/* Succeeded: log 1 is exactly +0. */
	return value.high + value.low;
}

/*
 * Returns the base-2 logarithm of x.
 */
double
log2(
	double x)
{
	struct libm_dd value;
	struct libm_dd inverse;
	int exponent;

	/* Zero, negative values, infinity and NaN have fixed answers. */
	if (!(x > 0.0) || x == HUGE_VAL)
		return libm_log_special(x);

	/* log2 x = e + log m / ln 2, so a power of two gives e exactly. */
	value = __libm_log_parts(x, &exponent);
	inverse.high = LIBM_INVERSE_LN2_HIGH;
	inverse.low = LIBM_INVERSE_LN2_LOW;
	value = libm_dd_multiply(value, inverse);
	value = libm_dd_add_double(value, (double)exponent);

	/* Succeeded: the renormalized high part is the rounded result. */
	return value.high;
}

/*
 * Returns the base-10 logarithm of x.
 */
double
log10(
	double x)
{
	struct libm_dd value;
	struct libm_dd inverse;

	/* Zero, negative values, infinity and NaN have fixed answers. */
	if (!(x > 0.0) || x == HUGE_VAL)
		return libm_log_special(x);

	/*
	 * log10 x = log x / ln 10.  The product is within 2^-100 of the true
	 * value, so a power of ten gives its integer exactly.
	 */
	value = __libm_log_dd(x);
	inverse.high = LIBM_INVERSE_LN10_HIGH;
	inverse.low = LIBM_INVERSE_LN10_LOW;
	value = libm_dd_multiply(value, inverse);

	/* Succeeded: the renormalized high part is the rounded result. */
	return value.high;
}

/*
 * Returns log(1 + x).
 */
double
log1p(
	double x)
{
	struct libm_dd sum;
	struct libm_dd value;
	struct libm_dd argument;
	uint64_t magnitude;

	/* A NaN, unequal to itself, is passed on; +infinity is its own result. */
	if (x != x)
		return x + x;
	if (x == HUGE_VAL)
		return x;

	/* Below -1 there is no logarithm, and at -1 it is a pole. */
	if (x < -1.0)
		return __libm_invalid();
	if (x == -1.0)
		return __libm_pole(1U);

	/* A tiny x is its own result; a subnormal one has underflowed. */
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude < LIBM_LOG1P_TINY) {
		if (magnitude == 0U)
			return x;

		/* The result is x, inexact. */
		return __libm_check_underflow(x);
	}

	/* A small x uses the series directly, with its relative precision. */
	if (magnitude < LIBM_LOG1P_SERIES) {
		argument.high = x;
		argument.low = 0.0;
		value = libm_log1p_series(argument);
		return value.high + value.low;
	}

	/*
	 * Otherwise 1 + x is formed exactly as a pair.  log(high + low) is
	 * log(high) + low/high to within (low/high)^2/2, under 2^-106.
	 */
	sum = libm_two_sum(1.0, x);
	value = __libm_log_dd(sum.high);
	value = libm_dd_add_double(value, sum.low / sum.high);

	/* Succeeded: the renormalized high part is the rounded result. */
	return value.high;
}

/*
 * Returns the natural logarithm of x in binary32.
 */
float
logf(
	float x)
{
	double value;

	/* Computes in double. */
	value = log((double)x);

	/* Succeeded: every float logarithm is inside the float range. */
	return (float)value;
}

/*
 * Returns the base-2 logarithm of x in binary32.
 */
float
log2f(
	float x)
{
	double value;

	/* Computes in double. */
	value = log2((double)x);

	/* Succeeded: every float logarithm is inside the float range. */
	return (float)value;
}

/*
 * Returns the base-10 logarithm of x in binary32.
 */
float
log10f(
	float x)
{
	double value;

	/* Computes in double. */
	value = log10((double)x);

	/* Succeeded: every float logarithm is inside the float range. */
	return (float)value;
}

/*
 * Returns log(1 + x) in binary32.
 */
float
log1pf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = log1p((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns log(1 + r) as a double-double for |r| <= 2^-8.
 *
 * r - r^2/2 + r^3/3 are formed in double-double; the tail from r^4 to
 * r^11 is below 2^-26 of the result and is summed in double.  The first
 * term left out, r^12/12, is below 2^-91 of the result.
 */
static struct libm_dd
libm_log1p_series(
	struct libm_dd reduced)
{
	struct libm_dd square;
	struct libm_dd cube;
	struct libm_dd third;
	struct libm_dd half_square;
	struct libm_dd sum;
	const double *coefficients;
	double tail;

	/* Squares r exactly, with the cross term of its low part. */
	square = libm_two_product(reduced.high, reduced.high);
	square.low += 2.0 * reduced.high * reduced.low;

	/* r^3 / 3 in double-double. */
	cube = libm_dd_multiply(square, reduced);
	third.high = LIBM_THIRD_HIGH;
	third.low = LIBM_THIRD_LOW;
	cube = libm_dd_multiply(cube, third);

	/* Sums the tail r^4 (-1/4 + r/5 - ... + r^7/11) by Horner's rule. */
	coefficients = __libm_log_coefficients;
	tail = coefficients[7];
	tail = coefficients[6] + reduced.high * tail;
	tail = coefficients[5] + reduced.high * tail;
	tail = coefficients[4] + reduced.high * tail;
	tail = coefficients[3] + reduced.high * tail;
	tail = coefficients[2] + reduced.high * tail;
	tail = coefficients[1] + reduced.high * tail;
	tail = coefficients[0] + reduced.high * tail;
	tail = square.high * square.high * tail;

	/* Adds the terms from the smallest: the tail, r^3/3, -r^2/2 and r. */
	sum = libm_dd_add_double(cube, tail);
	half_square.high = -0.5 * square.high;
	half_square.low = -0.5 * square.low;
	sum = libm_dd_add(half_square, sum);
	sum = libm_dd_add(reduced, sum);

	/* Succeeded: log(1 + r). */
	return sum;
}

/*
 * Returns the logarithm of zero, a negative value, infinity or NaN.
 */
static double
libm_log_special(
	double x)
{
	/* A NaN, unequal to itself, is passed on. */
	if (x != x)
		return x + x;

	/* Zero of either sign is a pole at minus infinity. */
	if (x == 0.0)
		return __libm_pole(1U);

	/* A negative value has no real logarithm. */
	if (x < 0.0)
		return __libm_invalid();

	/* Succeeded: the logarithm of +infinity is +infinity. */
	return x;
}
