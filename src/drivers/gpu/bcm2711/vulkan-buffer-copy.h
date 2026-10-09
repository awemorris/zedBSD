/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Coherent buffer copies retain exact typed resources and execute only after preceding FIFO native retirement. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_BUFFER_COPY_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_BUFFER_COPY_H

#include "drivers/gpu/bcm2711/vulkan-record.h"

#define BCM2711_VULKAN_BUFFER_COPY_REGIONS 64U

/* One opcode-selected complete region vector uses standard copied fields without storing application pointers. */
union bcm2711_vulkan_buffer_copy_regions {
	VkBufferCopy buffers[BCM2711_VULKAN_BUFFER_COPY_REGIONS];
	VkBufferImageCopy images[BCM2711_VULKAN_BUFFER_COPY_REGIONS];
};

/* One immutable transfer owns the whole copied vector and both independent typed input edges until its pending primary retires. */
struct bcm2711_vulkan_buffer_copy {
	struct bcm2711_vulkan_record record;
	union bcm2711_vulkan_buffer_copy_regions regions;
	bool retained[2];
};

int bcm2711_vulkan_buffer_copy_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_buffer_copy_validate(const struct bcm2711_vulkan_record *record, struct bcm2711_vulkan_object *device);
int bcm2711_vulkan_buffer_copy_run(const struct bcm2711_vulkan_record *record);

#endif
