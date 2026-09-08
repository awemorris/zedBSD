/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 binary128 compiler ABI wrappers.
 *
 * SPARC V9 makes long double a quad-precision value, and a compiler that
 * finds no hardware for it emits calls to these names.  Each one converts its
 * arguments to their object representations, hands them to the integer-only
 * core, and converts the result back.
 *
 * The names and their return conventions are fixed by the ABI, not chosen
 * here.  In particular the comparison helpers report an ordering, and each
 * one names the value it must report when the operands do not compare.
 */

#include <stdint.h>

#include "src/softfloat/zed-softfloat128.h"

static struct zsf128 tf_bits(long double value);
static long double bits_tf(struct zsf128 bits);
static uint64_t df_bits(double value);
static double bits_df(uint64_t bits);
static uint32_t sf_bits(float value);
static float bits_sf(uint32_t bits);
static uint64_t tf_to_uint(long double value, int is_unsigned);
static int compare128(long double left, long double right, int unordered_value);

/*
 * Adds two binary128 values.
 */
long double
__addtf3(
	long double left,
	long double right)
{
	/* Reports the sum. */
	return bits_tf(zsf128_add(tf_bits(left), tf_bits(right)));
}

/*
 * Subtracts one binary128 value from another.
 */
long double
__subtf3(
	long double left,
	long double right)
{
	/* Reports the difference. */
	return bits_tf(zsf128_sub(tf_bits(left), tf_bits(right)));
}

/*
 * Multiplies two binary128 values.
 */
long double
__multf3(
	long double left,
	long double right)
{
	/* Reports the product. */
	return bits_tf(zsf128_mul(tf_bits(left), tf_bits(right)));
}

/*
 * Divides one binary128 value by another.
 */
long double
__divtf3(
	long double left,
	long double right)
{
	/* Reports the quotient. */
	return bits_tf(zsf128_div(tf_bits(left), tf_bits(right)));
}

/*
 * Widens a binary32 value to binary128.
 */
long double
__extendsftf2(
	float value)
{
	/* Every binary32 value is exact in binary128. */
	return bits_tf(zsf32_to_128(sf_bits(value)));
}

/*
 * Widens a binary64 value to binary128.
 */
long double
__extenddftf2(
	double value)
{
	/* Every binary64 value is exact in binary128. */
	return bits_tf(zsf64_to_128(df_bits(value)));
}

/*
 * Narrows a binary128 value to binary32.
 */
float
__trunctfsf2(
	long double value)
{
	/* The narrowing rounds, and may overflow or lose precision. */
	return bits_sf(zsf128_to_32(tf_bits(value)));
}

/*
 * Narrows a binary128 value to binary64.
 */
double
__trunctfdf2(
	long double value)
{
	/* The narrowing rounds, and may overflow or lose precision. */
	return bits_df(zsf128_to_64(tf_bits(value)));
}

/*
 * Converts a signed int to binary128.
 */
long double
__floatsitf(
	int value)
{
	/* Reports the converted value. */
	return bits_tf(zsf_i64_to_128(value));
}

/*
 * Converts an unsigned int to binary128.
 */
long double
__floatunsitf(
	unsigned int value)
{
	/* Reports the converted value. */
	return bits_tf(zsf_u64_to_128(value));
}

/*
 * Converts a signed long long to binary128.
 */
long double
__floatditf(
	long long value)
{
	/* Reports the converted value. */
	return bits_tf(zsf_i64_to_128(value));
}

/*
 * Converts an unsigned long long to binary128.
 */
long double
__floatunditf(
	unsigned long long value)
{
	/* Reports the converted value. */
	return bits_tf(zsf_u64_to_128(value));
}

/*
 * Converts a binary128 value to a signed int.
 */
int
__fixtfsi(
	long double value)
{
	/* The conversion truncates towards zero and saturates. */
	return (int)tf_to_uint(value, 0);
}

/*
 * Converts a binary128 value to a signed long long.
 */
long long
__fixtfdi(
	long double value)
{
	/* The conversion truncates towards zero and saturates. */
	return (long long)tf_to_uint(value, 0);
}

/*
 * Converts a binary128 value to an unsigned int.
 */
unsigned int
__fixunstfsi(
	long double value)
{
	/* The conversion truncates towards zero and saturates. */
	return (unsigned int)tf_to_uint(value, 1);
}

/*
 * Converts a binary128 value to an unsigned long long.
 */
unsigned long long
__fixunstfdi(
	long double value)
{
	/* The conversion truncates towards zero and saturates. */
	return tf_to_uint(value, 1);
}

/*
 * Reports whether two binary128 values are unequal.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__eqtf2(
	long double left,
	long double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare128(left, right, 1) != 0;
}

/*
 * Reports whether two binary128 values are unequal.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__netf2(
	long double left,
	long double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare128(left, right, 1) != 0;
}

/*
 * Orders two binary128 values for a greater-or-equal test.
 *
 * The ABI has this helper report -1 when the operands do not compare.
 */
