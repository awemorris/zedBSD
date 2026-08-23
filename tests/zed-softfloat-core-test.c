/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdint.h>

#include "src/softfloat/zed-softfloat.h"

#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

static uint64_t random_state = UINT64_C(0x6a09e667f3bcc909);

static uint64_t
random64(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return random_state;
}

static uint32_t
native32(uint32_t left, uint32_t right, char operation)
{
	union { uint32_t bits; float value; } a = { left }, b = { right }, result;
	volatile float av = a.value;
	volatile float bv = b.value;

	switch (operation) {
	case '+': result.value = av + bv; break;
	case '-': result.value = av - bv; break;
	case '*': result.value = av * bv; break;
	default: result.value = av / bv; break;
	}
	return result.bits;
}

static uint64_t
native64(uint64_t left, uint64_t right, char operation)
{
	union { uint64_t bits; double value; } a = { left }, b = { right }, result;
	volatile double av = a.value;
	volatile double bv = b.value;

	switch (operation) {
	case '+': result.value = av + bv; break;
	case '-': result.value = av - bv; break;
	case '*': result.value = av * bv; break;
	default: result.value = av / bv; break;
	}
	return result.bits;
}

static int
ordinary32(uint32_t bits)
{
	return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}

static int
ordinary64(uint64_t bits)
{
	return (bits & UINT64_C(0x7ff0000000000000)) !=
	    UINT64_C(0x7ff0000000000000);
}

static int
test_fixed(void)
{
	int unordered;

	CHECK(zsf32_add(UINT32_C(0x3fa00000), UINT32_C(0x40200000)) ==
	    UINT32_C(0x40700000));
	CHECK(zsf32_sub(UINT32_C(0x40200000), UINT32_C(0x3fa00000)) ==
	    UINT32_C(0x3fa00000));
	CHECK(zsf32_mul(UINT32_C(0x3fa00000), UINT32_C(0x40200000)) ==
	    UINT32_C(0x40480000));
	CHECK(zsf32_div(UINT32_C(0x40200000), UINT32_C(0x3fa00000)) ==
	    UINT32_C(0x40000000));
	CHECK(zsf64_add(UINT64_C(0x3ff4000000000000),
	    UINT64_C(0x4004000000000000)) == UINT64_C(0x400e000000000000));
	CHECK(zsf64_mul(UINT64_C(0x3ff4000000000000),
	    UINT64_C(0x4004000000000000)) == UINT64_C(0x4009000000000000));
	CHECK(zsf32_to_64(UINT32_C(0x3fc00000)) ==
	    UINT64_C(0x3ff8000000000000));
	CHECK(zsf64_to_32(UINT64_C(0x3ff8000000000000)) ==
	    UINT32_C(0x3fc00000));
	CHECK(zsf_i64_to_64(-123456789) == UINT64_C(0xc19d6f3454000000));
	CHECK(zsf_u64_to_32(UINT64_C(4000000000)) == UINT32_C(0x4f6e6b28));
	CHECK(zsf64_to_i64(UINT64_C(0xc031000000000000), 32U, 0) == -17);
	CHECK(zsf32_compare(UINT32_C(0x80000000), 0U, &unordered) == 0 &&
	    !unordered);
	CHECK(zsf64_compare(UINT64_C(0x7ff8000000000000), 0U, &unordered) == 0 &&
	    unordered);
	return 0;
}

static int
test_random(void)
{
	unsigned int index;

	for (index = 0; index < 100000U; index++) {
		uint32_t a32 = (uint32_t)random64();
		uint32_t b32 = (uint32_t)random64();
		uint64_t a64 = random64();
		uint64_t b64 = random64();

		if (ordinary32(a32) && ordinary32(b32)) {
			CHECK(zsf32_add(a32, b32) == native32(a32, b32, '+'));
			CHECK(zsf32_sub(a32, b32) == native32(a32, b32, '-'));
			CHECK(zsf32_mul(a32, b32) == native32(a32, b32, '*'));
			if ((b32 & UINT32_C(0x7fffffff)) != 0U)
				CHECK(zsf32_div(a32, b32) == native32(a32, b32, '/'));
		}
		if (ordinary64(a64) && ordinary64(b64)) {
			CHECK(zsf64_add(a64, b64) == native64(a64, b64, '+'));
			CHECK(zsf64_sub(a64, b64) == native64(a64, b64, '-'));
			CHECK(zsf64_mul(a64, b64) == native64(a64, b64, '*'));
			if ((b64 & UINT64_C(0x7fffffffffffffff)) != 0U)
				CHECK(zsf64_div(a64, b64) == native64(a64, b64, '/'));
		}
	}
	return 0;
}

int
main(void)
{
	int result = test_fixed();
	return result != 0 ? result : test_random();
}
