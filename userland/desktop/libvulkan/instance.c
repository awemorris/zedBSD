/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Composes standard instances from independently owned compatible GPU sessions.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>
#include <uapi/gpu-allocation.h>
#include <uapi/gpu-fence.h>
#include <uapi/gpu-job.h>

static VkResult instance_extensions(const VkInstanceCreateInfo *info, uint64_t *enabled);
static VkResult instance_add_context(struct VkInstance_T *instance, const VkInstanceCreateInfo *info, const char *path);
static VkResult instance_create_remote(struct vulkan_instance_context *link, const VkInstanceCreateInfo *info);
static VkResult instance_enumerate_remote(struct vulkan_instance_context *link);
static VkResult physical_load(struct VkPhysicalDevice_T *physical);
static VkResult physical_load_queues(struct VkPhysicalDevice_T *physical);
static VkResult physical_load_core11(struct VkPhysicalDevice_T *physical);
static void physical_core11_features(struct vulkan_reader *reader, struct vulkan_core11_facts *facts);
static void physical_core11_properties(struct vulkan_reader *reader, struct vulkan_core11_facts *facts);
static VkResult physical_load_core12(struct VkPhysicalDevice_T *physical);
static void physical_core12_properties(struct vulkan_reader *reader, VkPhysicalDeviceVulkan12Properties *properties);
static void physical_chain_request(struct vulkan_writer *writer, uint32_t outer, const uint32_t *types, uint32_t count);
static void physical_chain_reply(struct vulkan_reader *reader, uint32_t outer, const uint32_t *types, uint32_t count);
static void physical_version(struct VkPhysicalDevice_T *physical);
static VkResult physical_load_video(struct VkPhysicalDevice_T *physical);
static VkBool32 physical_video_promised(const struct VkPhysicalDevice_T *physical);
static VkResult physical_query_begin(struct VkPhysicalDevice_T *physical, uint32_t opcode, struct vulkan_reader *reader);
static void physical_finish(struct VkPhysicalDevice_T *physical);
static void instance_finish(struct VkInstance_T *instance);
static VkResult enumerate_extensions(const VkExtensionProperties *available, uint32_t total, uint32_t *count, VkExtensionProperties *properties);
static VkBool32 gpu_node_name(const char *name);

/*
 * Creates a standard instance without coupling public handles to a single GPU.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(
	const VkInstanceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkInstance *pInstance)
{
	struct VkInstance_T *instance;
	struct vulkan_object *object;
	struct dirent *entry;
	DIR *directory;
	char path[262];
	uint64_t enabled;
	uint32_t version;
	VkResult status;
	VkBool32 selected;
	int mutex_status;
	int saved_errno;
	int directory_status;
	size_t length;

	/* The application's version, 1.0 when it names none. */
	version = VK_API_VERSION_1_0;
	if (pCreateInfo->pApplicationInfo != NULL && pCreateInfo->pApplicationInfo->apiVersion != 0)
		version = pCreateInfo->pApplicationInfo->apiVersion;

	/*
	 * A library of Vulkan 1.0 takes only 1.0; one of a later version takes
	 * any apiVersion, the devices' own versions deciding what they offer.
	 *
	 * Procedure referenced: the Vulkan 1.1 specification (section 4.2,
	 * VkApplicationInfo: an implementation of 1.1 or later does not
	 * return VK_ERROR_INCOMPATIBLE_DRIVER for any apiVersion).
	 */
	if (VULKAN_LIBRARY_VERSION < VK_MAKE_VERSION(1, 1, 0) &&
	    (VK_VERSION_MAJOR(version) != 1 || VK_VERSION_MINOR(version) != 0))
		return VK_ERROR_INCOMPATIBLE_DRIVER;

	/* Checks requested local extension and layer names before opening a GPU session. */
	status = instance_extensions(pCreateInfo, &enabled);
	if (status != VK_SUCCESS)
		return status;

	/* Gives the instance its own callback policy and independently owned children. */
	status = vulkan_object_alloc(sizeof(*instance), __alignof__(struct VkInstance_T), VULKAN_OBJECT_INSTANCE, NULL, NULL, pAllocator, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Prepares publication state before any renderer objects can be created. */
	instance = (struct VkInstance_T *)object;
	instance->api_version = version;
	instance->enabled_extensions = enabled;
	mutex_status = pthread_mutex_init(&instance->mutex, NULL);
	if (mutex_status != 0) {
		vulkan_object_free(object);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Enumerates ordinary device nodes instead of assuming a first GPU number. */
	directory = opendir("/dev");
	if (directory == NULL) {
		instance_finish(instance);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Opens each selected node once while retaining independent remote namespaces. */
	status = VK_SUCCESS;
	while (1) {
		/* Distinguishes a completed directory traversal from an I/O failure. */
		errno = 0;
		entry = readdir(directory);
		saved_errno = errno;
		if (entry == NULL) {
			/* A failed discovery cannot silently publish a partial instance. */
			if (saved_errno != 0)
				status = VK_ERROR_INITIALIZATION_FAILED;
			break;
		}

		/* Ignores unrelated device names without opening their interfaces. */
		selected = gpu_node_name(entry->d_name);
		if (!selected)
			continue;

		/* Builds the exact device path from a bounded directory name. */
		length = strlen(entry->d_name);
		if (length > sizeof(path) - 6) {
			status = VK_ERROR_INITIALIZATION_FAILED;
			break;
		}

		/* Appends this context only if its protocol and physical-device queries work. */
		memcpy(path, "/dev/", 5);
		memcpy(path + 5, entry->d_name, length + 1);
		status = instance_add_context(instance, pCreateInfo, path);
		if (status != VK_SUCCESS)
			break;
	}

	/* Releases the directory before any callback-owned instance rollback. */
	directory_status = closedir(directory);
	if (directory_status != 0 && status == VK_SUCCESS)
		status = VK_ERROR_INITIALIZATION_FAILED;

	/* Preserves the earlier discovery failure after consuming the directory. */
	if (status != VK_SUCCESS) {
		instance_finish(instance);
		return status;
	}

	/* Publishes a complete instance, including the valid zero-device case. */
	status = vulkan_object_publish(&instance->object);
	if (status != VK_SUCCESS) {
		instance_finish(instance);
		return status;
	}

	/* Changes caller output only after every local and remote setup step completed. */
	*pInstance = (VkInstance)instance;

	/* Succeeded: this instance owns all enumerated compatible GPU sessions. */
	return VK_SUCCESS;
}

/*
 * Destroys all instance-owned discovery and direct-display state.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(
	VkInstance instance,
	const VkAllocationCallbacks *pAllocator)
{
	struct VkInstance_T *owner;

	/* Accepts a null instance without touching application callback storage. */
	owner = vulkan_instance(instance);
	if (owner == NULL)
		return;

	/* Uses this destruction call's compatible policy for instance-scoped allocations. */
	if (pAllocator != NULL) {
		owner->object.allocator.callbacks = *pAllocator;
		owner->object.allocator.has_callbacks = VK_TRUE;
	}

	/* Runs WSI cleanup before releasing physical devices and their sessions. */
	vulkan_wsi_instance_finish(owner);
	instance_finish(owner);

	/* Succeeded: the public instance no longer owns a renderer namespace. */
	return;
}

/*
 * Enumerates stable physical handles with Vulkan's count and truncation semantics.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDevices(
	VkInstance instance,
	uint32_t *pPhysicalDeviceCount,
	VkPhysicalDevice *pPhysicalDevices)
{
	struct VkInstance_T *owner;
	uint32_t count;
	uint32_t index;

	/* Count-only calls do not require caller array storage. */
	owner = vulkan_instance(instance);
	if (pPhysicalDevices == NULL) {
		*pPhysicalDeviceCount = owner->physical_device_count;
		return VK_SUCCESS;
	}

	/* Copies only the caller's available capacity and reports actual copied elements. */
	count = *pPhysicalDeviceCount;
	if (count > owner->physical_device_count)
		count = owner->physical_device_count;

	/* Every returned handle retains the same instance-owned physical identity. */
	for (index = 0; index < count; index++) {
		pPhysicalDevices[index] = (VkPhysicalDevice)owner->physical_devices[index];
	}

	/* A short output array is an incomplete enumeration rather than a failed query. */
	*pPhysicalDeviceCount = count;
	if (count < owner->physical_device_count)
		return VK_INCOMPLETE;

	/* Succeeded: every physical device was returned to the caller. */
	return VK_SUCCESS;
}

/*
 * Returns the physical feature set that this implementation can actually expose.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceFeatures *pFeatures)
{
	struct VkPhysicalDevice_T *physical;

	/* Uses the stable discovery snapshot without a new host allocation or wire command. */
	physical = vulkan_physical_device(physicalDevice);
	*pFeatures = physical->features;

	/* Succeeded: the complete Vulkan 1.0 feature structure is initialized. */
	return;
}

/*
 * Returns physical limits after applying local memory-mapping guarantees.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceProperties *pProperties)
{
	struct VkPhysicalDevice_T *physical;

	/* Preserves every declared field through an ABI-correct cached structure copy. */
	physical = vulkan_physical_device(physicalDevice);
	*pProperties = physical->properties;

	/* Succeeded: the caller receives the implementation's actual exposed limits. */
	return;
}

/*
 * Returns original memory type indices with truthful host visibility properties.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
	struct VkPhysicalDevice_T *physical;

	/* Keeps memoryTypeBits indices compatible with renderer resource requirements. */
	physical = vulkan_physical_device(physicalDevice);
	*pMemoryProperties = physical->memory;

	/* Succeeded: the caller can select memory without unsupported cache assumptions. */
	return;
}

