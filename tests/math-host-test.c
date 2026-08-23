/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <math.h>
#include <stdint.h>

#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif
#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

static int test_errno;
int *__libc_errno_location(void) { return &test_errno; }
static uint32_t float_bits(float x)
{ union { float f; uint32_t u; } v = { x }; return v.u; }
static uint64_t double_bits(double x)
{ union { double f; uint64_t u; } v = { x }; return v.u; }
static double bits_double(uint64_t x)
{ union { uint64_t u; double f; } v = { x }; return v.f; }

int
main(void)
{
	CHECK(float_bits(sqrtf(9.0f)) == UINT32_C(0x40400000));
	CHECK(float_bits(sinf(0.5f)) == UINT32_C(0x3ef57744));
	CHECK(float_bits(cosf(0.5f)) == UINT32_C(0x3f60a940));
	CHECK(float_bits(tanf(0.5f)) == UINT32_C(0x3f0bda7b));
	CHECK(isnan(sqrt(-1.0)));
	CHECK(floor(-1.25) == -2.0 && ceil(-1.25) == -1.0);
	CHECK(trunc(-1.75) == -1.0 && round(-1.5) == -2.0);
	CHECK(double_bits(scalbn(1.0, -1074)) == UINT64_C(1));
	CHECK(nextafter(0.0, 1.0) == bits_double(UINT64_C(1)));
	CHECK(fabs(exp(log(2.0)) - 2.0) < 2e-14);
	CHECK(fabs(sin(M_PI / 6.0) - 0.5) < 2e-14);
	return 0;
}
