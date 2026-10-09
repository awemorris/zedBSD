/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Immutable ordered graphics records retain typed handles while submission freezes mutable descriptor contents. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_RECORD_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_RECORD_H

#include "drivers/gpu/bcm2711/vulkan-command.h"
#include "drivers/gpu/bcm2711/vulkan-pipeline.h"

/* Every primary shares this whole immutable recording budget across graphics, transfer and dependency nodes. */
#define BCM2711_VULKAN_RECORD_BYTES (1024U * 1024U)

/* One bounded graphics event copies its selected scalar payload and independently retains every referenced object. */
struct bcm2711_vulkan_record {
	struct bcm2711_vulkan_command_node node;
	uint32_t opcode;
	struct bcm2711_vulkan_object *objects[BCM2711_VULKAN_VERTEX_BINDINGS + 1U];
	/* Each bound set's update generation makes ordinary update-after-record detectable before End or submission. */
	uint64_t generations[BCM2711_VULKAN_PIPELINE_SETS];
	uint32_t first;
	uint32_t count;
	VkShaderStageFlags stages;
	uint32_t words[BCM2711_VULKAN_PUSH_WORDS];
	uint64_t offsets[BCM2711_VULKAN_VERTEX_BINDINGS];
	VkRect2D area;
	/* Image transfers preserve their explicit execution layout and fully consumed semantic refusal. */
	VkImageLayout layout;
	int semantic_error;
};

int bcm2711_vulkan_record_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_record_current(struct bcm2711_vulkan_command_buffer *command);
int bcm2711_vulkan_record_decode(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
int bcm2711_vulkan_record_validate(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);

#endif
