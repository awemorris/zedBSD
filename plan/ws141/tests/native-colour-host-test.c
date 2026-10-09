/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Independent host floating-point conversion checks the kernel's integer-only clear arithmetic without physical GPU execution. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-colour.h"

static void verify_component(uint32_t word);

/*
 * Compares normalized colour conversion at every quantization boundary and selected exact IEEE extremes.
 */
int
main(
	void)
{
	const uint32_t edges[] = {0, 1, 0x7fffffU, 0x800000U, 0x3f000000U, 0x3f7fffffU, 0x3f800000U, 0x3f800001U, 0x7f7fffffU, 0x7f800000U, 0x7fc00000U, 0x7fffffffU, 0x80000000U, 0xbf000000U, 0xff800000U, 0xffc00000U};
	uint32_t words[4];
	uint32_t colour;
	uint32_t word;
	uint32_t seed;
	uint32_t index;
	uint32_t component;
	float boundary;
	int error;

	/* Selected finite, zero, subnormal, infinity and NaN cases have deterministic independently checked clamping. */
	for (index = 0; index < sizeof(edges) / sizeof(edges[0]); index++)
		verify_component(edges[index]);

	/* Neighbours on both sides of all 255 quantization thresholds exercise rounding without a copied integer formula. */
	for (index = 0; index < 255U; index++) {
		boundary = (float)(((double)index + 0.5) / 255.0);
		memcpy(&word, &boundary, sizeof(word));
		verify_component(word - 1U);
		verify_component(word);
		verify_component(word + 1U);
	}

	/* Seeded finite in-range values supplement boundary checks without a timing-dependent corpus. */
	seed = 0x43a9e187U;
	for (index = 0; index < 256U; index++) {
		seed = seed * 1664525U + 1013904223U;
		word = seed % 0x3f800001U;
		verify_component(word);
	}

	/* Distinct raw component words verify complete RGBA/BGRA ordering and output-alias safety. */
	words[0] = 0x3e800000U;
	words[1] = 0x3f000000U;
	words[2] = 0x3f400000U;
	words[3] = 0x3f800000U;
	error = bcm2711_native_colour_pack(words, 0, &colour);
	assert(error == 0 && colour == 0xffbf8040U);
	error = bcm2711_native_colour_pack(words, 1, &colour);
	assert(error == 0 && colour == 0xff4080bfU);
	error = bcm2711_native_colour_pack(words, 0, &words[0]);
	assert(error == 0 && words[0] == 0xffbf8040U);

	/* Refusal never publishes partial output or reads missing input. */
	colour = 0x12345678U;
	error = bcm2711_native_colour_pack(words, 2, &colour);
	assert(error == EINVAL && colour == 0x12345678U);
	error = bcm2711_native_colour_pack(NULL, 0, &colour);
	assert(error == EINVAL && colour == 0x12345678U);
	for (component = 0; component < 4U; component++)
		words[component] = 0;
	error = bcm2711_native_colour_pack(words, 0, NULL);
	assert(error == EINVAL);
	puts("native-colour-host: PASS (1037 independent IEEE cases, RGBA/BGRA order and alias-safe atomic output)");

	/* Succeeded: only the fixture used floating-point operations; production conversion remains integer-only. */
	return 0;
}

/* Compares one raw IEEE word with independently evaluated double-precision normalized quantization. */
static void
verify_component(
	uint32_t word)
{
	uint32_t words[4];
	uint32_t colour;
	uint32_t expected;
	uint32_t index;
	float component;
	int error;
	int nan;

	/* Host IEEE interpretation evaluates the exact finite binary32 value before normalized quantization in double precision. */
	memcpy(&component, &word, sizeof(component));
	expected = 0;
	nan = isnan(component);
	if (nan || component <= 0)
		expected = 0;
	else if (component >= 1)
		expected = 255;
	else
		expected = (uint32_t)lround((double)component * 255.0);

	/* Four equal channels verify the entire packed storage word rather than a private conversion helper. */
	for (index = 0; index < 4U; index++)
		words[index] = word;
	error = bcm2711_native_colour_pack(words, 0, &colour);
	assert(error == 0 && colour == expected * 0x01010101U);

	/* Succeeded: exact native clear quantization matches an independently evaluated host operation. */
	return;
}
