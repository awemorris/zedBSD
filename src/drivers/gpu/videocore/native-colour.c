/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Clear components are raw client IEEE bits; the kernel never evaluates floating-point instructions. */
#include <stddef.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/native-colour.h"
#include "drivers/gpu/videocore/native-pass.h"

/*
 * The linear values at which an sRGB-encoded 8-bit channel steps up: entry
 * k is the decoded value of code k + 0.5, as IEEE single bits, so a linear
 * value encodes to the number of entries at or below it (the nearest
 * code, ties up).  Computed with the sRGB transfer function of Vulkan's
 * "sRGB EOTF" (s / 12.92 up to 0.04045, ((s + 0.055) / 1.055)^2.4 above).
 */
static const uint32_t colour_srgb_steps[255] = {
	0x391f22b4U, 0x39eeb40eU, 0x3a46eb61U, 0x3a8b3e5dU, 0x3ab3070aU, 0x3adacfb7U,
	0x3b014c32U, 0x3b153089U, 0x3b2914dfU, 0x3b3cf935U, 0x3b50f2d0U, 0x3b65fb9aU,
	0x3b7c3403U, 0x3b89d05fU, 0x3b962333U, 0x3ba314bdU, 0x3bb0a730U, 0x3bbedcb6U,
	0x3bcdb76dU, 0x3bdd3967U, 0x3bed64afU, 0x3bfe3b45U, 0x3c07df91U, 0x3c10f91aU,
	0x3c1a6b31U, 0x3c2436c7U, 0x3c2e5cc7U, 0x3c38de19U, 0x3c43bba4U, 0x3c4ef648U,
	0x3c5a8ee4U, 0x3c668654U, 0x3c72dd70U, 0x3c7f950fU, 0x3c865702U, 0x3c8d148eU,
	0x3c940395U, 0x3c9b247bU, 0x3ca277a6U, 0x3ca9fd77U, 0x3cb1b652U, 0x3cb9a298U,
	0x3cc1c2a8U, 0x3cca16e2U, 0x3cd29fa4U, 0x3cdb5d4aU, 0x3ce45031U, 0x3ced78b4U,
	0x3cf6d72eU, 0x3d0035fbU, 0x3d051bb4U, 0x3d0a1cecU, 0x3d0f39d0U, 0x3d14728aU,
	0x3d19c745U, 0x3d1f382bU, 0x3d24c567U, 0x3d2a6f21U, 0x3d303584U, 0x3d3618b7U,
	0x3d3c18e3U, 0x3d423631U, 0x3d4870c9U, 0x3d4ec8d2U, 0x3d553e72U, 0x3d5bd1d2U,
	0x3d628318U, 0x3d69526aU, 0x3d703feeU, 0x3d774bcaU, 0x3d7e7623U, 0x3d82df90U,
	0x3d869372U, 0x3d8a56caU, 0x3d8e29abU, 0x3d920c26U, 0x3d95fe4eU, 0x3d9a0035U,
	0x3d9e11ebU, 0x3da23384U, 0x3da6650fU, 0x3daaa6a0U, 0x3daef847U, 0x3db35a14U,
	0x3db7cc1bU, 0x3dbc4e6aU, 0x3dc0e114U, 0x3dc58428U, 0x3dca37b9U, 0x3dcefbd5U,
	0x3dd3d08eU, 0x3dd8b5f5U, 0x3dddac18U, 0x3de2b309U, 0x3de7cad8U, 0x3decf395U,
	0x3df22d4fU, 0x3df77817U, 0x3dfcd3fcU, 0x3e012087U, 0x3e03dfaeU, 0x3e06a77bU,
	0x3e0977f6U, 0x3e0c5126U, 0x3e0f3313U, 0x3e121dc5U, 0x3e151143U, 0x3e180d95U,
	0x3e1b12c1U, 0x3e1e20d1U, 0x3e2137caU, 0x3e2457b5U, 0x3e278099U, 0x3e2ab27cU,
	0x3e2ded67U, 0x3e313160U, 0x3e347e70U, 0x3e37d49cU, 0x3e3b33ecU, 0x3e3e9c67U,
	0x3e420e14U, 0x3e4588fbU, 0x3e490d21U, 0x3e4c9a8fU, 0x3e50314bU, 0x3e53d15dU,
	0x3e577acaU, 0x3e5b2d9aU, 0x3e5ee9d3U, 0x3e62af7dU, 0x3e667e9fU, 0x3e6a573eU,
	0x3e6e3961U, 0x3e722510U, 0x3e761a52U, 0x3e7a192bU, 0x3e7e21a4U, 0x3e8119e1U,
	0x3e8327c7U, 0x3e853a86U, 0x3e875221U, 0x3e896e9dU, 0x3e8b8ffcU, 0x3e8db641U,
	0x3e8fe16fU, 0x3e92118bU, 0x3e944696U, 0x3e968094U, 0x3e98bf89U, 0x3e9b0377U,
	0x3e9d4c61U, 0x3e9f9a4bU, 0x3ea1ed38U, 0x3ea4452aU, 0x3ea6a226U, 0x3ea9042dU,
	0x3eab6b44U, 0x3eadd76cU, 0x3eb048aaU, 0x3eb2bf00U, 0x3eb53a71U, 0x3eb7bb00U,
	0x3eba40b0U, 0x3ebccb85U, 0x3ebf5b80U, 0x3ec1f0a6U, 0x3ec48af9U, 0x3ec72a7cU,
	0x3ec9cf32U, 0x3ecc791dU, 0x3ecf2842U, 0x3ed1dca2U, 0x3ed49640U, 0x3ed75521U,
	0x3eda1945U, 0x3edce2b1U, 0x3edfb167U, 0x3ee2856aU, 0x3ee55ebdU, 0x3ee83d62U,
	0x3eeb215dU, 0x3eee0ab0U, 0x3ef0f95eU, 0x3ef3ed6aU, 0x3ef6e6d7U, 0x3ef9e5a7U,
	0x3efce9deU, 0x3efff37dU, 0x3f018144U, 0x3f030b81U, 0x3f049877U, 0x3f062826U,
	0x3f07ba91U, 0x3f094fb9U, 0x3f0ae79fU, 0x3f0c8244U, 0x3f0e1faaU, 0x3f0fbfd2U,
	0x3f1162bdU, 0x3f13086dU, 0x3f14b0e4U, 0x3f165c22U, 0x3f180a28U, 0x3f19baf9U,
	0x3f1b6e95U, 0x3f1d24feU, 0x3f1ede35U, 0x3f209a3bU, 0x3f225912U, 0x3f241abbU,
	0x3f25df37U, 0x3f27a688U, 0x3f2970afU, 0x3f2b3dacU, 0x3f2d0d82U, 0x3f2ee032U,
	0x3f30b5bdU, 0x3f328e23U, 0x3f346968U, 0x3f36478bU, 0x3f38288eU, 0x3f3a0c73U,
	0x3f3bf339U, 0x3f3ddce4U, 0x3f3fc974U, 0x3f41b8eaU, 0x3f43ab48U, 0x3f45a08eU,
	0x3f4798beU, 0x3f4993daU, 0x3f4b91e2U, 0x3f4d92d8U, 0x3f4f96bdU, 0x3f519d91U,
	0x3f53a758U, 0x3f55b410U, 0x3f57c3bdU, 0x3f59d65fU, 0x3f5bebf6U, 0x3f5e0486U,
	0x3f60200dU, 0x3f623e8fU, 0x3f64600cU, 0x3f668485U, 0x3f68abfbU, 0x3f6ad670U,
	0x3f6d03e4U, 0x3f6f345aU, 0x3f7167d1U, 0x3f739e4cU, 0x3f75d7ccU, 0x3f781451U,
	0x3f7a53ddU, 0x3f7c9671U, 0x3f7edc0eU
};

