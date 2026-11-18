/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * The zedBSD selection, formatting and additions in this file are Zlib.
 */

/*
 * This file is a derived work of the Khronos Vulkan-Headers
 * (v1.3.269, VK_KHR_wayland_surface).
 * The Vulkan API declarations it contains are:
 * Copyright 2015-2023 The Khronos Group Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * See API-PROVENANCE.md and LICENSE-API for the pinned declaration source.
 */
#ifndef KERN_VULKAN_WAYLAND_H
#define KERN_VULKAN_WAYLAND_H

#include "vulkan_core.h"

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_surface;

#define VK_KHR_wayland_surface 1
#define VK_KHR_WAYLAND_SURFACE_SPEC_VERSION 6
#define VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME "VK_KHR_wayland_surface"

typedef VkFlags VkWaylandSurfaceCreateFlagsKHR;

/* Borrows native Wayland objects for the lifetime of the resulting surface. */
typedef struct VkWaylandSurfaceCreateInfoKHR {
	VkStructureType sType;
	const void *pNext;
	VkWaylandSurfaceCreateFlagsKHR flags;
	struct wl_display *display;
	struct wl_surface *surface;
} VkWaylandSurfaceCreateInfoKHR;

typedef VkResult (VKAPI_PTR *PFN_vkCreateWaylandSurfaceKHR)(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface);
typedef VkBool32 (VKAPI_PTR *PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct wl_display *display);

#ifndef VK_NO_PROTOTYPES
VKAPI_ATTR VkResult VKAPI_CALL vkCreateWaylandSurfaceKHR(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface);
VKAPI_ATTR VkBool32 VKAPI_CALL vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct wl_display *display);
#endif

#ifdef __cplusplus
}
#endif
#endif
