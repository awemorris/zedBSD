/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display on Linux and FreeBSD (libkeiland-backend since ws131-p008;
 * from the compositor's os-linux.c and os-freebsd.c): the seat's primary
 * node handed to Vulkan through VK_EXT_acquire_drm_display, and the
 * default socket in the user's runtime directory.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/libkeiland-backend/keiland-backend-display.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The seat's primary node.
 */
int
kl_backend_display_node(
	const struct kl_backend *backend,
	int *descriptor,
	const char **path)
{
	/* The seat's descriptor (-1 while paused) and its fixed path. */
	*descriptor = kl_backend_seat_primary_fd(backend);
	*path = kl_backend_seat_primary_path(backend);

	/* Succeeded: the node is the seat's. */
	return 0;
}

/*
 * Gives Vulkan a duplicate of the seat's primary node for the display.
 */
VkResult
kl_backend_display_acquire(
	struct kl_backend *backend,
	const struct kl_backend_vulkan *vulkan,
	VkPhysicalDevice physical,
	VkDisplayKHR display)
{
	PFN_vkAcquireDrmDisplayEXT acquire;
	VkResult result;
	int descriptor;

	/* Without the primary node's lease there is no display to acquire. */
	descriptor = kl_backend_seat_primary_fd(backend);
	if (descriptor < 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The extension on the compositor's instance. */
	acquire = (PFN_vkAcquireDrmDisplayEXT)vulkan->get_instance_proc_addr(vulkan->instance, "vkAcquireDrmDisplayEXT");
	if (acquire == NULL)
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Vulkan duplicates the file; the seat keeps its original. */
	result = acquire(physical, descriptor, display);
	return result;
}

/*
 * Releases Vulkan's duplicate of the primary node.
 */
VkResult
kl_backend_display_release(
	struct kl_backend *backend,
	const struct kl_backend_vulkan *vulkan,
	VkPhysicalDevice physical,
	VkDisplayKHR display)
{
	PFN_vkReleaseDisplayEXT release;
	VkResult result;

	/* The standard release, after the swapchain has gone. */
	(void)backend;
	release = (PFN_vkReleaseDisplayEXT)vulkan->get_instance_proc_addr(vulkan->instance, "vkReleaseDisplayEXT");
	if (release == NULL)
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	result = release(physical, display);
	return result;
}

/*
 * The default socket: the user's runtime directory, else /tmp.
 */
int
kl_backend_display_socket(
	char *path,
	size_t size)
{
	const char *runtime;
	int length;

	/* The runtime directory, with the development fallback. */
	runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL || runtime[0] == '\0')
		runtime = "/tmp";

	/* A truncated path would bind a different endpoint. */
	length = snprintf(path, size, "%s/wayland-keiland", runtime);
	if (length < 0 || (size_t)length >= size)
		return ENAMETOOLONG;

	/* Succeeded: path names the socket. */
	return 0;
}
