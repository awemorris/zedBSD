/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shares completion payloads between native queue operations and WSI acquisition.
 */

#ifndef VULKAN_SYNC_INTERNAL_H
#define VULKAN_SYNC_INTERNAL_H

#include "internal.h"

/* A device mutex protects the software payload of one ordinary sync object. */
struct vulkan_sync {
	struct vulkan_object object;
	VkBool32 software_signaled;
};

/* These helpers never acquire a device mutex; their caller already owns it. */
struct vulkan_sync *vulkan_sync_object(uint64_t handle);
VkResult vulkan_sync_device_status_locked(struct VkDevice_T *device);
VkResult vulkan_sync_status_locked(struct VkDevice_T *device, struct vulkan_sync *sync, uint32_t opcode);
void vulkan_sync_device_error(struct VkDevice_T *device, VkResult status);

/* Clock sampling and cooperative pauses are performed without Vulkan locks. */
VkResult vulkan_sync_clock(uint64_t *nanoseconds);
VkResult vulkan_sync_pause(uint64_t nanoseconds);

#endif
