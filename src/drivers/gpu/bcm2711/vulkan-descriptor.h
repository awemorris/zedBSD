/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Descriptor pools bound retained set storage while independent draw snapshots retain the native resources they actually use. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_DESCRIPTOR_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_DESCRIPTOR_H

#include "drivers/gpu/bcm2711/vulkan-layout.h"

/* One pool owns its device and counts charges until each old or current set's final owner retires. */
struct bcm2711_vulkan_descriptor_pool {
	struct bcm2711_vulkan_input_owner owner;
	VkDescriptorPoolCreateFlags flags;
	uint32_t maximum_sets;
	uint32_t maximum_textures;
	uint32_t maximum_uniforms;
	uint32_t sets;
	uint32_t textures;
	uint32_t uniforms;
};

/* One initialized binding independently retains its exact sampled view/sampler or uniform-buffer interval. */
struct bcm2711_vulkan_descriptor {
	struct bcm2711_vulkan_object *view;
	struct bcm2711_vulkan_object *sampler;
	struct bcm2711_vulkan_object *buffer;
	uint64_t offset;
	uint64_t bytes;
	VkImageLayout image_layout;
};

/* One set retains its pool/layout and immutable native bindings even after reset withdraws its public identity. */
struct bcm2711_vulkan_descriptor_set {
	struct bcm2711_vulkan_input_owner owner;
	struct bcm2711_vulkan_object *pool;
	struct bcm2711_vulkan_object *layout;
	struct bcm2711_vulkan_descriptor bindings[BCM2711_VULKAN_LAYOUT_BINDINGS];
	bool charged;
};

int bcm2711_vulkan_descriptor_update_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_descriptor_clone(const struct bcm2711_vulkan_descriptor *source, struct bcm2711_vulkan_descriptor *destination);
int bcm2711_vulkan_descriptor_sets_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_descriptor_release(struct bcm2711_vulkan_descriptor *descriptor);
int bcm2711_vulkan_descriptor_pool_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

#endif
