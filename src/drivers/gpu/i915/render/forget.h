/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a session's objects let go of when one of them is destroyed
 * (BUG-260).
 *
 * The executor's objects name each other by pointer: a view its image, a
 * buffer view its buffer, a descriptor set its views, samplers, buffers and
 * buffer views, a framebuffer its views, and a command buffer's recorded
 * operations their buffers, images, sets, pipelines, passes, framebuffers
 * and query pools.  Vulkan has the application destroy an object only
 * after what names it, but an application that does not must not leave
 * the kernel reading a freed record: before an object is freed, what names
 * it lets go of it.  A view whose image is destroyed keeps no image and is
 * let go of as if it were destroyed too; a command buffer that recorded the
 * object loses its operations and is not run (forget.c, command.c).
 */

#ifndef DRIVERS_GPU_I915_RENDER_FORGET_H
#define DRIVERS_GPU_I915_RENDER_FORGET_H

#include "gfx.h"
#include "internal.h"

void drv_i915_gfx_forget(struct i915_render_session *session, enum i915_vk_object_kind kind, void *object);
int drv_i915_gfx_op_names(const struct i915_gfx_op *op, const void *object);
unsigned drv_i915_gfx_command_forget(struct i915_render_session *session, const void *object);
struct i915_gfx_view *drv_i915_gfx_view_lookup(struct i915_render_session *session, uint64_t identity);

#endif
