/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoder of the platform information register.
 */

#include "cpux86-pstate.h"

/*
 * The largest ratio the platform information register can hold: its
 * ratio fields are 8 bits wide.
 */
#define PLATFORM_RATIO_MAX		255U

/*
 * The reserved code of the ConfigTDP level count; the codes 0 to 2 are
 * the levels.
 *
 * Value referenced: Intel SDM Vol. 4 rev. 093, Table 2-29 (bits 34:33,
 * "03: Reserved").
 */
#define PLATFORM_TDP_LEVELS_RESERVED	3U

/*
 * Decodes MSR_PLATFORM_INFO.
 *
 * Each field is read at the bits Table 2-29 gives; the reserved bits are
 * not looked at because other models use them.
 */
int
cpux86_platform_info_decode(
	uint64_t raw,
	struct cpux86_platform_info *info)
{
	struct cpux86_platform_info decoded;
	uint64_t field;

	/* Refuses a missing output. */
	if (info == NULL)
		return EINVAL;

	/* Reads the number of additional ConfigTDP levels and refuses the reserved code. */
	field = cpux86_field64(raw, 33U, 2U);
	if (field == PLATFORM_TDP_LEVELS_RESERVED)
		return EINVAL;

	/* Keeps the level count now that it is known to be valid. */
	decoded.config_tdp_levels = (uint32_t)field;

	/* Reads the ratio the invariant time-stamp counter runs at. */
	field = cpux86_field64(raw, 8U, 8U);
	decoded.max_non_turbo_ratio = (uint32_t)field;

	/* Reads the lowest ratio the processor runs at for efficiency. */
	field = cpux86_field64(raw, 40U, 8U);
	decoded.max_efficiency_ratio = (uint32_t)field;

	/* Reads the lowest ratio the processor operates at. */
	field = cpux86_field64(raw, 48U, 8U);
	decoded.min_operating_ratio = (uint32_t)field;

	/* Starts the flags empty so each capability bit adds its own flag. */
	decoded.flags = 0U;

	/* Records that the turbo ratio limits can be written. */
	field = cpux86_field64(raw, 28U, 1U);
	if (field != 0U)
		decoded.flags |= CPUX86_PLATFORM_RATIO_PROGRAMMABLE;

	/* Records that the turbo TDP limit can be written. */
	field = cpux86_field64(raw, 29U, 1U);
	if (field != 0U)
		decoded.flags |= CPUX86_PLATFORM_TDP_PROGRAMMABLE;

	/* Records that the low power mode is supported. */
	field = cpux86_field64(raw, 32U, 1U);
	if (field != 0U)
		decoded.flags |= CPUX86_PLATFORM_LOW_POWER_MODE;

	/* Publishes the decoded register now that every field was accepted. */
	*info = decoded;

	/* Succeeded: *info holds the platform information. */
	return 0;
}

/*
 * Converts a ratio of the platform information register to MHz.
 *
 * The product is at most 255 * 100, which fits the output without a
 * range check.
 */
int
cpux86_ratio_mhz(
	uint32_t ratio,
	uint32_t *mhz)
{
	/* Refuses a missing output or a ratio the 8-bit fields cannot hold. */
	if (mhz == NULL || ratio > PLATFORM_RATIO_MAX)
		return EINVAL;

	/* Converts the ratio steps to MHz. */
	*mhz = ratio * CPUX86_RATIO_STEP_MHZ;

	/* Succeeded: *mhz holds the frequency. */
	return 0;
}
