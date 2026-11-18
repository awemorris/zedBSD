/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Classification, sign, exponent and neighbour functions.
 *
 * Every function here is exact: it reads or rebuilds the encoding and
 * never rounds, except where scaling leaves the range and the packing
 * helper rounds once.
 */

#include <errno.h>
#include <fenv.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/* The encoding of the default quiet binary32 NaN. */
#define LIBM_FLOAT_QUIET_NAN UINT32_C(0x7fc00000)

/* The encoding of the default quiet binary64 NaN. */
#define LIBM_DOUBLE_QUIET_NAN UINT64_C(0x7ff8000000000000)

/*
 * The largest scaling distance that can matter to a binary64 value.
 *
 * Any finite nonzero value scaled by 2^5000 overflows and by 2^-5000
 * underflows, so larger distances are clamped to it before they are added
 * to an exponent.
 */
#define LIBM_SCALE_LIMIT 5000

static double libm_step(double value, int upward);
static float libm_step_float(float value, int upward);

/*
 * Classifies a binary64 value.
 */
int
__fpclassify(
	double value)
{
	uint64_t magnitude;

	/* Orders the encodings from the top: NaN, infinity, zero, subnormal. */
	magnitude = libm_bits(value) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude > LIBM_DOUBLE_INFINITY)
		return FP_NAN;
	if (magnitude == LIBM_DOUBLE_INFINITY)
		return FP_INFINITE;
	if (magnitude == 0U)
		return FP_ZERO;
	if (magnitude < LIBM_DOUBLE_HIDDEN)
		return FP_SUBNORMAL;

	/* Everything else has a biased exponent inside the normal range. */
	return FP_NORMAL;
}

/*
 * Classifies a binary32 value.
 */
int
__fpclassifyf(
	float value)
{
	uint32_t magnitude;

	/* Orders the encodings from the top: NaN, infinity, zero, subnormal. */
	magnitude = libm_float_bits(value) & LIBM_FLOAT_MAGNITUDE;
	if (magnitude > LIBM_FLOAT_INFINITY)
		return FP_NAN;
	if (magnitude == LIBM_FLOAT_INFINITY)
		return FP_INFINITE;
	if (magnitude == 0U)
		return FP_ZERO;
	if (magnitude < LIBM_FLOAT_HIDDEN)
		return FP_SUBNORMAL;

	/* Everything else has a biased exponent inside the normal range. */
	return FP_NORMAL;
}

/*
 * Reports whether the sign bit of a binary64 value is set.
 */
int
__signbit(
	double value)
{
	/* The sign is the top bit of the encoding, NaNs and zeros included. */
	return (int)(libm_bits(value) >> 63);
}

/*
 * Reports whether the sign bit of a binary32 value is set.
 */
int
__signbitf(
	float value)
{
	/* The sign is the top bit of the encoding, NaNs and zeros included. */
	return (int)(libm_float_bits(value) >> 31);
}

/*
 * Returns the magnitude of a binary64 value.
 */
double
fabs(
	double x)
{
	/* Clearing the sign bit is exact for every encoding. */
	return libm_from_bits(libm_bits(x) & LIBM_DOUBLE_MAGNITUDE);
}

/*
 * Returns the magnitude of a binary32 value.
 */
float
fabsf(
	float x)
{
	/* Clearing the sign bit is exact for every encoding. */
	return libm_float_from_bits(libm_float_bits(x) & LIBM_FLOAT_MAGNITUDE);
}

/*
 * Returns the magnitude of one binary64 value with the sign of another.
 */
double
copysign(
	double x,
	double y)
{
	uint64_t magnitude;
	uint64_t sign;

	/* Takes the magnitude from x and only the sign bit from y. */
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	sign = libm_bits(y) & LIBM_DOUBLE_SIGN;

	/* The combined encoding is the result. */
	return libm_from_bits(magnitude | sign);
}

/*
 * Returns the magnitude of one binary32 value with the sign of another.
 */
float
copysignf(
	float x,
	float y)
{
	uint32_t magnitude;
	uint32_t sign;

	/* Takes the magnitude from x and only the sign bit from y. */
	magnitude = libm_float_bits(x) & LIBM_FLOAT_MAGNITUDE;
	sign = libm_float_bits(y) & LIBM_FLOAT_SIGN;

	/* The combined encoding is the result. */
	return libm_float_from_bits(magnitude | sign);
}

/*
 * Returns a quiet binary64 NaN.
 *
 * The tag selects no payload; every tag gives the default NaN.
 */
double
nan(
	const char *tag)
{
	(void)tag;

	/* The default quiet NaN serves every tag. */
	return libm_from_bits(LIBM_DOUBLE_QUIET_NAN);
}

