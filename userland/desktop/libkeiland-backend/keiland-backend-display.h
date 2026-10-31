/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display of libkeiland-backend (ws131-p008): the primary display node
 * and the Vulkan display's acquisition and release.
 *
 * The compositor composes with Vulkan into a VK_KHR_display swapchain.  On
 * Linux and FreeBSD the display belongs to the seat's primary node (the DRM
 * master file), which Vulkan is given through VK_EXT_acquire_drm_display;
 * on zedBSD libvulkan reaches the display itself and there is nothing to
 * hand over.  The backend uses only the Vulkan instance and the procedure
 * lookup the compositor passes; it makes no Vulkan object of its own.
 */

#ifndef KL_BACKEND_DISPLAY_H
#define KL_BACKEND_DISPLAY_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct kl_backend;

/*
 * The compositor's Vulkan, as far as the display needs it: its instance
 * and the procedure lookup that resolves the extensions on it.
 */
struct kl_backend_vulkan {
	VkInstance instance;
	PFN_vkGetInstanceProcAddr get_instance_proc_addr;
};

/*
 * The primary display node the seat holds: its descriptor (-1 while
 * paused) and its path, which the compositor's Vulkan inquiry must open
 * too.  Returns 0, or ENOTSUP where the display has no node to share
 * (zedBSD).
 */
int kl_backend_display_node(const struct kl_backend *backend, int *descriptor, const char **path);

/*
 * Lets Vulkan acquire the display chosen on physical, with its own
 * duplicate of the primary node (the seat keeps the original).  Returns
 * VK_SUCCESS or the failure (VK_ERROR_INITIALIZATION_FAILED without a node,
 * VK_ERROR_EXTENSION_NOT_PRESENT without the extension).
 */
VkResult kl_backend_display_acquire(struct kl_backend *backend, const struct kl_backend_vulkan *vulkan, VkPhysicalDevice physical, VkDisplayKHR display);

/*
 * Releases Vulkan's duplicate after the swapchain has gone.  Returns
 * VK_SUCCESS or the failure of the release.
 */
VkResult kl_backend_display_release(struct kl_backend *backend, const struct kl_backend_vulkan *vulkan, VkPhysicalDevice physical, VkDisplayKHR display);

/*
 * Fills path with the compositor's default socket for this system (the
 * user's runtime directory, else /tmp, then wayland-keiland).  Returns 0,
 * ENAMETOOLONG, or ENOTSUP where the compositor keeps its own default
 * (zedBSD).
 */
int kl_backend_display_socket(char *path, size_t size);

#endif
