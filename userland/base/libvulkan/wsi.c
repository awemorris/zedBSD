/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Standard Vulkan display discovery and instance-owned surface descriptions.
 */

#include "wsi-internal.h"

#include <limits.h>
#include <string.h>

/*
 * Published display identities belonging to all live Vulkan instances.
 * The WSI mutex protects linkage and mutable output snapshots. Instance
 * teardown removes its identities before their physical devices disappear.
 */
static struct vulkan_display *wsi_displays;

/*
 * Published surfaces retained independently of logical devices.
 * Swapchain references prevent surface retirement while images still use it.
 */
static struct vulkan_surface *wsi_surfaces;

/*
 * Serializes WSI identities and reference counts without holding a wire lock.
 * Allocator callbacks and native queries run outside this mutex.
 */
static pthread_mutex_t wsi_mutex = PTHREAD_MUTEX_INITIALIZER;

static VkResult wsi_discovery_error(VkResult error);
static VkResult wsi_display_get(struct VkPhysicalDevice_T *physical, const struct vulkan_wsi_output *output, struct vulkan_display **result);
static VkResult wsi_mode_get(struct vulkan_display *display, uint64_t generation, const VkDisplayModeParametersKHR *parameters, const VkAllocationCallbacks *allocator, struct vulkan_display_mode **result);
static VkResult wsi_plane_count(struct VkPhysicalDevice_T *physical, uint32_t *count);
static VkResult wsi_plane_lookup(struct VkPhysicalDevice_T *physical, uint32_t plane, struct vulkan_display **display, uint32_t *local_plane);
static VkBool32 wsi_mode_equal(const VkDisplayModeParametersKHR *first, const VkDisplayModeParametersKHR *second);
static void wsi_surface_unlink(struct vulkan_surface *surface);
static void wsi_surface_free(struct vulkan_surface *surface);
static void wsi_display_free(struct vulkan_display *display);

/*
 * Enumerates connected display identities and their current capabilities.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceDisplayPropertiesKHR(
	VkPhysicalDevice physical_handle,
	uint32_t *count,
	VkDisplayPropertiesKHR *properties)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_wsi_output output;
	struct vulkan_display *display;
	VkDisplayPropertiesKHR *property;
	VkResult error;
	uint32_t native_count;
	uint32_t observed_count;
	uint32_t capacity;
	uint32_t written;
	uint32_t available;
	uint32_t index;

	/* Resolves the physical device whose native outputs are being enumerated. */
	physical = vulkan_physical_device(physical_handle);
	if (physical == NULL || count == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Captures caller capacity without treating count-only input as initialized. */
	capacity = 0U;
	if (properties != NULL)
		capacity = *count;

	/* Gets one bounded native output snapshot before walking its entries. */
	error = vulkan_wsi_display_query(physical, UINT32_MAX, &native_count, NULL);
	if (error != VK_SUCCESS) {
		error = wsi_discovery_error(error);
		return error;
	}

	/* Publishes only connected outputs, preserving stable handles across queries. */
	written = 0U;
	available = 0U;
	for (index = 0U; index < native_count; index++) {
		/* Rejects topology changes instead of mixing two enumeration snapshots. */
		error = vulkan_wsi_display_query(physical, index, &observed_count, &output);
		if (error != VK_SUCCESS) {
			error = wsi_discovery_error(error);
			return error;
		}

		/* A changed ordinal space must be queried again by the caller. */
		if (observed_count != native_count)
			return VK_ERROR_UNKNOWN;

		/* Disconnected native outputs contribute no available VkDisplayKHR. */
		if ((output.flags & VULKAN_WSI_OUTPUT_CONNECTED) == 0U)
			continue;

		/* Counts displays even after the caller's output array becomes full. */
		available++;
		if (written >= capacity)
			continue;

		/* Reuses the instance-lifetime identity associated with this native output. */
		error = wsi_display_get(physical, &output, &display);
		if (error != VK_SUCCESS) {
			error = wsi_discovery_error(error);
			return error;
		}

		/* Describes only behavior supported by the direct-display adapter. */
		property = &properties[written];
		memset(property, 0, sizeof(*property));
		property->display = (VkDisplayKHR)vulkan_nondispatchable_handle(&display->object);
		property->physicalDimensions = output.physical_dimensions;
		property->physicalResolution = output.preferred_extent;
		property->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
		property->planeReorderPossible = VK_FALSE;
		property->persistentContent = VK_FALSE;

		/* An absent native name is represented by the standard NULL value. */
		if (display->name[0] != '\0')
			property->displayName = display->name;

		/* The next slot belongs to the next connected output. */
		written++;
	}

	/* Count-only queries report every available display without writing handles. */
	if (properties == NULL) {
		*count = available;
		return VK_SUCCESS;
	}

	/* A short array reports exactly the number of initialized records. */
	*count = written;
	if (written < available)
		return VK_INCOMPLETE;

	/* Succeeded: every available display has a complete output record. */
	return VK_SUCCESS;
}

