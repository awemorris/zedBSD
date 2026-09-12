/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Dynamic Vulkan swapchains over ordinary images and a separate native adapter.
 * Presentation uses one semaphore-consuming submission, then completed readback.
 */

#include "wsi-internal.h"

#include <string.h>
#include <time.h>

#define WSI_IMAGE_AVAILABLE	0U
#define WSI_IMAGE_ACQUIRED	1U
#define WSI_IMAGE_PRESENTING	2U
#define WSI_COMPLETION_TIMEOUT	10000000000ULL

/* One ordinary bound image whose allocation lives with a shared image group. */
struct wsi_image {
	VkImage image;
	VkDeviceMemory memory;
};

/* Images may be shared by multiple display swapchains until the final release. */
struct wsi_image_group {
	struct vulkan_allocator allocator;
	struct VkDevice_T *device;
	struct wsi_image *images;
	uint32_t count;
	uint32_t references;
};

/*
 * One device-owned swapchain with independent image acquisition and native lease.
 * The global swapchain mutex protects its state without holding queue locks.
 */
struct vulkan_swapchain {
	struct vulkan_object object;
	struct VkDevice_T *device;
	struct vulkan_surface *surface;
	struct vulkan_swapchain *next;
	struct wsi_image_group *group;
	void *lease;
	uint8_t *states;
	VkFormat format;
	VkExtent2D extent;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	void *readback_pixels;
	uint64_t frame;
	uint64_t sequence;
	uint32_t cursor;
	VkBool32 retired;
	VkBool32 presenting;
};

/* One finite presentation transaction, completely acquired before submission. */
struct wsi_present_job {
	struct vulkan_allocator allocator;
	struct VkQueue_T *queue;
	struct vulkan_swapchain **chains;
	VkResult *results;
	VkPipelineStageFlags *stages;
	void **composed;
	VkDisplayPresentInfoKHR display;
	VkBool32 has_display;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	uint32_t count;
	VkBool32 submitted;
	VkBool32 reserved;
};

/* Serializes only WSI linkage and image ownership, never a pending GPU fence. */
static pthread_mutex_t swapchain_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct vulkan_swapchain *swapchains;

