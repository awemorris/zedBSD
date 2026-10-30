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
 * Decodes MSR_PLATFORM_INFO.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_platform_info_decode(
	uint64_t raw,
	struct cpux86_platform_info *info)
{
	/* The skeleton reads none of its arguments. */
	(void)raw;
	(void)info;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Converts a ratio of the platform information register to MHz.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_ratio_mhz(
	uint32_t ratio,
	uint32_t *mhz)
{
	/* The skeleton reads none of its arguments. */
	(void)ratio;
	(void)mhz;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}
