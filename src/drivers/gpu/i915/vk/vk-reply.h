/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The replies every object command of the graphics path shares.
 *
 * A generic create is framed as
 *   [device][present][create info][pAllocator = 0][present][identity]
 *   -> [result][present][identity]
 * and a generic destroy as
 *   [device][identity][pAllocator = 0] -> (the echoed opcode alone).
 * The helpers here read the common tail of a create, publish the object it
 * made, and turn an errno into the VkResult the reply carries.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_REPLY_H
#define DRIVERS_GPU_I915_VK_VK_REPLY_H

#include "vk-object.h"

#include <stdint.h>

struct i915_render_session;
struct i915_wire_reader;
struct i915_wire_writer;

uint32_t drv_i915_gfx_result(int error);
uint64_t drv_i915_gfx_create_tail(struct i915_wire_reader *reader);
void drv_i915_gfx_create_reply(struct i915_render_session *session, struct i915_wire_writer *reply, enum i915_vk_object_kind kind, uint64_t identity, void *object, int error);

#endif
