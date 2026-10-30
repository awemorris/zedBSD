/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the platform information decoder: a hand-worked value, each
 * field alone, the reserved code and bits, the ratio conversion, and random
 * inputs against shifts written out here.  The expected values come from
 * Intel SDM Vol. 4 rev. 093, Table 2-29, by hand.
 */

#include "check.h"
#include "regbuild.h"
#include "cpux86-pstate.h"

/* The state of the random input generator; the seed is fixed so a failure repeats. */
static uint64_t random_state = ((uint64_t)0x12345678U << 32) | 0x9ABCDEF1U;

/*
 * Gives the next 64-bit pseudo-random value (xorshift).
 */
static uint64_t
next_random(void)
{
	/* Mixes the state three times and keeps it. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;

	/* Reports the new state. */
	return random_state;
}

/*
 * Checks the platform information of one 5330 value and the fields alone.
 */
static void
check_known_value(void)
{
	struct cpux86_platform_info info;
	uint64_t raw;
	uint32_t ratio;
	int error;

	/*
	 * 0x0804043DF0811900 worked by hand: bits 15:8 = 0x19 (25), bits 47:40
	 * = 0x04, bits 55:48 = 0x04, bits 34:33 = 10b (2 levels), bits 28, 29
	 * and 32 set; the reserved bits 30 and 31 are set too and do not matter.
	 */
	raw = ((uint64_t)0x0804043DU << 32) | 0xF0811900U;
	error = cpux86_platform_info_decode(raw, &info);
	CHECK(error == 0);
	CHECK(info.max_non_turbo_ratio == 0x19U);
	CHECK(info.max_efficiency_ratio == 4U);
	CHECK(info.min_operating_ratio == 4U);
	CHECK(info.config_tdp_levels == 2U);
	CHECK(info.flags == (CPUX86_PLATFORM_RATIO_PROGRAMMABLE | CPUX86_PLATFORM_TDP_PROGRAMMABLE | CPUX86_PLATFORM_LOW_POWER_MODE));

	/* An all-zero register decodes to all zero. */
	error = cpux86_platform_info_decode(0U, &info);
	CHECK(error == 0);
	CHECK(info.max_non_turbo_ratio == 0U);
	CHECK(info.max_efficiency_ratio == 0U);
	CHECK(info.min_operating_ratio == 0U);
	CHECK(info.config_tdp_levels == 0U);
	CHECK(info.flags == 0U);

	/* Each ratio field alone, for every value of its 8 bits. */
	for (ratio = 0U; ratio < 256U; ratio++) {
		raw = 0U;
		regbuild_put64(&raw, 8U, 8U, ratio);
		error = cpux86_platform_info_decode(raw, &info);
		CHECK(error == 0);
		CHECK(info.max_non_turbo_ratio == ratio);
		CHECK(info.max_efficiency_ratio == 0U);
		CHECK(info.min_operating_ratio == 0U);
		CHECK(info.flags == 0U);

		raw = 0U;
		regbuild_put64(&raw, 40U, 8U, ratio);
		error = cpux86_platform_info_decode(raw, &info);
		CHECK(error == 0);
		CHECK(info.max_non_turbo_ratio == 0U);
		CHECK(info.max_efficiency_ratio == ratio);
		CHECK(info.min_operating_ratio == 0U);
		CHECK(info.flags == 0U);

		raw = 0U;
		regbuild_put64(&raw, 48U, 8U, ratio);
		error = cpux86_platform_info_decode(raw, &info);
		CHECK(error == 0);
		CHECK(info.max_non_turbo_ratio == 0U);
		CHECK(info.max_efficiency_ratio == 0U);
		CHECK(info.min_operating_ratio == ratio);
		CHECK(info.flags == 0U);
	}
}

/*
 * Checks each flag bit alone and the ConfigTDP level codes.
 */