/*
 * Returns a quiet binary32 NaN.
 */
float
nanf(
	const char *tag)
{
	(void)tag;

	/* The default quiet NaN serves every tag. */
	return libm_float_from_bits(LIBM_FLOAT_QUIET_NAN);
}

/*
 * Returns the next binary64 value after x in the direction of y.
 */
double
nextafter(
	double x,
	double y)
{
	double neighbour;

	/* A NaN in either argument, unequal to itself, is passed on. */
	if (x != x || y != y)
		return x + y;

	/* Equal arguments return y, which carries the sign of a zero y. */
	if (x == y)
		return y;

	/* Steps once towards y. */
	if (x < y)
		return libm_step(x, 1);

	/* y lies below x, so the step goes downwards. */
	neighbour = libm_step(x, 0);

	/* Succeeded: the neighbour below x. */
	return neighbour;
}

/*
 * Returns the next binary32 value after x in the direction of y.
 */
float
nextafterf(
	float x,
	float y)
{
	float neighbour;

	/* A NaN in either argument, unequal to itself, is passed on. */
	if (x != x || y != y)
		return x + y;

	/* Equal arguments return y, which carries the sign of a zero y. */
	if (x == y)
		return y;

	/* Steps once towards y. */
	if (x < y)
		return libm_step_float(x, 1);

	/* y lies below x, so the step goes downwards. */
	neighbour = libm_step_float(x, 0);

	/* Succeeded: the neighbour below x. */
	return neighbour;
}

/*
 * Returns the next binary64 value after x in the direction of a long
 * double y.
 */
double
nexttoward(
	double x,
	long double y)
{
	double neighbour;

	/* A NaN in either argument is passed on. */
	if (x != x)
		return x + x;
	if (y != y)
		return (double)y;

	/* Equal arguments return y. */
	if ((long double)x == y)
		return (double)y;

	/* Steps once towards y, compared in the wider type. */
	if ((long double)x < y)
		return libm_step(x, 1);

	/* y lies below x, so the step goes downwards. */
	neighbour = libm_step(x, 0);

	/* Succeeded: the neighbour below x. */
	return neighbour;
}

/*
 * Returns the next binary32 value after x in the direction of a long
 * double y.
 */
float
nexttowardf(
	float x,
	long double y)
{
	float neighbour;

	/* A NaN in either argument is passed on. */
	if (x != x)
		return x + x;
	if (y != y)
		return (float)y;

	/* Equal arguments return y. */
	if ((long double)x == y)
		return (float)y;

	/* Steps once towards y, compared in the wider type. */
	if ((long double)x < y)
		return libm_step_float(x, 1);

	/* y lies below x, so the step goes downwards. */
	neighbour = libm_step_float(x, 0);

	/* Succeeded: the neighbour below x. */
	return neighbour;
}

/*
 * Returns the smaller of two binary64 values, ignoring a single NaN.
 */
double
fmin(
	double x,
	double y)
{
	int negative;

	/* One NaN argument yields the other argument. */
	if (x != x)
		return y;
	if (y != y)
		return x;

	/* Of two equal values, zeros of either sign, the negative one is the smaller. */
	negative = (int)(libm_bits(x) >> 63);
	if (x == y) {
		if (negative)
			return x;

		/* x is +0, or equal to y in value and sign. */
		return y;
	}

	/* Otherwise the ordinary comparison decides. */
	if (x < y)
		return x;

	/* Succeeded: y is the smaller. */
	return y;
}

/*
 * Returns the smaller of two binary32 values, ignoring a single NaN.
 */
float
fminf(
	float x,
	float y)
{
	int negative;

	/* One NaN argument yields the other argument. */
	if (x != x)
		return y;
	if (y != y)
		return x;

	/* Of two equal values, zeros of either sign, the negative one is the smaller. */
	negative = (int)(libm_float_bits(x) >> 31);
	if (x == y) {
		if (negative)
			return x;

		/* x is +0, or equal to y in value and sign. */
		return y;
	}

	/* Otherwise the ordinary comparison decides. */
	if (x < y)
		return x;

	/* Succeeded: y is the smaller. */
	return y;
}

/*
 * Returns the larger of two binary64 values, ignoring a single NaN.
 */
double
fmax(
	double x,
	double y)
{
	int negative;

	/* One NaN argument yields the other argument. */
	if (x != x)
		return y;
	if (y != y)
		return x;

	/* Of two equal values, zeros of either sign, the positive one is the larger. */
	negative = (int)(libm_bits(x) >> 63);
	if (x == y) {
		if (negative)
			return y;

		/* x is +0, or equal to y in value and sign. */
		return x;
	}

	/* Otherwise the ordinary comparison decides. */
	if (x > y)
		return x;

	/* Succeeded: y is the larger. */
	return y;
}

