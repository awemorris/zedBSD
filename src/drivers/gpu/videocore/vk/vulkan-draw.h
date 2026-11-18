/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Ordered graphics state is borrowed only while a serialized preparation callback acquires independent native owners. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_DRAW_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_DRAW_H

#include "drivers/gpu/videocore/vk/vulkan-record.h"
#include "drivers/gpu/videocore/vk/vulkan-descriptor.h"
#include "drivers/gpu/videocore/vk/vulkan-resource.h"

struct bcm2711_vulkan_occlusion_pool;

/* One bound set remembers the layout that selected it, including disturbed bindings whose descriptors are now undefined. */
struct bcm2711_vulkan_draw_set {
	struct bcm2711_vulkan_descriptor_set *set;
	const struct bcm2711_vulkan_pipeline_layout *layout;
	/* The offset the bind gave each dynamic uniform buffer, by canonical binding slot (zero for the others). */
	uint32_t dynamic[BCM2711_VULKAN_LAYOUT_BINDINGS];
};

/* One temporary walk keeps exact stage-specific push words and retained-record selections without owning an application pointer. */
struct bcm2711_vulkan_draw_state {
	/* The opcode of the record being walked, which a refusal names. */
	uint32_t opcode;
	const struct bcm2711_vulkan_record *pass;
	struct bcm2711_vulkan_pipeline *pipeline;
	struct bcm2711_vulkan_resource *vertices[BCM2711_VULKAN_VERTEX_BINDINGS];
	uint64_t offsets[BCM2711_VULKAN_VERTEX_BINDINGS];
	/* The bound index buffer, its offset and its VkIndexType; NULL before any bind. */
	struct bcm2711_vulkan_resource *indices;
	uint64_t index_offset;
	uint32_t index_type;
	struct bcm2711_vulkan_draw_set sets[BCM2711_VULKAN_PIPELINE_SETS];
	const struct bcm2711_vulkan_record *viewport;
	const struct bcm2711_vulkan_record *scissor;
	/* The other dynamic states, as the Set commands last recorded them (zero before any). */
	struct bcm2711_vulkan_dynamic dynamic;
	uint32_t push[2][BCM2711_VULKAN_PUSH_WORDS];
	const struct bcm2711_vulkan_pipeline_layout *push_layouts[2][BCM2711_VULKAN_PUSH_WORDS];

	/*
	 * The compute bind point: its pipeline, its sets and the compute
	 * stage's pushed words.  A dispatch sees them through a view of this
	 * state in which they take the places of the graphics pipeline, the
	 * graphics sets and the vertex stage's words.
	 */
	struct bcm2711_vulkan_pipeline *compute_pipeline;
	struct bcm2711_vulkan_draw_set compute_sets[BCM2711_VULKAN_PIPELINE_SETS];
	uint32_t compute_push[BCM2711_VULKAN_PUSH_WORDS];
	const struct bcm2711_vulkan_pipeline_layout *compute_push_layouts[BCM2711_VULKAN_PUSH_WORDS];

	/*
	 * The active occlusion query, its pool NULL when there is none, and
	 * the render pass it began in (NULL: outside one), where it ends.
	 */
	struct bcm2711_vulkan_occlusion_pool *occlusion;
	uint32_t occlusion_query;
	const struct bcm2711_vulkan_record *occlusion_pass;
};

/* Callbacks may prepare CPU/native owner graphs only; they never launch DMA and must discard every prepared prefix on failure. */
typedef int (*bcm2711_vulkan_draw_prepare_t)(void *payload, const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *record);

int bcm2711_vulkan_draw_walk(struct bcm2711_vulkan_command_buffer *command, bcm2711_vulkan_draw_prepare_t prepare, void *payload);
int bcm2711_vulkan_draw_validate(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *draw);
int bcm2711_vulkan_dispatch_validate(const struct bcm2711_vulkan_draw_state *view, const struct bcm2711_vulkan_record *dispatch);
int bcm2711_vulkan_draw_target(const struct bcm2711_vulkan_record *record);
int bcm2711_vulkan_draw_descriptor(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_shader_uniform *uniform, VkDescriptorType type, const struct bcm2711_vulkan_descriptor **descriptor);
int bcm2711_vulkan_draw_dynamic_offset(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_shader_uniform *uniform, const struct bcm2711_vulkan_descriptor *descriptor, uint64_t *offset);

#endif
