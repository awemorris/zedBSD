/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The exponential functions: exp, exp2 and expm1.
 *
 * The method is Tang's table-driven one.  x is written as
 * (128 m + j) ln2/128 + r with |r| <= ln2/256, so exp(x) is
 * 2^m * 2^(j/128) * exp(r).  The table holds 2^(j/128) as double-doubles,
 * ln2/128 is split into three pieces (Cody and Waite) so that r comes out
 * as a double-double, and exp(r) - 1 is its Taylor series to r^7, with the
 * first two terms kept in double-double.  The result, about 2^-70 from the
 * true value, is rounded once when the power of two is applied.
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/*
 * The magnitudes below which exp(x) rounds to 1 and expm1(x) to x.
 *
 * Below 2^-54 the next term of the series is under a quarter ulp.
 */
#define LIBM_EXP_TINY UINT64_C(0x3c90000000000000)

/*
 * The largest x for which expm1 uses its kernel directly, without a table
 * step: below ln2/256 the nearest step is always the zeroth.
 */
#define LIBM_EXPM1_DIRECT 0.0027

static double libm_power_of_two(int exponent);

/*
 * Returns exp(r) - 1 as a double-double for |r| <= about ln2/256.
 *
 * r + r^2/2 is kept in double-double; the tail r^3 (1/6 + r/24 + ... +
 * r^4/5040) is under 2^-27 of the result and is summed in double.  The
 * first term left out, r^8/8!, is below 2^-83.
 */
struct libm_dd
__libm_expm1_kernel(
	struct libm_dd reduced)
{
	struct libm_dd square;
	struct libm_dd half_square;
	struct libm_dd result;
	const double *coefficients;
	double tail;

	/* Squares r exactly, with the cross term of its low part. */
	square = libm_two_product(reduced.high, reduced.high);
	square.low += 2.0 * reduced.high * reduced.low;

	/* Sums the tail of the series by Horner's rule. */
	coefficients = __libm_exp_coefficients;
	tail = coefficients[4];
	tail = coefficients[3] + reduced.high * tail;
	tail = coefficients[2] + reduced.high * tail;
	tail = coefficients[1] + reduced.high * tail;
	tail = coefficients[0] + reduced.high * tail;
	tail = square.high * reduced.high * tail;

	/* Adds r^2/2, which is larger than the tail, and then r. */
	half_square = libm_fast_two_sum(square.high * 0.5, square.low * 0.5 + tail);
	result = libm_dd_add(reduced, half_square);

	/* Succeeded: exp(r) - 1. */
	return result;
}

/*
 * Returns 2^(index/128) * exp(r) as a double-double.
 */
struct libm_dd
__libm_exp_reduced(
	int index,
	struct libm_dd reduced)
{
	struct libm_dd power;
	struct libm_dd excess;
	struct libm_dd result;

	/* exp(r) - 1 is small, so 2^(j/128) (1 + e) keeps full precision. */
	power = __libm_exp_table[index];
	excess = __libm_expm1_kernel(reduced);
	excess = libm_dd_multiply(power, excess);
	result = libm_dd_add(power, excess);

	/* Succeeded: the table value times exp(r). */
	return result;
}

/*
 * Returns exp(x) for a double-double x as a double-double and a power of
 * two: the result is the pair times 2^exponent.
 *
 * |x.high| must stay below 2^11, which covers every argument whose
 * exponential is finite and nonzero.
 */
struct libm_dd
__libm_exp_dd(
	struct libm_dd x,
	int *exponent)
{
	struct libm_dd product;
	struct libm_dd reduced;
	struct libm_dd result;
	double steps;
	double remainder;
	int step;
	int index;

	/*
	 * Finds the nearest number of ln2/128 steps.  Adding and taking away
	 * 1.5 * 2^52 rounds to an integer without a conversion.
	 */
	steps = x.high * LIBM_EXP_INVERSE_STEP + LIBM_ROUNDING_SHIFTER;
	steps -= LIBM_ROUNDING_SHIFTER;
	step = (int)steps;

	/*
	 * Takes the steps away in three pieces.  The first piece has 35 bits,
	 * so its product with the step count and the difference are exact; the
	 * second is taken away without error, and the third is below the
	 * precision that matters.
	 */
	remainder = x.high - steps * LIBM_EXP_STEP_1;
	product = libm_two_product(steps, LIBM_EXP_STEP_2);
	reduced = libm_two_sum(remainder, -product.high);
	reduced.low += (x.low - product.low) - steps * LIBM_EXP_STEP_3;
	reduced = libm_two_sum(reduced.high, reduced.low);

	/* The step count splits into the table index and a power of two. */
	index = step & 127;
	*exponent = (step - index) / 128;

	/* Evaluates the table entry times exp(r). */
	result = __libm_exp_reduced(index, reduced);

	/* Succeeded: the mantissa of exp(x). */
	return result;
}

/*
 * Returns e raised to the power x.
 */