/*
 * Returns the larger of two binary32 values, ignoring a single NaN.
 */
float
fmaxf(
	float x,
	float y)
{
	int negative;

	/* One NaN argument yields the other argument. */
	if (x != x)
		return y;
	if (y != y)
		return x;

	/* Of two equal values, zeros of either sign, the positive one is the larger. */
	negative = (int)(libm_float_bits(x) >> 31);
	if (x == y) {
		if (negative)
			return y;

		/* x is +0, or equal to y in value and sign. */
		return x;
	}

	/* Otherwise the ordinary comparison decides. */
	if (x > y)
		return x;

	/* Succeeded: y is the larger. */
	return y;
}

/*
 * Returns the positive difference of two binary64 values.
 */
double
fdim(
	double x,
	double y)
{
	double difference;

	/* A NaN in either argument, unequal to itself, is passed on. */
	if (x != x || y != y)
		return x + y;

	/* A difference that is not positive is +0. */
	if (x <= y)
		return 0.0;

	/* A subtraction of finite values that became infinite overflowed. */
	difference = x - y;
	if (difference == HUGE_VAL && x != HUGE_VAL && y != -HUGE_VAL)
		return __libm_overflow(0U);

	/* Succeeded: the rounded difference is the result. */
	return difference;
}

/*
 * Returns the positive difference of two binary32 values.
 */
float
fdimf(
	float x,
	float y)
{
	double difference;
	float narrowed;

	/* A NaN in either argument, unequal to itself, is passed on. */
	if (x != x || y != y)
		return x + y;

	/* A difference that is not positive is +0. */
	if (x <= y)
		return 0.0f;

	/* The double difference is exact and rounds to float once. */
	difference = (double)x - (double)y;

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(difference);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Returns the unbiased exponent of a binary64 value as an int.
 */
int
ilogb(
	double x)
{
	struct libm_unpacked unpacked;
	uint64_t magnitude;

	/* Zero, infinity and NaN have no exponent and are invalid. */
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude == 0U) {
		(void)__libm_invalid();
		return FP_ILOGB0;
	}

	/* A NaN reports the NaN code. */
	if (magnitude > LIBM_DOUBLE_INFINITY) {
		(void)__libm_invalid();
		return FP_ILOGBNAN;
	}

	/* An infinity reports the largest int. */
	if (magnitude == LIBM_DOUBLE_INFINITY) {
		(void)__libm_invalid();
		return INT_MAX;
	}

	/* The exponent of the normalized significand's leading bit. */
	__libm_unpack(x, &unpacked);

	/* Succeeded: subnormals report their true exponent. */
	return unpacked.exponent + 52;
}

/*
 * Returns the unbiased exponent of a binary32 value as an int.
 */
int
ilogbf(
	float x)
{
	int exponent;

	/* Every float is a double with the same exponent. */
	exponent = ilogb((double)x);

	/* Succeeded: the exponent carries over unchanged. */
	return exponent;
}

/*
 * Returns the unbiased exponent of a binary64 value as a double.
 */
double
logb(
	double x)
{
	struct libm_unpacked unpacked;
	uint64_t magnitude;

	/* A NaN is passed on, and an infinity has an infinite exponent. */
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude > LIBM_DOUBLE_INFINITY)
		return x + x;
	if (magnitude == LIBM_DOUBLE_INFINITY)
		return HUGE_VAL;

	/* The exponent of zero is a pole at minus infinity. */
	if (x == 0.0)
		return __libm_pole(1U);

	/* The exponent of the normalized significand's leading bit. */
	__libm_unpack(x, &unpacked);

	/* Succeeded: the integer exponent is exact in double. */
	return (double)(unpacked.exponent + 52);
}

/*
 * Returns the unbiased exponent of a binary32 value as a float.
 */
float
logbf(
	float x)
{
	double exponent;

	/* Every float is a double with the same exponent. */
	exponent = logb((double)x);

	/* Succeeded: the exponent is exact in float. */
	return (float)exponent;
}

/*
 * Splits a binary64 value into a fraction in [0.5, 1) and a power of two.
 */
