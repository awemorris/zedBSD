/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checksums of libz-compat: Adler-32 (the zlib wrapper's, RFC 1950)
 * and CRC-32 (PNG's chunks, gzip), computed a byte and a bit at a time.
 */

#include <compat/zlib/zlib.h>

/* Adler-32's modulus, the largest prime below 65536. */
#define ADLER_BASE		65521U

/* CRC-32's polynomial, bit-reflected. */
#define CRC_POLYNOMIAL		0xedb88320U

/*
 * Continues an Adler-32 over a buffer; a null buffer gives the starting
 * value, 1.
 */
uLong
adler32(
	uLong adler,
	const Bytef *buf,
	uInt len)
{
	unsigned long low;
	unsigned long high;
	uInt index;

	/* The starting value. */
	if (buf == NULL)
		return 1UL;

	/* The two sums, each byte added to the low one and the low one to the high one. */
	low = adler & 0xffffUL;
	high = (adler >> 16) & 0xffffUL;
	for (index = 0; index < len; index++) {
		low = (low + buf[index]) % ADLER_BASE;
		high = (high + low) % ADLER_BASE;
	}

	/* The high sum above the low one. */
	return (high << 16) | low;
}

/*
 * Continues a CRC-32 over a buffer; a null buffer gives the starting
 * value, 0.
 */
uLong
crc32(
	uLong crc,
	const Bytef *buf,
	uInt len)
{
	unsigned long value;
	uInt index;
	unsigned bit;

	/* The starting value. */
	if (buf == NULL)
		return 0UL;

	/* Each byte, a bit at a time, on the inverted register. */
	value = ~crc & 0xffffffffUL;
	for (index = 0; index < len; index++) {
		value ^= buf[index];
		for (bit = 0; bit < 8U; bit++) {
			if ((value & 1UL) != 0UL) {
				value = (value >> 1) ^ CRC_POLYNOMIAL;
			} else {
				value >>= 1;
			}
		}
	}

	/* The register inverted back. */
	return ~value & 0xffffffffUL;
}
