/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Dynamic Vulkan swapchains over ordinary images and a separate native adapter.
 * Presentation enqueues owned jobs which verify GPU completion before native scanout or explicit copied fallback.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "wsi-internal.h"

#include <errno.h>
#include <poll.h>
#include <uapi/gpu-fence.h>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WSI_IMAGE_AVAILABLE	0U
#define WSI_IMAGE_ACQUIRED	1U
#define WSI_IMAGE_PRESENTING	2U
#define WSI_COMPLETION_TIMEOUT	10000000000ULL
#define WSI_PROGRESS_INTERVAL	10000000ULL

/* Shared presentation allocations transfer ownership to another Vulkan instance. */
#define WSI_QUEUE_FAMILY_EXTERNAL 0xfffffffeU

/* One ordinary bound image whose allocation lives with a shared image group. */
struct wsi_image {
	VkImage image;
	VkDeviceMemory memory;
};

/* One swapchain owns its scanout destination independently from shared application VkImages. */
struct wsi_present_image {
	VkImage shared_image;
	VkDeviceMemory shared_memory;
	void *native_image;
	VkBool32 shared_presented;
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
	struct wsi_present_image *presentation;
	uint32_t presentation_count;
	VkBool32 gpu_present;
	void *lease;
	uint8_t *states;
	VkFormat format;
	VkExtent2D extent;
	VkPresentModeKHR present_mode;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	void *readback_pixels;
	uint64_t frame;
	uint64_t sequence;
	uint32_t cursor;
	VkBool32 retired;
	uint32_t pending_jobs;
	VkResult error;
};

/* A job borrows its queue worker until the ordered completion path retires it. */
struct wsi_queue_worker;

/* One concurrent job borrows cached command storage and a fence until GPU and native use retire. */
struct wsi_present_fence {
	struct wsi_present_fence *next;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	int fd;
	VkBool32 shared;
	VkBool32 busy;
};

/* One owned presentation transaction outlives its caller until checked GPU and native completion. */
struct wsi_present_job {
	struct vulkan_allocator allocator;
	struct wsi_present_job *next;
	struct wsi_queue_worker *worker;
	struct wsi_present_fence *completion;
	VkPresentInfoKHR info;
	uint32_t *indices;
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
	int fence_fd;
	uint64_t fence_generation;
	uint32_t count;
	VkBool32 submitted;
	VkBool32 reserved;
};

/* One logical queue owns an ordered worker whose waits never retain queue or WSI locks. */
struct wsi_queue_worker {
	struct wsi_queue_worker *next;
	struct VkQueue_T *queue;
	struct wsi_present_job *head;
	struct wsi_present_job *tail;
	struct wsi_present_job *current;
	struct wsi_present_fence *fences;
	pthread_t thread;
	VkBool32 stopping;
};

/* Serializes only WSI linkage and image ownership, never a pending GPU fence. */
static pthread_mutex_t swapchain_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Live chains retain their surface and image ownership until all queued jobs have retired. */
static struct vulkan_swapchain *swapchains;

/* Workers remain linked until their logical device drains and joins them. */
static struct wsi_queue_worker *workers;

/* Ownership transitions wake workers, idle waiters and swapchain destruction under swapchain_mutex. */
static pthread_cond_t swapchain_condition = PTHREAD_COND_INITIALIZER;