double
frexp(
	double x,
	int *exponent)
{
	struct libm_unpacked unpacked;
	uint64_t magnitude;
	uint64_t bits;

	/* Zero, infinity and NaN are returned with a zero exponent. */
	*exponent = 0;
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude == 0U || magnitude >= LIBM_DOUBLE_INFINITY)
		return x;

	/* The normalized significand fixes the exponent of the leading bit. */
	__libm_unpack(x, &unpacked);
	*exponent = unpacked.exponent + 53;

	/* The fraction keeps the significand and takes the exponent of 0.5. */
	bits = ((uint64_t)unpacked.sign << 63) |
	    ((uint64_t)(LIBM_DOUBLE_BIAS - 1) << 52) |
	    (unpacked.significand & LIBM_DOUBLE_FRACTION);

	/* Succeeded: fraction * 2^exponent equals x exactly. */
	return libm_from_bits(bits);
}

/*
 * Splits a binary32 value into a fraction in [0.5, 1) and a power of two.
 */
float
frexpf(
	float x,
	int *exponent)
{
	double fraction;

	/* The split of the equal double is exact in float as well. */
	fraction = frexp((double)x, exponent);

	/* Succeeded: the fraction has at most 24 significant bits. */
	return (float)fraction;
}

/*
 * Multiplies a binary64 value by an integral power of two.
 */
double
scalbn(
	double x,
	int n)
{
	struct libm_unpacked unpacked;
	int distance;
	double result;
	uint64_t magnitude;

	/* Zero, infinity and NaN are unchanged by scaling. */
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude == 0U || magnitude >= LIBM_DOUBLE_INFINITY)
		return x;

	/* Distances past any possible range are clamped so the sum below fits. */
	distance = n;
	if (distance > LIBM_SCALE_LIMIT)
		distance = LIBM_SCALE_LIMIT;
	if (distance < -LIBM_SCALE_LIMIT)
		distance = -LIBM_SCALE_LIMIT;

	/*
	 * Moves the exponent and lets the packing helper round a subnormal
	 * result and report overflow or underflow.
	 */
	__libm_unpack(x, &unpacked);
	result = __libm_pack(unpacked.sign, unpacked.exponent + 52 + distance,
	    unpacked.significand << 11);

	/* Succeeded: the scaled value is the result. */
	return result;
}

/*
 * Multiplies a binary32 value by an integral power of two.
 */
float
scalbnf(
	float x,
	int n)
{
	int distance;
	double scaled;
	uint32_t magnitude;
	float narrowed;

	/* Zero, infinity and NaN are unchanged by scaling. */
	magnitude = libm_float_bits(x) & LIBM_FLOAT_MAGNITUDE;
	if (magnitude == 0U || magnitude >= LIBM_FLOAT_INFINITY)
		return x;

	/*
	 * Within 400 binades the scaled float stays a normal double, so the
	 * double product is exact and the narrowing below rounds once.  Past
	 * that distance every float overflows or underflows anyway.
	 */
	distance = n;
	if (distance > 400)
		distance = 400;
	if (distance < -400)
		distance = -400;
	scaled = scalbn((double)x, distance);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(scaled);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Multiplies a binary64 value by an integral power of two.
 */
double
ldexp(
	double x,
	int n)
{
	double result;

	/* ldexp and scalbn are the same operation for a binary radix. */
	result = scalbn(x, n);

	/* Succeeded: the scaled value is the result. */
	return result;
}

/*
 * Multiplies a binary32 value by an integral power of two.
 */
float
ldexpf(
	float x,
	int n)
{
	float result;

	/* ldexpf and scalbnf are the same operation for a binary radix. */
	result = scalbnf(x, n);

	/* Succeeded: the scaled value is the result. */
	return result;
}

/*
 * Multiplies a binary64 value by a power of two given as a long.
 */
double
scalbln(
	double x,
	long n)
{
	int distance;
	double result;

	/* Clamps the distance into int; the clamp is past every range. */
	distance = LIBM_SCALE_LIMIT;
	if (n < LIBM_SCALE_LIMIT)
		distance = (int)n;
	if (n < -LIBM_SCALE_LIMIT)
		distance = -LIBM_SCALE_LIMIT;

	/* Scales by the clamped distance. */
	result = scalbn(x, distance);

	/* Succeeded: the scaled value is the result. */
	return result;
}

/*
 * Multiplies a binary32 value by a power of two given as a long.
 */
float
scalblnf(
	float x,
	long n)
{
	int distance;
	float result;

	/* Clamps the distance into int; the clamp is past every range. */
	distance = LIBM_SCALE_LIMIT;
	if (n < LIBM_SCALE_LIMIT)
		distance = (int)n;
	if (n < -LIBM_SCALE_LIMIT)
		distance = -LIBM_SCALE_LIMIT;

	/* Scales by the clamped distance. */
	result = scalbnf(x, distance);

	/* Succeeded: the scaled value is the result. */
	return result;
}

/*
 * Splits a binary64 value into integral and fractional parts.
 */
double
modf(
	double x,
	double *integral)
{
	double whole;
	double fraction;
	uint64_t magnitude;

	/* A NaN is both parts of itself. */
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude > LIBM_DOUBLE_INFINITY) {
		*integral = x;
		return x + x;
	}

	/* An infinity is integral and has a zero fraction of its sign. */
	if (magnitude == LIBM_DOUBLE_INFINITY) {
		*integral = x;
		return copysign(0.0, x);
	}

	/* The truncation is the integral part, and the difference is exact. */
	whole = trunc(x);
	*integral = whole;
	fraction = x - whole;

	/* Succeeded: the fraction keeps the sign of x, even when it is zero. */
	return copysign(fraction, x);
}

