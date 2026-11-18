/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Keeps cursor-free output pixels in a standard Vulkan transfer buffer. */
#ifndef KWL_SCENE_H
#define KWL_SCENE_H

#include <vulkan/vulkan.h>

#include "draw-list.h"

/* One output owns its cache until idle teardown; only a successful frame makes it valid. */
struct kwl_scene {
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint32_t width;
	uint32_t height;
	unsigned valid;
	unsigned changed;
	unsigned written;
};

VkResult kwl_scene_open(struct kwl_scene *scene, VkPhysicalDevice physical, VkDevice device, uint32_t width, uint32_t height);
void kwl_scene_close(struct kwl_scene *scene, VkDevice device);
void kwl_scene_copy(struct kwl_scene *scene, struct kwl_list *command, VkImage image, VkImageLayout layout, const VkRect2D *regions, unsigned count, unsigned restore);

#endif
