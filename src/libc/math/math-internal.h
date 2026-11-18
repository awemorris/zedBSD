/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declarations shared by the files of the mathematical library.
 *
 * The library assumes binary64 doubles and binary32 floats rounded to
 * nearest, which every configured target provides: SSE2 on amd64, the
 * soft-float runtime on i386, and the hardware on arm64 and sparcv9.  The
 * floating-point environment is a software cell (src/libc/fenv.c) that only
 * knows round-to-nearest, so exceptions are raised here explicitly.
 */

#ifndef LIBC_MATH_INTERNAL_H
#define LIBC_MATH_INTERNAL_H

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-constants.h"

/*
 * The error-free transformations below depend on every product being
 * rounded before it is added.  The arm64 compiler fuses a*b+c into one
 * instruction unless it is told not to, which would silently change their
 * results.  GCC builds only sparcv9 here, with soft float and no fused
 * instruction to contract into, and it rejects this pragma.
 */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

/*
 * Keeps the helpers out of the dynamic symbol table of the C library.
 *
 * They are shared between the files of the library only, and a program
 * must not be able to interpose them.
 */
#define LIBM_HIDDEN __attribute__((visibility("hidden")))

/* The bias of the binary64 exponent field. */
#define LIBM_DOUBLE_BIAS 1023

/* The sign bit of a binary64 value. */
#define LIBM_DOUBLE_SIGN UINT64_C(0x8000000000000000)

/* The bits of the magnitude of a binary64 value. */
#define LIBM_DOUBLE_MAGNITUDE UINT64_C(0x7fffffffffffffff)

/* The encoding of positive infinity, above which every magnitude is a NaN. */
#define LIBM_DOUBLE_INFINITY UINT64_C(0x7ff0000000000000)

/* The implicit leading bit of a normal binary64 significand. */
#define LIBM_DOUBLE_HIDDEN UINT64_C(0x0010000000000000)

/* The stored fraction bits of a binary64 value. */
#define LIBM_DOUBLE_FRACTION UINT64_C(0x000fffffffffffff)

/* The sign bit of a binary32 value. */
#define LIBM_FLOAT_SIGN UINT32_C(0x80000000)

/* The bits of the magnitude of a binary32 value. */
#define LIBM_FLOAT_MAGNITUDE UINT32_C(0x7fffffff)

/* The encoding of positive infinity in binary32. */
#define LIBM_FLOAT_INFINITY UINT32_C(0x7f800000)

/* The implicit leading bit of a normal binary32 significand. */
#define LIBM_FLOAT_HIDDEN UINT32_C(0x00800000)

/* The stored fraction bits of a binary32 value. */
#define LIBM_FLOAT_FRACTION UINT32_C(0x007fffff)

/*
 * One binary64 value seen both as a number and as its encoding.
 *
 * Reading the member that was not written is how the library reaches the
 * bits; the compilers of every target define that access.
 */
union libm_double_shape {
	double value;
	uint64_t bits;
};

/*
 * One binary32 value seen both as a number and as its encoding.
 */
union libm_float_shape {
	float value;
	uint32_t bits;
};

/*
 * An unpacked finite nonzero binary64 value.
 *
 * The value is significand * 2^exponent with the significand in
 * [2^52, 2^53), so subnormal inputs are normalized and every caller sees
 * the same shape.
 */
struct libm_unpacked {
	unsigned int sign;
	int exponent;
	uint64_t significand;
};

/*
 * A double-double: an unevaluated sum of two doubles.
 *
 * The low part is at most half an ulp of the high part, so the pair holds
 * about 106 bits.  The functions of group B compute their results in this
 * form and round high + low once at the end.
 */
struct libm_dd {
	double high;
	double low;
};

/*
 * One interval of a piecewise polynomial approximation.
 *
 * On the interval up to upper the polynomial is in t = (x - centre) *
 * inverse_half_width, and its coefficients start at first in the shared
 * array: two double-doubles, then degree - 1 doubles.
 */
struct libm_polynomial_interval {
	double upper;
	double centre;
	double inverse_half_width;
	int first;
	int degree;
};

LIBM_HIDDEN double __libm_invalid(void);
LIBM_HIDDEN double __libm_pole(unsigned int sign);
LIBM_HIDDEN double __libm_overflow(unsigned int sign);
LIBM_HIDDEN double __libm_underflow(unsigned int sign);
LIBM_HIDDEN double __libm_check_underflow(double value);
LIBM_HIDDEN float __libm_narrow(double value);
LIBM_HIDDEN double __libm_pack(unsigned int sign, int exponent, uint64_t significand);
LIBM_HIDDEN void __libm_unpack(double value, struct libm_unpacked *unpacked);
LIBM_HIDDEN double __libm_scale(struct libm_dd value, int exponent);
LIBM_HIDDEN struct libm_dd __libm_exp_reduced(int index, struct libm_dd reduced);
LIBM_HIDDEN struct libm_dd __libm_expm1_kernel(struct libm_dd reduced);
LIBM_HIDDEN struct libm_dd __libm_exp_dd(struct libm_dd x, int *exponent);
LIBM_HIDDEN struct libm_dd __libm_log_parts(double x, int *exponent);
LIBM_HIDDEN struct libm_dd __libm_log_dd(double x);
LIBM_HIDDEN struct libm_dd __libm_atan_dd(struct libm_dd u);
LIBM_HIDDEN void __libm_sin_cos_dd(struct libm_dd reduced, struct libm_dd *sine, struct libm_dd *cosine);