/*
 * Splits a binary32 value into integral and fractional parts.
 */
float
modff(
	float x,
	float *integral)
{
	float whole;
	float fraction;
	uint32_t magnitude;

	/* A NaN is both parts of itself. */
	magnitude = libm_float_bits(x) & LIBM_FLOAT_MAGNITUDE;
	if (magnitude > LIBM_FLOAT_INFINITY) {
		*integral = x;
		return x + x;
	}

	/* An infinity is integral and has a zero fraction of its sign. */
	if (magnitude == LIBM_FLOAT_INFINITY) {
		*integral = x;
		return copysignf(0.0f, x);
	}

	/* The truncation is the integral part, and the difference is exact. */
	whole = truncf(x);
	*integral = whole;
	fraction = x - whole;

	/* Succeeded: the fraction keeps the sign of x, even when it is zero. */
	return copysignf(fraction, x);
}

/*
 * Returns the binary64 neighbour of a value that is not a NaN.
 *
 * Stepping away from zero adds one to the encoding of the magnitude and
 * stepping towards zero subtracts one; the encoding is ordered like the
 * magnitudes, so this crosses binades and reaches infinity by itself.
 */
static double
libm_step(
	double value,
	int upward)
{
	uint64_t bits;
	uint64_t magnitude;
	double result;

	/* From zero the step reaches the smallest subnormal of the direction. */
	bits = libm_bits(value);
	magnitude = bits & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude == 0U) {
		result = libm_from_bits(1U);
		if (!upward)
			result = -result;

		/* The smallest subnormal is a tiny inexact result. */
		return __libm_check_underflow(result);
	}

	/* Moves the magnitude away from zero or towards it. */
	if (upward == ((bits >> 63) == 0U)) {
		bits++;
	} else {
		bits--;
	}

	/* The neighbour is the value of the moved encoding. */
	result = libm_from_bits(bits);

	/* Stepping off the largest finite value overflows. */
	magnitude = bits & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude == LIBM_DOUBLE_INFINITY)
		return __libm_overflow((unsigned int)(bits >> 63));

	/* A subnormal or zero neighbour is reported as an underflow. */
	if (magnitude < LIBM_DOUBLE_HIDDEN)
		return __libm_check_underflow(result);

	/* Succeeded: the neighbour is a normal value. */
	return result;
}

/*
 * Returns the binary32 neighbour of a value that is not a NaN.
 */
static float
libm_step_float(
	float value,
	int upward)
{
	uint32_t bits;
	uint32_t magnitude;
	float result;

	/* From zero the step reaches the smallest subnormal of the direction. */
	bits = libm_float_bits(value);
	magnitude = bits & LIBM_FLOAT_MAGNITUDE;
	if (magnitude == 0U) {
		result = libm_float_from_bits(1U);
		if (!upward)
			result = -result;

		/* The smallest subnormal is a tiny inexact result. */
		errno = ERANGE;
		(void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
		return result;
	}

	/* Moves the magnitude away from zero or towards it. */
	if (upward == ((bits >> 31) == 0U)) {
		bits++;
	} else {
		bits--;
	}

	/* The neighbour is the value of the moved encoding. */
	result = libm_float_from_bits(bits);

	/* Stepping off the largest finite value overflows. */
	magnitude = bits & LIBM_FLOAT_MAGNITUDE;
	if (magnitude == LIBM_FLOAT_INFINITY) {
		errno = ERANGE;
		(void)feraiseexcept(FE_OVERFLOW | FE_INEXACT);
		return result;
	}

	/* A subnormal or zero neighbour is reported as an underflow. */
	if (magnitude < LIBM_FLOAT_HIDDEN) {
		errno = ERANGE;
		(void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
		return result;
	}

	/* Succeeded: the neighbour is a normal value. */
	return result;
}
