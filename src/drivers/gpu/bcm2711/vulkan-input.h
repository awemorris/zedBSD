/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Immutable shader, sampler and colour-view inputs retain every logical parent through final prepared-job retirement. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_INPUT_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_INPUT_H

#include "drivers/gpu/bcm2711/vulkan-private.h"

/* Every ordinary typed input owns its device and optional image dependency independently of public identities. */
struct bcm2711_vulkan_input_owner {
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *parent;
};

/* One full-colour 2D image view keeps its underlying image, format and native component selection immutable. */
struct bcm2711_vulkan_image_view {
	struct bcm2711_vulkan_input_owner owner;
	VkFormat format;
	VkComponentMapping components;
};

/* One normalized single-level sampler supplies exact nearest/linear filtering and native wrap modes at draw preparation. */
struct bcm2711_vulkan_sampler {
	struct bcm2711_vulkan_input_owner owner;
	VkFilter mag;
	VkFilter min;
	VkSamplerAddressMode u;
	VkSamplerAddressMode v;
};

/* One copied SPIR-V module remains independent of the per-command arena until its last compiled-pipeline owner retires. */
struct bcm2711_vulkan_module {
	struct bcm2711_vulkan_input_owner owner;
	uint32_t word_count;
	uint32_t words[1];
};

int bcm2711_vulkan_input_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

#endif
