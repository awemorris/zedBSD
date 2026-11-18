/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Bounded writing of a batch buffer.
 *
 * The draw, state and rectangle paths write their commands into a batch
 * object one dword at a time.  The writer never writes past the capacity it
 * was given; a batch that does not fit is remembered as overflowed and the
 * builder refuses it once it is complete, so a command list is never cut off
 * in the middle.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_BATCH_H
#define DRIVERS_GPU_I915_VK_VK_BATCH_H

#include <stdint.h>

/*
 * One batch under construction.
 *
 * It lives on the builder's stack and points into the CPU view of the batch
 * object.  count keeps counting past the capacity, so an overflowed batch
 * still reports how many dwords it needed.
 */
struct i915_gfx_batch {
	/* Where the dwords are written. */
	uint32_t *cmds;

	/* How many dwords were emitted, and how many fit. */
	unsigned count;
	unsigned capacity;

	/* Nonzero once a dword did not fit. */
	int overflow;

	/*
	 * The graphics version the commands are built for: 9
	 * selects the Gen9 forms of the packets that differ; any other value
	 * (12, or 0 in a batch a test makes by hand) the Gen12 ones.  The draw
	 * path sets it from the GT when it makes the session's batch.
	 */
	unsigned graphics_ver;
};

void drv_i915_batch_emit(struct i915_gfx_batch *batch, uint32_t dword);
void drv_i915_batch_zero(struct i915_gfx_batch *batch, uint32_t opcode, uint32_t dwords);
void drv_i915_batch_words(struct i915_gfx_batch *batch, const uint32_t *words, unsigned count);
void drv_i915_batch_pointer(struct i915_gfx_batch *batch, uint32_t opcode, uint32_t value);
void drv_i915_batch_pipe_control(struct i915_gfx_batch *batch, uint32_t flags);
void drv_i915_batch_pipe_control_hdc(struct i915_gfx_batch *batch, uint32_t flags);

#endif
