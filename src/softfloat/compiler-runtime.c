/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compiler ABI wrappers for the zedBSD IEEE 754 core.
 *
 * A compiler that finds no hardware floating point emits calls to these
 * names.  Each one converts its arguments to their object representations,
 * hands them to the integer-only core, and converts the result back, so that
 * no operation here can recurse into the compiler helpers.
 *
 * The names and their return conventions are fixed by the ABI, not chosen
 * here.  In particular the comparison helpers report an ordering, and each
 * one names the value it must report when the operands do not compare.
 */

#include <stdint.h>

#include "src/softfloat/zed-softfloat.h"

static uint32_t float_bits(float value);
static float bits_float(uint32_t bits);
static uint64_t double_bits(double value);
static double bits_double(uint64_t bits);
static int compare32(float left, float right, int unordered_value);
static int compare64(double left, double right, int unordered_value);

/*
 * Adds two binary32 values.
 */
float
__addsf3(
	float left,
	float right)
{
	/* Reports the sum. */
	return bits_float(zsf32_add(float_bits(left), float_bits(right)));
}

/*
 * Subtracts one binary32 value from another.
 */
float
__subsf3(
	float left,
	float right)
{
	/* Reports the difference. */
	return bits_float(zsf32_sub(float_bits(left), float_bits(right)));
}

/*
 * Multiplies two binary32 values.
 */
float
__mulsf3(
	float left,
	float right)
{
	/* Reports the product. */
	return bits_float(zsf32_mul(float_bits(left), float_bits(right)));
}

/*
 * Divides one binary32 value by another.
 */
float
__divsf3(
	float left,
	float right)
{
	/* Reports the quotient. */
	return bits_float(zsf32_div(float_bits(left), float_bits(right)));
}

/*
 * Adds two binary64 values.
 */
double
__adddf3(
	double left,
	double right)
{
	/* Reports the sum. */
	return bits_double(zsf64_add(double_bits(left), double_bits(right)));
}

/*
 * Subtracts one binary64 value from another.
 */
double
__subdf3(
	double left,
	double right)
{
	/* Reports the difference. */
	return bits_double(zsf64_sub(double_bits(left), double_bits(right)));
}

/*
 * Multiplies two binary64 values.
 */
double
__muldf3(
	double left,
	double right)
{
	/* Reports the product. */
	return bits_double(zsf64_mul(double_bits(left), double_bits(right)));
}

/*
 * Divides one binary64 value by another.
 */
double
__divdf3(
	double left,
	double right)
{
	/* Reports the quotient. */
	return bits_double(zsf64_div(double_bits(left), double_bits(right)));
}

/*
 * Widens a binary32 value to binary64.
 */
double
__extendsfdf2(
	float value)
{
	/* Every binary32 value is exact in binary64. */
	return bits_double(zsf32_to_64(float_bits(value)));
}

/*
 * Narrows a binary64 value to binary32.
 */
float
__truncdfsf2(
	double value)
{
	/* The narrowing rounds, and may overflow or lose precision. */
	return bits_float(zsf64_to_32(double_bits(value)));
}

/*
 * Converts a signed int to binary32.
 */
float
__floatsisf(
	int value)
{
	/* Reports the converted value. */
	return bits_float(zsf_i64_to_32(value));
}

/*
 * Converts a signed 64-bit integer to binary32.
 */
float
__floatdisf(
	int64_t value)
{
	/* Reports the converted value. */
	return bits_float(zsf_i64_to_32(value));
}

/*
 * Converts an unsigned int to binary32.
 */
float
__floatunsisf(
	unsigned int value)
{
	/* Reports the converted value. */
	return bits_float(zsf_u64_to_32(value));
}

/*
 * Converts an unsigned 64-bit integer to binary32.
 */
float
__floatundisf(
	uint64_t value)
{
	/* Reports the converted value. */
	return bits_float(zsf_u64_to_32(value));
}

/*
 * Converts a signed int to binary64.
 */
