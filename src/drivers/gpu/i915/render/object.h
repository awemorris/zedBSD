/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan object table.
 *
 * libvulkan numbers the objects it creates, and the executor remembers
 * which of its objects each number names.  The table indexes the objects
 * by session, kind and identity (every process numbers its objects from
 * the same start); the objects themselves belong to the parts that created
 * them, which free them.
 */

#ifndef DRIVERS_GPU_I915_RENDER_OBJECT_H
#define DRIVERS_GPU_I915_RENDER_OBJECT_H

#include "internal.h"

int drv_i915_object_table_create(struct i915_object_table **out);
void drv_i915_object_table_destroy(struct i915_object_table *table);
int drv_i915_object_insert(struct i915_render_session *session, enum i915_vk_object_kind kind, i915_vk_handle handle, void *object);
void *drv_i915_object_lookup(struct i915_render_session *session, enum i915_vk_object_kind kind, i915_vk_handle handle);
void drv_i915_object_remove(struct i915_render_session *session, enum i915_vk_object_kind kind, i915_vk_handle handle);
void *drv_i915_object_take(struct i915_render_session *session, enum i915_vk_object_kind kind, int (*match)(void *object, void *argument), void *argument);
void drv_i915_object_forget(struct i915_render_session *session);

#endif
