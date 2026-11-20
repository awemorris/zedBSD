/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Immutable ordered graphics records retain typed handles while submission freezes mutable descriptor contents. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_RECORD_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_RECORD_H

#include "drivers/gpu/videocore/vk/vulkan-command.h"
#include "drivers/gpu/videocore/vk/vulkan-pipeline.h"

/* Every primary shares this whole immutable recording budget across graphics, transfer and dependency nodes. */
#define BCM2711_VULKAN_RECORD_BYTES (8U * 1024U * 1024U)

/*
 * A vkCmdBeginRenderPass record keeps attachment a's clear value in words
 * 4a to 4a + 3 (a colour's four words, or a depth float's bits and the
 * stencil value); count is the number of clear values.
 *
 * A vkCmdClearAttachments record keeps colour slot s's clear in words 4s
 * to 4s + 3, the depth bits and the stencil value in words 16 and 17, the
 * aspects cleared in word 18 (the depth and stencil aspect bits, and bit
 * 8 + s for colour slot s), and each rectangle (x, y, width, height) from
 * word 20; count is the number of rectangles.
 */
#define BCM2711_VULKAN_CLEAR_RECTS 4U
#define BCM2711_VULKAN_CLEAR_DEPTH 16U
#define BCM2711_VULKAN_CLEAR_ASPECTS 18U
#define BCM2711_VULKAN_CLEAR_COLOUR_SHIFT 8U
#define BCM2711_VULKAN_CLEAR_RECT_WORD 20U

/*
 * A vkCmdClearColorImage record keeps the colour in words 0 to 3 and each
 * subresource range in one word from word 4 (count is the number of
 * ranges): its first level in bits 0 to 3, its level count in bits 4 to 7,
 * its first layer in bits 8 to 15 and its layer count in bits 16 to 24.  A
 * count of zero stands for VK_REMAINING_MIP_LEVELS or
 * VK_REMAINING_ARRAY_LAYERS: every level or layer from the first one.  A
 * 3D image's range names its one layer, and each of its levels is cleared
 * in all the slices the level has.
 */
#define BCM2711_VULKAN_CLEAR_RANGE_WORD 4U
#define BCM2711_VULKAN_CLEAR_RANGES 32U
#define BCM2711_VULKAN_CLEAR_LEVEL_SHIFT 0U
#define BCM2711_VULKAN_CLEAR_LEVELS_SHIFT 4U
#define BCM2711_VULKAN_CLEAR_LAYER_SHIFT 8U
#define BCM2711_VULKAN_CLEAR_LAYERS_SHIFT 16U

/* The words a record holds: the push constants', and a clear's values and rectangles. */
#define BCM2711_VULKAN_RECORD_WORDS 36U

/* Events one chunk of a recording holds. */
#define BCM2711_VULKAN_RECORD_CHUNK 128U

/* One bounded graphics event copies its selected scalar payload and independently retains every referenced object. */
struct bcm2711_vulkan_record {
	struct bcm2711_vulkan_command_node node;
	uint32_t opcode;
	struct bcm2711_vulkan_object *objects[BCM2711_VULKAN_VERTEX_BINDINGS + 1U];
	/* Each bound set's update generation makes ordinary update-after-record detectable before End or submission. */
	uint64_t generations[BCM2711_VULKAN_PIPELINE_SETS];
	uint32_t first;
	uint32_t count;
	/* A set binding's dynamic offsets, which its words hold in binding order. */
	uint32_t dynamic_count;
	VkShaderStageFlags stages;
	uint32_t words[BCM2711_VULKAN_RECORD_WORDS];
	uint64_t offsets[BCM2711_VULKAN_VERTEX_BINDINGS];
	VkRect2D area;
	/*
	 * A draw recorded by vkCmdDrawIndirect or vkCmdDrawIndexedIndirect: its
	 * buffer in objects[0] and the record's offset in offsets[0], its
	 * counts read when the draw is prepared.
	 */
	bool indirect;
	/* Image transfers preserve their explicit execution layout and fully consumed semantic refusal. */
	VkImageLayout layout;
	int semantic_error;
};

/* Frees a cleared recording's event chunks; every event has been released. */
void bcm2711_vulkan_record_chunks_free(struct bcm2711_vulkan_command_buffer *command);

int bcm2711_vulkan_record_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_record_current(struct bcm2711_vulkan_command_buffer *command);
int bcm2711_vulkan_record_decode(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
int bcm2711_vulkan_record_validate(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);

#endif
