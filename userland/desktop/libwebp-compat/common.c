/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The helpers of arithmetic and byte order every part of the library uses.
 */

#include "common.h"

/*
 * Reads a little-endian 16-bit unsigned integer.
 */
uint32_t
webpc_le16(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The low byte first. */
	value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
	return value;
}

/*
 * Reads a little-endian 24-bit unsigned integer.
 */
uint32_t
webpc_le24(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The low byte first. */
	value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16);
	return value;
}

/*
 * Reads a little-endian 32-bit unsigned integer.
 */
uint32_t
webpc_le32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The low byte first. */
	value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
	return value;
}

/*
 * Clamps a value to 0..255.
 */
int
webpc_clamp255(
	int value)
{
	/* Below the range. */
	if (value < 0)
		return 0;

	/* Above the range. */
	if (value > 255)
		return 255;

	/* Within the range: the value itself. */
	return value;
}

/*
 * Shifts a signed value right with the floor a format's ">>" means.
 */
int32_t
webpc_asr(
	int32_t value,
	unsigned shift)
{
	uint32_t magnitude;
	uint32_t ceiling;
	int32_t quotient;

	/* A value that is not negative shifts as it is. */
	if (value >= 0)
		return value >> shift;

	/*
	 * A negative value: the floor of value / 2^shift is minus the ceiling
	 * of -value / 2^shift, computed on the unsigned magnitude (which
	 * -2^31 also has).  The ceiling is at least 1, so minus it is formed
	 * without overflow as -(ceiling - 1) - 1.
	 */
	magnitude = (uint32_t)0 - (uint32_t)value;
	ceiling = (magnitude >> shift) + ((magnitude & (((uint32_t)1 << shift) - 1U)) != 0U ? 1U : 0U);
	quotient = -(int32_t)(ceiling - 1U) - 1;
	return quotient;
}

/*
 * Wraps a value to the 16-bit two's complement range.
 */
int32_t
webpc_wrap16(
	int32_t value)
{
	uint32_t low;
	int32_t wrapped;

	/* The low 16 bits, read as a signed 16-bit value. */
	low = (uint32_t)value & 0xffffU;
	if (low >= 0x8000U)
		wrapped = (int32_t)low - 0x10000;
	else
		wrapped = (int32_t)low;
	return wrapped;
}
