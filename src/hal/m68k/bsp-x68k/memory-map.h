/* X68000 physical device apertures exposed in the kernel direct map. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_M68K_X68K_MEMORY_MAP_H
#define ZEDBSD_HAL_M68K_X68K_MEMORY_MAP_H

#include <hal/hal.h>

struct x68k_physical_mapping {
	uintptr_t physical;
	size_t size;
	uint32_t attributes;
	const char *name;
};

const struct x68k_physical_mapping *x68k_physical_mappings(size_t *count);

#endif
