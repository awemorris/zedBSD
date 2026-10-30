/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoder of the platform information register of the Intel models
 * that have it: the ratio the time-stamp counter runs at and the lowest
 * ratios the processor operates at.
 */

#ifndef DRIVERS_PLATFORM_PCAT_CPU_X86_CPUX86_PSTATE_H
#define DRIVERS_PLATFORM_PCAT_CPU_X86_CPUX86_PSTATE_H

#include "cpux86.h"

/*
 * The platform information register.
 *
 * Value referenced: Intel SDM Vol. 4 (335592-093), Table 2-29
 * (MSR_PLATFORM_INFO CEH).
 */
#define CPUX86_MSR_PLATFORM_INFO	0x0CEU

/*
 * The frequency of one ratio step of the platform information register.
 *
 * Value referenced: Intel SDM Vol. 4 rev. 093, Table 2-29 ("Frequency =
 * ratio * 100 MHz").
 */
#define CPUX86_RATIO_STEP_MHZ		100U

/*
 * The flags of MSR_PLATFORM_INFO, as bits of struct cpux86_platform_info.flags.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, Table 2-29 (bits 28, 29
 * and 32).
 */
#define CPUX86_PLATFORM_RATIO_PROGRAMMABLE	0x1U	/* bit 28, turbo ratio limits writable */
#define CPUX86_PLATFORM_TDP_PROGRAMMABLE	0x2U	/* bit 29, turbo TDP limit writable */
#define CPUX86_PLATFORM_LOW_POWER_MODE		0x4U	/* bit 32 */

/*
 * What MSR_PLATFORM_INFO reports, in the layout of the models whose
 * MSR tables give that register as Vol. 4 Table 2-29 does.
 *
 * Ratios are multiples of CPUX86_RATIO_STEP_MHZ.  The maximum non-turbo
 * ratio is also the ratio the invariant time-stamp counter runs at.
 */
struct cpux86_platform_info {
	uint32_t max_non_turbo_ratio;	/* bits 15:8 */
	uint32_t max_efficiency_ratio;	/* bits 47:40, the lowest ratio for efficiency */
	uint32_t min_operating_ratio;	/* bits 55:48 */
	uint32_t config_tdp_levels;	/* bits 34:33, 0..2 additional levels */
	uint32_t flags;			/* CPUX86_PLATFORM_* */
};

/*
 * Decodes MSR_PLATFORM_INFO.
 *
 * Input: raw, the register as read on a model whose allow list
 * (cpux86_model_registers()) names it.
 * Output: *info.  Reserved bits (7:0, 27:16, 31:30, 39:35, 63:56) are not
 * checked: the register is model-specific and other models put fields
 * there.
 * Errors: EINVAL for a NULL pointer, or config_tdp_levels 3 (reserved).
 * Specification: Intel SDM Vol. 4 rev. 093, Table 2-29 (MSR_PLATFORM_INFO).
 */
int
cpux86_platform_info_decode(
	uint64_t raw,
	struct cpux86_platform_info *info);

/*
 * Converts a ratio of the platform information register to MHz.
 *
 * Input: ratio, 0..255.
 * Output: *mhz, ratio * 100.
 * Errors: EINVAL for a NULL pointer or a ratio above 255.
 * Specification: Intel SDM Vol. 4 rev. 093, Table 2-29 ("Frequency =
 * ratio * 100 MHz").
 */
int
cpux86_ratio_mhz(
	uint32_t ratio,
	uint32_t *mhz);

#endif
