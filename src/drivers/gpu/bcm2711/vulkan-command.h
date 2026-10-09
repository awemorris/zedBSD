/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Recorded primary commands retain their exact typed inputs until reset or final native retirement. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_COMMAND_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_COMMAND_H

#include "drivers/gpu/bcm2711/vulkan-input.h"

/* A pending submission keeps an executable recording immutable; invalid recordings require a permitted reset. */
enum bcm2711_vulkan_command_state {
	BCM2711_VULKAN_COMMAND_INITIAL,
	BCM2711_VULKAN_COMMAND_RECORDING,
	BCM2711_VULKAN_COMMAND_EXECUTABLE,
	BCM2711_VULKAN_COMMAND_INVALID
};

struct bcm2711_vulkan_command_buffer;

/* A pool's borrowed child list includes withdrawn buffers still retained by native work, without an ownership cycle. */
struct bcm2711_vulkan_command_pool {
	struct bcm2711_vulkan_input_owner owner;
	struct bcm2711_vulkan_command_buffer *children;
	VkCommandPoolCreateFlags flags;
};

/* Every independently allocated recording node releases all typed edges it acquired before its host storage retires. */
struct bcm2711_vulkan_command_node {
	struct bcm2711_vulkan_command_node *next;
	int (*release)(struct bcm2711_vulkan_command_node *node);
};

/* One primary recording owns its pool and nodes while pending counts prohibit mutation through any public identity. */
struct bcm2711_vulkan_command_buffer {
	struct bcm2711_vulkan_input_owner owner;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_buffer *next;
	struct bcm2711_vulkan_command_buffer **previous;
	struct bcm2711_vulkan_command_node *first;
	struct bcm2711_vulkan_command_node *last;
	enum bcm2711_vulkan_command_state state;
	VkCommandBufferUsageFlags flags;
	uint32_t pending;
	uint32_t recorded_bytes;
	int recording_error;
	bool render_open;
};

int bcm2711_vulkan_command_pool_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_command_batch_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_command_buffer_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_command_clear(struct bcm2711_vulkan_command_buffer *command);
int bcm2711_vulkan_command_release(struct bcm2711_vulkan_session *session, void *payload);

#endif
