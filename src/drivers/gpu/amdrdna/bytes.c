/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the little-endian words of the tables the GPU and its video BIOS
 * carry, at any byte offset.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * Reads a little-endian 16-bit word at a byte offset.
 */
uint16_t
amdrdna_le16(
	const uint8_t *bytes,
	size_t at)
{
	uint16_t word;

	/* Puts the high byte above the low one. */
	word = (uint16_t)bytes[at];
	word |= (uint16_t)((uint16_t)bytes[at + 1U] << 8);

	/* The assembled word. */
	return word;
}

/*
 * Reads a little-endian 32-bit word at a byte offset.
 */
uint32_t
amdrdna_le32(
	const uint8_t *bytes,
	size_t at)
{
	uint32_t word;

	/* Puts the four bytes in order of significance. */
	word = (uint32_t)bytes[at];
	word |= (uint32_t)bytes[at + 1U] << 8;
	word |= (uint32_t)bytes[at + 2U] << 16;
	word |= (uint32_t)bytes[at + 3U] << 24;

	/* The assembled word. */
	return word;
}