/*
 * Enumerates display planes without assuming a fixed number of outputs.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceDisplayPlanePropertiesKHR(
	VkPhysicalDevice physical_handle,
	uint32_t *count,
	VkDisplayPlanePropertiesKHR *properties)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_display *display;
	struct vulkan_wsi_output output;
	uint32_t available;
	uint32_t written;
	uint32_t local_plane;
	uint32_t index;
	VkResult error;

	/* Resolves the device before querying its independently counted planes. */
	physical = vulkan_physical_device(physical_handle);
	if (physical == NULL || count == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Counts every native plane before observing caller capacity. */
	error = wsi_plane_count(physical, &available);
	if (error != VK_SUCCESS) {
		error = wsi_discovery_error(error);
		return error;
	}

	/* Count-only queries do not consume the incoming count value. */
	if (properties == NULL) {
		*count = available;
		return VK_SUCCESS;
	}

	/* Limits initialization to the caller's actual array capacity. */
	written = *count;
	if (written > available)
		written = available;

	/* Resolves global plane ordinals to their native display and local plane. */
	for (index = 0U; index < written; index++) {
		error = wsi_plane_lookup(physical, index, &display, &local_plane);
		if (error != VK_SUCCESS) {
			error = wsi_discovery_error(error);
			return error;
		}

		/* Reads a coherent output snapshot without borrowing mutable cache fields. */
		error = vulkan_wsi_display_snapshot(display, &output);
		if (error != VK_SUCCESS) {
			error = wsi_discovery_error(error);
			return error;
		}

		/* Unbound planes are valid planes with no current display association. */
		properties[index].currentDisplay = VK_NULL_HANDLE;
		properties[index].currentStackIndex = local_plane;
		if ((output.flags & VULKAN_WSI_OUTPUT_ACTIVE) != 0U) {
			properties[index].currentDisplay =
			    (VkDisplayKHR)vulkan_nondispatchable_handle(&display->object);
		}
	}

	/* Reports the count of fully initialized plane records. */
	*count = written;
	if (written < available)
		return VK_INCOMPLETE;

	/* Succeeded: every native plane has been described. */
	return VK_SUCCESS;
}

/*
 * Enumerates displays reachable through one physical device's plane.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetDisplayPlaneSupportedDisplaysKHR(
	VkPhysicalDevice physical_handle,
	uint32_t plane_index,
	uint32_t *count,
	VkDisplayKHR *displays)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_display *display;
	struct vulkan_wsi_output output;
	uint32_t local_plane;
	uint32_t available;
	VkResult error;

	/* Resolves the selected global plane before examining array capacity. */
	physical = vulkan_physical_device(physical_handle);
	if (physical == NULL || count == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Each direct adapter plane belongs to one independently identified output. */
	error = wsi_plane_lookup(physical, plane_index, &display, &local_plane);
	if (error != VK_SUCCESS) {
		error = wsi_discovery_error(error);
		return error;
	}

	/* Reads connection state without claiming the plane for presentation. */
	error = vulkan_wsi_display_snapshot(display, &output);
	if (error != VK_SUCCESS) {
		error = wsi_discovery_error(error);
		return error;
	}

	/* A disconnected output remains a plane but currently supports no display. */
	available = 0U;
	if ((output.flags & VULKAN_WSI_OUTPUT_CONNECTED) != 0U)
		available = 1U;

	/* A count-only query reports the actual number of reachable displays. */
	if (displays == NULL) {
		*count = available;
		return VK_SUCCESS;
	}

	/* Empty output sets need no caller storage. */
	if (available == 0U) {
		*count = 0U;
		return VK_SUCCESS;
	}

	/* A zero-capacity array cannot receive the reachable display handle. */
	if (*count == 0U)
		return VK_INCOMPLETE;

	/* Publishes the stable display identity for this plane. */
	displays[0] = (VkDisplayKHR)vulkan_nondispatchable_handle(&display->object);
	*count = 1U;

	/* Succeeded: the plane's sole supported native output is identified. */
	return VK_SUCCESS;
}

/*
 * Enumerates validated modes while retaining their handles for the instance.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetDisplayModePropertiesKHR(
	VkPhysicalDevice physical_handle,
	VkDisplayKHR display_handle,
	uint32_t *count,
	VkDisplayModePropertiesKHR *properties)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_display *display;
	struct vulkan_display_mode *mode;
	struct vulkan_wsi_output output;
	VkDisplayModeParametersKHR parameters;
	uint32_t available;
	uint32_t observed_count;
	uint32_t written;
	uint32_t index;
	VkResult error;
	const VkAllocationCallbacks *allocator;

	/* Refuses a valid display handle belonging to another physical device. */
	physical = vulkan_physical_device(physical_handle);
	display = vulkan_wsi_display(display_handle);
	if (physical == NULL ||
	    display == NULL ||
	    count == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Display mode handles cannot cross physical-device ownership boundaries. */
	if (display->physical != physical)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refreshes connection and generation before native mode enumeration. */
	error = vulkan_wsi_display_refresh(display);
	if (error != VK_SUCCESS) {
		error = wsi_discovery_error(error);
		return error;
	}

	/* Copies the generation which every native mode query must preserve. */
	error = vulkan_wsi_display_snapshot(display, &output);
	if (error != VK_SUCCESS) {
		error = wsi_discovery_error(error);
		return error;
	}

	/* Retrieves the native mode count without dereferencing caller storage. */
	error = vulkan_wsi_display_mode_query(physical, &output, UINT32_MAX, &available, NULL);
	if (error != VK_SUCCESS) {
		error = wsi_discovery_error(error);
		return error;
	}

	/* Count-only mode queries leave every mode object lazily unallocated. */
	if (properties == NULL) {
		*count = available;
		return VK_SUCCESS;
	}

	/* Initializes no more than the capacity supplied by the caller. */
	written = *count;
	if (written > available)
		written = available;

	/* Retains one stable handle for each initialized native mode description. */
	for (index = 0U; index < written; index++) {
		error = vulkan_wsi_display_mode_query(physical, &output, index, &observed_count, &parameters);
		if (error != VK_SUCCESS) {
			error = wsi_discovery_error(error);
			return error;
		}

		/* Refuses an enumeration whose ordinal space changed during the query. */
		if (observed_count != available)
			return VK_ERROR_UNKNOWN;

		/* Uses the instance allocator for modes discovered without a create call. */
		allocator = NULL;
		if (physical->instance->object.allocator.has_callbacks != VK_FALSE)
			allocator = &physical->instance->object.allocator.callbacks;
		error = wsi_mode_get(display, output.generation, &parameters, allocator, &mode);
		if (error != VK_SUCCESS) {
			error = wsi_discovery_error(error);
			return error;
		}

		/* Publishes the standard mode description and its durable local handle. */
		properties[index].displayMode =
		    (VkDisplayModeKHR)vulkan_nondispatchable_handle(&mode->object);
		properties[index].parameters = parameters;
	}

	/* Distinguishes a truncated array from complete mode enumeration. */
	*count = written;
	if (written < available)
		return VK_INCOMPLETE;

	/* Succeeded: every supported mode has been described. */
	return VK_SUCCESS;
}

