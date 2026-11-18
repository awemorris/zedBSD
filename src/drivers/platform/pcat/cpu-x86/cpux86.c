/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bit-field helpers every x86 processor decoder uses.
 */

#include "cpux86.h"

/*
 * Extracts a field of a 32-bit register.
 *
 * A position outside the register gives 0, so a decoder handed a bad
 * constant reads nothing rather than shifting by the register's width.
 */
uint32_t
cpux86_field32(
	uint32_t value,
	uint32_t low,
	uint32_t width)
{
	uint32_t shifted;

	/*
	 * Refuses a field that is empty or does not lie inside the register;
	 * the last clause runs only once low is known to be below 32.
	 */
	if (width == 0U ||
	    width > 32U ||
	    low >= 32U ||
	    width > 32U - low)
		return 0U;

	/* Brings the field down to bit 0. */
	shifted = value >> low;

	/* A field that reaches the top bit needs no mask. */
	if (width == 32U)
		return shifted;

	/* Succeeded: keeps the field's own bits only. */
	return shifted & (((uint32_t)1U << width) - 1U);
}

/*
 * Extracts a field of a 64-bit register.
 *
 * A position outside the register gives 0, as for the 32-bit form.
 */
uint64_t
cpux86_field64(
	uint64_t value,
	uint32_t low,
	uint32_t width)
{
	uint64_t shifted;

	/*
	 * Refuses a field that is empty or does not lie inside the register;
	 * the last clause runs only once low is known to be below 64.
	 */
	if (width == 0U ||
	    width > 64U ||
	    low >= 64U ||
	    width > 64U - low)
		return 0U;

	/* Brings the field down to bit 0. */
	shifted = value >> low;

	/* A field that reaches the top bit needs no mask. */
	if (width == 64U)
		return shifted;

	/* Succeeded: keeps the field's own bits only. */
	return shifted & (((uint64_t)1U << width) - 1U);
}

/*
 * Replaces a field of a 64-bit register.
 *
 * A position outside the register leaves the value as it was.
 */
uint64_t
cpux86_insert64(
	uint64_t value,
	uint32_t low,
	uint32_t width,
	uint64_t field)
{
	uint64_t mask;

	/*
	 * Leaves the register alone for a field it does not have; the last
	 * clause runs only once low is known to be below 64.
	 */
	if (width == 0U ||
	    width > 64U ||
	    low >= 64U ||
	    width > 64U - low)
		return value;

	/* Builds the mask of the field's width without a shift by 64. */
	if (width == 64U) {
		mask = ~(uint64_t)0U;
	} else {
		mask = ((uint64_t)1U << width) - 1U;
	}

	/* Clears the field and puts the low bits of the new contents there. */
	value &= ~(mask << low);
	value |= (field & mask) << low;

	/* Succeeded: the register with the new field. */
	return value;
}

/*
 * Reports the number of bits needed to tell count things apart.
 *
 * Counts above 2^31 need a 32-bit field; every smaller count is found by
 * doubling a power of two until it reaches the count.
 */
uint32_t
cpux86_ceil_log2(
	uint32_t count)
{
	uint32_t bits;
	uint32_t reach;

	/* A count above 2^31 does not fit below the top bit. */
	if (count > ((uint32_t)1U << 31))
		return 32U;

	/*
	 * Doubles the reach until it covers the count; 0 and 1 need no bit,
	 * and the reach never passes 2^31 because the count does not.
	 */
	bits = 0U;
	reach = 1U;
	while (reach < count) {
		reach <<= 1;
		bits++;
	}

	/* Succeeded: the width that reaches count instances. */
	return bits;
}
