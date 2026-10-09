/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Integer significands supply the existing finite viewport contract without enabling kernel FP execution. */
#include <stddef.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-viewport.h"

static uint32_t scale_xy(uint32_t bits);
static uint32_t subtract_depth(uint32_t left, uint32_t right);
static uint32_t decode_significand(uint32_t bits, uint32_t *exponent);
static uint64_t shift_sticky(uint64_t significand, uint32_t shift);

/*
 * Computes exact native shader viewport uniforms from the admitted six copied IEEE words.
 *
 * XY export uses half-width/height multiplied by 256, hence an exact power
 * of two.  Depth subtraction rounds once to nearest-even and preserves
 * reversed ranges, cancellation, subnormals and the signed-zero endpoints.
 */
int
bcm2711_native_viewport_prepare(
	const uint32_t words[6],
	struct bcm2711_native_viewport *viewport)
{
	struct bcm2711_native_viewport prepared;
	uint32_t index;
	uint32_t magnitude;

	/* Missing copied input or output cannot supply native viewport uniforms. */
	if (words == NULL || viewport == NULL)
		return EINVAL;

	/* Coordinates obey the existing advertised finite -4096..4096 pixel interval. */
	for (index = 0; index < 2; index++) {
		/* Magnitude checks also exclude infinities and NaNs without floating-point comparisons. */
		magnitude = words[index] & 0x7fffffffU;
		if (magnitude > 0x45800000U)
			return EINVAL;
	}

	/* Positive dimensions stay within the Vulkan 1.0 interface already admitted by recording validation. */
	for (index = 2; index < 4; index++) {
		/* Negative zero and negative dimensions cannot become a native XY scale. */
		if (words[index] == 0 || words[index] > 0x45800000U)
			return EINVAL;
	}

	/* Each depth endpoint lies in zero through one; either signed zero remains legitimate. */
	for (index = 4; index < 6; index++) {
		/* Zero bypasses the sign restriction while retaining its exact endpoint bits. */
		magnitude = words[index] & 0x7fffffffU;
		if (magnitude == 0)
			continue;

		/* Nonzero negative endpoints are outside the current native depth interface. */
		if (words[index] > 0x3f800000U)
			return EINVAL;
	}

	/* All values are prepared before publication, including when the output aliases copied input words. */
	prepared.x_scale = scale_xy(words[2]);
	prepared.y_scale = scale_xy(words[3]);
	prepared.depth_scale = subtract_depth(words[5], words[4]);
	prepared.depth_offset = words[4];
	*viewport = prepared;

	/* Succeeded: native shaders and later clipper records can share the same exact viewport depth convention. */
	return 0;
}

/* Multiplies an admitted positive dimension by 128 without rounding away any input precision. */
static uint32_t
scale_xy(
	uint32_t bits)
{
	uint32_t exponent;
	uint32_t significand;

	/* Normal dimensions remain normal and finite after the exact exponent increment. */
	exponent = (bits >> 23) & 255U;
	if (exponent != 0)
		return bits + (7U << 23);

	/* Subnormal dimensions may remain subnormal or acquire an explicit normal exponent. */
	significand = (bits & 0x7fffffU) << 7;
	if (significand < 0x800000U)
		return significand;

	/* Initial scaling contributed seven zero low bits, so normalization discards no significant input bit. */
	exponent = 1;
	while (significand >= 0x1000000U) {
		significand >>= 1;
		exponent++;
	}

	/* The complete IEEE word preserves this exact power-of-two scale. */
	return (exponent << 23) | (significand & 0x7fffffU);
}

/* Subtracts two admitted nonnegative depth endpoints with three guard/round/sticky bits. */
static uint32_t
subtract_depth(
	uint32_t left,
	uint32_t right)
{
	uint32_t larger;
	uint32_t smaller;
	uint32_t temporary;
	uint32_t sign;
	uint32_t larger_exponent;
	uint32_t smaller_exponent;
	uint32_t larger_significand;
	uint32_t smaller_significand;
	uint32_t rounded;
	uint32_t remainder;
	uint64_t aligned;
	uint64_t difference;

	/* Comparing magnitudes orders every positive finite endpoint, including subnormals and either zero sign. */
	larger = left & 0x7fffffffU;
	smaller = right & 0x7fffffffU;
	sign = 0;
	if (larger < smaller) {
		temporary = larger;
		larger = smaller;
		smaller = temporary;
		sign = 0x80000000U;
	}

	/* Negative zero minus positive zero is the sole negative exact-zero case under nearest-even subtraction. */
	if (larger == 0) {
		if ((left & 0x80000000U) != 0 && (right & 0x80000000U) == 0)
			return 0x80000000U;
		return 0;
	}

	/* Subnormal significands use effective exponent one without adding the normal hidden bit. */
	larger_significand = decode_significand(larger, &larger_exponent);
	smaller_significand = decode_significand(smaller, &smaller_exponent);
	aligned = shift_sticky((uint64_t)smaller_significand << 3, larger_exponent - smaller_exponent);
	difference = ((uint64_t)larger_significand << 3) - aligned;

	/* Equal nonzero operands cancel to positive zero under the fixed nearest-even mode. */
	if (difference == 0)
		return 0;

	/* Cancellation renormalizes until the hidden-bit position or the subnormal boundary is reached. */
	while (difference < 0x4000000U && larger_exponent > 1U) {
		difference <<= 1;
		larger_exponent--;
	}

	/* A guard tie rounds toward the even retained significand; any larger remainder rounds away from zero. */
	rounded = (uint32_t)(difference >> 3);
	remainder = (uint32_t)(difference & 7U);
	if (remainder > 4U || (remainder == 4U && (rounded & 1U) != 0))
		rounded++;

	/* A rounding carry renormalizes the retained significand without saturating a reversed range. */
	if (rounded == 0x1000000U) {
		rounded >>= 1;
		larger_exponent++;
	}

	/* Subnormal differences keep an encoded exponent of zero, including a rounded transition to the smallest normal. */
	if (rounded < 0x800000U)
		return sign | rounded;

	/* The final depth scale retains the subtraction's sign and once-rounded exact IEEE representation. */
	return sign | (larger_exponent << 23) | (rounded & 0x7fffffU);
}

/* Decodes a finite magnitude into its effective biased exponent and unsigned integer significand. */
static uint32_t
decode_significand(
	uint32_t bits,
	uint32_t *exponent)
{
	uint32_t significand;

	/* Subnormals share the smallest normal's exponent but lack its implicit leading one. */
	*exponent = (bits >> 23) & 255U;
	significand = bits & 0x7fffffU;
	if (*exponent == 0) {
		*exponent = 1;
	} else {
		significand |= 0x800000U;
	}

	/* The caller can align this exact finite significand without a floating-point operation. */
	return significand;
}

/* Aligns a smaller significand while preserving every discarded nonzero bit in its sticky position. */
static uint64_t
shift_sticky(
	uint64_t significand,
	uint32_t shift)
{
	uint64_t discarded;
	uint64_t aligned;

	/* Equal exponents require no alignment and lose no input precision. */
	if (shift == 0)
		return significand;

	/* Very distant exponents contribute only their nonzero sticky information without an oversized C shift. */
	if (shift >= 64U) {
		if (significand != 0)
			return 1;
		return 0;
	}

	/* A finite-width mask distinguishes a truly exact alignment from a rounded low-bit remainder. */
	discarded = significand & (((uint64_t)1 << shift) - 1U);
	aligned = significand >> shift;
	if (discarded != 0)
		aligned |= 1U;

	/* Sticky information keeps the later single nearest-even rounding independent of exponent distance. */
	return aligned;
}