/*
 * Creates a display mode description without changing the active output.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDisplayModeKHR(
	VkPhysicalDevice physical_handle,
	VkDisplayKHR display_handle,
	const VkDisplayModeCreateInfoKHR *create_info,
	const VkAllocationCallbacks *allocator,
	VkDisplayModeKHR *mode_handle)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_display *display;
	struct vulkan_display_mode *mode;
	struct vulkan_wsi_output output;
	VkResult error;

	/* Checks object ownership before retaining any mode or caller allocator. */
	physical = vulkan_physical_device(physical_handle);
	display = vulkan_wsi_display(display_handle);
	if (physical == NULL ||
	    display == NULL ||
	    create_info == NULL ||
	    mode_handle == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* A mode can describe only an output belonging to the supplied device. */
	if (display->physical != physical)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Rejects malformed creation metadata before any native query. */
	if (create_info->sType != VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR ||
	    create_info->flags != 0U) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Zero dimensions or refresh cannot describe a realizable display mode. */
	if (create_info->parameters.visibleRegion.width == 0U ||
	    create_info->parameters.visibleRegion.height == 0U ||
	    create_info->parameters.refreshRate == 0U) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Resolves the current native generation without applying a modeset. */
	error = vulkan_wsi_display_refresh(display);
	if (error != VK_SUCCESS)
		return error;

	/* Validates the requested parameters against one consistent capability view. */
	error = vulkan_wsi_display_snapshot(display, &output);
	if (error != VK_SUCCESS)
		return error;

	/* The native adapter checks general mode limits, without demo-specific sizes. */
	error = vulkan_wsi_display_mode_validate(physical, &output, &create_info->parameters);
	if (error != VK_SUCCESS)
		return error;

	/* Retains a local mode object only after the native validation succeeds. */
	error = wsi_mode_get(display, output.generation, &create_info->parameters, allocator, &mode);
	if (error != VK_SUCCESS)
		return error;

	/* Publishes the instance-lifetime handle without changing display ownership. */
	*mode_handle = (VkDisplayModeKHR)vulkan_nondispatchable_handle(&mode->object);

	/* Succeeded: the caller may describe a surface using this validated mode. */
	return VK_SUCCESS;
}

/*
 * Reports the direct plane's exact full-image presentation limits.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetDisplayPlaneCapabilitiesKHR(
	VkPhysicalDevice physical_handle,
	VkDisplayModeKHR mode_handle,
	uint32_t plane_index,
	VkDisplayPlaneCapabilitiesKHR *capabilities)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_display_mode *mode;
	struct vulkan_display *display;
	struct vulkan_wsi_output output;
	uint32_t local_plane;
	VkResult error;

	/* Resolves the mode and refuses cross-device handles. */
	physical = vulkan_physical_device(physical_handle);
	mode = vulkan_wsi_display_mode(mode_handle);
	if (physical == NULL ||
	    mode == NULL ||
	    capabilities == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The physical owner is inherited from the mode's instance-owned display. */
	if (mode->display->physical != physical)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Resolves the global plane to its actual output before declaring support. */
	error = wsi_plane_lookup(physical, plane_index, &display, &local_plane);
	if (error != VK_SUCCESS) {
		error = wsi_discovery_error(error);
		return error;
	}

	/* Independent direct outputs cannot borrow one another's display modes. */
	if (display != mode->display)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A mode remains a valid handle but can become unusable after topology change. */
	error = vulkan_wsi_display_snapshot(display, &output);
	if (error != VK_SUCCESS) {
		error = wsi_discovery_error(error);
		return error;
	}

	/* Revalidates the parameters against the current native output. */
	error = vulkan_wsi_display_mode_validate(physical, &output, &mode->parameters);
	if (error != VK_SUCCESS) {
		error = wsi_discovery_error(error);
		return error;
	}

	/* Describes only full-image 1:1 opaque presentation, without scaling claims. */
	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	capabilities->minSrcExtent = mode->parameters.visibleRegion;
	capabilities->maxSrcExtent = mode->parameters.visibleRegion;
	capabilities->minDstExtent = mode->parameters.visibleRegion;
	capabilities->maxDstExtent = mode->parameters.visibleRegion;

	/* Succeeded: each reported rectangle limit matches the direct adapter. */
	return VK_SUCCESS;
}