/*
 * Enumerates queue family capabilities without replacing stable family indices.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties(
	VkPhysicalDevice physicalDevice,
	uint32_t *pQueueFamilyPropertyCount,
	VkQueueFamilyProperties *pQueueFamilyProperties)
{
	struct VkPhysicalDevice_T *physical;
	uint32_t count;

	/* A null array requests the full count without consuming caller capacity. */
	physical = vulkan_physical_device(physicalDevice);
	if (pQueueFamilyProperties == NULL) {
		*pQueueFamilyPropertyCount = physical->queue_family_count;
		return;
	}

	/* Copies only complete structures that fit the supplied element count. */
	count = *pQueueFamilyPropertyCount;
	if (count > physical->queue_family_count)
		count = physical->queue_family_count;

	/* Publishes the number written, as required by this void enumeration API. */
	memcpy(pQueueFamilyProperties, physical->queue_families, (size_t)count * sizeof(*pQueueFamilyProperties));
	*pQueueFamilyPropertyCount = count;

	/* Succeeded: each returned family retains its native queue capability fields. */
	return;
}

/*
 * Queries format capabilities for an arbitrary standard Vulkan format.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties(
	VkPhysicalDevice physicalDevice,
	VkFormat format,
	VkFormatProperties *pFormatProperties)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkFormatProperties properties;
	VkResult status;
	VkBool32 present;
	VkBool32 promised;

	/* Requests this format's complete output without assuming an application scene. */
	physical = vulkan_physical_device(physicalDevice);
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u32(&writer, format);
	vulkan_write_u64(&writer, 1);
	status = vulkan_command_execute(physical->object.context, &writer, 24, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);

	/* Decodes into private storage before publishing a complete output structure. */
	memset(&properties, 0, sizeof(properties));
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (present)
			vulkan_decode_VkFormatProperties(&reader, &properties);
	}

	/* A malformed response remains observable by subsequent result-bearing commands. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS)
		memset(&properties, 0, sizeof(properties));

	/*
	 * A renderer that does not promise video decode may still report the
	 * video format features of its own driver.  Without the video
	 * extensions those bits name nothing the caller can use, so they are
	 * withheld (the video features of encode included).
	 */
	promised = physical_video_promised(physical);
	if (!promised) {
		properties.linearTilingFeatures &= ~VULKAN_VIDEO_FORMAT_FEATURES;
		properties.optimalTilingFeatures &= ~VULKAN_VIDEO_FORMAT_FEATURES;
		properties.bufferFeatures &= ~VULKAN_VIDEO_FORMAT_FEATURES;
	}

	/* Publishes all fields, including the valid unsupported-format zero capability set. */
	*pFormatProperties = properties;

	/* Succeeded: the output contains no uninitialized or partial renderer bytes. */
	return;
}

/*
 * Queries arbitrary image-format combinations and preserves unsupported-format results.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties(
	VkPhysicalDevice physicalDevice,
	VkFormat format,
	VkImageType type,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkImageCreateFlags flags,
	VkImageFormatProperties *pImageFormatProperties)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkImageFormatProperties properties;
	VkResult status;
	VkBool32 present;

	/* Forwards every standard image-format parameter in declared wire order. */
	physical = vulkan_physical_device(physicalDevice);
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u32(&writer, format);
	vulkan_write_u32(&writer, type);
	vulkan_write_u32(&writer, tiling);
	vulkan_write_u32(&writer, usage);
	vulkan_write_u32(&writer, flags);
	vulkan_write_u64(&writer, 1);
	status = vulkan_command_execute(physical->object.context, &writer, 64, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		status = vulkan_reply_finish(physical->object.context, &reader, status);
		return status;
	}

	/* Publishes image properties only after the entire fixed output was validated. */
	memset(&properties, 0, sizeof(properties));
	present = vulkan_reply_pointer(&reader);
	if (present)
		vulkan_decode_VkImageFormatProperties(&reader, &properties);

	/* Retains protocol failure instead of exposing partially decoded limits. */
	status = vulkan_reply_finish(physical->object.context, &reader, VK_SUCCESS);
	if (status != VK_SUCCESS)
		return status;

	/* Changes caller storage only for a supported, completely decoded combination. */
	*pImageFormatProperties = properties;

	/* Succeeded: all returned image limits describe the requested combination. */
	return VK_SUCCESS;
}

