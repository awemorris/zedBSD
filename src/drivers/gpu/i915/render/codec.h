/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The wire codec of the Vulkan executor.
 *
 * libvulkan frames every value little endian (userland/base/libvulkan
 * wire.c): a byte run is padded to four, a string is its length including
 * the terminator followed by its bytes, and a float travels as its 32 bits.
 * The readers and writers below latch the first failure in their cursor.
 *
 * render/vulkan-codec.inc is generated from libvulkan's codec.c by
 * plan/ws031/handover/tools/gen_vk_server_codec.py: a decoder for every
 * record the library encodes and an encoder for every record it decodes.
 * A part that needs the record codec includes this header and then that
 * file; every generated function is static and unused ones are dropped.
 */

#ifndef DRIVERS_GPU_I915_RENDER_CODEC_H
#define DRIVERS_GPU_I915_RENDER_CODEC_H

#include "internal.h"

#include <libc/vulkan/vulkan_core.h>

#include <stddef.h>
#include <stdint.h>
#include <kern/kcrt.h>

uint32_t drv_i915_wire_read_u32(struct i915_wire_reader *reader);
uint64_t drv_i915_wire_read_u64(struct i915_wire_reader *reader);
i915_vk_handle drv_i915_wire_read_handle(struct i915_wire_reader *reader);
const void *drv_i915_wire_read_array(struct i915_wire_reader *reader, size_t count, size_t element);
void drv_i915_wire_reply_u32(struct i915_wire_writer *writer, uint32_t value);
void drv_i915_wire_reply_u64(struct i915_wire_writer *writer, uint64_t value);
void drv_i915_wire_reply_bytes(struct i915_wire_writer *writer, const void *data, size_t bytes);

/* The primitives the generated record codec stands on. */
void *i915_vkc_array(struct i915_wire_reader *reader, struct i915_wire_arena *arena, uint64_t count, size_t element);
const char *i915_vkc_read_string(struct i915_wire_reader *reader, struct i915_wire_arena *arena);
void i915_vkc_read_bytes(struct i915_wire_reader *reader, void *destination, size_t bytes);
void i915_vkc_read_float(struct i915_wire_reader *reader, float *destination);
void i915_vkc_skip_external_chain(struct i915_wire_reader *reader);
void i915_vkc_reply_bytes(struct i915_wire_writer *writer, const void *source, size_t bytes);
void i915_vkc_reply_float(struct i915_wire_writer *writer, const float *source);

#endif