int
__getf2(
	long double left,
	long double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare128(left, right, -1);
}

/*
 * Orders two binary128 values for a greater-than test.
 *
 * The ABI has this helper report -1 when the operands do not compare.
 */
int
__gttf2(
	long double left,
	long double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare128(left, right, -1);
}

/*
 * Orders two binary128 values for a less-or-equal test.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__letf2(
	long double left,
	long double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare128(left, right, 1);
}

/*
 * Orders two binary128 values for a less-than test.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__lttf2(
	long double left,
	long double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare128(left, right, 1);
}

/*
 * Orders two binary128 values.
 *
 * The ABI has this helper report 1 when the operands do not compare.
 */
int
__cmptf2(
	long double left,
	long double right)
{
	/* Reports the ordering the ABI asks for. */
	return compare128(left, right, 1);
}

/*
 * Reports whether two binary128 values fail to compare.
 */
int
__unordtf2(
	long double left,
	long double right)
{
	int unordered;

	/* Only the unordered flag matters here. */
	(void)zsf128_compare(tf_bits(left), tf_bits(right), &unordered);

	/* Reports whether either operand is a NaN. */
	return unordered;
}

/* Reads the object representation of a binary128 value. */
static struct zsf128
tf_bits(
	long double value)
{
	union {
		long double value;
		uint64_t limb[2];
	} shape = { value };

	/* The halves are stored in the machine's own order. */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return (struct zsf128){ shape.limb[1], shape.limb[0] };
#else
	return (struct zsf128){ shape.limb[0], shape.limb[1] };
#endif
}

/* Builds a binary128 value from an object representation. */
static long double
bits_tf(
	struct zsf128 bits)
{
	union {
		uint64_t limb[2];
		long double value;
	} shape;

	/* The halves are stored in the machine's own order. */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	shape.limb[0] = bits.low;
	shape.limb[1] = bits.high;
#else
	shape.limb[0] = bits.high;
	shape.limb[1] = bits.low;
#endif

	/* The union is how the bits are read without a strict-aliasing cast. */
	return shape.value;
}

/* Reads the object representation of a binary64 value. */
static uint64_t
df_bits(
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
bits_df(
	uint64_t bits)
{
	union {
		uint64_t bits;
		double value;
	} shape = { bits };

	/* The union is how the bits are read without a strict-aliasing cast. */
	return shape.value;
}

/* Reads the object representation of a binary32 value. */
static uint32_t
sf_bits(
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
bits_sf(
	uint32_t bits)
{
	union {
		uint32_t bits;
		float value;
	} shape = { bits };

	/* The union is how the bits are read without a strict-aliasing cast. */
	return shape.value;
}

/*
 * Truncates a binary128 value towards zero into 64 bits.
 *
 * The integer conversions are done here rather than through the core, because
 * every ABI helper that needs one wants the same 64-bit magnitude and differs
 * only in how it saturates.
 */
static uint64_t
tf_to_uint(
	long double value,
	int is_unsigned)
{
	struct zsf128 bits;
	uint64_t significand_high;
	uint64_t result;
	unsigned int sign;
	unsigned int exponent_field;
	unsigned int shift;
	int exponent;

	/* Takes the fields apart. */
	bits = tf_bits(value);
	sign = (unsigned int)(bits.high >> 63);
	exponent_field = (unsigned int)((bits.high >> 48) & 0x7fffU);
	exponent = (int)exponent_field - 16383;

	/*
	 * A subnormal, anything below one, and a negative value asked for as
	 * unsigned all truncate to zero.
	 */
	if (exponent_field == 0 || exponent < 0 || (sign && is_unsigned))
		return 0;

	/* An infinity, a NaN, or a value too wide to hold saturates. */
	if (exponent_field == 0x7fffU || exponent > 63)
		return is_unsigned ? UINT64_MAX :
		    (sign ? 0x8000000000000000ULL : 0x7fffffffffffffffULL);

	/* Restores the hidden bit and shifts the significand to an integer. */
	significand_high = (bits.high & 0x0000ffffffffffffULL) |
	    0x0001000000000000ULL;
	shift = (unsigned int)(112 - exponent);
	if (shift >= 64)
		result = significand_high >> (shift - 64);
	else if (shift == 0)
		result = bits.low;
	else
		result = (bits.low >> shift) | (significand_high << (64 - shift));

	/* A negative value is reported as its two's complement. */
	return sign ? (uint64_t)(0 - result) : result;
}

/* Orders two binary128 values, substituting a value when they do not compare. */
static int
compare128(
	long double left,
	long double right,
	int unordered_value)
{
	int unordered;
	int result;

	/* Asks the core for the ordering and whether the pair is unordered. */
	result = zsf128_compare(tf_bits(left), tf_bits(right), &unordered);

	/* An unordered pair reports whatever the calling helper asked for. */
	return unordered ? unordered_value : result;
}
