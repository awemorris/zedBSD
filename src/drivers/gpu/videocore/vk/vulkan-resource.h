/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Typed resources keep immutable storage requirements separate from their independently retained memory binding. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_RESOURCE_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_RESOURCE_H

#include "drivers/gpu/bcm2711/vk/vulkan-private.h"

struct bcm2711_v3d_view;

/* The most mip levels an image has: a 4096-pixel side halves twelve times. */
#define BCM2711_VULKAN_IMAGE_LEVELS 13U

/*
 * One mip level of a colour image: its raster rows, at offset from the
 * image's binding, of width texels at pitch bytes for height rows.
 */
struct bcm2711_vulkan_image_level {
	uint64_t offset;
	uint64_t bytes;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
};

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
	/* The layout of mip level 0 (all of a one-level image). */
	VkImageLayout layout;
	/*
	 * A mip-mapped colour image's levels follow level 0 (bytes, pitch,
	 * width and height) in its binding, smaller ones after larger, each
	 * its own raster; storage_bytes spans them all (zero: bytes does).
	 * level_layouts holds the layouts of levels 1 and up.
	 */
	uint32_t levels;
	uint64_t storage_bytes;
	VkImageLayout level_layouts[BCM2711_VULKAN_IMAGE_LEVELS];
};

int bcm2711_vulkan_resource_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_image_level(const struct bcm2711_vulkan_resource *image, uint32_t level, struct bcm2711_vulkan_image_level *result);
int bcm2711_vulkan_image_level_view(const struct bcm2711_vulkan_resource *image, uint32_t level, struct bcm2711_vulkan_resource *view);
VkImageLayout bcm2711_vulkan_image_level_layout(const struct bcm2711_vulkan_resource *image, uint32_t level);
void bcm2711_vulkan_image_set_level_layout(struct bcm2711_vulkan_resource *image, uint32_t level, VkImageLayout layout);
int bcm2711_vulkan_resource_backing(struct bcm2711_vulkan_resource *resource, uint64_t offset, uint64_t bytes, struct bcm2711_v3d_view **view, uint32_t *address, void **cpu);

#endif
