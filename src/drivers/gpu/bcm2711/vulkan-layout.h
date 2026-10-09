/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Immutable descriptor and pipeline layouts supply bounded shader interfaces independently of public layout identities. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_LAYOUT_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_LAYOUT_H

#include "drivers/gpu/bcm2711/vulkan-input.h"

#define BCM2711_VULKAN_LAYOUT_BINDINGS 12U
#define BCM2711_VULKAN_PIPELINE_SETS 4U
#define BCM2711_VULKAN_PUSH_WORDS 32U

/* One canonical binding supplies a single sampled image or uniform block and optionally owns its immutable sampler. */
struct bcm2711_vulkan_binding_layout {
	uint32_t number;
	VkDescriptorType type;
	VkShaderStageFlags stages;
	struct bcm2711_vulkan_object *immutable;
};

/* One retained descriptor layout bounds exact shader-visible resources and owns every immutable sampler dependency. */
struct bcm2711_vulkan_set_layout {
	struct bcm2711_vulkan_input_owner owner;
	uint32_t count;
	uint32_t textures;
	uint32_t uniforms;
	struct bcm2711_vulkan_binding_layout bindings[BCM2711_VULKAN_LAYOUT_BINDINGS];
};

/* One pipeline layout retains each set interface and exact per-word stage permission for its finite push range. */
struct bcm2711_vulkan_pipeline_layout {
	struct bcm2711_vulkan_input_owner owner;
	uint32_t count;
	struct bcm2711_vulkan_object *sets[BCM2711_VULKAN_PIPELINE_SETS];
	VkShaderStageFlags push[BCM2711_VULKAN_PUSH_WORDS];
	/* Exact range grouping distinguishes one combined-stage range from two otherwise identical per-stage ranges. */
	VkPushConstantRange ranges[2];
	uint32_t range_count;
};

int bcm2711_vulkan_layout_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_layout_set_compatible(const struct bcm2711_vulkan_set_layout *first, const struct bcm2711_vulkan_set_layout *second);
int bcm2711_vulkan_layout_push_compatible(const struct bcm2711_vulkan_pipeline_layout *first, const struct bcm2711_vulkan_pipeline_layout *second);
int bcm2711_vulkan_layout_compatible(const struct bcm2711_vulkan_pipeline_layout *first, const struct bcm2711_vulkan_pipeline_layout *second, uint32_t set);

#endif