static void
check_flags_and_levels(void)
{
	struct cpux86_platform_info info;
	struct cpux86_platform_info before;
	uint64_t raw;
	uint32_t level;
	uint32_t bit;
	int error;

	/* Bit 28 alone is the programmable ratio limit. */
	error = cpux86_platform_info_decode((uint64_t)1U << 28, &info);
	CHECK(error == 0);
	CHECK(info.flags == CPUX86_PLATFORM_RATIO_PROGRAMMABLE);

	/* Bit 29 alone is the programmable TDP limit. */
	error = cpux86_platform_info_decode((uint64_t)1U << 29, &info);
	CHECK(error == 0);
	CHECK(info.flags == CPUX86_PLATFORM_TDP_PROGRAMMABLE);

	/* Bit 32 alone is the low power mode. */
	error = cpux86_platform_info_decode((uint64_t)1U << 32, &info);
	CHECK(error == 0);
	CHECK(info.flags == CPUX86_PLATFORM_LOW_POWER_MODE);

	/* The three flags are distinct single bits. */
	CHECK(CPUX86_PLATFORM_RATIO_PROGRAMMABLE == 1U);
	CHECK(CPUX86_PLATFORM_TDP_PROGRAMMABLE == 2U);
	CHECK(CPUX86_PLATFORM_LOW_POWER_MODE == 4U);

	/* The level codes 0, 1 and 2 decode to themselves. */
	for (level = 0U; level < 3U; level++) {
		raw = 0U;
		regbuild_put64(&raw, 33U, 2U, level);
		error = cpux86_platform_info_decode(raw, &info);
		CHECK(error == 0);
		CHECK(info.config_tdp_levels == level);
		CHECK(info.flags == 0U);
	}

	/* The reserved level code 3 is refused and leaves the output alone. */
	before.max_non_turbo_ratio = 11U;
	before.max_efficiency_ratio = 12U;
	before.min_operating_ratio = 13U;
	before.config_tdp_levels = 1U;
	before.flags = 5U;
	info = before;
	raw = 0U;
	regbuild_put64(&raw, 33U, 2U, 3U);
	regbuild_put64(&raw, 8U, 8U, 0x19U);
	error = cpux86_platform_info_decode(raw, &info);
	CHECK(error == EINVAL);
	CHECK(info.max_non_turbo_ratio == 11U);
	CHECK(info.max_efficiency_ratio == 12U);
	CHECK(info.min_operating_ratio == 13U);
	CHECK(info.config_tdp_levels == 1U);
	CHECK(info.flags == 5U);

	/* A NULL output is refused. */
	error = cpux86_platform_info_decode(0U, NULL);
	CHECK(error == EINVAL);

	/*
	 * Every reserved bit set (7:0, 27:16, 31:30, 39:35, 63:56) with the
	 * level code 0 decodes without error and reads nothing from them.
	 */
	raw = 0U;
	regbuild_put64(&raw, 0U, 8U, 0xFFU);
	regbuild_put64(&raw, 16U, 12U, 0xFFFU);
	regbuild_put64(&raw, 30U, 2U, 3U);
	regbuild_put64(&raw, 35U, 5U, 0x1FU);
	regbuild_put64(&raw, 56U, 8U, 0xFFU);
	error = cpux86_platform_info_decode(raw, &info);
	CHECK(error == 0);
	CHECK(info.max_non_turbo_ratio == 0U);
	CHECK(info.max_efficiency_ratio == 0U);
	CHECK(info.min_operating_ratio == 0U);
	CHECK(info.config_tdp_levels == 0U);
	CHECK(info.flags == 0U);

	/* Setting any one reserved bit alone never changes the result or fails. */
	for (bit = 0U; bit < 64U; bit++) {
		if ((bit >= 8U && bit < 16U) || bit == 28U || bit == 29U || bit == 32U ||
		    bit == 33U || bit == 34U || (bit >= 40U && bit < 56U))
			continue;
		error = cpux86_platform_info_decode((uint64_t)1U << bit, &info);
		CHECK(error == 0);
		CHECK(info.max_non_turbo_ratio == 0U);
		CHECK(info.max_efficiency_ratio == 0U);
		CHECK(info.min_operating_ratio == 0U);
		CHECK(info.config_tdp_levels == 0U);
		CHECK(info.flags == 0U);
	}
}