/*
 * Creates an instance-owned description of one display plane and mode.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDisplayPlaneSurfaceKHR(
	VkInstance instance_handle,
	const VkDisplaySurfaceCreateInfoKHR *create_info,
	const VkAllocationCallbacks *allocator,
	VkSurfaceKHR *surface_handle)
{
	struct VkInstance_T *instance;
	struct vulkan_display_mode *mode;
	struct vulkan_display *display;
	struct vulkan_surface *surface;
	struct vulkan_object *object;
	VkDisplayPlaneCapabilitiesKHR capabilities;
	uint32_t local_plane;
	VkResult error;

	/* Resolves the instance and the mode before allocating a surface. */
	instance = vulkan_instance(instance_handle);
	if (instance == NULL ||
	    create_info == NULL ||
	    surface_handle == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires the exact standard display-surface creation structure. */
	if (create_info->sType != VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR ||
	    create_info->flags != 0U) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Mode handles carry their originating instance through the physical device. */
	mode = vulkan_wsi_display_mode(create_info->displayMode);
	if (mode == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A local mode from another instance cannot describe this instance's surface. */
	if (mode->display->physical->instance != instance)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Exposes the direct adapter's supported transform and composition behavior. */
	if (create_info->transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
	    create_info->alphaMode != VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* The initial adapter supports the complete selected mode without scaling. */
	if (create_info->imageExtent.width != mode->parameters.visibleRegion.width ||
	    create_info->imageExtent.height != mode->parameters.visibleRegion.height) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Verifies that this mode can be presented by the requested global plane. */
	error = vkGetDisplayPlaneCapabilitiesKHR(
		(VkPhysicalDevice)mode->display->physical,
		create_info->displayMode,
		create_info->planeIndex,
		&capabilities);
	if (error != VK_SUCCESS)
		return error;

	/* Retains the native local plane index independently of its public ordinal. */
	error = wsi_plane_lookup(mode->display->physical, create_info->planeIndex, &display, &local_plane);
	if (error != VK_SUCCESS)
		return error;

	/* Plane ordering is fixed by the adapter's reported current stack index. */
	if (create_info->planeStackIndex != local_plane)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Allocates only a local surface description; swapchain creation claims output. */
	error = vulkan_object_alloc(
		sizeof(*surface),
		__alignof__(struct vulkan_surface),
		VULKAN_OBJECT_SURFACE,
		&instance->object,
		mode->object.context,
		allocator,
		VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
		&object);
	if (error != VK_SUCCESS)
		return error;

	/* Saves creation values without retaining a caller-owned pNext chain. */
	surface = (struct vulkan_surface *)object;
	surface->instance = instance;
	surface->platform = &vulkan_wsi_display_platform;
	surface->display_mode = mode;
	surface->next = NULL;
	surface->active = NULL;
	surface->display_info = *create_info;
	surface->display_info.pNext = NULL;
	surface->native_plane = local_plane;
	surface->swapchain_references = 0U;

	/* Publishes generic ownership before making the surface discoverable to WSI. */
	error = vulkan_object_publish(object);
	if (error != VK_SUCCESS) {
		vulkan_object_free(object);
		return error;
	}

	/* Registers the description for instance-lifetime cleanup. */
	pthread_mutex_lock(&wsi_mutex);

	surface->next = wsi_surfaces;
	wsi_surfaces = surface;

	pthread_mutex_unlock(&wsi_mutex);

	/* Returns the standard opaque handle only after every ownership link exists. */
	*surface_handle = (VkSurfaceKHR)vulkan_nondispatchable_handle(object);

	/* Succeeded: no display ownership was acquired by describing this surface. */
	return VK_SUCCESS;
}


/*
 * Releases a surface after its swapchains have relinquished their references.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroySurfaceKHR(
	VkInstance instance_handle,
	VkSurfaceKHR surface_handle,
	const VkAllocationCallbacks *allocator)
{
	struct VkInstance_T *instance;
	struct vulkan_surface *surface;
	uint32_t references;

	/* Destruction uses the allocator saved at creation, as required by its lifetime. */


	/* Standard null-handle destruction performs no work. */
	if (surface_handle == VK_NULL_HANDLE)
		return;

	/* Refuses internal ownership mistakes without freeing a foreign description. */
	instance = vulkan_instance(instance_handle);
	surface = vulkan_wsi_surface(surface_handle);
	if (instance == NULL || surface == NULL)
		return;

	/* Surfaces remain children of their original instance. */
	if (surface->instance != instance)
		return;

	/* A live swapchain prevents native surface teardown. */
	pthread_mutex_lock(&wsi_mutex);

	references = surface->swapchain_references;

	pthread_mutex_unlock(&wsi_mutex);

	/* Valid Vulkan callers destroy their swapchains before their surface. */
	if (references != 0U)
		return;

	/* Withdraws the identity before freeing platform-specific state. */
	wsi_surface_unlink(surface);
	/* Compatible current callbacks may carry a different destruction user-data pointer. */
	if (allocator != NULL) {
		surface->object.allocator.callbacks = *allocator;
		surface->object.allocator.has_callbacks = VK_TRUE;
	}

	wsi_surface_free(surface);

	/* Succeeded: neither WSI nor the instance retains this surface. */
	return;
}

/*
 * Reports whether a queue family can present to the supplied surface.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceSupportKHR(
	VkPhysicalDevice physical_handle,
	uint32_t queue_family,
	VkSurfaceKHR surface_handle,
	VkBool32 *supported)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_surface *surface;
	VkSurfaceCapabilitiesKHR capabilities;
	VkQueueFlags queue_flags;
	VkResult error;

	/* Resolves the surface and initializes the answer only after validating storage. */
	physical = vulkan_physical_device(physical_handle);
	surface = vulkan_wsi_surface(surface_handle);
	if (physical == NULL ||
	    surface == NULL ||
	    supported == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A queue-family ordinal must name a queue exposed by this physical device. */
	if (queue_family >= physical->queue_family_count)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The direct backend cannot present through a different GPU's display engine. */
	*supported = VK_FALSE;
	if (surface->display_mode != NULL) {
		/* Cross-device surfaces remain valid objects with unsupported presentation. */
		if (surface->display_mode->display->physical != physical)
			return VK_SUCCESS;
	}

	/* Presentation readback requires a queue capable of transfer commands. */
	queue_flags = physical->queue_families[queue_family].queueFlags;
	if ((queue_flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) == 0U)
		return VK_SUCCESS;

	/* Native capabilities distinguish an available target from a lost surface. */
	error = surface->platform->capabilities(surface, physical, &capabilities);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: this queue can supply complete images to the native adapter. */
	*supported = VK_TRUE;
	return VK_SUCCESS;
}

/*
 * Retrieves the presentation limits of one surface on one physical device.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
	VkPhysicalDevice physical_handle,
	VkSurfaceKHR surface_handle,
	VkSurfaceCapabilitiesKHR *capabilities)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_surface *surface;
	VkResult error;

	/* Resolves standard handles before entering the selected native backend. */
	physical = vulkan_physical_device(physical_handle);
	surface = vulkan_wsi_surface(surface_handle);
	if (physical == NULL ||
	    surface == NULL ||
	    capabilities == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The platform reports current extent and usage without claiming output. */
	error = surface->platform->capabilities(surface, physical, capabilities);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: capabilities describe the current native surface generation. */
	return VK_SUCCESS;
}

/*
 * Enumerates the format and color-space pairs accepted by a surface.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceFormatsKHR(
	VkPhysicalDevice physical_handle,
	VkSurfaceKHR surface_handle,
	uint32_t *count,
	VkSurfaceFormatKHR *formats)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_surface *surface;
	VkResult error;

	/* Resolves the surface before delegating native format enumeration. */
	physical = vulkan_physical_device(physical_handle);
	surface = vulkan_wsi_surface(surface_handle);
	if (physical == NULL ||
	    surface == NULL ||
	    count == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The platform intersects native pixel layouts with actual GPU format support. */
	error = surface->platform->formats(surface, physical, count, formats);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: every reported format can back this surface's swapchain images. */
	return VK_SUCCESS;
}

/*
 * Enumerates the presentation modes actually implemented by a surface.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfacePresentModesKHR(
	VkPhysicalDevice physical_handle,
	VkSurfaceKHR surface_handle,
	uint32_t *count,
	VkPresentModeKHR *modes)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_surface *surface;
	VkResult error;

	/* Resolves standard handles without assuming that all native surfaces are alike. */
	physical = vulkan_physical_device(physical_handle);
	surface = vulkan_wsi_surface(surface_handle);
	if (physical == NULL ||
	    surface == NULL ||
	    count == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The platform must prove FIFO support before advertising a usable swapchain. */
	error = surface->platform->present_modes(surface, physical, count, modes);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the array contains only supported native presentation behavior. */
	return VK_SUCCESS;
}

/*
 * Resolves a standard surface handle to its library-owned description.
 */
struct vulkan_surface *
vulkan_wsi_surface(
	VkSurfaceKHR handle)
{
	struct vulkan_object *object;

	/* Converts the opaque representation in the common object implementation. */
	object = vulkan_nondispatchable_object((uint64_t)handle);
	if (object == NULL)
		return NULL;

	/* A different non-dispatchable object cannot become a native surface. */
	if (object->kind != VULKAN_OBJECT_SURFACE)
		return NULL;

	/* Succeeded: the object has the surface-specific representation. */
	return (struct vulkan_surface *)object;
}

/*
 * Resolves a display handle without confusing it with a renderer wire identity.
 */
struct vulkan_display *
vulkan_wsi_display(
	VkDisplayKHR handle)
{
	struct vulkan_object *object;

	/* Local display handles never contain a host renderer object number. */
	object = vulkan_nondispatchable_object((uint64_t)handle);
	if (object == NULL)
		return NULL;

	/* A mode or swapchain object cannot identify an entire output. */
	if (object->kind != VULKAN_OBJECT_DISPLAY)
		return NULL;

	/* Succeeded: the display remains owned by its originating instance. */
	return (struct vulkan_display *)object;
}

/*
 * Resolves a mode handle to its durable native mode description.
 */
struct vulkan_display_mode *
vulkan_wsi_display_mode(
	VkDisplayModeKHR handle)
{
	struct vulkan_object *object;

	/* Uses the common opaque-handle conversion before inspecting private metadata. */
	object = vulkan_nondispatchable_object((uint64_t)handle);
	if (object == NULL)
		return NULL;

	/* Only a display-mode object carries validated mode parameters. */
	if (object->kind != VULKAN_OBJECT_DISPLAY_MODE)
		return NULL;

	/* Succeeded: the mode's display and generation remain available to WSI. */
	return (struct vulkan_display_mode *)object;
}

/*
 * Refreshes one display by stable identity without changing its immutable name.
 */
VkResult
vulkan_wsi_display_refresh(
	struct vulkan_display *display)
{
	struct vulkan_wsi_output output;
	uint64_t identifier;
	uint32_t count;
	uint32_t observed_count;
	uint32_t index;
	VkResult error;

	/* Resolves the native enumeration belonging to this display's physical device. */
	error = vulkan_wsi_display_query(display->physical, UINT32_MAX, &count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Saves the stable identity separately from mutable native snapshot fields. */
	pthread_mutex_lock(&wsi_mutex);

	identifier = display->output.identifier;

	pthread_mutex_unlock(&wsi_mutex);

	/* Native ordinals may change, but an existing display handle keeps its identity. */
	for (index = 0U; index < count; index++) {
		error = vulkan_wsi_display_query(display->physical, index, &observed_count, &output);
		if (error != VK_SUCCESS)
			return error;

		/* A concurrent topology change requires a fresh complete query. */
		if (observed_count != count)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* Other outputs must not overwrite the retained display identity. */
		if (output.identifier != identifier)
			continue;

		/* Publishes one coherent capability snapshot while retaining the original name. */
		pthread_mutex_lock(&wsi_mutex);

		display->output = output;

		pthread_mutex_unlock(&wsi_mutex);

		/* A disconnected display keeps its handle but cannot accept presentation. */
		if ((output.flags & VULKAN_WSI_OUTPUT_CONNECTED) == 0U)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* Succeeded: the display now describes the current native generation. */
		return VK_SUCCESS;
	}

	/* Marks an absent output as disconnected without reusing its display handle. */
	pthread_mutex_lock(&wsi_mutex);

	display->output.flags &= ~VULKAN_WSI_OUTPUT_CONNECTED;

	pthread_mutex_unlock(&wsi_mutex);

	/* The native identity disappeared; the caller must rebuild its presentation target. */
	return VK_ERROR_SURFACE_LOST_KHR;
}

/*
 * Retains a surface while a swapchain owns images targeting it.
 */
VkResult
vulkan_wsi_surface_retain(
	struct vulkan_surface *surface)
{
	/* Prevents the surface count from wrapping into apparent unreferenced state. */
	pthread_mutex_lock(&wsi_mutex);

	/* Refuses reference exhaustion before a live surface can lose its retention accounting. */
	if (surface->swapchain_references == UINT32_MAX) {
		pthread_mutex_unlock(&wsi_mutex);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* A counted swapchain prevents surface retirement until its final release. */
	surface->swapchain_references++;

	pthread_mutex_unlock(&wsi_mutex);

	/* Succeeded: the swapchain owns one surface-lifetime reference. */
	return VK_SUCCESS;
}

/*
 * Releases the surface reference formerly owned by one swapchain.
 */
void
vulkan_wsi_surface_release(
	struct vulkan_surface *surface)
{
	/* Counts only real references, preventing an underflow from hiding ownership bugs. */
	pthread_mutex_lock(&wsi_mutex);

	/* Consumes only a surface reference previously retained by a swapchain. */
	if (surface->swapchain_references != 0U)
		surface->swapchain_references--;

	pthread_mutex_unlock(&wsi_mutex);

	/* Succeeded: the caller no longer retains this surface. */
	return;
}

/*
 * Releases display descriptions after their instance's devices have retired.
 */
void
vulkan_wsi_instance_finish(
	struct VkInstance_T *instance)
{
	struct vulkan_surface *surface;
	struct vulkan_display *display;
	struct vulkan_display **link;

	/* Removes the instance's surfaces without retaining the mutex during callbacks. */
	for (;;) {
		/* Selects one remaining surface belonging to the retiring instance. */
		pthread_mutex_lock(&wsi_mutex);

		surface = wsi_surfaces;
		while (surface != NULL && surface->instance != instance)
			surface = surface->next;

		pthread_mutex_unlock(&wsi_mutex);

		/* No native surface remains after the final matching object is removed. */
		if (surface == NULL)
			break;

		/* Device teardown must retire swapchains before instance teardown begins. */
		wsi_surface_unlink(surface);
		wsi_surface_free(surface);
	}

	/* Detaches each display before invoking its saved allocator outside the mutex. */
	for (;;) {
		/* Finds an instance-owned identity without disturbing another instance. */
		pthread_mutex_lock(&wsi_mutex);

		link = &wsi_displays;
		while (*link != NULL && (*link)->physical->instance != instance)
			link = &(*link)->next;

		/* Removes one identity while its parent physical device is still alive. */
		display = *link;
		if (display != NULL)
			*link = display->next;

		pthread_mutex_unlock(&wsi_mutex);

		/* Every mode belongs to a display which has already been withdrawn. */
		if (display == NULL)
			break;

		/* Frees dependent mode descriptions before their display object. */
		wsi_display_free(display);
	}

	/* Succeeded: this instance leaves no WSI-owned native identities behind. */
	return;
}

/*
 * Copies a coherent output snapshot without retaining the WSI mutex.
 */
VkResult
vulkan_wsi_display_snapshot(
	struct vulkan_display *display,
	struct vulkan_wsi_output *output)
{
	/* Mutable native capabilities are copied while their identity remains stable. */
	pthread_mutex_lock(&wsi_mutex);

	*output = display->output;

	pthread_mutex_unlock(&wsi_mutex);

	/* Succeeded: callers may query the native backend after releasing the mutex. */
	return VK_SUCCESS;
}

/* Restricts discovery failures to the standard display-query result set. */
static VkResult
wsi_discovery_error(
	VkResult error)
{
	/* Native topology loss is not a surface result for display inventory queries. */
	if (error == VK_ERROR_OUT_OF_HOST_MEMORY ||
	    error == VK_ERROR_OUT_OF_DEVICE_MEMORY)
		return error;

	/* Other discovery failures require a fresh query without inventing a surface. */
	return VK_ERROR_UNKNOWN;
}

/* Retains one stable display identity for a native output snapshot. */
static VkResult
wsi_display_get(
	struct VkPhysicalDevice_T *physical,
	const struct vulkan_wsi_output *output,
	struct vulkan_display **result)
{
	struct vulkan_display *display;
	struct vulkan_display *candidate;
	struct vulkan_object *object;
	const VkAllocationCallbacks *allocator;
	VkResult error;

	/* Native identifiers must distinguish outputs even when their names are absent. */
	if (output->identifier == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Uses the instance's saved callbacks for implicitly discovered identities. */
	allocator = NULL;
	if (physical->instance->object.allocator.has_callbacks != VK_FALSE)
		allocator = &physical->instance->object.allocator.callbacks;

	/* Allocates outside WSI serialization so user callbacks cannot deadlock this lock. */
	error = vulkan_object_alloc(
		sizeof(*candidate),
		__alignof__(struct vulkan_display),
		VULKAN_OBJECT_DISPLAY,
		&physical->object,
		physical->object.context,
		allocator,
		VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE,
		&object);
	if (error != VK_SUCCESS)
		return error;

	/* Initializes a candidate without modifying the common object's ownership fields. */
	candidate = (struct vulkan_display *)object;
	candidate->physical = physical;
	candidate->output = *output;
	candidate->next = NULL;
	candidate->modes = NULL;
	memcpy(candidate->name, output->name, sizeof(candidate->name));
	candidate->name[sizeof(candidate->name) - 1U] = '\0';

	/* Reuses a previously published identity even when native ordinals have moved. */
	pthread_mutex_lock(&wsi_mutex);

	display = wsi_displays;
	while (display != NULL) {
		/* Both physical ownership and stable native identity must match. */
		if (display->physical == physical && display->output.identifier == output->identifier)
			break;

		/* Advances without comparing mutable names or mode dimensions. */
		display = display->next;
	}

	/* A racing query keeps the existing handle and updates only its capability view. */
	if (display != NULL) {
		display->output = *output;
		pthread_mutex_unlock(&wsi_mutex);
		vulkan_object_free(object);
		*result = display;
		return VK_SUCCESS;
	}

	/* Generic publication gives instance teardown the same object ownership model. */
	error = vulkan_object_publish(object);
	if (error != VK_SUCCESS) {
		pthread_mutex_unlock(&wsi_mutex);
		vulkan_object_free(object);
		return error;
	}

	/* The newly published identity becomes the head of the protected display list. */
	candidate->next = wsi_displays;
	wsi_displays = candidate;

	pthread_mutex_unlock(&wsi_mutex);

	/* Publishes the stable identity only after its lifetime links exist. */
	*result = candidate;

	/* Succeeded: this native output has an instance-lifetime display handle. */
	return VK_SUCCESS;
}

/* Retains a mode description for one display generation. */
static VkResult
wsi_mode_get(
	struct vulkan_display *display,
	uint64_t generation,
	const VkDisplayModeParametersKHR *parameters,
	const VkAllocationCallbacks *allocator,
	struct vulkan_display_mode **result)
{
	struct vulkan_display_mode *mode;
	struct vulkan_display_mode *candidate;
	struct vulkan_object *object;
	VkBool32 equal;
	VkResult error;

	/* Allocates a local description without reserving a renderer wire object. */
	error = vulkan_object_alloc(
		sizeof(*candidate),
		__alignof__(struct vulkan_display_mode),
		VULKAN_OBJECT_DISPLAY_MODE,
		&display->object,
		display->object.context,
		allocator,
		VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE,
		&object);
	if (error != VK_SUCCESS)
		return error;

	/* Records the validated geometry while preserving generic object metadata. */
	candidate = (struct vulkan_display_mode *)object;
	candidate->display = display;
	candidate->next = NULL;
	candidate->parameters = *parameters;
	candidate->generation = generation;

	/* A concurrent native-generation change invalidates the just-validated parameters. */
	pthread_mutex_lock(&wsi_mutex);

	/* Rejects a mode validated against a topology snapshot which has since changed. */
	if (display->output.generation != generation) {
		pthread_mutex_unlock(&wsi_mutex);
		vulkan_object_free(object);
		return VK_ERROR_SURFACE_LOST_KHR;
	}

	/* Reuses an identical mode from the same capability generation. */
	mode = display->modes;
	while (mode != NULL) {
		/* Modes from prior generations remain valid identities but are not reused. */
		if (mode->generation == generation) {
			/* Compares parameters without conflating padding bytes with mode identity. */
			equal = wsi_mode_equal(&mode->parameters, parameters);
			if (equal != VK_FALSE)
				break;
		}

		/* Advances through the instance-lifetime mode list. */
		mode = mode->next;
	}

	/* A racing creator retains the existing mode's original allocator and identity. */
	if (mode != NULL) {
		pthread_mutex_unlock(&wsi_mutex);
		vulkan_object_free(object);
		*result = mode;
		return VK_SUCCESS;
	}

	/* Publishes local ownership before exposing the opaque mode handle. */
	error = vulkan_object_publish(object);
	if (error != VK_SUCCESS) {
		pthread_mutex_unlock(&wsi_mutex);
		vulkan_object_free(object);
		return error;
	}

	/* The display retains this mode until the instance destroys every mode. */
	candidate->next = display->modes;
	display->modes = candidate;

	pthread_mutex_unlock(&wsi_mutex);

	/* Publishes the retained description only after both ownership lists agree. */
	*result = candidate;

	/* Succeeded: the caller holds a mode identity for the validated generation. */
	return VK_SUCCESS;
}

/* Counts planes across dynamically enumerated native outputs. */
static VkResult
wsi_plane_count(
	struct VkPhysicalDevice_T *physical,
	uint32_t *count)
{
	struct vulkan_wsi_output output;
	uint32_t native_count;
	uint32_t observed_count;
	uint32_t total;
	uint32_t index;
	VkResult error;

	/* Obtains the number of native output records before traversing their planes. */
	error = vulkan_wsi_display_query(physical, UINT32_MAX, &native_count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Sums actual plane counts without imposing a library object-array limit. */
	total = 0U;
	for (index = 0U; index < native_count; index++) {
		error = vulkan_wsi_display_query(physical, index, &observed_count, &output);
		if (error != VK_SUCCESS)
			return error;

		/* Native output insertion or removal requires a fresh snapshot. */
		if (observed_count != native_count)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* Prevents a malformed backend count from wrapping public plane ordinals. */
		if (output.plane_count > UINT32_MAX - total)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* The next output's first global plane follows all preceding native planes. */
		total += output.plane_count;
	}

	/* Publishes a count only after the complete native snapshot was read. */
	*count = total;

	/* Succeeded: count describes every public plane ordinal. */
	return VK_SUCCESS;
}

/* Resolves a global plane ordinal to one native output and local plane. */
static VkResult
wsi_plane_lookup(
	struct VkPhysicalDevice_T *physical,
	uint32_t plane,
	struct vulkan_display **display,
	uint32_t *local_plane)
{
	struct vulkan_wsi_output output;
	uint32_t native_count;
	uint32_t observed_count;
	uint32_t remaining;
	uint32_t index;
	VkResult error;

	/* Retrieves a stable ordinal space before consuming the requested plane index. */
	error = vulkan_wsi_display_query(physical, UINT32_MAX, &native_count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Each output contributes its own number of native display planes. */
	remaining = plane;
	for (index = 0U; index < native_count; index++) {
		error = vulkan_wsi_display_query(physical, index, &observed_count, &output);
		if (error != VK_SUCCESS)
			return error;

		/* A mixed topology snapshot cannot safely assign a global plane ordinal. */
		if (observed_count != native_count)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* This output owns the requested ordinal when its local range contains it. */
		if (remaining < output.plane_count) {
			/* Retains a stable identity rather than exposing the current native ordinal. */
			error = wsi_display_get(physical, &output, display);
			if (error != VK_SUCCESS)
				return error;

			/* The adapter receives the native local index when it claims this plane. */
			*local_plane = remaining;
			return VK_SUCCESS;
		}

		/* Removes only a completely preceding output's plane contribution. */
		remaining -= output.plane_count;
	}

	/* No native output contains this plane ordinal. */
	return VK_ERROR_INITIALIZATION_FAILED;
}


/* Compares the actual mode parameters rather than their structure padding. */
static VkBool32
wsi_mode_equal(
	const VkDisplayModeParametersKHR *first,
	const VkDisplayModeParametersKHR *second)
{
	/* Width identifies the horizontal visible pixel extent. */
	if (first->visibleRegion.width != second->visibleRegion.width)
		return VK_FALSE;

	/* Height identifies the vertical visible pixel extent. */
	if (first->visibleRegion.height != second->visibleRegion.height)
		return VK_FALSE;

	/* Refresh is part of mode identity even when dimensions are equal. */
	if (first->refreshRate != second->refreshRate)
		return VK_FALSE;

	/* Succeeded: the two descriptions identify the same display mode. */
	return VK_TRUE;
}

/* Withdraws a surface from instance cleanup before releasing native state. */
static void
wsi_surface_unlink(
	struct vulkan_surface *surface)
{
	struct vulkan_surface **link;

	/* Locates the exact published surface under the WSI lifetime serializer. */
	pthread_mutex_lock(&wsi_mutex);

	link = &wsi_surfaces;
	while (*link != NULL && *link != surface)
		link = &(*link)->next;

	/* Removal affects only the matching object, preserving every other instance. */
	if (*link == surface)
		*link = surface->next;

	pthread_mutex_unlock(&wsi_mutex);

	/* Succeeded: instance cleanup can no longer select this surface. */
	return;
}

/* Releases a withdrawn surface using its original allocation callbacks. */
static void
wsi_surface_free(
	struct vulkan_surface *surface)
{
	/* Some platforms retain native surface objects independently of swapchain leases. */
	if (surface->platform->destroy_surface != NULL)
		surface->platform->destroy_surface(surface);

	/* Generic child ownership ends before the object's saved allocator is invoked. */
	vulkan_object_unpublish(&surface->object);
	vulkan_object_free(&surface->object);

	/* Succeeded: both native and common surface ownership are released. */
	return;
}

/* Releases an instance-owned display after all its mode descriptions retire. */
static void
wsi_display_free(
	struct vulkan_display *display)
{
	struct vulkan_display_mode *mode;
	struct vulkan_display_mode *next;

	/* Modes have no public destroy operation and remain children of this display. */
	mode = display->modes;
	while (mode != NULL) {
		/* Saves traversal state before the mode's allocator releases its storage. */
		next = mode->next;
		vulkan_object_unpublish(&mode->object);
		vulkan_object_free(&mode->object);
		mode = next;
	}

	/* Display names and native identity retire only after the final mode. */
	vulkan_object_unpublish(&display->object);
	vulkan_object_free(&display->object);

	/* Succeeded: the instance no longer retains this native display identity. */
	return;
}
