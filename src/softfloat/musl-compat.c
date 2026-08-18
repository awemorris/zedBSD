/*
 * zedBSD adapters for the selected musl string scanner.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "src/softfloat/musl-floatscan.h"

#include <stdint.h>

double fmod(double value, double divisor);
double scalbn(double value, int exponent);

int
__uflow(FILE *stream)
{
	(void)stream;
	return EOF;
}

long double
fabsl(long double value)
{
	union {
		long double value;
		unsigned char bytes[sizeof(long double)];
	} shape = { value };
	size_t sign =
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	    0;
#else
	    sizeof(shape.bytes) - 1U;
#endif

	shape.bytes[sign] &= 0x7fU;
	return shape.value;
}

long double
copysignl(long double magnitude, long double sign)
{
	union {
		long double value;
		unsigned char bytes[sizeof(long double)];
	} left = { magnitude }, right = { sign };
	size_t sign_byte =
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	    0;
#else
	    sizeof(left.bytes) - 1U;
#endif

	left.bytes[sign_byte] &= 0x7fU;
	left.bytes[sign_byte] |= right.bytes[sign_byte] & 0x80U;
	return left.value;
}

long double
scalbnl(long double value, int exponent)
{
	return (long double)scalbn((double)value, exponent);
}

long double
fmodl(long double value, long double divisor)
{
	return (long double)fmod((double)value, (double)divisor);
}
