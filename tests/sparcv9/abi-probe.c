/*
 * SPARC V9 compiler ABI probe.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

volatile unsigned long sparcv9_probe_sink;

unsigned long
sparcv9_abi_probe(unsigned long a, unsigned long b, unsigned long c,
		  unsigned long d, unsigned long e, unsigned long f)
{
	unsigned long value;

	value = (a ^ b) + (c << 7);
	value ^= (d >> 3) + e * 17UL;
	value += f;
	sparcv9_probe_sink = value;
	return value;
}
