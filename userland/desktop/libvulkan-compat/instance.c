/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The live instance ownership records and backend extension filtering.
 */

#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The live instances, inserted after creation and removed before destruction under compat_mutex. */
static struct compat_instance *compat_instances;

static VkResult instance_physicals(struct compat_instance *instance);
static VkResult instance_prepare(const VkInstanceCreateInfo *create, VkInstanceCreateInfo *rewritten, const char ***names, unsigned *enabled);
static void instance_functions(struct compat_instance *instance);

/*
 * Creates a backend instance and retains its physical-device ownership.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(
	const VkInstanceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkInstance *pInstance)
{
	struct compat_instance *instance;
	VkInstanceCreateInfo rewritten;
	const char **names;
	unsigned enabled;
	VkResult error;

	/* Requires the public creation inputs before using the backend. */
	if (pCreateInfo == NULL || pInstance == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Enters the creation boundary and initializes the process-wide backend. */
	compat_enter("vkCreateInstance");
	if (compat_backend.create_instance == NULL) {
		/* Leaves an unavailable driver without creating a foreign instance. */
		compat_leave();
		return VK_ERROR_INCOMPATIBLE_DRIVER;
	}

	/* Removes our WSI requests and adds supported backend query extensions. */
	names = NULL;
	enabled = 0;
	error = instance_prepare(pCreateInfo, &rewritten, &names, &enabled);
	if (error != VK_SUCCESS) {
		/* Leaves a refused extension request before allocating an instance owner. */
		compat_leave();
		return error;
	}

	/* Allocates the ownership record before creating the backend resource. */
	instance = calloc(1, sizeof(*instance));
	if (instance == NULL) {
		/* Leaves a refused allocation without invoking the backend. */
		free(names);
		compat_leave();
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Creates the instance through the directly resolved backend entry point. */
	error = compat_backend.create_instance(&rewritten, pAllocator, pInstance);

	/* Releases the request list after the backend has consumed it. */
	free(names);
	if (error != VK_SUCCESS) {
		/* Releases the unused record and the call boundary on failure. */
		free(instance);
		compat_leave();
		return error;
	}

	/* Remembers the API version without changing the caller's backend handle. */
	instance->handle = *pInstance;
	instance->api_version = VK_API_VERSION_1_0;
	instance->enabled = enabled;
	if (pCreateInfo->pApplicationInfo != NULL && pCreateInfo->pApplicationInfo->apiVersion != 0)
		instance->api_version = pCreateInfo->pApplicationInfo->apiVersion;

	/* Resolves the unmodified instance procedures used by our WSI. */
	instance_functions(instance);

	/* Records which physical devices belong to this backend instance. */
	error = instance_physicals(instance);
	if (error != VK_SUCCESS) {
		/* Unwinds the created instance before reporting a bookkeeping failure. */
		compat_backend.destroy_instance(*pInstance, pAllocator);
		free(instance->physicals);
		free(instance);
		*pInstance = VK_NULL_HANDLE;
		compat_leave();
		return error;
	}

	/* Publishes a fully initialized ownership record. */
	(void)pthread_mutex_lock(&compat_mutex);

	/* Makes the new instance discoverable only after its ownership is complete. */
	instance->next = compat_instances;
	compat_instances = instance;

	/* Ends list protection; callers retain the Vulkan handle lifetime themselves. */
	(void)pthread_mutex_unlock(&compat_mutex);

	/* Leaves the completed creation boundary. */
	compat_leave();

	/* Succeeded: the caller owns the backend instance and we retain its ownership record. */
	return VK_SUCCESS;
}

/*
 * Retires instance bookkeeping and destroys the backend instance.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(
	VkInstance instance,
	const VkAllocationCallbacks *pAllocator)
{
	struct compat_instance **link;
	struct compat_instance *record;

	/* Vulkan permits destruction of a null instance. */
	if (instance == VK_NULL_HANDLE)
		return;

	/* Enters the destruction boundary before any backend call. */
	compat_enter("vkDestroyInstance");

	/* Removes the ownership record while its backend handle is still live. */
	(void)pthread_mutex_lock(&compat_mutex);

	/* Begins the retirement search without a matched ownership record. */
	record = NULL;

	/* Locates the instance's link for removal. */
	for (link = &compat_instances; *link != NULL; link = &(*link)->next) {
		/* Retires only the matching backend handle. */
		if ((*link)->handle == instance) {
			record = *link;
			*link = record->next;
			break;
		}
	}

	/* Ends list protection; callers retain the Vulkan handle lifetime themselves. */
	(void)pthread_mutex_unlock(&compat_mutex);

	/* Destroys the resource through the backend's directly resolved interceptor. */
	if (compat_backend.destroy_instance == NULL)
		compat_missing("vkDestroyInstance");

	/* Releases the backend instance after its bookkeeping is no longer discoverable. */
	compat_backend.destroy_instance(instance, pAllocator);

	/* Releases physical-device membership with its parent record. */
	if (record != NULL) {
		free(record->physicals);
		free(record);
	}

	/* Leaves the completed destruction boundary. */
	compat_leave();

	/* Succeeded: the instance and its ownership record are retired. */
	return;
}

