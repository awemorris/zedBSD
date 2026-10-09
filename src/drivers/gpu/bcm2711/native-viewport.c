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

static uint32_t half_dimension(uint32_t bits);
static uint32_t add_magnitudes(uint32_t left, uint32_t right);
static uint32_t add_signed(uint32_t left, uint32_t right);
static uint32_t offset_word(uint32_t coordinate, uint32_t dimension);
static uint32_t positive_fixed(uint32_t bits);
static uint32_t integer_bits(uint32_t value);
static int32_t truncate_signed(uint32_t bits);
static uint32_t scale_xy(uint32_t bits);
static uint32_t subtract_magnitudes(uint32_t left, uint32_t right);
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
	prepared.depth_scale = subtract_magnitudes(words[5], words[4]);
	prepared.depth_offset = words[4];
	*viewport = prepared;

	/* Succeeded: native shaders and later clipper records can share the same exact viewport depth convention. */
	return 0;
}

/*
 * Computes native fine/coarse viewport offsets and the 4.2 clipper's nondegenerate depth transform.
 *
 * Exact shader depth remains unchanged.  Only clipper guardband arithmetic
 * uses the minimum nonzero range needed by 4.2 hardware; min/max planes use
 * that same clipped transform.  All calculations use integer IEEE bits.
 */
int
bcm2711_native_viewport_clip_prepare(
	const uint32_t words[6],
	struct bcm2711_native_viewport_clip *clip)
{
	struct bcm2711_native_viewport viewport;
	struct bcm2711_native_viewport_clip prepared;
	uint32_t end;
	uint32_t temporary;
	uint32_t magnitude;
	uint32_t half;
	uint32_t centre;
	uint32_t edge;
	uint32_t axis;
	int error;

	/* Atomic output cannot be supplied through absent storage. */
	if (clip == NULL)
		return EINVAL;
	error = bcm2711_native_viewport_prepare(words, &viewport);
	if (error != 0)
		return error;

	/* Hardware unsigned fine coordinates are accompanied by signed coarse offsets in units of sixty-four pixels. */
	prepared.x_offset = offset_word(words[0], words[2]);
	prepared.y_offset = offset_word(words[1], words[3]);
	prepared.depth_scale = viewport.depth_scale;
	prepared.depth_offset = viewport.depth_offset;

	/* Native 4.2 guardband clipping fails with very small depth scales; 0.0005 is the audited finite minimum. */
	magnitude = prepared.depth_scale & 0x7fffffffU;
	if (magnitude < 0x3a03126fU) {
		prepared.depth_scale = 0x3a03126fU;
		if (magnitude != 0 && (viewport.depth_scale & 0x80000000U) != 0)
			prepared.depth_scale |= 0x80000000U;
	}

	/* Plane bounds follow the same once-rounded native transform, including reversed depth ranges. */
	end = add_signed(prepared.depth_offset, prepared.depth_scale);
	prepared.minimum = prepared.depth_offset;
	prepared.maximum = end;
	if ((prepared.depth_scale & 0x80000000U) != 0) {
		temporary = prepared.minimum;
		prepared.minimum = prepared.maximum;
		prepared.maximum = temporary;
	}

	/* Guardband clipping also needs the viewport's integer edges before intersecting the drawable and user scissor. */
	for (axis = 0; axis < 2; axis++) {
		half = half_dimension(words[axis + 2U]);
		centre = add_signed(words[axis], half);
		edge = add_signed(centre, half | 0x80000000U);
		prepared.bounds[axis] = truncate_signed(edge);
		edge = add_signed(centre, half);
		prepared.bounds[axis + 2U] = truncate_signed(edge);
	}

	/* No failed validation publishes even part of the clipper state. */
	*clip = prepared;

	/* Succeeded: later packet encoding needs no floating-point arithmetic or borrowed source words. */
	return 0;
}

