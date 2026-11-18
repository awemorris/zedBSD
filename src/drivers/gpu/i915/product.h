/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the driver knows of a graphics device from its PCI device id.
 */

#ifndef DRIVERS_GPU_I915_PRODUCT_H
#define DRIVERS_GPU_I915_PRODUCT_H

#include <stdint.h>

/*
 * What a PCI device id says about the device: its graphics and display
 * versions, its platform (I915_PLATFORM_*), the engines the platform is
 * built with before the fuses take some away, the workaround rules of the
 * shared lists the platform takes (I915_PLATFORM_WA_*) and a name for the
 * log.  It is filled by drv_i915_product_kind() from a constant table.
 */
struct i915_product_kind {
	unsigned graphics_ver;
	unsigned display_ver;
	int platform;
	uint32_t engine_mask;
	uint32_t platform_wa;
	const char *name;
};

int drv_i915_product_kind(uint16_t product, struct i915_product_kind *kind);

#endif
