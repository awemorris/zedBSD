/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the armv7 HAL's 32-bit division helpers
 * (src/hal/armv7/aeabi-divide.c): the quotient and the remainder of each
 * form against the host's own division, at the boundaries (the most
 * negative word by minus one, all ones by one, a remainder in the high
 * half) and over a run of pseudo-random operands, and the report of a
 * zero divisor.  The register use (quotient in r0, remainder in r1) is
 * the target's self-test's.
 *
 * cc -m32 -std=c11 -Wall -Wextra -Werror \
 *    tests/aeabi-host-test.c src/hal/armv7/aeabi-divide.c -o <out> && <out>
 */

#include <stdint.h>
#include <stdio.h>

/* The pseudo-random operand pairs each form is checked on. */
#define RANDOM_ROUNDS	200000U

uint32_t __aeabi_uidiv(uint32_t numerator, uint32_t denominator);
uint64_t __aeabi_uidivmod(uint32_t numerator, uint32_t denominator);
int32_t __aeabi_idiv(int32_t numerator, int32_t denominator);
uint64_t __aeabi_idivmod(int32_t numerator, int32_t denominator);
void __aeabi_idiv0(void);

/* How many times the helpers reported a zero divisor. */
static unsigned zero_reports;

static int failures;

static void check(int condition, const char *what);
static void check_unsigned(uint32_t numerator, uint32_t denominator);
static void check_signed(int32_t numerator, int32_t denominator);
static uint32_t next_random(uint32_t *state);

/*
 * Runs the checks and reports PASS or FAIL.
 */
int
main(
	void)
{
	uint32_t state;
	uint32_t numerator;
	uint32_t denominator;
	uint64_t both;
	unsigned round;

	/* The boundaries. */
	check_unsigned(0xffffffffU, 1U);
	check_unsigned(0xffffffffU, 0x10000U);
	check_unsigned(0U, 7U);
	check_unsigned(7U, 0xffffffffU);
	check_signed(-7, 2);
	check_signed(7, -2);
	check_signed(-7, -2);
	check_signed(INT32_MAX, -1);
	both = __aeabi_idivmod(INT32_MIN, -1);
	check((uint32_t)both == 0x80000000U && (uint32_t)(both >> 32) == 0U, "most negative by minus one");

	/* Pseudo-random operands, small and large divisors alike. */
	state = 0x2468aceU;
	for (round = 0; round < RANDOM_ROUNDS; round++) {
		numerator = next_random(&state);
		denominator = next_random(&state) >> (round % 32U);
		if (denominator == 0)
			denominator = 1;
		check_unsigned(numerator, denominator);
		if ((int32_t)numerator == INT32_MIN && (int32_t)denominator == -1)
			continue;
		check_signed((int32_t)numerator, (int32_t)denominator);
	}

	/* A zero divisor is reported, with all ones and the dividend as the results. */
	both = __aeabi_uidivmod(5U, 0U);
	check(zero_reports == 1U, "zero divisor reported");
	check((uint32_t)both == 0xffffffffU && (uint32_t)(both >> 32) == 5U, "zero divisor results");

	/* The verdict. */
	if (failures != 0) {
		printf("armv7 aeabi host test FAIL (%d)\n", failures);
		return 1;
	}

	/* Succeeded: every form agreed with the host. */
	printf("armv7 aeabi host test PASS\n");
	return 0;
}

/*
 * Counts a zero divisor, as the kernel's handler would stop the kernel.
 */
void
__aeabi_idiv0(
	void)
{
	/* The test only counts it. */
	zero_reports++;
}

/* Counts and reports one failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A passed check says nothing. */
	if (condition)
		return;
	printf("FAIL: %s\n", what);
	failures++;
}

/* Checks both unsigned forms against the host's division. */
static void
check_unsigned(
	uint32_t numerator,
	uint32_t denominator)
{
	uint64_t both;

	/* The quotient alone, then the quotient and the remainder. */
	check(__aeabi_uidiv(numerator, denominator) == numerator / denominator, "unsigned quotient");
	both = __aeabi_uidivmod(numerator, denominator);
	check((uint32_t)both == numerator / denominator, "unsigned divmod quotient");
	check((uint32_t)(both >> 32) == numerator % denominator, "unsigned divmod remainder");
}

/* Checks both signed forms against the host's division. */
static void
check_signed(
	int32_t numerator,
	int32_t denominator)
{
	uint64_t both;

	/* The quotient alone, then the quotient and the remainder. */
	check(__aeabi_idiv(numerator, denominator) == numerator / denominator, "signed quotient");
	both = __aeabi_idivmod(numerator, denominator);
	check((int32_t)(uint32_t)both == numerator / denominator, "signed divmod quotient");
	check((int32_t)(uint32_t)(both >> 32) == numerator % denominator, "signed divmod remainder");
}

/* Steps a 32-bit xorshift generator. */
static uint32_t
next_random(
	uint32_t *state)
{
	uint32_t value;

	/* Three shifts and exclusive ors. */
	value = *state;
	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}
