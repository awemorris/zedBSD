/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Select ordinary display modes and swapchain images without native OS APIs.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "display.h"

static VkResult choose_mode(VkPhysicalDevice physical, VkDisplayKHR display, uint32_t width, uint32_t height, VkDisplayModeKHR *mode);
static VkResult choose_plane(VkPhysicalDevice physical, VkDisplayKHR display, VkDisplayModeKHR mode, uint32_t width, uint32_t height, uint32_t *plane, uint32_t *stack);
static VkResult choose_format(VkPhysicalDevice physical, VkSurfaceKHR surface, VkFormat *format);

/*
 * Create a surface using an enumerated display, mode and compatible plane.
 */
VkResult
vkdemo_display_open(
	VkInstance instance,
	VkPhysicalDevice physical,
	uint32_t width,
	uint32_t height,
	struct vkdemo_display *display)
{
	VkDisplayPropertiesKHR *properties;
	VkDisplaySurfaceCreateInfoKHR create;
	VkDisplayModeKHR mode;
	VkResult status;
	uint32_t count;
	uint32_t index;
	uint32_t plane;
	uint32_t stack;

	/* Preserve requested geometry without treating any device as a special case. */
	memset(display, 0, sizeof(*display));
	display->width = width;
	display->height = height;

	/* Size the display enumeration before allocating its result array. */
	count = 0;
	status = vkGetPhysicalDeviceDisplayPropertiesKHR(physical, &count, NULL);
	if (status != VK_SUCCESS)
		return status;

