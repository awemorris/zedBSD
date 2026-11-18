/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Ordered image copies, blits and resolves own copied bounded rectangles and exact typed images through native retirement. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_TRANSFER_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_TRANSFER_H

#include "drivers/gpu/videocore/vk/vulkan-record.h"

#define BCM2711_VULKAN_TRANSFER_REGIONS 64U

/* One pair of directed endpoint rectangles preserves axis reversal without a caller-owned Vulkan array. */
struct bcm2711_vulkan_transfer_region {
	int32_t source[4];
	int32_t destination[4];
};

/* The common immutable node prefix retains both actual resources; only its acquired edges belong to this destructor. */
struct bcm2711_vulkan_transfer {
	struct bcm2711_vulkan_record record;
	struct bcm2711_vulkan_transfer_region regions[BCM2711_VULKAN_TRANSFER_REGIONS];
	VkImageLayout source_layout;
	VkImageLayout destination_layout;
	/*
	 * The source's and the destination's mip level and array layer (a 3D
	 * image's slice, its region's z), the same for every region.
	 */
	uint32_t levels[2];
	uint32_t layers[2];
	/* The slice (z) each side's regions start at, as decoded: a 3D image's becomes its layer, any other's is zero. */
	int32_t slices[2];
	/* The aspects every region names on both sides: colour, or a depth/stencil image's depth, stencil or both. */
	uint32_t aspects;
	VkFilter filter;
	bool retained[2];
};

int bcm2711_vulkan_transfer_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_transfer_validate(const struct bcm2711_vulkan_transfer *transfer, struct bcm2711_vulkan_object *device);

#endif
