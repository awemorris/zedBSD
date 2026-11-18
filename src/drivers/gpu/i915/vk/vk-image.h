/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's images, image views and samplers.
 *
 * Every image is a linear 2D image of one layer, with one or more mip
 * levels, in one of the formats the executor lays out; anything else is
 * refused by name when it is created.  A view is a range of the image's
 * levels.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_IMAGE_H
#define DRIVERS_GPU_I915_VK_VK_IMAGE_H

struct i915_render_session;
struct i915_wire_reader;
struct i915_wire_writer;

int drv_i915_gfx_create_image(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_create_image_view(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_create_sampler(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_gfx_subresource_layout(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

#endif