/* The tables of src/libc/math/tables.c; their comments say what they hold. */
LIBM_HIDDEN extern const struct libm_dd __libm_exp_table[128];
LIBM_HIDDEN extern const double __libm_exp_coefficients[5];
LIBM_HIDDEN extern const double __libm_log_inverse[128];
LIBM_HIDDEN extern const struct libm_dd __libm_log_table[128];
LIBM_HIDDEN extern const double __libm_log_coefficients[8];
LIBM_HIDDEN extern const uint32_t __libm_two_over_pi_bits[40];
LIBM_HIDDEN extern const struct libm_dd __libm_sin_cos_table[104];
LIBM_HIDDEN extern const double __libm_sin_coefficients[4];
LIBM_HIDDEN extern const double __libm_cos_coefficients[4];
LIBM_HIDDEN extern const struct libm_dd __libm_atan_table[65];
LIBM_HIDDEN extern const double __libm_atan_coefficients[6];
LIBM_HIDDEN extern const double __libm_stirling_coefficients[12];
LIBM_HIDDEN extern const double __libm_lgamma_one_coefficients[8];
LIBM_HIDDEN extern const double __libm_lgamma_two_coefficients[8];
LIBM_HIDDEN extern const double __libm_erf_coefficients[16];
LIBM_HIDDEN extern const double __libm_erfc_coefficients[246];
LIBM_HIDDEN extern const struct libm_polynomial_interval __libm_erfc_intervals[15];

/*
 * Returns the encoding of a binary64 value.
 */
static __inline uint64_t
libm_bits(
	double value)
{
	union libm_double_shape shape;

	/* Stores the value into the shared storage. */
	shape.value = value;

	/* The encoding is the other view of the same storage. */
	return shape.bits;
}

/*
 * Returns the binary64 value of an encoding.
 */
static __inline double
libm_from_bits(
	uint64_t bits)
{
	union libm_double_shape shape;

	/* Stores the encoding into the shared storage. */
	shape.bits = bits;

	/* The value is the other view of the same storage. */
	return shape.value;
}

/*
 * Returns the encoding of a binary32 value.
 */
static __inline uint32_t
libm_float_bits(
	float value)
{
	union libm_float_shape shape;

	/* Stores the value into the shared storage. */
	shape.value = value;

	/* The encoding is the other view of the same storage. */
	return shape.bits;
}

/*
 * Returns the binary32 value of an encoding.
 */
static __inline float
libm_float_from_bits(
	uint32_t bits)
{
	union libm_float_shape shape;

	/* Stores the encoding into the shared storage. */
	shape.bits = bits;

	/* The value is the other view of the same storage. */
	return shape.value;
}


/*
 * Returns a + b as a double-double, exactly (Knuth's TwoSum).
 *
 * The rounding error of the sum is recovered from the two operands without
 * any assumption about their order.
 */
static __inline struct libm_dd
libm_two_sum(
	double a,
	double b)
{
	struct libm_dd sum;
	double b_part;
	double a_part;

	/* Rounds the sum and splits it back into the parts of a and b. */
	sum.high = a + b;
	b_part = sum.high - a;
	a_part = sum.high - b_part;

	/* What each part lost to the rounding adds up to the error. */
	sum.low = (a - a_part) + (b - b_part);

	/* Succeeded: high + low equals a + b exactly. */
	return sum;
}

/*
 * Returns a + b as a double-double for |a| >= |b| (Dekker's Fast2Sum).
 */
static __inline struct libm_dd
libm_fast_two_sum(
	double a,
	double b)
{
	struct libm_dd sum;

	/* With a the larger, the error is what b lost in the rounding. */
	sum.high = a + b;
	sum.low = b - (sum.high - a);

	/* Succeeded: high + low equals a + b exactly. */
	return sum;
}

/*
 * Returns a * b as a double-double, exactly.
 *
 * Where the target has a fused multiply-add, the error of the product is
 * one fused operation.  Elsewhere each factor is split into two halves of
 * 26 bits (Veltkamp) whose partial products are exact, and the error is
 * summed from them (Dekker).  The split needs |a| and |b| below 2^995.
 */
