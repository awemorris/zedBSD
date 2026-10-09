/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Serialized native operations use coherent RAM and whole-engine completion to implement explicit Vulkan dependencies. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_BARRIER_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_BARRIER_H

#include "drivers/gpu/bcm2711/vulkan-record.h"
#include "drivers/gpu/bcm2711/vulkan-resource.h"

/* One node bounds the combined memory, buffer and image arrays, including Keiland's batch of thirty-two imports. */
#define BCM2711_VULKAN_BARRIERS 64U

/* One immutable full-colour or buffer dependency owns its exact typed resource until the primary graph retires. */
struct bcm2711_vulkan_barrier_entry {
	struct bcm2711_vulkan_object *object;
	VkAccessFlags source;
	VkAccessFlags destination;
	uint64_t offset;
	uint64_t bytes;
	VkImageLayout before;
	VkImageLayout after;
	uint32_t source_family;
	uint32_t destination_family;
	VkImageSubresourceRange range;
	bool retained;
};

/* One barrier node shares the graphics record prefix while retaining its complete independently bounded dependency array. */
struct bcm2711_vulkan_barrier {
	struct bcm2711_vulkan_record record;
	VkPipelineStageFlags source;
	VkPipelineStageFlags destination;
	VkDependencyFlags flags;
	uint32_t count;
	int semantic_error;
	struct bcm2711_vulkan_barrier_entry entries[BCM2711_VULKAN_BARRIERS];
};

int bcm2711_vulkan_barrier_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_barrier_decode(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_barrier *barrier);
int bcm2711_vulkan_barrier_validate(const struct bcm2711_vulkan_barrier *barrier, struct bcm2711_vulkan_object *device);
int bcm2711_vulkan_barrier_run(const struct bcm2711_vulkan_record *record);

#endif
