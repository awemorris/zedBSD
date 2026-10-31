/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Select a direct display through portable Vulkan display and surface APIs.
 */

#ifndef VKDEMO_DISPLAY_H
#define VKDEMO_DISPLAY_H

#include <vulkan/vulkan.h>

/* One renderer owns this surface and swapchain until its device becomes idle. */
struct vkdemo_display {
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkImage *images;
	uint32_t image_count;
	VkFormat format;
	uint32_t width;
	uint32_t height;
	/* A record the swapchain's creation chains (pNext), NULL for none; the caller's, set after the surface opened. */
	const void *next;
};

VkResult vkdemo_display_open(VkInstance instance, VkPhysicalDevice physical, uint32_t width, uint32_t height, struct vkdemo_display *display);
VkResult vkdemo_display_open_on(VkInstance instance, VkPhysicalDevice physical, VkDisplayKHR target, uint32_t width, uint32_t height, struct vkdemo_display *display);
VkResult vkdemo_display_choose_format(VkPhysicalDevice physical, struct vkdemo_display *display);
VkResult vkdemo_display_create_swapchain(VkPhysicalDevice physical, VkDevice device, uint32_t family, struct vkdemo_display *display, int readback);
VkResult vkdemo_display_create_swapchain_usage(VkPhysicalDevice physical, VkDevice device, uint32_t family, struct vkdemo_display *display, VkImageUsageFlags extra);
VkResult vkdemo_display_create_swapchain_exact(VkPhysicalDevice physical, VkDevice device, uint32_t family, struct vkdemo_display *display, VkImageUsageFlags usage);
void vkdemo_display_close(VkInstance instance, VkDevice device, struct vkdemo_display *display);

#endif
