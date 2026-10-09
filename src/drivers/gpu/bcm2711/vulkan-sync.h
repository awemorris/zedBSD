/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native sync payloads remain controller-serialized and independent of common supervised completion markers. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_SYNC_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_SYNC_H

#include "drivers/gpu/bcm2711/vulkan-private.h"

/*
 * One retained logical device owns each fence or binary semaphore until all independently pending queue references retire.
 * Fence completion is explicit; semaphore sequence pairs distinguish reserved FIFO operations from actual retired signals and waits.
 */
struct bcm2711_vulkan_sync {
	struct bcm2711_vulkan_object *device;
	uint64_t signals_reserved;
	uint64_t waits_reserved;
	uint64_t signals_completed;
	uint64_t waits_completed;
	VkResult status;
	bool signaled;
	bool pending;
};

int bcm2711_vulkan_sync_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

#endif
