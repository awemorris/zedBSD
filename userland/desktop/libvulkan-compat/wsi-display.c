/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Vulkan display and primary-plane handles backed by our KMS inquiry and master ownership. */
#include "compat.h"
#include <stdlib.h>
#include <string.h>

static uint32_t display_refresh(const struct drm_mode_modeinfo *mode);
static VkResult display_list(uint32_t *count, VkDisplayKHR *output);

/*
 * Enumerates connected KMS outputs independently of the backend rendering physical device.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceDisplayPropertiesKHR(
	VkPhysicalDevice physicalDevice,
	uint32_t *pPropertyCount,
	VkDisplayPropertiesKHR *pProperties)
{
	struct compat_display *displays;
	struct compat_display *display;
	uint32_t count;
	uint32_t capacity;
	uint32_t written;
	VkResult error;

	/* All rendering devices use the same portable copy-to-KMS display path. */
	(void)physicalDevice;
	if (pPropertyCount == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Inquiry opens no host card when KEILAND_DRM_DEVICE=none is set. */
	error = compat_kms_displays(&displays, &count);
	if (error != VK_SUCCESS)
		return error;

	/* Measurement never reads the caller's uninitialized count input. */
	if (pProperties == NULL) {
		*pPropertyCount = count;
		return VK_SUCCESS;
	}

	/* Each stable display handle refers to one complete connector/mode record. */
	capacity = *pPropertyCount;
	written = 0;
	for (display = displays; display != NULL; display = display->next) {
		/* Copies only the caller's available enumeration slots. */
		if (written >= capacity)
			break;

		/* Preferred physical resolution is the kernel mode selected for compositor startup. */
		pProperties[written].display = (VkDisplayKHR)(uintptr_t)display;
		pProperties[written].displayName = display->name;
		pProperties[written].physicalDimensions.width = display->width_mm;
		pProperties[written].physicalDimensions.height = display->height_mm;
		pProperties[written].physicalResolution.width = display->preferred->mode.hdisplay;
		pProperties[written].physicalResolution.height = display->preferred->mode.vdisplay;
		pProperties[written].supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
		pProperties[written].planeReorderPossible = VK_FALSE;
		pProperties[written].persistentContent = VK_FALSE;
		written++;
	}

	/* Returns the count actually copied for both complete and incomplete enumerations. */
	*pPropertyCount = written;
	if (written < count)
		return VK_INCOMPLETE;

	/* Succeeded: every connected output fits in the supplied array. */
	return VK_SUCCESS;
}

/*
 * Exposes one primary display plane when at least one connected output exists.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceDisplayPlanePropertiesKHR(
	VkPhysicalDevice physicalDevice,
	uint32_t *pPropertyCount,
	VkDisplayPlanePropertiesKHR *pProperties)
{
	struct compat_display *displays;
	uint32_t count;
	VkResult error;

	/* The portable display copy path needs exactly one primary-plane abstraction. */
	(void)physicalDevice;
	if (pPropertyCount == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A headless inquiry supplies no primary display plane. */
	error = compat_kms_displays(&displays, &count);
	if (error != VK_SUCCESS)
		return error;

	/* Measurement reports a single plane only for a connected display card. */
	if (pProperties == NULL) {
		*pPropertyCount = 0;
		if (count != 0)
			*pPropertyCount = 1;
		return VK_SUCCESS;
	}

	/* Empty display inquiry has a successful empty plane enumeration. */
	if (count == 0) {
		*pPropertyCount = 0;
		return VK_SUCCESS;
	}

	/* No result can be copied into a zero-capacity output. */
	if (*pPropertyCount == 0)
		return VK_INCOMPLETE;

	/* The one primary plane uses stack index zero and no preselected Vulkan display. */
	pProperties[0].currentDisplay = VK_NULL_HANDLE;
	pProperties[0].currentStackIndex = 0;
	*pPropertyCount = 1;

	/* Succeeded: callers may select any enumerated connector on the primary plane. */
	return VK_SUCCESS;
}

/*
 * Enumerates connectors usable by the one primary plane.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetDisplayPlaneSupportedDisplaysKHR(
	VkPhysicalDevice physicalDevice,
	uint32_t planeIndex,
	uint32_t *pDisplayCount,
	VkDisplayKHR *pDisplays)
{
	VkResult submission;

	/* The display backend implements only primary plane index zero. */
	(void)physicalDevice;
	if (planeIndex != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Returns the shared stable connected-display enumeration. */
	submission = display_list(pDisplayCount, pDisplays);
	if (submission != VK_SUCCESS)
		return submission;

	/* Succeeded: the caller received the connected display handles. */
	return VK_SUCCESS;
}

