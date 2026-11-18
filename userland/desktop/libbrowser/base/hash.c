/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Hashing for the browser's tables: 32-bit FNV-1a over bytes or UTF-16
 * code units.
 *
 * The tables hash names the page chooses (property names, class names), so
 * a table must not degrade badly when many names share a hash; the tables
 * grow and chain rather than rely on the hash being hard to collide.
 */

#include "base/base.h"

/* The FNV-1a offset basis and prime for 32 bits. */
#define HASH_OFFSET_BASIS	2166136261U
#define HASH_PRIME		16777619U

/*
 * Hashes length bytes.
 */
uint32_t
wb_hash_bytes(
	const void *bytes,
	size_t length)
{
	const unsigned char *byte;
	uint32_t hash;
	size_t index;

	/* Folds each byte into the hash. */
	byte = bytes;
	hash = HASH_OFFSET_BASIS;
	for (index = 0; index < length; index++) {
		hash ^= byte[index];
		hash *= HASH_PRIME;
	}

	/* Reports the hash. */
	return hash;
}

/*
 * Hashes length UTF-16 code units.
 *
 * A unit below 256 hashes like the byte of the same value, so a Latin-1
 * string and its UTF-16 form hash alike.
 */
uint32_t
wb_hash_units(
	const uint16_t *units,
	size_t length)
{
	uint32_t hash;
	size_t index;

	/* Folds each unit into the hash, the high byte only when it is not zero. */
	hash = HASH_OFFSET_BASIS;
	for (index = 0; index < length; index++) {
		hash ^= units[index] & 0xffU;
		hash *= HASH_PRIME;
		if (units[index] > 0xffU) {
			hash ^= units[index] >> 8;
			hash *= HASH_PRIME;
		}
	}

	/* Reports the hash. */
	return hash;
}
