/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native graphics uniforms use copied IEEE bits without borrowing kernel floating-point registers. */
#ifndef KERN_DRIVERS_GPU_BCM2711_NATIVE_VIEWPORT_H
#define KERN_DRIVERS_GPU_BCM2711_NATIVE_VIEWPORT_H

#include <stdint.h>

/* One draw's IEEE scale words feed fixed-point XY export and floating-point depth scale/offset without kernel FP arithmetic. */
struct bcm2711_native_viewport {
	uint32_t x_scale;
	uint32_t y_scale;
	uint32_t depth_scale;
	uint32_t depth_offset;
};

int bcm2711_native_viewport_prepare(const uint32_t words[6], struct bcm2711_native_viewport *viewport);

#endif
