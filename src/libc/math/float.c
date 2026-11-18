/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The binary32 versions of the functions that are computed in double.
 *
 * Every float is exactly a double.  The exact operations here give a
 * double result that float can hold, so converting it back is exact.
 */

#include <math.h>

/*
 * Rounds a binary32 value towards zero to an integral value.
 */
float
truncf(
	float x)
{
	double result;

	/* Truncates the equal double. */
	result = trunc((double)x);

	/* Succeeded: the integral value is exact in float. */
	return (float)result;
}

/*
 * Rounds a binary32 value downwards to an integral value.
 */
float
floorf(
	float x)
{
	double result;

	/* Rounds the equal double down. */
	result = floor((double)x);

	/* Succeeded: the integral value is exact in float. */
	return (float)result;
}

/*
 * Rounds a binary32 value upwards to an integral value.
 */
float
ceilf(
	float x)
{
	double result;

	/* Rounds the equal double up. */
	result = ceil((double)x);

	/* Succeeded: the integral value is exact in float. */
	return (float)result;
}

/*
 * Rounds a binary32 value to the nearest integral value, ties away from zero.
 */
float
roundf(
	float x)
{
	double result;

	/* Rounds the equal double. */
	result = round((double)x);

	/* Succeeded: the integral value is exact in float. */
	return (float)result;
}

/*
 * Rounds a binary32 value to the nearest integral value, ties to even.
 */
float
rintf(
	float x)
{
	double result;

	/* Rounds the equal double, raising inexact when it changes. */
	result = rint((double)x);

	/* Succeeded: the integral value is exact in float. */
	return (float)result;
}

/*
 * Rounds a binary32 value to the nearest integral value without raising
 * inexact.
 */
float
nearbyintf(
	float x)
{
	double result;

	/* Rounds the equal double quietly. */
	result = nearbyint((double)x);

	/* Succeeded: the integral value is exact in float. */
	return (float)result;
}

/*
 * Rounds a binary32 value to the nearest long, ties to even.
 */
long
lrintf(
	float x)
{
	long result;

	/* The equal double rounds to the same integer. */
	result = lrint((double)x);

	/* Succeeded: the integer is the result. */
	return result;
}

/*
 * Rounds a binary32 value to the nearest long long, ties to even.
 */
long long
llrintf(
	float x)
{
	long long result;

	/* The equal double rounds to the same integer. */
	result = llrint((double)x);

	/* Succeeded: the integer is the result. */
	return result;
}

/*
 * Rounds a binary32 value to the nearest long, ties away from zero.
 */
long
lroundf(
	float x)
{
	long result;

	/* The equal double rounds to the same integer. */
	result = lround((double)x);

	/* Succeeded: the integer is the result. */
	return result;
}

/*
 * Rounds a binary32 value to the nearest long long, ties away from zero.
 */
long long
llroundf(
	float x)
{
	long long result;

	/* The equal double rounds to the same integer. */
	result = llround((double)x);

	/* Succeeded: the integer is the result. */
	return result;
}

/*
 * Returns the remainder of x / y with the quotient truncated towards zero.
 */
float
fmodf(
	float x,
	float y)
{
	double result;

	/* The exact double remainder of floats is a float. */
	result = fmod((double)x, (double)y);

	/* Succeeded: the remainder is exact in float. */
	return (float)result;
}

/*
 * Returns the remainder of x / y with the quotient rounded to nearest even.
 */
float
remainderf(
	float x,
	float y)
{
	double result;

	/* The exact double remainder of floats is a float. */
	result = remainder((double)x, (double)y);

	/* Succeeded: the remainder is exact in float. */
	return (float)result;
}

/*
 * Returns the nearest remainder of x / y and the low bits of its quotient.
 */
float
remquof(
	float x,
	float y,
	int *quotient)
{
	double result;

	/* The exact double remainder of floats is a float. */
	result = remquo((double)x, (double)y, quotient);

	/* Succeeded: the remainder is exact in float. */
	return (float)result;
}
