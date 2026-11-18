/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The gamma function and its logarithm.
 *
 * For y >= 10, log gamma(y) is Stirling's series,
 *   (y - 1/2) log y - y + log(2 pi)/2 + sum B(2k) / (2k (2k-1) y^(2k-1)),
 * whose twelve terms reach below 2^-66 there, with the leading terms in
 * double-double.  A smaller x is shifted up: gamma(x) = gamma(x + n) / P
 * with P = x (x + 1) ... (x + n - 1) formed exactly enough as a
 * double-double, for negative x as well down to -20.  Further left the
 * reflection formula gamma(x) gamma(1 - x) = pi / sin(pi x) is used in the
 * logarithm, with sin(pi x) reduced exactly.  Near the zeros of log gamma
 * at 1 and 2 its Taylor series about them keeps the relative precision
 * that the difference of two logarithms would lose.
 *
 * log gamma loses relative precision near its other zeros, which lie next
 * to the negative integers from -2 down to about -18: there the result is
 * accurate to about 2^-66 in absolute terms, not relative ones.
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/* From here on Stirling's series is used directly. */
#define LIBM_STIRLING_START 10.0

/* Below this log gamma uses the reflection formula instead of the shift. */
#define LIBM_REFLECTION_START -20.0

/* Within this distance of 1 or 2 the Taylor series about them is used. */
#define LIBM_NEAR_ROOT 0.0009765625

/* Past this gamma overflows. */
#define LIBM_GAMMA_OVERFLOW 171.625

/* Below this gamma of a negative non-integer underflows to zero. */
#define LIBM_GAMMA_UNDERFLOW -185.0

/* From 2^52 on log gamma is computed scaled, and every x is an integer. */
#define LIBM_TWO_TO_52 4503599627370496.0

/* The encoding of 2^-54: below it gamma(x) rounds to 1/x. */
#define LIBM_GAMMA_TINY UINT64_C(0x3c90000000000000)

int signgam;

static struct libm_dd libm_stirling(struct libm_dd y);
static void libm_shift_up(double x, struct libm_dd *shifted, struct libm_dd *product);
static struct libm_dd libm_sin_pi(double magnitude);
static struct libm_dd libm_log_abs_dd(struct libm_dd value);
static struct libm_dd libm_near_root(double offset, double first_high, double first_low, const double *coefficients);
static struct libm_dd libm_lgamma_dd(double x, int *sign);
static double libm_lgamma_huge(double x);
static int libm_is_integer(double x);

/*
 * Returns the logarithm of the absolute value of the gamma function, and
 * leaves the sign of gamma(x) in signgam.
 */
double
lgamma(
	double x)
{
	struct libm_dd value;
	uint64_t x_bits;
	uint64_t magnitude;
	int sign;
	int integral;

	/* A NaN is passed on, and an infinity gives +infinity. */
	signgam = 1;
	x_bits = libm_bits(x);
	magnitude = x_bits & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude >= LIBM_DOUBLE_INFINITY)
		return x * x;

	/* Zero and the negative integers are poles. */
	if (magnitude == 0U) {
		if (x_bits != magnitude)
			signgam = -1;

		/* log |gamma(+-0)| is +infinity. */
		return __libm_pole(0U);
	}

	/* The negative integers are poles too. */
	integral = libm_is_integer(x);
	if (x < 0.0 && integral)
		return __libm_pole(0U);

	/* A huge x needs its product scaled to stay inside the range. */
	if (x >= LIBM_TWO_TO_52)
		return libm_lgamma_huge(x);

	/* Everything else goes through the double-double evaluation. */
	value = libm_lgamma_dd(x, &sign);
	signgam = sign;

	/* Succeeded: the pair rounded once. */
	return value.high + value.low;
}

/*
 * Returns the gamma function of x.
 */
