/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Instance, physical, logical-device and queue payloads retain their parent independently of wire identities. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_DEVICE_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_DEVICE_H

#include "drivers/gpu/bcm2711/vulkan-private.h"

/* Each native root owns one parent edge; queue roots additionally own a guarded completion domain. */
struct bcm2711_vulkan_root {
	struct bcm2711_vulkan_object *parent;
	uint32_t queue_count;
	uint32_t timeline;
	uint32_t index;
};

int bcm2711_vulkan_device_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_query_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

#endif
