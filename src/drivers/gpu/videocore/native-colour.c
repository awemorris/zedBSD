/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Clear components are raw client IEEE bits; the kernel never evaluates floating-point instructions. */
#include <stddef.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-colour.h"

static uint32_t convert_component(uint32_t word);

/*
 * Packs four exact raw IEEE components into a clamped eight-bit UNORM storage word.
 *
 * Finite values round the exact binary value times 255 to nearest even.
 * Negative values clamp to zero and values at or above one clamp to 255.
 * NaNs choose zero deterministically.  This initializes a colour clear;
 * fragment blending and rasterization remain native GPU operations.
 */
int
bcm2711_native_colour_pack(
	const uint32_t words[4],
	uint32_t swap,
	uint32_t *colour)
{
	uint32_t red;
	uint32_t green;
	uint32_t blue;
	uint32_t alpha;
	uint32_t packed;

	/* Missing storage or an unsupported channel choice refuses before the output changes. */
	if (words == NULL ||
	    colour == NULL ||
	    swap > 1U)
		return EINVAL;

	/* Read every component before publishing, permitting output to alias one input word. */
	red = convert_component(words[0]);
	green = convert_component(words[1]);
	blue = convert_component(words[2]);
	alpha = convert_component(words[3]);

	/* BGRA storage matches the compiled fragment output conversion without a second native tile-store swap. */
	packed = red | (green << 8) | (blue << 16) | (alpha << 24);
	if (swap != 0)
		packed = blue | (green << 8) | (red << 16) | (alpha << 24);

	/* Succeeded: the exact complete storage word is published once. */
	*colour = packed;
	return 0;
}

/* Converts one exact IEEE binary32 bit image to clamped nearest-even eight-bit UNORM using bounded integer shifts. */
static uint32_t
convert_component(
	uint32_t word)
{
	uint32_t fraction;
	uint32_t exponent;
	uint32_t shift;
	uint32_t rounded;
	uint64_t product;
	uint64_t remainder;
	uint64_t halfway;

	/* A negative component, including negative zero or infinity, has the deterministic zero clamped result. */
	if ((word & 0x80000000U) != 0)
		return 0;
	fraction = word & 0x7fffffU;
	exponent = (word >> 23) & 255U;

	/* A positive NaN has no finite normalized value and chooses zero rather than inheriting arbitrary payload bits. */
	if (exponent == 255U && fraction != 0)
		return 0;

	/* Every finite value at or above one and positive infinity clamp at the final component value. */
	if (word >= 0x3f800000U)
		return 255;

	/* Exact significand multiplication needs at most thirty-two bits before binary scaling. */
	shift = 149;
	if (exponent != 0) {
		fraction |= 0x800000U;
		shift = 150U - exponent;
	}

	/* Values this small cannot reach a half UNORM step; no oversized native shift is evaluated. */
	product = (uint64_t)fraction * 255U;
	if (shift >= 64U)
		return 0;

	/* Quotient and remainder preserve the exact finite value without a floating multiply or double rounding. */
	rounded = (uint32_t)(product >> shift);
	remainder = product & (((uint64_t)1 << shift) - 1U);
	halfway = (uint64_t)1 << (shift - 1U);
	if (remainder > halfway)
		rounded++;
	else if (remainder == halfway && (rounded & 1U) != 0)
		rounded++;

	/* Succeeded: the admitted less-than-one finite range needs no narrowing or saturating arithmetic. */
	return rounded;
}
