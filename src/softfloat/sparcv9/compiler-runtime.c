/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/*
 * PIC-safe integer helpers needed by the SPARC V9 dynamic userland.  Keeping
 * these here avoids linking the compiler toolchain's non-PIC runtime archive.
 */
int
__clzdi2(unsigned long long value)
{
	int count = 0;
	unsigned long long mask = 1ULL << 63;

	while (mask != 0 && (value & mask) == 0) {
		count++;
		mask >>= 1;
	}
	return count;
}

int
__clzsi2(unsigned int value)
{
	int count = 0;
	unsigned int mask = 1U << 31;

	while (mask != 0 && (value & mask) == 0) {
		count++;
		mask >>= 1;
	}
	return count;
}