double
exp(
	double x)
{
	struct libm_dd argument;
	struct libm_dd value;
	uint64_t magnitude;
	int exponent;
	double result;

	/* A NaN, unequal to itself, is passed on. */
	if (x != x)
		return x + x;

	/* The infinities give infinity and zero, without an error. */
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude == LIBM_DOUBLE_INFINITY) {
		if (x > 0.0)
			return x;

		/* exp(-infinity) is exactly zero. */
		return 0.0;
	}

	/*
	 * Past these bounds the result overflows or rounds to zero; between
	 * them and the exact thresholds the scaling below decides.
	 */
	if (x > 710.0)
		return __libm_overflow(0U);
	if (x < -746.0)
		return __libm_underflow(0U);

	/* A tiny argument rounds to one, on the side of its sign. */
	if (magnitude < LIBM_EXP_TINY)
		return 1.0 + x;

	/* Evaluates the mantissa and applies the power of two once. */
	argument.high = x;
	argument.low = 0.0;
	value = __libm_exp_dd(argument, &exponent);
	result = __libm_scale(value, exponent);

	/* Succeeded: exp(x) rounded once. */
	return result;
}

/*
 * Returns 2 raised to the power x.
 */
double
exp2(
	double x)
{
	struct libm_dd reduced;
	struct libm_dd ln2;
	struct libm_dd value;
	uint64_t magnitude;
	double steps;
	double fraction;
	double result;
	int step;
	int index;

	/* A NaN, unequal to itself, is passed on. */
	if (x != x)
		return x + x;

	/* The infinities give infinity and zero, without an error. */
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude == LIBM_DOUBLE_INFINITY) {
		if (x > 0.0)
			return x;

		/* exp2(-infinity) is exactly zero. */
		return 0.0;
	}

	/* 2^1024 overflows, and below 2^-1080 everything rounds to zero. */
	if (x >= 1024.0)
		return __libm_overflow(0U);
	if (x < -1080.0)
		return __libm_underflow(0U);

	/* A tiny argument rounds to one, on the side of its sign. */
	if (magnitude < LIBM_EXP_TINY)
		return 1.0 + x;

	/*
	 * Finds the nearest multiple of 1/128.  The remaining fraction is
	 * exact: it has no bits above 2^-8 and none below the last bit of x.
	 */
	steps = x * 128.0 + LIBM_ROUNDING_SHIFTER;
	steps -= LIBM_ROUNDING_SHIFTER;
	step = (int)steps;
	fraction = x - steps * 0.0078125;

	/* 2^f is exp(f ln 2); the product is formed in double-double. */
	ln2.high = LIBM_LN2_HIGH;
	ln2.low = LIBM_LN2_LOW;
	reduced = libm_dd_multiply_double(ln2, fraction);

	/* The table entry times exp(f ln 2), then the power of two. */
	index = step & 127;
	value = __libm_exp_reduced(index, reduced);
	result = __libm_scale(value, (step - index) / 128);

	/* Succeeded: an integral x gives the exact power of two. */
	return result;
}

/*
 * Returns e raised to the power x, minus one.
 */
double
expm1(
	double x)
{
	struct libm_dd argument;
	struct libm_dd value;
	uint64_t magnitude;
	int exponent;
	double power;
	double result;

	/* A NaN, unequal to itself, is passed on. */
	if (x != x)
		return x + x;

	/* The infinities give infinity and minus one, without an error. */
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude == LIBM_DOUBLE_INFINITY) {
		if (x > 0.0)
			return x;

		/* exp(-infinity) - 1 is exactly minus one. */
		return -1.0;
	}

	/* A large x overflows; below -40 the result rounds to minus one. */
	if (x > 710.0)
		return __libm_overflow(0U);
	if (x < -40.0)
		return -1.0;

	/* A tiny x is its own result; a subnormal one has underflowed. */
	if (magnitude < LIBM_EXP_TINY) {
		if (magnitude == 0U)
			return x;

		/* The result is x, inexact. */
		return __libm_check_underflow(x);
	}

	/*
	 * Near zero the kernel gives exp(x) - 1 directly, with its full
	 * relative precision, instead of subtracting one from exp(x).
	 */
	argument.high = x;
	argument.low = 0.0;
	if (x < LIBM_EXPM1_DIRECT && x > -LIBM_EXPM1_DIRECT) {
		value = __libm_expm1_kernel(argument);
		return value.high + value.low;
	}

	/* Elsewhere exp(x) is at least 1.0027 or at most 0.9973, and one is taken away. */
	value = __libm_exp_dd(argument, &exponent);

	/* Past 2^100 taking away one no longer changes the rounded result. */
	if (exponent > 100) {
		result = __libm_scale(value, exponent);
		return result;
	}

	/* Scales the pair exactly and subtracts one in double-double. */
	power = libm_power_of_two(exponent);
	value.high *= power;
	value.low *= power;
	value = libm_dd_add_double(value, -1.0);

	/* Succeeded: the renormalized high part is the rounded result. */
	return value.high;
}

/*
 * Returns e raised to the power x in binary32.
 */
float
expf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = exp((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns 2 raised to the power x in binary32.
 */
float
exp2f(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = exp2((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns e raised to the power x, minus one, in binary32.
 */
float
expm1f(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = expm1((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns 2^exponent for an exponent inside the normal range.
 */
static double
libm_power_of_two(
	int exponent)
{
	/* The biased exponent alone encodes the power. */
	return libm_from_bits((uint64_t)(exponent + LIBM_DOUBLE_BIAS) << 52);
}
