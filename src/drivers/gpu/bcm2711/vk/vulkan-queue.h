/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The one native queue executes complete validated submissions inside the ordered command worker. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_QUEUE_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_QUEUE_H

#include "drivers/gpu/bcm2711/vk/vulkan-private.h"

int bcm2711_vulkan_queue_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

struct bcm2711_render_session;
struct bcm2711_vulkan_native_job;

/*
 * The job the executor runs after current in its session: the next command
 * of the running submission, or the first command of the submission queued
 * behind it.  NULL when there is none yet.  Only the executor calls it.
 */
struct bcm2711_vulkan_native_job *bcm2711_vulkan_queue_next_job(struct bcm2711_render_session *render, const struct bcm2711_vulkan_native_job *current);

#endif