/*
 * Checks the ratio-to-MHz conversion.
 */
static void
check_ratio_mhz(void)
{
	uint32_t mhz;
	uint32_t ratio;
	int error;

	/* The bounds: 0 gives 0, 25 gives 2500 MHz, 255 gives 25500 MHz. */
	mhz = 7U;
	error = cpux86_ratio_mhz(0U, &mhz);
	CHECK(error == 0);
	CHECK(mhz == 0U);
	error = cpux86_ratio_mhz(25U, &mhz);
	CHECK(error == 0);
	CHECK(mhz == 2500U);
	error = cpux86_ratio_mhz(255U, &mhz);
	CHECK(error == 0);
	CHECK(mhz == 25500U);

	/* Every ratio in range is 100 times itself. */
	for (ratio = 0U; ratio < 256U; ratio++) {
		error = cpux86_ratio_mhz(ratio, &mhz);
		CHECK(error == 0);
		CHECK(mhz == ratio * 100U);
	}

	/* 256 and larger are refused and leave the output alone. */
	mhz = 7U;
	error = cpux86_ratio_mhz(256U, &mhz);
	CHECK(error == EINVAL);
	CHECK(mhz == 7U);
	error = cpux86_ratio_mhz(0xFFFFFFFFU, &mhz);
	CHECK(error == EINVAL);
	CHECK(mhz == 7U);

	/* A NULL output is refused. */
	error = cpux86_ratio_mhz(25U, NULL);
	CHECK(error == EINVAL);
}

/*
 * Checks random registers against shifts written out by hand.
 */
static void
check_random(void)
{
	struct cpux86_platform_info info;
	uint64_t raw;
	uint32_t levels;
	uint32_t expected_flags;
	uint32_t mhz;
	uint32_t round;
	int error;

	/* Runs many fixed-seed values through both functions. */
	for (round = 0U; round < 20000U; round++) {
		raw = next_random();
		levels = (uint32_t)((raw >> 33) & 3U);
		expected_flags = 0U;
		if (((raw >> 28) & 1U) != 0U)
			expected_flags |= 1U;
		if (((raw >> 29) & 1U) != 0U)
			expected_flags |= 2U;
		if (((raw >> 32) & 1U) != 0U)
			expected_flags |= 4U;

		error = cpux86_platform_info_decode(raw, &info);
		if (levels == 3U) {
			CHECK(error == EINVAL);
		} else {
			CHECK(error == 0);
			CHECK(info.max_non_turbo_ratio == (uint32_t)((raw >> 8) & 0xFFU));
			CHECK(info.max_efficiency_ratio == (uint32_t)((raw >> 40) & 0xFFU));
			CHECK(info.min_operating_ratio == (uint32_t)((raw >> 48) & 0xFFU));
			CHECK(info.config_tdp_levels == levels);
			CHECK(info.flags == expected_flags);
		}

		error = cpux86_ratio_mhz((uint32_t)raw, &mhz);
		if ((uint32_t)raw > 255U) {
			CHECK(error == EINVAL);
		} else {
			CHECK(error == 0);
			CHECK(mhz == (uint32_t)raw * 100U);
		}
	}
}

/*
 * Runs the platform information checks.
 */
int
main(void)
{
	/* The value of a real machine, worked by hand, and every field alone. */
	check_known_value();

	/* The flag bits, the level codes and the reserved bits. */
	check_flags_and_levels();

	/* The ratio conversion. */
	check_ratio_mhz();

	/* Random registers against hand-written shifts. */
	check_random();

	/* Reports the result. */
	return check_finish("pstate");
}
