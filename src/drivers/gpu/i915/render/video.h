/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video decode part of the Vulkan executor (ws083): H.264 decode on the
 * video decode engine VCS0, offered only on a device whose executor has
 * `video` set (the default, on a GT with VCS0).
 *
 * The commands are zedBSD's own (uapi/gpu-op.h, GPU_OP_OWN_FIRST onward).
 * The physical-device queries, the sessions and the parameters objects are
 * routed here by the dispatcher; the four recorded commands are recorded by
 * the command buffer part into operations that hold a record made here, and
 * a submission on the video decode queue family is run here.
 */

#ifndef DRIVERS_GPU_I915_RENDER_VIDEO_H
#define DRIVERS_GPU_I915_RENDER_VIDEO_H

#include "internal.h"

#include <stdint.h>

struct i915_gfx_op;

/* The last opcode of the video range: the commands routed here run up to it. */
#define I915_VK_VIDEO_OP_LAST		GPU_OP_CMD_DECODE_VIDEO

/* The last opcode the dispatcher routes here; the ones after it are recordings. */
#define I915_VK_VIDEO_OP_LAST_ROUTED	GPU_OP_DESTROY_VIDEO_SESSION_PARAMETERS

/* One recorded video command; what it holds is private to video.c. */
struct i915_video_command;

int drv_i915_video_dispatch(struct i915_render_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_video_record(struct i915_render_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_video_command **result);
void drv_i915_video_command_free(struct i915_video_command *command);
uint32_t drv_i915_video_submit(struct i915_render_session *session, const struct i915_gfx_op *const *lists, const uint32_t *counts, uint32_t list_count);
void drv_i915_video_objects_release(struct i915_render_session *session);

#endif