/*
 * Lists backend instance extensions without backend WSI extensions.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(
	const char *pLayerName,
	uint32_t *pPropertyCount,
	VkExtensionProperties *pProperties)
{
	VkResult error;

	/* Requires the enumeration count destination. */
	if (pPropertyCount == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Enters the backend enumeration boundary. */
	compat_enter("vkEnumerateInstanceExtensionProperties");

	/* Filters only driver extensions; named layer extensions retain their own meaning. */
	error = compat_extensions(VK_NULL_HANDLE, pLayerName, pPropertyCount, pProperties);

	/* Leaves the completed enumeration boundary. */
	compat_leave();

	/* Reports an incomplete caller array or a backend enumeration failure. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the caller has the available non-WSI extension list. */
	return VK_SUCCESS;
}

/*
 * Lists the backend's instance layers or an empty list without a backend.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(
	uint32_t *pPropertyCount,
	VkLayerProperties *pProperties)
{
	VkResult error;

	/* Requires the enumeration count destination. */
	if (pPropertyCount == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Enters the backend's global layer enumeration boundary. */
	compat_enter("vkEnumerateInstanceLayerProperties");
	if (compat_backend.instance_layers == NULL) {
		/* An absent backend provides no layers and is not an enumeration failure. */
		*pPropertyCount = 0;
		compat_leave();
		return VK_SUCCESS;
	}

	/* Preserves the backend's layer-specific enumeration contract. */
	error = compat_backend.instance_layers(pPropertyCount, pProperties);

	/* Leaves the completed layer enumeration boundary. */
	compat_leave();

	/* Preserves a backend error or an incomplete caller array. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the layer list is the backend's own list. */
	return VK_SUCCESS;
}

/*
 * Reports the backend's API version, falling back to Vulkan 1.0.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceVersion(
	uint32_t *pApiVersion)
{
	VkResult error;

	/* Requires a version destination. */
	if (pApiVersion == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Enters the backend's global version boundary. */
	compat_enter("vkEnumerateInstanceVersion");
	if (compat_backend.instance_version == NULL) {
		/* A Vulkan 1.0 or missing backend has no version entry point. */
		*pApiVersion = VK_API_VERSION_1_0;
		compat_leave();
		return VK_SUCCESS;
	}

	/* Preserves the backend's supported API version. */
	error = compat_backend.instance_version(pApiVersion);

	/* Leaves the completed version query. */
	compat_leave();

	/* Preserves a backend version-query error. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the caller knows the backend's supported version. */
	return VK_SUCCESS;
}

/*
 * Finds a live ownership record for an instance whose lifetime the caller holds.
 */
struct compat_instance *
compat_instance_get(
	VkInstance handle)
{
	struct compat_instance *instance;

	/* Searches the live ownership list while insertion and removal are excluded. */
	(void)pthread_mutex_lock(&compat_mutex);

	/* Finds the caller's backend instance handle. */
	for (instance = compat_instances; instance != NULL; instance = instance->next) {
		/* Stops at the requested owner. */
		if (instance->handle == handle)
			break;
	}

	/* Ends list protection; callers retain the Vulkan handle lifetime themselves. */
	(void)pthread_mutex_unlock(&compat_mutex);

	/* Succeeded: returns the live record, or NULL for an unknown handle. */
	return instance;
}

