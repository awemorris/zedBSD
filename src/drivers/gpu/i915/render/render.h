/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The entry of the Vulkan executor, as the GPU node's operations use it.
 *
 * The executor attaches to a device before its node is published, opens a
 * session for each open of the node, executes the Vulkan command streams
 * the sessions submit, reports the capset, and learns which blob is the
 * storage of which device memory allocation.
 */

#ifndef DRIVERS_GPU_I915_RENDER_RENDER_H
#define DRIVERS_GPU_I915_RENDER_RENDER_H

#include <stddef.h>
#include <stdint.h>

struct gpu_capset;
struct i915_device;
struct i915_session;
struct i915_gem_object;
struct i915_render_device;
struct i915_render_session;

int drv_i915_render_attach(struct i915_device *device, struct i915_render_device **out);
void drv_i915_render_detach(struct i915_render_device *vk);
int drv_i915_render_open(struct i915_render_device *vk, struct i915_session *gpu_session, struct i915_render_session **out);
void drv_i915_render_close(struct i915_render_session *session);
int drv_i915_render_execute(struct i915_render_session *session, const void *wire, size_t bytes, void *reply, size_t *reply_bytes);
int drv_i915_render_get_capset(const struct i915_render_device *vk, struct gpu_capset *capset);

/*
 * The blob libvulkan exports for a device memory allocation names it by
 * blob_id, and that blob is the allocation's storage; attach reports
 * ENOENT when no allocation has that identity.  The memory part of the
 * executor defines both.
 */
int drv_i915_render_blob_attach(struct i915_render_device *vk, struct i915_session *gpu, uint64_t blob_id, struct i915_gem_object *object);
void drv_i915_render_blob_detach(struct i915_render_device *vk, struct i915_gem_object *object);

#endif
