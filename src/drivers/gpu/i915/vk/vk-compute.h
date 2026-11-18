/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Recorded dispatches on the GPU: a compute pipeline's kernel
 * over a grid of workgroups, through the GPGPU pipeline.
 *
 * A dispatch is an operation of the submission's batch like a draw: it
 * writes its slot (the interface descriptor, the group counts and the
 * CURBE) and appends its commands, which switch the pipeline to GPGPU and
 * back to 3D.  drv_i915_gfx_dispatch() does both for the executor;
 * drv_i915_gfx_dispatch_write() and drv_i915_gfx_dispatch_build() are the
 * two halves, which the host tests drive on their own.  An indirect
 * dispatch reads its group counts from a buffer when it runs.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_COMPUTE_H
#define DRIVERS_GPU_I915_VK_VK_COMPUTE_H

#include <stdint.h>

struct i915_gfx_batch;
struct i915_gfx_draw_state;
struct i915_gfx_kernels;
struct i915_gfx_op_space;
struct i915_gfx_pipeline;
struct i915_render_session;

/*
 * The grid of one dispatch: its three group counts (vkCmdDispatch), or,
 * when indirect_va is not zero, the GPU address of the three counts a
 * vkCmdDispatchIndirect reads when it runs (groups unused).
 */
struct i915_gfx_grid {
	uint32_t groups[3];
	uint64_t indirect_va;
};

int drv_i915_gfx_dispatch(struct i915_render_session *session, const struct i915_gfx_draw_state *state, const struct i915_gfx_grid *grid);
int drv_i915_gfx_dispatch_write(uint8_t *slot, uint64_t slot_va, const struct i915_gfx_draw_state *state, const struct i915_gfx_grid *grid);
int drv_i915_gfx_dispatch_write_gen(uint8_t *slot, uint64_t slot_va, const struct i915_gfx_draw_state *state, const struct i915_gfx_grid *grid, unsigned graphics_ver);
void drv_i915_gfx_dispatch_build(struct i915_gfx_batch *batch, const struct i915_gfx_op_space *space, const struct i915_gfx_pipeline *pipeline, const struct i915_gfx_kernels *kernels, const struct i915_gfx_grid *grid, uint32_t dss_count, uint32_t mocs);

#endif