double
tgamma(
	double x)
{
	struct libm_dd shifted;
	struct libm_dd product;
	struct libm_dd logarithm;
	struct libm_dd value;
	uint64_t magnitude;
	unsigned int sign;
	int integral;
	int exponent;
	int product_exponent;
	int log_sign;
	double result;

	/* A NaN is passed on; +infinity is its own result, -infinity has none. */
	if (x != x)
		return x + x;
	if (x == HUGE_VAL)
		return x;
	if (x == -HUGE_VAL)
		return __libm_invalid();

	/* Zero is a pole with its sign; a negative integer has no result. */
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude == 0U)
		return __libm_pole((unsigned int)(libm_bits(x) >> 63));
	integral = libm_is_integer(x);
	if (x < 0.0 && integral)
		return __libm_invalid();

	/* Past 171.62 gamma overflows. */
	if (x > LIBM_GAMMA_OVERFLOW)
		return __libm_overflow(0U);

	/* A tiny x gives 1/x - gamma, and the constant is below the ulp. */
	if (magnitude < LIBM_GAMMA_TINY) {
		result = 1.0 / x;
		if (result == HUGE_VAL || result == -HUGE_VAL)
			return __libm_overflow((unsigned int)(libm_bits(x) >> 63));

		/* The reciprocal rounded once. */
		return result;
	}

	/* From 10 on, exp of Stirling's series. */
	if (x >= LIBM_STIRLING_START) {
		shifted.high = x;
		shifted.low = 0.0;
		logarithm = libm_stirling(shifted);
		value = __libm_exp_dd(logarithm, &exponent);
		result = __libm_scale(value, exponent);
		return result;
	}

	/*
	 * Far left, gamma(x) = pi / (sin(pi x) gamma(1 - x)) in the logarithm,
	 * which stays inside the range when gamma(1 - x) would overflow.
	 */
	if (x <= LIBM_REFLECTION_START) {
		if (x < LIBM_GAMMA_UNDERFLOW) {
			value = libm_sin_pi(-x);
			sign = 0U;
			if (value.high > 0.0)
				sign = 1U;
			return __libm_underflow(sign);
		}

		/* The logarithm of |gamma(x)|, then its exponential and sign. */
		logarithm = libm_lgamma_dd(x, &log_sign);
		value = __libm_exp_dd(logarithm, &exponent);
		if (log_sign < 0) {
			value.high = -value.high;
			value.low = -value.low;
		}

		/* Applies the power of two, rounding once. */
		result = __libm_scale(value, exponent);
		return result;
	}

	/*
	 * Elsewhere gamma(x) = gamma(x + n) / P.  P is brought near one by a
	 * power of two first, so the quotient cannot leave the range before
	 * the final scaling.
	 */
	libm_shift_up(x, &shifted, &product);
	logarithm = libm_stirling(shifted);
	value = __libm_exp_dd(logarithm, &exponent);
	product_exponent = ilogb(product.high);
	product.high = scalbn(product.high, -product_exponent);
	product.low = scalbn(product.low, -product_exponent);
	value = libm_dd_divide(value, product);
	result = __libm_scale(value, exponent - product_exponent);

	/* Succeeded: gamma(x) rounded once. */
	return result;
}

/*
 * Returns log |gamma(x)| in binary32, leaving the sign in signgam.
 */
float
lgammaf(
	float x)
{
	double value;

	/* Computes in double, which sets signgam, and rounds once more. */
	value = lgamma((double)x);

	/* Succeeded: log gamma of a float is inside the float range. */
	return (float)value;
}

/*
 * Returns the gamma function of x in binary32.
 */
