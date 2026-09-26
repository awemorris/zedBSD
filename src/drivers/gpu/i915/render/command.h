/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Command pools, command buffers, recording and vkQueueSubmit of the
 * executor's graphics path.
 *
 * A recording command arrives as [opcode][0][command buffer][arguments]
 * (libvulkan commands.c command_record_begin); what it says is kept as one
 * entry of the command buffer's operation list (gfx.h).
 *
 * vkQueueSubmit walks the operation lists in order and finishes every
 * operation before it replies: every draw, clear, copy and blit is one GPU
 * batch run to its end, and no CPU touches pixels.  The fence of the
 * submission is signalled when vkQueueSubmit replies.
 * XXX: one batch per operation and a synchronous submit: no batching, no
 * overlap between the CPU and the GPU.
 */

#ifndef DRIVERS_GPU_I915_RENDER_COMMAND_H
#define DRIVERS_GPU_I915_RENDER_COMMAND_H

#include <stdint.h>

struct i915_gfx_cmdpool;
struct i915_render_session;
struct i915_wire_reader;
struct i915_wire_writer;

int drv_i915_gfx_rec_dispatch(struct i915_render_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/* Frees a command pool whose identity is withdrawn, with its buffers. */
void drv_i915_gfx_command_pool_free(struct i915_render_session *session, struct i915_gfx_cmdpool *pool);

#endif
