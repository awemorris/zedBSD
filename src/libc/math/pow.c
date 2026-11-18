/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The power function.
 *
 * pow(x, y) is exp(y log |x|) with the sign an odd integer y gives a
 * negative x.  log |x| comes from the logarithm's kernel as a
 * double-double within 2^-78 of the true value, y times it is formed as a
 * double-double, and the exponential's kernel takes that double-double
 * argument, so the result is within about 2^-68 before it is rounded once.
 * A result that is exactly representable, such as an integral power of an
 * integer, therefore comes out exactly.  A power of two raised to an
 * integer is scaled directly, which also keeps an exact subnormal result
 * free of a spurious underflow.  The special cases are those of C11 Annex
 * F, F.10.4.4.
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/*
 * Past this magnitude of y every result overflows or underflows, because
 * |log x| is at least 2^-53 for any x other than one.
 */
#define LIBM_POW_HUGE_Y 18446744073709551616.0

/* The encoding of 1.0, which orders magnitudes against one. */
#define LIBM_ONE_BITS UINT64_C(0x3ff0000000000000)

/*
 * The kinds of y that decide the sign of a power of a negative base.
 */
enum libm_parity {
	LIBM_NOT_INTEGER,
	LIBM_EVEN_INTEGER,
	LIBM_ODD_INTEGER
};

static enum libm_parity libm_classify_integer(double y);
static double libm_pow_special(double x, double y, enum libm_parity parity);
static double libm_signed(double magnitude, unsigned int sign);

/*
 * Returns x raised to the power y.
 */
double
pow(
	double x,
	double y)
{
	struct libm_dd logarithm;
	struct libm_dd product;
	struct libm_dd value;
	enum libm_parity parity;
	uint64_t x_bits;
	uint64_t x_magnitude;
	uint64_t y_magnitude;
	unsigned int sign;
	double base;
	double power;
	double result;
	int exponent;

	/* Any x to the power zero, and one to any power, is one, NaNs included. */
	y_magnitude = libm_bits(y) & LIBM_DOUBLE_MAGNITUDE;
	if (y_magnitude == 0U || x == 1.0)
		return 1.0;

	/* A NaN in either argument, unequal to itself, is passed on. */
	if (x != x || y != y)
		return x + y;

	/* Infinities and zeros follow the table of Annex F. */
	parity = libm_classify_integer(y);
	x_bits = libm_bits(x);
	x_magnitude = x_bits & LIBM_DOUBLE_MAGNITUDE;
	if (y_magnitude == LIBM_DOUBLE_INFINITY ||
	    x_magnitude == LIBM_DOUBLE_INFINITY ||
	    x_magnitude == 0U)
		return libm_pow_special(x, y, parity);

	/* A negative base needs an integral exponent, and an odd one keeps the sign. */
	sign = 0U;
	if ((x_bits >> 63) != 0U) {
		if (parity == LIBM_NOT_INTEGER)
			return __libm_invalid();
		if (parity == LIBM_ODD_INTEGER)
			sign = 1U;
	}

	/* The rest works on |x|. */
	base = libm_from_bits(x_magnitude);

	/*
	 * A power of two to an integral power is a power of two: the product
	 * of the exponents, which scalbn applies exactly or rounds once.
	 * Exponents past the range are clamped; they overflow or underflow.
	 */
	if (parity != LIBM_NOT_INTEGER &&
	    (x_bits & LIBM_DOUBLE_FRACTION) == 0U &&
	    x_magnitude >= LIBM_DOUBLE_HIDDEN) {
		power = (double)((int)(x_magnitude >> 52) - LIBM_DOUBLE_BIAS) * y;
		if (power > 5000.0)
			power = 5000.0;
		if (power < -5000.0)
			power = -5000.0;
		result = scalbn(1.0, (int)power);
		return libm_signed(result, sign);
	}

	/* A huge y sends every other base to infinity or to zero. */
	if (y > LIBM_POW_HUGE_Y || y < -LIBM_POW_HUGE_Y) {
		if ((base > 1.0) == (y > 0.0))
			return __libm_overflow(sign);

		/* The base is on the other side of one from the sign of y. */
		return __libm_underflow(sign);
	}

	/* y log |x| as a double-double. */
	logarithm = __libm_log_dd(base);
	product = libm_two_product(y, logarithm.high);
	product.low += y * logarithm.low;
	product = libm_fast_two_sum(product.high, product.low);

	/* Past these bounds the result overflows or rounds to zero. */
	if (product.high > 710.0)
		return __libm_overflow(sign);
	if (product.high < -746.0)
		return __libm_underflow(sign);

	/* The exponential of the product, given the sign and rounded once. */
	value = __libm_exp_dd(product, &exponent);
	if (sign != 0U) {
		value.high = -value.high;
		value.low = -value.low;
	}

	/* Applies the power of two, rounding once. */
	result = __libm_scale(value, exponent);

	/* Succeeded: pow(x, y) rounded once. */
	return result;
}

