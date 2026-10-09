/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Renderer-owned virtual ranges, serialized with native jobs by the caller. */
#ifndef KERN_DRIVERS_GPU_BCM2711_V3D_MEMORY_H
#define KERN_DRIVERS_GPU_BCM2711_V3D_MEMORY_H

#include <stdbool.h>
#include "drivers/gpu/bcm2711/buffer.h"

struct bcm2711_v3d;

/* Each mapping owns its allocation independently of any resource descriptor. */
struct bcm2711_v3d_view {
	struct bcm2711_v3d_view *next;
	struct bcm2711_buffer *buffer;
	uint32_t address;
	uint32_t references;
	uint64_t bytes;
	bool quarantined;
};

/* The controller mutex serializes every edit with all submitted native jobs. */
struct bcm2711_v3d_space {
	struct bcm2711_v3d *native;
	struct bcm2711_v3d_view *views;
};

int bcm2711_v3d_memory_map(struct bcm2711_v3d_space *space, struct bcm2711_buffer *buffer, struct bcm2711_v3d_view **result);
void bcm2711_v3d_memory_retain(struct bcm2711_v3d_view *view);
int bcm2711_v3d_memory_release(struct bcm2711_v3d_space *space, struct bcm2711_v3d_view *view);
int bcm2711_v3d_memory_recover(struct bcm2711_v3d_space *space);

#endif