	/* A direct-display invocation requires at least one accessible display. */
	if (count == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Bound multiplication even on the supported 32-bit user ABI. */
	if (sizeof(*properties) > SIZE_MAX / count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Retain the complete advertised list during mode and plane selection. */
	properties = calloc(count, sizeof(*properties));
	if (properties == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* A changed enumeration is reported instead of reading beyond the allocation. */
	status = vkGetPhysicalDeviceDisplayPropertiesKHR(physical, &count, properties);
	if (status != VK_SUCCESS) {
		free(properties);
		return status;
	}

	/* Try each display without inventing a device-specific preferred identifier. */
	status = VK_ERROR_INITIALIZATION_FAILED;
	for (index = 0; index < count; index++) {
		/* This demo draws an unrotated full-plane image. */
		if ((properties[index].supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) == 0)
			continue;

		/* Reuse a matching mode or ask this display to validate the requested size. */
		status = choose_mode(physical, properties[index].display, width, height, &mode);
		if (status != VK_SUCCESS)
			continue;

		/* Require a plane that supports this display, geometry and opaque pixels. */
		status = choose_plane(physical, properties[index].display, mode, width, height, &plane, &stack);
		if (status != VK_SUCCESS)
			continue;

		/* Initialize the one-to-one, opaque direct-display surface. */
		memset(&create, 0, sizeof(create));
		create.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
		create.displayMode = mode;
		create.planeIndex = plane;
		create.planeStackIndex = stack;
		create.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
		create.globalAlpha = 1.0f;
		create.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
		create.imageExtent.width = width;
		create.imageExtent.height = height;

		/* Publish only a surface accepted by the selected Vulkan implementation. */
		status = vkCreateDisplayPlaneSurfaceKHR(instance, &create, NULL, &display->surface);
		if (status == VK_SUCCESS)
			break;
	}

	/* Mode and plane handles remain owned by Vulkan after enumeration storage ends. */
	free(properties);
	if (status != VK_SUCCESS)
		return status;

	/* Refuse an all-incompatible list that never attempted surface creation. */
	if (display->surface == VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Succeeded: the renderer owns one direct-display surface. */
	return VK_SUCCESS;
}

/*
 * Create readable color attachments from the surface's supported FIFO contract.
 */
VkResult
vkdemo_display_create_swapchain(
	VkPhysicalDevice physical,
	VkDevice device,
	uint32_t family,
	struct vkdemo_display *display)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSwapchainCreateInfoKHR create;
	VkBool32 supported;
	VkImageUsageFlags usage;
	VkResult status;
	uint32_t count;

	/* Require graphics and presentation on the queue already chosen by the app. */
	supported = VK_FALSE;
	status = vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, display->surface, &supported);
	if (status != VK_SUCCESS)
		return status;

	/* Another queue family cannot be used without explicit ownership transfers. */
	if (supported == VK_FALSE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Query concrete extent, usage and image-count constraints before allocation. */
	status = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, display->surface, &capabilities);
	if (status != VK_SUCCESS)
		return status;

	/* The image is rendered directly and copied only for independent readback. */
	usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if ((capabilities.supportedUsageFlags & usage) != usage)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Keep the selected mode's pixel geometry unchanged. */
	if (display->width < capabilities.minImageExtent.width)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Reject width outside the advertised surface range. */
	if (display->width > capabilities.maxImageExtent.width)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Reject height below the advertised surface range. */
	if (display->height < capabilities.minImageExtent.height)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Reject height above the advertised surface range. */
	if (display->height > capabilities.maxImageExtent.height)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A fixed surface extent must match the mode selected by this application. */
	if (capabilities.currentExtent.width != UINT32_MAX) {
		/* The surface cannot silently rescale the independently verified image. */
		if (capabilities.currentExtent.width != display->width)
			return VK_ERROR_INITIALIZATION_FAILED;

		/* Both components of a fixed extent belong to the same mode. */
		if (capabilities.currentExtent.height != display->height)
			return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* This renderer does not introduce an image transform. */
	if ((capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The fragment shader and background both produce opaque pixels. */
	if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Prefer RGBA readback while accepting the standard BGRA channel layout. */
	status = choose_format(physical, display->surface, &display->format);
	if (status != VK_SUCCESS)
		return status;

	/* Request one spare image when the surface's count limits permit it. */
	count = capabilities.minImageCount;
	if (count < UINT32_MAX)
		count++;

	/* A nonzero maximum is a real upper bound, rather than an optional hint. */
	if (capabilities.maxImageCount != 0) {
		/* Retain the supported minimum when no extra image is available. */
		if (count > capabilities.maxImageCount)
			count = capabilities.maxImageCount;
	}

	/* FIFO is the mandatory Vulkan surface mode, with one color layer. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.surface = display->surface;
	create.minImageCount = count;
	create.imageFormat = display->format;
	create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	create.imageExtent.width = display->width;
	create.imageExtent.height = display->height;
	create.imageArrayLayers = 1;
	create.imageUsage = usage;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	create.clipped = VK_TRUE;
	status = vkCreateSwapchainKHR(device, &create, NULL, &display->swapchain);
	if (status != VK_SUCCESS)
		return status;

	/* Enumerate the actual images, which may exceed the requested minimum. */
	count = 0;
	status = vkGetSwapchainImagesKHR(device, display->swapchain, &count, NULL);
	if (status != VK_SUCCESS)
		return status;

	/* A successful swapchain cannot be used without an image to acquire. */
	if (count == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Preserve the host allocation size on both pointer-width ABIs. */
	if (sizeof(*display->images) > SIZE_MAX / count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The image handles are borrowed until swapchain destruction. */
	display->images = calloc(count, sizeof(*display->images));
	if (display->images == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Populate the caller-owned image array without manufacturing image handles. */
	status = vkGetSwapchainImagesKHR(device, display->swapchain, &count, display->images);
	if (status != VK_SUCCESS)
		return status;

	/* Publish the count only after enumeration populated the array successfully. */
	display->image_count = count;

	/* Succeeded: each borrowed image supports rendering and GPU readback. */
	return VK_SUCCESS;
}

/*
 * Release a completed swapchain and its surface after device work is idle.
 */
void
vkdemo_display_close(
	VkInstance instance,
	VkDevice device,
	struct vkdemo_display *display)
{
	/* Borrowed image handles expire with their owning swapchain. */
	free(display->images);
	display->images = NULL;
	display->image_count = 0;

	/* Partial surface initialization may not have created a logical device. */
	if (device != VK_NULL_HANDLE) {
		/* Destroy a successfully created swapchain exactly once. */
		if (display->swapchain != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(device, display->swapchain, NULL);
	}

	/* Surface ownership belongs to the instance even if device setup failed. */
	if (display->surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(instance, display->surface, NULL);

	/* Prevent a repeated close from touching consumed Vulkan handles. */
	display->swapchain = VK_NULL_HANDLE;
	display->surface = VK_NULL_HANDLE;

	/* Succeeded: no surface, swapchain or image-array allocation remains. */
	return;
}

/* Choose an existing matching mode or ask the display to validate that geometry. */
static VkResult
choose_mode(
	VkPhysicalDevice physical,
	VkDisplayKHR display,
	uint32_t width,
	uint32_t height,
	VkDisplayModeKHR *mode)
{
	VkDisplayModePropertiesKHR *properties;
	VkDisplayModeCreateInfoKHR create;
	VkResult status;
	uint32_t count;
	uint32_t index;
	uint32_t refresh;

	/* Enumerate this display's modes before choosing a refresh rate. */
	count = 0;
	status = vkGetDisplayModePropertiesKHR(physical, display, &count, NULL);
	if (status != VK_SUCCESS)
		return status;

	/* Do not invent a refresh rate when this display exposes no mode. */
	if (count == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Validate the mode-array allocation for both supported pointer widths. */
	if (sizeof(*properties) > SIZE_MAX / count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Retain mode parameters until matching or creating the desired mode. */
	properties = calloc(count, sizeof(*properties));
	if (properties == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* A changing mode list cannot be accepted as a complete array. */
	status = vkGetDisplayModePropertiesKHR(physical, display, &count, properties);
	if (status != VK_SUCCESS) {
		free(properties);
		return status;
	}

	/* Prefer a mode already accepted by the display implementation. */
	*mode = VK_NULL_HANDLE;
	refresh = 0;
	for (index = 0; index < count; index++) {
		/* Retain an advertised refresh for a possible geometry request. */
		if (refresh == 0)
			refresh = properties[index].parameters.refreshRate;

		/* Skip modes with a different pixel width. */
		if (properties[index].parameters.visibleRegion.width != width)
			continue;

		/* Skip modes with a different pixel height. */
		if (properties[index].parameters.visibleRegion.height != height)
			continue;

		/* This matching mode needs no device-specific creation path. */
		*mode = properties[index].displayMode;
		break;
	}

	/* Vulkan owns the mode handles independently of this enumeration array. */
	free(properties);
	if (*mode != VK_NULL_HANDLE)
		return VK_SUCCESS;

	/* Zero refresh is not a usable basis for requesting another mode. */
	if (refresh == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Ask the standard display API to accept the exact desired geometry. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR;
	create.parameters.visibleRegion.width = width;
	create.parameters.visibleRegion.height = height;
	create.parameters.refreshRate = refresh;
	status = vkCreateDisplayModeKHR(physical, display, &create, NULL, mode);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the chosen display accepted the requested pixel mode. */
	return VK_SUCCESS;
}

/* Find an opaque plane whose source and destination ranges cover the full image. */
static VkResult
choose_plane(
	VkPhysicalDevice physical,
	VkDisplayKHR display,
	VkDisplayModeKHR mode,
	uint32_t width,
	uint32_t height,
	uint32_t *plane,
	uint32_t *stack)
{
	VkDisplayPlanePropertiesKHR *properties;
	VkDisplayPlaneCapabilitiesKHR capabilities;
	VkDisplayKHR *supported;
	VkResult status;
	uint32_t count;
	uint32_t index;
	uint32_t display_count;
	uint32_t at;
	int matches;
	int found;

	/* Query every plane because array index is its standard Vulkan identity. */
	count = 0;
	status = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical, &count, NULL);
	if (status != VK_SUCCESS)
		return status;

	/* A display without a usable plane cannot expose the requested surface. */
	if (count == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Bound storage before preserving the complete plane list. */
	if (sizeof(*properties) > SIZE_MAX / count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Keep plane stack indices alive while checking compatible displays. */
	properties = calloc(count, sizeof(*properties));
	if (properties == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Populate only the allocated plane-list capacity. */
	status = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical, &count, properties);
	if (status != VK_SUCCESS) {
		free(properties);
		return status;
	}

	/* Try plane compatibility before requesting a full-frame surface. */
	status = VK_ERROR_INITIALIZATION_FAILED;
	found = 0;
	for (index = 0; index < count; index++) {
		/* Each plane explicitly lists the displays it can drive. */
		display_count = 0;
		status = vkGetDisplayPlaneSupportedDisplaysKHR(physical, index, &display_count, NULL);
		if (status != VK_SUCCESS)
			break;

		/* An empty list cannot contain this display. */
		if (display_count == 0)
			continue;

		/* Keep allocation geometry valid on a 32-bit host. */
		if (sizeof(*supported) > SIZE_MAX / display_count) {
			status = VK_ERROR_OUT_OF_HOST_MEMORY;
			break;
		}

		/* Retain supported display identities only for this plane. */
		supported = calloc(display_count, sizeof(*supported));
		if (supported == NULL) {
			status = VK_ERROR_OUT_OF_HOST_MEMORY;
			break;
		}

		/* Resolve compatibility without assuming that plane zero is special. */
		status = vkGetDisplayPlaneSupportedDisplaysKHR(physical, index, &display_count, supported);
		if (status != VK_SUCCESS) {
			free(supported);
			break;
		}

		/* Find this display in the plane's complete compatible list. */
		matches = 0;
		for (at = 0; at < display_count; at++) {
			/* Standard Vulkan handles are compared only for equality. */
			if (supported[at] == display)
				matches = 1;
		}

		/* The compatible list is no longer needed after identity comparison. */
		free(supported);
		if (matches == 0)
			continue;

		/* Query the mode-specific source and destination geometry restrictions. */
		status = vkGetDisplayPlaneCapabilitiesKHR(physical, mode, index, &capabilities);
		if (status != VK_SUCCESS)
			break;

		/* The scene has no per-plane alpha blending requirement. */
		if ((capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR) == 0)
			continue;

		/* Require the full source rectangle at origin zero. */
		if (capabilities.minSrcPosition.x > 0 || capabilities.maxSrcPosition.x < 0)
			continue;

		/* Require source origin zero on the vertical axis. */
		if (capabilities.minSrcPosition.y > 0 || capabilities.maxSrcPosition.y < 0)
			continue;

		/* Require destination origin zero on the horizontal axis. */
		if (capabilities.minDstPosition.x > 0 || capabilities.maxDstPosition.x < 0)
			continue;

		/* Require destination origin zero on the vertical axis. */
		if (capabilities.minDstPosition.y > 0 || capabilities.maxDstPosition.y < 0)
			continue;

		/* Do not scale the source image or truncate it to another extent. */
		if (width < capabilities.minSrcExtent.width || width > capabilities.maxSrcExtent.width)
			continue;

		/* Preserve the complete source height. */
		if (height < capabilities.minSrcExtent.height || height > capabilities.maxSrcExtent.height)
			continue;

		/* Require matching destination width for one-to-one presentation. */
		if (width < capabilities.minDstExtent.width || width > capabilities.maxDstExtent.width)
			continue;

		/* Require matching destination height for one-to-one presentation. */
		if (height < capabilities.minDstExtent.height || height > capabilities.maxDstExtent.height)
			continue;

		/* Publish the compatible plane with its existing stack position. */
		*plane = index;
		*stack = properties[index].currentStackIndex;
		found = 1;
		break;
	}

	/* Preserve a real enumeration failure while releasing the plane list. */
	free(properties);
	if (status != VK_SUCCESS)
		return status;

	/* No plane met every geometry and ownership prerequisite. */
	if (found == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Succeeded: expose one compatible plane and its current stack position. */
	return VK_SUCCESS;
}

/* Select a standard UNORM format whose readback has an explicit channel order. */
static VkResult
choose_format(
	VkPhysicalDevice physical,
	VkSurfaceKHR surface,
	VkFormat *format)
{
	VkSurfaceFormatKHR *formats;
	VkResult status;
	uint32_t count;
	uint32_t index;

	/* Size this surface's supported format list. */
	count = 0;
	status = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, NULL);
	if (status != VK_SUCCESS)
		return status;

	/* A surface without a format cannot be used for rendering. */
	if (count == 0)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Bound multiplication before allocating the complete format array. */
	if (sizeof(*formats) > SIZE_MAX / count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Retain explicit format and color-space pairs during selection. */
	formats = calloc(count, sizeof(*formats));
	if (formats == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Reject an incomplete result rather than choosing from unknown entries. */
	status = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats);
	if (status != VK_SUCCESS) {
		free(formats);
		return status;
	}

	/* Prefer RGBA, with BGRA as an equivalent UNORM readback representation. */
	*format = VK_FORMAT_UNDEFINED;
	for (index = 0; index < count; index++) {
		/* The demo does not perform HDR or color-space conversion. */
		if (formats[index].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			continue;

		/* UNDEFINED means this older surface lets the application choose. */
		if (formats[index].format == VK_FORMAT_UNDEFINED) {
			*format = VK_FORMAT_R8G8B8A8_UNORM;
			break;
		}

		/* RGBA matches the existing independently checked image contract directly. */
		if (formats[index].format == VK_FORMAT_R8G8B8A8_UNORM) {
			*format = formats[index].format;
			break;
		}

		/* Retain BGRA while continuing to look for the preferred RGBA pair. */
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM)
			*format = formats[index].format;
	}

	/* The selected scalar remains valid after its enumeration storage is freed. */
	free(formats);
	if (*format == VK_FORMAT_UNDEFINED)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Succeeded: the renderer can decode every color component without guessing. */
	return VK_SUCCESS;
}
