/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's float arithmetic on bits (amdrdna-vk-exec.h): the kernel
 * has no floating point, so a float is turned into fixed point, worked on
 * as an integer, and turned back.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/vk/amdrdna-vk-exec.h"

/*
 * A 32-bit float's fields: the sign, the exponent and its bias, the
 * mantissa and its hidden bit.
 *
 * Values referenced: IEEE 754-2008 binary32 (sign 31, biased exponent
 * 30:23 with a bias of 127, fraction 22:0).
 */
#define FLOAT_SIGN			0x80000000U
#define FLOAT_EXPONENT_SHIFT		23U
#define FLOAT_EXPONENT_MASK		0xffU
#define FLOAT_MANTISSA_MASK		0x7fffffU
#define FLOAT_HIDDEN			0x800000U
#define FLOAT_BIAS			127
#define FLOAT_MANTISSA_BITS		23

/* Reports the float bits of a whole number (exact up to 2^24). */
uint32_t
amdrdna_vk_float(
	int32_t value)
{
	uint32_t bits;

	/* A whole number is a fixed point value without fraction. */
	bits = amdrdna_vk_float_fixed((int64_t)value, 0U);

	/* Succeeded: the float's bits. */
	return bits;
}

/*
 * Reports a float, given by its bits, in fixed point with fraction
 * fractional bits, truncated toward zero; a value too large for 40 integer
 * bits saturates, a denormal is zero.
 */
int64_t
amdrdna_vk_fixed(
	uint32_t bits,
	uint32_t fraction)
{
	int64_t value;
	int32_t exponent;
	int32_t shift;
	uint64_t mantissa;

	/* The exponent and the mantissa with its hidden bit. */
	exponent = (int32_t)((bits >> FLOAT_EXPONENT_SHIFT) & FLOAT_EXPONENT_MASK);
	if (exponent == 0)
		return 0;
	mantissa = (uint64_t)((bits & FLOAT_MANTISSA_MASK) | FLOAT_HIDDEN);

	/* The mantissa moved to the fixed point's scale. */
	shift = exponent - FLOAT_BIAS - FLOAT_MANTISSA_BITS + (int32_t)fraction;
	if (shift > 39) {
		value = (int64_t)1 << 62;
	} else if (shift >= 0) {
		value = (int64_t)(mantissa << shift);
	} else if (shift > -64) {
		value = (int64_t)(mantissa >> (uint32_t)(-shift));
	} else {
		value = 0;
	}

	/* The sign. */
	if ((bits & FLOAT_SIGN) != 0U)
		value = -value;

	/* Succeeded: the value in fixed point. */
	return value;
}

/*
 * Reports the float bits of a fixed point value with fraction fractional
 * bits: exact when its significant bits fit the mantissa, truncated
 * otherwise.
 */
uint32_t
amdrdna_vk_float_fixed(
	int64_t value,
	uint32_t fraction)
{
	uint64_t magnitude;
	uint32_t sign;
	uint32_t mantissa;
	int32_t top;
	int32_t exponent;

	/* Zero has no bits set. */
	if (value == 0)
		return 0U;

	/* The sign and the magnitude. */
	sign = 0U;
	magnitude = (uint64_t)value;
	if (value < 0) {
		sign = FLOAT_SIGN;
		magnitude = (uint64_t)(-value);
	}

	/* The highest bit set is the hidden bit. */
	top = 63;
	while ((magnitude & ((uint64_t)1 << top)) == 0U)
		top--;

	/* The exponent; a value too small for a normal float is zero. */
	exponent = top - (int32_t)fraction + FLOAT_BIAS;
	if (exponent <= 0)
		return 0U;

	/* The mantissa's 23 bits below the hidden bit. */
	if (top > FLOAT_MANTISSA_BITS) {
		mantissa = (uint32_t)(magnitude >> (uint32_t)(top - FLOAT_MANTISSA_BITS));
	} else {
		mantissa = (uint32_t)(magnitude << (uint32_t)(FLOAT_MANTISSA_BITS - top));
	}

	/* Succeeded: the float's bits. */
	return sign | ((uint32_t)exponent << FLOAT_EXPONENT_SHIFT) | (mantissa & FLOAT_MANTISSA_MASK);
}

/*
 * Gives a float's bits multiplied by 2 to a power: its exponent raised by
 * the power, a value grown past the largest float infinite, and a
 * subnormal value normalized first.  Zero, infinity and NaN stay as they
 * are.
 */
uint32_t
amdrdna_vk_float_scale(
	uint32_t bits,
	uint32_t power)
{
	uint32_t sign;
	uint32_t exponent;
	uint32_t mantissa;

	sign = bits & FLOAT_SIGN;
	exponent = (bits >> FLOAT_EXPONENT_SHIFT) & FLOAT_EXPONENT_MASK;
	mantissa = bits & FLOAT_MANTISSA_MASK;

	/* Infinity, NaN and zero are their own multiples. */
	if (exponent == FLOAT_EXPONENT_MASK)
		return bits;
	if (exponent == 0U && mantissa == 0U)
		return bits;

	/* A subnormal value doubles by its mantissa until its hidden bit is set or the power is spent. */
	while (exponent == 0U && power != 0U) {
		mantissa <<= 1;
		power--;
		if ((mantissa & FLOAT_HIDDEN) != 0U) {
			mantissa &= FLOAT_MANTISSA_MASK;
			exponent = 1U;
		}
	}

	/* The rest of the power on the exponent; past the largest exponent is infinity. */
	if (power >= FLOAT_EXPONENT_MASK - exponent)
		return sign | (FLOAT_EXPONENT_MASK << FLOAT_EXPONENT_SHIFT);
	exponent += power;

	/* Succeeded: the scaled float's bits. */
	return sign | (exponent << FLOAT_EXPONENT_SHIFT) | mantissa;
}
