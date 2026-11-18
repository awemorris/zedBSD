/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exact remainders: fmod, remainder and remquo.
 *
 * Both arguments are unpacked into integer significands and the dividend
 * is reduced modulo the divisor by long division on those integers, eleven
 * bits of quotient at a time.  The partial remainder always stays below
 * the divisor, so nothing is ever rounded and the result is exact, as IEEE
 * 754 requires.
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/*
 * How many quotient bits one division step produces.
 *
 * The partial remainder is below the 53-bit divisor, so shifting it by
 * eleven bits still fits in 64.
 */
#define LIBM_DIVISION_STEP 11

/* The quotient bits remquo reports, which is more than the three C needs. */
#define LIBM_QUOTIENT_MASK UINT32_C(0x7fffffff)

/*
 * The result of reducing one significand modulo another.
 *
 * rest is the remainder in units of 2^exponent, below divisor, and quotient
 * holds the low bits of the integral quotient.
 */
struct libm_division {
	uint64_t rest;
	uint64_t divisor;
	int exponent;
	uint32_t quotient;
};

static void libm_reduce(const struct libm_unpacked *dividend, const struct libm_unpacked *divisor, struct libm_division *division);
static double libm_make(unsigned int sign, uint64_t magnitude, int exponent);
static double libm_nearest_remainder(double x, double y, int *quotient);

/*
 * Returns the remainder of x / y with the quotient truncated towards zero.
 */
double
fmod(
	double x,
	double y)
{
	struct libm_unpacked dividend;
	struct libm_unpacked divisor;
	struct libm_division division;
	uint64_t x_magnitude;
	uint64_t y_magnitude;
	double result;

	/* A NaN in either argument, unequal to itself, is passed on. */
	if (x != x || y != y)
		return x + y;

	/* An infinite dividend or a zero divisor has no remainder. */
	x_magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	y_magnitude = libm_bits(y) & LIBM_DOUBLE_MAGNITUDE;
	if (x_magnitude == LIBM_DOUBLE_INFINITY || y_magnitude == 0U)
		return __libm_invalid();

	/*
	 * A dividend smaller than the divisor, an infinite divisor included,
	 * is its own remainder.  The encodings are ordered like the magnitudes.
	 */
	if (x_magnitude < y_magnitude)
		return x;

	/* Reduces the significands against each other. */
	__libm_unpack(x, &dividend);
	__libm_unpack(y, &divisor);
	libm_reduce(&dividend, &divisor, &division);

	/* The remainder keeps the sign of the dividend, zero included. */
	result = libm_make(dividend.sign, division.rest, division.exponent);

	/* Succeeded: the remainder is exact. */
	return result;
}

/*
 * Returns the remainder of x / y with the quotient rounded to nearest even.
 */
double
remainder(
	double x,
	double y)
{
	int quotient;
	double result;

	/* The remainder is remquo's without the quotient bits. */
	result = libm_nearest_remainder(x, y, &quotient);

	/* Succeeded: the remainder is exact. */
	return result;
}

/*
 * Returns the remainder of x / y with the quotient rounded to nearest even,
 * and stores the low bits and the sign of that quotient.
 */
double
remquo(
	double x,
	double y,
	int *quotient)
{
	double result;

	/* The shared reduction fills in the quotient. */
	result = libm_nearest_remainder(x, y, quotient);

	/* Succeeded: the remainder is exact. */
	return result;
}

/*
 * Returns the remainder with the quotient rounded to nearest even.
 */
