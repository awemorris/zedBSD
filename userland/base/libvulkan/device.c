/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Owns logical devices and cached queues independently of application scenes.
 */

#include <stdlib.h>
#include <string.h>
#include "internal.h"

static VkResult device_validate(struct VkPhysicalDevice_T *physical, const VkDeviceCreateInfo *info, uint64_t *extensions, uint32_t *queues);
static VkResult device_create_remote(struct VkDevice_T *device, const VkDeviceCreateInfo *info);
static VkResult device_queue_remote(struct VkQueue_T *queue);
static void device_destroy_remote(struct VkDevice_T *device);
static void device_finish(struct VkDevice_T *device);
static VkResult queue_timeline_reserve(struct vulkan_context *context, uint32_t *timeline);
static void queue_timeline_release(struct vulkan_context *context, uint32_t timeline);

/*
 * Creates a standard logical device with exactly the requested features and queues.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(
	VkPhysicalDevice physicalDevice,
	const VkDeviceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkDevice *pDevice)
{
	struct VkPhysicalDevice_T *physical;
	struct VkDevice_T *device;
	struct VkQueue_T *queue;
	struct vulkan_object *object;
	const VkDeviceQueueCreateInfo *queue_info;
	uint64_t extensions;
	uint32_t count;
	uint32_t family;
	uint32_t index;
	uint32_t created;
	size_t bytes;
	VkResult status;
	int mutex_status;

	/* Validates supported capability and native family selection before remote side effects. */
	physical = vulkan_physical_device(physicalDevice);
	status = device_validate(physical, pCreateInfo, &extensions, &count);
	if (status != VK_SUCCESS)
		return status;

	/* Uses the logical device's allocator, with the instance policy as its fallback. */
	status = vulkan_object_alloc(sizeof(*device), __alignof__(struct VkDevice_T), VULKAN_OBJECT_DEVICE, &physical->object, physical->object.context, pAllocator, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Initializes device state before any queue object may reference its lifetime. */
	device = (struct VkDevice_T *)object;
	device->physical = physical;
	device->enabled_extensions = extensions;
	if (pCreateInfo->pEnabledFeatures != NULL)
		device->enabled_features = *pCreateInfo->pEnabledFeatures;

	/* A failed private mutex allocation cannot leave an exposed device handle. */
	mutex_status = pthread_mutex_init(&device->mutex, NULL);
	if (mutex_status != 0) {
		vulkan_object_free(object);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Reserves the device identity before encoding its output handle. */
	status = vulkan_object_reserve_id(object);
	if (status != VK_SUCCESS) {
		device_finish(device);
		return status;
	}

	/* Checks the dynamically sized queue pointer array on the active user ABI. */
	bytes = (size_t)count * sizeof(*device->queues);
	if (bytes / sizeof(*device->queues) != count) {
		device_finish(device);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Allocates all queue ownership slots before creating a native logical device. */
	device->queues = vulkan_allocate(&device->object.allocator, bytes, __alignof__(void *), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (device->queues == NULL) {
		device_finish(device);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Zeroed slots ensure a failed queue allocation has only one local cleanup path. */
	memset(device->queues, 0, bytes);
	created = 0;
	status = VK_SUCCESS;
	for (family = 0; family < pCreateInfo->queueCreateInfoCount; family++) {
		queue_info = &pCreateInfo->pQueueCreateInfos[family];

		/* Preallocates every requested queue and its protocol-specific timeline slot. */
		for (index = 0; index < queue_info->queueCount; index++) {
			status = vulkan_object_alloc(sizeof(*queue), __alignof__(struct VkQueue_T), VULKAN_OBJECT_QUEUE, &device->object, device->object.context, NULL, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &object);
			if (status != VK_SUCCESS)
				break;

			/* Initializes the queue's private submission serialization before ownership transfer. */
			queue = (struct VkQueue_T *)object;
			queue->device = device;
			queue->family = queue_info->queueFamilyIndex;
			queue->index = index;
			mutex_status = pthread_mutex_init(&queue->mutex, NULL);
			if (mutex_status != 0) {
				vulkan_object_free(object);
				status = VK_ERROR_OUT_OF_HOST_MEMORY;
				break;
			}

			/* Transfers the fully initialized local queue into the device's rollback array. */
			device->queues[created++] = queue;
			device->queue_count = created;
			status = vulkan_object_reserve_id(object);
			if (status != VK_SUCCESS)
				break;

			/* Keeps timeline identities unique across logical devices sharing this GPU session. */
			status = queue_timeline_reserve(device->object.context, &queue->timeline_index);
			if (status != VK_SUCCESS)
				break;
		}

		/* Stops before reading additional create records after a resource shortage. */
		if (status != VK_SUCCESS)
			break;
	}

	/* No renderer device exists when local resource reservation fails. */
	if (status != VK_SUCCESS) {
		device_finish(device);
		return status;
	}

	/* Creates native queues using the caller's actual family counts and priorities. */
	status = device_create_remote(device, pCreateInfo);
	if (status != VK_SUCCESS) {
		device_finish(device);
		return status;
	}

	/* Retrieves each native queue exactly once as required by the pinned Venus protocol. */
	for (index = 0; index < device->queue_count; index++) {
		status = device_queue_remote(device->queues[index]);
		if (status != VK_SUCCESS)
			break;
	}

	/* Destroys the native device before releasing any reserved queue timeline on failure. */
	if (status != VK_SUCCESS) {
		device_destroy_remote(device);
		device_finish(device);
		return status;
	}

	/* Publishes the device only after every standard queue handle is locally cached. */
	status = vulkan_object_publish(&device->object);
	if (status != VK_SUCCESS) {
		device_destroy_remote(device);
		device_finish(device);
		return status;
	}

	/* Queue publication now gives ordinary device teardown a complete child list. */
	for (index = 0; index < device->queue_count; index++) {
		status = vulkan_object_publish(&device->queues[index]->object);
		if (status != VK_SUCCESS) {
			device_destroy_remote(device);
			device_finish(device);
			return status;
		}
	}

	/* The caller sees no partially created logical-device handle. */
	*pDevice = (VkDevice)device;

	/* Succeeded: every enabled feature and requested queue has a real native implementation. */
	return VK_SUCCESS;
}

/*
 * Destroys an externally synchronized logical device and its cached queue identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(
	VkDevice device,
	const VkAllocationCallbacks *pAllocator)
{
	struct VkDevice_T *owner;

	/* Null device destruction has no local or remote lifetime to consume. */
	owner = vulkan_device(device);
	if (owner == NULL)
		return;

	/* Uses this command's compatible allocation callbacks for device-scoped cleanup. */
	if (pAllocator != NULL) {
		owner->object.allocator.callbacks = *pAllocator;
		owner->object.allocator.has_callbacks = VK_TRUE;
	}

	/* Releases internal WSI resources while the native device can still service their destruction. */
	vulkan_wsi_device_finish(owner);
	device_destroy_remote(owner);
	device_finish(owner);

	/* Succeeded: the device and every implicitly owned queue have been consumed. */
	return;
}

/*
 * Returns the stable queue identity cached during successful logical-device creation.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue(
	VkDevice device,
	uint32_t queueFamilyIndex,
	uint32_t queueIndex,
	VkQueue *pQueue)
{
	struct VkDevice_T *owner;
	struct VkQueue_T *queue;
	uint32_t index;

	/* A repeated public lookup must never reinitialize a renderer queue's object ID. */
	owner = vulkan_device(device);
	for (index = 0; index < owner->queue_count; index++) {
		queue = owner->queues[index];

		/* Returns exactly the queue selected by the standard family and local index. */
		if (queue->family == queueFamilyIndex && queue->index == queueIndex) {
			*pQueue = (VkQueue)queue;
			return;
		}
	}

	/* A nonexistent queue violates valid usage and is never replaced by another family. */
	*pQueue = VK_NULL_HANDLE;

	/* Succeeded: the output cannot accidentally alias an unrelated valid queue. */
	return;
}

/* Validates feature support, selected local extensions, and queue-family capacities. */
static VkResult
device_validate(
	struct VkPhysicalDevice_T *physical,
	const VkDeviceCreateInfo *info,
	uint64_t *extensions,
	uint32_t *queues)
{
	VkBool32 requested[sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32)];
	VkBool32 available[sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32)];
	const VkDeviceQueueCreateInfo *queue;
	uint64_t bits;
	uint32_t total;
	uint32_t index;
	uint32_t earlier;
	int match;

	/* No unavailable legacy device layer can be silently enabled. */
	if (info->enabledLayerCount != 0)
		return VK_ERROR_LAYER_NOT_PRESENT;

	/* Copies scalar feature fields without aliasing the public structure through another type. */
	memset(requested, 0, sizeof(requested));
	if (info->pEnabledFeatures != NULL)
		memcpy(requested, info->pEnabledFeatures, sizeof(requested));

	/* Compares requested bits against this device snapshot, preserving caller storage. */
	memcpy(available, &physical->features, sizeof(available));

	/* Every requested feature must be exposed by this physical-device snapshot. */
	for (index = 0; index < sizeof(requested) / sizeof(requested[0]); index++) {
		/* Rejects enabling functionality that this same physical device did not advertise. */
		if (requested[index] && !available[index])
			return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	/* Recognizes only local device extensions with complete implementation support. */
	bits = 0;
	for (index = 0; index < info->enabledExtensionCount; index++) {
		/* Recognizes the ordinary swapchain API before checking its local dependencies. */
		match = strcmp(info->ppEnabledExtensionNames[index], VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_DEVICE_SWAPCHAIN;
			continue;
		}

		/* Shared display swapchains remain a separately selected capability. */
		match = strcmp(info->ppEnabledExtensionNames[index], VK_KHR_DISPLAY_SWAPCHAIN_EXTENSION_NAME);
		if (match == 0) {
			bits |= VULKAN_DEVICE_DISPLAY_SWAPCHAIN;
			continue;
		}

		/* Does not forward renderer-private extensions as guest capabilities. */
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}

	/* Enforces device support and instance-level WSI dependencies together. */
	if ((bits & physical->supported_extensions) != bits)
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* A device swapchain cannot outlive an absent instance surface capability. */
	if ((bits & VULKAN_DEVICE_SWAPCHAIN) && !(physical->instance->enabled_extensions & VULKAN_INSTANCE_SURFACE))
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Shared direct-display swapchains need both the device and instance prerequisites. */
	if ((bits & VULKAN_DEVICE_DISPLAY_SWAPCHAIN) &&
	    (!(bits & VULKAN_DEVICE_SWAPCHAIN) ||
	     !(physical->instance->enabled_extensions & VULKAN_INSTANCE_DISPLAY)))
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Counts actual requested queues without imposing a library object-array ceiling. */
	total = 0;
	for (index = 0; index < info->queueCreateInfoCount; index++) {
		queue = &info->pQueueCreateInfos[index];

		/* A queue family must exist and contain every queue requested from it. */
		if (queue->queueFamilyIndex >= physical->queue_family_count || queue->queueCount == 0)
			return VK_ERROR_INITIALIZATION_FAILED;

		/* Keeps this family reservation within the stable reported native capacity. */
		if (queue->queueCount > physical->queue_families[queue->queueFamilyIndex].queueCount)
			return VK_ERROR_INITIALIZATION_FAILED;

		/* Core 1.0 permits one creation record for each selected queue family. */
		for (earlier = 0; earlier < index; earlier++) {
			if (info->pQueueCreateInfos[earlier].queueFamilyIndex == queue->queueFamilyIndex)
				return VK_ERROR_INITIALIZATION_FAILED;
		}

		/* Keeps total count arithmetic independent of the host pointer width. */
		if (queue->queueCount > UINT32_MAX - total)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Reserves one local pointer and one renderer timeline for every requested queue. */
		total += queue->queueCount;
	}

	/* A logical device must request at least one actual native queue. */
	if (total == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Publishes capability validation only after every request record passed. */
	*extensions = bits;
	*queues = total;

	/* Succeeded: local reservation and native creation may now proceed. */
	return VK_SUCCESS;
}

/* Creates one native logical device with renderer-independent local WSI extension selection. */
static VkResult
device_create_remote(
	struct VkDevice_T *device,
	const VkDeviceCreateInfo *info)
{
	VkDeviceCreateInfo create;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;
	VkBool32 present;
	uint64_t identity;

	/* Local presentation is implemented by this library rather than a native host surface. */
	create = *info;
	create.pNext = NULL;
	create.enabledLayerCount = 0;
	create.ppEnabledLayerNames = NULL;
	create.enabledExtensionCount = 0;
	create.ppEnabledExtensionNames = NULL;

	/* Preserves real caller feature bits, queue priorities, and family indices. */
	vulkan_writer_init_for_object(&writer, &device->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateDevice);
	vulkan_write_u64(&writer, device->physical->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkDeviceCreateInfo(&writer, &create);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, device->object.wire_id);
	status = vulkan_command_execute(device->object.context, &writer, 24, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		status = vulkan_reply_finish(device->object.context, &reader, status);
		return status;
	}

	/* Requires exactly the reserved logical-device identity in the successful reply. */
	present = vulkan_reply_pointer(&reader);
	identity = vulkan_read_u64(&reader);
	if (!present || identity != device->object.wire_id)
		reader.error = VK_ERROR_DEVICE_LOST;

	/* Rejects incomplete framing before completing native object creation. */
	status = vulkan_reply_finish(device->object.context, &reader, VK_SUCCESS);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: all native queues exist and await their single ID-assignment lookup. */
	return VK_SUCCESS;
}

/* Binds one cached queue identity to its reserved renderer timeline exactly once. */
static VkResult
device_queue_remote(
	struct VkQueue_T *queue)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;
	VkBool32 present;
	uint64_t identity;

	/* Uses the required private GetDeviceQueue2 path while the public API remains 1.0. */
	vulkan_writer_init_for_object(&writer, &queue->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkGetDeviceQueue2);
	vulkan_write_u64(&writer, queue->device->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, 1000145003U);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, 1000384005U);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u32(&writer, queue->timeline_index);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u32(&writer, queue->family);
	vulkan_write_u32(&writer, queue->index);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, queue->object.wire_id);
	status = vulkan_command_execute(queue->object.context, &writer, 20, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		status = vulkan_reply_finish(queue->object.context, &reader, status);
		return status;
	}

	/* Verifies the output pointer and exact identity before caching a public queue. */
	present = vulkan_reply_pointer(&reader);
	identity = vulkan_read_u64(&reader);
	if (!present || identity != queue->object.wire_id)
		reader.error = VK_ERROR_DEVICE_LOST;

	/* Rejects incomplete framing before completing native object creation. */
	status = vulkan_reply_finish(queue->object.context, &reader, VK_SUCCESS);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: repeated public lookups can return the local queue without another wire call. */
	return VK_SUCCESS;
}