/*
 * Advertises only the selected standard direct-display instance extensions.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(
	const char *pLayerName,
	uint32_t *pPropertyCount,
	VkExtensionProperties *pProperties)
{
	VkExtensionProperties available[11];
	VkResult status;

	/* This library does not impersonate a separately installable validation layer. */
	if (pLayerName != NULL)
		return VK_ERROR_LAYER_NOT_PRESENT;

	/* Associates exact registry extension versions with the implemented entry points. */
	memset(available, 0, sizeof(available));
	strcpy(available[0].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
	available[0].specVersion = VK_KHR_SURFACE_SPEC_VERSION;
	strcpy(available[1].extensionName, VK_KHR_DISPLAY_EXTENSION_NAME);
	available[1].specVersion = VK_KHR_DISPLAY_SPEC_VERSION;
	strcpy(available[2].extensionName, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
	available[2].specVersion = VK_KHR_WAYLAND_SURFACE_SPEC_VERSION;
	strcpy(available[3].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	available[3].specVersion = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION;
	strcpy(available[4].extensionName, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
	available[4].specVersion = VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_SPEC_VERSION;
	strcpy(available[5].extensionName, VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME);
	available[5].specVersion = VK_KHR_EXTERNAL_FENCE_CAPABILITIES_SPEC_VERSION;
	strcpy(available[6].extensionName, VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME);
	available[6].specVersion = VK_EXT_DISPLAY_SURFACE_COUNTER_SPEC_VERSION;
	strcpy(available[7].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
	available[7].specVersion = VK_KHR_GET_SURFACE_CAPABILITIES_2_SPEC_VERSION;
	strcpy(available[8].extensionName, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
	available[8].specVersion = VK_EXT_SURFACE_MAINTENANCE_1_SPEC_VERSION;
	strcpy(available[9].extensionName, VK_KHR_GET_DISPLAY_PROPERTIES_2_EXTENSION_NAME);
	available[9].specVersion = VK_KHR_GET_DISPLAY_PROPERTIES_2_SPEC_VERSION;
	strcpy(available[10].extensionName, VK_ZEDBSD_DISPLAY_EDID_EXTENSION_NAME);
	available[10].specVersion = VK_ZEDBSD_DISPLAY_EDID_SPEC_VERSION;

	/* Preserves the required partial-array result when caller capacity is smaller. */
	status = enumerate_extensions(available, 11, pPropertyCount, pProperties);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the caller received the complete count or extension array. */
	return VK_SUCCESS;
}

/*
 * Advertises device WSI only when the associated GPU can actually present.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(
	VkPhysicalDevice physicalDevice,
	const char *pLayerName,
	uint32_t *pPropertyCount,
	VkExtensionProperties *pProperties)
{
	struct VkPhysicalDevice_T *physical;
	VkExtensionProperties available[16];
	uint32_t count;
	VkResult status;

	/* No device layer is installed or claimed by the ordinary driver library. */
	if (pLayerName != NULL)
		return VK_ERROR_LAYER_NOT_PRESENT;

	/* Constructs only the local device extensions supported by this physical device. */
	physical = vulkan_physical_device(physicalDevice);
	count = 0;
	memset(available, 0, sizeof(available));
	if (physical->supported_extensions & VULKAN_DEVICE_SWAPCHAIN) {
		strcpy(available[count].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_SWAPCHAIN_SPEC_VERSION;
		count++;
	}

	/* The changed rectangles of a present, with the swapchain (BUG-221). */
	if (physical->supported_extensions & VULKAN_DEVICE_INCREMENTAL_PRESENT) {
		strcpy(available[count].extensionName, VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_INCREMENTAL_PRESENT_SPEC_VERSION;
		count++;
	}

	/* Exposes shared direct-display swapchains only with their actual local support. */
	if (physical->supported_extensions & VULKAN_DEVICE_DISPLAY_SWAPCHAIN) {
		strcpy(available[count].extensionName, VK_KHR_DISPLAY_SWAPCHAIN_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_DISPLAY_SWAPCHAIN_SPEC_VERSION;
		count++;
	}

	/* External handles use the kernel's typed fd capability, independently of presentation. */
	if (physical->supported_extensions & VULKAN_DEVICE_EXTERNAL_MEMORY) {
		strcpy(available[count].extensionName, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_EXTERNAL_MEMORY_SPEC_VERSION;
		count++;
	}
	if (physical->supported_extensions & VULKAN_DEVICE_EXTERNAL_MEMORY_FD) {
		strcpy(available[count].extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_EXTERNAL_MEMORY_FD_SPEC_VERSION;
		count++;
	}
	if (physical->supported_extensions & VULKAN_DEVICE_EXTERNAL_FENCE) {
		strcpy(available[count].extensionName, VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_EXTERNAL_FENCE_SPEC_VERSION;
		count++;
	}
	if (physical->supported_extensions & VULKAN_DEVICE_EXTERNAL_FENCE_FD) {
		strcpy(available[count].extensionName, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_EXTERNAL_FENCE_FD_SPEC_VERSION;
		count++;
	}

	/* Memory requirements with extensible output, answered locally from the 1.0 queries. */
	if (physical->supported_extensions & VULKAN_DEVICE_MEMORY_REQUIREMENTS2) {
		strcpy(available[count].extensionName, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_GET_MEMORY_REQUIREMENTS_2_SPEC_VERSION;
		count++;
	}

	/* An allocation, an imported one above all, can name the one image it is for. */
	if (physical->supported_extensions & VULKAN_DEVICE_DEDICATED_ALLOCATION) {
		strcpy(available[count].extensionName, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_DEDICATED_ALLOCATION_SPEC_VERSION;
		count++;
	}

	/* The barrier and submission forms the video extensions are written against. */
	if (physical->supported_extensions & VULKAN_DEVICE_SYNCHRONIZATION2) {
		strcpy(available[count].extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_SYNCHRONIZATION_2_SPEC_VERSION;
		count++;
	}

	/* Present fences, present modes, scaling and image release of a swapchain. */
	if (physical->supported_extensions & VULKAN_DEVICE_SWAPCHAIN_MAINTENANCE1) {
		strcpy(available[count].extensionName, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
		available[count].specVersion = VK_EXT_SWAPCHAIN_MAINTENANCE_1_SPEC_VERSION;
		count++;
	}

	/* Display power, hotplug and refresh events need the node's event and control requests. */
	if (physical->supported_extensions & VULKAN_DEVICE_DISPLAY_CONTROL) {
		strcpy(available[count].extensionName, VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME);
		available[count].specVersion = VK_EXT_DISPLAY_CONTROL_SPEC_VERSION;
		count++;
	}

	/* Video decode is named only when the native renderer promised a decode family of a codec. */
	if (physical->supported_extensions & VULKAN_DEVICE_VIDEO_QUEUE) {
		strcpy(available[count].extensionName, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_VIDEO_QUEUE_SPEC_VERSION;
		count++;
	}
	if (physical->supported_extensions & VULKAN_DEVICE_VIDEO_DECODE_QUEUE) {
		strcpy(available[count].extensionName, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_VIDEO_DECODE_QUEUE_SPEC_VERSION;
		count++;
	}
	if (physical->supported_extensions & VULKAN_DEVICE_VIDEO_DECODE_H264) {
		strcpy(available[count].extensionName, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_VIDEO_DECODE_H264_SPEC_VERSION;
		count++;
	}
	if (physical->supported_extensions & VULKAN_DEVICE_VIDEO_DECODE_H265) {
		strcpy(available[count].extensionName, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_VIDEO_DECODE_H265_SPEC_VERSION;
		count++;
	}
	if (physical->supported_extensions & VULKAN_DEVICE_VIDEO_ENCODE_QUEUE) {
		strcpy(available[count].extensionName, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_VIDEO_ENCODE_QUEUE_SPEC_VERSION;
		count++;
	}
	if (physical->supported_extensions & VULKAN_DEVICE_VIDEO_ENCODE_H264) {
		strcpy(available[count].extensionName, VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME);
		available[count].specVersion = VK_KHR_VIDEO_ENCODE_H264_SPEC_VERSION;
		count++;
	}

	/* Preserves truncation without claiming unsupported host extensions exist locally. */
	status = enumerate_extensions(available, count, pPropertyCount, pProperties);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the caller received the complete local device extension set. */
	return VK_SUCCESS;
}

/*
 * Reports the empty set of installed instance layers.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(
	uint32_t *pPropertyCount,
	VkLayerProperties *pProperties)
{
	/* No layer array elements exist, so caller storage needs no writes. */
	(void)pProperties;
	*pPropertyCount = 0;

	/* Succeeded: zero installed layers is a complete enumeration. */
	return VK_SUCCESS;
}

/*
 * Reports the empty set of installed legacy device layers.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceLayerProperties(
	VkPhysicalDevice physicalDevice,
	uint32_t *pPropertyCount,
	VkLayerProperties *pProperties)
{
	/* Device layers are not synthesized from renderer extensions. */
	(void)physicalDevice;
	(void)pProperties;
	*pPropertyCount = 0;

	/* Succeeded: no device layer entries remain to be enumerated. */
	return VK_SUCCESS;
}

/* Validates the instance's explicitly selected extension dependencies. */
static VkResult
instance_extensions(
	const VkInstanceCreateInfo *info,
	uint64_t *enabled)
{
	uint64_t bits;
	uint32_t index;
	int match;

	/* Refuses unavailable layers before their names can reach the renderer. */
	if (info->enabledLayerCount != 0)
		return VK_ERROR_LAYER_NOT_PRESENT;

	/* Recognizes the full locally implemented instance-extension set. */
	bits = 0;
	for (index = 0; index < info->enabledExtensionCount; index++) {
		/* Matches surface support as an explicitly enabled instance capability. */
		match = strcmp(info->ppEnabledExtensionNames[index], VK_KHR_SURFACE_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_INSTANCE_SURFACE;
			continue;
		}

		/* Matches the direct-display extension without forwarding it to host WSI. */
		match = strcmp(info->ppEnabledExtensionNames[index], VK_KHR_DISPLAY_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_INSTANCE_DISPLAY;
			continue;
		}

		/* Wayland presentation remains local to the guest connection. */
		match = strcmp(info->ppEnabledExtensionNames[index], VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_INSTANCE_WAYLAND;
			continue;
		}

		/* Standard capability queries precede any device extension selection. */
		match = strcmp(info->ppEnabledExtensionNames[index], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_INSTANCE_PROPERTIES2;
			continue;
		}
		match = strcmp(info->ppEnabledExtensionNames[index], VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_INSTANCE_EXTERNAL_MEMORY;
			continue;
		}
		match = strcmp(info->ppEnabledExtensionNames[index], VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_INSTANCE_EXTERNAL_FENCE;
			continue;
		}

		/* The surface capability query with counters; this library offers no counter. */
		match = strcmp(info->ppEnabledExtensionNames[index], VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_INSTANCE_SURFACE_COUNTER;
			continue;
		}

		/* The extensible surface queries, and the surface maintenance records they carry. */
		match = strcmp(info->ppEnabledExtensionNames[index], VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_INSTANCE_SURFACE_CAPABILITIES2;
			continue;
		}

		/* The surface maintenance records of the swapchains. */
		match = strcmp(info->ppEnabledExtensionNames[index], VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_INSTANCE_SURFACE_MAINTENANCE1;
			continue;
		}

		/* The extensible display queries. */
		match = strcmp(info->ppEnabledExtensionNames[index], VK_KHR_GET_DISPLAY_PROPERTIES_2_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_INSTANCE_DISPLAY_PROPERTIES2;
			continue;
		}

		/* A display's EDID, chained to the extensible display records. */
		match = strcmp(info->ppEnabledExtensionNames[index], VK_ZEDBSD_DISPLAY_EDID_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_INSTANCE_DISPLAY_EDID;
			continue;
		}

		/* An unknown requested extension cannot be silently treated as implemented. */
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}

	/* The extensible surface queries describe surfaces. */
	if ((bits & VULKAN_INSTANCE_SURFACE_CAPABILITIES2) && !(bits & VULKAN_INSTANCE_SURFACE))
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Surface maintenance answers through the extensible surface queries. */
	if ((bits & VULKAN_INSTANCE_SURFACE_MAINTENANCE1) && !(bits & VULKAN_INSTANCE_SURFACE_CAPABILITIES2))
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Both presentation backends require the surface capability in the same instance. */
	if ((bits & (VULKAN_INSTANCE_DISPLAY | VULKAN_INSTANCE_WAYLAND)) && !(bits & VULKAN_INSTANCE_SURFACE))
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* The extensible display queries describe direct displays. */
	if ((bits & VULKAN_INSTANCE_DISPLAY_PROPERTIES2) && !(bits & VULKAN_INSTANCE_DISPLAY))
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* The EDID is chained to the extensible display records. */
	if ((bits & VULKAN_INSTANCE_DISPLAY_EDID) && !(bits & VULKAN_INSTANCE_DISPLAY_PROPERTIES2))
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* The surface counters are a direct-display extension. */
	if ((bits & VULKAN_INSTANCE_SURFACE_COUNTER) && !(bits & VULKAN_INSTANCE_DISPLAY))
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Vulkan 1.0 external capabilities depend on the properties2 extension. */
	if ((bits & (VULKAN_INSTANCE_EXTERNAL_MEMORY | VULKAN_INSTANCE_EXTERNAL_FENCE)) && !(bits & VULKAN_INSTANCE_PROPERTIES2))
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Publishes the validated enabled-bit snapshot. */
	*enabled = bits;

	/* Succeeded: every selected extension has its required local implementation. */
	return VK_SUCCESS;
}

/* Opens and initializes one independent renderer namespace. */
static VkResult
instance_add_context(
	struct VkInstance_T *instance,
	const VkInstanceCreateInfo *info,
	const char *path)
{
	struct vulkan_instance_context *link;
	struct vulkan_context *context;
	struct vulkan_object identity;
	VkResult status;
	VkResult cleanup;

	/* Retains session storage for the complete lifetime of the public instance. */
	context = vulkan_allocate(&instance->object.allocator, sizeof(*context), __alignof__(struct vulkan_context), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	if (context == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Non-Venus or incompatible GPU nodes do not prevent discovery of other devices. */
	status = vulkan_context_open(context, path);
	if (status != VK_SUCCESS) {
		vulkan_free(&instance->object.allocator, context);
		if (status == VK_ERROR_INITIALIZATION_FAILED ||
		    status == VK_ERROR_INCOMPATIBLE_DRIVER ||
		    status == VK_ERROR_DEVICE_LOST)
			return VK_SUCCESS;
		return status;
	}

	/* Excludes an older recognized profile before any native instance or transport storage exists. */
	if (context->strict_queue == VK_FALSE ||
	    context->native_quiescence == VK_FALSE ||
	    (context->capabilities & (GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY)) != (GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY)) {
		cleanup = vulkan_context_close(context);
		vulkan_free(&instance->object.allocator, context);
		if (cleanup != VK_SUCCESS)
			return cleanup;

		/* Succeeded: compatible nodes remain discoverable, including an empty supported inventory. */
		return VK_SUCCESS;
	}

	/* Keeps each remote instance identity separate from the public composite instance. */
	link = vulkan_allocate(&instance->object.allocator, sizeof(*link), __alignof__(struct vulkan_instance_context), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	if (link == NULL) {
		cleanup = vulkan_context_close(context);
		(void)cleanup;
		vulkan_free(&instance->object.allocator, context);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Assigns a process-unique remote identity without publishing a fake API object. */
	memset(link, 0, sizeof(*link));
	memset(&identity, 0, sizeof(identity));
	status = vulkan_object_reserve_id(&identity);
	if (status != VK_SUCCESS) {
		cleanup = vulkan_context_close(context);
		(void)cleanup;
		vulkan_free(&instance->object.allocator, context);
		vulkan_free(&instance->object.allocator, link);
		return status;
	}

	/* Links rollback ownership before any host Vulkan object can be created. */
	link->instance = instance;
	link->context = context;
	link->wire_id = identity.wire_id;
	link->next = instance->contexts;
	instance->contexts = link;
	status = instance_create_remote(link, info);
	if (status == VK_SUCCESS) {
		/* Validates all native physical devices before exposing this namespace. */
		status = instance_enumerate_remote(link);
	}

	/* An incompatible GPU does not prevent independent compatible GPU discovery. */
	if (status == VK_ERROR_INCOMPATIBLE_DRIVER) {
		instance->contexts = link->next;
		cleanup = vulkan_context_close(context);
		vulkan_free(&instance->object.allocator, context);
		vulkan_free(&instance->object.allocator, link);
		if (cleanup != VK_SUCCESS)
			return cleanup;

		/* Succeeded: no public physical handle refers to the excluded GPU. */
		return VK_SUCCESS;
	}

	/* Retains real allocation and protocol failures instead of publishing partial discovery. */
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: this GPU's namespace and physical handles belong to the public instance. */
	return VK_SUCCESS;
}

/* Creates a private host instance with the protocol's required native API version. */
static VkResult
instance_create_remote(
	struct vulkan_instance_context *link,
	const VkInstanceCreateInfo *info)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo create;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint64_t identity;
	VkBool32 present;
	VkResult status;

	/*
	 * Preserves application identity while using Vulkan 1.1 internally for
	 * Venus queues, or 1.2 with a renderer that answers the Vulkan 1.2
	 * queries, so a Venus host offers the 1.2 commands (vkCreateRenderPass2)
	 * the library sends it.
	 */
	memset(&application, 0, sizeof(application));
	if (info->pApplicationInfo != NULL)
		application = *info->pApplicationInfo;

	/* Local WSI and layers do not become unsupported native host extension requests. */
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pNext = NULL;
	application.apiVersion = VK_MAKE_VERSION(1, 1, 0);
	if (link->context->core12_queries)
		application.apiVersion = VK_MAKE_VERSION(1, 2, 0);
	create = *info;
	create.pNext = NULL;
	create.pApplicationInfo = &application;
	create.enabledLayerCount = 0;
	create.ppEnabledLayerNames = NULL;
	create.enabledExtensionCount = 0;
	create.ppEnabledExtensionNames = NULL;

	/* Encodes real application fields and an independently reserved instance identity. */
	vulkan_writer_init_for_object(&writer, &link->instance->object);
	vulkan_command_begin(&writer, GPU_OP_CREATE_INSTANCE);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkInstanceCreateInfo(&writer, &create);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, link->wire_id);
	status = vulkan_command_execute(link->context, &writer, 24, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		status = vulkan_reply_finish(link->context, &reader, status);
		return status;
	}

	/* Requires the renderer to acknowledge exactly this requested namespace identity. */
	present = vulkan_reply_pointer(&reader);
	identity = vulkan_read_u64(&reader);
	if (!present || identity != link->wire_id)
		reader.error = VK_ERROR_DEVICE_LOST;

	/* Rejects incomplete framing before completing native object creation. */
	status = vulkan_reply_finish(link->context, &reader, VK_SUCCESS);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the checked reply records the new private renderer instance. */
	return VK_SUCCESS;
}

/* Creates stable local identities for a renderer's initial physical-device snapshot. */
static VkResult
instance_enumerate_remote(
	struct vulkan_instance_context *link)
{
	struct VkInstance_T *instance;
	struct VkPhysicalDevice_T **devices;
	struct VkPhysicalDevice_T **combined;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint64_t array_count;
	uint64_t identity;
	uint32_t count;
	uint32_t actual;
	uint32_t index;
	size_t bytes;
	VkBool32 present;
	VkResult status;

	/* Asks for the count before allocating a dynamically sized local identity array. */
	instance = link->instance;
	vulkan_writer_init_for_object(&writer, &instance->object);
	vulkan_command_begin(&writer, GPU_OP_ENUMERATE_PHYSICAL_DEVICES);
	vulkan_write_u64(&writer, link->wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 0);
	status = vulkan_command_execute(link->context, &writer, 28, &reader, VK_TRUE);
	if (status != VK_SUCCESS) {
		vulkan_writer_finish(&writer);
		status = vulkan_reply_finish(link->context, &reader, status);
		return status;
	}

	/* Validates the count response independently of any later array allocation. */
	present = vulkan_reply_pointer(&reader);
	count = vulkan_read_u32(&reader);
	array_count = vulkan_read_u64(&reader);
	if (!present || array_count != 0)
		reader.error = VK_ERROR_DEVICE_LOST;

	/* Refuses malformed count framing before it can determine host allocation sizes. */
	status = vulkan_reply_finish(link->context, &reader, VK_SUCCESS);
	if (status != VK_SUCCESS || count == 0) {
		vulkan_writer_finish(&writer);
		return status;
	}

	/* Checks both pointer storage and the future response extent without truncation. */
	bytes = (size_t)count * sizeof(*devices);
	if (bytes / sizeof(*devices) != count || (uint64_t)count * 8 + 28 > SIZE_MAX) {
		vulkan_writer_finish(&writer);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Allocates all local identities before host enumeration can create object mappings. */
	devices = vulkan_allocate(&instance->object.allocator, bytes, __alignof__(void *), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	if (devices == NULL) {
		vulkan_writer_finish(&writer);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* A zeroed array gives every partial creation failure one exact cleanup owner. */
	memset(devices, 0, bytes);
	status = VK_SUCCESS;
	for (index = 0; index < count; index++) {
		status = vulkan_object_alloc(sizeof(*devices[index]), __alignof__(struct VkPhysicalDevice_T), VULKAN_OBJECT_PHYSICAL_DEVICE, &instance->object, link->context, NULL, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE, &object);
		if (status != VK_SUCCESS)
			break;

		/* Associates each unexposed physical object with its public and remote instances. */
		devices[index] = (struct VkPhysicalDevice_T *)object;
		devices[index]->instance = instance;
		devices[index]->instance_context = link;
		status = vulkan_object_reserve_id(object);
		if (status != VK_SUCCESS)
			break;
	}

	/* Builds the array request only after every requested output identity exists. */
	if (status == VK_SUCCESS) {
		vulkan_command_begin(&writer, GPU_OP_ENUMERATE_PHYSICAL_DEVICES);
		vulkan_write_u64(&writer, link->wire_id);
		vulkan_write_u64(&writer, 1);
		vulkan_write_u32(&writer, count);
		vulkan_write_u64(&writer, count);

		/* The renderer fills the host handles behind these guest-owned identities. */
		for (index = 0; index < count; index++) {
			vulkan_write_u64(&writer, devices[index]->object.wire_id);
		}

		/* Retrieves a finite response sized from the previously checked device count. */
		status = vulkan_command_execute(link->context, &writer, (size_t)count * 8 + 28, &reader, VK_TRUE);
		if (status == VK_SUCCESS) {
			/* The renderer caches this instance snapshot, so its count must remain stable. */
			present = vulkan_reply_pointer(&reader);
			actual = vulkan_read_u32(&reader);
			array_count = vulkan_read_u64(&reader);
			if (!present ||
			    actual != count ||
			    array_count != count)
				reader.error = VK_ERROR_DEVICE_LOST;

			/* Rejects every foreign or reordered identity before publishing the array. */
			for (index = 0;
			     index < count && reader.error == VK_SUCCESS;
			     index++) {
				identity = vulkan_read_u64(&reader);
				if (identity != devices[index]->object.wire_id)
					reader.error = VK_ERROR_DEVICE_LOST;
			}
		}

		/* Retains a failed response as a failure of the whole unexposed discovery batch. */
		status = vulkan_reply_finish(link->context, &reader, status);
	}

	/* Local input bytes never remain live after this enumeration call. */
	vulkan_writer_finish(&writer);
	if (status == VK_SUCCESS) {
		/* Queries all fixed properties before the public instance gains any new device. */
		for (index = 0; index < count; index++) {
			status = physical_load(devices[index]);
			if (status != VK_SUCCESS)
				break;
		}
	}

	/* Checks the composite count before combining different GPU namespaces. */
	combined = NULL;
	if (status == VK_SUCCESS) {
		if (count > UINT32_MAX - instance->physical_device_count) {
			status = VK_ERROR_OUT_OF_HOST_MEMORY;
		} else {
			/* Refuses pointer-array size overflow on either supported application ABI. */
			bytes = (size_t)(count + instance->physical_device_count) * sizeof(*combined);
			if (bytes / sizeof(*combined) != count + instance->physical_device_count)
				status = VK_ERROR_OUT_OF_HOST_MEMORY;
		}
	}

	/* Allocates replacement composite storage before changing existing instance ownership. */
	if (status == VK_SUCCESS) {
		combined = vulkan_allocate(&instance->object.allocator, bytes, __alignof__(void *), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
		if (combined == NULL)
			status = VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Reclaims every private physical allocation when discovery cannot complete. */
	if (status != VK_SUCCESS) {
		for (index = 0; index < count; index++) {
			physical_finish(devices[index]);
		}

		/* Returns temporary array storage through the instance's effective allocator. */
		vulkan_free(&instance->object.allocator, devices);
		return status;
	}

	/* Preserves prior GPU handles while appending the newly completed namespace. */
	if (instance->physical_device_count != 0)
		memcpy(combined, instance->physical_devices, (size_t)instance->physical_device_count * sizeof(*combined));

	/* Publishes each already complete physical object to its parent lifetime list. */
	for (index = 0; index < count; index++) {
		combined[instance->physical_device_count + index] = devices[index];
		status = vulkan_object_publish(&devices[index]->object);
		if (status != VK_SUCCESS)
			abort();
	}

	/* Commits the new array only after all allocations and remote queries succeeded. */
	vulkan_free(&instance->object.allocator, instance->physical_devices);
	vulkan_free(&instance->object.allocator, devices);
	instance->physical_devices = combined;
	instance->physical_device_count += count;

	/* Succeeded: all physical handles are stable for the lifetime of this instance. */
	return VK_SUCCESS;
}

/* Retrieves properties whose immutable output is cached by the public physical device. */
static VkResult
physical_load(
	struct VkPhysicalDevice_T *physical)
{
	struct vulkan_reader reader;
	VkResult status;
	VkBool32 present;
	VkBool32 coherent;
	VkBool32 local;
	uint32_t index;
	VkMemoryPropertyFlags *flags;

	/* Retrieves renderer limits using their typed protocol fields rather than native layout. */
	status = physical_query_begin(physical, GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES, &reader);
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (present)
			vulkan_decode_VkPhysicalDeviceProperties(&reader, &physical->properties);
	}

	/* Refuses an incomplete or malformed physical-property snapshot. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Venus's queue path requires a native Vulkan 1.1 implementation. */
	if (physical->properties.apiVersion < VK_MAKE_VERSION(1, 1, 0))
		return VK_ERROR_INCOMPATIBLE_DRIVER;

	/* The local mapped-address guarantee; the public version is decided below from the renderer's answers. */
	physical->core11.api_version = physical->properties.apiVersion;
	physical->properties.apiVersion = VK_API_VERSION_1_0;
	physical->properties.limits.minMemoryMapAlignment = 64;
	physical->properties.deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1] = '\0';
	status = physical_query_begin(physical, GPU_OP_GET_PHYSICAL_DEVICE_FEATURES, &reader);
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (present)
			vulkan_decode_VkPhysicalDeviceFeatures(&reader, &physical->features);
	}

	/* Preserves every host-supported 1.0 feature that the complete core encoder implements. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* The Vulkan 1.1 and 1.2 facts, where the renderer answers their queries, and the version they decide. */
	status = physical_load_core11(physical);
	if (status != VK_SUCCESS)
		return status;
	status = physical_load_core12(physical);
	if (status != VK_SUCCESS)
		return status;
	physical_version(physical);

	/* Retrieves original memory type indices before filtering unsupported cache behavior. */
	status = physical_query_begin(physical, GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES, &reader);
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (present)
			vulkan_decode_VkPhysicalDeviceMemoryProperties(&reader, &physical->memory);
	}

	/* Requires a complete memory snapshot before trusting indices or property flags. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Enforces the fixed ABI bounds independently of the renderer's advertised counts. */
	if (physical->memory.memoryTypeCount > VK_MAX_MEMORY_TYPES || physical->memory.memoryHeapCount > VK_MAX_MEMORY_HEAPS)
		return VK_ERROR_INCOMPATIBLE_DRIVER;

	/* Keeps native indices while withholding host access that requires unavailable cache calls. */
	coherent = VK_FALSE;
	local = VK_FALSE;
	for (index = 0; index < physical->memory.memoryTypeCount; index++) {
		/* Filters this native index in place so resource type bits remain meaningful. */
		flags = &physical->memory.memoryTypes[index].propertyFlags;

		/* A memory type cannot refer to a heap outside this same physical snapshot. */
		if (physical->memory.memoryTypes[index].heapIndex >= physical->memory.memoryHeapCount)
			return VK_ERROR_INCOMPATIBLE_DRIVER;

		/* Noncoherent host cache operations have no supported pinned renderer dispatch. */
		if (!(*flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			*flags &= ~(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

		/* Counts actual standard host-visible coherent and device-local choices. */
		if ((*flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			coherent = VK_TRUE;

		/* Requires an actual native device-local choice independently of CPU access. */
		if (*flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
			local = VK_TRUE;
	}

	/* Refuses a physical device that cannot meet mandatory standard memory availability. */
	if (!coherent || !local)
		return VK_ERROR_INCOMPATIBLE_DRIVER;

	/* Queries stable native family indices before exposing logical-device creation. */
	status = physical_load_queues(physical);
	if (status != VK_SUCCESS)
		return status;

	/* Local WSI can pair a renderer with a separate display node or a Wayland connection. */
	physical->supported_extensions = VULKAN_DEVICE_SWAPCHAIN | VULKAN_DEVICE_DISPLAY_SWAPCHAIN;

	/* A present may name its changed rectangles; the Wayland commit damages only them (BUG-221). */
	physical->supported_extensions |= VULKAN_DEVICE_INCREMENTAL_PRESENT;

	/*
	 * Every swapchain's presents retire on its worker, which signals a
	 * present's fences when it retires the present, on both backends.
	 */
	physical->supported_extensions |= VULKAN_DEVICE_SWAPCHAIN_MAINTENANCE1;

	/*
	 * The memory requirement queries of VK_KHR_get_memory_requirements2 and
	 * the dedicated allocation that depends on them are answered in this
	 * library, on every renderer (ws103-p003).
	 */
	physical->supported_extensions |= VULKAN_DEVICE_MEMORY_REQUIREMENTS2 | VULKAN_DEVICE_DEDICATED_ALLOCATION;

	/*
	 * Per-resource format queries determine the actual external memory
	 * profiles.  A node with image sharing only (GPU_CAP_SHARE, the native
	 * i915) imports the images other opens export (the Wayland WSI's
	 * buffers) and exports nothing (ws035-p066).
	 */
	if (physical->object.context->capabilities & (GPU_CAP_ALLOCATION_SHARE | GPU_CAP_SHARE))
		physical->supported_extensions |= VULKAN_DEVICE_EXTERNAL_MEMORY | VULKAN_DEVICE_EXTERNAL_MEMORY_FD;
	if (physical->object.context->capabilities & GPU_CAP_FENCE)
		physical->supported_extensions |= VULKAN_DEVICE_EXTERNAL_FENCE | VULKAN_DEVICE_EXTERNAL_FENCE_FD;

	/*
	 * VK_EXT_display_control needs the node's topology events, power and
	 * refresh requests (ws113-p003).  A renderer whose displays live on a
	 * separate display-only node does not offer it.
	 */
	if ((physical->object.context->capabilities & (GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS | GPU_CAP_DISPLAY_CONTROL)) ==
	    (GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS | GPU_CAP_DISPLAY_CONTROL))
		physical->supported_extensions |= VULKAN_DEVICE_DISPLAY_CONTROL;

	/* Asks the codec operations of each family when the renderer promised video decode. */
	status = physical_load_video(physical);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the snapshot combines real renderer capability and real guest transport support. */
	return VK_SUCCESS;
}

/*
 * The structures of the Vulkan 1.1 features and properties chained to the
 * *2 queries, first to last.
 *
 * Values referenced: the Vulkan 1.1 specification (VkStructureType of
 * the 1.1 feature and property structures).
 */
static const uint32_t core11_feature_types[] = {
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES
};
static const uint32_t core11_property_types[] = {
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES
};

/*
 * The structures of the Vulkan 1.2 features and properties chained to the
 * *2 queries: one of each.
 */
static const uint32_t core12_feature_types[] = {
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
};
static const uint32_t core12_property_types[] = {
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES
};

/* The number of entries of a table. */
#define CORE11_COUNT(table)		((uint32_t)(sizeof(table) / sizeof((table)[0])))

/*
 * Asks the renderer for its Vulkan 1.1 features and properties, when it
 * answers those queries (a Venus host, or a native renderer that promises
 * it), into physical->core11.  A renderer not asked leaves the facts
 * unreported; one that answers malformed loses the session.
 */
static VkResult
physical_load_core11(
	struct VkPhysicalDevice_T *physical)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkPhysicalDeviceFeatures features;
	VkPhysicalDeviceProperties properties;
	VkBool32 present;
	VkResult status;

	/* A renderer that does not answer the queries reports nothing of Vulkan 1.1. */
	physical->core11.reported = VK_FALSE;
	if (!physical->object.context->core11_queries)
		return VK_SUCCESS;

	/* The features: the 1.1 structures chained to VkPhysicalDeviceFeatures2. */
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_FEATURES2);
	vulkan_write_u64(&writer, physical->object.wire_id);
	physical_chain_request(&writer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, core11_feature_types, CORE11_COUNT(core11_feature_types));
	status = vulkan_command_execute(physical->object.context, &writer, 4096U, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (!present)
			reader.error = VK_ERROR_DEVICE_LOST;
		physical_chain_reply(&reader, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, core11_feature_types, CORE11_COUNT(core11_feature_types));
		physical_core11_features(&reader, &physical->core11);
		vulkan_decode_VkPhysicalDeviceFeatures(&reader, &features);
	}

	/* A malformed answer is the renderer's failure, not an absent feature. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* The properties: the 1.1 structures chained to VkPhysicalDeviceProperties2. */
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2);
	vulkan_write_u64(&writer, physical->object.wire_id);
	physical_chain_request(&writer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, core11_property_types, CORE11_COUNT(core11_property_types));
	status = vulkan_command_execute(physical->object.context, &writer, 4096U, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (!present)
			reader.error = VK_ERROR_DEVICE_LOST;
		physical_chain_reply(&reader, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, core11_property_types, CORE11_COUNT(core11_property_types));
		physical_core11_properties(&reader, &physical->core11);
		vulkan_decode_VkPhysicalDeviceProperties(&reader, &properties);
	}

	/* The same for the properties. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the renderer reported its Vulkan 1.1 facts. */
	physical->core11.reported = VK_TRUE;
	return VK_SUCCESS;
}

/*
 * Asks the renderer for its Vulkan 1.2 features and properties into
 * physical->core12, when it answers the Vulkan 1.1 queries, its own
 * version is 1.2 or later and it answers the 1.2 queries (a Venus host,
 * or a native renderer that promises it).  A renderer not asked leaves
 * the facts unreported; one that answers malformed loses the session.
 */
static VkResult
physical_load_core12(
	struct VkPhysicalDevice_T *physical)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkPhysicalDeviceFeatures features;
	VkPhysicalDeviceProperties properties;
	VkBool32 *flag;
	VkBool32 present;
	VkResult status;
	uint32_t index;

	/* A renderer that is not asked reports nothing of Vulkan 1.2. */
	memset(&physical->core12, 0, sizeof(physical->core12));
	if (!physical->core11.reported || physical->core11.api_version < VK_MAKE_VERSION(1, 2, 0))
		return VK_SUCCESS;
	if (!physical->object.context->core12_queries)
		return VK_SUCCESS;

	/* The features: VkPhysicalDeviceVulkan12Features chained to VkPhysicalDeviceFeatures2. */
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_FEATURES2);
	vulkan_write_u64(&writer, physical->object.wire_id);
	physical_chain_request(&writer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, core12_feature_types, CORE11_COUNT(core12_feature_types));
	status = vulkan_command_execute(physical->object.context, &writer, 4096U, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (!present)
			reader.error = VK_ERROR_DEVICE_LOST;
		physical_chain_reply(&reader, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, core12_feature_types, CORE11_COUNT(core12_feature_types));
		for (index = 0U; index < VULKAN_CORE12_FEATURE_COUNT; index++) {
			flag = vulkan_core12_feature(&physical->core12.features, index);
			*flag = vulkan_read_u32(&reader);
		}
		vulkan_decode_VkPhysicalDeviceFeatures(&reader, &features);
	}

	/* A malformed answer is the renderer's failure, not an absent feature. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* The properties: VkPhysicalDeviceVulkan12Properties chained to VkPhysicalDeviceProperties2. */
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2);
	vulkan_write_u64(&writer, physical->object.wire_id);
	physical_chain_request(&writer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, core12_property_types, CORE11_COUNT(core12_property_types));
	status = vulkan_command_execute(physical->object.context, &writer, 4096U, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (!present)
			reader.error = VK_ERROR_DEVICE_LOST;
		physical_chain_reply(&reader, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, core12_property_types, CORE11_COUNT(core12_property_types));
		physical_core12_properties(&reader, &physical->core12.properties);
		vulkan_decode_VkPhysicalDeviceProperties(&reader, &properties);
	}

	/* The same for the properties. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the renderer reported its Vulkan 1.2 facts. */
	physical->core12.reported = VK_TRUE;
	return VK_SUCCESS;
}

/*
 * Reads the fields of VkPhysicalDeviceVulkan12Properties in their
 * declared order: the driver's identity, the float controls, the limits
 * of descriptor indexing, the depth and stencil resolve, the min and max
 * filter, the timeline semaphore and the integer colour samples.
 *
 * Values referenced: the Vulkan 1.2 specification (section 40.1,
 * VkPhysicalDeviceVulkan12Properties); a character array is its length
 * and its bytes, as the device name of VkPhysicalDeviceProperties is.
 */
static void
physical_core12_properties(
	struct vulkan_reader *reader,
	VkPhysicalDeviceVulkan12Properties *properties)
{
	uint64_t count;

	/* The driver's identity: its number, name, information and conformance. */
	properties->driverID = (VkDriverId)vulkan_read_u32(reader);
	count = vulkan_read_u64(reader);
	if (count != VK_MAX_DRIVER_NAME_SIZE)
		reader->error = VK_ERROR_DEVICE_LOST;
	vulkan_read_bytes(reader, properties->driverName, VK_MAX_DRIVER_NAME_SIZE);
	properties->driverName[VK_MAX_DRIVER_NAME_SIZE - 1] = '\0';
	count = vulkan_read_u64(reader);
	if (count != VK_MAX_DRIVER_INFO_SIZE)
		reader->error = VK_ERROR_DEVICE_LOST;
	vulkan_read_bytes(reader, properties->driverInfo, VK_MAX_DRIVER_INFO_SIZE);
	properties->driverInfo[VK_MAX_DRIVER_INFO_SIZE - 1] = '\0';
	properties->conformanceVersion.major = (uint8_t)vulkan_read_u32(reader);
	properties->conformanceVersion.minor = (uint8_t)vulkan_read_u32(reader);
	properties->conformanceVersion.subminor = (uint8_t)vulkan_read_u32(reader);
	properties->conformanceVersion.patch = (uint8_t)vulkan_read_u32(reader);

	/* The float controls. */
	properties->denormBehaviorIndependence = (VkShaderFloatControlsIndependence)vulkan_read_u32(reader);
	properties->roundingModeIndependence = (VkShaderFloatControlsIndependence)vulkan_read_u32(reader);
	properties->shaderSignedZeroInfNanPreserveFloat16 = vulkan_read_u32(reader);
	properties->shaderSignedZeroInfNanPreserveFloat32 = vulkan_read_u32(reader);
	properties->shaderSignedZeroInfNanPreserveFloat64 = vulkan_read_u32(reader);
	properties->shaderDenormPreserveFloat16 = vulkan_read_u32(reader);
	properties->shaderDenormPreserveFloat32 = vulkan_read_u32(reader);
	properties->shaderDenormPreserveFloat64 = vulkan_read_u32(reader);
	properties->shaderDenormFlushToZeroFloat16 = vulkan_read_u32(reader);
	properties->shaderDenormFlushToZeroFloat32 = vulkan_read_u32(reader);
	properties->shaderDenormFlushToZeroFloat64 = vulkan_read_u32(reader);
	properties->shaderRoundingModeRTEFloat16 = vulkan_read_u32(reader);
	properties->shaderRoundingModeRTEFloat32 = vulkan_read_u32(reader);
	properties->shaderRoundingModeRTEFloat64 = vulkan_read_u32(reader);
	properties->shaderRoundingModeRTZFloat16 = vulkan_read_u32(reader);
	properties->shaderRoundingModeRTZFloat32 = vulkan_read_u32(reader);
	properties->shaderRoundingModeRTZFloat64 = vulkan_read_u32(reader);

	/* The limits of descriptor indexing. */
	properties->maxUpdateAfterBindDescriptorsInAllPools = vulkan_read_u32(reader);
	properties->shaderUniformBufferArrayNonUniformIndexingNative = vulkan_read_u32(reader);
	properties->shaderSampledImageArrayNonUniformIndexingNative = vulkan_read_u32(reader);
	properties->shaderStorageBufferArrayNonUniformIndexingNative = vulkan_read_u32(reader);
	properties->shaderStorageImageArrayNonUniformIndexingNative = vulkan_read_u32(reader);
	properties->shaderInputAttachmentArrayNonUniformIndexingNative = vulkan_read_u32(reader);
	properties->robustBufferAccessUpdateAfterBind = vulkan_read_u32(reader);
	properties->quadDivergentImplicitLod = vulkan_read_u32(reader);
	properties->maxPerStageDescriptorUpdateAfterBindSamplers = vulkan_read_u32(reader);
	properties->maxPerStageDescriptorUpdateAfterBindUniformBuffers = vulkan_read_u32(reader);
	properties->maxPerStageDescriptorUpdateAfterBindStorageBuffers = vulkan_read_u32(reader);
	properties->maxPerStageDescriptorUpdateAfterBindSampledImages = vulkan_read_u32(reader);
	properties->maxPerStageDescriptorUpdateAfterBindStorageImages = vulkan_read_u32(reader);
	properties->maxPerStageDescriptorUpdateAfterBindInputAttachments = vulkan_read_u32(reader);
	properties->maxPerStageUpdateAfterBindResources = vulkan_read_u32(reader);
	properties->maxDescriptorSetUpdateAfterBindSamplers = vulkan_read_u32(reader);
	properties->maxDescriptorSetUpdateAfterBindUniformBuffers = vulkan_read_u32(reader);
	properties->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = vulkan_read_u32(reader);
	properties->maxDescriptorSetUpdateAfterBindStorageBuffers = vulkan_read_u32(reader);
	properties->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = vulkan_read_u32(reader);
	properties->maxDescriptorSetUpdateAfterBindSampledImages = vulkan_read_u32(reader);
	properties->maxDescriptorSetUpdateAfterBindStorageImages = vulkan_read_u32(reader);
	properties->maxDescriptorSetUpdateAfterBindInputAttachments = vulkan_read_u32(reader);

	/* The depth and stencil resolve, the min and max filter, the timeline and the integer samples. */
	properties->supportedDepthResolveModes = vulkan_read_u32(reader);
	properties->supportedStencilResolveModes = vulkan_read_u32(reader);
	properties->independentResolveNone = vulkan_read_u32(reader);
	properties->independentResolve = vulkan_read_u32(reader);
	properties->filterMinmaxSingleComponentFormats = vulkan_read_u32(reader);
	properties->filterMinmaxImageComponentMapping = vulkan_read_u32(reader);
	properties->maxTimelineSemaphoreValueDifference = vulkan_read_u64(reader);
	properties->framebufferIntegerColorSampleCounts = vulkan_read_u32(reader);
}

/*
 * Reads the fields of the 1.1 feature structures, last of the chain
 * first: shader draw parameters, sampler YCbCr conversion, protected
 * memory, variable pointers, 16-bit storage, multiview.
 */
static void
physical_core11_features(
	struct vulkan_reader *reader,
	struct vulkan_core11_facts *facts)
{
	/* Shader draw parameters, sampler YCbCr conversion and protected memory. */
	facts->shader_draw_parameters = vulkan_read_u32(reader);
	facts->sampler_ycbcr_conversion = vulkan_read_u32(reader);
	facts->protected_memory = vulkan_read_u32(reader);

	/* Variable pointers. */
	facts->variable_pointers_storage_buffer = vulkan_read_u32(reader);
	facts->variable_pointers = vulkan_read_u32(reader);

	/* 16-bit storage. */
	facts->storage_buffer_16bit = vulkan_read_u32(reader);
	facts->uniform_storage_buffer_16bit = vulkan_read_u32(reader);
	facts->storage_push_constant_16 = vulkan_read_u32(reader);
	facts->storage_input_output_16 = vulkan_read_u32(reader);

	/* Multiview. */
	facts->multiview = vulkan_read_u32(reader);
	facts->multiview_geometry_shader = vulkan_read_u32(reader);
	facts->multiview_tessellation_shader = vulkan_read_u32(reader);
}

/*
 * Reads the fields of the 1.1 property structures, last of the chain
 * first: maintenance 3, protected memory, multiview, point clipping,
 * subgroup.
 */
static void
physical_core11_properties(
	struct vulkan_reader *reader,
	struct vulkan_core11_facts *facts)
{
	/* Maintenance 3 and protected memory. */
	facts->max_per_set_descriptors = vulkan_read_u32(reader);
	facts->max_memory_allocation_size = vulkan_read_u64(reader);
	facts->protected_no_fault = vulkan_read_u32(reader);

	/* Multiview and point clipping. */
	facts->max_multiview_view_count = vulkan_read_u32(reader);
	facts->max_multiview_instance_index = vulkan_read_u32(reader);
	facts->point_clipping = vulkan_read_u32(reader);

	/* The subgroup. */
	facts->subgroup_size = vulkan_read_u32(reader);
	facts->subgroup_stages = vulkan_read_u32(reader);
	facts->subgroup_operations = vulkan_read_u32(reader);
	facts->subgroup_quad_all_stages = vulkan_read_u32(reader);
}

/* Writes the output chain's headers of a *2 query: the outer structure, then each chained one. */
static void
physical_chain_request(
	struct vulkan_writer *writer,
	uint32_t outer,
	const uint32_t *types,
	uint32_t count)
{
	uint32_t index;

	/* The outer structure, present. */
	vulkan_write_u64(writer, 1U);
	vulkan_write_u32(writer, outer);

	/* Each chained structure, the last followed by an absent next. */
	for (index = 0U; index < count; index++) {
		vulkan_write_u64(writer, 1U);
		vulkan_write_u32(writer, types[index]);
	}
	vulkan_write_u64(writer, 0U);
}

/* Checks the reply's chain headers of a *2 query against the request's. */
static void
physical_chain_reply(
	struct vulkan_reader *reader,
	uint32_t outer,
	const uint32_t *types,
	uint32_t count)
{
	uint64_t next;
	uint32_t type;
	uint32_t index;

	/* The outer structure. */
	type = vulkan_read_u32(reader);
	if (type != outer)
		reader->error = VK_ERROR_DEVICE_LOST;

	/* Each chained structure in the same order, then the end of the chain. */
	for (index = 0U; index < count; index++) {
		next = vulkan_read_u64(reader);
		type = vulkan_read_u32(reader);
		if (next != 1U || type != types[index])
			reader->error = VK_ERROR_DEVICE_LOST;
	}
	next = vulkan_read_u64(reader);
	if (next != 0U)
		reader->error = VK_ERROR_DEVICE_LOST;
}

/*
 * Decides the device's public version from its Vulkan 1.1 facts, and
 * says why a later version is not named when VULKAN_ZEDBSD_VERSION_LOG
 * asks for it (the log of the version rule, for a person finding why a
 * device names 1.0).
 */
static void
physical_version(
	struct VkPhysicalDevice_T *physical)
{
	const char *reasons[VULKAN_VERSION_REASONS];
	const char *variable;
	uint32_t version;
	uint32_t count;
	uint32_t index;

	/* The rule, the same for every renderer. */
	version = vulkan_version_decide(VULKAN_LIBRARY_VERSION, &physical->core11, &physical->core12, reasons, VULKAN_VERSION_REASONS, &count);
	physical->properties.apiVersion = version;

	/* The reasons, on request. */
	variable = getenv("VULKAN_ZEDBSD_VERSION_LOG");
	if (variable == NULL || variable[0] == '\0')
		return;
	fprintf(stderr, "VULKAN VERSION device=\"%s\" renderer=%u.%u version=%u.%u reasons=%u\n",
	    physical->properties.deviceName,
	    VK_VERSION_MAJOR(physical->core11.api_version),
	    VK_VERSION_MINOR(physical->core11.api_version),
	    VK_VERSION_MAJOR(version),
	    VK_VERSION_MINOR(version),
	    count);
	for (index = 0U; index < count && index < VULKAN_VERSION_REASONS; index++)
		fprintf(stderr, "VULKAN VERSION reason: %s\n", reasons[index]);
}

/* Requests one fixed immutable physical output, including partial-array framing. */
static VkResult
physical_query_begin(
	struct VkPhysicalDevice_T *physical,
	uint32_t opcode,
	struct vulkan_reader *reader)
{
	struct vulkan_writer writer;
	VkResult status;

	/* Encodes the physical identity and one required output structure. */
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, opcode);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u64(&writer, 1);

	/* Memory properties contain arrays of nested output structures on this protocol. */
	if (opcode == GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES) {
		vulkan_write_u64(&writer, VK_MAX_MEMORY_TYPES);
		vulkan_write_u64(&writer, VK_MAX_MEMORY_HEAPS);
	}

	/* A 4-KiB reply exceeds the maximum fixed 1.0 physical structure wire extent. */
	status = vulkan_command_execute(physical->object.context, &writer, 4096, reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the caller owns a framed response ready for typed field decoding. */
	return VK_SUCCESS;
}

/* Fetches a dynamically sized immutable array of native queue families. */
static VkResult
physical_load_queues(
	struct VkPhysicalDevice_T *physical)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkQueueFamilyProperties *families;
	VkResult status;
	VkBool32 present;
	VkBool32 promised;
	uint32_t count;
	uint32_t actual;
	uint32_t index;
	uint32_t remaining;
	uint32_t additional;
	uint64_t array_count;
	size_t bytes;

	/* Queries the native family count without assuming a platform-specific queue topology. */
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 0);
	status = vulkan_command_execute(physical->object.context, &writer, 24, &reader, VK_FALSE);
	if (status != VK_SUCCESS) {
		vulkan_writer_finish(&writer);
		status = vulkan_reply_finish(physical->object.context, &reader, status);
		return status;
	}

	/* Validates count-only framing before the host count controls an allocation. */
	present = vulkan_reply_pointer(&reader);
	count = vulkan_read_u32(&reader);
	array_count = vulkan_read_u64(&reader);
	if (!present ||
	    array_count != 0 ||
	    count == 0)
		reader.error = VK_ERROR_DEVICE_LOST;

	/* Checks both host and wire array extents on the active application ABI. */
	status = vulkan_reply_finish(physical->object.context, &reader, VK_SUCCESS);
	bytes = (size_t)count * sizeof(*families);
	if (status == VK_SUCCESS &&
	    (bytes / sizeof(*families) != count ||
	     (uint64_t)count * 24 + 24 > SIZE_MAX))
		status = VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Leaves no partial cache visible after a count or allocation-size failure. */
	if (status != VK_SUCCESS) {
		vulkan_writer_finish(&writer);
		return status;
	}

	/* Allocates family storage under the instance-owned physical-device lifetime. */
	families = vulkan_allocate(&physical->object.allocator, bytes, __alignof__(VkQueueFamilyProperties), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	if (families == NULL) {
		vulkan_writer_finish(&writer);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* An array of these primitive-only records has no input placeholder fields. */
	memset(families, 0, bytes);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, count);
	vulkan_write_u64(&writer, count);
	status = vulkan_command_execute(physical->object.context, &writer, (size_t)count * 24 + 24, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	if (status == VK_SUCCESS) {
		/* The physical-device family snapshot is immutable during its instance lifetime. */
		present = vulkan_reply_pointer(&reader);
		actual = vulkan_read_u32(&reader);
		array_count = vulkan_read_u64(&reader);
		if (!present ||
		    actual != count ||
		    array_count != count)
			reader.error = VK_ERROR_DEVICE_LOST;

		/* Decodes each complete queue family without copying native structure padding. */
		for (index = 0;
		     index < count && reader.error == VK_SUCCESS;
		     index++) {
			vulkan_decode_VkQueueFamilyProperties(&reader, &families[index]);
		}
	}

	/* Returns unexposed family storage if the renderer response cannot be trusted. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS) {
		vulkan_free(&physical->object.allocator, families);
		return status;
	}

	/*
	 * A renderer that does not promise video decode may still report the
	 * video queue flags of its own driver (a host renderer over a driver
	 * with video).  Without the video extensions a family with those flags
	 * would break the queue family contract, so the flags are withheld.
	 */
	promised = physical_video_promised(physical);
	if (!promised) {
		for (index = 0; index < count; index++) {
			/* Keeps the graphics, compute and transfer flags of the same family. */
			families[index].queueFlags &= ~(VkQueueFlags)VULKAN_VIDEO_QUEUE_FLAGS;
		}
	}

	/* Every reported family needs at least one distinct renderer queue timeline. */
	if (count >= VULKAN_QUEUE_TIMELINE_COUNT) {
		vulkan_free(&physical->object.allocator, families);
		return VK_ERROR_INCOMPATIBLE_DRIVER;
	}

	/* Preserves native family indices while fitting their entire advertised queue set. */
	remaining = VULKAN_QUEUE_TIMELINE_COUNT - 1U - count;
	for (index = 0; index < count; index++) {
		/* A native family without a usable queue cannot satisfy the public family contract. */
		if (families[index].queueCount == 0) {
			vulkan_free(&physical->object.allocator, families);
			return VK_ERROR_INCOMPATIBLE_DRIVER;
		}

		/* Gives each later family its reserved queue before distributing extra capacity. */
		additional = families[index].queueCount - 1U;
		if (additional > remaining)
			additional = remaining;

		/* The published total never requires a sixty-fourth device timeline. */
		families[index].queueCount = 1U + additional;
		remaining -= additional;
	}

	/* Publishes the complete cache together with its element count. */
	physical->queue_families = families;
	physical->queue_family_count = count;

	/* Succeeded: logical-device validation can use preserved native families and implementable queue counts. */
	return VK_SUCCESS;
}

/* Asks the renderer which video codec operations each queue family performs. */
static VkResult
physical_load_video(
	struct VkPhysicalDevice_T *physical)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;
	VkBool32 present;
	VkBool32 h264;
	VkBool32 h265;
	VkBool32 encode_h264;
	uint32_t count;
	uint32_t index;
	uint32_t operations;
	uint32_t promised;

	/* Only the native word's promises make the question meaningful. */
	promised = 0;
	if (physical->object.context->video_h264)
		promised |= VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	if (physical->object.context->video_h265)
		promised |= VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
	if (physical->object.context->video_encode_h264)
		promised |= VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
	if (promised == 0)
		return VK_SUCCESS;

	/* Asks one codec operation word for every family the snapshot holds. */
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u32(&writer, physical->queue_family_count);
	status = vulkan_command_execute(physical->object.context, &writer, 16 + (size_t)physical->queue_family_count * 4, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		status = vulkan_reply_finish(physical->object.context, &reader, status);
		return status;
	}

	/* The renderer answers for exactly the families it already reported. */
	present = vulkan_reply_pointer(&reader);
	count = vulkan_read_u32(&reader);
	if (present && count != physical->queue_family_count)
		reader.error = VK_ERROR_DEVICE_LOST;

	/*
	 * Keeps only the decode operations this library carries whose codec
	 * the native word promised, and only on a family that has the video
	 * decode flag.
	 */
	h264 = VK_FALSE;
	h265 = VK_FALSE;
	encode_h264 = VK_FALSE;
	for (index = 0;
	     index < count && reader.error == VK_SUCCESS;
	     index++) {
		/* Reads the family's word and keeps the promised video operations. */
		operations = vulkan_read_u32(&reader);
		operations &= promised;

		/* A family without the video decode flag cannot run a decode. */
		if ((physical->queue_families[index].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) == 0)
			operations &= ~(uint32_t)(VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR);

		/* A family without the video encode flag cannot run an encode. */
		if ((physical->queue_families[index].queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) == 0)
			operations &= ~(uint32_t)VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;

		/* A family that codes a codec is what that codec's extension stands for. */
		physical->queue_video_operations[index] = operations;
		if ((operations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0)
			h264 = VK_TRUE;
		if ((operations & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) != 0)
			h265 = VK_TRUE;
		if ((operations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) != 0)
			encode_h264 = VK_TRUE;
	}

	/* A malformed answer fails the physical device rather than hiding its video family. */
	status = vulkan_reply_finish(physical->object.context, &reader, VK_SUCCESS);
	if (status != VK_SUCCESS)
		return status;

	/*
	 * The renderer promised video coding but no family performs a promised
	 * codec.  The extensions stay unnamed and the video flags of every
	 * family are withheld, as on a renderer without the promise.
	 */
	if (!h264 && !h265 && !encode_h264) {
		for (index = 0; index < physical->queue_family_count; index++) {
			/* Keeps the graphics, compute and transfer flags of the same family. */
			physical->queue_families[index].queueFlags &= ~(VkQueueFlags)VULKAN_VIDEO_QUEUE_FLAGS;
		}

		/* Succeeded: the physical device has no video coding. */
		return VK_SUCCESS;
	}

	/*
	 * Names the video extensions and synchronization2, the barrier
	 * form they are written against (translated in this library).
	 */
	physical->supported_extensions |= VULKAN_DEVICE_SYNCHRONIZATION2;
	physical->supported_extensions |= VULKAN_DEVICE_VIDEO_QUEUE;
	if (h264 || h265)
		physical->supported_extensions |= VULKAN_DEVICE_VIDEO_DECODE_QUEUE;
	if (h264)
		physical->supported_extensions |= VULKAN_DEVICE_VIDEO_DECODE_H264;
	if (h265)
		physical->supported_extensions |= VULKAN_DEVICE_VIDEO_DECODE_H265;
	if (encode_h264) {
		physical->supported_extensions |= VULKAN_DEVICE_VIDEO_ENCODE_QUEUE;
		physical->supported_extensions |= VULKAN_DEVICE_VIDEO_ENCODE_H264;
	}

	/* Succeeded: the families that decode each codec are known. */
	return VK_SUCCESS;
}

/* Tells whether the native renderer promised a video decode family of any codec. */
static VkBool32
physical_video_promised(
	const struct VkPhysicalDevice_T *physical)
{
	/* Any codec's bit in the native word is a promise of the video queue flags and features. */
	if (physical->object.context->video_h264 ||
	    physical->object.context->video_h265 ||
	    physical->object.context->video_encode_h264)
		return VK_TRUE;

	/* No promise: the backend's video flags and features name nothing usable. */
	return VK_FALSE;
}

/* Reclaims one local physical-device snapshot after dependent logical devices are gone. */
static void
physical_finish(
	struct VkPhysicalDevice_T *physical)
{
	/* Accepts never-allocated array slots during discovery rollback. */
	if (physical == NULL)
		return;

	/* Physical snapshots follow the current compatible instance destruction policy. */
	physical->object.allocator = physical->instance->object.allocator;
	vulkan_free(&physical->object.allocator, physical->queue_families);
	vulkan_object_free(&physical->object);

	/* Succeeded: the local physical identity no longer owns host allocations. */
	return;
}

/* Consumes instance-owned host storage and closes every private renderer namespace. */
static void
instance_finish(
	struct VkInstance_T *instance)
{
	struct vulkan_instance_context *link;
	struct vulkan_instance_context *next;
	VkResult cleanup;
	uint32_t index;
	int mutex_status;

	/* Reclaims local physical metadata before invalidating its associated GPU sessions. */
	for (index = 0; index < instance->physical_device_count; index++) {
		physical_finish(instance->physical_devices[index]);
	}

	/* Closing each session also destroys any partially created remote instance objects. */
	link = instance->contexts;
	while (link != NULL) {
		next = link->next;
		cleanup = vulkan_context_close(link->context);
		(void)cleanup;
		vulkan_free(&instance->object.allocator, link->context);
		vulkan_free(&instance->object.allocator, link);
		link = next;
	}

	/* Releases the final composite handle array and host synchronization state. */
	vulkan_free(&instance->object.allocator, instance->physical_devices);
	mutex_status = pthread_mutex_destroy(&instance->mutex);
	if (mutex_status != 0)
		abort();

	/* Releases the root object after its synchronization lifetime has ended. */
	vulkan_object_free(&instance->object);

	/* Succeeded: every remaining remote object belongs to a consumed kernel descriptor. */
	return;
}

/* Copies a finite extension list with standard count-only and incomplete semantics. */
static VkResult
enumerate_extensions(
	const VkExtensionProperties *available,
	uint32_t total,
	uint32_t *count,
	VkExtensionProperties *properties)
{
	uint32_t copied;

	/* Count-only queries return the full available extension set. */
	if (properties == NULL) {
		*count = total;
		return VK_SUCCESS;
	}

	/* Limits output to the caller's capacity without changing any adjacent storage. */
	copied = *count;
	if (copied > total)
		copied = total;

	/* Copies exact public records and publishes the number actually returned. */
	memcpy(properties, available, (size_t)copied * sizeof(*properties));
	*count = copied;
	if (copied < total)
		return VK_INCOMPLETE;

	/* Succeeded: all available extension records fit the caller's array. */
	return VK_SUCCESS;
}

/* Selects only decimal GPU character-device names owned by the common GPU subsystem. */
static VkBool32
gpu_node_name(
	const char *name)
{
	size_t index;

	/* Excludes short names and unrelated device prefixes before suffix inspection. */
	if (name[0] != 'g' ||
	    name[1] != 'p' ||
	    name[2] != 'u' ||
	    name[3] == '\0')
		return VK_FALSE;

	/* Every remaining character must be part of an ordinary numeric GPU instance name. */
	for (index = 3; name[index] != '\0'; index++) {
		if (name[index] < '0' || name[index] > '9')
			return VK_FALSE;
	}

	/* Succeeded: the path names one ordinary dynamically registered GPU instance. */
	return VK_TRUE;
}