/* Halves a finite positive dimension with nearest-even rounding at the normal/subnormal boundary. */
static uint32_t
half_dimension(
	uint32_t bits)
{
	uint32_t exponent;
	uint32_t significand;
	uint32_t rounded;

	/* A normal exponent above one is halved exactly. */
	exponent = (bits >> 23) & 255U;
	if (exponent > 1U)
		return bits - (1U << 23);

	/* Tiny dimensions may lose one low bit when halved into subnormal storage. */
	significand = bits & 0x7fffffU;
	if (exponent == 1U)
		significand |= 0x800000U;
	rounded = significand >> 1;
	if ((significand & 1U) != 0 && (rounded & 1U) != 0)
		rounded++;

	/* The raw rounded significand also represents the possible smallest-normal carry. */
	return rounded;
}

/* Adds two admitted finite magnitudes using guard/round/sticky bits and a single nearest-even rounding. */
static uint32_t
add_magnitudes(
	uint32_t left,
	uint32_t right)
{
	uint32_t left_exponent;
	uint32_t right_exponent;
	uint32_t left_significand;
	uint32_t right_significand;
	uint32_t temporary;
	uint32_t rounded;
	uint32_t remainder;
	uint64_t sum;
	uint64_t aligned;

	/* Larger magnitudes have no smaller effective exponents, including the subnormal boundary. */
	if (left < right) {
		temporary = left;
		left = right;
		right = temporary;
	}

	/* Effective exponents align both exact significands before a single final rounding. */
	left_significand = decode_significand(left, &left_exponent);
	right_significand = decode_significand(right, &right_exponent);
	aligned = shift_sticky((uint64_t)right_significand << 3, left_exponent - right_exponent);
	sum = ((uint64_t)left_significand << 3) + aligned;

	/* A carry shifts all retained information once and preserves the discarded sticky bit. */
	if (sum >= 0x8000000U) {
		sum = (sum >> 1) | (sum & 1U);
		left_exponent++;
	}

	/* Exactly halfway values retain the even significand; larger remainders round upward. */
	rounded = (uint32_t)(sum >> 3);
	remainder = (uint32_t)(sum & 7U);
	if (remainder > 4U ||
	    (remainder == 4U &&
	     (rounded & 1U) != 0))
		rounded++;
	if (rounded == 0x1000000U) {
		rounded >>= 1;
		left_exponent++;
	}

	/* Subnormal sums have no implicit leading bit. */
	if (rounded < 0x800000U)
		return rounded;

	/* Finite admitted coordinates cannot approach an exponent overflow. */
	return (left_exponent << 23) | (rounded & 0x7fffffU);
}

/* Adds finite signed words within the viewport domain, preserving ordinary nearest-even cancellation. */
static uint32_t
add_signed(
	uint32_t left,
	uint32_t right)
{
	uint32_t left_magnitude;
	uint32_t right_magnitude;
	uint32_t result;

	/* Equal signs use magnitude addition and keep negative zero only when both zero operands were negative. */
	left_magnitude = left & 0x7fffffffU;
	right_magnitude = right & 0x7fffffffU;
	if (((left ^ right) & 0x80000000U) == 0) {
		result = add_magnitudes(left_magnitude, right_magnitude);
		result |= left & 0x80000000U;
	} else {
		/* Opposite signs reduce to ordered magnitude subtraction; exact cancellation is positive zero. */
		if ((left & 0x80000000U) != 0)
			result = subtract_magnitudes(right_magnitude, left_magnitude);
		else
			result = subtract_magnitudes(left_magnitude, right_magnitude);
	}

	/* The signed sum is fully rounded before later native fixed-point packing. */
	return result;
}