/*
 * Returns x raised to the power y in binary32.
 */
float
powf(
	float x,
	float y)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = pow((double)x, (double)y);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Tells whether y is an integer, and if so whether it is odd.
 */
static enum libm_parity
libm_classify_integer(
	double y)
{
	uint64_t bits;
	int exponent;
	uint64_t unit;

	/* Reads the unbiased exponent of y. */
	bits = libm_bits(y);
	exponent = (int)((bits >> 52) & 0x7ffU) - LIBM_DOUBLE_BIAS;

	/* From 2^53 on every value is an even integer, infinity included. */
	if (exponent >= 53)
		return LIBM_EVEN_INTEGER;

	/* Below one only zero is an integer, and zero is even. */
	if (exponent < 0) {
		if ((bits & LIBM_DOUBLE_MAGNITUDE) == 0U)
			return LIBM_EVEN_INTEGER;

		/* A nonzero magnitude below one has a fraction. */
		return LIBM_NOT_INTEGER;
	}

	/* Fraction bits below the unit make y non-integral. */
	unit = LIBM_DOUBLE_HIDDEN >> exponent;
	if ((bits & (unit - 1U)) != 0U)
		return LIBM_NOT_INTEGER;

	/* The unit bit is the parity; for 1 <= |y| < 2 it is the hidden bit. */
	if (exponent == 0 || (bits & unit) != 0U)
		return LIBM_ODD_INTEGER;

	/* Succeeded: an even integer. */
	return LIBM_EVEN_INTEGER;
}

/*
 * Returns pow(x, y) for an infinite y, an infinite x or a zero x.
 */
static double
libm_pow_special(
	double x,
	double y,
	enum libm_parity parity)
{
	uint64_t x_magnitude;
	uint64_t y_magnitude;
	unsigned int x_sign;
	unsigned int odd_sign;
	double signed_value;

	/* Reads the classes the table depends on. */
	x_magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	y_magnitude = libm_bits(y) & LIBM_DOUBLE_MAGNITUDE;
	x_sign = (unsigned int)(libm_bits(x) >> 63);

	/* An infinite y: -1 gives 1, otherwise |x| against one decides. */
	if (y_magnitude == LIBM_DOUBLE_INFINITY) {
		if (x == -1.0)
			return 1.0;
		if ((x_magnitude < LIBM_ONE_BITS) == (y > 0.0))
			return 0.0;

		/* |x| > 1 with y = +infinity, or |x| < 1 with y = -infinity. */
		return HUGE_VAL;
	}

	/* An odd integral y keeps the sign of a zero or infinite x. */
	odd_sign = 0U;
	if (parity == LIBM_ODD_INTEGER)
		odd_sign = x_sign;

	/* A zero x: a negative y is a pole, a positive one gives zero. */
	if (x_magnitude == 0U) {
		if (y < 0.0)
			return __libm_pole(odd_sign);

		/* Zero to a positive power is zero. */
		return libm_signed(0.0, odd_sign);
	}

	/* An infinite x: a negative y gives zero, a positive one infinity. */
	if (y < 0.0)
		return libm_signed(0.0, odd_sign);

	/* Gives the infinity the sign an odd y keeps. */
	signed_value = libm_signed(HUGE_VAL, odd_sign);

	/* Succeeded: infinity to a positive power. */
	return signed_value;
}

/*
 * Returns a magnitude with the given sign.
 */
static double
libm_signed(
	double magnitude,
	unsigned int sign)
{
	/* A set sign bit negates, which is exact. */
	if (sign != 0U)
		return -magnitude;

	/* Succeeded: the magnitude is positive. */
	return magnitude;
}