static __inline struct libm_dd
libm_two_product(
	double a,
	double b)
{
	struct libm_dd product;
#ifndef __FP_FAST_FMA
	double a_split;
	double a_high;
	double a_low;
	double b_split;
	double b_high;
	double b_low;
#endif

	/* Rounds the product. */
	product.high = a * b;

#ifdef __FP_FAST_FMA
	/* The fused operation computes the rounding error directly. */
	product.low = __builtin_fma(a, b, -product.high);
#else
	/* Splits each factor into a high and a low half. */
	a_split = 134217729.0 * a;
	a_high = a_split - (a_split - a);
	a_low = a - a_high;
	b_split = 134217729.0 * b;
	b_high = b_split - (b_split - b);
	b_low = b - b_high;

	/* The four partial products are exact and sum to the error. */
	product.low = ((a_high * b_high - product.high) + a_high * b_low +
	    a_low * b_high) + a_low * b_low;
#endif

	/* Succeeded: high + low equals a * b exactly. */
	return product;
}

/*
 * Adds two double-doubles.
 *
 * Both the high and the low parts are summed without error before the
 * result is renormalized, so the sum is accurate even when the operands
 * cancel.
 */
static __inline struct libm_dd
libm_dd_add(
	struct libm_dd a,
	struct libm_dd b)
{
	struct libm_dd high;
	struct libm_dd low;

	/* Sums the high parts and the low parts, each exactly. */
	high = libm_two_sum(a.high, b.high);
	low = libm_two_sum(a.low, b.low);

	/* Folds the low sum into the high one, renormalizing twice. */
	high.low += low.high;
	high = libm_fast_two_sum(high.high, high.low);
	high.low += low.low;
	high = libm_fast_two_sum(high.high, high.low);

	/* Succeeded: the renormalized sum. */
	return high;
}

/*
 * Adds a double to a double-double.
 */
static __inline struct libm_dd
libm_dd_add_double(
	struct libm_dd a,
	double b)
{
	struct libm_dd sum;

	/* Sums the high part and b exactly, then folds in the low part. */
	sum = libm_two_sum(a.high, b);
	sum.low += a.low;
	sum = libm_fast_two_sum(sum.high, sum.low);

	/* Succeeded: the renormalized sum. */
	return sum;
}

/*
 * Multiplies two double-doubles.
 */
static __inline struct libm_dd
libm_dd_multiply(
	struct libm_dd a,
	struct libm_dd b)
{
	struct libm_dd product;

	/* The product of the high parts exactly, plus the cross terms. */
	product = libm_two_product(a.high, b.high);
	product.low += a.high * b.low + a.low * b.high;
	product = libm_fast_two_sum(product.high, product.low);

	/* Succeeded: the renormalized product. */
	return product;
}

/*
 * Multiplies a double-double by a double.
 */
static __inline struct libm_dd
libm_dd_multiply_double(
	struct libm_dd a,
	double b)
{
	struct libm_dd product;

	/* The product of the high part exactly, plus that of the low part. */
	product = libm_two_product(a.high, b);
	product.low += a.low * b;
	product = libm_fast_two_sum(product.high, product.low);

	/* Succeeded: the renormalized product. */
	return product;
}

/*
 * Divides one double-double by another.
 *
 * The first quotient digit is corrected once with the exact remainder,
 * which gives about 104 bits.
 */
static __inline struct libm_dd
libm_dd_divide(
	struct libm_dd a,
	struct libm_dd b)
{
	struct libm_dd remainder;
	struct libm_dd product;
	struct libm_dd quotient;
	double first;
	double second;

	/* The quotient of the high parts is the first approximation. */
	first = a.high / b.high;

	/* Takes first * b away from a, keeping the difference exact enough. */
	product = libm_dd_multiply_double(b, first);
	remainder = libm_two_sum(a.high, -product.high);
	remainder.low += a.low - product.low;

	/* The remainder divided by b corrects the first approximation. */
	second = (remainder.high + remainder.low) / b.high;
	quotient = libm_fast_two_sum(first, second);

	/* Succeeded: the two digits of the quotient. */
	return quotient;
}

/*
 * Returns the square root of a positive double-double.
 *
 * One Newton step from the correctly rounded root of the high part doubles
 * its precision.
 */
static __inline struct libm_dd
libm_dd_sqrt(
	struct libm_dd a)
{
	struct libm_dd square;
	struct libm_dd corrected;
	double root;
	double correction;

	/* The root of zero is zero; the correction below would divide by it. */
	if (a.high <= 0.0) {
		corrected.high = 0.0;
		corrected.low = 0.0;
		return corrected;
	}

	/* The root of the high part, correctly rounded. */
	root = sqrt(a.high);

	/* (a - root^2) / (2 root) is what the root is short by. */
	square = libm_two_product(root, root);
	correction = ((a.high - square.high) - square.low + a.low) / (2.0 * root);
	corrected = libm_fast_two_sum(root, correction);

	/* Succeeded: the corrected root. */
	return corrected;
}

#endif
