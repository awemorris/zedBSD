/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The live backend devices and their queue ownership records.
 */

#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The live device records, published and retired under compat_mutex. */
static struct compat_device *compat_devices;

/* The queues retrieved from live devices, removed with their device under compat_mutex. */
static struct compat_queue *compat_queues;

static void device_queue_record(VkDevice device, VkQueue queue, uint32_t family);
static VkBool32 device_modifier_supported(struct compat_instance *instance, VkPhysicalDevice physical, VkFormat format);
static VkResult device_prepare(struct compat_instance *instance, VkPhysicalDevice physical, const VkDeviceCreateInfo *create, VkDeviceCreateInfo *rewritten, const char ***names, struct compat_capabilities *capabilities, unsigned *swapchain);
static void device_functions(struct compat_device *device);

/*
 * Creates a backend device while retaining its instance and physical-device ownership.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(
	VkPhysicalDevice physicalDevice,
	const VkDeviceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkDevice *pDevice)
{
	struct compat_device *device;
	struct compat_instance *instance;
	PFN_vkGetPhysicalDeviceProperties get_properties;
	VkPhysicalDeviceProperties properties;
	VkResult error;
	VkDeviceCreateInfo rewritten;
	const char **names;
	struct compat_capabilities capabilities;
	unsigned swapchain;
	VkQueue initial_queue;

	/* Requires valid public creation inputs. */
	if (pCreateInfo == NULL || pDevice == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Enters the device boundary through directly resolved backend calls. */
	compat_enter("vkCreateDevice");
	if (compat_backend.create_device == NULL) {
		/* Leaves a refused device creation without a backend entry point. */
		compat_leave();
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Requires the still-live instance whose backend returned the physical device. */
	instance = compat_instance_for_physical(physicalDevice);
	if (instance == NULL) {
		/* Leaves an unknown physical device outside the bookkeeping boundary. */
		compat_leave();
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Separates application WSI requests from the backend's required image extensions. */
	error = device_prepare(instance, physicalDevice, pCreateInfo, &rewritten, &names, &capabilities, &swapchain);
	if (error != VK_SUCCESS) {
		/* Leaves rejected extensions without creating a device. */
		compat_leave();
		return error;
	}

	/* Allocates ownership storage before creating a backend resource. */
	device = calloc(1, sizeof(*device));
	if (device == NULL) {
		/* Leaves a refused bookkeeping allocation. */
		free(names);
		compat_leave();
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Queries the physical API version through an unmodified backend procedure. */
	get_properties = (PFN_vkGetPhysicalDeviceProperties)compat_backend.get_instance_proc(instance->handle, "vkGetPhysicalDeviceProperties");
	if (get_properties == NULL) {
		/* Releases the unused record before refusing an incomplete backend. */
		free(names);
		free(device);
		compat_leave();
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Reads the device's capabilities without changing its backend handle. */
	get_properties(physicalDevice, &properties);

	/* Creates the device through its directly resolved interceptor entry point. */
	error = compat_backend.create_device(physicalDevice, &rewritten, pAllocator, pDevice);
	free(names);
	if (error != VK_SUCCESS) {
		/* Releases the unused record and the active call on backend failure. */
		free(device);
		compat_leave();
		return error;
	}

	/* Retains the effective API version and the device's unchanged backend handle. */
	device->handle = *pDevice;
	device->physical = physicalDevice;
	device->instance = instance;
	device->api_version = instance->api_version;
	if (properties.apiVersion < device->api_version)
		device->api_version = properties.apiVersion;

	/* Retains the application's WSI choice and resolves backend-only image operations. */
	device->swapchain = swapchain;
	device->path = capabilities.path;
	device->implicit_sync = capabilities.implicit_sync;
	device->foreign = capabilities.foreign;
	device_functions(device);

	/* Publishes the initialized device record under the ownership mutex. */
	(void)pthread_mutex_lock(&compat_mutex);

	/* Makes the initialized device discoverable by queue and presentation calls. */
	device->next = compat_devices;
	compat_devices = device;

	/* Ends list protection; callers retain the Vulkan handle lifetime themselves. */
	(void)pthread_mutex_unlock(&compat_mutex);

	/* Records one ordinary created queue even if the application acquires before retrieving it. */
	if (pCreateInfo->queueCreateInfoCount != 0) {
		/* Flagged queues require vkGetDeviceQueue2 and remain recorded on application retrieval. */
		if (pCreateInfo->pQueueCreateInfos[0].flags == 0) {
			compat_backend.get_queue(*pDevice, pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex, 0, &initial_queue);
			device_queue_record(*pDevice, initial_queue, pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex);
		}
	}

	/* Leaves the completed device creation boundary. */
	compat_leave();

	/* Succeeded: the caller owns the backend device with retained ownership metadata. */
	return VK_SUCCESS;
}

/*
 * Retires device and queue ownership records before destroying the backend device.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(
	VkDevice device,
	const VkAllocationCallbacks *pAllocator)
{
	struct compat_device **link;
	struct compat_device *record;
	struct compat_queue **queue_link;
	struct compat_queue *queue;

	/* Vulkan permits destruction of a null device. */
	if (device == VK_NULL_HANDLE)
		return;

	/* Enters the device destruction boundary. */
	compat_enter("vkDestroyDevice");

	/* Removes the device and its queues atomically from the live ownership lists. */
	(void)pthread_mutex_lock(&compat_mutex);

	/* Begins the retirement search without a matched ownership record. */
	record = NULL;

	/* Finds the device record to retire. */
	for (link = &compat_devices; *link != NULL; link = &(*link)->next) {
		/* Removes only the caller's device. */
		if ((*link)->handle == device) {
			record = *link;
			*link = record->next;
			break;
		}
	}

	/* Retires every cached queue whose parent device is being destroyed. */
	queue_link = &compat_queues;
	while (*queue_link != NULL) {
		/* Removes matching queues while preserving the next list link. */
		queue = *queue_link;
		if (queue->device == record) {
			*queue_link = queue->next;
			free(queue);
		} else {
			queue_link = &queue->next;
		}
	}

	/* Ends list protection; callers retain the Vulkan handle lifetime themselves. */
	(void)pthread_mutex_unlock(&compat_mutex);

	/* Requires the directly resolved backend destruction function. */
	if (compat_backend.destroy_device == NULL)
		compat_missing("vkDestroyDevice");

	/* Destroys the backend resource after its records are no longer discoverable. */
	compat_backend.destroy_device(device, pAllocator);

	/* Releases the retired parent ownership record. */
	free(record);

	/* Leaves the completed destruction boundary. */
	compat_leave();

	/* Succeeded: the device and every retrieved queue record are retired. */
	return;
}

/*
 * Lists physical-device extensions without backend presentation extensions.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(
	VkPhysicalDevice physicalDevice,
	const char *pLayerName,
	uint32_t *pPropertyCount,
	VkExtensionProperties *pProperties)
{
	VkResult error;

	/* Requires the caller's enumeration count. */
	if (pPropertyCount == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Enters the physical-device extension enumeration boundary. */
	compat_enter("vkEnumerateDeviceExtensionProperties");

	/* Filters backend-owned WSI while preserving unrelated device extensions. */
	error = compat_extensions(physicalDevice, pLayerName, pPropertyCount, pProperties);

	/* Leaves the completed physical-device query. */
	compat_leave();

	/* Preserves a backend error or an incomplete caller array. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the caller holds the available non-WSI device extensions. */
	return VK_SUCCESS;
}

/*
 * Retrieves a backend queue and retains its device ownership.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue(
	VkDevice device,
	uint32_t queueFamilyIndex,
	uint32_t queueIndex,
	VkQueue *pQueue)
{
	/* Enters the queue retrieval boundary through a directly resolved backend entry. */
	compat_enter("vkGetDeviceQueue");
	if (compat_backend.get_queue == NULL)
		compat_missing("vkGetDeviceQueue");

	/* Retrieves the unchanged backend queue handle. */
	compat_backend.get_queue(device, queueFamilyIndex, queueIndex, pQueue);

	/* Retains the queue's parent for later presentation calls. */
	device_queue_record(device, *pQueue, queueFamilyIndex);

	/* Leaves the completed queue retrieval boundary. */
	compat_leave();

	/* Succeeded: the caller holds the backend queue and its ownership is recorded. */
	return;
}

/*
 * Retrieves a Vulkan 1.1 queue and retains its device ownership.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue2(
	VkDevice device,
	const VkDeviceQueueInfo2 *pQueueInfo,
	VkQueue *pQueue)
{
	/* Enters the queue retrieval boundary through the backend's direct entry point. */
	compat_enter("vkGetDeviceQueue2");
	if (compat_backend.get_queue2 == NULL)
		compat_missing("vkGetDeviceQueue2");

	/* Preserves the backend's queue retrieval semantics. */
	compat_backend.get_queue2(device, pQueueInfo, pQueue);

	/* Records ownership for a queue successfully returned by the backend. */
	if (*pQueue != VK_NULL_HANDLE)
		device_queue_record(device, *pQueue, pQueueInfo->queueFamilyIndex);

	/* Leaves the completed queue retrieval boundary. */
	compat_leave();

	/* Succeeded: the backend queue result and ownership are preserved. */
	return;
}

/*
 * Finds a device record whose handle lifetime the caller already holds.
 */
struct compat_device *
compat_device_get(
	VkDevice handle)
{
	struct compat_device *device;

	/* Searches while device insertion and retirement are excluded. */
	(void)pthread_mutex_lock(&compat_mutex);

	/* Finds the caller's unchanged backend device handle. */
	for (device = compat_devices; device != NULL; device = device->next) {
		/* Stops at the requested live owner. */
		if (device->handle == handle)
			break;
	}

	/* Ends list protection; callers retain the Vulkan handle lifetime themselves. */
	(void)pthread_mutex_unlock(&compat_mutex);

	/* Succeeded: returns the live owner or NULL for an unknown device. */
	return device;
}

/*
 * Finds a queue record whose parent device lifetime the caller already holds.
 */
struct compat_queue *
compat_queue_get(
	VkQueue handle)
{
	struct compat_queue *queue;

	/* Searches while queue publication and parent-device retirement are excluded. */
	(void)pthread_mutex_lock(&compat_mutex);

	/* Finds the retrieved backend queue handle. */
	for (queue = compat_queues; queue != NULL; queue = queue->next) {
		/* Stops at the requested live queue. */
		if (queue->handle == handle)
			break;
	}

	/* Ends list protection; callers retain the Vulkan handle lifetime themselves. */
	(void)pthread_mutex_unlock(&compat_mutex);

	/* Succeeded: returns the queue owner or NULL when the queue was not retrieved. */
	return queue;
}

/*
 * Returns an already retrieved queue belonging to this still-live device.
 */
struct compat_queue *
compat_device_queue(
	struct compat_device *device)
{
	struct compat_queue *queue;
	struct compat_queue *found;

	/* Serializes access to the queue ownership list. */
	(void)pthread_mutex_lock(&compat_mutex);

	/* Searches only queues whose device lifetime covers this caller. */
	found = NULL;
	for (queue = compat_queues; queue != NULL; queue = queue->next) {
		/* Returns the first application-retrieved queue for initial acquire synchronization. */
		if (queue->device == device) {
			found = queue;
			break;
		}
	}

	/* Ends list protection before invoking any backend operation. */
	(void)pthread_mutex_unlock(&compat_mutex);

	/* Vulkan callers keep the parent device live while using the returned record. */
	return found;
}

/*
 * Queries actual DMA-BUF export and SYNC_FD support before advertising a swapchain.
 */
void
compat_physical_capabilities(
	VkPhysicalDevice physical,
	struct compat_capabilities *capabilities)
{
	struct compat_instance *instance;
	VkExtensionProperties *extensions;
	VkPhysicalDeviceExternalSemaphoreInfo semaphore_info;
	VkExternalSemaphoreProperties semaphore_properties;
	VkPhysicalDeviceExternalFenceInfo fence_info;
	VkExternalFenceProperties fence_properties;
	VkResult error;
	VkBool32 supported;
	uint32_t count;
	int present;

	/* Starts without an export path or synchronization promise. */
	memset(capabilities, 0, sizeof(*capabilities));

	/* Requires the live instance's enabled physical-device query functions. */
	instance = compat_instance_for_physical(physical);
	if (instance == NULL || instance->image_properties == NULL)
		return;

	/* Measures actual backend device extensions without public WSI filtering. */
	error = compat_backend_extensions(physical, NULL, &count, &extensions);
	if (error != VK_SUCCESS)
		return;

	/* A DMA-BUF export path requires the external memory and fd interfaces. */
	present = compat_extension_has(extensions, count, "VK_EXT_external_memory_dma_buf");
	if (present == 0) {
		free(extensions);
		return;
	}

	/* Requires the operation that exports allocated memory as a descriptor. */
	present = compat_extension_has(extensions, count, "VK_KHR_external_memory_fd");
	if (present == 0) {
		free(extensions);
		return;
	}

	/* Prefers explicit modifiers when a one-plane colour image is actually exportable. */
	present = compat_extension_has(extensions, count, "VK_EXT_image_drm_format_modifier");
	if (present != 0 && instance->format_properties != NULL) {
		/* Checks the backend's actual modifier list for the desktop's primary colour format. */
		supported = device_modifier_supported(instance, physical, VK_FORMAT_B8G8R8A8_UNORM);
		if (supported != VK_FALSE)
			capabilities->path = COMPAT_WSI_MODIFIER;
	}

	/* Falls back to an exportable linear colour image when no modifier path works. */
	if (capabilities->path == COMPAT_WSI_NONE) {
		/* Queries real external image export support rather than trusting extension names. */
		supported = compat_image_supported(physical, VK_FORMAT_B8G8R8A8_UNORM, COMPAT_WSI_LINEAR, 0);
		if (supported != VK_FALSE)
			capabilities->path = COMPAT_WSI_LINEAR;
	}

	/* Retains whether the backend supports foreign queue-family ownership transfers. */
	present = compat_extension_has(extensions, count, "VK_EXT_queue_family_foreign");
	if (present != 0)
		capabilities->foreign = 1;

	/* Semaphore fd import/export must be present before its capability query is meaningful. */
	present = compat_extension_has(extensions, count, "VK_KHR_external_semaphore_fd");
	if (present == 0 || instance->semaphore_properties == NULL) {
		free(extensions);
		return;
	}

	/* Fence fd import must also be present for acquire's optional fence result. */
	present = compat_extension_has(extensions, count, "VK_KHR_external_fence_fd");
	if (present == 0 || instance->fence_properties == NULL) {
		free(extensions);
		return;
	}

	/* Queries SYNC_FD semaphore import and export together. */
	memset(&semaphore_info, 0, sizeof(semaphore_info));
	semaphore_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
	semaphore_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

	/* Initializes the external semaphore property destination. */
	memset(&semaphore_properties, 0, sizeof(semaphore_properties));
	semaphore_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
	instance->semaphore_properties(physical, &semaphore_info, &semaphore_properties);

	/* Queries SYNC_FD fence import for the caller's acquire fence. */
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO;
	fence_info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

	/* Initializes the external fence property destination. */
	memset(&fence_properties, 0, sizeof(fence_properties));
	fence_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES;
	instance->fence_properties(physical, &fence_info, &fence_properties);

	/* Enables implicit sync only when every required operation is supported. */
	if ((semaphore_properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0 &&
	    (semaphore_properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0 &&
	    (fence_properties.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT) != 0)
		capabilities->implicit_sync = 1;

	/* Releases the capability list after retaining its selected path. */
	free(extensions);

	/* Succeeded: the caller holds the physical device's actual WSI capabilities. */
	return;
}

/*
 * Queries exportability of one image format, tiling and modifier combination.
 */
VkBool32
compat_image_supported(
	VkPhysicalDevice physical,
	VkFormat format,
	unsigned path,
	uint64_t modifier)
{
	struct compat_instance *instance;
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info;
	VkPhysicalDeviceExternalImageFormatInfo external_info;
	VkPhysicalDeviceImageFormatInfo2 image_info;
	VkExternalImageFormatProperties external_properties;
	VkImageFormatProperties2 image_properties;
	VkResult error;

	/* Requires an enabled image-format query belonging to the physical device's instance. */
	instance = compat_instance_for_physical(physical);
	if (instance == NULL || instance->image_properties == NULL)
		return VK_FALSE;

	/* Describes the selected modifier without passing an unselected platform handle. */
	memset(&modifier_info, 0, sizeof(modifier_info));
	modifier_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
	modifier_info.drmFormatModifier = modifier;
	modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	/* Requests DMA-BUF export capability for this actual image combination. */
	memset(&external_info, 0, sizeof(external_info));
	external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
	external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	if (path == COMPAT_WSI_MODIFIER)
		external_info.pNext = &modifier_info;

	/* Queries all usage bits offered by the surface capabilities. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
	image_info.pNext = &external_info;
	image_info.format = format;
	image_info.type = VK_IMAGE_TYPE_2D;
	image_info.tiling = VK_IMAGE_TILING_LINEAR;
	image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (path == COMPAT_WSI_MODIFIER)
		image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

	/* Receives external memory features alongside ordinary image limits. */
	memset(&external_properties, 0, sizeof(external_properties));
	external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;

	/* Connects the property destination to the external-memory feature record. */
	memset(&image_properties, 0, sizeof(image_properties));
	image_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	image_properties.pNext = &external_properties;

	/* Queries the backend's actual image creation/export support. */
	error = instance->image_properties(physical, &image_info, &image_properties);
	if (error != VK_SUCCESS)
		return VK_FALSE;

	/* A supported ordinary image is insufficient when its memory cannot be exported. */
	if ((external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0)
		return VK_FALSE;

	/* Succeeded: this combination can be used for an exported swapchain image. */
	return VK_TRUE;
}

/*
 * Offers swapchains when either exported Wayland images or a portable KMS copy path is available.
 */
int
compat_swapchain_available(
	VkPhysicalDevice physical)
{
	struct compat_capabilities capabilities;
	struct compat_display *displays;
	uint32_t count;
	VkResult error;

	/* An actual DMA-BUF export path needs no display-card inquiry. */
	compat_physical_capabilities(physical, &capabilities);
	if (capabilities.path != COMPAT_WSI_NONE)
		return 1;

	/* KMS copying uses ordinary Vulkan images even when external-memory export is absent. */
	error = compat_kms_displays(&displays, &count);
	if (error != VK_SUCCESS)
		return 0;

	/* A headless or explicitly disabled display card cannot supply a KMS swapchain. */
	if (count == 0)
		return 0;

	/* Succeeded: the display copy path supplies swapchain presentation for this rendering device. */
	return 1;
}

/* Records one retrieved queue without duplicating an existing handle's ownership. */
static void
device_queue_record(
	VkDevice handle,
	VkQueue queue_handle,
	uint32_t family)
{
	struct compat_device *device;
	struct compat_queue *queue;

	/* Resolves the live parent before publishing any queue metadata. */
	device = compat_device_get(handle);
	if (device == NULL)
		return;

	/* Finds or creates the single queue record under the ownership mutex. */
	(void)pthread_mutex_lock(&compat_mutex);

	/* Checks whether an earlier retrieval already recorded the queue. */
	for (queue = compat_queues; queue != NULL; queue = queue->next) {
		/* Stops at the existing queue record. */
		if (queue->handle == queue_handle)
			break;
	}

	/* Allocates a queue record only for a new retrieved handle. */
	if (queue == NULL) {
		/* Keeps queue ownership available to later presentation calls. */
		queue = calloc(1, sizeof(*queue));
		if (queue == NULL) {
			/* Releases the mutex before terminating an unreportable void-API allocation failure. */
			(void)pthread_mutex_unlock(&compat_mutex);
			(void)fputs("libvulkan-compat: no memory for queue ownership\n", stderr);
			abort();
		}

		/* Publishes one fully initialized queue ownership record. */
		queue->handle = queue_handle;
		queue->device = device;
		queue->family = family;
		queue->next = compat_queues;
		compat_queues = queue;
	}

	/* Ends list protection; callers retain the Vulkan handle lifetime themselves. */
	(void)pthread_mutex_unlock(&compat_mutex);

	/* Succeeded: the retrieved queue retains its parent device. */
	return;
}

/* Finds one exportable single-plane colour modifier from the backend's v1 list. */
static VkBool32
device_modifier_supported(
	struct compat_instance *instance,
	VkPhysicalDevice physical,
	VkFormat format)
{
	VkDrmFormatModifierPropertiesListEXT list;
	VkFormatProperties2 properties;
	VkDrmFormatModifierPropertiesEXT *modifiers;
	VkBool32 supported;
	uint32_t count;
	uint32_t index;

	/* Measures the backend's v1 modifier list rather than the unsupported lavapipe v2 list. */
	memset(&list, 0, sizeof(list));
	list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;

	/* Connects the measured list to the physical format query. */
	memset(&properties, 0, sizeof(properties));
	properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
	properties.pNext = &list;
	instance->format_properties(physical, format, &properties);

	/* Allocates exactly the measured modifier list. */
	count = list.drmFormatModifierCount;
	modifiers = calloc((size_t)count + 1, sizeof(*modifiers));
	if (modifiers == NULL)
		return VK_FALSE;

	/* Fills the measured list before examining its plane and colour features. */
	list.pDrmFormatModifierProperties = modifiers;
	instance->format_properties(physical, format, &properties);

	/* Bounds the search by the storage allocated from the first query. */
	if (list.drmFormatModifierCount < count)
		count = list.drmFormatModifierCount;

	/* Searches for actual export support among single-plane colour modifiers. */
	supported = VK_FALSE;
	for (index = 0; index < count; index++) {
		/* The current WSI handles one image plane. */
		if (modifiers[index].drmFormatModifierPlaneCount != 1)
			continue;

		/* A swapchain modifier must permit colour attachment rendering. */
		if ((modifiers[index].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0)
			continue;

		/* Confirms that the actual modifier can export its external image memory. */
		supported = compat_image_supported(physical, format, COMPAT_WSI_MODIFIER, modifiers[index].drmFormatModifier);
		if (supported != VK_FALSE)
			break;
	}

	/* Releases the temporary list after selecting the physical export path. */
	free(modifiers);

	/* Reports that no exportable colour modifier exists. */
	if (supported == VK_FALSE)
		return VK_FALSE;

	/* Succeeded: at least one one-plane modifier supports exported colour images. */
	return VK_TRUE;
}

/*
 * Builds the private backend extension list without changing the application input.
 */
static VkResult
device_prepare(
	struct compat_instance *instance,
	VkPhysicalDevice physical,
	const VkDeviceCreateInfo *create,
	VkDeviceCreateInfo *rewritten,
	const char ***names,
	struct compat_capabilities *capabilities,
	unsigned *swapchain)
{
	VkExtensionProperties *extensions;
	VkPhysicalDeviceProperties properties;
	VkResult error;
	const char **list;
	const char *name;
	const char *required[20];
	uint32_t count;
	uint32_t used;
	uint32_t needed;
	uint32_t index;
	uint32_t other;
	uint32_t version;
	int present;
	int evaluated;

	/* Measures available backend extensions and allocates one owned rewrite. */
	error = compat_backend_extensions(physical, NULL, &count, &extensions);
	if (error != VK_SUCCESS)
		return error;

	/* Reserves the application's requests plus the bounded internal prerequisites. */
	list = calloc((size_t)create->enabledExtensionCount + 20, sizeof(*list));
	if (list == NULL) {
		/* Releases the query on allocation failure. */
		free(extensions);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Collects application extensions, owning swapchain handles ourselves. */
	used = 0;
	*swapchain = 0;
	memset(capabilities, 0, sizeof(*capabilities));
	for (index = 0; index < create->enabledExtensionCount; index++) {
		/* Removes our swapchain request from the unchanged application input. */
		name = create->ppEnabledExtensionNames[index];
		evaluated = strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		if (evaluated == 0) {
			*swapchain = 1;
			continue;
		}

		/* Refuses foreign WSI handles before passing ordinary extensions through. */
		evaluated = compat_wsi_extension(name);
		if (evaluated != 0) {
			free(list);
			free(extensions);
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		}

		/* Preserves the application's ordinary extension order. */
		list[used++] = name;
	}

	/* Adds WSI prerequisites only when this device requested our swapchain. */
	needed = 0;
	if (*swapchain != 0) {
		/* Accepts either actual DMA-BUF export or the portable KMS copy path. */
		compat_physical_capabilities(physical, capabilities);
		evaluated = compat_swapchain_available(physical);
		if (evaluated == 0) {
			free(list);
			free(extensions);
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		}

		/* Uses the effective physical and instance API for promoted prerequisites. */
		instance->properties(physical, &properties);
		version = instance->api_version;
		if (properties.apiVersion < version)
			version = properties.apiVersion;

		/* Enables backend swapchain layout semantics when provided. */
		evaluated = compat_extension_has(extensions, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		if (evaluated != 0)
			required[needed++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

		/* The portable display copy path needs no external-memory or synchronization extension. */
		if (capabilities->path != COMPAT_WSI_NONE) {
			/* DMA-BUF export remains an extension at every supported API version. */
			required[needed++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
			required[needed++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;

			/* API 1.0 needs explicit external-memory and dedicated-allocation names. */
			if (version < VK_API_VERSION_1_1) {
				required[needed++] = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
				required[needed++] = VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME;
				required[needed++] = VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME;
			}

			/* Adds file-descriptor synchronization only when imports and exports are supported. */
			if (capabilities->implicit_sync != 0) {
				required[needed++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
				required[needed++] = VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME;

				/* API 1.0 also needs the unpromoted synchronization dependencies. */
				if (version < VK_API_VERSION_1_1) {
					required[needed++] = VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME;
					required[needed++] = VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME;
				}
			}

			/* Modifier images need their unpromoted format and binding dependencies. */
			if (capabilities->path == COMPAT_WSI_MODIFIER) {
				required[needed++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
				if (version < VK_API_VERSION_1_2)
					required[needed++] = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;

				/* API 1.0 lacks the promoted binding and sampler capabilities. */
				if (version < VK_API_VERSION_1_1) {
					required[needed++] = VK_KHR_BIND_MEMORY_2_EXTENSION_NAME;
					required[needed++] = VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME;
					required[needed++] = VK_KHR_MAINTENANCE_1_EXTENSION_NAME;
				}
			}

			/* Foreign ownership transfer is available only when the driver advertises it. */
			if (capabilities->foreign != 0)
				required[needed++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
		}
	}

	/* Appends each supported prerequisite once. */
	for (index = 0; index < needed; index++) {
		/* Rejects a missing prerequisite without creating a partial device. */
		evaluated = compat_extension_has(extensions, count, required[index]);
		if (evaluated == 0) {
			free(list);
			free(extensions);
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		}

		/* Searches the collected application and internal extension names. */
		present = 0;
		for (other = 0; other < used; other++) {
			/* Retains a single name for a prerequisite requested by the application. */
			evaluated = strcmp(list[other], required[index]);
			if (evaluated == 0)
				present = 1;
		}

		/* Adds only a previously absent prerequisite. */
		if (present == 0)
			list[used++] = required[index];
	}

	/* Transfers the rewritten list to the creation caller. */
	*rewritten = *create;
	rewritten->enabledExtensionCount = used;
	rewritten->ppEnabledExtensionNames = list;
	*names = list;
	free(extensions);

	/* Succeeded: the caller owns names until backend creation returns. */
	return VK_SUCCESS;
}

/*
 * Resolves unmodified image and synchronization operations on one backend device.
 */
static void
device_functions(
	struct compat_device *device)
{
	/* Resolves the backend vkCreateImage operation without interposition. */
	device->create_image = (PFN_vkCreateImage)compat_backend.get_device_proc(device->handle, "vkCreateImage");

	/* Resolves the backend vkDestroyImage operation without interposition. */
	device->destroy_image = (PFN_vkDestroyImage)compat_backend.get_device_proc(device->handle, "vkDestroyImage");

	/* Resolves the backend vkGetImageMemoryRequirements2 operation without interposition. */
	if (device->api_version < VK_API_VERSION_1_1)
		device->image_requirements = (PFN_vkGetImageMemoryRequirements2)compat_backend.get_device_proc(device->handle, "vkGetImageMemoryRequirements2KHR");
	else
		device->image_requirements = (PFN_vkGetImageMemoryRequirements2)compat_backend.get_device_proc(device->handle, "vkGetImageMemoryRequirements2");

	/* Optimal KMS copy images also work with the mandatory API-1.0 requirements query. */
	device->image_requirements1 = (PFN_vkGetImageMemoryRequirements)compat_backend.get_device_proc(device->handle, "vkGetImageMemoryRequirements");

	/* Resolves the backend vkAllocateMemory operation without interposition. */
	device->allocate_memory = (PFN_vkAllocateMemory)compat_backend.get_device_proc(device->handle, "vkAllocateMemory");

	/* Resolves the backend vkFreeMemory operation without interposition. */
	device->free_memory = (PFN_vkFreeMemory)compat_backend.get_device_proc(device->handle, "vkFreeMemory");

	/* Resolves the backend vkBindImageMemory operation without interposition. */
	device->bind_image = (PFN_vkBindImageMemory)compat_backend.get_device_proc(device->handle, "vkBindImageMemory");

	/* Resolves the backend vkGetMemoryFdKHR operation without interposition. */
	device->memory_fd = (PFN_vkGetMemoryFdKHR)compat_backend.get_device_proc(device->handle, "vkGetMemoryFdKHR");

	/* Resolves the backend vkGetImageDrmFormatModifierPropertiesEXT operation without interposition. */
	device->image_modifier = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)compat_backend.get_device_proc(device->handle, "vkGetImageDrmFormatModifierPropertiesEXT");

	/* Resolves the backend vkGetImageSubresourceLayout operation without interposition. */
	device->image_layout = (PFN_vkGetImageSubresourceLayout)compat_backend.get_device_proc(device->handle, "vkGetImageSubresourceLayout");

	/* Resolves the backend vkCreateCommandPool operation without interposition. */
	device->create_pool = (PFN_vkCreateCommandPool)compat_backend.get_device_proc(device->handle, "vkCreateCommandPool");

	/* Resolves the backend vkDestroyCommandPool operation without interposition. */
	device->destroy_pool = (PFN_vkDestroyCommandPool)compat_backend.get_device_proc(device->handle, "vkDestroyCommandPool");

	/* Resolves the backend vkAllocateCommandBuffers operation without interposition. */
	device->allocate_commands = (PFN_vkAllocateCommandBuffers)compat_backend.get_device_proc(device->handle, "vkAllocateCommandBuffers");

	/* Resolves the backend vkBeginCommandBuffer operation without interposition. */
	device->begin_command = (PFN_vkBeginCommandBuffer)compat_backend.get_device_proc(device->handle, "vkBeginCommandBuffer");

	/* Resolves the backend vkEndCommandBuffer operation without interposition. */
	device->end_command = (PFN_vkEndCommandBuffer)compat_backend.get_device_proc(device->handle, "vkEndCommandBuffer");

	/* Resolves the backend vkCmdPipelineBarrier operation without interposition. */
	device->barrier = (PFN_vkCmdPipelineBarrier)compat_backend.get_device_proc(device->handle, "vkCmdPipelineBarrier");

	/* Resolves the backend vkCmdCopyImageToBuffer operation without interposition. */
	device->copy_image = (PFN_vkCmdCopyImageToBuffer)compat_backend.get_device_proc(device->handle, "vkCmdCopyImageToBuffer");

	/* Resolves the backend vkQueueSubmit operation without interposition. */
	device->submit = (PFN_vkQueueSubmit)compat_backend.get_device_proc(device->handle, "vkQueueSubmit");

	/* Resolves the backend vkQueueWaitIdle operation without interposition. */
	device->queue_idle = (PFN_vkQueueWaitIdle)compat_backend.get_device_proc(device->handle, "vkQueueWaitIdle");

	/* Resolves the backend vkCreateSemaphore operation without interposition. */
	device->create_semaphore = (PFN_vkCreateSemaphore)compat_backend.get_device_proc(device->handle, "vkCreateSemaphore");

	/* Resolves the backend vkDestroySemaphore operation without interposition. */
	device->destroy_semaphore = (PFN_vkDestroySemaphore)compat_backend.get_device_proc(device->handle, "vkDestroySemaphore");

	/* Resolves the backend vkGetSemaphoreFdKHR operation without interposition. */
	device->semaphore_fd = (PFN_vkGetSemaphoreFdKHR)compat_backend.get_device_proc(device->handle, "vkGetSemaphoreFdKHR");

	/* Resolves the backend vkImportSemaphoreFdKHR operation without interposition. */
	device->import_semaphore = (PFN_vkImportSemaphoreFdKHR)compat_backend.get_device_proc(device->handle, "vkImportSemaphoreFdKHR");

	/* Resolves the backend vkCreateFence operation without interposition. */
	device->create_fence = (PFN_vkCreateFence)compat_backend.get_device_proc(device->handle, "vkCreateFence");

	/* Resolves the backend vkDestroyFence operation without interposition. */
	device->destroy_fence = (PFN_vkDestroyFence)compat_backend.get_device_proc(device->handle, "vkDestroyFence");

	/* Resolves the backend vkImportFenceFdKHR operation without interposition. */
	device->import_fence = (PFN_vkImportFenceFdKHR)compat_backend.get_device_proc(device->handle, "vkImportFenceFdKHR");

	/* Resolves the backend vkWaitForFences operation without interposition. */
	device->wait_fences = (PFN_vkWaitForFences)compat_backend.get_device_proc(device->handle, "vkWaitForFences");

	/* Resolves the backend vkResetFences operation without interposition. */
	device->reset_fences = (PFN_vkResetFences)compat_backend.get_device_proc(device->handle, "vkResetFences");

	/* Resolves the backend vkCreateBuffer operation without interposition. */
	device->create_buffer = (PFN_vkCreateBuffer)compat_backend.get_device_proc(device->handle, "vkCreateBuffer");

	/* Resolves the backend vkDestroyBuffer operation without interposition. */
	device->destroy_buffer = (PFN_vkDestroyBuffer)compat_backend.get_device_proc(device->handle, "vkDestroyBuffer");

	/* Resolves the backend vkGetBufferMemoryRequirements operation without interposition. */
	device->buffer_requirements = (PFN_vkGetBufferMemoryRequirements)compat_backend.get_device_proc(device->handle, "vkGetBufferMemoryRequirements");

	/* Resolves the backend vkBindBufferMemory operation without interposition. */
	device->bind_buffer = (PFN_vkBindBufferMemory)compat_backend.get_device_proc(device->handle, "vkBindBufferMemory");

	/* Resolves the backend vkMapMemory operation without interposition. */
	device->map_memory = (PFN_vkMapMemory)compat_backend.get_device_proc(device->handle, "vkMapMemory");

	/* Resolves the backend vkUnmapMemory operation without interposition. */
	device->unmap_memory = (PFN_vkUnmapMemory)compat_backend.get_device_proc(device->handle, "vkUnmapMemory");

	/* Succeeded: the device owns its resolved backend procedures. */
	return;
}
