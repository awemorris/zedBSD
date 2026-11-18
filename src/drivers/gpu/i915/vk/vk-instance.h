/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The instance, physical-device, device and queue commands of the Vulkan
 * executor.
 *
 * What is reported is one Gen12 device: one memory type that is both
 * device-local and host-coherent (the GPU shares the system's memory), and
 * one queue family of one graphics queue on the render engine, and on a
 * device that offers video decode a second family of one video decode queue.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_INSTANCE_H
#define DRIVERS_GPU_I915_VK_VK_INSTANCE_H

#include "vk-internal.h"

#include <stdint.h>

int drv_i915_render_instance_dispatch(struct i915_render_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
uint32_t drv_i915_render_queue_family(struct i915_render_session *session, uint64_t identity);

#endif
