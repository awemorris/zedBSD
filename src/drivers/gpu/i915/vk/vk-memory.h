/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's device memory and buffers.
 *
 * A VkDeviceMemory has no storage of its own: libvulkan exports a blob for
 * it right after the allocation, and that blob, named by the allocation's
 * identity, is its storage.  Buffers and images are bound to a range of an
 * allocation.  The CPU view and the GPU address of a range are declared in
 * vk-gfx.h, and the blob attach and detach in vk.h; this file defines them.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_MEMORY_H
#define DRIVERS_GPU_I915_VK_VK_MEMORY_H

#include <stdint.h>

struct i915_render_session;
struct i915_wire_reader;
struct i915_wire_writer;

int drv_i915_gfx_allocate_memory(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_free_memory(struct i915_render_session *session, struct i915_wire_reader *reader);
int drv_i915_gfx_bind(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int image);
int drv_i915_gfx_requirements(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int image);
int drv_i915_gfx_create_buffer(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_create_buffer_view(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

#endif
