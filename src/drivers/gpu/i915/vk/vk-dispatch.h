/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The opcode router of the Vulkan executor.
 *
 * libvulkan frames every command as a little-endian u32 opcode, a u32
 * reply-request flag, then the encoded parameters (userland/desktop/libvulkan
 * wire.c vulkan_command_begin).  The router reads that header, echoes the
 * opcode into the reply of a command that asks for one, and hands the
 * command to the one part that owns its opcode.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_DISPATCH_H
#define DRIVERS_GPU_I915_VK_VK_DISPATCH_H

#include "vk-internal.h"

int drv_i915_render_dispatch(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

#endif
