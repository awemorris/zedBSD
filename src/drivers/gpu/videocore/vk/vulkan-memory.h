/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native device memory retains one coherent allocation/VA independently of its BLOB aliases and wire identity. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_MEMORY_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_MEMORY_H

#include "drivers/gpu/bcm2711/vk/vulkan-private.h"
#include <uapi/gpu-scanout.h>

/*
 * The one heap the physical query reports, and the budget every live
 * allocation declaration of all sessions shares.  Each physical run is
 * charged once, however many declarations (an export and its imports) hold
 * it.
 */
#define BCM2711_VULKAN_MEMORY_BUDGET (512ULL << 20)

struct bcm2711_buffer;
struct bcm2711_v3d_view;

/* Lazy physical backing lets the first BLOB's actual placement requirements precede allocation and GPU publication. */
struct bcm2711_vulkan_memory {
	struct bcm2711_vulkan_object *device;
	struct bcm2711_v3d_view *view;
	uint64_t bytes;
	/* The budget this declaration holds itself, while it has no run; a bound declaration's charge is its run's. */
	uint64_t charged;
	uint32_t external_type;
};

int bcm2711_vulkan_memory_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_memory_blob(struct bcm2711_vulkan_session *session, const struct gpu_blob_create *request, const struct gpu_placement *placement, struct bcm2711_buffer **buffer);

#endif
