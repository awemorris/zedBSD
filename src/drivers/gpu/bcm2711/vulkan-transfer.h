/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Ordered image transfers own copied bounded rectangles and exact typed images through native retirement. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_TRANSFER_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_TRANSFER_H

#include "drivers/gpu/bcm2711/vulkan-record.h"

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
	VkFilter filter;
	bool retained[2];
};

int bcm2711_vulkan_transfer_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_transfer_validate(const struct bcm2711_vulkan_transfer *transfer, struct bcm2711_vulkan_object *device);

#endif
