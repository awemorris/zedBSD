/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The native command transport uses one complete table of implemented typed routes. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_DISPATCH_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_DISPATCH_H

#include "drivers/gpu/bcm2711/vulkan-private.h"

int bcm2711_vulkan_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

#endif
