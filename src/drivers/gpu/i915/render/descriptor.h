/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's descriptor set layouts, pools and sets.
 *
 * A set is a small host object that records, per binding, the image view
 * and the sampler a draw samples.  A pool bounds nothing.
 */

#ifndef DRIVERS_GPU_I915_RENDER_DESCRIPTOR_H
#define DRIVERS_GPU_I915_RENDER_DESCRIPTOR_H

struct i915_render_session;
struct i915_wire_reader;
struct i915_wire_writer;

int drv_i915_gfx_create_dsl(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_create_dpool(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_allocate_dsets(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

/* Destroys a descriptor pool with its sets (vkDestroyDescriptorPool), or frees one whose identity is withdrawn. */
int drv_i915_gfx_destroy_dpool(struct i915_render_session *session, struct i915_wire_reader *reader);
void drv_i915_gfx_dpool_free(struct i915_render_session *session, void *pool);
int drv_i915_gfx_update_dsets(struct i915_render_session *session, struct i915_wire_reader *reader);

#endif
