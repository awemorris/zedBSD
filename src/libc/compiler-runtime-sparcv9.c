/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PIC-safe integer helpers for the SPARC V9 dynamic userland.
 *
 * The compiler toolchain ships these in a non-PIC runtime archive, which a
 * shared object cannot link against.  Keeping our own here avoids pulling
 * that archive into the dynamic userland at all.
 */

/*
 * Counts the leading zero bits of a 64-bit value.
 */
int
__clzdi2(
	unsigned long long value)
{
	unsigned long long mask;
	int count;

	/* Walks down from the top bit until a set bit stops the search. */
	count = 0;
	mask = 1ULL << 63;
	while (mask != 0 && (value & mask) == 0) {
		count++;
		mask >>= 1;
	}

	/* Reports how many bits were empty. */
	return count;
}

/*
 * Counts the leading zero bits of a 32-bit value.
 */
int
__clzsi2(
	unsigned int value)
{
	unsigned int mask;
	int count;

	/* Walks down from the top bit until a set bit stops the search. */
	count = 0;
	mask = 1U << 31;
	while (mask != 0 && (value & mask) == 0) {
		count++;
		mask >>= 1;
	}

	/* Reports how many bits were empty. */
	return count;
}