/* Consumes the native logical device before releasing timeline IDs for reuse. */
static void
device_destroy_remote(
	struct VkDevice_T *device)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;

	/* Device destruction has one identity and one deliberately null native allocator. */
	vulkan_writer_init_for_object(&writer, &device->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkDestroyDevice);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u64(&writer, 0);
	status = vulkan_command_execute(device->object.context, &writer, 4, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(device->object.context, &reader, status);

	/* Never reuse queue timelines while an unconsumed native device could still own them. */
	if (status != VK_SUCCESS)
		__atomic_store_n(&device->object.context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);

	/* Succeeded: the native device was consumed or the whole session is terminally lost. */
	return;
}

/* Releases initialized local queues and the device's own effective host allocations. */
static void
device_finish(
	struct VkDevice_T *device)
{
	struct VkQueue_T *queue;
	uint32_t index;
	int status;

	/* Every stored queue owns an initialized mutex, even during creation rollback. */
	for (index = 0; index < device->queue_count; index++) {
		queue = device->queues[index];
		queue_timeline_release(device->object.context, queue->timeline_index);
		status = pthread_mutex_destroy(&queue->mutex);
		if (status != 0)
			abort();

		/* Implicit queues use the current compatible device destruction allocator. */
		queue->object.allocator = device->object.allocator;
		vulkan_object_free(&queue->object);
	}

