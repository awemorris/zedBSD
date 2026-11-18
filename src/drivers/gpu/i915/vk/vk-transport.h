/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transport of the Vulkan executor.
 *
 * The submit ioctl returns no reply inline.  A stream opens with
 * vkSetReplyCommandStreamMESA (178), which names a session blob by its
 * resource id; the replies are written there, and the stream ends with
 * vkSeekReplyCommandStreamMESA (179) and vkEnumerateInstanceVersion (137),
 * whose 20-byte reply at a fixed position is the completion trailer
 * libvulkan polls.  A command too long for one submission travels in a
 * session blob and is named by vkExecuteCommandStreamsMESA (180).
 */

#ifndef DRIVERS_GPU_I915_VK_VK_TRANSPORT_H
#define DRIVERS_GPU_I915_VK_VK_TRANSPORT_H

#include "vk-internal.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Values referenced: venus-protocol xmls/VK_MESA_venus_protocol.xml, commit
 * ca19b6358d7cc491bc3e4de76f04c6700876a8fa (the MESA command names, and
 * the arguments of GPU_OP_SET_REPLY_STREAM, GPU_OP_SEEK_REPLY_STREAM and
 * GPU_OP_EXECUTE_STREAMS of uapi/gpu-op.h).
 */
void *drv_i915_render_transport_reply(struct i915_session *session, const void *wire, uint32_t bytes, size_t *capacity);
int drv_i915_render_transport_execute(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int drv_i915_render_transport_dispatch(struct i915_render_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

#endif