/* Encodes one centre coordinate using an unsigned u14.8 fine value and a signed coarse sixty-four-pixel offset. */
static uint32_t
offset_word(
	uint32_t coordinate,
	uint32_t dimension)
{
	uint32_t half;
	uint32_t centre;
	uint32_t magnitude;
	uint32_t significand;
	uint32_t exponent;
	uint32_t shift;
	uint32_t blocks;
	uint32_t coarse;
	uint32_t fine;
	uint32_t adjustment;

	/* The half dimension and centre follow the same two nearest-even IEEE operations as native viewport setup. */
	half = half_dimension(dimension);
	centre = add_signed(coordinate, half);
	magnitude = centre & 0x7fffffffU;
	coarse = 0;

	/* Negative centres need a whole number of sixty-four-pixel blocks before unsigned fine packing. */
	if ((centre & 0x80000000U) != 0 && magnitude != 0) {
		significand = decode_significand(magnitude, &exponent);
		shift = 156U - exponent;
		blocks = 1;
		if (shift < 32U) {
			blocks = significand >> shift;
			if ((significand & (((uint32_t)1 << shift) - 1U)) != 0)
				blocks++;
		}

		/* Every adjustment is an exactly representable bounded integer; its addition rounds only once. */
		adjustment = integer_bits(blocks * 64U);
		centre = subtract_magnitudes(adjustment, magnitude);
		coarse = (0U - blocks) & 1023U;
	}

	/* Fixed-point rounding uses ties away from zero, matching the unsigned native packet field. */
	fine = positive_fixed(centre & 0x7fffffffU);

	/* Admitted centres fit fine's fourteen integer bits and coarse's signed ten-bit range. */
	return (coarse << 22) | fine;
}

/* Rounds an admitted positive IEEE value times 256 to the nearest integer, with half ties rounded upward. */
static uint32_t
positive_fixed(
	uint32_t bits)
{
	uint32_t exponent;
	uint32_t significand;
	uint32_t shift;
	uint32_t rounded;

	/* Fine coordinates are bounded below 16384 pixels, so all nonzero results use a finite right shift. */
	significand = decode_significand(bits, &exponent);
	shift = 142U - exponent;
	if (shift >= 32U)
		return 0;
	rounded = significand >> shift;

	/* The native unsigned fixed-point packer resolves exact half ties away from zero. */
	if ((significand & ((uint32_t)1 << (shift - 1U))) != 0)
		rounded++;

	/* The complete native fine value has no floating-point source dependency. */
	return rounded;
}

/* Encodes one bounded nonzero integer exactly as an IEEE word without a kernel floating-point conversion. */
static uint32_t
integer_bits(
	uint32_t value)
{
	uint32_t leading;
	uint32_t probe;

	/* At most 4096 pixels of coarse adjustment fit well below the twenty-four-bit significand precision. */
	leading = 0;
	probe = value;
	while (probe > 1U) {
		probe >>= 1;
		leading++;
	}

	/* The explicit exponent and mantissa preserve every integer bit exactly. */
	return ((127U + leading) << 23) | ((value << (23U - leading)) & 0x7fffffU);
}

/* Converts one admitted finite signed viewport edge to an integer pixel coordinate with truncation toward zero. */
static int32_t
truncate_signed(
	uint32_t bits)
{
	uint32_t exponent;
	uint32_t significand;
	uint32_t shift;
	int32_t result;

	/* Viewport centres and edges stay below 8192, so normal integer conversion uses a bounded right shift. */
	significand = decode_significand(bits & 0x7fffffffU, &exponent);
	shift = 150U - exponent;
	result = 0;
	if (shift < 32U)
		result = (int32_t)(significand >> shift);
	if ((bits & 0x80000000U) != 0)
		result = -result;

	/* The signed pixel edge preserves truncation without any kernel floating-point conversion. */
	return result;
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

/* Subtracts two bounded finite nonnegative viewport magnitudes with three guard/round/sticky bits. */
static uint32_t
subtract_magnitudes(
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
	if (remainder > 4U ||
	    (remainder == 4U &&
	     (rounded & 1U) != 0))
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