	/* Returns the dynamically allocated queue array after all queue references are gone. */
	vulkan_free(&device->object.allocator, device->queues);
	status = pthread_mutex_destroy(&device->mutex);
	if (status != 0)
		abort();

	/* Withdraws physical-device parent ownership before releasing the logical device. */
	vulkan_object_free(&device->object);

	/* Succeeded: the local device owns no remaining queue or allocation. */
	return;
}

/* Reserves one protocol-required unique queue timeline without an arbitrary object limit. */
static VkResult
queue_timeline_reserve(
	struct vulkan_context *context,
	uint32_t *timeline)
{
	uint64_t observed;
	uint64_t desired;
	uint64_t mask;
	uint32_t index;
	VkBool32 changed;

	/* Atomically competes with other logical-device creations on the same GPU session. */
	observed = __atomic_load_n(&context->queue_timelines, __ATOMIC_ACQUIRE);
	while (1) {
		/* Zero is reserved by the pinned renderer for its non-device command timeline. */
		for (index = 1; index < VULKAN_QUEUE_TIMELINE_COUNT; index++) {
			mask = UINT64_C(1) << index;
			if (!(observed & mask))
				break;
		}

		/* Exhaustion is a real renderer resource shortage, without modifying existing devices. */
		if (index == VULKAN_QUEUE_TIMELINE_COUNT)
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;

		/* Publishes this reservation only if no concurrent creator consumed the same slot. */
		desired = observed | mask;
		changed = __atomic_compare_exchange_n(&context->queue_timelines, &observed, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
		if (changed)
			break;
	}

	/* The caller releases this slot only after native device destruction or local rollback. */
	*timeline = index;

	/* Succeeded: this native queue owns a distinct usable renderer timeline. */
	return VK_SUCCESS;
}

/* Returns a consumed native timeline reservation without affecting other live queues. */
static void
queue_timeline_release(
	struct vulkan_context *context,
	uint32_t timeline)
{
	uint64_t mask;

	/* Partial creation may stop before a queue acquired any native timeline slot. */
	if (timeline == 0)
		return;

	/* Clears only the exact protocol slot associated with this consumed queue. */
	mask = UINT64_C(1) << timeline;
	__atomic_fetch_and(&context->queue_timelines, ~mask, __ATOMIC_ACQ_REL);

	/* Succeeded: later logical devices may reserve the now-unused renderer timeline. */
	return;
}
