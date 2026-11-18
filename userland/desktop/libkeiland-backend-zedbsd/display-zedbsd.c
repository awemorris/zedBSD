/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display on zedBSD (libkeiland-backend since ws131-p008; from the
 * compositor's os-zedbsd.c).
 *
 * sessiond hands the display over before the compositor starts, and
 * libvulkan reaches the display itself: there is no node to share, nothing
 * to acquire or release, and the compositor keeps its own default socket.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend-display.h"
#include "include/libc/vulkan/vulkan_zedbsd.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Has no node to share.
 */
int
kl_backend_display_node(
	const struct kl_backend *backend,
	int *descriptor,
	const char **path)
{
	/* libvulkan opens the display itself. */
	(void)backend;
	*descriptor = -1;
	*path = "";
	return ENOTSUP;
}

/*
 * Has nothing to hand over: libvulkan reaches the display.
 */
VkResult
kl_backend_display_acquire(
	struct kl_backend *backend,
	const struct kl_backend_vulkan *vulkan,
	VkPhysicalDevice physical,
	VkDisplayKHR display)
{
	/* Succeeded: the display needs no OS descriptor. */
	(void)backend;
	(void)vulkan;
	(void)physical;
	(void)display;
	return VK_SUCCESS;
}

/*
 * Has nothing to release: libvulkan releases the display with the
 * swapchain.
 */
VkResult
kl_backend_display_release(
	struct kl_backend *backend,
	const struct kl_backend_vulkan *vulkan,
	VkPhysicalDevice physical,
	VkDisplayKHR display)
{
	/* Succeeded: nothing was acquired here. */
	(void)backend;
	(void)vulkan;
	(void)physical;
	(void)display;
	return VK_SUCCESS;
}

/*
 * Keeps the compositor's own default socket.
 */
int
kl_backend_display_socket(
	char *path,
	size_t size)
{
	/* The compositor's default stays. */
	(void)path;
	(void)size;
	return ENOTSUP;
}

/*
 * Copies a display's EDID from libvulkan's VK_ZEDBSD_display_edid,
 * chained to the display's record of
 * vkGetPhysicalDeviceDisplayProperties2KHR (the compositor enabled both
 * extensions, kl_backend_gpu_instance_extensions).
 */
int
kl_backend_display_edid(
	struct kl_backend *backend,
	const struct kl_backend_vulkan *vulkan,
	VkPhysicalDevice physical,
	VkDisplayKHR display,
	uint8_t *edid,
	size_t size,
	size_t *length)
{
	PFN_vkGetPhysicalDeviceDisplayProperties2KHR properties2;
	VkDisplayProperties2KHR *records;
	VkDisplayEdidPropertiesZEDBSD *edids;
	VkResult result;
	uint32_t count;
	uint32_t index;
	size_t copied;

	/* Nothing copied yet. */
	(void)backend;
	*length = 0U;

	/* The extensible display query. */
	properties2 = (PFN_vkGetPhysicalDeviceDisplayProperties2KHR)vulkan->get_instance_proc_addr(vulkan->instance,
	    "vkGetPhysicalDeviceDisplayProperties2KHR");
	if (properties2 == NULL)
		return ENOTSUP;

	/* How many displays there are. */
	count = 0U;
	result = properties2(physical, &count, NULL);
	if (result != VK_SUCCESS || count == 0U)
		return ENODEV;

	/* A record and an EDID for each. */
	records = calloc(count, sizeof(*records));
	if (records == NULL)
		return ENOMEM;
	edids = calloc(count, sizeof(*edids));
	if (edids == NULL) {
		free(records);
		return ENOMEM;
	}
	for (index = 0U; index < count; index++) {
		records[index].sType = VK_STRUCTURE_TYPE_DISPLAY_PROPERTIES_2_KHR;
		records[index].pNext = &edids[index];
		edids[index].sType = VK_STRUCTURE_TYPE_DISPLAY_EDID_PROPERTIES_ZEDBSD;
	}

	/* The displays, each with its EDID. */
	result = properties2(physical, &count, records);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
		free(edids);
		free(records);
		return EIO;
	}

	/* The display asked for (handles compare only for equality). */
	for (index = 0U; index < count; index++) {
		if (records[index].displayProperties.display == display)
			break;
	}
	if (index == count) {
		free(edids);
		free(records);
		return ENODEV;
	}

	/* Its whole blocks the caller has room for. */
	copied = edids[index].edidSize;
	if (copied > sizeof(edids[index].edid))
		copied = sizeof(edids[index].edid);
	if (copied > size)
		copied = size - size % 128U;
	memcpy(edid, edids[index].edid, copied);
	*length = copied;
	free(edids);
	free(records);

	/* Succeeded: the display's EDID, or none. */
	return 0;
}
