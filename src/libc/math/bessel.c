/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Bessel functions of the first and second kinds (POSIX XSI).
 *
 * Up to |x| = 25 the defining power series are summed in double-double
 * (Abramowitz and Stegun 9.1.10 for J, 9.1.13 and 9.1.11 for Y), which
 * leaves about 70 bits after the cancellation between their terms.
 * Beyond it Hankel's asymptotic expansions (A&S 9.2.5 to 9.2.10) are
 * summed until their terms stop shrinking, with the phase formed from the
 * accurately reduced sin x and cos x.  Integral orders come from the
 * three-term recurrence: upwards for Y and for J below x, where it is
 * stable, and downwards from a high order for J above x (Miller's method),
 * normalized by J0 or J1.
 *
 * These functions are outside the one-ulp goal of WS076: their results
 * are accurate in absolute terms to about 2^-60 times their envelope, so
 * next to a zero the relative error grows.
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/* Up to this |x| the power series are used. */
#define LIBM_BESSEL_SERIES 25.0

/* The largest number of terms of a series or an expansion. */
#define LIBM_BESSEL_TERMS 120

/* 2/pi as a double-double: the high part. */
#define LIBM_TWO_OVER_PI_HIGH 0.6366197723675814

/* 2/pi as a double-double: the low part. */
#define LIBM_TWO_OVER_PI_LOW -3.935735335036497e-17

/* 1/sqrt(2), rounded. */
#define LIBM_SQRT_HALF 0.7071067811865476

/* 1/pi, rounded. */
#define LIBM_ONE_OVER_PI 0.3183098861837907

/* 2^-55: below this ratio of the next series term the first one is enough. */
#define LIBM_SERIES_TINY 2.7755575615628914e-17

static struct libm_dd libm_series_j(int order, double x);
static struct libm_dd libm_series_y0(double x, struct libm_dd j0_value);
static struct libm_dd libm_series_y1(double x, struct libm_dd j1_value);
static void libm_hankel(int order, double x, double *p, double *q);
static double libm_asymptotic(int order, int second_kind, double x);
static int libm_bessel_y_domain(double x, double *result);
static struct libm_dd libm_divide_integer(struct libm_dd value, double divisor);
static int libm_negligible(struct libm_dd term, struct libm_dd sum);

/*
 * Returns the Bessel function of the first kind of order 0.
 */
double
j0(
	double x)
{
	struct libm_dd value;
	double magnitude;
	double result;

	/* A NaN is passed on; J0 is even, and zero at infinity. */
	if (x != x)
		return x + x;
	magnitude = fabs(x);
	if (magnitude == HUGE_VAL)
		return 0.0;

	/* Near the origin the series, beyond it the expansion. */
	if (magnitude <= LIBM_BESSEL_SERIES) {
		value = libm_series_j(0, magnitude);
		return value.high + value.low;
	}

	/* The asymptotic expansion. */
	result = libm_asymptotic(0, 0, magnitude);

	/* Succeeded: J0(|x|). */
	return result;
}

/*
 * Returns the Bessel function of the first kind of order 1.
 */
double
j1(
	double x)
{
	struct libm_dd value;
	double magnitude;
	double result;

	/* A NaN is passed on; J1 is zero at infinity. */
	if (x != x)
		return x + x;
	magnitude = fabs(x);
	if (magnitude == HUGE_VAL)
		return copysign(0.0, x);

	/* Near the origin the series, beyond it the expansion. */
	if (magnitude <= LIBM_BESSEL_SERIES) {
		value = libm_series_j(1, magnitude);
		result = value.high + value.low;
	} else {
		result = libm_asymptotic(1, 0, magnitude);
	}

	/* J1 is odd. */
	if (x < 0.0)
		return -result;

	/* Succeeded: x is positive. */
	return result;
}

/*
 * Returns the Bessel function of the first kind of integral order n.
 */
double
jn(
	int n,
	double x)
{
	double magnitude;
	double previous;
	double current;
	double next;
	double upper;
	double lower;
	double size;
	double normalizer;
	double other;
	double other_size;
	double result;
	int order;
	int start;
	int step;
	int sign;

	/* J(-n) = (-1)^n J(n), and J(n)(-x) = (-1)^n J(n)(x). */
	sign = 1;
	order = n;
	if (order < 0) {
		order = -order;
		if ((order & 1) != 0)
			sign = -sign;
	}

	/* An odd order is odd in x as well. */
	if (x < 0.0 && (order & 1) != 0)
		sign = -sign;

	/* A NaN is passed on; the low orders have their own functions. */
	if (x != x)
		return x + x;
	magnitude = fabs(x);
	if (order == 0)
		return j0(x);
	if (order == 1)
		return sign * j1(magnitude);

	/* Zero and infinity give zero. */
	if (magnitude == 0.0 || magnitude == HUGE_VAL)
		return sign * 0.0;

	/* Below the argument the upward recurrence is stable. */
	if ((double)order < magnitude) {
		previous = j0(magnitude);
		current = j1(magnitude);
		for (step = 1; step < order; step++) {
			next = 2.0 * (double)step / magnitude * current - previous;
			previous = current;
			current = next;
		}

		/* The recurrence has reached the order. */
		return sign * current;
	}

	/*
	 * For a small x the first term of the series, (x/2)^n / n!, is the
	 * whole value to double precision.
	 */
	if (0.25 * magnitude * magnitude < (double)(order + 1) * LIBM_SERIES_TINY) {
		result = pow(0.5 * magnitude, (double)order);
		normalizer = tgamma((double)(order + 1));
		result /= normalizer;
		return sign * result;
	}

	/*
	 * Above the argument the downward recurrence from a high enough order
	 * is stable (Miller): f(k-1) = (2k/x) f(k) - f(k+1) from f(start+1) = 0
	 * and a tiny f(start).  The values are kept inside the range by
	 * rescaling, and the result is normalized by whichever of J0 and J1 is
	 * further from a zero.
	 */
	start = order + 20 + (int)sqrt(40.0 * (double)order);
	if ((double)start < magnitude + 20.0)
		start = (int)magnitude + 20;
	upper = 0.0;
	current = 1e-300;
	result = 0.0;
	for (step = start; step > 0; step--) {
		lower = 2.0 * (double)step / magnitude * current - upper;
		upper = current;
		current = lower;

		/* The order asked for passes by. */
		if (step - 1 == order)
			result = current;

		/* Keeps the values inside the range, the kept result included. */
		size = fabs(current);
		if (size > 1e250) {
			current *= 1e-250;
			upper *= 1e-250;
			result *= 1e-250;
		}
	}

	/* current is f(0) and upper f(1), in the recurrence's units. */
	normalizer = j0(magnitude);
	other = j1(magnitude);
	size = fabs(normalizer);
	other_size = fabs(other);
	if (size > other_size) {
		result = result * (normalizer / current);
	} else {
		result = result * (other / upper);
	}

	/* Succeeded: J(n)(x) with its sign. */
	return sign * result;
}

/*
 * Returns the Bessel function of the second kind of order 0.
 */
double
y0(
	double x)
{
	struct libm_dd first;
	struct libm_dd value;
	double result;
	int special;

	/* Negative arguments, zero, infinity and NaN. */
	special = libm_bessel_y_domain(x, &result);
	if (special)
		return result;

	/* Near the origin the series, beyond it the expansion. */
	if (x <= LIBM_BESSEL_SERIES) {
		first = libm_series_j(0, x);
		value = libm_series_y0(x, first);
		return value.high + value.low;
	}

	/* The asymptotic expansion. */
	result = libm_asymptotic(0, 1, x);

	/* Succeeded: Y0(x). */
	return result;
}

/*
 * Returns the Bessel function of the second kind of order 1.
 */
double
y1(
	double x)
{
	struct libm_dd first;
	struct libm_dd value;
	double result;
	int special;

	/* Negative arguments, zero, infinity and NaN. */
	special = libm_bessel_y_domain(x, &result);
	if (special)
		return result;

	/* Near the origin the series, beyond it the expansion. */
	if (x <= LIBM_BESSEL_SERIES) {
		first = libm_series_j(1, x);
		value = libm_series_y1(x, first);
		return value.high + value.low;
	}

	/* The asymptotic expansion. */
	result = libm_asymptotic(1, 1, x);

	/* Succeeded: Y1(x). */
	return result;
}

/*
 * Returns the Bessel function of the second kind of integral order n.
 */
double
yn(
	int n,
	double x)
{
	double previous;
	double current;
	double next;
	double result;
	int order;
	int step;
	int sign;
	int special;

	/* Y(-n) = (-1)^n Y(n). */
	sign = 1;
	order = n;
	if (order < 0) {
		order = -order;
		if ((order & 1) != 0)
			sign = -1;
	}

	/* Negative arguments, zero, infinity and NaN. */
	special = libm_bessel_y_domain(x, &result);
	if (special)
		return sign * result;

	/* The low orders have their own functions. */
	if (order == 0)
		return y0(x);
	if (order == 1)
		return sign * y1(x);

	/* The upward recurrence is stable for Y; it stops once it overflows. */
	previous = y0(x);
	current = y1(x);
	for (step = 1; step < order; step++) {
		next = 2.0 * step / x * current - previous;
		previous = current;
		current = next;
		if (current == -HUGE_VAL)
			break;
	}

	/* An infinite result is an overflow, of the sign the order gives. */
	if (current == -HUGE_VAL && sign > 0)
		return __libm_overflow(1U);
	if (current == -HUGE_VAL)
		return __libm_overflow(0U);

	/* Succeeded: Y(n)(x) with its sign. */
	return sign * current;
}

/*
 * Returns J0 in binary32.
 */
float
j0f(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = j0((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns J1 in binary32.
 */
float
j1f(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = j1((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns J(n) in binary32.
 */
float
jnf(
	int n,
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = jn(n, (double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns Y0 in binary32.
 */
float
y0f(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = y0((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns Y1 in binary32.
 */
float
y1f(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = y1((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns Y(n) in binary32.
 */
float
ynf(
	int n,
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = yn(n, (double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Sums J(order)(x) = sum (-1)^k (x/2)^(2k+order) / (k! (k+order)!) for
 * order 0 or 1 and 0 <= x <= 25, in double-double.
 *
 * Each term is the last times x^2/4 divided exactly by -k (k + order); the
 * largest term is about 5e8 at x = 25, so the double-double sum keeps
 * about 75 bits of the result.
 */
static struct libm_dd
libm_series_j(
	int order,
	double x)
{
	struct libm_dd quarter_square;
	struct libm_dd term;
	struct libm_dd sum;
	int index;
	int finished;

	/* x^2/4 exactly, and the first term (x/2)^order / order!. */
	quarter_square = libm_two_product(0.5 * x, 0.5 * x);
	term.high = 1.0;
	term.low = 0.0;
	if (order == 1)
		term.high = 0.5 * x;
	sum = term;

	/* Adds terms until they no longer reach the sum's last bits. */
	for (index = 1; index < LIBM_BESSEL_TERMS; index++) {
		term = libm_dd_multiply(term, quarter_square);
		term = libm_divide_integer(term, -(double)index * (double)(index + order));
		sum = libm_dd_add(sum, term);
		finished = libm_negligible(term, sum);
		if (finished)
			break;
	}

	/* Succeeded: the series. */
	return sum;
}

/*
 * Sums Y0(x) = (2/pi) (log(x/2) + gamma) J0(x)
 *   + (2/pi) sum (-1)^(k+1) H(k) (x^2/4)^k / (k!)^2
 * for 0 < x <= 25, in double-double, with the harmonic numbers H(k) in
 * double-double as well.
 */
static struct libm_dd
libm_series_y0(
	double x,
	struct libm_dd j0_value)
{
	struct libm_dd quarter_square;
	struct libm_dd term;
	struct libm_dd sum;
	struct libm_dd logarithm;
	struct libm_dd factor;
	struct libm_dd euler;
	struct libm_dd harmonic;
	struct libm_dd one;
	int index;
	int finished;

	/* The logarithmic part: (log(x/2) + gamma) J0(x). */
	logarithm = __libm_log_dd(0.5 * x);
	euler.high = LIBM_EULER_HIGH;
	euler.low = LIBM_EULER_LOW;
	logarithm = libm_dd_add(logarithm, euler);
	sum = libm_dd_multiply(logarithm, j0_value);

	/* The series with the harmonic numbers, H(k) = H(k-1) + 1/k. */
	quarter_square = libm_two_product(0.5 * x, 0.5 * x);
	term.high = 1.0;
	term.low = 0.0;
	harmonic.high = 0.0;
	harmonic.low = 0.0;
	one.high = 1.0;
	one.low = 0.0;
	for (index = 1; index < LIBM_BESSEL_TERMS; index++) {
		term = libm_dd_multiply(term, quarter_square);
		term = libm_divide_integer(term, -(double)index * (double)index);
		factor = libm_divide_integer(one, (double)index);
		harmonic = libm_dd_add(harmonic, factor);
		factor = libm_dd_multiply(term, harmonic);
		factor.high = -factor.high;
		factor.low = -factor.low;
		sum = libm_dd_add(sum, factor);
		finished = libm_negligible(factor, sum);
		if (finished)
			break;
	}

	/* Times 2/pi. */
	factor.high = LIBM_TWO_OVER_PI_HIGH;
	factor.low = LIBM_TWO_OVER_PI_LOW;
	sum = libm_dd_multiply(sum, factor);

	/* Succeeded: Y0(x). */
	return sum;
}

/*
 * Sums Y1(x) = (2/pi) (log(x/2) + gamma) J1(x) - 2/(pi x)
 *   - (1/pi) sum (-1)^k (H(k) + H(k+1)) (x/2)^(2k+1) / (k! (k+1)!)
 * for 0 < x <= 25, in double-double.
 */
static struct libm_dd
libm_series_y1(
	double x,
	struct libm_dd j1_value)
{
	struct libm_dd quarter_square;
	struct libm_dd term;
	struct libm_dd sum;
	struct libm_dd logarithm;
	struct libm_dd factor;
	struct libm_dd euler;
	struct libm_dd harmonic;
	struct libm_dd next_harmonic;
	struct libm_dd one;
	struct libm_dd inverse;
	int index;
	int finished;

	/* The logarithmic part: (log(x/2) + gamma) J1(x). */
	logarithm = __libm_log_dd(0.5 * x);
	euler.high = LIBM_EULER_HIGH;
	euler.low = LIBM_EULER_LOW;
	logarithm = libm_dd_add(logarithm, euler);
	sum = libm_dd_multiply(logarithm, j1_value);

	/*
	 * The series, halved so that the whole can be multiplied by 2/pi:
	 * -(1/2) sum (-1)^k (H(k) + H(k+1)) (x/2)^(2k+1) / (k! (k+1)!).  The
	 * term for k = 0 is -(1/2)(0 + 1)(x/2).
	 */
	quarter_square = libm_two_product(0.5 * x, 0.5 * x);
	term.high = 0.5 * x;
	term.low = 0.0;
	one.high = 1.0;
	one.low = 0.0;
	harmonic.high = 0.0;
	harmonic.low = 0.0;
	next_harmonic = one;
	factor.high = -0.25 * x;
	factor.low = 0.0;
	sum = libm_dd_add(sum, factor);
	for (index = 1; index < LIBM_BESSEL_TERMS; index++) {
		term = libm_dd_multiply(term, quarter_square);
		term = libm_divide_integer(term, -(double)index * (double)(index + 1));
		harmonic = next_harmonic;
		factor = libm_divide_integer(one, (double)(index + 1));
		next_harmonic = libm_dd_add(next_harmonic, factor);
		factor = libm_dd_add(harmonic, next_harmonic);
		factor = libm_dd_multiply(term, factor);
		factor.high *= -0.5;
		factor.low *= -0.5;
		sum = libm_dd_add(sum, factor);
		finished = libm_negligible(factor, sum);
		if (finished)
			break;
	}

	/* The pole term, -1/x before the factor 2/pi. */
	inverse = libm_divide_integer(one, -x);
	sum = libm_dd_add(sum, inverse);

	/* Times 2/pi. */
	factor.high = LIBM_TWO_OVER_PI_HIGH;
	factor.low = LIBM_TWO_OVER_PI_LOW;
	sum = libm_dd_multiply(sum, factor);

	/* Succeeded: Y1(x). */
	return sum;
}

/*
 * Sums Hankel's P and Q for order 0 or 1 (A&S 9.2.9, 9.2.10) until the
 * terms stop shrinking.
 */
static void
libm_hankel(
	int order,
	double x,
	double *p,
	double *q)
{
	double mu;
	double term;
	double previous;
	double eight_x;
	double size;
	int k;

	/* mu = 4 order^2; the terms are products of (mu - (2j-1)^2) / (j 8x). */
	mu = 4.0 * (double)(order * order);
	eight_x = 8.0 * x;
	*p = 1.0;
	*q = 0.0;
	term = 1.0;
	previous = HUGE_VAL;

	/* Alternates between the next Q term and the next P term. */
	for (k = 1; k < 2 * LIBM_BESSEL_TERMS; k++) {
		term *= (mu - (double)((2 * k - 1) * (2 * k - 1))) /
		    ((double)k * eight_x);
		size = fabs(term);
		if (size >= previous)
			break;

		/* Odd k feeds Q, even k feeds P, each with the alternating sign. */
		previous = size;
		if ((k & 1) != 0) {
			if ((k & 3) == 1) {
				*q += term;
			} else {
				*q -= term;
			}
		} else {
			if ((k & 3) == 2) {
				*p -= term;
			} else {
				*p += term;
			}
		}

		/* A term below the last bit of P ends the sum. */
		if (size < 1e-18)
			break;
	}
}

/*
 * Returns J or Y of order 0 or 1 at a large x from Hankel's expansion:
 *   J = sqrt(2/(pi x)) (P cos(chi) - Q sin(chi)),
 *   Y = sqrt(2/(pi x)) (P sin(chi) + Q cos(chi)),
 * with chi = x - (2 order + 1) pi/4.
 */
static double
libm_asymptotic(
	int order,
	int second_kind,
	double x)
{
	double p;
	double q;
	double sine;
	double cosine;
	double phase_cosine;
	double phase_sine;
	double amplitude;
	double result;

	/* The expansions in 1/x. */
	libm_hankel(order, x, &p, &q);

	/*
	 * cos(x - pi/4) = (cos x + sin x)/sqrt 2 and
	 * sin(x - pi/4) = (sin x - cos x)/sqrt 2; order 1 turns a further
	 * quarter, giving (sin x - cos x) and -(cos x + sin x).
	 */
	sine = sin(x);
	cosine = cos(x);
	phase_cosine = (cosine + sine) * LIBM_SQRT_HALF;
	phase_sine = (sine - cosine) * LIBM_SQRT_HALF;
	if (order == 1) {
		result = phase_cosine;
		phase_cosine = phase_sine;
		phase_sine = -result;
	}

	/* The amplitude sqrt(2/(pi x)). */
	amplitude = sqrt(2.0 * LIBM_ONE_OVER_PI / x);

	/* Combines the two parts for the kind asked for. */
	if (second_kind) {
		result = amplitude * (p * phase_sine + q * phase_cosine);
	} else {
		result = amplitude * (p * phase_cosine - q * phase_sine);
	}

	/* Succeeded: the asymptotic value. */
	return result;
}

/*
 * Handles the arguments of Y outside its domain: returns 1 and stores the
 * result for NaN, negative values, zero and infinity, and 0 otherwise.
 */
static int
libm_bessel_y_domain(
	double x,
	double *result)
{
	/* A NaN is passed on. */
	if (x != x) {
		*result = x + x;
		return 1;
	}

	/* Negative arguments have no value; zero is a pole at -infinity. */
	if (x < 0.0) {
		*result = __libm_invalid();
		return 1;
	}

	/* Zero is a pole at -infinity. */
	if (x == 0.0) {
		*result = __libm_pole(1U);
		return 1;
	}

	/* Y is zero at infinity. */
	if (x == HUGE_VAL) {
		*result = 0.0;
		return 1;
	}

	/* Succeeded: x is inside the domain. */
	return 0;
}

/*
 * Divides a double-double by an exact double, such as an integer.
 */
static struct libm_dd
libm_divide_integer(
	struct libm_dd value,
	double divisor)
{
	struct libm_dd denominator;
	struct libm_dd quotient;

	/* The double-double division is accurate to about 2^-104. */
	denominator.high = divisor;
	denominator.low = 0.0;
	quotient = libm_dd_divide(value, denominator);

	/* Succeeded: the quotient. */
	return quotient;
}

/*
 * Reports whether a term no longer reaches the last bits of a sum.
 */
static int
libm_negligible(
	struct libm_dd term,
	struct libm_dd sum)
{
	double term_size;
	double sum_size;

	/* Compares magnitudes, with a floor for a sum that is zero. */
	term_size = fabs(term.high);
	sum_size = fabs(sum.high);
	if (term_size < 1e-40 * sum_size + 1e-300)
		return 1;

	/* The term still matters. */
	return 0;
}