float
tgammaf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = tgamma((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns log |gamma(x)| as a double-double for a finite x that is not a
 * pole and below 2^52, with the sign of gamma(x).
 */
static struct libm_dd
libm_lgamma_dd(
	double x,
	int *sign)
{
	struct libm_dd shifted;
	struct libm_dd product;
	struct libm_dd result;
	struct libm_dd sine;
	struct libm_dd pi;
	struct libm_dd complement;

	/* gamma is positive right of zero. */
	*sign = 1;

	/* Stirling's series directly. */
	if (x >= LIBM_STIRLING_START) {
		shifted.high = x;
		shifted.low = 0.0;
		result = libm_stirling(shifted);
		return result;
	}

	/* The Taylor series about the zeros at 1 and 2. */
	if (x > 1.0 - LIBM_NEAR_ROOT && x < 1.0 + LIBM_NEAR_ROOT) {
		result = libm_near_root(x - 1.0, -LIBM_EULER_HIGH, -LIBM_EULER_LOW,
		    __libm_lgamma_one_coefficients);
		return result;
	}

	/* The same about 2. */
	if (x > 2.0 - LIBM_NEAR_ROOT && x < 2.0 + LIBM_NEAR_ROOT) {
		result = libm_near_root(x - 2.0, LIBM_ONE_MINUS_EULER_HIGH,
		    LIBM_ONE_MINUS_EULER_LOW, __libm_lgamma_two_coefficients);
		return result;
	}

	/*
	 * Far left: log |gamma(x)| = log pi - log |sin(pi x)| - log gamma(1 - x),
	 * with 1 - x exact as a pair and the sign that of sin(pi x).
	 */
	if (x <= LIBM_REFLECTION_START) {
		sine = libm_sin_pi(-x);
		if (sine.high > 0.0)
			*sign = -1;
		complement = libm_two_sum(1.0, -x);
		result = libm_stirling(complement);
		sine = libm_log_abs_dd(sine);
		result = libm_dd_add(result, sine);
		pi.high = LIBM_LOG_PI_HIGH;
		pi.low = LIBM_LOG_PI_LOW;
		result.high = -result.high;
		result.low = -result.low;
		result = libm_dd_add(pi, result);
		return result;
	}

	/* Elsewhere log gamma(x + n) - log |P|, with the sign of P. */
	libm_shift_up(x, &shifted, &product);
	if (product.high < 0.0)
		*sign = -1;
	result = libm_log_abs_dd(product);
	result.high = -result.high;
	result.low = -result.low;
	shifted = libm_stirling(shifted);
	result = libm_dd_add(shifted, result);

	/* Succeeded: log |gamma(x)|. */
	return result;
}

/*
 * Returns log gamma(y) by Stirling's series for y >= 10, a double-double.
 */
static struct libm_dd
libm_stirling(
	struct libm_dd y)
{
	struct libm_dd logarithm;
	struct libm_dd factor;
	struct libm_dd result;
	struct libm_dd constant;
	struct libm_dd one;
	struct libm_dd reciprocal;
	struct libm_dd twelfth;
	struct libm_dd first;
	const double *coefficients;
	double inverse;
	double square;
	double tail;

	/* (y - 1/2) log y - y, in double-double. */
	logarithm = libm_log_abs_dd(y);
	factor = libm_dd_add_double(y, -0.5);
	result = libm_dd_multiply(factor, logarithm);
	factor.high = -y.high;
	factor.low = -y.low;
	result = libm_dd_add(result, factor);

	/* Adds log(2 pi) / 2. */
	constant.high = LIBM_HALF_LOG_TWO_PI_HIGH;
	constant.low = LIBM_HALF_LOG_TWO_PI_LOW;
	result = libm_dd_add(result, constant);

	/*
	 * The series in 1/y.  Its first term, 1/(12 y), is up to 1/120 and is
	 * formed in double-double; the rest, below 3e-6, in double.
	 */
	one.high = 1.0;
	one.low = 0.0;
	reciprocal = libm_dd_divide(one, y);
	twelfth.high = LIBM_ONE_TWELFTH_HIGH;
	twelfth.low = LIBM_ONE_TWELFTH_LOW;
	first = libm_dd_multiply(reciprocal, twelfth);
	result = libm_dd_add(result, first);
	inverse = reciprocal.high;
	square = inverse * inverse;
	coefficients = __libm_stirling_coefficients;
	tail = coefficients[11];
	tail = coefficients[10] + square * tail;
	tail = coefficients[9] + square * tail;
	tail = coefficients[8] + square * tail;
	tail = coefficients[7] + square * tail;
	tail = coefficients[6] + square * tail;
	tail = coefficients[5] + square * tail;
	tail = coefficients[4] + square * tail;
	tail = coefficients[3] + square * tail;
	tail = coefficients[2] + square * tail;
	tail = coefficients[1] + square * tail;
	result = libm_dd_add_double(result, tail * inverse * square);

	/* Succeeded: log gamma(y). */
	return result;
}

/*
 * Shifts x up to x + n >= 10, and forms P = x (x + 1) ... (x + n - 1).
 *
 * Every step adds one to a pair exactly and multiplies the product by it,
 * so a factor next to zero keeps its full relative precision.
 */
static void
libm_shift_up(
	double x,
	struct libm_dd *shifted,
	struct libm_dd *product)
{
	/* Starts from x itself and the empty product. */
	shifted->high = x;
	shifted->low = 0.0;
	product->high = 1.0;
	product->low = 0.0;

	/* Multiplies in each factor and steps up by one. */
	while (shifted->high < LIBM_STIRLING_START) {
		*product = libm_dd_multiply(*product, *shifted);
		*shifted = libm_dd_add_double(*shifted, 1.0);
	}
}

/*
 * Returns sin(pi m) for a positive non-integer m, as a double-double.
 *
 * m mod 2 is exact, and folding it into [0, 1/2] by the symmetries of the
 * sine is exact as well, so the product with pi is the only rounding.
 */
static struct libm_dd
libm_sin_pi(
	double magnitude)
{
	struct libm_dd pi;
	struct libm_dd angle;
	struct libm_dd sine;
	struct libm_dd cosine;
	struct libm_dd result;
	double remainder;
	int negative;

	/* sin(pi m) has period 2 and changes sign every unit. */
	remainder = fmod(magnitude, 2.0);
	negative = 0;
	if (remainder >= 1.0) {
		remainder -= 1.0;
		negative = 1;
	}

	/* sin(pi r) = sin(pi (1 - r)). */
	if (remainder > 0.5)
		remainder = 1.0 - remainder;

	/* Up to 1/4 the sine directly, beyond it the cosine of the rest. */
	pi.high = LIBM_PI_HIGH;
	pi.low = LIBM_PI_LOW;
	if (remainder <= 0.25) {
		angle = libm_dd_multiply_double(pi, remainder);
		__libm_sin_cos_dd(angle, &sine, &cosine);
		result = sine;
	} else {
		angle = libm_dd_multiply_double(pi, 0.5 - remainder);
		__libm_sin_cos_dd(angle, &sine, &cosine);
		result = cosine;
	}

	/* An odd unit turns the sign. */
	if (negative) {
		result.high = -result.high;
		result.low = -result.low;
	}

	/* Succeeded: sin(pi m). */
	return result;
}

/*
 * Returns log |high + low| for a nonzero double-double.
 */
static struct libm_dd
libm_log_abs_dd(
	struct libm_dd value)
{
	struct libm_dd result;
	double magnitude;

	/* log |h + l| = log |h| + l/h to within (l/h)^2 / 2. */
	magnitude = fabs(value.high);
	result = __libm_log_dd(magnitude);
	result = libm_dd_add_double(result, value.low / value.high);

	/* Succeeded: the logarithm of the magnitude. */
	return result;
}

/*
 * Evaluates first e + sum c(k) e^k for k = 2 .. 9, the Taylor series of
 * log gamma about 1 or 2, for |e| < 2^-10.
 */
static struct libm_dd
libm_near_root(
	double offset,
	double first_high,
	double first_low,
	const double *coefficients)
{
	struct libm_dd first;
	struct libm_dd result;
	double tail;

	/* The tail from e^2, below 2^-10 of the first term, in double. */
	tail = coefficients[7];
	tail = coefficients[6] + offset * tail;
	tail = coefficients[5] + offset * tail;
	tail = coefficients[4] + offset * tail;
	tail = coefficients[3] + offset * tail;
	tail = coefficients[2] + offset * tail;
	tail = coefficients[1] + offset * tail;
	tail = coefficients[0] + offset * tail;
	tail = offset * offset * tail;

	/* The first term exactly enough, then the tail. */
	first.high = first_high;
	first.low = first_low;
	result = libm_dd_multiply_double(first, offset);
	result = libm_dd_add_double(result, tail);

	/* Succeeded: log gamma near its zero. */
	return result;
}

/*
 * Returns log gamma(x) for x >= 2^52.
 *
 * x (log x - 1) is formed with x scaled down by 2^128, so the double-double
 * product cannot overflow, and scaled back once at the end; the terms that
 * do not grow with x are added in the scaled units.
 */
static double
libm_lgamma_huge(
	double x)
{
	struct libm_dd logarithm;
	struct libm_dd scaled;
	struct libm_dd small;
	double scale;
	double result;

	/* x (log x - 1) in units of 2^128. */
	logarithm = __libm_log_dd(x);
	scaled = libm_dd_add_double(logarithm, -1.0);
	scale = 2.9387358770557188e-39;
	scaled = libm_dd_multiply_double(scaled, x * scale);

	/* log(2 pi)/2 - (log x)/2 in the same units. */
	small.high = LIBM_HALF_LOG_TWO_PI_HIGH - 0.5 * logarithm.high;
	small.low = LIBM_HALF_LOG_TWO_PI_LOW - 0.5 * logarithm.low;
	small.high *= scale;
	small.low *= scale;
	scaled = libm_dd_add(scaled, small);

	/* Past the largest finite value divided by 2^128 the result overflows. */
	if (scaled.high > 5.2829453113566524e269)
		return __libm_overflow(0U);

	/* Scales back exactly. */
	result = (scaled.high + scaled.low) * 3.4028236692093846e38;
	if (result == HUGE_VAL)
		return __libm_overflow(0U);

	/* Succeeded: the scaled sum rounded once. */
	return result;
}

/*
 * Reports whether a finite x is an integer.
 */
static int
libm_is_integer(
	double x)
{
	double whole;

	/* An integer is its own truncation. */
	whole = trunc(x);
	if (whole == x)
		return 1;

	/* x has a fraction. */
	return 0;
}
