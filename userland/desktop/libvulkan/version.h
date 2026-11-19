/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan version a physical device names: the facts a renderer reports
 * of the later cores, and the rule that turns them into a version with the
 * reasons the next version is not named.  Nothing here talks to a renderer,
 * so a host test asks the rule directly.
 */

#ifndef LIBVULKAN_VERSION_H
#define LIBVULKAN_VERSION_H

#include <stdint.h>
#include <vulkan/vulkan.h>

/* The most reasons the rule gives for one device. */
#define VULKAN_VERSION_REASONS		16U

/*
 * What a renderer reports of the Vulkan 1.1 core, from the 1.1 structures
 * chained to vkGetPhysicalDeviceFeatures2 and vkGetPhysicalDeviceProperties2
 * (the multiview, 16-bit storage, variable pointers, protected memory,
 * sampler YCbCr conversion and shader draw parameters features; the
 * subgroup, point clipping, multiview, protected memory and maintenance 3
 * properties).  reported is VK_FALSE when the renderer did not answer those
 * queries; every other field is then zero.  api_version is the apiVersion
 * of the renderer's VkPhysicalDeviceProperties.
 */
struct vulkan_core11_facts {
	VkBool32 reported;
	uint32_t api_version;
	VkBool32 multiview;
	VkBool32 multiview_geometry_shader;
	VkBool32 multiview_tessellation_shader;
	VkBool32 storage_buffer_16bit;
	VkBool32 uniform_storage_buffer_16bit;
	VkBool32 storage_push_constant_16;
	VkBool32 storage_input_output_16;
	VkBool32 variable_pointers_storage_buffer;
	VkBool32 variable_pointers;
	VkBool32 protected_memory;
	VkBool32 sampler_ycbcr_conversion;
	VkBool32 shader_draw_parameters;
	uint32_t subgroup_size;
	uint32_t subgroup_stages;
	uint32_t subgroup_operations;
	VkBool32 subgroup_quad_all_stages;
	uint32_t point_clipping;
	uint32_t max_multiview_view_count;
	uint32_t max_multiview_instance_index;
	VkBool32 protected_no_fault;
	uint32_t max_per_set_descriptors;
	uint64_t max_memory_allocation_size;
};

uint32_t vulkan_version_decide(uint32_t library, const struct vulkan_core11_facts *facts, const char **reasons, uint32_t capacity, uint32_t *count);

#endif