/*
 * Finds the instance owning a physical device without wrapping its dispatchable handle.
 */
struct compat_instance *
compat_instance_for_physical(
	VkPhysicalDevice physical)
{
	struct compat_instance *instance;
	struct compat_instance *owner;
	uint32_t index;

	/* Searches immutable physical-device membership under the live-list mutex. */
	(void)pthread_mutex_lock(&compat_mutex);

	/* Starts without a known physical-device owner. */
	owner = NULL;

	/* Inspects each still-live instance's physical-device membership. */
	for (instance = compat_instances; instance != NULL; instance = instance->next) {
		/* Finds the physical device in this instance's immutable list. */
		for (index = 0; index < instance->physical_count; index++) {
			/* Records the instance whose backend returned this device. */
			if (instance->physicals[index] == physical) {
				owner = instance;
				break;
			}
		}

		/* Stops once physical-device ownership is established. */
		if (owner != NULL)
			break;
	}

	/* Ends list protection; callers retain the Vulkan handle lifetime themselves. */
	(void)pthread_mutex_unlock(&compat_mutex);

	/* Succeeded: returns the live physical-device owner or NULL. */
	return owner;
}

/*
 * Copies backend extension lists while excluding foreign WSI responsibilities.
 */
VkResult
compat_extensions(
	VkPhysicalDevice physical,
	const char *layer,
	uint32_t *count,
	VkExtensionProperties *properties)
{
	VkExtensionProperties *available;
	VkExtensionProperties owned[5];
	uint32_t total;
	uint32_t index;
	uint32_t kept;
	uint32_t capacity;
	uint32_t own_count;
	VkResult error;
	int wsi;

	/* A missing backend offers no WSI or driver extensions. */
	if (compat_backend.handle == NULL) {
		*count = 0;
		return VK_SUCCESS;
	}

	/* Named layers retain their exact backend enumeration contract. */
	if (layer != NULL) {
		/* Selects the matching backend layer extension query. */
		if (physical == VK_NULL_HANDLE) {
			error = compat_backend.instance_extensions(layer, count, properties);
		} else {
			error = compat_backend.device_extensions(physical, layer, count, properties);
		}

		/* Preserves the backend's layer query status. */
		if (error != VK_SUCCESS)
			return error;

		/* Succeeded: the named layer's extension list is unchanged. */
		return VK_SUCCESS;
	}

	/* Obtains a complete temporary backend list before filtering its WSI. */
	error = compat_backend_extensions(physical, NULL, &total, &available);
	if (error != VK_SUCCESS)
		return error;

	/* Initializes our small appended list independently of backend storage. */
	memset(owned, 0, sizeof(owned));
	own_count = 0;
	if (physical == VK_NULL_HANDLE) {
		/* Advertises our own instance surface responsibilities. */
		(void)snprintf(owned[0].extensionName, sizeof(owned[0].extensionName), "%s", VK_KHR_SURFACE_EXTENSION_NAME);
		owned[0].specVersion = VK_KHR_SURFACE_SPEC_VERSION;
		(void)snprintf(owned[1].extensionName, sizeof(owned[1].extensionName), "%s", VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
		owned[1].specVersion = VK_KHR_WAYLAND_SURFACE_SPEC_VERSION;
		/* Display inquiry and explicit DRM acquisition are implemented by our portable KMS path. */
		(void)snprintf(owned[2].extensionName, sizeof(owned[2].extensionName), "%s", VK_KHR_DISPLAY_EXTENSION_NAME);
		owned[2].specVersion = VK_KHR_DISPLAY_SPEC_VERSION;
		(void)snprintf(owned[3].extensionName, sizeof(owned[3].extensionName), "%s", VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME);
		owned[3].specVersion = VK_EXT_DIRECT_MODE_DISPLAY_SPEC_VERSION;
		(void)snprintf(owned[4].extensionName, sizeof(owned[4].extensionName), "%s", VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME);
		owned[4].specVersion = VK_EXT_ACQUIRE_DRM_DISPLAY_SPEC_VERSION;
		own_count = 5;
	} else {
		/* Offers a swapchain for actual image export or a connected portable KMS display. */
		wsi = compat_swapchain_available(physical);
		if (wsi != 0) {
			/* Advertises the swapchain implemented by this library. */
			(void)snprintf(owned[0].extensionName, sizeof(owned[0].extensionName), "%s", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
			owned[0].specVersion = VK_KHR_SWAPCHAIN_SPEC_VERSION;
			own_count = 1;
		}
	}

	/* Keeps backend order and bounds each copied property by the caller's capacity. */
	capacity = 0;
	if (properties != NULL)
		capacity = *count;

	/* Copies only extensions whose handles do not belong to the backend's WSI. */
	kept = 0;
	for (index = 0; index < total; index++) {
		/* Excludes backend surface, display and presentation ownership. */
		wsi = compat_wsi_extension(available[index].extensionName);
		if (wsi != 0)
			continue;

		/* Writes only caller-owned slots. */
		if (properties != NULL && kept < capacity)
			properties[kept] = available[index];

		/* Counts retained entries independently of caller truncation. */
		kept++;
	}

	/* Appends each supported WSI extension after the non-WSI backend list. */
	for (index = 0; index < own_count; index++) {
		/* Writes an owned extension when the caller provided enough space. */
		if (properties != NULL && kept < capacity)
			properties[kept] = owned[index];

		/* Counts the appended WSI extension in the public enumeration. */
		kept++;
	}

	/* Releases the temporary backend list after both passes. */
	free(available);

	/* Reports the full count when the caller asked only for a measurement. */
	if (properties == NULL) {
		*count = kept;
		return VK_SUCCESS;
	}

	/* Reports the number written when the caller's array was too small. */
	if (kept > capacity) {
		*count = capacity;
		return VK_INCOMPLETE;
	}

	/* Succeeded: every retained driver and owned WSI property fits. */
	*count = kept;
	return VK_SUCCESS;
}

/*
 * Retrieves a complete backend extension list without applying public WSI filtering.
 */
VkResult
compat_backend_extensions(
	VkPhysicalDevice physical,
	const char *layer,
	uint32_t *count,
	VkExtensionProperties **properties)
{
	VkExtensionProperties *available;
	uint32_t total;
	VkResult error;

	/* Initializes failure outputs before selecting the direct backend query. */
	*properties = NULL;
	*count = 0;
	total = 0;
	if (physical == VK_NULL_HANDLE) {
		/* A missing global enumerator has a valid empty list. */
		if (compat_backend.instance_extensions == NULL)
			return VK_SUCCESS;

		/* Measures instance-level backend extensions. */
		error = compat_backend.instance_extensions(layer, &total, NULL);
	} else {
		/* Requires a physical-device enumerator for a device query. */
		if (compat_backend.device_extensions == NULL)
			return VK_ERROR_INITIALIZATION_FAILED;

		/* Measures physical-device backend extensions. */
		error = compat_backend.device_extensions(physical, layer, &total, NULL);
	}

	/* Does not allocate an unmeasured backend list. */
	if (error != VK_SUCCESS)
		return error;

	/* Allocates independent temporary storage including the empty-list case. */
	available = calloc((size_t)total + 1, sizeof(*available));
	if (available == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Fills the selected backend's measured list. */
	if (physical == VK_NULL_HANDLE) {
		error = compat_backend.instance_extensions(layer, &total, available);
	} else {
		error = compat_backend.device_extensions(physical, layer, &total, available);
	}

	/* Releases a failed or unstable enumeration before returning its status. */
	if (error != VK_SUCCESS) {
		free(available);
		return error;
	}

	/* Transfers ownership of the unfiltered list to the caller. */
	*properties = available;
	*count = total;

	/* Succeeded: the caller owns all measured backend properties. */
	return VK_SUCCESS;
}

/*
 * Finds one exact extension name in a measured backend list.
 */
int
compat_extension_has(
	const VkExtensionProperties *properties,
	uint32_t count,
	const char *name)
{
	uint32_t index;
	int differs;

	/* Looks for the requested extension without treating a substring as support. */
	for (index = 0; index < count; index++) {
		/* Compares this measured backend extension's complete name. */
		differs = strcmp(properties[index].extensionName, name);
		if (differs == 0)
			return 1;
	}

	/* Reports that this exact extension is absent. */
	return 0;
}

/*
 * Identifies the instance WSI extensions implemented by this library.
 */
unsigned
compat_instance_extension(
	const char *name)
{
	int differs;

	/* Owns the generic surface interface. */
	differs = strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME);
	if (differs == 0)
		return COMPAT_INSTANCE_SURFACE;

	/* Owns the Keiland Wayland surface interface. */
	differs = strcmp(name, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
	if (differs == 0)
		return COMPAT_INSTANCE_WAYLAND;

	/* Owns KMS mode and primary-plane enumeration. */
	differs = strcmp(name, VK_KHR_DISPLAY_EXTENSION_NAME);
	if (differs == 0)
		return COMPAT_INSTANCE_DISPLAY;

	/* Owns explicit release of a directly acquired display. */
	differs = strcmp(name, VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME);
	if (differs == 0)
		return COMPAT_INSTANCE_DIRECT;

	/* Owns duplication of the compositor seat's DRM master descriptor. */
	differs = strcmp(name, VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME);
	if (differs == 0)
		return COMPAT_INSTANCE_DRM;

	/* Reports that this extension is not part of our current WSI. */
	return 0;
}

/* Populates immutable physical-device membership after successful instance creation. */
static VkResult
instance_physicals(
	struct compat_instance *instance)
{
	PFN_vkEnumeratePhysicalDevices enumerate;
	VkResult error;

	/* Resolves an unmodified instance procedure from the backend's own resolver. */
	enumerate = (PFN_vkEnumeratePhysicalDevices)compat_backend.get_instance_proc(instance->handle, "vkEnumeratePhysicalDevices");
	if (enumerate == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Measures the physical-device membership. */
	error = enumerate(instance->handle, &instance->physical_count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Allocates membership storage even for a valid zero-device instance. */
	instance->physicals = calloc((size_t)instance->physical_count + 1, sizeof(*instance->physicals));
	if (instance->physicals == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Fills the instance's ownership list before publishing it. */
	error = enumerate(instance->handle, &instance->physical_count, instance->physicals);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: physical-device handles retain their instance owner. */
	return VK_SUCCESS;
}

/* Resolves instance queries using the enabled API version's core or extension spelling. */
static void
instance_functions(
	struct compat_instance *instance)
{
	/* Resolves the backend vkGetPhysicalDeviceProperties query. */
	instance->properties = (PFN_vkGetPhysicalDeviceProperties)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceProperties");

	/* Chooses the enabled version of vkGetPhysicalDeviceFormatProperties2 rather than an unavailable promoted core entry. */
	if (instance->api_version < VK_API_VERSION_1_1) {
		instance->format_properties = (PFN_vkGetPhysicalDeviceFormatProperties2)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceFormatProperties2KHR");
	} else {
		instance->format_properties = (PFN_vkGetPhysicalDeviceFormatProperties2)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceFormatProperties2");
	}

	/* Chooses the enabled version of vkGetPhysicalDeviceImageFormatProperties2 rather than an unavailable promoted core entry. */
	if (instance->api_version < VK_API_VERSION_1_1) {
		instance->image_properties = (PFN_vkGetPhysicalDeviceImageFormatProperties2)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceImageFormatProperties2KHR");
	} else {
		instance->image_properties = (PFN_vkGetPhysicalDeviceImageFormatProperties2)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceImageFormatProperties2");
	}

	/* Resolves the backend vkGetPhysicalDeviceMemoryProperties query. */
	instance->memory_properties = (PFN_vkGetPhysicalDeviceMemoryProperties)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceMemoryProperties");

	/* Resolves the backend vkGetPhysicalDeviceQueueFamilyProperties query. */
	instance->queue_properties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceQueueFamilyProperties");

	/* Chooses the enabled version of vkGetPhysicalDeviceExternalSemaphoreProperties rather than an unavailable promoted core entry. */
	if (instance->api_version < VK_API_VERSION_1_1) {
		instance->semaphore_properties = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");
	} else {
		instance->semaphore_properties = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceExternalSemaphoreProperties");
	}

	/* Chooses the enabled version of vkGetPhysicalDeviceExternalFenceProperties rather than an unavailable promoted core entry. */
	if (instance->api_version < VK_API_VERSION_1_1) {
		instance->fence_properties = (PFN_vkGetPhysicalDeviceExternalFenceProperties)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceExternalFencePropertiesKHR");
	} else {
		instance->fence_properties = (PFN_vkGetPhysicalDeviceExternalFenceProperties)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceExternalFenceProperties");
	}

	/* Succeeded: the instance owns its backend query table. */
	return;
}

/* Prepares a copied instance request without exposing our WSI to the backend. */
static VkResult
instance_prepare(
	const VkInstanceCreateInfo *create,
	VkInstanceCreateInfo *rewritten,
	const char ***names,
	unsigned *enabled)
{
	VkExtensionProperties *available;
	const char **selected;
	const char *name;
	const char *internal[4] = {
	    "VK_KHR_get_physical_device_properties2",
	    "VK_KHR_external_memory_capabilities",
	    "VK_KHR_external_semaphore_capabilities",
	    "VK_KHR_external_fence_capabilities"};
	uint32_t available_count;
	uint32_t count;
	uint32_t index;
	uint32_t check;
	uint32_t version;
	unsigned own;
	VkResult error;
	int wsi;
	int supported;
	int differs;
	int duplicate;

	/* Measures supported backend queries independently of the rewritten public list. */
	error = compat_backend_extensions(VK_NULL_HANDLE, NULL, &available_count, &available);
	if (error != VK_SUCCESS)
		return error;

	/* Allocates a request list with space for each internal Vulkan 1.0 query dependency. */
	selected = calloc((size_t)create->enabledExtensionCount + 4, sizeof(*selected));
	if (selected == NULL) {
		/* Releases the capability list after a failed request allocation. */
		free(available);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Retains ordinary backend requests and records our own enabled WSI extensions. */
	count = 0;
	for (index = 0; index < create->enabledExtensionCount; index++) {
		/* Classifies the requested instance extension by ownership. */
		name = create->ppEnabledExtensionNames[index];
		own = compat_instance_extension(name);
		if (own != 0) {
			*enabled |= own;
			continue;
		}

		/* Refuses a backend WSI extension we do not implement. */
		wsi = compat_wsi_extension(name);
		if (wsi != 0) {
			free(selected);
			free(available);
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		}

		/* Retains the caller's unrelated backend extension request. */
		selected[count] = name;
		count++;
	}

	/* Selects the app's requested core version before adding promoted query extensions. */
	version = VK_API_VERSION_1_0;
	if (create->pApplicationInfo != NULL && create->pApplicationInfo->apiVersion != 0)
		version = create->pApplicationInfo->apiVersion;

	/* Enables supported Vulkan 1.1 queries as extensions for a Vulkan 1.0 app. */
	if (version < VK_API_VERSION_1_1) {
		/* Adds supported internal instance queries once each. */
		for (index = 0; index < 4; index++) {
			/* Skips a query extension absent from this backend. */
			supported = compat_extension_has(available, available_count, internal[index]);
			if (supported == 0)
				continue;

			/* Avoids duplicating an explicit app request for the same query. */
			duplicate = 0;
			for (check = 0; check < count; check++) {
				/* Records an already selected query dependency. */
				differs = strcmp(selected[check], internal[index]);
				if (differs == 0) {
					duplicate = 1;
					break;
				}
			}

			/* Appends the supported dependency only when it was not requested already. */
			if (duplicate == 0) {
				selected[count] = internal[index];
				count++;
			}
		}
	}

	/* Releases measured capabilities after selecting the request's dependencies. */
	free(available);

	/* Copies the public creation request and changes only its extension list. */
	*rewritten = *create;
	rewritten->enabledExtensionCount = count;
	rewritten->ppEnabledExtensionNames = selected;
	*names = selected;

	/* Succeeded: the caller owns the temporary backend request list. */
	return VK_SUCCESS;
}
