/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's render passes and framebuffers.
 *
 * A render pass is one subpass with at most one colour attachment and one
 * depth attachment; anything else is refused by name when it is created.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_RENDER_PASS_H
#define DRIVERS_GPU_I915_VK_VK_RENDER_PASS_H

struct i915_render_session;
struct i915_wire_reader;
struct i915_wire_writer;

int drv_i915_gfx_create_render_pass(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_create_framebuffer(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

#endif