double
__floatsidf(
	int value)
{
	/* Reports the converted value. */
	return bits_double(zsf_i64_to_64(value));
}

/*
 * Converts a signed 64-bit integer to binary64.
 */
double
__floatdidf(
	int64_t value)
{
	/* Reports the converted value. */
	return bits_double(zsf_i64_to_64(value));
}

/*
 * Converts an unsigned int to binary64.
 */
double
__floatunsidf(
	unsigned int value)
{
	/* Reports the converted value. */
	return bits_double(zsf_u64_to_64(value));
}

/*
 * Converts an unsigned 64-bit integer to binary64.
 */
double
__floatundidf(
	uint64_t value)
{
	/* Reports the converted value. */
	return bits_double(zsf_u64_to_64(value));
}

/*
 * Converts a binary32 value to a signed int.
 */
int
__fixsfsi(
	float value)
{
	/* The conversion truncates towards zero and saturates. */
	return (int)zsf32_to_i64(float_bits(value), 32U, 0);
}

/*
 * Converts a binary32 value to a signed 64-bit integer.
 */
int64_t
__fixsfdi(
	float value)
{
	/* The conversion truncates towards zero and saturates. */
	return zsf32_to_i64(float_bits(value), 64U, 0);
}

/*
 * Converts a binary32 value to an unsigned int.
 */
unsigned int
__fixunssfsi(
	float value)
{
	/* The conversion truncates towards zero and saturates. */
	return (unsigned int)zsf32_to_i64(float_bits(value), 32U, 1);
}

/*
 * Converts a binary32 value to an unsigned 64-bit integer.
 */
uint64_t
__fixunssfdi(
	float value)
{
	/* The conversion truncates towards zero and saturates. */
	return (uint64_t)zsf32_to_i64(float_bits(value), 64U, 1);
}

/*
 * Converts a binary64 value to a signed int.
 */
int
__fixdfsi(
	double value)
{
	/* The conversion truncates towards zero and saturates. */
	return (int)zsf64_to_i64(double_bits(value), 32U, 0);
}

/*
 * Converts a binary64 value to a signed 64-bit integer.
 */
int64_t
__fixdfdi(
	double value)
{
	/* The conversion truncates towards zero and saturates. */
	return zsf64_to_i64(double_bits(value), 64U, 0);
}

/*
 * Converts a binary64 value to an unsigned int.
 */
unsigned int
__fixunsdfsi(
	double value)
{
	/* The conversion truncates towards zero and saturates. */
	return (unsigned int)zsf64_to_i64(double_bits(value), 32U, 1);
}

/*
 * Converts a binary64 value to an unsigned 64-bit integer.
 */
uint64_t
__fixunsdfdi(
	double value)
{
	/* The conversion truncates towards zero and saturates. */
	return (uint64_t)zsf64_to_i64(double_bits(value), 64U, 1);
}

/*
 * Reports whether two binary32 values are unequal.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__eqsf2(
	float left,
	float right)
{
	/* Reports the ordering the ABI asks for. */
	return compare32(left, right, 1) != 0;
}

/*
 * Reports whether two binary32 values are unequal.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__nesf2(
	float left,
	float right)
{
	/* Reports the ordering the ABI asks for. */
	return compare32(left, right, 1) != 0;
}

/*
 * Orders two binary32 values for a greater-or-equal test.
 *
 * The ABI has this helper report -1 when the operands do not compare.
 */
int
__gesf2(
	float left,
	float right)
{
	/* Reports the ordering the ABI asks for. */
	return compare32(left, right, -1);
}

/*
 * Orders two binary32 values for a greater-than test.
 *
 * The ABI has this helper report -1 when the operands do not compare.
 */
int
__gtsf2(
	float left,
	float right)
{
	/* Reports the ordering the ABI asks for. */
	return compare32(left, right, -1);
}

/*
 * Orders two binary32 values for a less-or-equal test.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__lesf2(
	float left,
	float right)
{
	/* Reports the ordering the ABI asks for. */
	return compare32(left, right, 1);
}

