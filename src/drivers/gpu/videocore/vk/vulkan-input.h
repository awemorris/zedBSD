/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Immutable shader, sampler and colour-view inputs retain every logical parent through final prepared-job retirement. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_INPUT_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_INPUT_H

#include "drivers/gpu/videocore/vk/vulkan-private.h"

/* Every ordinary typed input owns its device and optional image dependency independently of public identities. */
struct bcm2711_vulkan_input_owner {
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *parent;
};

/*
 * One 2D image view keeps its underlying image, format and native component selection immutable.
 *
 * plane is zero for a view of the whole image, or names the plane of a
 * two-plane NV12 picture the view reads: 1 its luma as R8, 2 its chroma as
 * RG8.  stencil is set for a view of a D24S8 image's stencil aspect alone,
 * which a shader samples as an unsigned integer in red.
 */
struct bcm2711_vulkan_image_view {
	struct bcm2711_vulkan_input_owner owner;
	VkFormat format;
	VkComponentMapping components;
	uint32_t plane;
	bool stencil;
	/* The view's first mip level, and the levels it spans from there (one at least). */
	uint32_t base_level;
	uint32_t levels;
	/*
	 * The view's type (2D, 2D array, cube or 3D), its first array layer and
	 * the layers it spans from there (one at least; six for a cube; one for
	 * a 3D view, which reads every slice of its level).
	 */
	VkImageViewType type;
	uint32_t base_layer;
	uint32_t layers;
};

/* One normalized sampler supplies exact filtering, wraps, levels of detail, border and depth compare at draw preparation. */
struct bcm2711_vulkan_sampler {
	struct bcm2711_vulkan_input_owner owner;
	VkFilter mag;
	VkFilter min;
	VkSamplerAddressMode u;
	VkSamplerAddressMode v;
	VkSamplerAddressMode w;
	/* VK_SAMPLER_MIPMAP_MODE_LINEAR, and maxLod and minLod in 4.8 fixed point (no more than 15.996). */
	uint32_t mip_linear;
	uint32_t max_lod;
	uint32_t min_lod;
	/* mipLodBias in signed 8.8 fixed point (sixteen bits, two's complement). */
	uint32_t bias;
	VkBorderColor border;
	/* The depth compare: VK_TRUE and its operation, or VK_FALSE. */
	VkBool32 compare;
	VkCompareOp compare_op;
};

/* One copied SPIR-V module remains independent of the per-command arena until its last compiled-pipeline owner retires. */
struct bcm2711_vulkan_module {
	struct bcm2711_vulkan_input_owner owner;
	uint32_t word_count;
	uint32_t words[1];
};

int bcm2711_vulkan_input_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

#endif
