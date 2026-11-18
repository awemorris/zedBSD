/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The correctly rounded square root.
 *
 * Where the processor has floating-point registers, its square root
 * instruction gives the result (BUG-109): IEEE 754 requires it to be
 * correctly rounded, so it is the same result, only faster.  amd64 uses
 * SQRTSD and SQRTSS, arm64 FSQRT.  Elsewhere (i386 without SSE, the
 * kernels' soft-float objects, sparc64, m68k) and wherever the compiler is
 * told not to use those registers, the significand is square-rooted in
 * integers, one result bit per step, the way square roots are taken by
 * hand in base two.  Fifty-five bits of root are produced, and a nonzero
 * final remainder becomes a sticky bit, so the one rounding at the end is
 * correct.
 */

#include <fenv.h>
#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/*
 * Whether the processor's square root instruction is used: an amd64
 * compilation with SSE2, or an arm64 one with floating point (neither is
 * the case with -mgeneral-regs-only or -mno-sse2).
 */
#if defined(__x86_64__) && defined(__SSE2__)
#define LIBM_SQRT_HARDWARE 1
#elif defined(__aarch64__) && defined(__ARM_FP)
#define LIBM_SQRT_HARDWARE 1
#else
#define LIBM_SQRT_HARDWARE 0
#endif

/*
 * The number of radicand bits appended below the significand.
 *
 * A significand of 53 or 54 bits followed by 56 zero bits has a square
 * root of exactly 55 bits: 53 kept, one to round with, and one more below.
 */
#define LIBM_SQRT_EXTRA_BITS 56

/*
 * A radicand below 2^-900 is checked for an exact root at 2^1000 times its
 * size (its root at 2^500 times), where the product's rounding error cannot
 * underflow: the biased exponents of the three powers of two.
 */
#define LIBM_SQRT_TINY_EXPONENT		123U
#define LIBM_SQRT_SCALE_EXPONENT	2023U
#define LIBM_SQRT_ROOT_SCALE_EXPONENT	1523U

#if LIBM_SQRT_HARDWARE
static double sqrt_hardware(double x);
static float sqrtf_hardware(float x);
static int sqrt_exact(double x, double root);
#else
static double sqrt_software(double x);
#endif

/*
 * Returns the correctly rounded square root of a binary64 value.
 */
double
sqrt(
	double x)
{
	double result;

	/*
	 * A NaN, unequal to itself, is passed on; a zero and positive infinity
	 * are their own roots, and anything else below zero has none (which
	 * sets errno as well as the invalid exception).
	 */
	if (x != x)
		return x + x;
	if (x == 0.0)
		return x;
	if (x < 0.0)
		return __libm_invalid();
	if (x == HUGE_VAL)
		return x;

	/* The root of a positive finite value. */
#if LIBM_SQRT_HARDWARE
	result = sqrt_hardware(x);
#else
	result = sqrt_software(x);
#endif

	/* Succeeded: the root was rounded once. */
	return result;
}

/*
 * Returns the correctly rounded square root of a binary32 value.
 *
 * Without the processor's instruction the double root is taken: it is
 * correctly rounded to 53 bits, and rounding it again to 24 bits gives the
 * correctly rounded float root, because 53 is more than twice 24 plus two.
 */
float
sqrtf(
	float x)
{
	float root;

	/* A NaN is passed on, and a value below zero has no root (errno and the exception, as for sqrt). */
	if (x != x)
		return x + x;
	if (x < 0.0f)
		return (float)__libm_invalid();

	/*
	 * The processor's single-precision root (a zero and infinity are their
	 * own roots), or the double root rounded once more.
	 */
#if LIBM_SQRT_HARDWARE
	root = sqrtf_hardware(x);
#else
	root = (float)sqrt((double)x);
#endif

	/* Succeeded: the correctly rounded root. */
	return root;
}

#if LIBM_SQRT_HARDWARE
/*
 * Takes a positive finite double's square root with the processor's
 * instruction.
 *
 * libc keeps the floating-point exceptions in software (fenv.c), so an
 * inexact root raises FE_INEXACT there, as the integer root does: the root
 * is exact when its square, taken without error, is the radicand.
 */
static double
sqrt_hardware(
	double x)
{
	double root;
	int exact;

	/* One instruction, correctly rounded as IEEE 754 requires. */
#if defined(__x86_64__)
	__asm__("sqrtsd %1, %0" : "=x"(root) : "x"(x));
#else
	__asm__("fsqrt %d0, %d1" : "=w"(root) : "w"(x));
#endif

	/* An inexact root raises FE_INEXACT. */
	exact = sqrt_exact(x, root);
	if (!exact)
		(void)feraiseexcept(FE_INEXACT);

	/* Succeeded: the root. */
	return root;
}

/*
 * Takes a positive or zero float's square root with the processor's
 * instruction, raising FE_INEXACT as sqrt_hardware does.  The square of a
 * float root has at most 48 bits, so a double holds it exactly.
 */
static float
sqrtf_hardware(
	float x)
{
	double square;
	float root;

	/* One instruction, correctly rounded as IEEE 754 requires. */
#if defined(__x86_64__)
	__asm__("sqrtss %1, %0" : "=x"(root) : "x"(x));
#else
	__asm__("fsqrt %s0, %s1" : "=w"(root) : "w"(x));
#endif

	/* An inexact root raises FE_INEXACT (infinity is its own exact root). */
	square = (double)root * (double)root;
	if (square != (double)x)
		(void)feraiseexcept(FE_INEXACT);

	/* Succeeded: the root. */
	return root;
}

/* Tells whether a correctly rounded root of a positive finite double is exact. */
static int
sqrt_exact(
	double x,
	double root)
{
	struct libm_dd square;
	double tiny;

	/* A tiny radicand and its root are scaled up by powers of two (exactly), so the check cannot underflow. */
	tiny = libm_from_bits((uint64_t)LIBM_SQRT_TINY_EXPONENT << 52);
	if (x < tiny) {
		x *= libm_from_bits((uint64_t)LIBM_SQRT_SCALE_EXPONENT << 52);
		root *= libm_from_bits((uint64_t)LIBM_SQRT_ROOT_SCALE_EXPONENT << 52);
	}

	/* The root's square without error, as a rounded part and its error. */
	square = libm_two_product(root, root);

	/* The root is exact only when the square is the radicand. */
	if (square.high != x)
		return 0;
	if (square.low != 0.0)
		return 0;

	/* Succeeded: the root is exact. */
	return 1;
}
#else
/* Takes a positive finite double's correctly rounded square root in integers. */
static double
sqrt_software(
	double x)
{
	struct libm_unpacked unpacked;
	uint64_t significand;
	uint64_t root;
	uint64_t rest;
	uint64_t trial;
	uint64_t pair;
	int exponent;
	int position;
	double result;

	/*
	 * Writes x as significand * 2^exponent with an even exponent, so the
	 * root of the power of two is exact.
	 */
	__libm_unpack(x, &unpacked);
	significand = unpacked.significand;
	exponent = unpacked.exponent;
	if ((exponent & 1) != 0) {
		significand <<= 1;
		exponent--;
	}

	/*
	 * Takes the root of significand * 2^56 two radicand bits at a time,
	 * from the top.  At each step the remainder is at most twice the root,
	 * so it stays below 2^57 however long the radicand is.
	 */
	root = 0U;
	rest = 0U;
	for (position = 54; position >= 0; position--) {
		/* Brings down the next pair; the pairs below the significand are 0. */
		pair = 0U;
		if (2 * position >= LIBM_SQRT_EXTRA_BITS)
			pair = (significand >> (2 * position - LIBM_SQRT_EXTRA_BITS)) & 3U;
		rest = (rest << 2) | pair;

		/* The next bit is one when (2 * root + 1) fits in the remainder. */
		trial = (root << 2) | 1U;
		root <<= 1;
		if (rest >= trial) {
			rest -= trial;
			root |= 1U;
		}
	}

	/*
	 * The root has 55 bits and is worth root * 2^(exponent / 2 - 28).
	 * Moving it up by nine bits and adding the sticky bit gives the packing
	 * helper its 64-bit form.
	 */
	root <<= 9;
	if (rest != 0U)
		root |= 1U;
	result = __libm_pack(0U, exponent / 2 - 28 + 54, root);

	/* Succeeded: the root was rounded once. */
	return result;
}
#endif