static uint32_t convert_component(uint32_t word);
static uint32_t unorm(uint32_t word, uint32_t width);
static uint32_t half_float(uint32_t word);
static uint32_t small_float(uint32_t word, uint32_t mantissa_bits);
static uint32_t srgb(uint32_t word);
static void put(uint8_t *texel, uint32_t offset, uint32_t value, uint32_t bytes);

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

/*
 * Converts a clear colour's four raw words to the tile buffer's clear
 * colour of a target of internal type and bpp (the tile clear words, bpp's
 * of them counting).
 *
 * As Mesa's v3dX(get_hw_clear_color): an 8-bit type packs RGBA8 UNORM in
 * one word, 8-bit integers their low bytes, 16-bit floats four halves and
 * 16-bit integers their low halves in two words, 32-bit types the words
 * themselves.  swap exchanges red and blue first (a BGRA target).
 */
int
bcm2711_native_colour_clear(
	const uint32_t words[4],
	uint32_t type,
	uint32_t bpp,
	uint32_t swap,
	uint32_t clear[4])
{
	uint32_t channels[4];
	int error;

	/* Missing storage or a type the tile buffer has not refuses before the output changes. */
	if (words == NULL || clear == NULL || swap > 1U || bpp > 2U)
		return EINVAL;

	/* The channels in the target's order. */
	channels[0] = words[0];
	channels[1] = words[1];
	channels[2] = words[2];
	channels[3] = words[3];
	if (swap != 0) {
		channels[0] = words[2];
		channels[2] = words[0];
	}

	/* The tile buffer's words, by type. */
	clear[0] = 0;
	clear[1] = 0;
	clear[2] = 0;
	clear[3] = 0;
	switch (type) {
	case BCM2711_NATIVE_TYPE_8:
		error = bcm2711_native_colour_pack(channels, 0, &clear[0]);
		if (error != 0)
			return error;
		break;
	case BCM2711_NATIVE_TYPE_8I:
	case BCM2711_NATIVE_TYPE_8UI:
		clear[0] = (channels[0] & 0xffU) | ((channels[1] & 0xffU) << 8) | ((channels[2] & 0xffU) << 16) | ((channels[3] & 0xffU) << 24);
		break;
	case BCM2711_NATIVE_TYPE_16F:
		clear[0] = half_float(channels[0]) | (half_float(channels[1]) << 16);
		clear[1] = half_float(channels[2]) | (half_float(channels[3]) << 16);
		break;
	case BCM2711_NATIVE_TYPE_16I:
	case BCM2711_NATIVE_TYPE_16UI:
		clear[0] = (channels[0] & 0xffffU) | (channels[1] << 16);
		clear[1] = (channels[2] & 0xffffU) | (channels[3] << 16);
		break;
	case BCM2711_NATIVE_TYPE_32I:
	case BCM2711_NATIVE_TYPE_32UI:
	case BCM2711_NATIVE_TYPE_32F:
		clear[0] = channels[0];
		clear[1] = channels[1];
		clear[2] = channels[2];
		clear[3] = channels[3];
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the tile clear colour. */
	return 0;
}

/*
 * Packs a clear colour's four raw words into one pixel of a raster image
 * of Output Image Format format, as the tile buffer's store would write it.
 *
 * UNORM channels round to the nearest, clamped to zero and one; sRGB
 * channels are encoded first; float channels convert to the nearest,
 * ties to even; integer channels keep their low bits.  swap exchanges red
 * and blue first (a BGRA image).  texel receives bytes (one to sixteen)
 * little-endian bytes.  ENOTSUP for a format not packed here.
 */
int
bcm2711_native_colour_texel(
	const uint32_t words[4],
	uint32_t format,
	uint32_t swap,
	uint8_t *texel,
	uint32_t *bytes)
{
	uint32_t channels[4];
	uint32_t value;
	uint32_t index;
	uint32_t count;

	/* Missing storage refuses before the output changes. */
	if (words == NULL || texel == NULL || bytes == NULL || swap > 1U)
		return EINVAL;

	/* The channels in the image's order. */
	channels[0] = words[0];
	channels[1] = words[1];
	channels[2] = words[2];
	channels[3] = words[3];
	if (swap != 0) {
		channels[0] = words[2];
		channels[2] = words[0];
	}

	/* The pixel, by format (Mesa's v3d_packet.xml "Output Image Format", channels named from the low bits). */
	switch (format) {
	case 27U:
	case 29U:
	case 30U:
		/* rgba8, rg8 and r8: UNORM bytes. */
		count = 4U;
		if (format == 29U)
			count = 2U;
		if (format == 30U)
			count = 1U;
		for (index = 0; index < count; index++)
			texel[index] = (uint8_t)unorm(channels[index], 8U);
		*bytes = count;
		break;
	case 0U:
		/* srgb8_alpha8: encoded colour bytes, a UNORM alpha byte. */
		texel[0] = (uint8_t)srgb(channels[0]);
		texel[1] = (uint8_t)srgb(channels[1]);
		texel[2] = (uint8_t)srgb(channels[2]);
		texel[3] = (uint8_t)unorm(channels[3], 8U);
		*bytes = 4U;
		break;
	case 31U:
	case 32U:
	case 33U:
	case 34U:
	case 35U:
	case 36U:
		/* rgba8i to r8ui: the low bytes. */
		count = 4U;
		if (format == 32U || format == 35U)
			count = 2U;
		if (format == 33U || format == 36U)
			count = 1U;
		for (index = 0; index < count; index++)
			texel[index] = (uint8_t)channels[index];
		*bytes = count;
		break;
	case 18U:
	case 19U:
	case 20U:
		/* rgba16f, rg16f and r16f: halves. */
		count = 4U;
		if (format == 19U)
			count = 2U;
		if (format == 20U)
			count = 1U;
		for (index = 0; index < count; index++)
			put(texel, index * 2U, half_float(channels[index]), 2U);
		*bytes = count * 2U;
		break;
	case 21U:
	case 22U:
	case 23U:
	case 24U:
	case 25U:
	case 26U:
		/* rgba16i to r16ui: the low halves. */
		count = 4U;
		if (format == 22U || format == 25U)
			count = 2U;
		if (format == 23U || format == 26U)
			count = 1U;
		for (index = 0; index < count; index++)
			put(texel, index * 2U, channels[index], 2U);
		*bytes = count * 2U;
		break;
	case 9U:
	case 10U:
	case 11U:
	case 12U:
	case 13U:
	case 14U:
	case 15U:
	case 16U:
	case 17U:
		/* rgba32f to r32ui: the words. */
		count = 4U;
		if ((format - 9U) % 3U == 1U)
			count = 2U;
		if ((format - 9U) % 3U == 2U)
			count = 1U;
		for (index = 0; index < count; index++)
			put(texel, index * 4U, channels[index], 4U);
		*bytes = count * 4U;
		break;
	case 3U:
		/* rgb10_a2: ten-bit UNORM red, green and blue, a two-bit alpha. */
		value = unorm(channels[0], 10U) | (unorm(channels[1], 10U) << 10) | (unorm(channels[2], 10U) << 20) | (unorm(channels[3], 2U) << 30);
		put(texel, 0, value, 4U);
		*bytes = 4U;
		break;
	case 2U:
		/* rgb10_a2ui: the low bits. */
		value = (channels[0] & 0x3ffU) | ((channels[1] & 0x3ffU) << 10) | ((channels[2] & 0x3ffU) << 20) | (channels[3] << 30);
		put(texel, 0, value, 4U);
		*bytes = 4U;
		break;
	case 8U:
		/* r11f_g11f_b10f: unsigned floats of six, six and five mantissa bits. */
		value = small_float(channels[0], 6U) | (small_float(channels[1], 6U) << 11) | (small_float(channels[2], 5U) << 22);
		put(texel, 0, value, 4U);
		*bytes = 4U;
		break;
	case 7U:
		/* bgr565: blue in the low bits, red in the high. */
		value = unorm(channels[2], 5U) | (unorm(channels[1], 6U) << 5) | (unorm(channels[0], 5U) << 11);
		put(texel, 0, value, 2U);
		*bytes = 2U;
		break;
	case 6U:
		/* abgr4444: alpha in the low bits, red in the high. */
		value = unorm(channels[3], 4U) | (unorm(channels[2], 4U) << 4) | (unorm(channels[1], 4U) << 8) | (unorm(channels[0], 4U) << 12);
		put(texel, 0, value, 2U);
		*bytes = 2U;
		break;
	case 4U:
		/* abgr1555: the alpha bit low, red in the high bits. */
		value = unorm(channels[3], 1U) | (unorm(channels[2], 5U) << 1) | (unorm(channels[1], 5U) << 6) | (unorm(channels[0], 5U) << 11);
		put(texel, 0, value, 2U);
		*bytes = 2U;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: the pixel's bytes. */
	return 0;
}

/* Converts one exact IEEE binary32 bit image to clamped nearest-even eight-bit UNORM. */
static uint32_t
convert_component(
	uint32_t word)
{
	/* The eight-bit case of the general conversion. */
	return unorm(word, 8U);
}

/*
 * Converts one exact IEEE binary32 bit image to clamped nearest-even UNORM
 * of width bits (1 to 16) using bounded integer shifts.
 *
 * Negative values, including negative zero and infinity, and NaNs give
 * zero; values at or above one give all ones.  Finite values round the
 * exact binary value times 2^width - 1 to the nearest, ties to even.
 */
static uint32_t
unorm(
	uint32_t word,
	uint32_t width)
{
	uint32_t fraction;
	uint32_t exponent;
	uint32_t shift;
	uint32_t rounded;
	uint32_t most;
	uint64_t product;
	uint64_t remainder;
	uint64_t halfway;

	/* A negative component, including negative zero or infinity, has the deterministic zero clamped result. */
	most = (1U << width) - 1U;
	if ((word & 0x80000000U) != 0)
		return 0;
	fraction = word & 0x7fffffU;
	exponent = (word >> 23) & 255U;

	/* A positive NaN has no finite normalized value and chooses zero rather than inheriting arbitrary payload bits. */
	if (exponent == 255U && fraction != 0)
		return 0;

	/* Every finite value at or above one and positive infinity clamp at the final component value. */
	if (word >= 0x3f800000U)
		return most;

	/* Exact significand multiplication needs at most forty bits before binary scaling. */
	shift = 149;
	if (exponent != 0) {
		fraction |= 0x800000U;
		shift = 150U - exponent;
	}

	/* Values this small cannot reach a half UNORM step; no oversized native shift is evaluated. */
	product = (uint64_t)fraction * most;
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

/*
 * Converts IEEE single bits to half-float bits, rounding to the nearest
 * (ties to even) as Mesa's _mesa_float_to_half does; too large a value
 * becomes infinity and a NaN stays a NaN.
 */
static uint32_t
half_float(
	uint32_t word)
{
	uint32_t sign;
	uint32_t exponent;
	uint32_t mantissa;
	uint32_t half;
	uint32_t shift;
	uint32_t remainder;
	uint32_t halfway;

	/* The sign moves to bit 15; the exponent and the mantissa are converted below. */
	sign = (word >> 16) & 0x8000U;
	exponent = (word >> 23) & 0xffU;
	mantissa = word & 0x7fffffU;

	/* Infinity and NaN keep their kind. */
	if (exponent == 0xffU) {
		if (mantissa != 0)
			return sign | 0x7e00U;
		return sign | 0x7c00U;
	}

	/* A value of 65536 or more is beyond the largest finite half. */
	if (exponent > 142U)
		return sign | 0x7c00U;

	/* A value below 2^-25 rounds to zero. */
	if (exponent < 102U)
		return sign;

	/*
	 * A normal half keeps ten mantissa bits and rebiases the exponent; a
	 * subnormal one shifts the mantissa, with its implicit bit, into the
	 * ten bits of 2^-24 units.  A carry out of the mantissa rounds up into
	 * the exponent, as it should.
	 */
	if (exponent >= 113U) {
		half = ((exponent - 112U) << 10) | (mantissa >> 13);
		shift = 13U;
	} else {
		mantissa |= 0x800000U;
		shift = 126U - exponent;
		half = mantissa >> shift;
	}

	/* The dropped bits round to the nearest, ties to even. */
	remainder = mantissa & ((1U << shift) - 1U);
	halfway = 1U << (shift - 1U);
	if (remainder > halfway ||
	    (remainder == halfway && (half & 1U) != 0))
		half++;

	/* Succeeded: the half's bits. */
	return sign | half;
}

/*
 * Converts IEEE single bits to an unsigned float of a five-bit exponent
 * (bias 15) and mantissa_bits (6 or 5) mantissa bits, rounding to the
 * nearest, ties to even: negative values give zero, a NaN a NaN, infinity
 * infinity, and a finite value beyond the largest the largest.
 */
static uint32_t
small_float(
	uint32_t word,
	uint32_t mantissa_bits)
{
	uint32_t exponent;
	uint32_t mantissa;
	uint32_t value;
	uint32_t shift;
	uint32_t remainder;
	uint32_t halfway;
	uint32_t largest;

	/* The field's infinity, NaN and largest finite value. */
	exponent = (word >> 23) & 0xffU;
	mantissa = word & 0x7fffffU;
	largest = (30U << mantissa_bits) | ((1U << mantissa_bits) - 1U);

	/* A NaN keeps its kind; another negative value, or zero, is zero; infinity stays. */
	if (exponent == 0xffU && mantissa != 0)
		return (31U << mantissa_bits) | 1U;
	if ((word & 0x80000000U) != 0)
		return 0;
	if (exponent == 0xffU)
		return 31U << mantissa_bits;

	/* Beyond 65536 is beyond the largest finite value; below the smallest step's half is zero. */
	if (exponent > 142U)
		return largest;
	if (exponent < 112U - mantissa_bits)
		return 0;

	/* A normal value keeps its top mantissa bits under a rebiased exponent; a subnormal one shifts its whole significand into units of the smallest step. */
	if (exponent >= 113U) {
		value = ((exponent - 112U) << mantissa_bits) | (mantissa >> (23U - mantissa_bits));
		shift = 23U - mantissa_bits;
	} else {
		mantissa |= 0x800000U;
		shift = 136U - mantissa_bits - exponent;
		value = mantissa >> shift;
	}

	/* The dropped bits round to the nearest, ties to even; a rounding past the largest stays the largest. */
	remainder = mantissa & ((1U << shift) - 1U);
	halfway = 1U << (shift - 1U);
	if (remainder > halfway ||
	    (remainder == halfway && (value & 1U) != 0))
		value++;
	if (value > largest)
		value = largest;

	/* Succeeded: the field's bits. */
	return value;
}

/* Encodes one linear channel (IEEE single bits) as the nearest 8-bit sRGB code. */
static uint32_t
srgb(
	uint32_t word)
{
	uint32_t code;

	/* Negative values, negative zero and NaNs are code 0; a positive value is compared as an unsigned word. */
	if ((word & 0x80000000U) != 0 || word > 0x7f800000U)
		return 0;

	/* The steps at or below the value. */
	code = 0;
	while (code < 255U && colour_srgb_steps[code] <= word)
		code++;

	/* Succeeded: the code. */
	return code;
}

/* Stores the low bytes of value at offset of a pixel, little-endian. */
static void
put(
	uint8_t *texel,
	uint32_t offset,
	uint32_t value,
	uint32_t bytes)
{
	uint32_t index;

	/* Each byte from the lowest. */
	for (index = 0; index < bytes; index++)
		texel[offset + index] = (uint8_t)(value >> (index * 8U));

	/* Succeeded: the bytes are stored. */
	return;
}
