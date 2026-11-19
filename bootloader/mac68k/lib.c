/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 68040 Macintosh second stage's small helpers: copying, filling,
 * big-endian fields, the CRC-32 and string lengths.
 */

#include "bootloader/mac68k/stage2.h"

/* The reflected polynomial of zlib's CRC-32. */
#define CRC32_POLYNOMIAL	0xedb88320U

/*
 * Copies bytes from one buffer to another that does not overlap it.
 */
void
mac68k_copy_bytes(
	void *target,
	const void *source,
	uint32_t length)
{
	uint8_t *to;
	const uint8_t *from;
	uint32_t index;

	/* Copies one byte at a time; the buffers are small or copied once. */
	to = target;
	from = source;
	for (index = 0; index < length; index++)
		to[index] = from[index];
}

/*
 * Fills a buffer with one byte value.
 */
void
mac68k_fill_bytes(
	void *target,
	int value,
	uint32_t length)
{
	uint8_t *to;
	uint32_t index;

	/* Stores the value in every byte. */
	to = target;
	for (index = 0; index < length; index++)
		to[index] = (uint8_t)value;
}

/*
 * Reads a big-endian 32-bit field.
 */
uint32_t
mac68k_read_be32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* Most significant byte first. */
	value = (uint32_t)bytes[0] << 24;
	value |= (uint32_t)bytes[1] << 16;
	value |= (uint32_t)bytes[2] << 8;
	value |= (uint32_t)bytes[3];

	/* Succeeded: the field's value. */
	return value;
}

/*
 * Reads a big-endian 16-bit field.
 */
uint16_t
mac68k_read_be16(
	const uint8_t *bytes)
{
	uint16_t value;

	/* Most significant byte first. */
	value = (uint16_t)(((uint32_t)bytes[0] << 8) | bytes[1]);

	/* Succeeded: the field's value. */
	return value;
}

/*
 * Writes a big-endian 32-bit field.
 */
void
mac68k_write_be32(
	uint8_t *bytes,
	uint32_t value)
{
	/* Most significant byte first. */
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}

/*
 * Writes a big-endian 16-bit field.
 */
void
mac68k_write_be16(
	uint8_t *bytes,
	uint16_t value)
{
	/* Most significant byte first. */
	bytes[0] = (uint8_t)(value >> 8);
	bytes[1] = (uint8_t)value;
}

/*
 * Sums bytes with zlib's CRC-32, the sum the image's tool writes.
 */
uint32_t
mac68k_crc32(
	const uint8_t *bytes,
	uint32_t length)
{
	uint32_t crc;
	uint32_t index;
	unsigned bit;

	/* Folds every byte in, low bit first. */
	crc = 0xffffffffU;
	for (index = 0; index < length; index++) {
		crc ^= bytes[index];
		for (bit = 0; bit < 8U; bit++) {
			/* Divides by the polynomial where the low bit is set. */
			if ((crc & 1U) != 0U) {
				crc = (crc >> 1) ^ CRC32_POLYNOMIAL;
			} else {
				crc >>= 1;
			}
		}
	}

	/* Succeeded: the sum, inverted as zlib's is. */
	return crc ^ 0xffffffffU;
}

/*
 * Counts the characters of a string before its terminator.
 */
uint32_t
mac68k_string_length(
	const char *text)
{
	uint32_t length;

	/* Steps up to the terminator. */
	length = 0;
	while (text[length] != '\0')
		length++;

	/* Succeeded: the length. */
	return length;
}
