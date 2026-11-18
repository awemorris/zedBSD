/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's pipeline layouts, shader modules, and graphics and compute
 * pipelines.
 *
 * A pipeline is decoded from its create info into the state a draw needs,
 * and its kernels are prepared when it is created; the preparation and the
 * release of the kernels are declared in vk-gfx.h.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_PIPELINE_H
#define DRIVERS_GPU_I915_VK_VK_PIPELINE_H

struct i915_render_session;
struct i915_wire_reader;
struct i915_wire_writer;

int drv_i915_gfx_create_pipeline_layout(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_create_shader(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_create_pipelines(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_create_compute_pipelines(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_destroy_pipeline(struct i915_render_session *session, struct i915_wire_reader *reader);

#endif
