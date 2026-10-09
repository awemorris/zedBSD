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

/* Native clipper state owns unsigned fine/signed coarse offsets and a hardware guardband depth transform distinct from exact shader depth. */
struct bcm2711_native_viewport_clip {
	uint32_t x_offset;
	uint32_t y_offset;
	uint32_t depth_scale;
	uint32_t depth_offset;
	uint32_t minimum;
	uint32_t maximum;
	/* Integer viewport edges match native guardband scissor restriction after IEEE centre/half arithmetic. */
	int32_t bounds[4];
};

int bcm2711_native_viewport_prepare(const uint32_t words[6], struct bcm2711_native_viewport *viewport);
int bcm2711_native_viewport_clip_prepare(const uint32_t words[6], struct bcm2711_native_viewport_clip *clip);

#endif
