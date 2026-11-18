/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's synchronization objects.
 *
 * One queue runs every submission to its end before the next command is
 * decoded, so a semaphore has nothing to order: it is an identity and
 * nothing else.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_SYNC_H
#define DRIVERS_GPU_I915_VK_VK_SYNC_H

struct i915_render_session;
struct i915_wire_reader;
struct i915_wire_writer;

int drv_i915_gfx_create_semaphore(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

#endif