static double
libm_nearest_remainder(
	double x,
	double y,
	int *quotient)
{
	struct libm_unpacked dividend;
	struct libm_unpacked divisor;
	struct libm_division division;
	unsigned int sign;
	int magnitude;
	uint64_t x_magnitude;
	uint64_t y_magnitude;
	double remainder_value;

	/* A NaN in either argument, unequal to itself, is passed on. */
	*quotient = 0;
	if (x != x || y != y)
		return x + y;

	/* An infinite dividend or a zero divisor has no remainder. */
	x_magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	y_magnitude = libm_bits(y) & LIBM_DOUBLE_MAGNITUDE;
	if (x_magnitude == LIBM_DOUBLE_INFINITY || y_magnitude == 0U)
		return __libm_invalid();

	/* A finite dividend is its own remainder by an infinite divisor. */
	if (y_magnitude == LIBM_DOUBLE_INFINITY || x_magnitude == 0U)
		return x;

	/* Unpacks both arguments; their exponents decide the easy cases. */
	__libm_unpack(x, &dividend);
	__libm_unpack(y, &divisor);

	/* A dividend below half the divisor is its own remainder. */
	if (dividend.exponent < divisor.exponent - 1)
		return x;

	/*
	 * A dividend one binade below the divisor has a zero quotient; the
	 * divisor is doubled into the dividend's units for the comparison
	 * below.  Otherwise the long division runs.
	 */
	if (dividend.exponent < divisor.exponent) {
		division.rest = dividend.significand;
		division.divisor = divisor.significand << 1;
		division.exponent = dividend.exponent;
		division.quotient = 0U;
	} else {
		libm_reduce(&dividend, &divisor, &division);
	}

	/*
	 * The truncated remainder r is in [0, divisor).  When 2r passes the
	 * divisor, or ties it with an odd quotient, the nearest quotient is
	 * one larger and the remainder is r - divisor, of the opposite sign.
	 */
	sign = dividend.sign;
	if (division.rest * 2U > division.divisor) {
		division.rest = division.divisor - division.rest;
		division.quotient++;
		sign ^= 1U;
	} else if (division.rest * 2U == division.divisor &&
	    (division.quotient & 1U) != 0U) {
		division.rest = division.divisor - division.rest;
		division.quotient++;
		sign ^= 1U;
	}

	/* The quotient carries the sign of x / y. */
	magnitude = (int)(division.quotient & LIBM_QUOTIENT_MASK);
	*quotient = magnitude;
	if (dividend.sign != divisor.sign)
		*quotient = -magnitude;

	/* A zero remainder takes the sign of the dividend. */
	if (division.rest == 0U)
		return libm_make(dividend.sign, 0U, 0);

	/* Packs the remainder, which is exact. */
	remainder_value = libm_make(sign, division.rest, division.exponent);

	/* Succeeded: the remainder. */
	return remainder_value;
}

/*
 * Reduces the dividend modulo the divisor for a dividend of at least the
 * divisor's exponent.
 *
 * The dividend is m * 2^e and the divisor n * 2^f with e >= f, so the
 * remainder is (m * 2^(e - f) mod n) * 2^f.  The power of two is brought
 * in a few bits at a time, reducing after each step.
 */
static void
libm_reduce(
	const struct libm_unpacked *dividend,
	const struct libm_unpacked *divisor,
	struct libm_division *division)
{
	uint64_t rest;
	uint64_t step_quotient;
	uint32_t quotient;
	int distance;
	int step;

	/* The significands have the same length, so the first quotient is 0 or 1. */
	rest = dividend->significand;
	quotient = 0U;
	if (rest >= divisor->significand) {
		rest -= divisor->significand;
		quotient = 1U;
	}

	/* Brings in the remaining power of two, a few bits per step. */
	distance = dividend->exponent - divisor->exponent;
	while (distance > 0) {
		step = LIBM_DIVISION_STEP;
		if (distance < step)
			step = distance;

		/* Shifts the partial remainder and divides it once more. */
		rest <<= step;
		step_quotient = rest / divisor->significand;
		rest -= step_quotient * divisor->significand;
		quotient = (quotient << step) + (uint32_t)step_quotient;
		distance -= step;
	}

	/* Records the remainder in the divisor's units. */
	division->rest = rest;
	division->divisor = divisor->significand;
	division->exponent = divisor->exponent;
	division->quotient = quotient;
}

/*
 * Builds sign * magnitude * 2^exponent, which the caller knows is exact.
 */
static double
libm_make(
	unsigned int sign,
	uint64_t magnitude,
	int exponent)
{
	uint64_t significand;
	int shift;
	double result;

	/* A zero magnitude is a zero of the given sign. */
	if (magnitude == 0U)
		return libm_from_bits((uint64_t)sign << 63);

	/* Moves the leading bit to bit 63, as the packing helper expects. */
	significand = magnitude;
	shift = 0;
	while ((significand >> 63) == 0U) {
		significand <<= 1;
		shift++;
	}

	/* Packs the value; an exact value raises nothing. */
	result = __libm_pack(sign, exponent + 63 - shift, significand);

	/* Succeeded: the exact value is the result. */
	return result;
}