/*
 * Orders two binary32 values for a less-than test.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__ltsf2(
	float left,
	float right)
{
	/* Reports the ordering the ABI asks for. */
	return compare32(left, right, 1);
}

/*
 * Orders two binary32 values.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__cmpsf2(
	float left,
	float right)
{
	/* Reports the ordering the ABI asks for. */
	return compare32(left, right, 1);
}

/*
 * Reports whether two binary32 values fail to compare.
 */
int
__unordsf2(
	float left,
	float right)
{
	int unordered;

	/* Only the unordered flag matters here. */
	(void)zsf32_compare(float_bits(left), float_bits(right), &unordered);

	/* Reports whether either operand is a NaN. */
	return unordered;
}

/*
 * Reports whether two binary64 values are unequal.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__eqdf2(
	double left,
	double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare64(left, right, 1) != 0;
}

/*
 * Reports whether two binary64 values are unequal.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__nedf2(
	double left,
	double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare64(left, right, 1) != 0;
}

/*
 * Orders two binary64 values for a greater-or-equal test.
 *
 * The ABI has this helper report -1 when the operands do not compare.
 */
int
__gedf2(
	double left,
	double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare64(left, right, -1);
}

/*
 * Orders two binary64 values for a greater-than test.
 *
 * The ABI has this helper report -1 when the operands do not compare.
 */
int
__gtdf2(
	double left,
	double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare64(left, right, -1);
}

/*
 * Orders two binary64 values for a less-or-equal test.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__ledf2(
	double left,
	double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare64(left, right, 1);
}

/*
 * Orders two binary64 values for a less-than test.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__ltdf2(
	double left,
	double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare64(left, right, 1);
}

/*
 * Orders two binary64 values.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__cmpdf2(
	double left,
	double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare64(left, right, 1);
}

/*
 * Reports whether two binary64 values fail to compare.
 */
int
__unorddf2(
	double left,
	double right)
{
	int unordered;

	/* Only the unordered flag matters here. */
	(void)zsf64_compare(double_bits(left), double_bits(right), &unordered);

	/* Reports whether either operand is a NaN. */
	return unordered;
}

/* Reads the object representation of a binary32 value. */
static uint32_t
float_bits(
	float value)
{
	union {
		float value;
		uint32_t bits;
	} shape = { value };

	/* The union is how the bits are read without a strict-aliasing cast. */
	return shape.bits;
}

/* Builds a binary32 value from an object representation. */
static float
bits_float(
	uint32_t bits)
{
	union {
		uint32_t bits;
		float value;
	} shape = { bits };

	/* The union is how the bits are read without a strict-aliasing cast. */
	return shape.value;
}

/* Reads the object representation of a binary64 value. */
static uint64_t
double_bits(
	double value)
{
	union {
		double value;
		uint64_t bits;
	} shape = { value };

	/* The union is how the bits are read without a strict-aliasing cast. */
	return shape.bits;
}

/* Builds a binary64 value from an object representation. */
static double
bits_double(
	uint64_t bits)
{
	union {
		uint64_t bits;
		double value;
	} shape = { bits };

	/* The union is how the bits are read without a strict-aliasing cast. */
	return shape.value;
}

/* Orders two binary32 values, substituting a value when they do not compare. */
static int
compare32(
	float left,
	float right,
	int unordered_value)
{
	int unordered;
	int result;

	/* Asks the core for the ordering and whether the pair is unordered. */
	result = zsf32_compare(float_bits(left), float_bits(right), &unordered);

	/* An unordered pair reports whatever the calling helper asked for. */
	return unordered ? unordered_value : result;
}

/* Orders two binary64 values, substituting a value when they do not compare. */
static int
compare64(
	double left,
	double right,
	int unordered_value)
{
	int unordered;
	int result;

	/* Asks the core for the ordering and whether the pair is unordered. */
	result = zsf64_compare(double_bits(left), double_bits(right),
	    &unordered);

	/* An unordered pair reports whatever the calling helper asked for. */
	return unordered ? unordered_value : result;
}