static struct vulkan_swapchain *swapchain_get(VkSwapchainKHR handle);
static VkResult swapchain_create(struct VkDevice_T *device, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *allocator, struct wsi_image_group *shared, struct vulkan_swapchain **result);
static VkResult swapchain_validate(struct VkDevice_T *device, const VkSwapchainCreateInfoKHR *info, struct vulkan_surface **surface);
static VkResult swapchain_publish(struct vulkan_swapchain *chain);
static void swapchain_free(struct vulkan_swapchain *chain);
static VkResult swapchain_group_create(struct vulkan_swapchain *chain, const VkSwapchainCreateInfoKHR *info);
static void swapchain_group_free(struct wsi_image_group *group);
static VkResult swapchain_image_create(struct vulkan_swapchain *chain, const VkSwapchainCreateInfoKHR *info, struct wsi_image *image);
static VkResult swapchain_readback_create(struct vulkan_swapchain *chain);
static VkResult swapchain_native_images_create(struct vulkan_swapchain *chain);
static void swapchain_native_images_free(struct vulkan_swapchain *chain);
static VkResult swapchain_allocate_memory(struct vulkan_swapchain *chain, const VkMemoryRequirements *requirements, VkMemoryPropertyFlags required, const struct vulkan_allocator *policy, VkDeviceMemory *memory);
static const VkAllocationCallbacks *swapchain_allocator(const struct vulkan_allocator *allocator);
static VkBool32 swapchain_compatible(const VkSwapchainCreateInfoKHR *first, const VkSwapchainCreateInfoKHR *second);
static VkResult swapchain_current(struct vulkan_swapchain *chain);
static VkResult swapchain_now(uint64_t *nanoseconds);
static VkResult swapchain_acquire(struct vulkan_swapchain *chain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *result, struct vulkan_wake *wake);
static VkResult swapchain_poll(struct vulkan_swapchain *chain, struct vulkan_wake *wake, uint64_t timeout);
static void swapchain_notify_locked(void);
static VkResult present_prepare(struct wsi_present_job *job, struct VkQueue_T *queue, const VkPresentInfoKHR *info);
static VkResult present_reserve(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static VkResult present_fence_prepare(struct wsi_present_job *job);
static VkResult present_record(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static void present_copy(VkCommandBuffer command, struct vulkan_swapchain *chain, uint32_t index, uint32_t family, const VkDisplayPresentInfoKHR *display);
static void present_copy_shared(VkCommandBuffer command, struct vulkan_swapchain *chain, uint32_t index, uint32_t family, const VkDisplayPresentInfoKHR *display);
static VkResult present_submit(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static void present_compose(struct wsi_present_job *job, uint32_t index, struct vulkan_wsi_pixels *pixels);
static VkResult present_native(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static VkBool32 present_early(const struct wsi_present_job *job);
static void present_finish(struct wsi_present_job *job, const VkPresentInfoKHR *info, VkResult error);
static VkResult present_combine(VkResult first, VkResult second);
static VkResult swapchain_device_lost(struct VkDevice_T *device);
static VkResult present_worker_get(struct VkQueue_T *queue, struct wsi_queue_worker **result);
static void *present_worker_main(void *argument);
static void present_workers_stop(struct VkDevice_T *device);
static VkResult present_drain(struct VkDevice_T *device, struct VkQueue_T *queue);

/*
 * Wakes acquisition after a native adapter releases image ownership.
 */
void
vulkan_wsi_image_notify(
	void)
{
	/* Serializes notification with the last image-state observation before sleep. */
	pthread_mutex_lock(&swapchain_mutex);

	swapchain_notify_locked();

	pthread_mutex_unlock(&swapchain_mutex);

	/* Succeeded: every waiting acquirer will observe the adapter's published state. */
	return;
}

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
 * Acquires a reusable image while retaining a private direct-display wake source.
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
	struct vulkan_wake wake;
	struct vulkan_wake *registered;
	VkResult error;

	/* Valid Vulkan ownership keeps the swapchain and its native lease alive throughout acquisition. */
	device = vulkan_device(device_handle);
	chain = swapchain_get(handle);
	if (chain == NULL || result == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* An independent logical device cannot acquire another owner's image. */
	if (chain->device != device)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Ownership transfer must signal at least one application completion primitive. */
	if (semaphore == VK_NULL_HANDLE && fence == VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Immediate acquisitions create no fd; blocked direct calls register this storage lazily. */
	memset(&wake, 0, sizeof(wake));
	wake.read_fd = -1;
	wake.write_fd = -1;
	registered = NULL;
	if (timeout != 0) {
		/* Wayland keeps its existing finite event-dispatch progression. */
		if (chain->surface->platform->wait_descriptor != NULL)
			registered = &wake;
	}

	/* One cleanup boundary covers success, timeout, topology change and renderer failure. */
	error = swapchain_acquire(chain, timeout, semaphore, fence, result, registered);

	/* Publication ownership ends before the fd number becomes available to another object. */
	if (registered != NULL)
		vulkan_wake_destroy(registered);

	/* Failed acquisition leaves no descriptor capable of waking a later fd owner. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: only the image and standard completion primitive were transferred to the caller. */
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
	struct wsi_present_job *job;
	struct wsi_queue_worker *worker;
	struct vulkan_allocator allocator;
	VkResult error;
	VkResult combined;
	size_t bytes;
	uint32_t index;

	/* The library owns every object which may remain live after this API call returns. */
	queue = vulkan_queue(queue_handle);
	if (queue == NULL || info == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Job allocation precedes acquisition reservation and any semaphore consumption. */
	allocator = queue->device->object.allocator;
	job = vulkan_allocate(&allocator, sizeof(*job), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (job == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Partial preparation remains safe for the common unsubmitted cleanup path. */
	memset(job, 0, sizeof(*job));
	job->fence_fd = -1;
	error = present_prepare(job, queue, info);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* Caller arrays and pResults cease to be valid inputs as soon as this call returns. */
	bytes = (size_t)job->count * sizeof(*job->indices);
	if (bytes / sizeof(*job->indices) != job->count) {
		error = VK_ERROR_OUT_OF_HOST_MEMORY;
		goto cleanup;
	}

	/* The worker reads only this owned image-index snapshot and already resolved chain pointers. */
	job->indices = vulkan_allocate(&allocator, bytes, sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (job->indices == NULL) {
		error = VK_ERROR_OUT_OF_HOST_MEMORY;
		goto cleanup;
	}

	/* Native completion never reads an application pNext chain or writes an earlier pResults array. */
	memcpy(job->indices, info->pImageIndices, bytes);
	job->info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	job->info.swapchainCount = job->count;
	job->info.pImageIndices = job->indices;
	error = present_worker_get(queue, &worker);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* Recording and native metadata allocation complete before waits can be consumed. */
	job->worker = worker;
	error = present_record(job, info);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* Queue submission and worker publication retain one issue order with other queue operations. */
	pthread_mutex_lock(&queue->mutex);

	error = present_submit(job, info);
	if (error != VK_SUCCESS) {
		pthread_mutex_unlock(&queue->mutex);
		goto cleanup;
	}

	/* Publish only results known at enqueue time while caller-owned result storage is still valid. */
	combined = VK_SUCCESS;
	for (index = 0U; index < job->count; index++) {
		/* Native asynchronous failures are latched later on the chain, never written into this array. */
		if (info->pResults != NULL)
			info->pResults[index] = job->results[index];

		/* Each independently validated target contributes its enqueue status. */
		combined = present_combine(combined, job->results[index]);
	}

	/* From this publication onward only the worker may release the owned job. */
	pthread_mutex_lock(&swapchain_mutex);

	if (worker->tail != NULL)
		worker->tail->next = job;
	else
		worker->head = job;
	worker->tail = job;
	swapchain_notify_locked();

	pthread_mutex_unlock(&swapchain_mutex);

	pthread_mutex_unlock(&queue->mutex);

	/* Publication transfers the entire job to its worker, including all cleanup responsibility. */
	job = NULL;
	error = combined;

cleanup:
	/* Native acceptance followed by notification failure is terminal, never an ordinary retry. */
	if (error == VK_ERROR_DEVICE_LOST)
		error = swapchain_device_lost(queue->device);

	/* Only unpublished jobs remain caller-owned; preparation failure restores acquisition. */
	if (job != NULL) {
		present_finish(job, info, error);
		vulkan_free(&allocator, job);
	}

	/* Rejected targets or failed enqueue retain their precise public error result. */
	if (error < VK_SUCCESS)
		return error;

	/* Succeeded: the library owns the accepted GPU work and ordered native presentation job. */
	return error;
}

/*
 * Drains queue-owned presentation jobs without waiting for the last scanout image to be replaced.
 */
VkResult
vulkan_wsi_queue_idle(
	struct VkQueue_T *queue)
{
	VkResult error;

	/* Native request completion is distinct from current front allocation retention. */
	error = present_drain(queue->device, queue);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: no worker will issue another native request for earlier work on this queue. */
	return VK_SUCCESS;
}

/*
 * Drains every presentation producer before public device idle places its GPU completion markers.
 */
VkResult
vulkan_wsi_device_idle(
	struct VkDevice_T *device)
{
	VkResult error;

	/* A surviving current scanout is idle even while its allocation remains unavailable for reuse. */
	error = present_drain(device, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: every earlier device presentation job has retired its native request work. */
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

	/* Workers must stop borrowing queue, chain and allocator state before core device children retire. */
	present_workers_stop(device);

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
		swapchain_notify_locked();

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
	chain->present_mode = info->presentMode;

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

	/* A complete shared allocation/import trial selects the GPU route exactly once for this chain. */
	error = VK_ERROR_FORMAT_NOT_SUPPORTED;
	if (surface->platform->import_image != NULL)
		error = swapchain_native_images_create(chain);
	if (error == VK_SUCCESS) {
		chain->gpu_present = VK_TRUE;
	} else {
		/* Only unsupported layout or physical sharing can select copied fallback; failures remain failures. */
		if ((error != VK_ERROR_FORMAT_NOT_SUPPORTED &&
		    error != VK_ERROR_FEATURE_NOT_PRESENT) ||
		    surface->platform->present == NULL)
			goto cleanup;

		/* Every partial shared image and native alias retires before committing to the copied route. */
		swapchain_native_images_free(chain);
		if (surface->platform->prepare_copy != NULL) {
			error = surface->platform->prepare_copy(chain->lease, chain->format, chain->extent);
			if (error != VK_SUCCESS)
				goto cleanup;
		}

		/* Copied storage belongs exclusively to this fixed fallback route for the chain's lifetime. */
		error = swapchain_readback_create(chain);
		if (error != VK_SUCCESS)
			goto cleanup;
	}

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
	VkPresentModeKHR modes[4];
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

	/* Native export may impose a minimum extent beyond ordinary image creation. */
	if (info->imageExtent.width < capabilities.minImageExtent.width ||
	    info->imageExtent.height < capabilities.minImageExtent.height)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Renderer and native scanout limits both constrain the maximum image extent. */
	if (info->imageExtent.width > capabilities.maxImageExtent.width ||
	    info->imageExtent.height > capabilities.maxImageExtent.height)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Direct-display surfaces require their selected mode; window systems choose an extent. */
	if (capabilities.currentExtent.width != UINT32_MAX) {
		/* Fixed native dimensions must agree with both coordinates supplied by the application. */
		if (info->imageExtent.width != capabilities.currentExtent.width ||
		    info->imageExtent.height != capabilities.currentExtent.height)
			return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Refuses transforms, alpha modes or presentation modes absent from native capabilities. */
	if (info->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
	    info->compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Selects only modes actually implemented by the native presentation backend. */
	count = 4U;
	error = surface->platform->present_modes(surface, device->physical, &count, modes);
	if (error != VK_SUCCESS)
		return error;

	/* Finds the requested mode in the native backend's actual enumeration. */
	matched = VK_FALSE;
	for (index = 0U; index < count; index++) {
		/* A matching native mode authorizes this swapchain's pacing behavior. */
		if (modes[index] == info->presentMode)
			matched = VK_TRUE;
	}

	/* Unadvertised modes cannot be substituted with a different pacing contract. */
	if (matched == VK_FALSE)
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

	/* Pending jobs retain the lease, images and allocator until checked native completion. */
	pthread_mutex_lock(&swapchain_mutex);

	while (chain->pending_jobs != 0U)
		pthread_cond_wait(&swapchain_condition, &swapchain_mutex);

	/* Remove publication only after no worker can borrow this chain. */
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

	/* Mapped readback is an ordinary bound Vulkan buffer with a distinct allocation. */
	if (chain->readback_pixels != NULL)
		vkUnmapMemory((VkDevice)chain->device, chain->readback_memory);

	/* Destroys staging storage only when readback buffer creation succeeded. */
	if (chain->readback != VK_NULL_HANDLE)
		vkDestroyBuffer((VkDevice)chain->device, chain->readback, allocator);

	/* Releases readback memory after its mapped view and bound buffer have retired. */
	if (chain->readback_memory != VK_NULL_HANDLE)
		vkFreeMemory((VkDevice)chain->device, chain->readback_memory, allocator);

	/* Independent native destinations retire before their lease or shared application image group. */
	swapchain_native_images_free(chain);

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

	/* Native image identities retire before the lease which owns their imported namespace. */
	if (chain->lease != NULL) {
		error = chain->surface->platform->release(chain->lease);

		/* Propagates terminal namespace loss to every target affected by the transaction. */
		if (error == VK_ERROR_DEVICE_LOST)
			swapchain_device_lost(chain->device);
		chain->lease = NULL;
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
	uint32_t index;
	size_t bytes;
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
	uint32_t index;
	struct vulkan_image *image;

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

/* Creates independent GPU scanout images even when application VkImages are shared across chains. */
static VkResult
swapchain_native_images_create(
	struct vulkan_swapchain *chain)
{
	struct gpu_image_descriptor descriptor;
	struct gpu_placement placement;
	const struct gpu_placement *conditions;
	VkFormatProperties properties;
	const VkAllocationCallbacks *allocator;
	size_t bytes;
	uint32_t index;
	VkResult error;
	int fd;

	/* A renderer without allocation sharing can still serve an explicitly advertised copied display. */
	if ((chain->device->object.context->capabilities & GPU_CAP_SHARE) == 0U)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Direct-display rectangle presentation requires real GPU blits throughout the fixed shared route. */
	if (chain->surface->display_mode != NULL) {
		/* The fixed GPU route must support both optimal-source and linear-destination blits. */
		vkGetPhysicalDeviceFormatProperties((VkPhysicalDevice)chain->device->physical, chain->format, &properties);
		if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0U ||
		    (properties.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0U)
			return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* Direct displays supply physical requirements once; a window-system adapter may omit them. */
	conditions = NULL;
	if (chain->surface->platform->placement != NULL) {
		error = chain->surface->platform->placement(chain->lease, &placement);
		if (error != VK_SUCCESS)
			return error;
		conditions = &placement;
	}

	/* The exact native image count is bounded by the already validated application image group. */
	bytes = (size_t)chain->group->count * sizeof(*chain->presentation);
	if (bytes / sizeof(*chain->presentation) != chain->group->count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Partial construction remains reachable through ordinary swapchain cleanup. */
	chain->presentation = vulkan_allocate(&chain->object.allocator, bytes, sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (chain->presentation == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Each native destination belongs to this chain's lease independently from its public image group. */
	memset(chain->presentation, 0, bytes);
	chain->presentation_count = chain->group->count;

	/* Construct one independent native destination for every acquired application image. */
	allocator = swapchain_allocator(&chain->object.allocator);
	for (index = 0U; index < chain->presentation_count; index++) {
		/* GPU image construction publishes real row pitch, offset and an independent allocation capability. */
		error = vulkan_wsi_shared_image_create(
			chain->device,
			chain->format,
			chain->extent,
			allocator,
			conditions,
			&chain->presentation[index].shared_image,
			&chain->presentation[index].shared_memory,
			&fd,
			&descriptor);
		if (error != VK_SUCCESS)
			return error;

		/* The native backend keeps its alias after this producer-owned fd is closed. */
		error = chain->surface->platform->import_image(chain->lease, fd, &descriptor, &chain->presentation[index].native_image);
		close(fd);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: every acquired application image has its own native GPU-copy destination. */
	return VK_SUCCESS;
}

/* Retires every native destination, including rollback before selecting a copied route. */
static void
swapchain_native_images_free(
	struct vulkan_swapchain *chain)
{
	const VkAllocationCallbacks *allocator;
	uint32_t index;

	/* Partial construction leaves zero handles in every slot which did not acquire ownership. */
	allocator = swapchain_allocator(&chain->object.allocator);

	/* Each chain retires native buffers before releasing its lease or shared application image group. */
	for (index = 0U; index < chain->presentation_count; index++) {
		/* Native identities stop referencing the local image before its renderer object is destroyed. */
		if (chain->presentation[index].native_image != NULL && chain->surface->platform->destroy_image != NULL)
			chain->surface->platform->destroy_image(chain->presentation[index].native_image);

		/* Partial construction may own an image without a bound allocation. */
		if (chain->presentation[index].shared_image != VK_NULL_HANDLE)
			vkDestroyImage((VkDevice)chain->device, chain->presentation[index].shared_image, allocator);

		/* Allocation release follows both native alias retirement and renderer image destruction. */
		if (chain->presentation[index].shared_memory != VK_NULL_HANDLE)
			vkFreeMemory((VkDevice)chain->device, chain->presentation[index].shared_memory, allocator);
	}

	/* No native image pointer survives into lease retirement. */
	vulkan_free(&chain->object.allocator, chain->presentation);
	chain->presentation = NULL;
	chain->presentation_count = 0U;

	/* Succeeded: no chain-owned native allocation or alias remains. */
	return;
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

	/* The transient primary command buffer belongs to this already-created private pool. */
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

	/* Asynchronous native loss remains visible even when a later image would otherwise be available. */
	error = __atomic_load_n(&chain->error, __ATOMIC_ACQUIRE);
	if (error != VK_SUCCESS)
		return error;

	/* A lost logical device prevents both native use and acquire signaling. */
	error = __atomic_load_n(&chain->device->error, __ATOMIC_ACQUIRE);
	if (error != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;

	/* A failed renderer context remains terminal even before another entrypoint updates its device. */
	error = __atomic_load_n(&chain->device->object.context->error, __ATOMIC_ACQUIRE);
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

		/* Only a copied-pixel adapter requires host composition scratch before submission. */
		if (job->has_display != VK_FALSE && chain->gpu_present == VK_FALSE) {
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
	struct vulkan_swapchain *chain;
	uint32_t index;
	VkBool32 copied_busy;

	/* All target images are reserved together, without excluding another image of the same chain. */
	pthread_mutex_lock(&swapchain_mutex);

	/* Legacy copied adapters own one staging buffer and must finish its earlier reader before reuse. */
	for (;;) {
		copied_busy = VK_FALSE;
		for (index = 0U; index < job->count; index++) {
			/* GPU-resident adapters have one independent destination per image and need no chain-wide stall. */
			chain = job->chains[index];
			if (chain->gpu_present == VK_FALSE && chain->pending_jobs != 0U) {
				copied_busy = VK_TRUE;
				break;
			}
		}

		/* No earlier copied presentation still borrows a staging slot reserved by this job. */
		if (copied_busy == VK_FALSE)
			break;

		/* The worker retains no WSI lock while it completes this finite native operation. */
		pthread_cond_wait(&swapchain_condition, &swapchain_mutex);
	}

	/* Recheck every application image before publishing the all-or-nothing reservation. */
	for (index = 0U; index < job->count; index++) {
		/* Recheck ownership under the publication lock before changing any target. */
		chain = job->chains[index];
		if (chain->states[info->pImageIndices[index]] != WSI_IMAGE_ACQUIRED || chain->pending_jobs == UINT32_MAX) {
			pthread_mutex_unlock(&swapchain_mutex);
			return VK_ERROR_INITIALIZATION_FAILED;
		}
	}

	/* Each image contributes one chain lifetime hold until its job has retired. */
	for (index = 0U; index < job->count; index++) {
		chain = job->chains[index];
		/* This hold keeps the chain, lease and allocation policy alive until native work retires. */
		chain->pending_jobs++;
		chain->states[info->pImageIndices[index]] = WSI_IMAGE_PRESENTING;
	}

	/* Reservation makes cleanup responsible for returning every image and dropping every chain hold. */
	job->reserved = VK_TRUE;

	pthread_mutex_unlock(&swapchain_mutex);

	/* Succeeded: every image and chain remains owned until enqueue rollback or worker completion. */
	return VK_SUCCESS;
}

/* Records private GPU copy or explicit fallback staging without consuming caller waits. */
static VkResult
present_record(
	struct wsi_present_job *job,
	const VkPresentInfoKHR *info)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	const VkAllocationCallbacks *allocator;
	const VkDisplayPresentInfoKHR *display;
	uint32_t index;
	VkResult error;

	/* Reserves one independent slot before recording can overwrite cached command state. */
	error = present_fence_prepare(job);
	if (error != VK_SUCCESS)
		return error;

	/* A completed slot keeps command storage in its actual queue family. */
	allocator = swapchain_allocator(&job->allocator);
	if (job->completion->pool == VK_NULL_HANDLE) {
		/* Begin may implicitly reset this private buffer only after the slot retires. */
		memset(&pool, 0, sizeof(pool));
		pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool.queueFamilyIndex = job->queue->family;
		error = vkCreateCommandPool((VkDevice)job->queue->device, &pool, allocator, &job->completion->pool);
		if (error != VK_SUCCESS)
			return error;
	}

	/* A failed first allocation leaves its pool cached for the next ordinary retry. */
	if (job->completion->command == VK_NULL_HANDLE) {
		memset(&allocate, 0, sizeof(allocate));
		allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate.commandPool = job->completion->pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1U;
		error = vkAllocateCommandBuffers((VkDevice)job->queue->device, &allocate, &job->completion->command);
		if (error != VK_SUCCESS)
			return error;
	}

	/* The job borrows these identities until both GPU and native presentation use have retired. */
	job->pool = job->completion->pool;
	job->command = job->completion->command;

	/* The whole command buffer is ready before the one submission consumes waits. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	error = vkBeginCommandBuffer(job->command, &begin);
	if (error != VK_SUCCESS)
		return error;

	/* Every admitted target records its chosen presentation route within this single submission. */
	for (index = 0U; index < job->count; index++) {
		/* Skips readback commands for targets whose surface was already lost or replaced. */
		if (job->results[index] != VK_SUCCESS)
			continue;

		/* Optional display composition remains GPU-resident on both direct and shared-image paths. */
		display = NULL;
		if (job->has_display != VK_FALSE)
			display = &job->display;

		/* Copy and composition preserve the application's optimal image contents. */
		present_copy(job->command, job->chains[index], info->pImageIndices[index], job->queue->family, display);
	}


	/* Final recording failure occurs before any semaphore-consuming submit is attempted. */
	error = vkEndCommandBuffer(job->command);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: all fallible command storage exists before any queue operation. */
	return VK_SUCCESS;
}

/* Borrows one queue-owned fence slot and prepares its exact next generation before submission. */
static VkResult
present_fence_prepare(
	struct wsi_present_job *job)
{
	struct wsi_present_fence *completion;
	VkFenceCreateInfo fence;
	VkExportFenceCreateInfo export;
	VkFenceGetFdInfoKHR get_fd;
	struct gpu_fence_state state;
	const VkAllocationCallbacks *allocator;
	VkBool32 shared_fence;
	VkResult error;
	uint32_t index;
	int status;

	/* A native adapter can receive this job's exact shared producer generation without borrowing caller state. */
	shared_fence = VK_FALSE;
	if ((job->queue->device->object.context->capabilities & GPU_CAP_FENCE) != 0U) {
		/* One synchronized native target requires a shared producer payload for the whole submission. */
		for (index = 0U; index < job->count; index++) {
			/* Wayland and direct GPU images both use the same authoritative kernel completion payload. */
			if (job->chains[index]->gpu_present != VK_FALSE)
				shared_fence = VK_TRUE;
		}
	}

	/* Only idle slots of the right payload type can be borrowed by a new job. */
	pthread_mutex_lock(&swapchain_mutex);

	/* Cache reuse is exclusive until the previous job has retired every native request. */
	for (completion = job->worker->fences; completion != NULL; completion = completion->next) {
		/* Payload type cannot change while a cached native fence and fd remain alive. */
		if (completion->busy == VK_FALSE && completion->shared == shared_fence)
			break;
	}

	/* Busy transfers this slot to exactly one preparing or accepted job. */
	if (completion != NULL)
		completion->busy = VK_TRUE;

	pthread_mutex_unlock(&swapchain_mutex);

	/* New concurrency grows the cache once; allocation callbacks execute outside WSI serialization. */
	if (completion == NULL) {
		completion = vulkan_allocate(&job->allocator, sizeof(*completion), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
		if (completion == NULL)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Failed partial initialization remains queue-owned and can be retried or finally destroyed. */
		memset(completion, 0, sizeof(*completion));
		completion->fd = -1;
		completion->shared = shared_fence;
		completion->busy = VK_TRUE;

		/* The cache retains this candidate even if its later native fence creation fails. */
		pthread_mutex_lock(&swapchain_mutex);

		completion->next = job->worker->fences;
		job->worker->fences = completion;

		pthread_mutex_unlock(&swapchain_mutex);
	}

	/* Every partial initialization is owned by the queue and released by ordinary device teardown. */
	job->completion = completion;
	allocator = swapchain_allocator(&job->allocator);

	/* First use constructs native ownership; later uses advance the completed payload generation. */
	if (completion->fence == VK_NULL_HANDLE) {
		/* Ordinary Vulkan creation owns the optional shared completion payload. */
		memset(&fence, 0, sizeof(fence));
		fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

		/* A native shared fence is private WSI storage rather than an application extension dependency. */
		memset(&export, 0, sizeof(export));
		export.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
		export.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;

		/* Only native synchronized targets need the transferable private producer capability. */
		if (shared_fence != VK_FALSE)
			fence.pNext = &export;

		/* Native fence creation owns all completion resources before any public queue work is accepted. */
		error = vkCreateFence((VkDevice)job->queue->device, &fence, allocator, &completion->fence);
	} else {
		/* A completed or unsubmitted earlier use is reset before its slot can accept a new submission. */
		error = vkResetFences((VkDevice)job->queue->device, 1U, &completion->fence);
	}

	/* Failed native creation or reset leaves this cache slot owned but unusable by the current job. */
	if (error != VK_SUCCESS)
		return error;

	/* The job borrows native fence ownership until its final completion cleanup. */
	job->fence = completion->fence;

	/* Descriptor duplication and generation capture also finish before any wait semaphore is consumed. */
	if (shared_fence != VK_FALSE) {
		/* Descriptor duplication is needed only for a newly created or previously failed cache slot. */
		if (completion->fd < 0) {
			/* Exported ownership remains with the reusable queue slot until device teardown. */
			memset(&get_fd, 0, sizeof(get_fd));
			get_fd.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
			get_fd.fence = job->fence;
			get_fd.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
			error = vkGetFenceFdKHR((VkDevice)job->queue->device, &get_fd, &completion->fd);
			if (error != VK_SUCCESS)
				return error;
		}

		/* The job borrows the cached fd and never consumes its ownership. */
		job->fence_fd = completion->fd;

		/* Query reads the actual kernel payload generation, never a guessed renderer fence number. */
		memset(&state, 0, sizeof(state));
		state.version = GPU_ABI_VERSION;
		state.size = sizeof(state);
		state.fd = job->fence_fd;

		/* Serialize exact generation observation with other renderer-open control requests. */
		vulkan_context_lock(job->queue->device->object.context);

		status = ioctl(job->queue->device->object.context->fd, GPU_FENCE_QUERY, &state);

		vulkan_context_unlock(job->queue->device->object.context);

		/* A missing payload or zero generation cannot authorize native hardware access. */
		if (status != 0 || state.generation == 0U)
			return VK_ERROR_DEVICE_LOST;

		/* Capture the exact reset generation before any queue semaphore may be consumed. */
		job->fence_generation = state.generation;
	}

	/* Succeeded: fence and exported descriptor ownership remain with the queue until its final join. */
	return VK_SUCCESS;
}

/* Copies one image read-only and restores its public present-source layout. */
static void
present_copy(
	VkCommandBuffer command,
	struct vulkan_swapchain *chain,
	uint32_t index,
	uint32_t family,
	const VkDisplayPresentInfoKHR *display)
{
	VkImageMemoryBarrier image;
	VkBufferMemoryBarrier buffer;
	VkBufferImageCopy copy;

	/* Window-system buffers stay in GPU memory throughout transfer and presentation. */
	if (chain->gpu_present != VK_FALSE) {
		present_copy_shared(command, chain, index, family, display);
		return;
	}

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
	struct vulkan_swapchain *chain;
	uint32_t index;
	VkResult error;

	/* Mixed acquired software payloads and genuine GPU waits are handled by core. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = info->waitSemaphoreCount;
	submit.pWaitSemaphores = info->pWaitSemaphores;
	submit.pWaitDstStageMask = job->stages;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &job->command;
	error = vulkan_queue_submit_locked(job->queue, 1U, &submit, job->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Cleanup can no longer restore the transaction's pre-enqueue wait ownership. */
	job->submitted = VK_TRUE;

	/* External ownership changes only after the native queue accepts the recorded barriers. */
	for (index = 0U; index < job->count; index++) {
		/* Only barriers for accepted shared-image targets establish external ownership. */
		chain = job->chains[index];
		if (job->results[index] == VK_SUCCESS && chain->gpu_present != VK_FALSE)
			chain->presentation[info->pImageIndices[index]].shared_presented = VK_TRUE;
	}

	/* Succeeded: the ordered worker will wait for actual completion before any native image read. */
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
		/* Every packed pixel outside the eventual destination rectangle starts opaque black. */
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

		/* Scale the source row into the checked destination rectangle with nearest sampling. */
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

	/* Each native backend contributes to the same post-submission result handling. */
	combined = VK_SUCCESS;
	for (index = 0U; index < job->count; index++) {
		/* Targets rejected before submission never reach their native presentation adapter. */
		chain = job->chains[index];
		error = job->results[index];
		if (error == VK_SUCCESS) {
			/* A frame identifier belongs to its chain and never substitutes for a fence. */
			if (chain->frame == UINT64_MAX) {
				error = swapchain_device_lost(chain->device);
			} else if (chain->gpu_present != VK_FALSE) {
				/* Names this GPU image commit without creating a CPU pixel staging view. */
				chain->frame++;
				if (job->fence_fd >= 0 && chain->surface->platform->present_image_sync != NULL) {
					/* Native K repeats the exact producer dependency check before hardware access. */
					error = chain->surface->platform->present_image_sync(
						chain->lease,
						chain->presentation[info->pImageIndices[index]].native_image,
						chain->present_mode,
						&chain->sequence,
						job->fence_fd,
						job->fence_generation);
				} else {
					/* Wayland retains its protocol: actual producer completion precedes the ordinary commit. */
					error = chain->surface->platform->present_image(
						chain->lease,
						chain->presentation[info->pImageIndices[index]].native_image,
						chain->present_mode,
						&chain->sequence);
				}
			} else {
				/* Legacy direct display retains its completed-pixel fallback contract. */
				chain->frame++;
				memset(&pixels, 0, sizeof(pixels));
				pixels.pixels = chain->readback_pixels;
				pixels.extent = chain->extent;
				pixels.stride = chain->extent.width * 4U;
				pixels.format = chain->format;
				pixels.frame = chain->frame;

				/* Composed rectangles apply only to the standard direct-display extension. */
				if (job->has_display != VK_FALSE)
					present_compose(job, index, &pixels);

				/* The copied-pixel adapter retains its existing synchronous presentation semantics. */
				error = chain->surface->platform->present(chain->lease, &pixels, &chain->sequence);
			}
		}

		/* Consumed waits cannot be rolled back by allocation failure in either backend. */
		if (error == VK_ERROR_OUT_OF_HOST_MEMORY || error == VK_ERROR_OUT_OF_DEVICE_MEMORY)
			error = swapchain_device_lost(chain->device);

		/* Keeps each target's result independent while forming the caller-visible summary. */
		job->results[index] = error;
		combined = present_combine(combined, error);
	}

	/* A failed target retains its combined Vulkan error without changing successful peers. */
	if (combined < VK_SUCCESS)
		return combined;

	/* Succeeded: preserves accepted or suboptimal status from every presented target. */
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
	uint32_t index;
	uint32_t image;

	/* No helper may touch the queue when validation failed before it was assigned. */
	if (job->queue == NULL)
		return;

	/* A failed enqueue leaves acquisition unchanged; an enqueued present releases it. */
	pthread_mutex_lock(&swapchain_mutex);

	/* Cache reuse begins only after GPU completion or terminal device loss has retired this job. */
	if (job->completion != NULL)
		job->completion->busy = VK_FALSE;

	/* Each reservation contributes one lifetime hold and one independently acquired public image. */
	for (index = 0U; index < job->count; index++) {
		/* Partial preparation may not yet have resolved this target's swapchain. */
		chain = NULL;
		if (job->chains != NULL)
			chain = job->chains[index];

		/* Returns image ownership only for targets reserved by this transaction. */
		if (chain != NULL && job->reserved != VK_FALSE) {
			/* Zero permits destruction because no queued native operation still borrows this chain. */
			chain->pending_jobs--;
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

	/* Wake idle and destruction waiters after all image and chain ownership transitions are visible. */
	vulkan_wsi_image_notify();

	/* All application allocation callbacks run after the state mutex is released. */
	if (job->composed != NULL) {
		/* Only copied fallback jobs own these separately composed pixel arrays. */
		for (index = 0U; index < job->count; index++)
			vulkan_free(&job->allocator, job->composed[index]);
	}

	vulkan_free(&job->allocator, job->composed);
	vulkan_free(&job->allocator, job->stages);
	vulkan_free(&job->allocator, job->results);
	vulkan_free(&job->allocator, job->chains);
	vulkan_free(&job->allocator, job->indices);

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

	/* The second target retains a failure outside the priority classes above. */
	if (second < VK_SUCCESS)
		return second;

	/* Succeeded: neither target contributes a remaining failure. */
	return second;
}

/* Records failure when the library can no longer preserve ordinary resource state. */
static VkResult
swapchain_device_lost(
	struct VkDevice_T *device)
{
	/* The core's device error makes later queue and acquire operations fail consistently. */
	vulkan_device_error(device, VK_ERROR_DEVICE_LOST);

	/* Makes subsequent renderer commands observe the same terminal device loss. */
	if (device->object.context != NULL)
		vulkan_context_error(device->object.context, VK_ERROR_DEVICE_LOST);

	/* A consumed transaction cannot be reported as an unchanged allocation failure. */
	return VK_ERROR_DEVICE_LOST;
}

/* Copies a rendered optimal image into exportable linear GPU storage. */
static void
present_copy_shared(
	VkCommandBuffer command,
	struct vulkan_swapchain *chain,
	uint32_t index,
	uint32_t family,
	const VkDisplayPresentInfoKHR *display)
{
	VkImageMemoryBarrier barriers[2];
	VkImageMemoryBarrier clear_barrier;
	VkImageCopy copy;
	VkImageBlit blit;
	VkClearColorValue clear;

	/* The acquired destination is no longer in compositor use, so its old contents may retire. */
	memset(barriers, 0, sizeof(barriers));
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = chain->group->images[index].image;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].subresourceRange.levelCount = 1U;
	barriers[0].subresourceRange.layerCount = 1U;

	/* The separate linear allocation receives the GPU copy without discarding the app image. */
	barriers[1] = barriers[0];
	barriers[1].srcAccessMask = 0U;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].image = chain->presentation[index].shared_image;

	/* Compositor release precedes reacquisition from an earlier external consumer. */
	if (chain->presentation[index].shared_presented != VK_FALSE) {
		barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[1].srcQueueFamilyIndex = WSI_QUEUE_FAMILY_EXTERNAL;
		barriers[1].dstQueueFamilyIndex = family;
	}

	/* Makes producer reads and shared-allocation writes legal before copying pixels. */
	vkCmdPipelineBarrier(
		command,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0U,
		0U,
		NULL,
		0U,
		NULL,
		2U,
		barriers);

	/* Display rectangles compose into separate native storage without changing the application image. */
	if (display != NULL) {
		/* Uncovered pixels remain opaque black, matching the explicit copied composition path. */
		memset(&clear, 0, sizeof(clear));
		clear.float32[3] = 1.0f;
		vkCmdClearColorImage(command, barriers[1].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1U, &barriers[1].subresourceRange);

		/* The destination blit overwrites the completed clear within its selected rectangle. */
		clear_barrier = barriers[1];
		clear_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		clear_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		clear_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		clear_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		clear_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		clear_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &clear_barrier);

		/* Nearest sampling maps the checked source rectangle into the checked native destination. */
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1U;
		blit.dstSubresource = blit.srcSubresource;
		blit.srcOffsets[0].x = display->srcRect.offset.x;
		blit.srcOffsets[0].y = display->srcRect.offset.y;
		blit.srcOffsets[1].x = display->srcRect.offset.x + (int32_t)display->srcRect.extent.width;
		blit.srcOffsets[1].y = display->srcRect.offset.y + (int32_t)display->srcRect.extent.height;
		blit.srcOffsets[1].z = 1;
		blit.dstOffsets[0].x = display->dstRect.offset.x;
		blit.dstOffsets[0].y = display->dstRect.offset.y;
		blit.dstOffsets[1].x = display->dstRect.offset.x + (int32_t)display->dstRect.extent.width;
		blit.dstOffsets[1].y = display->dstRect.offset.y + (int32_t)display->dstRect.extent.height;
		blit.dstOffsets[1].z = 1;
		vkCmdBlitImage(command, barriers[0].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, barriers[1].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &blit, VK_FILTER_NEAREST);
	} else {
		/* Every pixel moves on the GPU; no mapped readback allocation exists on this path. */
		memset(&copy, 0, sizeof(copy));
		copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.srcSubresource.layerCount = 1U;
		copy.dstSubresource = copy.srcSubresource;
		copy.extent.width = chain->extent.width;
		copy.extent.height = chain->extent.height;
		copy.extent.depth = 1U;
		vkCmdCopyImage(
			command,
			barriers[0].image,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			barriers[1].image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1U,
			&copy);
	}

	/* Presentation observes complete writes only after this submission's fence completes. */
	barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	/* The shared allocation enters the external consumer's ownership in GENERAL layout. */
	barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[1].srcQueueFamilyIndex = family;
	barriers[1].dstQueueFamilyIndex = WSI_QUEUE_FAMILY_EXTERNAL;

	/* Completes both layout transitions within the submission's existing fence boundary. */
	vkCmdPipelineBarrier(
		command,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0U,
		0U,
		NULL,
		0U,
		NULL,
		2U,
		barriers);

	/* Succeeded: GPU commands copy and release the shared presentation allocation. */
	return;
}

/* Finds or starts the logical queue's independently ordered presentation worker. */
static VkResult
present_worker_get(
	struct VkQueue_T *queue,
	struct wsi_queue_worker **result)
{
	struct wsi_queue_worker *worker;
	struct wsi_queue_worker *candidate;
	int status;

	/* Allocation callbacks run before entering the global WSI state lock. */
	candidate = vulkan_allocate(&queue->device->object.allocator, sizeof(*candidate), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (candidate == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* A candidate owns no queue jobs until thread creation and list publication both succeed. */
	memset(candidate, 0, sizeof(*candidate));
	candidate->queue = queue;
	pthread_mutex_lock(&swapchain_mutex);

	/* Concurrent work on other queues cannot replace this queue's existing ordering domain. */
	for (worker = workers; worker != NULL; worker = worker->next) {
		/* Queue identity remains stable until the device has joined every worker. */
		if (worker->queue == queue)
			break;
	}

	/* Reuse a previously started worker without allocating another native thread. */
	if (worker != NULL) {
		*result = worker;
		pthread_mutex_unlock(&swapchain_mutex);
		vulkan_free(&queue->device->object.allocator, candidate);
		return VK_SUCCESS;
	}

	/* The new thread initially waits for this same mutex before inspecting any job. */
	status = pthread_create(&candidate->thread, NULL, present_worker_main, candidate);
	if (status != 0) {
		pthread_mutex_unlock(&swapchain_mutex);
		vulkan_free(&queue->device->object.allocator, candidate);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Publication keeps the worker discoverable by device drain and teardown. */
	candidate->next = workers;
	workers = candidate;
	*result = candidate;

	pthread_mutex_unlock(&swapchain_mutex);

	/* Succeeded: this queue has an owned worker ready for its next accepted presentation. */
	return VK_SUCCESS;
}

/* Waits for real producer completion before submitting each native presentation in queue order. */
static void *
present_worker_main(
	void *argument)
{
	struct wsi_queue_worker *worker;
	struct wsi_present_job *job;
	struct vulkan_swapchain *chain;
	struct vulkan_allocator allocator;
	VkResult error;
	VkResult native_error;
	VkResult waited;
	uint32_t index;
	VkBool32 early;

	/* The logical device joins this worker before any referenced queue can retire. */
	worker = argument;
	for (;;) {
		/* Idle workers sleep until publication or the device's explicit stop request. */
		pthread_mutex_lock(&swapchain_mutex);

		/* Empty queues wait without retaining any producer or native admission lock. */
		while (worker->head == NULL && worker->stopping == VK_FALSE)
			pthread_cond_wait(&swapchain_condition, &swapchain_mutex);

		/* Teardown waits for all accepted jobs before asking an empty worker to exit. */
		if (worker->head == NULL && worker->stopping != VK_FALSE) {
			pthread_mutex_unlock(&swapchain_mutex);
			break;
		}

		/* Queue order is fixed at publication; no later job can overtake its predecessor. */
		job = worker->head;
		worker->head = job->next;

		/* Empty queued work has no tail, while current continues to pin the accepted job. */
		if (worker->head == NULL)
			worker->tail = NULL;

		/* Current remains visible to idle and teardown until every transient owner is retired. */
		worker->current = job;

		pthread_mutex_unlock(&swapchain_mutex);

		/* Terminal native loss suppresses later submissions to the same obsolete surface. */
		for (index = 0U; index < job->count; index++) {
			/* A late error from earlier work invalidates this chain before any new native selection. */
			chain = job->chains[index];
			native_error = __atomic_load_n(&chain->error, __ATOMIC_ACQUIRE);
			if (native_error != VK_SUCCESS)
				job->results[index] = native_error;
		}

		/*
		 * A compositor that takes acquire fences gets the commit now, with the
		 * fence; the wait below then only retires the job.
		 */
		early = present_early(job);
		error = VK_SUCCESS;
		if (early)
			error = present_native(job, &job->info);

		/* Neither transport acceptance nor a context-zero display fence proves producer completion. */
		waited = vulkan_fences_wait(job->queue->device, 1U, &job->fence, VK_TRUE, UINT64_MAX);
		if (waited != VK_SUCCESS)
			error = swapchain_device_lost(job->queue->device);

		/* The unchanged Wayland v1 contract receives commits only after GPU writes and ownership release finish. */
		if (error == VK_SUCCESS && !early)
			error = present_native(job, &job->info);

		/* Native failures belong to future chain operations, never a returned caller output array. */
		for (index = 0U; index < job->count; index++) {
			/* Device loss dominates each target's previous per-surface result. */
			chain = job->chains[index];
			native_error = job->results[index];
			if (error == VK_ERROR_DEVICE_LOST)
				native_error = error;

			/* Success does not clear an earlier terminal asynchronous error. */
			if (native_error < VK_SUCCESS)
				__atomic_store_n(&chain->error, native_error, __ATOMIC_RELEASE);
		}

		/* Completion retires command storage and releases every job-owned image and chain hold. */
		allocator = job->allocator;
		present_finish(job, &job->info, error);

		/* A completed current pointer is cleared before device idle may observe an empty queue. */
		pthread_mutex_lock(&swapchain_mutex);

		worker->current = NULL;
		swapchain_notify_locked();

		pthread_mutex_unlock(&swapchain_mutex);
		vulkan_free(&allocator, job);
	}

	/* Succeeded: no job or library object remains borrowed by this native worker. */
	return NULL;
}

/*
 * Reports whether a job's images may be committed before the GPU finishes
 * them: every target presents a GPU image to a surface whose compositor takes
 * the job's exported fence with the commit.
 */
static VkBool32
present_early(
	const struct wsi_present_job *job)
{
	struct vulkan_swapchain *chain;
	uint32_t index;
	VkBool32 early;

	/* Without an exported fence there is nothing to send. */
	if (job->fence_fd < 0)
		return VK_FALSE;

	/* Every target must take it. */
	for (index = 0U; index < job->count; index++) {
		chain = job->chains[index];
		if (chain->gpu_present == VK_FALSE ||
		    chain->surface->platform->commit_early == NULL ||
		    chain->surface->platform->present_image_sync == NULL)
			return VK_FALSE;
		early = chain->surface->platform->commit_early(chain->lease);
		if (early == VK_FALSE)
			return VK_FALSE;
	}

	/* Succeeded: the commit may precede completion. */
	return VK_TRUE;
}

/* Drains accepted native request work without waiting for scanout retention or future frame replacement. */
static VkResult
present_drain(
	struct VkDevice_T *device,
	struct VkQueue_T *queue)
{
	struct wsi_queue_worker *worker;
	VkBool32 pending;
	VkResult error;

	/* Workers release this mutex throughout GPU, native display and Wayland waits. */
	pthread_mutex_lock(&swapchain_mutex);

	for (;;) {
		/* Device idle includes every queue while queue idle selects only one ordering domain. */
		pending = VK_FALSE;
		for (worker = workers; worker != NULL; worker = worker->next) {
			/* Ignore workers belonging to another logical device or queue. */
			if (worker->queue->device != device)
				continue;

			/* Queue idle observes only the specified public ordering domain. */
			if (queue != NULL && worker->queue != queue)
				continue;

			/* The current job remains pending until its transient owners have all retired. */
			if (worker->head != NULL || worker->current != NULL) {
				pending = VK_TRUE;
				break;
			}
		}

		/* Current front buffers are intentionally absent from this completion condition. */
		if (pending == VK_FALSE)
			break;

		/* Completion and device loss both eventually wake this ownership-based drain. */
		pthread_cond_wait(&swapchain_condition, &swapchain_mutex);
	}

	pthread_mutex_unlock(&swapchain_mutex);

	/* Only device loss belongs to the public queue/device idle result domain. */
	error = __atomic_load_n(&device->error, __ATOMIC_ACQUIRE);
	if (error != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;

	/* Succeeded: no accepted presentation job remains pending in the selected ordering domain. */
	return VK_SUCCESS;
}

/* Stops and joins every drained worker before logical-device children can be destroyed. */
static void
present_workers_stop(
	struct VkDevice_T *device)
{
	struct wsi_queue_worker *worker;
	struct wsi_queue_worker **link;
	struct wsi_present_fence *completion;
	const VkAllocationCallbacks *allocator;
	VkResult error;
	int status;

	/* Even a lost device must let jobs retire their host ownership before thread teardown. */
	error = present_drain(device, NULL);
	if (error != VK_SUCCESS)
		vulkan_device_error(device, error);

	/* Each joined worker consumes exactly one device-owned thread and allocation. */
	for (;;) {
		pthread_mutex_lock(&swapchain_mutex);

		/* Detach one matching worker while preserving all other device ordering domains. */
		link = &workers;
		while (*link != NULL && (*link)->queue->device != device)
			link = &(*link)->next;
		worker = *link;
		if (worker == NULL) {
			pthread_mutex_unlock(&swapchain_mutex);
			break;
		}

		/* An empty stopping worker exits at its next protected wakeup. */
		*link = worker->next;
		worker->stopping = VK_TRUE;
		swapchain_notify_locked();

		pthread_mutex_unlock(&swapchain_mutex);

		/* Joining precedes freeing the worker or any queue identity its thread could still inspect. */
		status = pthread_join(worker->thread, NULL);
		if (status != 0) {
			vulkan_device_error(device, VK_ERROR_DEVICE_LOST);
			return;
		}

		/* Cached native resources retire only after all associated jobs and their worker have drained. */
		allocator = swapchain_allocator(&device->object.allocator);
		while (worker->fences != NULL) {
			/* No accepted job can borrow this cached completion after its owning worker has joined. */
			completion = worker->fences;
			worker->fences = completion->next;

			/* No pending command buffer may outlive the worker that retained this pool. */
			if (completion->pool != VK_NULL_HANDLE)
				vkDestroyCommandPool((VkDevice)device, completion->pool, allocator);

			/* The fence remains alive until all command storage and marker accesses have retired. */
			if (completion->fence != VK_NULL_HANDLE)
				vkDestroyFence((VkDevice)device, completion->fence, allocator);

			/* The exported capability outlives all native dependencies and retires exactly once here. */
			if (completion->fd >= 0)
				close(completion->fd);

			/* The saved device allocation policy owns every dynamically grown completion slot. */
			vulkan_free(&device->object.allocator, completion);
		}

		/* Worker storage is unobservable after its thread and cache ownership have retired. */
		vulkan_free(&device->object.allocator, worker);
	}

	/* Succeeded: the device owns no background WSI thread or accepted presentation job. */
	return;
}

/* Acquires an image using either direct fd notification or finite Wayland protocol progress. */
static VkResult
swapchain_acquire(
	struct vulkan_swapchain *chain,
	uint64_t timeout,
	VkSemaphore semaphore,
	VkFence fence,
	uint32_t *result,
	struct vulkan_wake *wake)
{
	struct VkDevice_T *device;
	struct timespec deadline;
	uint64_t started;
	uint64_t now;
	uint64_t remaining;
	uint64_t delay;
	uint32_t offset;
	uint32_t index;
	VkResult error;
	VkBool32 acquired;
	VkBool32 available;
	int status;

	/* The public wrapper validated and retained the caller-owned chain for this operation. */
	device = chain->device;

	/* Measures the whole operation in the caller's monotonic timeout domain. */
	error = swapchain_now(&started);
	if (error != VK_SUCCESS)
		return error;

	/* Rechecks surface progress after each ownership notification or finite progress interval. */
	for (;;) {
		/* Draining precedes state observation, so a later error or release leaves an armed token. */
		if (wake != NULL) {
			/* Lazy registration has no token until the first observation actually requires blocking. */
			if (wake->read_fd >= 0)
				vulkan_wake_drain(wake);
		}

		/* Dispatches WSI-owned socket events without retaining the image-state mutex. */
		if (chain->surface->platform->progress != NULL) {
			error = chain->surface->platform->progress(chain->lease);
			if (error != VK_SUCCESS)
				return error;
		}

		/* Preserves asynchronous device and surface errors before choosing an image. */
		error = swapchain_current(chain);
		if (error != VK_SUCCESS)
			return error;

		/* Keeps the image-state observation and condition registration indivisible. */
		pthread_mutex_lock(&swapchain_mutex);

		/* An error published after capability refresh must still prevent a new indefinite sleep. */
		error = __atomic_load_n(&chain->error, __ATOMIC_ACQUIRE);
		if (error == VK_SUCCESS)
			error = __atomic_load_n(&device->error, __ATOMIC_ACQUIRE);

		/* Context failure can be published without acquiring the WSI state mutex. */
		if (error == VK_SUCCESS)
			error = __atomic_load_n(&device->object.context->error, __ATOMIC_ACQUIRE);

		/* Every error publication arms this caller's pipe after making its state visible. */
		if (error != VK_SUCCESS) {
			pthread_mutex_unlock(&swapchain_mutex);
			return error;
		}

		/* A replacement retires acquisition even if its own creation later fails. */
		if (chain->retired != VK_FALSE) {
			pthread_mutex_unlock(&swapchain_mutex);
			return VK_ERROR_OUT_OF_DATE_KHR;
		}

		/* Selects a reusable image without imposing an application presentation order. */
		acquired = VK_FALSE;
		index = chain->cursor;
		for (offset = 0U; offset < chain->group->count; offset++) {
			/* Compositor retention is independent of the producer's image-state entry. */
			available = VK_TRUE;
			if (chain->gpu_present != VK_FALSE && chain->surface->platform->image_available != NULL)
				available = chain->surface->platform->image_available(chain->presentation[index].native_image);

			/* Skips images still borrowed by application rendering or native presentation. */
			if (chain->states[index] != WSI_IMAGE_AVAILABLE || available == VK_FALSE) {
				index++;

				/* Wraps the cursor without changing ownership. */
				if (index == chain->group->count)
					index = 0U;

				/* A retained image contributes no ownership to this acquisition attempt. */
				continue;
			}

			/* Acquisition excludes competing callers before completion signaling begins. */
			chain->states[index] = WSI_IMAGE_ACQUIRED;
			acquired = VK_TRUE;
			break;
		}

		/* Leaves serialization before signaling the acquired image's public sync objects. */
		if (acquired != VK_FALSE) {
			pthread_mutex_unlock(&swapchain_mutex);
			break;
		}

		/* Timeout zero performs one observation without registering a sleep. */
		if (timeout == 0U) {
			pthread_mutex_unlock(&swapchain_mutex);
			return VK_NOT_READY;
		}

		/* Only a genuinely blocked direct call allocates its own retained wake token. */
		if (wake != NULL) {
			/* Registration runs unlocked and is followed by an unconditional full predicate recheck. */
			if (wake->read_fd < 0) {
				pthread_mutex_unlock(&swapchain_mutex);
				error = vulkan_wake_create(wake, device);
				if (error != VK_SUCCESS)
					return error;

				/* A release or error occurring before registration is visible to the next observation. */
				continue;
			}
		}

		/* Samples the remaining application deadline before either native or protocol sleep. */
		error = swapchain_now(&now);
		if (error != VK_SUCCESS) {
			pthread_mutex_unlock(&swapchain_mutex);
			return error;
		}

		/* A finite caller deadline takes priority over the protocol progress interval. */
		delay = WSI_PROGRESS_INTERVAL;
		if (wake != NULL)
			delay = UINT64_MAX;

		/* Finite application deadlines constrain both native fd and protocol waits. */
		if (timeout != UINT64_MAX) {
			/* Includes native capability and socket processing in the original deadline. */
			if (now - started >= timeout) {
				pthread_mutex_unlock(&swapchain_mutex);
				return VK_TIMEOUT;
			}

			/* Never deliberately sleeps beyond the remaining requested duration. */
			remaining = timeout - (now - started);
			if (remaining < delay)
				delay = remaining;
		}

		/* Direct display sleeps on real fd readiness, with no periodic progress quantum. */
		if (wake != NULL) {
			pthread_mutex_unlock(&swapchain_mutex);
			error = swapchain_poll(chain, wake, delay);
			if (error != VK_SUCCESS)
				return error;

			/* Native readiness must be acknowledged and image ownership rechecked before acquisition. */
			continue;
		}

		/* Converts the bounded absolute deadline without overflowing nanosecond arithmetic. */
		deadline.tv_sec = (time_t)(now / 1000000000ULL);
		deadline.tv_nsec = (long)(now % 1000000000ULL + delay);
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}

		/* Atomically releases image serialization while monotonic time or an event wakes this caller. */
		status = pthread_cond_clockwait(&swapchain_condition, &swapchain_mutex, CLOCK_MONOTONIC, &deadline);

		pthread_mutex_unlock(&swapchain_mutex);

		/* Spurious notifications and elapsed progress intervals both require another observation. */
		if (status != 0 &&
		    status != ETIMEDOUT &&
		    status != EINTR) {
			vulkan_device_error(device, VK_ERROR_DEVICE_LOST);
			return VK_ERROR_DEVICE_LOST;
		}
	}

	/* Software-complete acquisition does not wait behind unrelated GPU work. */
	error = vulkan_wsi_acquire_signal(device, semaphore, fence);
	if (error != VK_SUCCESS) {
		/* Failed signaling restores image ownership before waking another acquirer. */
		pthread_mutex_lock(&swapchain_mutex);

		chain->states[index] = WSI_IMAGE_AVAILABLE;
		swapchain_notify_locked();

		pthread_mutex_unlock(&swapchain_mutex);

		/* Failure: the public completion primitive acquired no reusable image. */
		return error;
	}

	/* Advances selection only after the public completion primitive was signaled. */
	pthread_mutex_lock(&swapchain_mutex);

	chain->cursor = (index + 1U) % chain->group->count;

	pthread_mutex_unlock(&swapchain_mutex);

	/* Succeeded: the caller owns this reusable image and its signaled completion primitive. */
	*result = index;
	return VK_SUCCESS;
}

/* Waits on native errors, topology and independent image-ownership publication. */
static VkResult
swapchain_poll(
	struct vulkan_swapchain *chain,
	struct vulkan_wake *wake,
	uint64_t timeout)
{
	struct pollfd descriptors[3];
	struct timespec interval;
	const struct timespec *deadline;
	int status;

	/* Ignoring ordinary GPU POLLIN avoids spinning on another observer's retained completion. */
	memset(descriptors, 0, sizeof(descriptors));
	descriptors[0].fd = chain->device->object.context->fd;
	descriptors[1].fd = chain->surface->platform->wait_descriptor(chain->lease);
	descriptors[1].events = POLLPRI;
	descriptors[2].fd = wake->read_fd;
	descriptors[2].events = POLLIN;
	deadline = NULL;
	if (timeout != UINT64_MAX) {
		/* The caller already deducted inventory processing from its monotonic deadline. */
		interval.tv_sec = (time_t)(timeout / UINT64_C(1000000000));
		interval.tv_nsec = (long)(timeout % UINT64_C(1000000000));
		deadline = &interval;
	}

	/* No queue, device, renderer or WSI mutex is retained throughout this fd wait. */
	status = ppoll(descriptors, 3, deadline, NULL);
	if (status < 0 && errno != EINTR) {
		vulkan_device_error(chain->device, VK_ERROR_DEVICE_LOST);
		return VK_ERROR_DEVICE_LOST;
	}

	/* Native events and interruption both require fresh error, topology and image observations. */
	if ((descriptors[2].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
		vulkan_device_error(chain->device, VK_ERROR_DEVICE_LOST);
		return VK_ERROR_DEVICE_LOST;
	}

	/* Succeeded: the caller will consume its token before rechecking all protected predicates. */
	return VK_SUCCESS;
}

/* Publishes image-state changes to condition waiters and independently armed fd waiters. */
static void
swapchain_notify_locked(
	void)
{
	/* Callers retain swapchain_mutex so registration and predicate checks cannot miss this transition. */
	pthread_cond_broadcast(&swapchain_condition);
	vulkan_wake_notify(NULL, NULL);

	/* Succeeded: no waiter consumes another waiter's independently retained event. */
	return;
}