static struct vulkan_swapchain *swapchain_get(VkSwapchainKHR handle);
static VkResult swapchain_create(struct VkDevice_T *device, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *allocator, struct wsi_image_group *shared, struct vulkan_swapchain **result);
static VkResult swapchain_validate(struct VkDevice_T *device, const VkSwapchainCreateInfoKHR *info, struct vulkan_surface **surface);
static VkResult swapchain_publish(struct vulkan_swapchain *chain);
static void swapchain_free(struct vulkan_swapchain *chain);
static VkResult swapchain_group_create(struct vulkan_swapchain *chain, const VkSwapchainCreateInfoKHR *info);
static void swapchain_group_free(struct wsi_image_group *group);
static VkResult swapchain_image_create(struct vulkan_swapchain *chain, const VkSwapchainCreateInfoKHR *info, struct wsi_image *image);
static VkResult swapchain_readback_create(struct vulkan_swapchain *chain);
static VkResult swapchain_allocate_memory(struct vulkan_swapchain *chain, const VkMemoryRequirements *requirements, VkMemoryPropertyFlags required, const struct vulkan_allocator *policy, VkDeviceMemory *memory);
static const VkAllocationCallbacks *swapchain_allocator(const struct vulkan_allocator *allocator);
static VkBool32 swapchain_compatible(const VkSwapchainCreateInfoKHR *first, const VkSwapchainCreateInfoKHR *second);
static VkResult swapchain_current(struct vulkan_swapchain *chain);
static VkResult swapchain_now(uint64_t *nanoseconds);
static VkResult present_prepare(struct wsi_present_job *job, struct VkQueue_T *queue, const VkPresentInfoKHR *info);
static VkResult present_reserve(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static VkResult present_record(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static void present_copy(VkCommandBuffer command, struct vulkan_swapchain *chain, uint32_t index);
static VkResult present_submit(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static void present_compose(struct wsi_present_job *job, uint32_t index, struct vulkan_wsi_pixels *pixels);
static VkResult present_native(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static void present_finish(struct wsi_present_job *job, const VkPresentInfoKHR *info, VkResult error);
static VkResult present_combine(VkResult first, VkResult second);
static VkResult swapchain_device_lost(struct VkDevice_T *device);

/*
 * Creates an independent swapchain and retires the specified old chain on entry.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSwapchainKHR(
	VkDevice device_handle,
	const VkSwapchainCreateInfoKHR *info,
	const VkAllocationCallbacks *allocator,
	VkSwapchainKHR *result)
{
	struct VkDevice_T *device;
	struct vulkan_swapchain *chain;
	VkResult error;

	/* Failed creation never exposes a partial public handle. */
	if (result == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;
	*result = VK_NULL_HANDLE;
	device = vulkan_device(device_handle);
	if (device == NULL || info == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Complete acquisition precedes publication and native surface association. */
	error = swapchain_create(device, info, allocator, NULL, &chain);
	if (error != VK_SUCCESS)
		return error;
	error = swapchain_publish(chain);
	if (error != VK_SUCCESS) {
		swapchain_free(chain);
		return error;
	}

	*result = (VkSwapchainKHR)vulkan_nondispatchable_handle(&chain->object);

	/* Succeeded: the new chain owns dynamic images and one native presentation lease. */
	return VK_SUCCESS;
}

/*
 * Creates all display swapchains together, sharing the same ordinary VkImages.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSharedSwapchainsKHR(
	VkDevice device_handle,
	uint32_t count,
	const VkSwapchainCreateInfoKHR *infos,
	const VkAllocationCallbacks *allocator,
	VkSwapchainKHR *results)
{
	struct VkDevice_T *device;
	struct vulkan_swapchain **chains;
	struct wsi_image_group *group;
	struct vulkan_allocator policy;
	VkResult error;
	VkBool32 compatible;
	size_t bytes;
	uint32_t index;

	/* Every output remains null unless the whole shared creation succeeds. */
	if (results == NULL ||
	    infos == NULL ||
	    count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;
	for (index = 0U; index < count; index++)
		results[index] = VK_NULL_HANDLE;
	device = vulkan_device(device_handle);
	if (device == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The standard extension requires identical presentable image properties. */
	for (index = 1U; index < count; index++) {
		compatible = swapchain_compatible(&infos[0], &infos[index]);
		if (compatible == VK_FALSE)
			return VK_ERROR_INCOMPATIBLE_DISPLAY_KHR;
	}

	/* A transaction array remains independent of each chain's published list link. */
	memset(&policy, 0, sizeof(policy));
	if (allocator != NULL) {
		policy.callbacks = *allocator;
		policy.has_callbacks = VK_TRUE;
	}

	bytes = (size_t)count * sizeof(*chains);
	if (bytes / sizeof(*chains) != count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	chains = vulkan_allocate(&policy, bytes, sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (chains == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(chains, 0, bytes);

	/* Builds one shared image group and independent native leases transactionally. */
	group = NULL;
	error = VK_SUCCESS;
	for (index = 0U; index < count; index++) {
		error = swapchain_create(device, &infos[index], allocator, group, &chains[index]);
		if (error != VK_SUCCESS)
			goto cleanup;
		group = chains[index]->group;
	}

	/* Publication cannot allocate; rollback still owns every chain independently. */
	for (index = 0U; index < count; index++) {
		error = swapchain_publish(chains[index]);
		if (error != VK_SUCCESS)
			goto cleanup;
		results[index] = (VkSwapchainKHR)vulkan_nondispatchable_handle(&chains[index]->object);
	}

cleanup:
	/* Removes published and unpublished chains only after an all-or-nothing failure. */
	if (error != VK_SUCCESS) {
		/* Retires every failed transaction member and leaves no partial public output. */
		for (index = 0U; index < count; index++) {
			/* Only successfully allocated chains own resources which need retirement. */
			if (chains[index] != NULL)
				swapchain_free(chains[index]);

			/* Failed shared creation never leaves a usable member of the batch. */
			results[index] = VK_NULL_HANDLE;
		}
	}

	/* Releases transaction storage independently from successful public chain ownership. */
	vulkan_free(&policy, chains);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: every chain references the same complete image array. */
	return VK_SUCCESS;
}

/*
 * Destroys one chain while shared images remain live for surviving chains.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR(
	VkDevice device_handle,
	VkSwapchainKHR handle,
	const VkAllocationCallbacks *allocator)
{
	struct VkDevice_T *device;
	struct vulkan_swapchain *chain;

	/* Null destruction handles are valid independently of allocator selection. */
	if (handle == VK_NULL_HANDLE)
		return;
	device = vulkan_device(device_handle);
	chain = swapchain_get(handle);
	if (chain == NULL || chain->device != device)
		return;

	/* Compatible destruction callbacks may carry a different current user-data pointer. */
	if (allocator != NULL) {
		chain->object.allocator.callbacks = *allocator;
		chain->object.allocator.has_callbacks = VK_TRUE;
	}

	/* Valid applications externally synchronize use before destroying a chain. */
	swapchain_free(chain);

	/* Succeeded: this chain no longer owns images or native display references. */
	return;
}

/*
 * Returns stable standard image handles, including identical shared-chain handles.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetSwapchainImagesKHR(
	VkDevice device_handle,
	VkSwapchainKHR handle,
	uint32_t *count,
	VkImage *images)
{
	struct VkDevice_T *device;
	struct vulkan_swapchain *chain;
	uint32_t written;
	uint32_t index;

	/* A foreign logical device cannot inspect another device's swapchain objects. */
	device = vulkan_device(device_handle);
	chain = swapchain_get(handle);
	if (chain == NULL || count == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Prevents another logical device from acquiring or enumerating this swapchain. */
	if (chain->device != device)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Count-only output is independent of the caller's initial count value. */
	if (images == NULL) {
		*count = chain->group->count;
		return VK_SUCCESS;
	}

	written = *count;
	if (written > chain->group->count)
		written = chain->group->count;
	for (index = 0U; index < written; index++)
		images[index] = chain->group->images[index].image;
	*count = written;
	if (written < chain->group->count)
		return VK_INCOMPLETE;

	/* Succeeded: all presentable images have been returned in stable order. */
	return VK_SUCCESS;
}

/*
 * Acquires only a completely reusable image and signals the standard sync objects.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImageKHR(
	VkDevice device_handle,
	VkSwapchainKHR handle,
	uint64_t timeout,
	VkSemaphore semaphore,
	VkFence fence,
	uint32_t *result)
{
	struct VkDevice_T *device;
	struct vulkan_swapchain *chain;
	struct timespec pause;
	uint64_t started;
	uint64_t now;
	uint32_t offset;
	uint32_t index;
	VkResult error;
	VkBool32 acquired;

	/* Core synchronization code owns the actual Vulkan semaphore/fence payloads. */
	device = vulkan_device(device_handle);
	chain = swapchain_get(handle);
	if (chain == NULL || result == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Prevents another logical device from acquiring or enumerating this swapchain. */
	if (chain->device != device)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires an acquire completion primitive before handing image ownership to the application. */
	if (semaphore == VK_NULL_HANDLE && fence == VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;
	error = swapchain_now(&started);
	if (error != VK_SUCCESS)
		return error;
	pause.tv_sec = 0;
	pause.tv_nsec = 1000000L;

	/* A finite timeout never consumes an image or signals either synchronization object. */
	for (;;) {
		error = swapchain_current(chain);
		if (error != VK_SUCCESS)
			return error;
		pthread_mutex_lock(&swapchain_mutex);

		/* Stops new acquisition after a replacement has retired this swapchain. */
		if (chain->retired != VK_FALSE) {
			pthread_mutex_unlock(&swapchain_mutex);
			return VK_ERROR_OUT_OF_DATE_KHR;
		}

		/* Round-robin ownership does not impose an application presentation order. */
		acquired = VK_FALSE;
		index = chain->cursor;
		for (offset = 0U; offset < chain->group->count; offset++) {
			/* Skips an image still owned by rendering or presentation. */
			if (chain->states[index] != WSI_IMAGE_AVAILABLE) {
				index++;

				/* Wraps the acquisition cursor without changing any image ownership state. */
				if (index == chain->group->count)
					index = 0U;
				continue;
			}

			chain->states[index] = WSI_IMAGE_ACQUIRED;
			acquired = VK_TRUE;
			break;
		}

		pthread_mutex_unlock(&swapchain_mutex);
		if (acquired != VK_FALSE)
			break;

		/* Standard timeout zero reports immediate unavailability without sleeping. */
		if (timeout == 0U)
			return VK_NOT_READY;
		error = swapchain_now(&now);
		if (error != VK_SUCCESS)
			return error;

		/* Ends a finite acquire wait only after its monotonic deadline expires. */
		if (timeout != UINT64_MAX && now - started >= timeout)
			return VK_TIMEOUT;
		nanosleep(&pause, NULL);
	}

	/* Software-complete acquire payloads avoid deadlocking behind unrelated queue work. */
	error = vulkan_wsi_acquire_signal(device, semaphore, fence);
	if (error != VK_SUCCESS) {
		pthread_mutex_lock(&swapchain_mutex);
		chain->states[index] = WSI_IMAGE_AVAILABLE;
		pthread_mutex_unlock(&swapchain_mutex);
		return error;
	}

	/* Only a fully successful acquire advances selection and returns the image index. */
	pthread_mutex_lock(&swapchain_mutex);
	chain->cursor = (index + 1U) % chain->group->count;
	pthread_mutex_unlock(&swapchain_mutex);
	*result = index;

	/* Succeeded: image contents are reusable and the requested sync objects are signaled. */
	return VK_SUCCESS;
}

/*
 * Enqueues all waits once and presents completed pixels without modifying images.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(
	VkQueue queue_handle,
	const VkPresentInfoKHR *info)
{
	struct VkQueue_T *queue;
	struct wsi_present_job job;
	VkResult error;

	/* A job retains every transient object until a checked completion or device loss. */
	queue = vulkan_queue(queue_handle);
	if (queue == NULL || info == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;
	memset(&job, 0, sizeof(job));
	error = present_prepare(&job, queue, info);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* Recording and allocations finish before any semaphore wait can be consumed. */
	error = present_record(&job, info);
	if (error != VK_SUCCESS)
		goto cleanup;
	error = present_submit(&job, info);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* Each native result contributes to both its pResults entry and overall priority. */
	error = present_native(&job, info);

cleanup:
	/* Restores unsubmitted acquisition or releases images after an enqueued operation. */
	present_finish(&job, info, error);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: every native target accepted its completed whole image. */
	return VK_SUCCESS;
}

/*
 * Native presentation calls complete synchronously, so no extra queue work remains.
 */
VkResult
vulkan_wsi_queue_idle(
	struct VkQueue_T *queue)
{
	VkResult error;

	/* Public queue idle has already completed ordinary GPU submissions. */
	error = __atomic_load_n(&queue->device->error, __ATOMIC_ACQUIRE);
	if (error != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;

	/* Succeeded: every returned native presentation has already consumed its pixels. */
	return VK_SUCCESS;
}

/*
 * Completes the synchronous native presentation side of public device idle.
 */
VkResult
vulkan_wsi_device_idle(
	struct VkDevice_T *device)
{
	VkResult error;

	/* Public device idle separately waits for every ordinary rendering queue. */
	error = __atomic_load_n(&device->error, __ATOMIC_ACQUIRE);
	if (error != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;

	/* Succeeded: native FIFO operations retain no asynchronous library pixel reads. */
	return VK_SUCCESS;
}

/*
 * Tears down WSI objects before core device children and the native GPU open.
 */
void
vulkan_wsi_device_finish(
	struct VkDevice_T *device)
{
	struct vulkan_swapchain *chain;

	/* Valid applications have already synchronized GPU use before device destruction. */
	for (;;) {
		pthread_mutex_lock(&swapchain_mutex);
		chain = swapchains;
		while (chain != NULL) {
			/* Selects only swapchains whose owning device is being destroyed. */
			if (chain->device == device)
				break;
			chain = chain->next;
		}

		pthread_mutex_unlock(&swapchain_mutex);
		if (chain == NULL)
			break;
		swapchain_free(chain);
	}

	/* Succeeded: no swapchain retains this device or its instance-owned surfaces. */
	return;
}

/* Converts the standard non-dispatchable handle without confusing object kinds. */
static struct vulkan_swapchain *
swapchain_get(
	VkSwapchainKHR handle)
{
	struct vulkan_object *object;

	/* Null handles and other Vulkan object kinds are not swapchain identities. */
	object = vulkan_nondispatchable_object((uint64_t)handle);
	if (object == NULL)
		return NULL;

	/* Rejects an unrelated Vulkan object masquerading as a swapchain handle. */
	if (object->kind != VULKAN_OBJECT_SWAPCHAIN)
		return NULL;

	/* Succeeded: the common object prefix identifies a local WSI swapchain. */
	return (struct vulkan_swapchain *)object;
}

/* Acquires complete private state without publishing a partially usable chain. */
static VkResult
swapchain_create(
	struct VkDevice_T *device,
	const VkSwapchainCreateInfoKHR *info,
	const VkAllocationCallbacks *allocator,
	struct wsi_image_group *shared,
	struct vulkan_swapchain **result)
{
	struct vulkan_swapchain *chain;
	struct vulkan_swapchain *old;
	struct vulkan_surface *surface;
	struct vulkan_object *object;
	VkResult error;
	size_t bytes;

	/* Retirement is required even when a later allocation or validation fails. */
	*result = NULL;
	surface = vulkan_wsi_surface(info->surface);
	old = swapchain_get(info->oldSwapchain);

	/* Retires a supplied old swapchain before fallible replacement creation begins. */
	if (old != NULL) {
		/* Rejects a replacement relationship spanning unrelated devices or surfaces. */
		if (old->device != device || old->surface != surface)
			return VK_ERROR_INITIALIZATION_FAILED;
		pthread_mutex_lock(&swapchain_mutex);
		old->retired = VK_TRUE;

		/* Removes the retired swapchain from the surface active slot. */
		if (old->surface->active == old)
			old->surface->active = NULL;
		pthread_mutex_unlock(&swapchain_mutex);
	}

	/* Surface capabilities and mode generation precede native resource acquisition. */
	error = swapchain_validate(device, info, &surface);
	if (error != VK_SUCCESS)
		return error;
	error = vulkan_object_alloc(
		sizeof(*chain),
		sizeof(void *),
		VULKAN_OBJECT_SWAPCHAIN,
		&device->object,
		device->object.context,
		allocator,
		VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
		&object);
	if (error != VK_SUCCESS)
		return error;
	chain = (struct vulkan_swapchain *)object;
	chain->device = device;
	chain->surface = surface;
	chain->format = info->imageFormat;
	chain->extent = info->imageExtent;

	/* Retains the instance surface before any native lease can borrow its allocator. */
	error = vulkan_wsi_surface_retain(surface);
	if (error != VK_SUCCESS) {
		chain->surface = NULL;
		goto cleanup;
	}

	error = surface->platform->claim(surface, device, &chain->lease);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* Every chain has independent acquisition state even when its images are shared. */
	bytes = (size_t)info->minImageCount;
	chain->states = vulkan_allocate(
		&chain->object.allocator,
		bytes,
		sizeof(void *),
		VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (chain->states == NULL) {
		error = VK_ERROR_OUT_OF_HOST_MEMORY;
		goto cleanup;
	}

	memset(chain->states, WSI_IMAGE_AVAILABLE, bytes);

	/* Creates an image group only for the first independently owned swapchain. */
	if (shared == NULL) {
		error = swapchain_group_create(chain, info);
		if (error != VK_SUCCESS)
			goto cleanup;
	} else {
		/* A shared creation holds the group through every subsequent fallible step. */
		if (shared->references == UINT32_MAX) {
			error = VK_ERROR_OUT_OF_HOST_MEMORY;
			goto cleanup;
		}

		shared->references++;
		chain->group = shared;
	}

	/* Readback storage is mapped once and reused for every presented frame. */
	error = swapchain_readback_create(chain);
	if (error != VK_SUCCESS)
		goto cleanup;
	/* Transfers the complete private chain to the publication stage. */
	*result = chain;
	chain = NULL;

cleanup:
	/* A failed creation still owns its partially acquired private chain. */
	if (chain != NULL)
		swapchain_free(chain);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: publication now needs no resource acquisition or wire transaction. */
	return VK_SUCCESS;
}

/* Checks the advertised surface contract and the active swapchain association. */
static VkResult
swapchain_validate(
	struct VkDevice_T *device,
	const VkSwapchainCreateInfoKHR *info,
	struct vulkan_surface **result)
{
	struct vulkan_surface *surface;
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[2];
	uint32_t count;
	uint32_t index;
	VkBool32 matched;
	VkResult error;

	/* This implementation exposes the Vulkan 1.0 creation structure and FIFO mode. */
	surface = vulkan_wsi_surface(info->surface);
	if (surface == NULL)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Requires the standard swapchain record before interpreting its presentation parameters. */
	if (info->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses unadvertised swapchain flags or layered native scanout. */
	if (info->flags != 0U || info->imageArrayLayers != 1U)
		return VK_ERROR_INITIALIZATION_FAILED;
	error = surface->platform->capabilities(surface, device->physical, &capabilities);
	if (error != VK_SUCCESS)
		return error;

	/* Requires enough independently acquired images for the native FIFO contract. */
	if (info->minImageCount < capabilities.minImageCount)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Honors a finite native image quota when the selected backend advertises one. */
	if (capabilities.maxImageCount != 0U && info->minImageCount > capabilities.maxImageCount)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires the image extent to match the surface mode selected by the application. */
	if (info->imageExtent.width != capabilities.currentExtent.width ||
	    info->imageExtent.height != capabilities.currentExtent.height)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses transforms, alpha modes or presentation modes absent from native capabilities. */
	if (info->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
	    info->compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ||
	    info->presentMode != VK_PRESENT_MODE_FIFO_KHR)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires every requested image operation to be supported by the presentation backend. */
	if (info->imageUsage == 0U || (info->imageUsage & ~capabilities.supportedUsageFlags) != 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Packed native formats are finite, but dynamic image and swapchain counts are not. */
	count = 2U;
	error = surface->platform->formats(surface, device->physical, &count, formats);
	if (error != VK_SUCCESS)
		return error;
	matched = VK_FALSE;
	for (index = 0U; index < count; index++) {
		if (formats[index].format == info->imageFormat &&
		    formats[index].colorSpace == info->imageColorSpace)
			matched = VK_TRUE;
	}

	/* Refuses an image format and color space pair missing from surface enumeration. */
	if (matched == VK_FALSE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* An active association must be retired explicitly through oldSwapchain. */
	pthread_mutex_lock(&swapchain_mutex);

	/* Preserves the single active swapchain slot until its previous occupant is retired. */
	if (surface->active != NULL) {
		pthread_mutex_unlock(&swapchain_mutex);
		return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
	}

	pthread_mutex_unlock(&swapchain_mutex);
	*result = surface;

	/* Succeeded: the requested image contract is supported by this native surface. */
	return VK_SUCCESS;
}

/* Atomically associates a fully acquired chain with its surface and device. */
static VkResult
swapchain_publish(
	struct vulkan_swapchain *chain)
{
	VkResult error;

	/* No application allocation callback or wire call runs under this state mutex. */
	pthread_mutex_lock(&swapchain_mutex);

	/* Rejects a competing active publication without taking ownership from another swapchain. */
	if (chain->surface->active != NULL) {
		pthread_mutex_unlock(&swapchain_mutex);
		return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
	}

	error = vulkan_object_publish(&chain->object);
	if (error != VK_SUCCESS) {
		pthread_mutex_unlock(&swapchain_mutex);
		return error;
	}

	chain->surface->active = chain;
	chain->next = swapchains;
	swapchains = chain;
	pthread_mutex_unlock(&swapchain_mutex);

	/* Succeeded: handle lookup and instance teardown can now observe the same chain. */
	return VK_SUCCESS;
}

/* Ends native ownership and releases only resources retained by this chain. */
static void
swapchain_free(
	struct vulkan_swapchain *chain)
{
	struct vulkan_swapchain **link;
	const VkAllocationCallbacks *allocator;
	VkResult error;
	VkBool32 final_group;

	/* Removes publication before any allocator callback can observe stale linkage. */
	pthread_mutex_lock(&swapchain_mutex);
	link = &swapchains;

	/* Unlinks only the swapchain whose public lifetime is being retired. */
	while (*link != NULL) {
		/* Removes the exact retiring handle from the live swapchain registry. */
		if (*link == chain) {
			*link = chain->next;
			break;
		}

		link = &(*link)->next;
	}

	/* Releases surface publication state only after surface ownership was acquired. */
	if (chain->surface != NULL) {
		/* Clears the active slot only when it still points to this retiring chain. */
		if (chain->surface->active == chain)
			chain->surface->active = NULL;
	}

	pthread_mutex_unlock(&swapchain_mutex);
	vulkan_object_unpublish(&chain->object);
	allocator = swapchain_allocator(&chain->object.allocator);

	/* Native copies have completed before a returned presentation can be destroyed. */
	if (chain->lease != NULL) {
		error = chain->surface->platform->release(chain->lease);

		/* Propagates terminal namespace loss to every target affected by the transaction. */
		if (error == VK_ERROR_DEVICE_LOST)
			swapchain_device_lost(chain->device);
		chain->lease = NULL;
	}

	/* Mapped readback is an ordinary bound Vulkan buffer with a distinct allocation. */
	if (chain->readback_pixels != NULL)
		vkUnmapMemory((VkDevice)chain->device, chain->readback_memory);

	/* Destroys staging storage only when readback buffer creation succeeded. */
	if (chain->readback != VK_NULL_HANDLE)
		vkDestroyBuffer((VkDevice)chain->device, chain->readback, allocator);

	/* Releases readback memory after its mapped view and bound buffer have retired. */
	if (chain->readback_memory != VK_NULL_HANDLE)
		vkFreeMemory((VkDevice)chain->device, chain->readback_memory, allocator);

	/* Shared images retire only when the final surviving swapchain releases them. */
	if (chain->group != NULL) {
		pthread_mutex_lock(&swapchain_mutex);
		chain->group->references--;
		final_group = VK_FALSE;

		/* Recognizes the final swapchain hold on a shared image group. */
		if (chain->group->references == 0U)
			final_group = VK_TRUE;
		pthread_mutex_unlock(&swapchain_mutex);

		/* Destroys shared images only after all swapchain handles have released their group. */
		if (final_group != VK_FALSE)
			swapchain_group_free(chain->group);
	}

	vulkan_free(&chain->object.allocator, chain->states);

	/* Releases the retained surface after all chain-owned presentation resources retire. */
	if (chain->surface != NULL)
		vulkan_wsi_surface_release(chain->surface);
	vulkan_object_free(&chain->object);

	/* Succeeded: no local WSI ownership remains for this chain. */
	return;
}

/* Allocates the exact requested dynamic image count without a fixed ring bound. */
static VkResult
swapchain_group_create(
	struct vulkan_swapchain *chain,
	const VkSwapchainCreateInfoKHR *info)
{
	struct wsi_image_group *group;
	size_t bytes;
	uint32_t index;
	VkResult error;

	/* Multiplication must remain representable on both ILP32 and LP64 hosts. */
	bytes = (size_t)info->minImageCount * sizeof(*group->images);
	if (bytes / sizeof(*group->images) != info->minImageCount)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	group = vulkan_allocate(
		&chain->device->object.allocator,
		sizeof(*group),
		sizeof(void *),
		VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (group == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(group, 0, sizeof(*group));
	group->allocator = chain->device->object.allocator;
	group->device = chain->device;
	group->references = 1U;
	chain->group = group;

	/* Partial image creation remains fully owned by the group for unwind. */
	group->images = vulkan_allocate(
		&group->allocator,
		bytes,
		sizeof(void *),
		VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (group->images == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(group->images, 0, bytes);
	group->count = info->minImageCount;
	for (index = 0U; index < group->count; index++) {
		error = swapchain_image_create(chain, info, &group->images[index]);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: every presentable image has ordinary bound device memory. */
	return VK_SUCCESS;
}

/* Frees a shared group after its last swapchain has ended acquisition ownership. */
static void
swapchain_group_free(
	struct wsi_image_group *group)
{
	const VkAllocationCallbacks *allocator;
	struct vulkan_image *image;
	uint32_t index;

	/* Application image destruction is forbidden, but the owning WSI may retire it. */
	allocator = swapchain_allocator(&group->allocator);
	for (index = 0U; index < group->count; index++) {
		/* Retires only images actually acquired before a partial group creation failure. */
		if (group->images[index].image != VK_NULL_HANDLE) {
			image = vulkan_image(group->images[index].image);

			/* Clears WSI metadata before the ordinary Vulkan image object is destroyed. */
			if (image != NULL)
				image->swapchain_owned = VK_FALSE;
			vkDestroyImage((VkDevice)group->device, group->images[index].image, allocator);
		}

		/* Releases each image allocation after the image bound to it has retired. */
		if (group->images[index].memory != VK_NULL_HANDLE)
			vkFreeMemory((VkDevice)group->device, group->images[index].memory, allocator);
	}

	/* The copied allocator remains available until the group's final allocation retires. */
	vulkan_free(&group->allocator, group->images);
	vulkan_free(&group->allocator, group);

	/* Succeeded: no Vulkan allocation survives the image group's final owner. */
	return;
}

/* Creates one standard image and binds memory using its real renderer requirements. */
static VkResult
swapchain_image_create(
	struct vulkan_swapchain *chain,
	const VkSwapchainCreateInfoKHR *info,
	struct wsi_image *image)
{
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	struct vulkan_image *private_image;
	const VkAllocationCallbacks *allocator;
	VkResult error;

	/* Internal transfer use is added without restricting the application's declared use. */
	allocator = swapchain_allocator(&chain->group->allocator);
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = info->imageFormat;
	create.extent.width = info->imageExtent.width;
	create.extent.height = info->imageExtent.height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = info->imageArrayLayers;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	create.sharingMode = info->imageSharingMode;
	create.queueFamilyIndexCount = info->queueFamilyIndexCount;
	create.pQueueFamilyIndices = info->pQueueFamilyIndices;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	error = vkCreateImage((VkDevice)chain->device, &create, allocator, &image->image);
	if (error != VK_SUCCESS)
		return error;

	/* Requirements, rather than packed pixel assumptions, determine device allocation. */
	memset(&requirements, 0, sizeof(requirements));
	vkGetImageMemoryRequirements((VkDevice)chain->device, image->image, &requirements);
	error = swapchain_allocate_memory(chain, &requirements, 0U, &chain->group->allocator, &image->memory);
	if (error != VK_SUCCESS)
		return error;
	error = vkBindImageMemory((VkDevice)chain->device, image->image, image->memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* The standard image handle remains valid but its lifetime belongs to WSI. */
	private_image = vulkan_image(image->image);
	private_image->swapchain_owned = VK_TRUE;

	/* Succeeded: rendering uses an ordinary image whose allocation is independently owned. */
	return VK_SUCCESS;
}

/* Creates one persistently mapped coherent readback allocation per swapchain. */
static VkResult
swapchain_readback_create(
	struct vulkan_swapchain *chain)
{
	VkBufferCreateInfo create;
	VkMemoryRequirements requirements;
	const VkAllocationCallbacks *allocator;
	VkResult error;
	uint32_t *families;
	uint32_t index;
	uint32_t family_count;
	size_t bytes;

	/* Tight four-byte rows preserve the surface format exactly through presentation. */
	allocator = swapchain_allocator(&chain->object.allocator);
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create.size = (VkDeviceSize)chain->extent.width * chain->extent.height * 4U;
	create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	/* A private staging buffer supports presentation on any device queue family. */
	families = NULL;
	family_count = chain->device->physical->queue_family_count;

	/* Allows readback staging to serve every available presenting queue family. */
	if (family_count > 1U) {
		bytes = (size_t)family_count * sizeof(*families);
		if (bytes / sizeof(*families) != family_count)
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		families = vulkan_allocate(
			&chain->object.allocator,
			bytes,
			sizeof(void *),
			VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
		if (families == NULL)
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		for (index = 0U; index < family_count; index++)
			families[index] = index;
		create.sharingMode = VK_SHARING_MODE_CONCURRENT;
		create.queueFamilyIndexCount = family_count;
		create.pQueueFamilyIndices = families;
	}

	error = vkCreateBuffer((VkDevice)chain->device, &create, allocator, &chain->readback);
	vulkan_free(&chain->object.allocator, families);
	if (error != VK_SUCCESS)
		return error;

	/* Genuine shared device mapping is supplied by the core memory implementation. */
	memset(&requirements, 0, sizeof(requirements));
	vkGetBufferMemoryRequirements((VkDevice)chain->device, chain->readback, &requirements);
	error = swapchain_allocate_memory(
		chain,
		&requirements,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&chain->object.allocator,
		&chain->readback_memory);
	if (error != VK_SUCCESS)
		return error;
	error = vkBindBufferMemory((VkDevice)chain->device, chain->readback, chain->readback_memory, 0U);
	if (error != VK_SUCCESS)
		return error;
	error = vkMapMemory(
		(VkDevice)chain->device,
		chain->readback_memory,
		0U,
		VK_WHOLE_SIZE,
		0U,
		&chain->readback_pixels);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: later frames reuse the same map without re-exporting device memory. */
	return VK_SUCCESS;
}

/* Chooses a legal memory type from the actual physical device and image requirements. */
static VkResult
swapchain_allocate_memory(
	struct vulkan_swapchain *chain,
	const VkMemoryRequirements *requirements,
	VkMemoryPropertyFlags required,
	const struct vulkan_allocator *policy,
	VkDeviceMemory *memory)
{
	VkMemoryAllocateInfo allocate;
	VkPhysicalDeviceMemoryProperties *properties;
	uint32_t index;
	VkResult error;

	/* There is no fixed memory-type index or assumption about host device locality. */
	properties = &chain->device->physical->memory;
	for (index = 0U; index < properties->memoryTypeCount; index++) {
		/* Skips a physical memory type which cannot back the actual readback or image resource. */
		if ((requirements->memoryTypeBits & (1U << index)) == 0U)
			continue;

		/* Selects a compatible allocation whose physical properties satisfy this resource role. */
		if ((properties->memoryTypes[index].propertyFlags & required) == required)
			break;
	}

	/* Reports unavailable backing when no physical memory type satisfies the resource. */
	if (index == properties->memoryTypeCount)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* Allocation size is the exact nonzero renderer requirement. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements->size;
	allocate.memoryTypeIndex = index;
	error = vkAllocateMemory(
		(VkDevice)chain->device,
		&allocate,
		swapchain_allocator(policy),
		memory);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the caller owns a compatible ordinary Vulkan allocation. */
	return VK_SUCCESS;
}

/* Returns the retained application's callbacks, or the standard default allocator. */
static const VkAllocationCallbacks *
swapchain_allocator(
	const struct vulkan_allocator *allocator)
{
	/* A NULL creation allocator must not silently inherit another object's callbacks. */
	if (allocator->has_callbacks == VK_FALSE)
		return NULL;

	/* Succeeded: these copied callbacks remain valid with the owning object. */
	return &allocator->callbacks;
}

/* Compares every property which determines the shared presentable image contract. */
static VkBool32
swapchain_compatible(
	const VkSwapchainCreateInfoKHR *first,
	const VkSwapchainCreateInfoKHR *second)
{
	uint32_t index;

	/* Native surfaces may differ, while image geometry, use and count must match. */
	if (first->minImageCount != second->minImageCount ||
	    first->imageFormat != second->imageFormat ||
	    first->imageColorSpace != second->imageColorSpace ||
	    first->imageExtent.width != second->imageExtent.width ||
	    first->imageExtent.height != second->imageExtent.height ||
	    first->imageArrayLayers != second->imageArrayLayers ||
	    first->imageUsage != second->imageUsage ||
	    first->imageSharingMode != second->imageSharingMode)
		return VK_FALSE;

	/* Requires shared swapchain images to name the same concurrent queue-family ownership. */
	if (first->imageSharingMode == VK_SHARING_MODE_CONCURRENT) {
		/* Rejects shared image creation with different queue-family array lengths. */
		if (first->queueFamilyIndexCount != second->queueFamilyIndexCount)
			return VK_FALSE;
		for (index = 0U; index < first->queueFamilyIndexCount; index++) {
			/* Rejects shared image creation with differing concurrent queue-family identities. */
			if (first->pQueueFamilyIndices[index] != second->pQueueFamilyIndices[index])
				return VK_FALSE;
		}
	}

	/* Succeeded: one image array can satisfy both declared swapchain contracts. */
	return VK_TRUE;
}

/* Checks current presentation validity without changing image acquisition state. */
static VkResult
swapchain_current(
	struct vulkan_swapchain *chain)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkResult error;

	/* A lost logical device prevents both native use and acquire signaling. */
	error = __atomic_load_n(&chain->device->error, __ATOMIC_ACQUIRE);
	if (error != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;
	/* Revalidates the selected native mode without altering acquisition ownership. */
	error = chain->surface->platform->capabilities(
		chain->surface,
		chain->device->physical,
		&capabilities);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the native surface still accepts this swapchain's complete images. */
	return VK_SUCCESS;
}

/* Measures acquire timeouts in the portable monotonic clock domain. */
static VkResult
swapchain_now(
	uint64_t *nanoseconds)
{
	struct timespec now;
	int status;

	/* Realtime clock adjustments cannot shorten or lengthen a Vulkan timeout. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return VK_ERROR_DEVICE_LOST;
	*nanoseconds = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;

	/* Succeeded: timeout comparisons use a monotonic elapsed duration. */
	return VK_SUCCESS;
}

/* Acquires a complete presentation transaction before consuming any wait semaphore. */
static VkResult
present_prepare(
	struct wsi_present_job *job,
	struct VkQueue_T *queue,
	const VkPresentInfoKHR *info)
{
	struct vulkan_swapchain *chain;
	const VkDisplayPresentInfoKHR *display;
	size_t bytes;
	uint32_t index;
	uint32_t image;
	uint32_t previous;
	VkResult error;

	/* Every later unwind can rely on initialized ownership fields. */
	job->queue = queue;
	job->allocator = queue->device->object.allocator;
	job->count = info->swapchainCount;

	/* Requires the standard present record before reading any target arrays. */
	if (info->sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses a present request that cannot transfer ownership of any image. */
	if (job->count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Dynamic arrays are bounded by representable allocation sizes, not fixed counts. */
	bytes = (size_t)job->count * sizeof(*job->chains);
	if (bytes / sizeof(*job->chains) != job->count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	job->chains = vulkan_allocate(
		&job->allocator,
		bytes,
		sizeof(void *),
		VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (job->chains == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(job->chains, 0, bytes);
	bytes = (size_t)job->count * sizeof(*job->results);
	if (bytes / sizeof(*job->results) != job->count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	job->results = vulkan_allocate(
		&job->allocator,
		bytes,
		sizeof(void *),
		VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (job->results == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(job->results, 0, bytes);

	/* Every semaphore wait uses the transfer stage of the one readback submission. */
	if (info->waitSemaphoreCount != 0U) {
		bytes = (size_t)info->waitSemaphoreCount * sizeof(*job->stages);
		if (bytes / sizeof(*job->stages) != info->waitSemaphoreCount)
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		job->stages = vulkan_allocate(
			&job->allocator,
			bytes,
			sizeof(void *),
			VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
		if (job->stages == NULL)
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		for (index = 0U; index < info->waitSemaphoreCount; index++)
			job->stages[index] = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}

	/* Optional display rectangles affect native composition, never image contents. */
	display = info->pNext;

	/* Finds the optional standard direct-display rectangle presentation record. */
	while (display != NULL) {
		/* Selects the direct-display rectangle extension from the application chain. */
		if (display->sType == VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR)
			break;
		display = display->pNext;
	}

	/* Allocates composition ownership only when the request includes display rectangles. */
	if (display != NULL) {
		job->display = *display;
		job->display.pNext = NULL;
		job->has_display = VK_TRUE;
		bytes = (size_t)job->count * sizeof(*job->composed);
		if (bytes / sizeof(*job->composed) != job->count)
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		job->composed = vulkan_allocate(
			&job->allocator,
			bytes,
			sizeof(void *),
			VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
		if (job->composed == NULL)
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		memset(job->composed, 0, bytes);
	}

	/* Ownership moves to PRESENTING only after each complete per-chain validation. */
	for (index = 0U; index < job->count; index++) {
		chain = swapchain_get(info->pSwapchains[index]);
		if (chain == NULL || chain->device != queue->device)
			return VK_ERROR_INITIALIZATION_FAILED;
		image = info->pImageIndices[index];
		if (image >= chain->group->count)
			return VK_ERROR_INITIALIZATION_FAILED;

		/* Surface rejection still consumes submitted waits as required by Vulkan. */
		error = swapchain_current(chain);
		if (error != VK_SUCCESS &&
		    error != VK_ERROR_SURFACE_LOST_KHR &&
		    error != VK_ERROR_OUT_OF_DATE_KHR)
			return error;
		job->results[index] = error;

		/* Invalid rectangles cannot escape their checked source or destination image. */
		if (job->has_display != VK_FALSE) {
			if (job->display.srcRect.offset.x < 0 ||
			    job->display.srcRect.offset.y < 0 ||
			    job->display.dstRect.offset.x < 0 ||
			    job->display.dstRect.offset.y < 0)
				return VK_ERROR_INITIALIZATION_FAILED;
			if ((uint32_t)job->display.srcRect.offset.x > chain->extent.width ||
			    (uint32_t)job->display.srcRect.offset.y > chain->extent.height ||
			    (uint32_t)job->display.dstRect.offset.x > chain->extent.width ||
			    (uint32_t)job->display.dstRect.offset.y > chain->extent.height)
				return VK_ERROR_INITIALIZATION_FAILED;
			if (job->display.srcRect.extent.width > chain->extent.width - (uint32_t)job->display.srcRect.offset.x ||
			    job->display.srcRect.extent.height > chain->extent.height - (uint32_t)job->display.srcRect.offset.y ||
			    job->display.dstRect.extent.width > chain->extent.width - (uint32_t)job->display.dstRect.offset.x ||
			    job->display.dstRect.extent.height > chain->extent.height - (uint32_t)job->display.dstRect.offset.y)
				return VK_ERROR_INITIALIZATION_FAILED;
			if (job->display.srcRect.extent.width == 0U ||
			    job->display.srcRect.extent.height == 0U ||
			    job->display.dstRect.extent.width == 0U ||
			    job->display.dstRect.extent.height == 0U ||
			    job->display.persistent != VK_FALSE)
				return VK_ERROR_INITIALIZATION_FAILED;
		}

		/* All CPU composition allocation finishes before the queue transaction begins. */
		if (job->has_display != VK_FALSE) {
			bytes = (size_t)chain->extent.width * chain->extent.height * 4U;
			job->composed[index] = vulkan_allocate(
				&job->allocator,
				bytes,
				sizeof(void *),
				VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
			if (job->composed[index] == NULL)
				return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		/* Repeated targets cannot share one readback slot within a single present batch. */
		for (previous = 0U; previous < index; previous++) {
			/* Rejects duplicate target ownership within one presentation transaction. */
			if (job->chains[previous] == chain)
				return VK_ERROR_INITIALIZATION_FAILED;
		}

		/* A retired chain may still present an image which was already acquired. */
		pthread_mutex_lock(&swapchain_mutex);

		/* Requires application acquisition before reserving the selected image for presentation. */
		if (chain->states[image] != WSI_IMAGE_ACQUIRED) {
			pthread_mutex_unlock(&swapchain_mutex);
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		job->chains[index] = chain;
		pthread_mutex_unlock(&swapchain_mutex);
	}

	/* Reserve all readback slots together so concurrent queue presents cannot overwrite them. */
	error = present_reserve(job, info);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: queue submission can no longer fail because a job array is missing. */
	return VK_SUCCESS;
}

/*
 * Reserves every target's readback storage without locking a queue or calling callbacks.
 * All-or-nothing reservation avoids ordering deadlocks between multi-display presents.
 */
static VkResult
present_reserve(
	struct wsi_present_job *job,
	const VkPresentInfoKHR *info)
{
	struct timespec pause;
	uint64_t started;
	uint64_t now;
	uint32_t index;
	VkBool32 available;
	VkResult error;

	/* A finite wait preserves QueuePresent's required finite host-call duration. */
	error = swapchain_now(&started);
	if (error != VK_SUCCESS)
		return error;
	pause.tv_sec = 0;
	pause.tv_nsec = 1000000L;
	for (;;) {
		pthread_mutex_lock(&swapchain_mutex);
		available = VK_TRUE;
		for (index = 0U; index < job->count; index++) {
			/* Waits for an earlier queue to finish using this chain private readback storage. */
			if (job->chains[index]->presenting != VK_FALSE) {
				available = VK_FALSE;
				break;
			}
		}

		/* No subset remains reserved while waiting for another target. */
		if (available != VK_FALSE) {
			for (index = 0U; index < job->count; index++) {
				job->chains[index]->presenting = VK_TRUE;
				job->chains[index]->states[info->pImageIndices[index]] = WSI_IMAGE_PRESENTING;
			}

			job->reserved = VK_TRUE;
			pthread_mutex_unlock(&swapchain_mutex);
			break;
		}

		pthread_mutex_unlock(&swapchain_mutex);

		/* A stalled earlier native transaction cannot block a later API call forever. */
		error = swapchain_now(&now);
		if (error != VK_SUCCESS)
			return error;

		/* Turns a stalled earlier native transaction into bounded terminal device failure. */
		if (now - started >= WSI_COMPLETION_TIMEOUT) {
			error = swapchain_device_lost(job->queue->device);
			return error;
		}

		nanosleep(&pause, NULL);
	}

	/* Succeeded: only this job can record or read each target's private staging buffer. */
	return VK_SUCCESS;
}

/* Records one readback transaction for every target without consuming its waits. */
static VkResult
present_record(
	struct wsi_present_job *job,
	const VkPresentInfoKHR *info)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	VkFenceCreateInfo fence;
	const VkAllocationCallbacks *allocator;
	uint32_t index;
	VkResult error;

	/* A transient pool belongs to the actual presenting queue family. */
	allocator = swapchain_allocator(&job->allocator);
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pool.queueFamilyIndex = job->queue->family;
	error = vkCreateCommandPool((VkDevice)job->queue->device, &pool, allocator, &job->pool);
	if (error != VK_SUCCESS)
		return error;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = job->pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1U;
	error = vkAllocateCommandBuffers((VkDevice)job->queue->device, &allocate, &job->command);
	if (error != VK_SUCCESS)
		return error;

	/* The whole command buffer is ready before the one submission consumes waits. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	error = vkBeginCommandBuffer(job->command, &begin);
	if (error != VK_SUCCESS)
		return error;
	for (index = 0U; index < job->count; index++) {
		/* Skips readback commands for targets whose surface was already lost or replaced. */
		if (job->results[index] != VK_SUCCESS)
			continue;
		present_copy(job->command, job->chains[index], info->pImageIndices[index]);
	}

	error = vkEndCommandBuffer(job->command);
	if (error != VK_SUCCESS)
		return error;

	/* An ordinary Vulkan fence proves readback availability before CPU presentation. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	error = vkCreateFence((VkDevice)job->queue->device, &fence, allocator, &job->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: all fallible command storage exists before any queue operation. */
	return VK_SUCCESS;
}

/* Copies one image read-only and restores its public present-source layout. */
static void
present_copy(
	VkCommandBuffer command,
	struct vulkan_swapchain *chain,
	uint32_t index)
{
	VkImageMemoryBarrier image;
	VkBufferMemoryBarrier buffer;
	VkBufferImageCopy copy;

	/* Prior available writes become visible to the presentation engine's transfer read. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	image.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	image.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	image.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image.image = chain->group->images[index].image;
	image.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	image.subresourceRange.levelCount = 1U;
	image.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(
		command,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0U,
		0U,
		NULL,
		0U,
		NULL,
		1U,
		&image);

	/* The staging buffer preserves every original pixel and the image's contents. */
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1U;
	copy.imageExtent.width = chain->extent.width;
	copy.imageExtent.height = chain->extent.height;
	copy.imageExtent.depth = 1U;
	vkCmdCopyImageToBuffer(
		command,
		image.image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		chain->readback,
		1U,
		&copy);

	/* Restores the present layout without changing queue-family ownership or pixels. */
	image.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	image.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	image.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	buffer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	buffer.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	buffer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer.buffer = chain->readback;
	buffer.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(
		command,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0U,
		0U,
		NULL,
		1U,
		&buffer,
		1U,
		&image);

	/* Succeeded: fence completion makes staging pixels visible to the CPU. */
	return;
}

/* Consumes every wait exactly once through the core's common synchronization path. */
static VkResult
present_submit(
	struct wsi_present_job *job,
	const VkPresentInfoKHR *info)
{
	VkSubmitInfo submit;
	VkResult error;

	/* Mixed acquired software payloads and genuine GPU waits are handled by core. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = info->waitSemaphoreCount;
	submit.pWaitSemaphores = info->pWaitSemaphores;
	submit.pWaitDstStageMask = job->stages;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &job->command;
	error = vulkan_queue_submit(job->queue, 1U, &submit, job->fence);
	if (error != VK_SUCCESS)
		return error;
	job->submitted = VK_TRUE;

	/* A finite implementation deadline cannot become an infinite QueuePresent call. */
	error = vulkan_fences_wait(
		job->queue->device,
		1U,
		&job->fence,
		VK_TRUE,
		WSI_COMPLETION_TIMEOUT);
	if (error != VK_SUCCESS) {
		error = swapchain_device_lost(job->queue->device);
		return error;
	}

	/* Succeeded: all native presentation reads observe completed Vulkan image data. */
	return VK_SUCCESS;
}

/* Applies optional display rectangle scaling to a separate native pixel copy. */
static void
present_compose(
	struct wsi_present_job *job,
	uint32_t index,
	struct vulkan_wsi_pixels *pixels)
{
	struct vulkan_swapchain *chain;
	const VkRect2D *source_rect;
	const VkRect2D *target_rect;
	const uint8_t *source;
	uint8_t *destination;
	uint32_t x;
	uint32_t y;
	uint32_t source_x;
	uint32_t source_y;
	uint32_t target_x;
	uint32_t target_y;
	size_t offset;

	/* Composition affects display pixels only; acquired Vulkan image contents survive. */
	chain = job->chains[index];
	source_rect = &job->display.srcRect;
	target_rect = &job->display.dstRect;
	source = chain->readback_pixels;
	destination = job->composed[index];

	/* With one opaque plane, all pixels outside the destination rectangle are black. */
	for (y = 0U; y < chain->extent.height; y++) {
		for (x = 0U; x < chain->extent.width; x++) {
			offset = ((size_t)y * chain->extent.width + x) * 4U;
			destination[offset] = 0U;
			destination[offset + 1U] = 0U;
			destination[offset + 2U] = 0U;
			destination[offset + 3U] = 255U;
		}
	}

	/* Nearest sampling gives a deterministic supported scaling implementation. */
	for (y = 0U; y < target_rect->extent.height; y++) {
		target_y = (uint32_t)target_rect->offset.y + y;
		source_y = (uint32_t)source_rect->offset.y +
		    (uint32_t)((uint64_t)y * source_rect->extent.height / target_rect->extent.height);
		for (x = 0U; x < target_rect->extent.width; x++) {
			target_x = (uint32_t)target_rect->offset.x + x;
			source_x = (uint32_t)source_rect->offset.x +
			    (uint32_t)((uint64_t)x * source_rect->extent.width / target_rect->extent.width);
			memcpy(
				destination + ((size_t)target_y * chain->extent.width + target_x) * 4U,
				source + ((size_t)source_y * chain->extent.width + source_x) * 4U,
				4U);
		}
	}

	pixels->pixels = destination;

	/* Succeeded: the native frame contains the requested crop, scale and black surround. */
	return;
}

/* Presents every target and records native rejection after consuming all waits. */
static VkResult
present_native(
	struct wsi_present_job *job,
	const VkPresentInfoKHR *info)
{
	struct vulkan_swapchain *chain;
	struct vulkan_wsi_pixels pixels;
	VkResult error;
	VkResult combined;
	uint32_t index;

	/* The API results array is populated during common transaction cleanup. */
	(void)info;
	combined = VK_SUCCESS;
	for (index = 0U; index < job->count; index++) {
		chain = job->chains[index];
		error = job->results[index];
		if (error == VK_SUCCESS) {
			/* A frame identifier belongs to its chain and never substitutes for a fence. */
			if (chain->frame == UINT64_MAX) {
				error = swapchain_device_lost(chain->device);
			} else {
				chain->frame++;
				memset(&pixels, 0, sizeof(pixels));
				pixels.pixels = chain->readback_pixels;
				pixels.extent = chain->extent;
				pixels.stride = chain->extent.width * 4U;
				pixels.format = chain->format;
				pixels.frame = chain->frame;

				/* Uses composed rectangle pixels only when the standard display extension requested them. */
				if (job->has_display != VK_FALSE)
					present_compose(job, index, &pixels);
				error = chain->surface->platform->present(chain->lease,
				    &pixels, &chain->sequence);
			}
		}

		/* After waits were consumed, allocation failure cannot promise unchanged sync state. */
		if (error == VK_ERROR_OUT_OF_HOST_MEMORY ||
		    error == VK_ERROR_OUT_OF_DEVICE_MEMORY)
			error = swapchain_device_lost(chain->device);
		job->results[index] = error;
		combined = present_combine(combined, error);
	}

	/* Succeeded targets remain successful even when another target rejects presentation. */
	return combined;
}

/* Frees a complete or partial job and applies the enqueue ownership boundary. */
static void
present_finish(
	struct wsi_present_job *job,
	const VkPresentInfoKHR *info,
	VkResult error)
{
	struct vulkan_swapchain *chain;
	const VkAllocationCallbacks *allocator;
	uint32_t index;
	uint32_t image;

	/* No helper may touch the queue when validation failed before it was assigned. */
	if (job->queue == NULL)
		return;
	allocator = swapchain_allocator(&job->allocator);

	/* A completed fence or device loss ends the job's command-buffer lifetime. */
	if (job->fence != VK_NULL_HANDLE)
		vkDestroyFence((VkDevice)job->queue->device, job->fence, allocator);

	/* Destroys transient command storage after presentation completion or namespace loss. */
	if (job->pool != VK_NULL_HANDLE)
		vkDestroyCommandPool((VkDevice)job->queue->device, job->pool, allocator);

	/* A failed enqueue leaves acquisition unchanged; an enqueued present releases it. */
	pthread_mutex_lock(&swapchain_mutex);
	for (index = 0U; index < job->count; index++) {
		chain = NULL;
		if (job->chains != NULL)
			chain = job->chains[index];

		/* Returns image ownership only for targets reserved by this transaction. */
		if (chain != NULL && job->reserved != VK_FALSE) {
			chain->presenting = VK_FALSE;
			image = info->pImageIndices[index];
			chain->states[image] = WSI_IMAGE_ACQUIRED;

			/* Releases a submitted image to acquisition after its presentation transaction finishes. */
			if (job->submitted != VK_FALSE)
				chain->states[image] = WSI_IMAGE_AVAILABLE;
		}

		/* Publishes per-target completion only when the application supplied result storage. */
		if (info->pResults != NULL) {
			info->pResults[index] = error;

			/* Preserves individual native presentation results after a successful wait-consuming submission. */
			if (job->submitted != VK_FALSE && job->results != NULL) {
				info->pResults[index] = job->results[index];

				/* Propagates terminal namespace loss to every target affected by the transaction. */
				if (error == VK_ERROR_DEVICE_LOST)
					info->pResults[index] = error;
			}
		}
	}

	pthread_mutex_unlock(&swapchain_mutex);

	/* All application allocation callbacks run after the state mutex is released. */
	if (job->composed != NULL) {
		for (index = 0U; index < job->count; index++)
			vulkan_free(&job->allocator, job->composed[index]);
	}

	vulkan_free(&job->allocator, job->composed);
	vulkan_free(&job->allocator, job->stages);
	vulkan_free(&job->allocator, job->results);
	vulkan_free(&job->allocator, job->chains);

	/* Succeeded: no transient job object retains a swapchain or caller output array. */
	return;
}

/* Applies the standard multi-swapchain result priority to independently completed targets. */
static VkResult
present_combine(
	VkResult first,
	VkResult second)
{
	/* Device loss dominates surface loss, which dominates obsolete swapchains. */
	if (first == VK_ERROR_DEVICE_LOST || second == VK_ERROR_DEVICE_LOST)
		return VK_ERROR_DEVICE_LOST;

	/* Prioritizes a destroyed native surface over a recoverable mode replacement. */
	if (first == VK_ERROR_SURFACE_LOST_KHR || second == VK_ERROR_SURFACE_LOST_KHR)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Reports that at least one target requires swapchain replacement. */
	if (first == VK_ERROR_OUT_OF_DATE_KHR || second == VK_ERROR_OUT_OF_DATE_KHR)
		return VK_ERROR_OUT_OF_DATE_KHR;

	/* Preserves successful presentation with a nonideal but still usable target. */
	if (first == VK_SUBOPTIMAL_KHR || second == VK_SUBOPTIMAL_KHR)
		return VK_SUBOPTIMAL_KHR;

	/* Preserves the first remaining per-target failure when neither has a stronger domain. */
	if (first != VK_SUCCESS)
		return first;

	/* Succeeded so far, or the second result supplies the remaining failure. */
	return second;
}

/* Records failure when the library can no longer preserve ordinary resource state. */
static VkResult
swapchain_device_lost(
	struct VkDevice_T *device)
{
	/* The core's device error makes later queue and acquire operations fail consistently. */
	__atomic_store_n(&device->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);

	/* Makes subsequent renderer commands observe the same terminal device loss. */
	if (device->object.context != NULL)
		__atomic_store_n(&device->object.context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);

	/* A consumed transaction cannot be reported as an unchanged allocation failure. */
	return VK_ERROR_DEVICE_LOST;
}