/*
 * Enumerates stable connector timing handles with refresh rates expressed in millihertz.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetDisplayModePropertiesKHR(
	VkPhysicalDevice physicalDevice,
	VkDisplayKHR handle,
	uint32_t *pPropertyCount,
	VkDisplayModePropertiesKHR *pProperties)
{
	struct compat_display *display;
	struct compat_display_mode *mode;
	uint32_t count;
	uint32_t capacity;
	uint32_t written;

	/* Each display handle owns its process-lifetime mode list. */
	(void)physicalDevice;
	display = (struct compat_display *)(uintptr_t)handle;
	if (display == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires an enumeration destination before measuring modes. */
	if (pPropertyCount == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Counts every timing without depending on a caller-owned array lifetime. */
	count = 0;
	for (mode = display->modes; mode != NULL; mode = mode->next) {
		/* Each record describes exactly one connector timing. */
		count++;
	}

	/* Measurement reports the total without reading the count input. */
	if (pProperties == NULL) {
		*pPropertyCount = count;
		return VK_SUCCESS;
	}

	/* Copies mode identities and their exact visible-region/refresh contract. */
	capacity = *pPropertyCount;
	written = 0;
	for (mode = display->modes; mode != NULL; mode = mode->next) {
		/* Stops at the caller's capacity without discarding the remaining mode records. */
		if (written >= capacity)
			break;

		/* A backend image's display mode is our KMS timing, not a backend WSI handle. */
		pProperties[written].displayMode = (VkDisplayModeKHR)(uintptr_t)mode;
		pProperties[written].parameters.visibleRegion.width = mode->mode.hdisplay;
		pProperties[written].parameters.visibleRegion.height = mode->mode.vdisplay;
		pProperties[written].parameters.refreshRate = display_refresh(&mode->mode);
		written++;
	}

	/* Reports the output count actually written. */
	*pPropertyCount = written;
	if (written < count)
		return VK_INCOMPLETE;

	/* Succeeded: every timing fits in the caller's output array. */
	return VK_SUCCESS;
}

/*
 * Reuses a matching connector timing or records a bounded custom timing for a later kernel-validated modeset.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDisplayModeKHR(
	VkPhysicalDevice physicalDevice,
	VkDisplayKHR handle,
	const VkDisplayModeCreateInfoKHR *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkDisplayModeKHR *pMode)
{
	struct compat_display *display;
	struct compat_display_mode *mode;
	uint32_t width;
	uint32_t height;
	uint64_t clock;
	uint32_t refresh;
	uint32_t current_refresh;

	/* Mode handles share their display's stable inquiry lifetime rather than an application allocation lifetime. */
	(void)physicalDevice;
	(void)pAllocator;
	display = (struct compat_display *)(uintptr_t)handle;
	if (display == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires a complete requested timing and output destination. */
	if (pCreateInfo == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses an absent handle destination. */
	if (pMode == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Bounds the 16-bit DRM timing fields before deriving porch positions. */
	width = pCreateInfo->parameters.visibleRegion.width;
	height = pCreateInfo->parameters.visibleRegion.height;
	if (width == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A positive height likewise describes a real scanout. */
	if (height == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Reserves room for horizontal synchronization and blanking within DRM's timing fields. */
	if (width > 65000)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Vertical blanking needs the same bounded timing representation. */
	if (height > 65000)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A zero refresh rate cannot define a display timing. */
	refresh = pCreateInfo->parameters.refreshRate;
	if (refresh == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Searches current connector timings before allocating any custom record. */
	for (mode = display->modes; mode != NULL; mode = mode->next) {
		/* Both visible dimensions must identify the same timing. */
		if (mode->mode.hdisplay != width)
			continue;

		/* A differing height is a distinct scanout mode. */
		if (mode->mode.vdisplay != height)
			continue;

		/* A matching refresh rate permits the existing stable timing handle. */
		current_refresh = display_refresh(&mode->mode);
		if (current_refresh == refresh) {
			*pMode = (VkDisplayModeKHR)(uintptr_t)mode;
			return VK_SUCCESS;
		}
	}

	/* A custom record remains owned by the display for the lifetime required by Vulkan mode handles. */
	mode = calloc(1, sizeof(*mode));
	if (mode == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Derives a simple bounded timing; the KMS driver's SETCRTC validates actual monitor support. */
	mode->display = display;
	mode->mode.hdisplay = (uint16_t)width;
	mode->mode.hsync_start = (uint16_t)(width + 16);
	mode->mode.hsync_end = (uint16_t)(width + 32);
	mode->mode.htotal = (uint16_t)(width + 80);
	mode->mode.vdisplay = (uint16_t)height;
	mode->mode.vsync_start = (uint16_t)(height + 3);
	mode->mode.vsync_end = (uint16_t)(height + 6);
	mode->mode.vtotal = (uint16_t)(height + 20);
	clock = (uint64_t)mode->mode.htotal * mode->mode.vtotal * refresh;
	clock = (clock + 500000) / 1000000;
	if (clock > UINT32_MAX) {
		free(mode);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Stores the requested refresh and marks the timing as user-defined for the kernel modeset. */
	mode->mode.clock = (uint32_t)clock;
	mode->mode.vrefresh = (refresh + 500) / 1000;
	mode->mode.type = DRM_MODE_TYPE_USERDEF;
	mode->next = display->modes;
	display->modes = mode;
	*pMode = (VkDisplayModeKHR)(uintptr_t)mode;

	/* Succeeded: presentation still reports any actual kernel rejection of this custom timing. */
	return VK_SUCCESS;
}

/*
 * Describes full-plane identity scanout at the selected timing's exact visible dimensions.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetDisplayPlaneCapabilitiesKHR(
	VkPhysicalDevice physicalDevice,
	VkDisplayModeKHR handle,
	uint32_t planeIndex,
	VkDisplayPlaneCapabilitiesKHR *pCapabilities)
{
	struct compat_display_mode *mode;
	VkExtent2D extent;

	/* Only our one primary plane supports display presentation. */
	(void)physicalDevice;
	if (planeIndex != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires a stable timing and output capabilities destination. */
	mode = (struct compat_display_mode *)(uintptr_t)handle;
	if (mode == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* An absent output cannot receive the primary-plane contract. */
	if (pCapabilities == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Plane placement and source/destination extents remain one-to-one. */
	memset(pCapabilities, 0, sizeof(*pCapabilities));
	extent.width = mode->mode.hdisplay;
	extent.height = mode->mode.vdisplay;
	pCapabilities->supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	pCapabilities->minSrcExtent = extent;
	pCapabilities->maxSrcExtent = extent;
	pCapabilities->minDstExtent = extent;
	pCapabilities->maxDstExtent = extent;

	/* Succeeded: the compositor renders an opaque full-plane image with no scaling. */
	return VK_SUCCESS;
}

/*
 * Creates our own display surface while retaining its stable KMS timing record.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDisplayPlaneSurfaceKHR(
	VkInstance instance,
	const VkDisplaySurfaceCreateInfoKHR *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkSurfaceKHR *pSurface)
{
	struct compat_instance *owner;
	struct compat_display_mode *mode;
	struct compat_surface *surface;

	/* Requires the application's explicitly enabled display extension. */
	owner = compat_instance_get(instance);
	if (owner == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A display procedure cannot create a surface when its instance extension was not enabled. */
	if ((owner->enabled & COMPAT_INSTANCE_DISPLAY) == 0)
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Requires complete creation and output inputs before allocating storage. */
	if (pCreateInfo == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses a missing output destination. */
	if (pSurface == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Only the one primary plane and stack position are implemented. */
	if (pCreateInfo->planeIndex != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Reordering multiple hardware planes is outside the advertised contract. */
	if (pCreateInfo->planeStackIndex != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The selected timing is a stable KMS record rather than a backend WSI object. */
	mode = (struct compat_display_mode *)(uintptr_t)pCreateInfo->displayMode;
	if (mode == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Scaling or rotating scanout is outside the portable copy path. */
	if (pCreateInfo->transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Display buffers are always opaque XRGB storage. */
	if (pCreateInfo->alphaMode != VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Source images cover the exact selected physical timing. */
	if (pCreateInfo->imageExtent.width != mode->mode.hdisplay)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Both dimensions must match, since this implementation never silently rescales. */
	if (pCreateInfo->imageExtent.height != mode->mode.vdisplay)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Allocates the Vulkan surface object with its caller's allocation callbacks. */
	surface = compat_object_allocate(sizeof(*surface), pAllocator);
	if (surface == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Retains KMS ownership and geometry without creating any Wayland proxy. */
	surface->instance = owner;
	surface->kms = mode->display;
	surface->display_mode = mode;
	surface->extent = pCreateInfo->imageExtent;
	if (pAllocator != NULL) {
		surface->allocator = *pAllocator;
		surface->allocated = 1;
	}

	/* Publishes our private display surface handle. */
	*pSurface = (VkSurfaceKHR)(uintptr_t)surface;

	/* Succeeded: actual KMS ownership begins on explicit acquire or swapchain creation. */
	return VK_SUCCESS;
}

/*
 * Takes the seat's already acquired master file by duplicate, leaving the caller free to close its copy.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireDrmDisplayEXT(
	VkPhysicalDevice physicalDevice,
	int32_t drmFd,
	VkDisplayKHR handle)
{
	VkResult submission;
	struct compat_display *display;

	/* Rendering physical devices share the same portable display copy path. */
	(void)physicalDevice;
	display = (struct compat_display *)(uintptr_t)handle;
	if (display == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A caller-supplied master descriptor must be valid before duplication. */
	if (drmFd < 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Returns the exact master acquisition and saved-CRTC result. */
	submission = compat_kms_acquire(display, drmFd);
	if (submission != VK_SUCCESS)
		return submission;

	/* Succeeded: the display owns a duplicate of the caller master file. */
	return VK_SUCCESS;
}

/*
 * Maps a card's connector identity to the same stable Vulkan display handle used in enumeration.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetDrmDisplayEXT(
	VkPhysicalDevice physicalDevice,
	int32_t drmFd,
	uint32_t connectorId,
	VkDisplayKHR *pDisplay)
{
	struct compat_display *displays;
	struct compat_display *display;
	uint32_t count;
	VkResult error;

	/* This mapping performs inquiry only and never acquires master. */
	(void)physicalDevice;
	if (drmFd < 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A missing output destination cannot receive a mapped display. */
	if (pDisplay == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Enumerates the selected card's stable connector records. */
	error = compat_kms_displays(&displays, &count);
	if (error != VK_SUCCESS)
		return error;

	/* Requires the supplied file to address the same card without acquiring master. */
	error = compat_kms_card_matches(drmFd);
	if (error != VK_SUCCESS)
		return error;

	/* Finds the actual connector identity rather than its enumeration index. */
	for (display = displays; display != NULL; display = display->next) {
		/* The matched record remains stable for the process lifetime. */
		if (display->connector == connectorId)
			break;
	}

	/* An unknown connector is not a connected output of the selected card. */
	if (display == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Publishes the connector handle without acquiring display authority. */
	*pDisplay = (VkDisplayKHR)(uintptr_t)display;

	/* Succeeded: the caller received the selected card's stable connector handle. */
	return VK_SUCCESS;
}

/*
 * Releases explicit master ownership after the application has retired its display swapchains.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkReleaseDisplayEXT(
	VkPhysicalDevice physicalDevice,
	VkDisplayKHR handle)
{
	struct compat_display *display;

	/* Rendering physical-device ownership does not change the portable KMS display handle. */
	(void)physicalDevice;
	display = (struct compat_display *)(uintptr_t)handle;
	if (display == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Restores the acquired console and retires our duplicate of its master file. */
	compat_kms_release(display);

	/* Succeeded: no private master descriptor remains on this display. */
	return VK_SUCCESS;
}

/* Converts kernel timing units to Vulkan millihertz with bounded 64-bit arithmetic. */
static uint32_t
display_refresh(
	const struct drm_mode_modeinfo *mode)
{
	uint64_t numerator;
	uint64_t denominator;

	/* The timing's pixel clock is in kilohertz, so millihertz needs a million multiplier. */
	numerator = (uint64_t)mode->clock * 1000000;
	denominator = (uint64_t)mode->htotal * mode->vtotal;
	if (denominator == 0)
		return mode->vrefresh * 1000;

	/* Rounds to the nearest millihertz without floating point. */
	return (uint32_t)((numerator + denominator / 2) / denominator);
}

/* Enumerates display identities for primary-plane support without copying unrelated connector properties. */
static VkResult
display_list(
	uint32_t *count,
	VkDisplayKHR *output)
{
	struct compat_display *displays;
	struct compat_display *display;
	uint32_t total;
	uint32_t capacity;
	uint32_t written;
	VkResult error;

	/* Requires an enumeration count before requesting stable display records. */
	if (count == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Queries the selected card independently of the rendering backend. */
	error = compat_kms_displays(&displays, &total);
	if (error != VK_SUCCESS)
		return error;

	/* Measurement reports all connected display identities. */
	if (output == NULL) {
		*count = total;
		return VK_SUCCESS;
	}

	/* Copies only the caller's capacity and retains all process-lifetime records. */
	capacity = *count;
	written = 0;
	for (display = displays; display != NULL; display = display->next) {
		/* An exhausted output never writes past the caller's array. */
		if (written >= capacity)
			break;

		/* Each identity belongs to the same record returned by display-property enumeration. */
		output[written++] = (VkDisplayKHR)(uintptr_t)display;
	}

	/* Returns the actual copied count and an explicit incomplete outcome when necessary. */
	*count = written;
	if (written < total)
		return VK_INCOMPLETE;

	/* Succeeded: every connected display handle fits the output array. */
	return VK_SUCCESS;
}
