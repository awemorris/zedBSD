/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Ordered graphics state is borrowed only while a serialized preparation callback acquires independent native owners. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_DRAW_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_DRAW_H

#include "drivers/gpu/bcm2711/vulkan-record.h"
#include "drivers/gpu/bcm2711/vulkan-descriptor.h"
#include "drivers/gpu/bcm2711/vulkan-resource.h"

/* One bound set remembers the layout that selected it, including disturbed bindings whose descriptors are now undefined. */
struct bcm2711_vulkan_draw_set {
	struct bcm2711_vulkan_descriptor_set *set;
	const struct bcm2711_vulkan_pipeline_layout *layout;
};

/* One temporary walk keeps exact stage-specific push words and retained-record selections without owning an application pointer. */
struct bcm2711_vulkan_draw_state {
	const struct bcm2711_vulkan_record *pass;
	struct bcm2711_vulkan_pipeline *pipeline;
	struct bcm2711_vulkan_resource *vertices[BCM2711_VULKAN_VERTEX_BINDINGS];
	uint64_t offsets[BCM2711_VULKAN_VERTEX_BINDINGS];
	struct bcm2711_vulkan_draw_set sets[BCM2711_VULKAN_PIPELINE_SETS];
	const struct bcm2711_vulkan_record *viewport;
	const struct bcm2711_vulkan_record *scissor;
	uint32_t push[2][BCM2711_VULKAN_PUSH_WORDS];
	const struct bcm2711_vulkan_pipeline_layout *push_layouts[2][BCM2711_VULKAN_PUSH_WORDS];
};

/* Callbacks may prepare CPU/native owner graphs only; they never launch DMA and must discard every prepared prefix on failure. */
typedef int (*bcm2711_vulkan_draw_prepare_t)(void *payload, const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *record);

int bcm2711_vulkan_draw_walk(struct bcm2711_vulkan_command_buffer *command, bcm2711_vulkan_draw_prepare_t prepare, void *payload);
int bcm2711_vulkan_draw_validate(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *draw);
int bcm2711_vulkan_draw_target(const struct bcm2711_vulkan_record *record);
int bcm2711_vulkan_draw_descriptor(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_shader_uniform *uniform, VkDescriptorType type, const struct bcm2711_vulkan_descriptor **descriptor);

#endif
