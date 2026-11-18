/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: reading and writing the chip's multi-byte fields.
 *
 * The chip's bus headers and control messages are little-endian; the
 * event messages its firmware sends inside Ethernet frames are
 * big-endian.  Every field is moved a byte at a time, so neither the CPU's
 * byte order nor the buffer's alignment matters.
 */

#ifndef KERN_DRIVERS_BCM43455_BYTES_H
#define KERN_DRIVERS_BCM43455_BYTES_H

#include <stdint.h>

/* Reads a little-endian 16-bit field. */
static __inline uint16_t
bcm43455_get_le16(
	const uint8_t *bytes)
{
	uint16_t value;

	/* The low byte comes first. */
	value = (uint16_t)bytes[0];
	value |= (uint16_t)((uint16_t)bytes[1] << 8);

	/* Succeeded: the field's value. */
	return value;
}

/* Reads a little-endian 32-bit field. */
static __inline uint32_t
bcm43455_get_le32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The lowest byte comes first. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* Succeeded: the field's value. */
	return value;
}

/* Reads a big-endian 16-bit field. */
static __inline uint16_t
bcm43455_get_be16(
	const uint8_t *bytes)
{
	uint16_t value;

	/* The high byte comes first. */
	value = (uint16_t)((uint16_t)bytes[0] << 8);
	value |= (uint16_t)bytes[1];

	/* Succeeded: the field's value. */
	return value;
}

/* Reads a big-endian 32-bit field. */
static __inline uint32_t
bcm43455_get_be32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The highest byte comes first. */
	value = (uint32_t)bytes[0] << 24;
	value |= (uint32_t)bytes[1] << 16;
	value |= (uint32_t)bytes[2] << 8;
	value |= (uint32_t)bytes[3];

	/* Succeeded: the field's value. */
	return value;
}

/* Writes a little-endian 16-bit field. */
static __inline void
bcm43455_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	/* The low byte goes first. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)(value >> 8);
}

/* Writes a little-endian 32-bit field. */
static __inline void
bcm43455_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	/* The lowest byte goes first. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)((value >> 8) & 0xffU);
	bytes[2] = (uint8_t)((value >> 16) & 0xffU);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Writes a big-endian 16-bit field, as the event frames carry them. */
static __inline void
bcm43455_put_be16(
	uint8_t *bytes,
	uint16_t value)
{
	/* The high byte goes first. */
	bytes[0] = (uint8_t)(value >> 8);
	bytes[1] = (uint8_t)(value & 0xffU);
}

/* Writes a big-endian 32-bit field, as the event frames carry them. */
static __inline void
bcm43455_put_be32(
	uint8_t *bytes,
	uint32_t value)
{
	/* The highest byte goes first. */
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)((value >> 16) & 0xffU);
	bytes[2] = (uint8_t)((value >> 8) & 0xffU);
	bytes[3] = (uint8_t)(value & 0xffU);
}

#endif
