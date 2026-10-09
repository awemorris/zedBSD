/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Typed resources keep immutable storage requirements separate from their independently retained memory binding. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_RESOURCE_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_RESOURCE_H

#include "drivers/gpu/bcm2711/vulkan-private.h"

struct bcm2711_v3d_view;

/* One buffer or single-subresource colour image retains its logical device and exact bound allocation. */
struct bcm2711_vulkan_resource {
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *memory;
	uint64_t bytes;
	uint64_t required_bytes;
	uint64_t alignment;
	uint64_t offset;
	uint32_t usage;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	VkFormat format;
	VkImageTiling tiling;
	VkImageLayout layout;
};

int bcm2711_vulkan_resource_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_resource_backing(struct bcm2711_vulkan_resource *resource, uint64_t offset, uint64_t bytes, struct bcm2711_v3d_view **view, uint32_t *address, void **cpu);

#endif
