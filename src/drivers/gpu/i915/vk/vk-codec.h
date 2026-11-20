/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The wire codec of the Vulkan executor.
 *
 * libvulkan frames every value little endian (userland/desktop/libvulkan
 * wire.c): a byte run is padded to four, a string is its length including
 * the terminator followed by its bytes, and a float travels as its 32 bits.
 * The readers and writers below latch the first failure in their cursor.
 *
 * vk/vk-codec.inc is generated from libvulkan's codec.c by
 * gen_vk_server_codec.py: a decoder for every
 * record the library encodes and an encoder for every record it decodes.
 * A part that needs the record codec includes this header and then that
 * file; every generated function is static and unused ones are dropped.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_CODEC_H
#define DRIVERS_GPU_I915_VK_VK_CODEC_H

#include "vk-internal.h"

#include <libc/vulkan/vulkan_core.h>
#include <libc/vulkan/vulkan_core11.h>
#include <libc/vulkan/vulkan_core12.h>

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

/*
 * The forms of a vkGetPhysicalDeviceProperties2 query: the device's
 * identity (VkPhysicalDeviceIDProperties chained), its Vulkan 1.1
 * properties (the five structures of i915_vkc_reply_core11_properties())
 * or its Vulkan 1.2 properties (VkPhysicalDeviceVulkan12Properties).
 */
#define I915_VKC_PROPERTIES2_IDENTITY	1
#define I915_VKC_PROPERTIES2_CORE11	2
#define I915_VKC_PROPERTIES2_CORE12	3

/*
 * The forms of a vkGetPhysicalDeviceFeatures2 query: the Vulkan 1.1
 * features (the six structures of i915_vkc_reply_core11_features()) or
 * the Vulkan 1.2 features (VkPhysicalDeviceVulkan12Features).
 */
#define I915_VKC_FEATURES2_CORE11	1
#define I915_VKC_FEATURES2_CORE12	2

/*
 * What an executor answers to the Vulkan 1.1 feature and property queries
 * (GPU_OP_GET_PHYSICAL_DEVICE_FEATURES2 and the Vulkan 1.1 form of
 * GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2).
 *
 * One instance describes one physical device; the executor fills it from
 * what its hardware and its compilers really do each time it answers, from
 * i915_vkc_core11_defaults() and then its own values.  libvulkan names
 * Vulkan 1.1 for the device only when the answers meet the version's
 * requirements (multiview, the basic subgroup operations in compute and
 * the 1.1 limits), so an executor that cannot do multiview yet reports it
 * FALSE and its device stays at 1.0.  The sType and pNext of the members
 * are not used.
 */
struct i915_vkc_core11 {
	VkPhysicalDeviceMultiviewFeatures multiview;
	VkPhysicalDevice16BitStorageFeatures storage_16bit;
	VkPhysicalDeviceVariablePointersFeatures variable_pointers;
	VkPhysicalDeviceProtectedMemoryFeatures protected_memory;
	VkPhysicalDeviceSamplerYcbcrConversionFeatures sampler_ycbcr_conversion;
	VkPhysicalDeviceShaderDrawParametersFeatures shader_draw_parameters;
	VkPhysicalDeviceSubgroupProperties subgroup;
	VkPhysicalDevicePointClippingProperties point_clipping;
	VkPhysicalDeviceMultiviewProperties multiview_limits;
	VkPhysicalDeviceProtectedMemoryProperties protected_memory_limits;
	VkPhysicalDeviceMaintenance3Properties maintenance3;
};

/*
 * What an executor answers to the Vulkan 1.2 feature and property queries
 * (the Vulkan 1.2 forms of GPU_OP_GET_PHYSICAL_DEVICE_FEATURES2 and
 * _PROPERTIES2), filled as struct i915_vkc_core11 is, from
 * i915_vkc_core12_defaults() and then the executor's own values.
 * libvulkan names Vulkan 1.2 for the device only when the answers meet the
 * version's requirements; among them a depth and a stencil resolve of
 * sample 0 (supportedDepthResolveModes and supportedStencilResolveModes),
 * which only an executor that does that resolve at the end of a subpass
 * (i915_vkc_read_render_pass2()) reports.  The sType and pNext of the
 * members are not used.
 */
struct i915_vkc_core12 {
	VkPhysicalDeviceVulkan12Features features;
	VkPhysicalDeviceVulkan12Properties properties;
};

void i915_vkc_core11_defaults(struct i915_vkc_core11 *core11);
void i915_vkc_core12_defaults(struct i915_vkc_core12 *core12, const char *driver_name);
int i915_vkc_read_features2(struct i915_wire_reader *reader, int *form);
int i915_vkc_read_properties2(struct i915_wire_reader *reader, int *form);
void i915_vkc_reply_core11_features(struct i915_wire_writer *writer, const struct i915_vkc_core11 *core11);
void i915_vkc_reply_core11_properties(struct i915_wire_writer *writer, const struct i915_vkc_core11 *core11);
void i915_vkc_reply_core12_features(struct i915_wire_writer *writer, const struct i915_vkc_core12 *core12);
void i915_vkc_reply_core12_properties(struct i915_wire_writer *writer, const struct i915_vkc_core12 *core12);
void i915_vkc_read_device_chain(struct i915_wire_reader *reader, struct i915_wire_arena *arena, const void **chain);
void i915_vkc_read_render_pass_chain(struct i915_wire_reader *reader, struct i915_wire_arena *arena, const void **chain);
int i915_vkc_core11_check_device(const VkDeviceCreateInfo *info, const struct i915_vkc_core11 *core11);
int i915_vkc_core12_check_device(const VkDeviceCreateInfo *info, const struct i915_vkc_core12 *core12);
/*
 * The resolve of a subpass's depth and stencil (i915_vkc_depth_resolve()):
 * sample 0 of the multisampled source attachment's depth, stencil or both
 * written into the single-sample target attachment, at the end of the
 * subpass.  target is VK_ATTACHMENT_UNUSED for a subpass without one.
 */
struct i915_vkc_depth_resolve {
	uint32_t source;
	uint32_t target;
	uint32_t depth;
	uint32_t stencil;
};

void i915_vkc_read_render_pass2(struct i915_wire_reader *reader, struct i915_wire_arena *arena, VkRenderPassCreateInfo *info, const VkSubpassDescriptionDepthStencilResolve **resolves);
const VkRenderPassMultiviewCreateInfo *i915_vkc_render_pass_multiview(const VkRenderPassCreateInfo *info);
int i915_vkc_depth_resolve(const VkRenderPassCreateInfo *info, const VkSubpassDescriptionDepthStencilResolve *resolves, uint32_t subpass, struct i915_vkc_depth_resolve *resolve);

#endif
