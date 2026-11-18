/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Rectangles on the GPU: fills and copies between linear surfaces.
 *
 * Every transfer the executor performs -- attachment and image clears,
 * buffer and image copies, image copies and blits, and the display's scaled
 * copy into the scanout buffer -- is one RECTLIST drawn by the 3D pipeline
 * on the render engine.  A rectangle names its
 * surfaces by GPU address, extent, pitch and format (struct i915_gfx_surface)
 * and holds no Vulkan object.
 *
 * A copy scales when the two rectangles differ in size, with a nearest or a
 * linear filter.  A fill writes four float words at every pixel; a depth
 * clear writes the depth value's bits through an R32_FLOAT surface.
 *
 * The rectangle is written into the session's state and batch objects
 * (vk-draw.h), so it shares them with the session's draws and runs between
 * them.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_BLIT_H
#define DRIVERS_GPU_I915_VK_VK_BLIT_H

#include <stdint.h>

#include "vk-gfx.h"

struct i915_render_session;

/*
 * Copies, blits and clears of rectangles on the render engine, drawn as a
 * RECTLIST.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/blorp/blorp_blit.c and
 * blorp_genX_exec_brw.h (blorp_exec).
 */
int drv_i915_gfx_rect_prepare(struct i915_render_session *session);
int drv_i915_gfx_rect_build(struct i915_render_session *session, const struct i915_gfx_surface *dst, const struct i915_gfx_rect *dst_rect, const struct i915_gfx_surface *src, const struct i915_gfx_rect *src_rect, const uint32_t clear[4], int linear, uint64_t *batch_va);
int drv_i915_gfx_rect(struct i915_render_session *session, const struct i915_gfx_surface *dst, const struct i915_gfx_rect *dst_rect, const struct i915_gfx_surface *src, const struct i915_gfx_rect *src_rect, const uint32_t clear[4], int linear);
int drv_i915_gfx_resolve(struct i915_render_session *session, const struct i915_gfx_surface *dst, const struct i915_gfx_rect *dst_rect, const struct i915_gfx_surface *samples, const struct i915_gfx_rect *src_rect);

#endif
