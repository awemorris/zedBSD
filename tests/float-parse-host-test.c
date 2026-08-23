/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

static int test_errno;

int *
__libc_errno_location(void)
{
	return &test_errno;
}

static uint64_t
double_bits(double value)
{
	union { double value; uint64_t bits; } shape = { value };
	return shape.bits;
}

static uint32_t
float_bits(float value)
{
	union { float value; uint32_t bits; } shape = { value };
	return shape.bits;
}

int
main(void)
{
	char *end;
	double value;

	errno = 0;
	value = strtod("  -2.25tail", &end);
	CHECK(double_bits(value) == UINT64_C(0xc002000000000000));
	CHECK(strcmp(end, "tail") == 0 && errno == 0);
	CHECK(double_bits(strtod("0.383177570093458", &end)) ==
	    UINT64_C(0x3fd885fb37072d76) && *end == '\0');
	CHECK(double_bits(strtod("1e20", &end)) ==
	    UINT64_C(0x4415af1d78b58c40));
	CHECK(double_bits(strtod("1e-20", &end)) ==
	    UINT64_C(0x3bc79ca10c924223));
	CHECK(double_bits(strtod("0x1.8p+1", &end)) ==
	    UINT64_C(0x4008000000000000) && *end == '\0');
	CHECK(float_bits(strtof("0.5", &end)) == UINT32_C(0x3f000000));
	errno = 0;
	value = strtod("1e9999", &end);
	CHECK(double_bits(value) == UINT64_C(0x7ff0000000000000));
	CHECK(errno == ERANGE);
	errno = 0;
	value = strtod("1e-9999", &end);
	CHECK(double_bits(value) == 0U && errno == ERANGE);
	CHECK(strtod("nothing", &end) == 0.0 && end[0] == 'n');
	return 0;
}
