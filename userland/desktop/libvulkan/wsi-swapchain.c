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
#include "sync-internal.h"

#include <errno.h>
#include <poll.h>
#include <uapi/gpu-fence.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WSI_IMAGE_AVAILABLE	0U
#define WSI_IMAGE_ACQUIRED	1U
#define WSI_IMAGE_PRESENTING	2U
#define WSI_COMPLETION_TIMEOUT	10000000000ULL
#define WSI_PROGRESS_INTERVAL	10000000ULL

/* How long a cursor present keeps trying while the output has no free cursor image. */
#define WSI_CURSOR_BUSY_LIMIT	1000000000ULL

/* The refresh period a cursor retry waits when the mode names no rate (60 Hz). */
#define WSI_CURSOR_DEFAULT_PERIOD	16666667ULL

/* The environment variable that turns on one log line for every cursor present. */
#define WSI_CURSOR_LOG_VARIABLE	"VULKAN_ZEDBSD_CURSOR_LOG"

/* Shared presentation allocations transfer ownership to another Vulkan instance. */
#define WSI_QUEUE_FAMILY_EXTERNAL 0xfffffffeU

/*
 * How many slots of one payload kind a worker keeps before a present whose
 * recording no idle slot holds records over an idle slot's (ws211-p011).
 */
#define WSI_PRESENT_SLOTS	8U

/*
 * One ordinary bound image whose allocation lives with a shared image group.
 * A direct chain's image is itself the linear shared image the output
 * shows: fd is its allocation's capability until the import takes it (-1
 * otherwise), with the descriptor the import needs.
 */
struct wsi_image {
	VkImage image;
	VkDeviceMemory memory;
	int fd;
	struct gpu_image_descriptor descriptor;
};

/* One swapchain owns its scanout destination independently from shared application VkImages. */
struct wsi_present_image {
	VkImage shared_image;
	VkDeviceMemory shared_memory;
	void *native_image;
	VkBool32 shared_presented;
	/*
	 * What the native image holds, for a cursor chain: copied_valid once a
	 * completed copy of the application image put its pixels there, with
	 * the image's content serial and the device's content epoch that copy
	 * saw.  While both still match, a present may show the native image
	 * without copying again.  The swapchain mutex protects all three.
	 */
	uint64_t copied_serial;
	uint64_t copied_epoch;
	VkBool32 copied_valid;
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
	/*
	 * The application renders into the linear images the output shows
	 * (a direct display, not a cursor or overlay plane): a present copies
	 * nothing and only waits for the rendering before the flip.
	 */
	VkBool32 direct;
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
	/*
	 * The application's image use is transfer only, so every write of an
	 * image is a transfer command the content serial counts; only such a
	 * cursor chain shows an earlier copy without copying again.
	 */
	VkBool32 transfer_only;
	/*
	 * The chain's number, which no other chain of the process ever has: a
	 * slot's kept recording names its chain by it (ws211-p011), since a
	 * freed chain's address can be a later chain's.
	 */
	uint64_t serial;
	/*
	 * The application may give the images away (it named
	 * VkExternalMemoryImageCreateInfo at creation): a direct chain's image
	 * goes out as an fd with vkGetMemoryFdKHR.
	 */
	VkBool32 exportable;
	/* The chain's creation, which a window system's lend of images is checked against. */
	struct vulkan_wsi_lend_fit fit;
	/* The serial of the window system's lend whose images the chain draws into; zero for a chain of its own images. */
	uint64_t lend_serial;
};

/* The independently ordered workers of one queue: ordinary presents, the cursor plane's, and the overlay planes'. */
enum wsi_lane {
	WSI_LANE_ORDINARY,
	WSI_LANE_CURSOR,
	WSI_LANE_OVERLAY
};

/* How one cursor-plane or overlay-plane present reaches the output. */
enum wsi_route {
	/* The worker waits for a GPU copy of the image, then shows it. */
	WSI_ROUTE_COPY,
	/* The worker waits for the present's GPU waits, then shows the unchanged earlier copy. */
	WSI_ROUTE_EMPTY,
	/* The caller shows the unchanged earlier copy at once, with no GPU work. */
	WSI_ROUTE_FAST
};

/* The counter times of one cursor present for its log line; zero where the present had no such step. */
struct wsi_cursor_log {
	uint64_t call_ns;
	uint64_t call;
	uint64_t mutex_wait;
	uint64_t queue_wait;
	uint64_t publish;
	uint64_t fence;
	struct vulkan_wsi_cursor_times times;
	uint32_t depth;
	uint32_t retries;
	/* Why a cursor present took its route: the first condition of the fast path it failed, or none. */
	const char *why;
};

/* A job borrows its queue worker until the ordered completion path retires it. */
struct wsi_queue_worker;

/*
 * What one present's copy command buffer records (ws211-p011).
 *
 * The commands depend on nothing else: the chain's images, the image, how
 * far the native image's ownership has come (shared_presented), the display
 * rectangles composed into it, and whether a cursor present copies at all.
 * A slot whose command buffer holds a recording with the same key is
 * submitted again without recording it, which saves the Begin and End round
 * trips to the renderer.  Zeroed, it is no recording (valid false); it is
 * compared byte for byte, so it is always zeroed before it is filled.
 */
struct wsi_present_recording {
	VkBool32 valid;
	enum wsi_route route;
	uint64_t chain;
	uint32_t image;
	VkBool32 presented;
	VkBool32 composed;
	VkRect2D source;
	VkRect2D destination;
};

/* One concurrent job borrows cached command storage and a fence until GPU and native use retire. */
struct wsi_present_fence {
	struct wsi_present_fence *next;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	int fd;
	VkBool32 shared;
	/*
	 * True for a slot whose fences are sent to a Wayland compositor: each
	 * present makes a new fence there and none is reset (ws103-p005).  A slot
	 * keeps this for its lifetime, so a display job never resets a fence a
	 * compositor may still be polling.
	 */
	VkBool32 sent;
	VkBool32 busy;
	/*
	 * True while a display slot's fence is already reset for the slot's
	 * next job, generation then being its payload generation: the worker
	 * resets it as the job retires, so the next present spares the reset's
	 * round trips to the renderer.  Only the job that has the slot busy
	 * changes these.
	 */
	VkBool32 rearmed;
	uint64_t generation;
	/*
	 * What the command buffer holds after its last successful End, or no
	 * recording.  Only the job that has the slot busy changes it.
	 */
	struct wsi_present_recording recording;
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
	/* Each target's changed part (x, y, width, height), from the application's present regions; NULL when none were named. */
	struct wsi_present_damage *damage;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	int fence_fd;
	uint64_t fence_generation;
	uint32_t count;
	VkBool32 submitted;
	VkBool32 reserved;
	/* The worker the job goes to: cursor-plane presents never wait behind ordinary ones. */
	enum wsi_lane lane;
	enum wsi_route route;
	/* Each target's cursor ticket, taken at reservation; zero for a target that is not a cursor plane. */
	uint64_t *tickets;
	/*
	 * For a cursor-lane job: the image's content serial and the device's
	 * content epoch at reservation, which its copy shows; copy_done once
	 * the worker saw the copy complete, so completion records them.
	 */
	uint64_t content_serial;
	uint64_t content_epoch;
	VkBool32 copy_recorded;
	VkBool32 copy_done;
	/* The job carries no GPU work: its waits were consumed and only the native present remains. */
	VkBool32 native_only;
	/*
	 * What the job's copy command buffer records, and whether the slot it
	 * borrowed already held that recording, which it then submits as it is.
	 */
	struct wsi_present_recording recording;
	VkBool32 reused;
	/*
	 * Each target's overlay present (ws214-p004), allocated when a target
	 * is an overlay plane: the swapchain fills what it asks, the output's
	 * decision its serial and whether it is held.  NULL otherwise.
	 */
	struct vulkan_wsi_overlay_present *overlays;
	/* What the last overlay commit of the job did. */
	enum vulkan_wsi_overlay_outcome overlay_outcome;
	/* What the cursor present did, and its log times when the log is on. */
	enum vulkan_wsi_cursor_outcome outcome;
	VkBool32 logged;
	struct wsi_cursor_log log;
};

/* The part of one target's image that changed (VK_KHR_incremental_present): known, and the box around its rectangles. */
struct wsi_present_damage {
	VkBool32 known;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
};

/* One logical queue owns an ordered worker per lane whose waits never retain queue or WSI locks. */
struct wsi_queue_worker {
	struct wsi_queue_worker *next;
	struct VkQueue_T *queue;
	enum wsi_lane lane;
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

/*
 * The serial of the chain created last; a new chain takes the next one.
 * It only ever increases, so no two chains of the process share a serial.
 */
static uint64_t swapchain_serial;

/*
 * Nonzero once a cursor swapchain was created while the cursor log's
 * environment variable was set; it never returns to zero.  A present reads
 * the clocks only while it is set.
 */
static int present_cursor_log_on;

static struct vulkan_swapchain *swapchain_get(VkSwapchainKHR handle);
static VkResult swapchain_create(struct VkDevice_T *device, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *allocator, struct wsi_image_group *shared, VkBool32 single, struct vulkan_swapchain **result);
static VkBool32 swapchain_spare_wanted(struct VkDevice_T *device, struct vulkan_surface *surface, const VkSwapchainCreateInfoKHR *info);
static VkResult swapchain_validate(struct VkDevice_T *device, const VkSwapchainCreateInfoKHR *info, struct vulkan_surface **surface);
static VkResult swapchain_publish(struct vulkan_swapchain *chain);
static void swapchain_free(struct vulkan_swapchain *chain);
static VkResult swapchain_group_create(struct vulkan_swapchain *chain, const VkSwapchainCreateInfoKHR *info);
static void swapchain_group_free(struct wsi_image_group *group);
static VkResult swapchain_image_create(struct vulkan_swapchain *chain, const VkSwapchainCreateInfoKHR *info, struct wsi_image *image);
static VkResult swapchain_readback_create(struct vulkan_swapchain *chain);
static VkResult swapchain_native_images_create(struct vulkan_swapchain *chain);
static VkBool32 swapchain_direct_possible(struct vulkan_swapchain *chain, const VkSwapchainCreateInfoKHR *info);
static VkResult swapchain_native_images_import(struct vulkan_swapchain *chain);
static VkResult swapchain_exportable(struct vulkan_swapchain *chain, const VkSwapchainCreateInfoKHR *info);
static void swapchain_fit(struct vulkan_swapchain *chain, const VkSwapchainCreateInfoKHR *info, uint32_t asked);
static VkResult swapchain_lent_create(struct vulkan_swapchain *chain, const VkSwapchainCreateInfoKHR *info, uint32_t count, const int *fds);
static VkBool32 swapchain_lend_changed(struct vulkan_swapchain *chain);
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
static VkResult present_regions(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static VkResult present_fence_prepare(struct wsi_present_job *job);
static VkResult present_record(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static void present_recording_key(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static VkBool32 present_recording_same(const struct wsi_present_recording *kept, const struct wsi_present_recording *wanted);
static VkBool32 present_recording_image(const struct wsi_present_recording *kept, const struct wsi_present_recording *wanted);
static void present_slots_forget(struct vulkan_swapchain *chain);
static struct wsi_present_fence *present_slot_of_chain(const struct vulkan_swapchain *chain);
static void present_copy(VkCommandBuffer command, struct vulkan_swapchain *chain, uint32_t index, uint32_t family, const VkDisplayPresentInfoKHR *display);
static void present_copy_shared(VkCommandBuffer command, struct vulkan_swapchain *chain, uint32_t index, uint32_t family, const VkDisplayPresentInfoKHR *display);
static VkResult present_submit(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static void present_fence_rearm(struct wsi_present_job *job);
static void present_compose(struct wsi_present_job *job, uint32_t index, struct vulkan_wsi_pixels *pixels);
static VkResult present_native(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static VkBool32 present_early(const struct wsi_present_job *job);
static void present_finish(struct wsi_present_job *job, const VkPresentInfoKHR *info, VkResult error);
static VkResult present_combine(VkResult first, VkResult second);
static VkResult swapchain_device_lost(struct VkDevice_T *device);
static VkResult present_worker_get(struct VkQueue_T *queue, enum wsi_lane lane, struct wsi_queue_worker **result);
static void present_lane(struct wsi_present_job *job);
static enum wsi_route present_route(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static VkBool32 present_waits_consume(struct wsi_present_job *job, const VkPresentInfoKHR *info);
static VkResult present_fast(struct wsi_present_job *job, const VkPresentInfoKHR *info, VkBool32 *published);
static VkResult present_cursor_show(struct wsi_present_job *job, uint32_t index, VkBool32 retry);
static void present_cursor_pause(struct vulkan_swapchain *chain);
static VkResult present_overlay_prepare(struct wsi_present_job *job);
static VkResult present_overlay_rects(const struct wsi_present_job *job, const struct vulkan_swapchain *chain);
static VkBool32 present_overlay_hidden(const VkDisplayPresentInfoKHR *display);
static void present_overlay_decide(struct wsi_present_job *job, VkBool32 ready);
static VkResult present_overlay_commit(struct wsi_present_job *job, uint32_t index, VkBool32 retry);
static void present_overlay_settle(struct wsi_present_job *job, VkResult error);
static void present_primary_skip(struct wsi_present_job *job, uint32_t index, VkResult error);
static VkResult swapchain_reclaim(struct vulkan_swapchain *chain, uint64_t timeout, uint64_t started);
static void present_publish_locked(struct wsi_queue_worker *worker, struct wsi_present_job *job);
static void present_cursor_log_start(struct wsi_cursor_log *log);
static void present_cursor_log(const struct wsi_present_job *job, const char *path);
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
	error = swapchain_create(device, info, allocator, NULL, VK_TRUE, &chain);
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
		error = swapchain_create(device, &infos[index], allocator, group, VK_FALSE, &chains[index]);
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
		/* A display's events, or a window system's connection, wake the wait as well as the image-state changes. */
		if (chain->surface->platform->wait_descriptor != NULL || chain->surface->platform->wait_input != NULL)
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
 *
 * A cursor plane's present whose image is unchanged since its last copy,
 * and whose waits are already complete, is shown at once without GPU work
 * or the worker (ws141-p016 (B)).
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
	VkResult result;
	VkBool32 published;
	VkBool32 changed;
	uint64_t waited;
	uint64_t present_started;
	uint64_t part_started;
	size_t bytes;
	uint32_t index;
	int logging;

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
	job->outcome = VULKAN_WSI_CURSOR_DROPPED;

	/* The cursor log's start times, read only while the log is on. */
	logging = __atomic_load_n(&present_cursor_log_on, __ATOMIC_RELAXED);
	if (logging != 0)
		present_cursor_log_start(&job->log);

	/* Validates the targets and reserves their images (timed for VULKAN TIMING, as the parts below). */
	present_started = vulkan_timing_start();
	part_started = present_started;
	error = present_prepare(job, queue, info);
	if (error != VK_SUCCESS)
		goto cleanup;
	vulkan_timing_add(VULKAN_TIMING_PRESENT_PREPARE, part_started, 0U);

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

	/* A cursor plane's present is logged when the log is on. */
	if (logging != 0 && job->lane == WSI_LANE_CURSOR)
		job->logged = VK_TRUE;

	/* A cursor or overlay plane's present chooses between a copy, waits alone, and showing at once. */
	if (job->lane == WSI_LANE_CURSOR || job->lane == WSI_LANE_OVERLAY)
		job->route = present_route(job, info);

	/* Shows an unchanged cursor or overlay image at once; only a busy output hands it to the worker. */
	if (job->route == WSI_ROUTE_FAST) {
		error = present_fast(job, info, &published);

		/* A published job belongs to its worker, which reports nothing more to this caller. */
		if (published != VK_FALSE)
			job = NULL;
		goto cleanup;
	}

	/* The job's ordered worker: the cursor plane's never waits behind ordinary presents. */
	error = present_worker_get(queue, job->lane, &worker);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* Recording and native metadata allocation complete before waits can be consumed. */
	job->worker = worker;
	part_started = vulkan_timing_start();
	error = present_record(job, info);
	if (error != VK_SUCCESS)
		goto cleanup;
	vulkan_timing_add(VULKAN_TIMING_PRESENT_RECORD, part_started, 0U);

	/* Queue submission and worker publication retain one issue order with other queue operations. */
	waited = 0U;
	if (job->logged != VK_FALSE)
		waited = vulkan_wsi_cursor_clock();

	pthread_mutex_lock(&queue->mutex);

	/* The time the queue lock took, for the cursor log. */
	if (job->logged != VK_FALSE)
		job->log.queue_wait = vulkan_wsi_cursor_clock() - waited;

	/* Consumes every wait in the one submission that also carries the copy. */
	part_started = vulkan_timing_start();
	error = present_submit(job, info);
	if (error != VK_SUCCESS) {
		pthread_mutex_unlock(&queue->mutex);
		goto cleanup;
	}
	vulkan_timing_add(VULKAN_TIMING_PRESENT_SUBMIT, part_started, 0U);

	/* Each overlay target is held for a primary present or left to its own commit, in call order. */
	if (job->overlays != NULL)
		present_overlay_decide(job, VK_FALSE);

	/* Publish only results known at enqueue time while caller-owned result storage is still valid. */
	combined = VK_SUCCESS;
	for (index = 0U; index < job->count; index++) {
		/* A target a window system's lend would serve better (or whose lend ended) is presented, and suboptimal. */
		result = job->results[index];
		if (result == VK_SUCCESS) {
			changed = swapchain_lend_changed(job->chains[index]);
			if (changed != VK_FALSE)
				result = VK_SUBOPTIMAL_KHR;
		}

		/* Native asynchronous failures are latched later on the chain, never written into this array. */
		if (info->pResults != NULL)
			info->pResults[index] = result;

		/* Each independently validated target contributes its enqueue status. */
		combined = present_combine(combined, result);
	}

	/* From this publication onward only the worker may release the owned job. */
	pthread_mutex_lock(&swapchain_mutex);

	present_publish_locked(worker, job);

	pthread_mutex_unlock(&swapchain_mutex);

	pthread_mutex_unlock(&queue->mutex);

	/* Publication transfers the entire job to its worker, including all cleanup responsibility. */
	job = NULL;
	error = combined;

cleanup:
	/* Native acceptance followed by notification failure is terminal, never an ordinary retry. */
	if (error == VK_ERROR_DEVICE_LOST)
		error = swapchain_device_lost(queue->device);

	/* A primary ticket of a job that goes no further drops the overlay changes held for it. */
	if (job != NULL &&
	    job->tickets != NULL &&
	    error != VK_SUCCESS) {
		for (index = 0U; index < job->count; index++)
			present_primary_skip(job, index, error);
	}

	/* Only unpublished jobs remain caller-owned; preparation failure restores acquisition. */
	if (job != NULL) {
		present_finish(job, info, error);
		vulkan_free(&allocator, job);
	}

	/* The whole call's time, and the sums said every few seconds (VULKAN TIMING). */
	vulkan_timing_add(VULKAN_TIMING_PRESENT, present_started, 0U);
	vulkan_timing_report();

	/* Rejected targets or failed enqueue retain their precise public error result. */
	if (error < VK_SUCCESS)
		return error;

	/* Succeeded: the library owns the accepted GPU work and ordered native presentation job. */
	return error;
}

/*
 * Reads a monotonic nanosecond clock fine enough for the cursor log's lengths.
 *
 * The system's monotonic clock advances once a millisecond on the Pi 4, so
 * on arm64 the generic timer's virtual counter is read instead (the kernel
 * lets user code read it); elsewhere the monotonic clock is fine enough.
 */
uint64_t
vulkan_wsi_cursor_clock(
	void)
{
#if defined(__aarch64__)
	uint64_t count;
	uint64_t frequency;

	/* The counter and its rate. */
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(count));
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));

	/* A timer without a rate cannot be converted. */
	if (frequency == 0U)
		return 0U;

	/* Succeeded: whole seconds and the remainder, each scaled without overflow. */
	return (count / frequency) * 1000000000ULL + (count % frequency) * 1000000000ULL / frequency;
#else
	uint64_t nanoseconds;
	VkResult error;

	/* The monotonic clock. */
	error = swapchain_now(&nanoseconds);
	if (error != VK_SUCCESS)
		return 0U;

	/* Succeeded: the monotonic time. */
	return nanoseconds;
#endif
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

/*
 * Finds the memory and description of an image of an exportable direct
 * chain of a device (one created with VkExternalMemoryImageCreateInfo):
 * what vkGetMemoryFdKHR gives away for a swapchain image.  Returns
 * VK_SUCCESS, or VK_ERROR_INVALID_EXTERNAL_HANDLE for an image of no such
 * chain.
 */
VkResult
vulkan_wsi_swapchain_image_memory(
	struct VkDevice_T *device,
	VkImage image,
	VkDeviceMemory *memory,
	struct gpu_image_descriptor *descriptor)
{
	struct vulkan_swapchain *chain;
	uint32_t index;
	VkBool32 found;

	/* Looks through the device's exportable direct chains for the image. */
	found = VK_FALSE;
	pthread_mutex_lock(&swapchain_mutex);

	for (chain = swapchains; chain != NULL && found == VK_FALSE; chain = chain->next) {
		/* Only this device's direct chain of its own images that may give them away. */
		if (chain->device != device || chain->exportable == VK_FALSE)
			continue;
		if (chain->direct == VK_FALSE || chain->lend_serial != 0U)
			continue;

		/* The image among the chain's own. */
		for (index = 0U; index < chain->group->count; index++) {
			if (chain->group->images[index].image != image)
				continue;
			*memory = chain->group->images[index].memory;
			*descriptor = chain->group->images[index].descriptor;
			found = VK_TRUE;
			break;
		}
	}

	pthread_mutex_unlock(&swapchain_mutex);

	/* No exportable chain has the image. */
	if (found == VK_FALSE)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* Succeeded: the caller exports the image's memory with its description. */
	return VK_SUCCESS;
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

/*
 * Acquires complete private state without publishing a partially usable
 * chain; a single chain (not one of a shared creation) may get a spare
 * image (swapchain_spare_wanted).
 */
static VkResult
swapchain_create(
	struct VkDevice_T *device,
	const VkSwapchainCreateInfoKHR *info,
	const VkAllocationCallbacks *allocator,
	struct wsi_image_group *shared,
	VkBool32 single,
	struct vulkan_swapchain **result)
{
	VkSwapchainCreateInfoKHR counted;
	struct vulkan_swapchain *chain;
	struct vulkan_swapchain *old;
	struct vulkan_surface *surface;
	struct vulkan_object *object;
	const char *log_variable;
	int lent_fds[VULKAN_WSI_LEND_MAX];
	uint64_t lent_serial;
	uint32_t lent_count;
	uint32_t asked;
	uint32_t index;
	VkResult error;
	size_t bytes;
	int log_compare;

	/* Retirement is required even when a later allocation or validation fails; no lent image fd is held yet. */
	*result = NULL;
	lent_count = 0U;
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

		/* A retired overlay chain's held change is never shown (ws214-p004). */
		if (old->surface->overlay != VK_FALSE &&
		    old->lease != NULL &&
		    old->surface->platform->overlay_forget != NULL)
			old->surface->platform->overlay_forget(old->lease, old->serial);
	}

	/* Surface capabilities and mode generation precede native resource acquisition. */
	error = swapchain_validate(device, info, &surface);
	if (error != VK_SUCCESS)
		return error;

	/* The image count the application asked for, before any spare. */
	asked = info->minImageCount;

	/* The images the chain makes: the asked count, and one more when a spare is wanted. */
	if (single != VK_FALSE && swapchain_spare_wanted(device, surface, info)) {
		counted = *info;
		counted.minImageCount++;
		info = &counted;
	}
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

	/* The chain's own number, which its slots' kept recordings name it by. */
	chain->serial = __atomic_add_fetch(&swapchain_serial, 1U, __ATOMIC_RELAXED);

	/* Only transfer commands, which the content serial counts, can write a transfer-only chain's images. */
	if ((info->imageUsage & ~(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) == 0U)
		chain->transfer_only = VK_TRUE;

	/* A cursor swapchain asks once whether its presents are to be logged. */
	log_variable = NULL;
	if (surface->cursor != VK_FALSE)
		log_variable = getenv(WSI_CURSOR_LOG_VARIABLE);

	/* Only the value 1 turns the log on, for every later present. */
	if (log_variable != NULL) {
		log_compare = strcmp(log_variable, "1");
		if (log_compare == 0)
			__atomic_store_n(&present_cursor_log_on, 1, __ATOMIC_RELAXED);
	}

	/* Retains the instance surface before any native lease can borrow its allocator. */
	error = vulkan_wsi_surface_retain(surface);
	if (error != VK_SUCCESS) {
		chain->surface = NULL;
		goto cleanup;
	}

	error = surface->platform->claim(surface, device, &chain->lease);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* A see-through alpha mode is told to the native backend, which advertised it, before any image is shared. */
	if (info->compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
		error = VK_ERROR_INITIALIZATION_FAILED;
		if (surface->platform->composite_alpha != NULL)
			error = surface->platform->composite_alpha(chain->lease, info->compositeAlpha);
		if (error != VK_SUCCESS)
			goto cleanup;
	}

	/* Whether the application gives the images away (a compositor lending them, vkGetMemoryFdKHR). */
	error = swapchain_exportable(chain, info);
	if (error != VK_SUCCESS)
		goto cleanup;

	/*
	 * The chain's creation, told to a window system that lends images,
	 * and the images of its lend when one fits (a compositor's game mode):
	 * the chain then draws into them.  A chain of a shared creation never
	 * takes a lend.
	 */
	swapchain_fit(chain, info, asked);
	if (shared == NULL && surface->platform->lend != NULL) {
		error = surface->platform->lend(chain->lease, &chain->fit, VULKAN_WSI_LEND_MAX, &lent_count, lent_fds, &lent_serial);

		/* No lend fits: the chain makes its own images. */
		if (error == VK_ERROR_FEATURE_NOT_PRESENT) {
			lent_count = 0U;
			error = VK_SUCCESS;
		}
		if (error != VK_SUCCESS)
			goto cleanup;

		/* A lend fits: the chain draws into its images. */
		if (lent_count != 0U)
			chain->lend_serial = lent_serial;
	}

	/* Every chain has independent acquisition state even when its images are shared; room for the lend's count or its own. */
	bytes = (size_t)info->minImageCount;
	if (chain->lend_serial != 0U && lent_count > info->minImageCount)
		bytes = (size_t)lent_count;
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

	/* A lent chain's images are the lend's, which the compositor shows as they are: nothing is copied. */
	if (chain->lend_serial != 0U) {
		chain->direct = VK_TRUE;
		error = swapchain_lent_create(chain, info, lent_count, lent_fds);
		if (error == VK_SUCCESS) {
			chain->gpu_present = VK_TRUE;

			/* Transfers the complete private chain to the publication stage. */
			*result = chain;
			chain = NULL;
			goto cleanup;
		}

		/* Lent images this device cannot import (another layout, another device): refused, and the chain has its own. */
		if (error != VK_ERROR_FORMAT_NOT_SUPPORTED &&
		    error != VK_ERROR_FEATURE_NOT_PRESENT &&
		    error != VK_ERROR_INVALID_EXTERNAL_HANDLE)
			goto cleanup;
		swapchain_native_images_free(chain);
		if (chain->group != NULL)
			swapchain_group_free(chain->group);
		chain->group = NULL;
		if (surface->platform->lend_decline != NULL)
			surface->platform->lend_decline(chain->lease, chain->lend_serial);
		chain->lend_serial = 0U;
		chain->direct = VK_FALSE;
		memset(chain->states, WSI_IMAGE_AVAILABLE, bytes);
	}

	/* A chain with its own images on a direct display renders into what the output shows. */
	if (shared == NULL)
		chain->direct = swapchain_direct_possible(chain, info);

	/* Creates an image group only for the first independently owned swapchain. */
	if (shared == NULL) {
		error = swapchain_group_create(chain, info);

		/* A direct chain whose shared images cannot be made has images of its own instead (its presents copy). */
		if (chain->direct != VK_FALSE &&
		    (error == VK_ERROR_FEATURE_NOT_PRESENT ||
		     error == VK_ERROR_FORMAT_NOT_SUPPORTED)) {
			swapchain_group_free(chain->group);
			chain->group = NULL;
			chain->direct = VK_FALSE;
			error = swapchain_group_create(chain, info);
		}
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
	if (chain->direct != VK_FALSE)
		error = swapchain_native_images_import(chain);
	if (error != VK_SUCCESS)
		chain->direct = VK_FALSE;
	if (error != VK_SUCCESS && surface->platform->import_image != NULL)
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
	/* The lent images' fds were the creation's; their images and native buffers hold what they need. */
	for (index = 0U; index < lent_count; index++)
		close(lent_fds[index]);

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
	const VkBaseInStructure *extension;
	const VkSwapchainCounterCreateInfoEXT *counters;
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

	/* No surface offers a counter (VK_EXT_display_surface_counter), so none can be asked for. */
	for (extension = info->pNext; extension != NULL; extension = extension->pNext) {
		/* Other chained records are ignored as before. */
		if (extension->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT)
			continue;

		/* An empty counter set is the only one a surface supports. */
		counters = (const VkSwapchainCounterCreateInfoEXT *)extension;
		if (counters->surfaceCounters != 0U)
			return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Asks the surface for the limits the creation is checked against. */
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

	/* Refuses transforms absent from native capabilities. */
	if (info->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The alpha mode is exactly one of those the native backend advertises. */
	if ((info->compositeAlpha & capabilities.supportedCompositeAlpha) == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;
	if ((info->compositeAlpha & (info->compositeAlpha - 1U)) != 0U)
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

	/* Idle slots stop holding copies of this chain's images, which their recordings keep alive. */
	present_slots_forget(chain);

	/* A destroyed overlay chain's held change is never shown; its shown image is hidden as it is destroyed. */
	if (chain->surface != NULL &&
	    chain->surface->overlay != VK_FALSE &&
	    chain->lease != NULL &&
	    chain->surface->platform->overlay_forget != NULL)
		chain->surface->platform->overlay_forget(chain->lease, chain->serial);

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

	/* No image has a capability to close until its creation makes one. */
	for (index = 0U; index < group->count; index++)
		group->images[index].fd = -1;

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

		/* A direct image's capability the import never took. */
		if (group->images[index].fd >= 0)
			close(group->images[index].fd);
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
	struct gpu_placement placement;
	const struct gpu_placement *conditions;
	struct gpu_image_layouts scanout;
	uint32_t tiling;
	uint32_t parameter;
	VkResult error;

	/* Internal transfer use is added without restricting the application's declared use. */
	allocator = swapchain_allocator(&chain->group->allocator);
	image->fd = -1;

	/* A direct chain's image is the linear shared image the output shows. */
	if (chain->direct != VK_FALSE) {
		conditions = NULL;
		if (chain->surface->platform->placement != NULL) {
			error = chain->surface->platform->placement(chain->lease, &placement);
			if (error != VK_SUCCESS)
				return error;
			conditions = &placement;
		}

		/* The fastest layout both the renderer and the display take (linear rows unless one comes first). */
		tiling = GPU_IMAGE_LINEAR;
		parameter = 0U;
		if (chain->surface->platform->layouts != NULL &&
		    vulkan_wsi_layout_choose != NULL &&
		    vulkan_wsi_shared_image_create_layout != NULL) {
			memset(&scanout, 0, sizeof(scanout));
			scanout.format = GPU_PIXEL_RGBA8888;
			if (info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM || info->imageFormat == VK_FORMAT_B8G8R8A8_SRGB)
				scanout.format = GPU_PIXEL_BGRA8888;
			scanout.width = info->imageExtent.width;
			scanout.height = info->imageExtent.height;
			error = chain->surface->platform->layouts(chain->lease, &scanout);
			if (error == VK_SUCCESS)
				vulkan_wsi_layout_choose(chain->device, info->imageFormat, info->imageExtent, &scanout, &tiling, &parameter);
		}

		/* The image in that layout (rows of pixels as before when the library has no layouts). */
		if (tiling != GPU_IMAGE_LINEAR) {
			error = vulkan_wsi_shared_image_create_layout(
				chain->device,
				info->imageFormat,
				info->imageExtent,
				info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				tiling,
				parameter,
				allocator,
				conditions,
				&image->image,
				&image->memory,
				&image->fd,
				&image->descriptor);
		} else {
			error = vulkan_wsi_shared_image_create_usage(
				chain->device,
				info->imageFormat,
				info->imageExtent,
				info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				allocator,
				conditions,
				&image->image,
				&image->memory,
				&image->fd,
				&image->descriptor);
		}
		if (error != VK_SUCCESS)
			return error;

		/* The standard image handle remains valid but its lifetime belongs to WSI. */
		private_image = vulkan_image(image->image);
		private_image->swapchain_owned = VK_TRUE;

		/* Succeeded: the application renders straight into scanout memory. */
		return VK_SUCCESS;
	}

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

/*
 * Tells whether a chain may render into the images the output shows: a
 * direct display's primary plane, a renderer that shares allocations, and
 * a format that a linear image renders to, with no use a linear image
 * cannot have.
 */
static VkBool32
swapchain_direct_possible(
	struct vulkan_swapchain *chain,
	const VkSwapchainCreateInfoKHR *info)
{
	VkFormatProperties properties;
	VkImageUsageFlags allowed;

	/* The library must make shared images with a swapchain's uses. */
	if (vulkan_wsi_shared_image_create_usage == NULL)
		return VK_FALSE;

	/* Only a direct display's primary plane; Wayland, cursor and overlay chains copy. */
	if (chain->surface->display_mode == NULL)
		return VK_FALSE;
	if (chain->surface->cursor != VK_FALSE || chain->surface->overlay != VK_FALSE)
		return VK_FALSE;
	if (chain->surface->platform->import_image == NULL)
		return VK_FALSE;

	/* The renderer must share the allocation with the display node. */
	if ((chain->device->object.context->capabilities & GPU_CAP_SHARE) == 0U)
		return VK_FALSE;

	/* One layer, and uses a linear image has. */
	if (info->imageArrayLayers != 1U)
		return VK_FALSE;
	allowed = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if ((info->imageUsage & ~allowed) != 0U)
		return VK_FALSE;

	/* A format a linear image renders to and is sampled from. */
	vkGetPhysicalDeviceFormatProperties((VkPhysicalDevice)chain->device->physical, info->imageFormat, &properties);
	if ((properties.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0U)
		return VK_FALSE;
	if ((properties.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U)
		return VK_FALSE;

	/* Succeeded: the chain renders into scanout memory. */
	return VK_TRUE;
}

/*
 * Gives the output a direct chain's own images: each image's allocation is
 * imported into the display once, and the output shows it as it is.
 */
static VkResult
swapchain_native_images_import(
	struct vulkan_swapchain *chain)
{
	struct wsi_image *image;
	size_t bytes;
	uint32_t index;
	VkResult error;

	/* One native alias for each image. */
	bytes = (size_t)chain->group->count * sizeof(*chain->presentation);
	if (bytes / sizeof(*chain->presentation) != chain->group->count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	chain->presentation = vulkan_allocate(&chain->object.allocator, bytes, sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (chain->presentation == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(chain->presentation, 0, bytes);
	chain->presentation_count = chain->group->count;

	/* Imports each image's allocation; the capability is closed once taken. */
	for (index = 0U; index < chain->presentation_count; index++) {
		image = &chain->group->images[index];
		if (image->fd < 0)
			return VK_ERROR_FORMAT_NOT_SUPPORTED;
		error = chain->surface->platform->import_image(chain->lease, image->fd, &image->descriptor, &chain->presentation[index].native_image);
		close(image->fd);
		image->fd = -1;
		if (error != VK_SUCCESS)
			return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* Succeeded: the output shows the application's own images. */
	return VK_SUCCESS;
}

/*
 * Records whether the application gives the chain's images away: a
 * VkExternalMemoryImageCreateInfo in the creation's chain names the opaque
 * fd, which vkGetMemoryFdKHR then gives for an image of a direct chain (a
 * display's primary plane whose images the output shows).  Vulkan has no
 * export of swapchain images; this pairing of standard records is the
 * library's own.  Returns VK_SUCCESS, or VK_ERROR_INITIALIZATION_FAILED for
 * another handle type.
 */
static VkResult
swapchain_exportable(
	struct vulkan_swapchain *chain,
	const VkSwapchainCreateInfoKHR *info)
{
	const VkBaseInStructure *extension;
	const VkExternalMemoryImageCreateInfo *external;

	/* Each record of the creation's chain; only the external memory one counts. */
	for (extension = info->pNext; extension != NULL; extension = extension->pNext) {
		if (extension->sType != VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)
			continue;

		/* Only the opaque fd, which carries the image's description. */
		external = (const VkExternalMemoryImageCreateInfo *)extension;
		if ((external->handleTypes & ~(VkExternalMemoryHandleTypeFlags)VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) != 0U)
			return VK_ERROR_INITIALIZATION_FAILED;

		/* An empty set gives nothing away. */
		if (external->handleTypes != 0U)
			chain->exportable = VK_TRUE;
	}

	/* Succeeded: the chain knows whether its images may go out. */
	return VK_SUCCESS;
}

/*
 * Records a window-system chain's creation as a lend is checked against:
 * its format, extent, uses and the count the application asked for, and
 * whether this device draws the chain into images it imports -- one layer,
 * opaque, only uses an imported image has, and a format a linear image
 * renders to and is sampled from.
 */
static void
swapchain_fit(
	struct vulkan_swapchain *chain,
	const VkSwapchainCreateInfoKHR *info,
	uint32_t asked)
{
	VkFormatProperties properties;
	VkImageUsageFlags allowed;

	/* What the chain is. */
	memset(&chain->fit, 0, sizeof(chain->fit));
	chain->fit.format = info->imageFormat;
	chain->fit.extent = info->imageExtent;
	chain->fit.usage = info->imageUsage;
	chain->fit.count = asked;

	/* Only a window system's surface lends images, and the library must import them. */
	if (chain->surface->display_mode != NULL || vulkan_wsi_shared_image_import_format == NULL)
		return;
	if ((chain->device->object.context->capabilities & GPU_CAP_SHARE) == 0U)
		return;

	/* One layer, opaque, and the uses an imported image has. */
	if (info->imageArrayLayers != 1U || info->compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
		return;
	allowed = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if ((info->imageUsage & ~allowed) != 0U)
		return;

	/* A format a linear image renders to and is sampled from. */
	vkGetPhysicalDeviceFormatProperties((VkPhysicalDevice)chain->device->physical, info->imageFormat, &properties);
	if ((properties.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0U)
		return;
	if ((properties.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U)
		return;

	/* Succeeded: the chain can draw into lent images. */
	chain->fit.drawable = VK_TRUE;
}

/*
 * Makes a lent chain's images from the lend's fds (which stay the
 * caller's): each is imported as an image of the chain's format bound to
 * the lent memory, and its native image is the window system's buffer of
 * it, busy until the compositor gives it to the chain.
 */
static VkResult
swapchain_lent_create(
	struct vulkan_swapchain *chain,
	const VkSwapchainCreateInfoKHR *info,
	uint32_t count,
	const int *fds)
{
	struct wsi_image_group *group;
	struct vulkan_image *private_image;
	const VkAllocationCallbacks *allocator;
	uint32_t index;
	size_t bytes;
	VkResult error;

	/* The group of the lend's count of images, owned by the chain for unwind. */
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

	/* The images' records; none holds an fd. */
	bytes = (size_t)count * sizeof(*group->images);
	group->images = vulkan_allocate(&group->allocator, bytes, sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (group->images == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(group->images, 0, bytes);
	group->count = count;
	for (index = 0U; index < count; index++)
		group->images[index].fd = -1;

	/* Each lent image imported as the chain's format, its handle the application's and its lifetime the library's. */
	allocator = swapchain_allocator(&group->allocator);
	for (index = 0U; index < count; index++) {
		error = vulkan_wsi_shared_image_import_format(
			chain->device,
			fds[index],
			info->imageFormat,
			allocator,
			&group->images[index].image,
			&group->images[index].memory,
			&group->images[index].descriptor);
		if (error != VK_SUCCESS)
			return error;
		private_image = vulkan_image(group->images[index].image);
		private_image->swapchain_owned = VK_TRUE;
	}

	/* One native image (the window system's buffer) for each lent image. */
	bytes = (size_t)count * sizeof(*chain->presentation);
	chain->presentation = vulkan_allocate(&chain->object.allocator, bytes, sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (chain->presentation == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(chain->presentation, 0, bytes);
	chain->presentation_count = count;

	/* Each buffer named as the lend's image (with the kernel's description of it), busy until the compositor gives it. */
	for (index = 0U; index < count; index++) {
		error = chain->surface->platform->lend_import(
			chain->lease,
			fds[index],
			&group->images[index].descriptor,
			chain->lend_serial,
			index,
			&chain->presentation[index].native_image);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: the application draws into the compositor's images. */
	return VK_SUCCESS;
}

/*
 * Tells whether a chain is better made again by the application: its
 * window system offers a lend that fits a chain of its own images, or the
 * lend a lent chain draws into ended.
 */
static VkBool32
swapchain_lend_changed(
	struct vulkan_swapchain *chain)
{
	VkBool32 changed;

	/* Only a window system that lends images. */
	if (chain->lease == NULL || chain->surface->platform->lend_changed == NULL)
		return VK_FALSE;

	/* The window system's answer for the chain's creation and lend. */
	changed = chain->surface->platform->lend_changed(chain->lease, &chain->fit, chain->lend_serial);
	if (changed == VK_FALSE)
		return VK_FALSE;

	/* Succeeded: the chain is suboptimal. */
	return VK_TRUE;
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

/*
 * Says whether a chain gets one image more than it asked for: a mailbox
 * chain on a compositor's surface (no display mode, not a cursor or an
 * overlay) whose images the compositor can show itself, within the
 * surface's image quota.  The compositor holds the image on the screen and
 * the one it flips to while the application draws; with the asked count
 * alone every acquire waits for the release of the image the last present
 * replaced, and the spare keeps one free.  vkGetSwapchainImagesKHR reports
 * the count made, which Vulkan allows to exceed minImageCount.
 */
static VkBool32
swapchain_spare_wanted(
	struct VkDevice_T *device,
	struct vulkan_surface *surface,
	const VkSwapchainCreateInfoKHR *info)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkResult error;

	/* A mailbox chain of a compositor's surface whose images it imports. */
	if (info->presentMode != VK_PRESENT_MODE_MAILBOX_KHR)
		return VK_FALSE;
	if (surface->display_mode != NULL || surface->cursor != VK_FALSE || surface->overlay != VK_FALSE)
		return VK_FALSE;
	if (surface->platform->import_image == NULL)
		return VK_FALSE;

	/* Within the surface's quota. */
	if (info->minImageCount == UINT32_MAX)
		return VK_FALSE;
	error = surface->platform->capabilities(surface, device->physical, &capabilities);
	if (error != VK_SUCCESS)
		return VK_FALSE;
	if (capabilities.maxImageCount != 0U && info->minImageCount + 1U > capabilities.maxImageCount)
		return VK_FALSE;

	/* Succeeded: the chain gets its spare. */
	return VK_TRUE;
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

	/* An overlay chain whose held change the output refused is out of date (ws214-p004). */
	if (chain->surface->overlay != VK_FALSE &&
	    chain->lease != NULL &&
	    chain->surface->platform->overlay_status != NULL) {
		error = chain->surface->platform->overlay_status(chain->lease, chain->serial);
		if (error != VK_SUCCESS)
			return error;
	}

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

	/* Every target's cursor ticket, zero for a target that is not a cursor plane. */
	bytes = (size_t)job->count * sizeof(*job->tickets);
	if (bytes / sizeof(*job->tickets) != job->count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	job->tickets = vulkan_allocate(
		&job->allocator,
		bytes,
		sizeof(void *),
		VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (job->tickets == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(job->tickets, 0, bytes);

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

		/* A cursor plane's image is shown whole, unscaled, wherever its destination puts it (the kernel clips). */
		if (job->has_display != VK_FALSE && chain->surface->cursor != VK_FALSE) {
			if (job->display.srcRect.offset.x != 0 ||
			    job->display.srcRect.offset.y != 0 ||
			    job->display.srcRect.extent.width != chain->extent.width ||
			    job->display.srcRect.extent.height != chain->extent.height ||
			    job->display.dstRect.extent.width != chain->extent.width ||
			    job->display.dstRect.extent.height != chain->extent.height ||
			    job->display.persistent != VK_FALSE)
				return VK_ERROR_INITIALIZATION_FAILED;
		}

		/* An overlay shows a part of its image anywhere inside the output, or hides (ws214-p004). */
		if (job->has_display != VK_FALSE && chain->surface->overlay != VK_FALSE) {
			error = present_overlay_rects(job, chain);
			if (error != VK_SUCCESS)
				return error;
		}

		/* Invalid rectangles cannot escape their checked source or destination image. */
		if (job->has_display != VK_FALSE &&
		    chain->surface->cursor == VK_FALSE &&
		    chain->surface->overlay == VK_FALSE) {
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

	/* The changed parts the application named, if it enabled the extension. */
	error = present_regions(job, info);
	if (error != VK_SUCCESS)
		return error;

	/* What each overlay target asks of its plane. */
	error = present_overlay_prepare(job);
	if (error != VK_SUCCESS)
		return error;

	/* A present of one cursor or overlay plane goes to that plane kind's own worker. */
	present_lane(job);

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
	struct vulkan_image *image;
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

		/*
		 * A cursor plane's present takes the next ticket in reservation
		 * order: a present with a larger ticket replaces it on the output,
		 * whichever reaches the output first.
		 */
		if (chain->surface->cursor != VK_FALSE)
			job->tickets[index] = __atomic_add_fetch(&chain->surface->cursor_ticket, 1U, __ATOMIC_ACQ_REL);

		/*
		 * A primary present of an output with overlay planes takes the
		 * next primary ticket, in reservation order too: it shows every
		 * overlay change held before it was called (ws214-p004).
		 */
		if (chain->surface->cursor == VK_FALSE &&
		    chain->surface->overlay == VK_FALSE &&
		    chain->surface->platform->primary_ticket != NULL)
			job->tickets[index] = chain->surface->platform->primary_ticket(chain->lease);
	}

	/*
	 * A cursor-lane or overlay-lane job notes what its copy will show.
	 * Every legal write of the reserved image was accepted before this
	 * present was called, and none can follow while the image is presenting.
	 */
	if (job->lane == WSI_LANE_CURSOR || job->lane == WSI_LANE_OVERLAY) {
		chain = job->chains[0];
		image = vulkan_image(chain->group->images[info->pImageIndices[0]].image);
		job->content_serial = __atomic_load_n(&image->content_serial, __ATOMIC_ACQUIRE);
		job->content_epoch = __atomic_load_n(&chain->device->content_epoch, __ATOMIC_ACQUIRE);
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

	/* What the copy command buffer is to hold, which picks a slot that already holds it. */
	present_recording_key(job, info);

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

	/*
	 * A recording interrupted part way holds nothing a later job may
	 * submit again, so the slot keeps no recording until End succeeds.
	 */
	if (job->reused == VK_FALSE)
		memset(&job->completion->recording, 0, sizeof(job->completion->recording));

	/*
	 * The whole command buffer is ready before the one submission consumes
	 * waits.  It is not a one-time recording: the slot submits it again for
	 * a later present of the same image (ws211-p011), each time after the
	 * previous submission's fence completed.
	 */
	if (job->reused == VK_FALSE) {
		memset(&begin, 0, sizeof(begin));
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = 0U;
		error = vkBeginCommandBuffer(job->command, &begin);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Every admitted target records its chosen presentation route within this single submission. */
	for (index = 0U; index < job->count; index++) {
		/* Skips readback commands for targets whose surface was already lost or replaced. */
		if (job->results[index] != VK_SUCCESS)
			continue;

		/* A cursor image unchanged since its last copy needs only the present's waits. */
		if (job->route == WSI_ROUTE_EMPTY)
			continue;

		/* A direct chain's image is what the output shows: only the present's waits. */
		if (job->chains[index]->direct != VK_FALSE)
			continue;

		/* Optional display composition remains GPU-resident on both direct and shared-image paths; a cursor or overlay image is copied whole. */
		display = NULL;
		if (job->has_display != VK_FALSE &&
		    job->chains[index]->surface->cursor == VK_FALSE &&
		    job->chains[index]->surface->overlay == VK_FALSE)
			display = &job->display;

		/* Copy and composition preserve the application's optimal image contents; a kept recording already holds them. */
		if (job->reused == VK_FALSE)
			present_copy(job->command, job->chains[index], info->pImageIndices[index], job->queue->family, display);

		/* A cursor-lane or overlay-lane copy's completion records what the native image now holds, recorded now or kept. */
		if (job->lane == WSI_LANE_CURSOR || job->lane == WSI_LANE_OVERLAY)
			job->copy_recorded = VK_TRUE;
	}

	/* A kept recording is complete as it is. */
	if (job->reused != VK_FALSE)
		return VK_SUCCESS;

	/* Final recording failure occurs before any semaphore-consuming submit is attempted. */
	error = vkEndCommandBuffer(job->command);
	if (error != VK_SUCCESS)
		return error;

	/* The slot now holds this recording, for a later present of the same image to submit again. */
	memcpy(&job->completion->recording, &job->recording, sizeof(job->recording));

	/* Succeeded: all fallible command storage exists before any queue operation. */
	return VK_SUCCESS;
}

/*
 * Works out what a job's copy command buffer records (ws211-p011), so that
 * a slot already holding it is submitted again without recording it.
 *
 * Only a job of one target that is still to be presented has such a key;
 * any other job records afresh and leaves its slot no recording.
 */
static void
present_recording_key(
	struct wsi_present_job *job,
	const VkPresentInfoKHR *info)
{
	struct wsi_present_recording *key;
	struct vulkan_swapchain *chain;
	uint32_t index;

	/* No recording unless proven otherwise; the key is compared byte for byte. */
	key = &job->recording;
	memset(key, 0, sizeof(*key));

	/* Several targets, or a target that already failed, record afresh. */
	if (job->count != 1U)
		return;
	if (job->results[0] != VK_SUCCESS)
		return;

	/* A cursor image unchanged since its last copy records nothing, whichever chain it is. */
	key->route = job->route;
	if (job->route == WSI_ROUTE_EMPTY) {
		key->valid = VK_TRUE;
		return;
	}

	/* The chain and the image the copy reads and writes. */
	chain = job->chains[0];
	index = info->pImageIndices[0];
	key->chain = chain->serial;
	key->image = index;

	/* The native image's first copy takes it from nothing, later ones from its outside reader. */
	if (chain->gpu_present != VK_FALSE)
		key->presented = chain->presentation[index].shared_presented;

	/* A display rectangle composes into the native image; a cursor or overlay image is copied whole. */
	if (job->has_display != VK_FALSE &&
	    chain->surface->cursor == VK_FALSE &&
	    chain->surface->overlay == VK_FALSE) {
		key->composed = VK_TRUE;
		key->source = job->display.srcRect;
		key->destination = job->display.dstRect;
	}

	/* Succeeded: the job's recording is named. */
	key->valid = VK_TRUE;
}

/* Reports whether a slot's kept recording is the one a job wants. */
static VkBool32
present_recording_same(
	const struct wsi_present_recording *kept,
	const struct wsi_present_recording *wanted)
{
	int difference;

	/* A job with no key never takes a kept recording. */
	if (wanted->valid == VK_FALSE)
		return VK_FALSE;

	/* Both keys are zeroed before they are filled, so their bytes compare. */
	difference = memcmp(kept, wanted, sizeof(*kept));
	if (difference != 0)
		return VK_FALSE;

	/* Succeeded: the kept recording is the wanted one. */
	return VK_TRUE;
}

/* Reports whether a slot's kept recording copies the same image of the same chain as a job's, in another way. */
static VkBool32
present_recording_image(
	const struct wsi_present_recording *kept,
	const struct wsi_present_recording *wanted)
{
	/* Either side without a recording, or a cursor present that copies nothing, names no image. */
	if (kept->valid == VK_FALSE)
		return VK_FALSE;
	if (wanted->valid == VK_FALSE)
		return VK_FALSE;
	if (wanted->route == WSI_ROUTE_EMPTY)
		return VK_FALSE;

	/* Another chain, or another image of it. */
	if (kept->chain != wanted->chain)
		return VK_FALSE;
	if (kept->image != wanted->image)
		return VK_FALSE;

	/* Succeeded: the slot holds a copy of the job's image, which the job's recording replaces. */
	return VK_TRUE;
}

/*
 * Drops every idle slot's recording of a retiring chain (ws211-p011).
 *
 * A recording keeps the images it names alive in the renderer until the
 * command buffer is recorded again or reset, so the chain's slots are reset
 * now rather than when a later present happens to take them.  The chain has
 * no job left, so none of its slots is busy for it.
 */
static void
present_slots_forget(
	struct vulkan_swapchain *chain)
{
	struct wsi_present_fence *completion;
	VkCommandBuffer command;

	/* Resets the chain's slots one at a time, each borrowed while its reset runs. */
	for (;;) {
		/* Finds an idle slot still holding a copy of this chain and borrows it. */
		pthread_mutex_lock(&swapchain_mutex);

		completion = present_slot_of_chain(chain);
		if (completion != NULL) {
			completion->busy = VK_TRUE;
			memset(&completion->recording, 0, sizeof(completion->recording));
		}

		pthread_mutex_unlock(&swapchain_mutex);

		/* No slot of the chain is left. */
		if (completion == NULL)
			break;

		/* Releases what the recording held; a failure leaves it for the slot's next Begin, which resets it too. */
		command = completion->command;
		(void)vkResetCommandBuffer(command, 0U);

		/* The slot is free again, holding no recording. */
		pthread_mutex_lock(&swapchain_mutex);

		completion->busy = VK_FALSE;

		pthread_mutex_unlock(&swapchain_mutex);
	}

	/* Succeeded: no idle slot names the chain. */
	return;
}

/* Finds an idle slot of the chain's device whose recording copies the chain's image; the caller holds swapchain_mutex. */
static struct wsi_present_fence *
present_slot_of_chain(
	const struct vulkan_swapchain *chain)
{
	struct wsi_queue_worker *worker;
	struct wsi_present_fence *completion;

	/* Every worker of the chain's device, and every slot of each. */
	for (worker = workers; worker != NULL; worker = worker->next) {
		/* Another device's workers never record this chain. */
		if (worker->queue->device != chain->device)
			continue;

		/* A busy slot belongs to another chain's job; an idle one may still hold this chain's copy. */
		for (completion = worker->fences; completion != NULL; completion = completion->next) {
			/* Skips a slot another job borrows. */
			if (completion->busy != VK_FALSE)
				continue;

			/* Skips a slot without a recording, and one recording another chain or none at all. */
			if (completion->recording.valid == VK_FALSE)
				continue;
			if (completion->recording.chain != chain->serial)
				continue;

			/* Succeeded: this slot holds a copy of the chain. */
			return completion;
		}
	}

	/* No idle slot names the chain. */
	return NULL;
}

/* Borrows one queue-owned fence slot and prepares its exact next generation before submission. */
static VkResult
present_fence_prepare(
	struct wsi_present_job *job)
{
	struct wsi_present_fence *completion;
	struct wsi_present_fence *match;
	struct wsi_present_fence *older;
	struct wsi_present_fence *empty;
	struct wsi_present_fence *idle;
	VkFenceCreateInfo fence;
	VkExportFenceCreateInfo export;
	VkFenceGetFdInfoKHR get_fd;
	struct gpu_fence_state state;
	const VkAllocationCallbacks *allocator;
	VkBool32 shared_fence;
	VkBool32 fresh;
	VkBool32 same;
	VkBool32 image;
	VkResult error;
	uint32_t index;
	uint32_t slots;
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

	/*
	 * A fence sent to a Wayland compositor stands for this present alone
	 * (ws103-p005): the compositor keeps its fd and decides readiness by
	 * polling it, which a later reset of a reused fence would mislead.  So a
	 * job with a Wayland target does not reuse a slot's fence: it takes a slot
	 * whose fences are sent, gives up the previous present's fence there
	 * (signaled, as that job retired) and the fd sent with it, the
	 * compositor's copy of the fd keeping the kernel fence alive, and makes a
	 * new fence below.  A job for the display alone keeps reusing the fence
	 * of a slot whose fences are never sent, its generation never leaving
	 * this library.
	 */
	fresh = VK_FALSE;
	if (shared_fence != VK_FALSE) {
		/* Looks for a Wayland target among the job's swapchains. */
		for (index = 0U; index < job->count; index++) {
			/* A Wayland target is the one whose compositor takes the fence with an early commit. */
			if (job->chains[index]->surface->platform->commit_early != NULL)
				fresh = VK_TRUE;
		}
	}

	/* Only idle slots of the right payload type can be borrowed by a new job. */
	pthread_mutex_lock(&swapchain_mutex);

	/*
	 * Cache reuse is exclusive until the previous job has retired every
	 * native request.  An idle slot holding the job's recording is taken
	 * first (ws211-p011), then one holding an older recording of the same
	 * image (its first copy, before the native image was ever presented),
	 * then one holding none, so that the slots settle one to an image and
	 * keep their recordings.
	 */
	match = NULL;
	older = NULL;
	empty = NULL;
	idle = NULL;
	slots = 0U;
	for (completion = job->worker->fences; completion != NULL; completion = completion->next) {
		/* Payload type, and whether its fences are sent, cannot change while a cached native fence and fd remain alive. */
		if (completion->shared != shared_fence)
			continue;
		if (completion->sent != fresh)
			continue;
		slots++;

		/* A slot an earlier job still borrows is not free. */
		if (completion->busy != VK_FALSE)
			continue;

		/* The job's own recording, submitted again as it is. */
		same = present_recording_same(&completion->recording, &job->recording);
		if (same != VK_FALSE) {
			match = completion;
			break;
		}

		/* The first idle slot holding another recording of the job's image. */
		image = present_recording_image(&completion->recording, &job->recording);
		if (older == NULL && image != VK_FALSE)
			older = completion;

		/* The first idle slot without a recording, and the first idle slot at all. */
		if (empty == NULL && completion->recording.valid == VK_FALSE)
			empty = completion;
		if (idle == NULL)
			idle = completion;
	}

	/*
	 * Without its recording, a job takes a slot holding none; a job with a
	 * key keeps the other recordings while the worker has room for another
	 * slot, and otherwise records over an idle slot's.
	 */
	completion = match;
	if (completion == NULL)
		completion = older;
	if (completion == NULL)
		completion = empty;
	if (completion == NULL && job->recording.valid == VK_FALSE)
		completion = idle;
	if (completion == NULL && slots >= WSI_PRESENT_SLOTS)
		completion = idle;

	/* Busy transfers this slot to exactly one preparing or accepted job. */
	if (completion != NULL)
		completion->busy = VK_TRUE;

	pthread_mutex_unlock(&swapchain_mutex);

	/* A slot that already holds the job's recording is submitted without recording it. */
	job->reused = VK_FALSE;
	if (completion != NULL && completion == match)
		job->reused = VK_TRUE;

	/* New concurrency grows the cache once; allocation callbacks execute outside WSI serialization. */
	if (completion == NULL) {
		completion = vulkan_allocate(&job->allocator, sizeof(*completion), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
		if (completion == NULL)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Failed partial initialization remains queue-owned and can be retried or finally destroyed. */
		memset(completion, 0, sizeof(*completion));
		completion->fd = -1;
		completion->shared = shared_fence;
		completion->sent = fresh;
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

	/* A slot the worker already reset needs neither the reset nor the generation's query. */
	if (completion->rearmed != VK_FALSE) {
		completion->rearmed = VK_FALSE;
		job->fence = completion->fence;
		job->fence_fd = completion->fd;
		job->fence_generation = completion->generation;
		return VK_SUCCESS;
	}

	/* Gives up the previous present's fence of a reused slot whose fences are sent. */
	if (fresh != VK_FALSE && completion->fence != VK_NULL_HANDLE) {
		vkDestroyFence((VkDevice)job->queue->device, completion->fence, allocator);
		completion->fence = VK_NULL_HANDLE;
	}

	/* Gives up this library's fd of it; the compositor holds its own. */
	if (fresh != VK_FALSE && completion->fd >= 0) {
		close(completion->fd);
		completion->fd = -1;
	}

	/* First use (and every Wayland present) constructs native ownership; later display uses advance the completed payload generation. */
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
	if (error != VK_SUCCESS) {
		/* A refused submission's slot keeps no recording for a later present to trust. */
		memset(&job->completion->recording, 0, sizeof(job->completion->recording));
		return error;
	}

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

/*
 * Resets a retired display job's fence for its slot's next job, and notes
 * the new generation, so that the next present takes the slot without the
 * reset's round trips (the reset and the query of present_fence_prepare).
 *
 * Only an ordinary-lane job whose fence is not sent anywhere, was submitted,
 * completed and was presented: the native present waited for the fence in
 * the kernel and returned, so nothing reads the old generation any more.
 * A failure leaves the slot as before, for present_fence_prepare to reset.
 */
static void
present_fence_rearm(
	struct wsi_present_job *job)
{
	struct wsi_present_fence *completion;
	struct gpu_fence_state state;
	VkResult error;
	int status;

	/* Only a submitted ordinary-lane job, whose GPU work completed, of a display slot. */
	completion = job->completion;
	if (completion == NULL || job->submitted == VK_FALSE || job->native_only != VK_FALSE)
		return;
	if (job->lane != WSI_LANE_ORDINARY)
		return;
	if (completion->shared == VK_FALSE || completion->sent != VK_FALSE)
		return;
	if (completion->fence == VK_NULL_HANDLE || completion->fd < 0)
		return;

	/* The fence, unsignaled for the next job. */
	error = vkResetFences((VkDevice)job->queue->device, 1U, &completion->fence);
	if (error != VK_SUCCESS)
		return;

	/* Its new payload generation, read from the kernel as present_fence_prepare reads it. */
	memset(&state, 0, sizeof(state));
	state.version = GPU_ABI_VERSION;
	state.size = sizeof(state);
	state.fd = completion->fd;
	vulkan_context_lock(job->queue->device->object.context);

	status = ioctl(job->queue->device->object.context->fd, GPU_FENCE_QUERY, &state);

	vulkan_context_unlock(job->queue->device->object.context);

	/* A missing payload leaves the reset to the next job's preparation. */
	if (status != 0 || state.generation == 0U)
		return;

	/* Succeeded: the next job takes the reset fence and its generation. */
	completion->generation = state.generation;
	completion->rearmed = VK_TRUE;
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
	VkBool32 primary;

	/* Each native backend contributes to the same post-submission result handling. */
	combined = VK_SUCCESS;
	for (index = 0U; index < job->count; index++) {
		/* Targets rejected before submission never reach their native presentation adapter. */
		chain = job->chains[index];
		error = job->results[index];
		primary = VK_FALSE;
		if (error == VK_SUCCESS) {
			/*
			 * A cursor plane shows the image where the present puts it,
			 * unless a later present was shown; its frame is counted under
			 * the cursor's own lock.
			 */
			if (chain->gpu_present != VK_FALSE &&
			    chain->surface->cursor != VK_FALSE &&
			    chain->surface->platform->present_cursor != NULL) {
				error = present_cursor_show(job, index, VK_TRUE);
			} else if (job->overlays != NULL && chain->surface->overlay != VK_FALSE) {
				/* An overlay plane's present was decided at enqueue: a held one waits for its primary. */
				error = present_overlay_commit(job, index, VK_TRUE);
			} else if (chain->frame == UINT64_MAX) {
				/* A frame identifier belongs to its chain and never substitutes for a fence. */
				error = swapchain_device_lost(chain->device);
			} else if (chain->gpu_present != VK_FALSE) {
				/* Names this GPU image commit without creating a CPU pixel staging view. */
				chain->frame++;

				/* The part that changed, when the application named it and the backend damages parts. */
				if (job->damage != NULL && job->damage[index].known && chain->surface->platform->damage != NULL)
					chain->surface->platform->damage(chain->lease, job->damage[index].x, job->damage[index].y, job->damage[index].width, job->damage[index].height);

				if (job->tickets[index] != 0U && chain->surface->platform->present_primary != NULL) {
					/* A primary with a ticket shows the overlay changes held for it in the same commit (ws214-p004). */
					primary = VK_TRUE;
					error = chain->surface->platform->present_primary(
						chain->lease,
						chain->presentation[info->pImageIndices[index]].native_image,
						chain->present_mode,
						&chain->sequence,
						job->fence_fd,
						job->fence_generation,
						job->tickets[index]);
				} else if (job->fence_fd >= 0 && chain->surface->platform->present_image_sync != NULL) {
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

		/* A present that failed takes back the part it named, so the next one does not commit it. */
		if (error != VK_SUCCESS && job->damage != NULL && job->damage[index].known && chain->surface->platform->damage != NULL)
			chain->surface->platform->damage(chain->lease, 0, 0, 0, 0);

		/* A primary ticket that did not reach present_primary lets its held overlay changes go their way. */
		if (primary == VK_FALSE)
			present_primary_skip(job, index, error);

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

			/*
			 * A completed cursor copy put the image's pixels of the noted
			 * serial and epoch into the native image, so a later present
			 * of the unchanged image may show them without copying.
			 */
			if (job->copy_done != VK_FALSE && index == 0U) {
				chain->presentation[image].copied_serial = job->content_serial;
				chain->presentation[image].copied_epoch = job->content_epoch;
				chain->presentation[image].copied_valid = VK_TRUE;
			}
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
	vulkan_free(&job->allocator, job->overlays);
	vulkan_free(&job->allocator, job->damage);
	vulkan_free(&job->allocator, job->stages);
	vulkan_free(&job->allocator, job->results);
	vulkan_free(&job->allocator, job->tickets);
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
	enum wsi_lane lane,
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
	candidate->lane = lane;
	pthread_mutex_lock(&swapchain_mutex);

	/* Concurrent work on other queues or lanes cannot replace this ordering domain. */
	for (worker = workers; worker != NULL; worker = worker->next) {
		/* Queue identity remains stable until the device has joined every worker. */
		if (worker->queue == queue && worker->lane == lane)
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
	VkBool32 presented;
	const char *path;

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
		 * fence; the wait below then only retires the job.  A cursor present
		 * whose waits were consumed without GPU work has neither.
		 */
		early = VK_FALSE;
		presented = VK_FALSE;
		error = VK_SUCCESS;
		if (job->native_only == VK_FALSE)
			early = present_early(job);
		if (early) {
			error = present_native(job, &job->info);
			presented = VK_TRUE;
		}

		/* Neither transport acceptance nor a context-zero display fence proves producer completion. */
		if (job->native_only == VK_FALSE) {
			waited = vulkan_fences_wait(job->queue->device, 1U, &job->fence, VK_TRUE, UINT64_MAX);

			/* The time the GPU work completed, for the cursor log. */
			if (job->logged != VK_FALSE)
				job->log.fence = vulkan_wsi_cursor_clock();

			/* A failed wait loses the device; a completed copy is what the native image now holds. */
			if (waited != VK_SUCCESS) {
				error = swapchain_device_lost(job->queue->device);
			} else if (job->copy_recorded != VK_FALSE) {
				job->copy_done = VK_TRUE;
			}

			/* Held overlay changes may now be shown by their primary present, or are dropped. */
			if (job->overlays != NULL)
				present_overlay_settle(job, error);
		}

		/* The unchanged Wayland v1 contract receives commits only after GPU writes and ownership release finish. */
		if (error == VK_SUCCESS && !early) {
			error = present_native(job, &job->info);
			presented = VK_TRUE;
		}

		/* A primary ticket of a job that never reached the output drops the changes held for it. */
		if (presented == VK_FALSE) {
			for (index = 0U; index < job->count; index++)
				present_primary_skip(job, index, error);
		}

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

		/* A logged cursor present reports its route and times before the job retires. */
		if (job->logged != VK_FALSE) {
			path = "worker";
			if (job->route == WSI_ROUTE_EMPTY)
				path = "empty";
			if (job->native_only != VK_FALSE)
				path = "busy";
			present_cursor_log(job, path);
		}

		/* A display slot's fence is reset for its next job now, off the presenting thread. */
		if (error == VK_SUCCESS && presented != VK_FALSE)
			present_fence_rearm(job);

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

	/* Overlay changes no primary present will take are shown alone before idle returns (ws214-p004). */
	vulkan_wsi_display_overlay_flush(device);

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
			/*
			 * Compositor retention is independent of the producer's image-state
			 * entry.  A cursor plane's output copies the image at once and
			 * holds none, so its acquisition never waits for the display lock.
			 */
			available = VK_TRUE;
			if (chain->gpu_present != VK_FALSE &&
			    chain->surface->cursor == VK_FALSE &&
			    chain->surface->platform->image_available != NULL)
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

		/*
		 * An overlay image the output still reads comes free at an
		 * adoption, which is waited for without any lock (ws214-p004).
		 */
		if (chain->surface->overlay != VK_FALSE && chain->surface->platform->reclaim != NULL) {
			pthread_mutex_unlock(&swapchain_mutex);

			/* A freed image is looked for again; nothing to wait for leaves the ordinary sleep. */
			error = swapchain_reclaim(chain, timeout, started);
			if (error == VK_SUCCESS)
				continue;
			if (error != VK_NOT_READY)
				return error;

			pthread_mutex_lock(&swapchain_mutex);
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

		/*
		 * A finite caller deadline takes priority over the protocol progress
		 * interval.  A direct display wakes on its own events alone; a window
		 * system's connection still looks every interval, for a release
		 * another thread read off the connection into the surface's queue.
		 */
		delay = WSI_PROGRESS_INTERVAL;
		if (wake != NULL && chain->surface->platform->wait_descriptor != NULL)
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

		/* A direct display or a window system's connection sleeps on fd readiness (the display with no periodic quantum). */
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

/* Waits on native errors, topology, a display's events or a window system's connection, and independent image-ownership publication. */
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
	if (chain->surface->platform->wait_descriptor != NULL) {
		descriptors[1].fd = chain->surface->platform->wait_descriptor(chain->lease);
		descriptors[1].events = POLLPRI;
	} else {
		descriptors[1].fd = chain->surface->platform->wait_input(chain->lease);
		descriptors[1].events = POLLIN;
	}
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

/*
 * Takes the changed parts of the present's targets from the application's
 * VkPresentRegionsKHR (VK_KHR_incremental_present, BUG-221): for each
 * target the box around its rectangles of layer 0, clipped to the image.
 * A target with no rectangles, or a present without the record (or on a
 * device that did not enable the extension), names none: the whole image
 * changed.  Returns VK_SUCCESS, or VK_ERROR_OUT_OF_HOST_MEMORY.
 */
static VkResult
present_regions(
	struct wsi_present_job *job,
	const VkPresentInfoKHR *info)
{
	const VkBaseInStructure *extension;
	const VkPresentRegionsKHR *regions;
	const VkPresentRegionKHR *region;
	const VkRectLayerKHR *rect;
	struct wsi_present_damage *damage;
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;
	size_t bytes;
	uint32_t index;
	uint32_t at;

	/* Only a device that enabled the extension names regions. */
	if ((job->queue->device->enabled_extensions & VULKAN_DEVICE_INCREMENTAL_PRESENT) == 0U)
		return VK_SUCCESS;

	/* The record in the application's chain, if there is one. */
	regions = NULL;
	for (extension = info->pNext; extension != NULL; extension = extension->pNext) {
		if (extension->sType == VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR) {
			regions = (const VkPresentRegionsKHR *)(const void *)extension;
			break;
		}
	}

	/* None, or one for other targets: every image changed whole. */
	if (regions == NULL || regions->pRegions == NULL || regions->swapchainCount != job->count)
		return VK_SUCCESS;

	/* One part a target. */
	bytes = (size_t)job->count * sizeof(*job->damage);
	job->damage = vulkan_allocate(&job->allocator, bytes, sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (job->damage == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(job->damage, 0, bytes);

	/* Each target's rectangles of layer 0, their box clipped to the image. */
	for (index = 0U; index < job->count; index++) {
		/* No rectangles: the whole image. */
		region = &regions->pRegions[index];
		if (region->rectangleCount == 0U || region->pRectangles == NULL || job->chains[index] == NULL)
			continue;

		/* The box around them. */
		left = INT64_MAX;
		top = INT64_MAX;
		right = INT64_MIN;
		bottom = INT64_MIN;
		for (at = 0U; at < region->rectangleCount; at++) {
			/* Another layer, or nothing. */
			rect = &region->pRectangles[at];
			if (rect->layer != 0U || rect->extent.width == 0U || rect->extent.height == 0U)
				continue;

			/* The box grows to hold it. */
			if (rect->offset.x < left)
				left = rect->offset.x;
			if (rect->offset.y < top)
				top = rect->offset.y;
			if ((int64_t)rect->offset.x + rect->extent.width > right)
				right = (int64_t)rect->offset.x + rect->extent.width;
			if ((int64_t)rect->offset.y + rect->extent.height > bottom)
				bottom = (int64_t)rect->offset.y + rect->extent.height;
		}

		/* Clipped to the image; an empty box names nothing. */
		if (left < 0)
			left = 0;
		if (top < 0)
			top = 0;
		if (right > (int64_t)job->chains[index]->extent.width)
			right = (int64_t)job->chains[index]->extent.width;
		if (bottom > (int64_t)job->chains[index]->extent.height)
			bottom = (int64_t)job->chains[index]->extent.height;
		if (right <= left || bottom <= top)
			continue;

		/* The target's part. */
		damage = &job->damage[index];
		damage->known = VK_TRUE;
		damage->x = (int32_t)left;
		damage->y = (int32_t)top;
		damage->width = (int32_t)(right - left);
		damage->height = (int32_t)(bottom - top);
	}

	/* Succeeded: the parts are the job's. */
	return VK_SUCCESS;
}

/* Chooses the worker of a present: one cursor or overlay plane's alone goes to that plane kind's lane. */
static void
present_lane(
	struct wsi_present_job *job)
{
	struct vulkan_swapchain *chain;

	/* A present of several targets keeps the ordinary order among them. */
	job->lane = WSI_LANE_ORDINARY;
	if (job->count != 1U)
		return;

	/* Only GPU images of a plane with its own present take another lane. */
	chain = job->chains[0];
	if (chain->gpu_present == VK_FALSE)
		return;

	/* A cursor plane's images, shown by the cursor present. */
	if (chain->surface->cursor != VK_FALSE && chain->surface->platform->present_cursor != NULL) {
		job->lane = WSI_LANE_CURSOR;
		return;
	}

	/* An overlay plane's images, shown by the overlay commit (ws214-p004). */
	if (chain->surface->overlay != VK_FALSE && chain->surface->platform->overlay_commit != NULL) {
		job->lane = WSI_LANE_OVERLAY;
		return;
	}

	/* Succeeded: any other present keeps the ordinary lane. */
	return;
}

/*
 * Chooses how a cursor-lane present reaches the output.
 *
 * The image must be unchanged since the copy its native image holds; then
 * complete waits are consumed here and the present is shown at once, and
 * waits still on the GPU need a submission without a copy.  Anything else
 * copies as before.
 */
static enum wsi_route
present_route(
	struct wsi_present_job *job,
	const VkPresentInfoKHR *info)
{
	struct vulkan_swapchain *chain;
	struct wsi_present_image *presentation;
	struct vulkan_image *image;
	const VkBaseInStructure *extension;
	uint64_t serial;
	uint64_t epoch;
	VkBool32 unchanged;
	VkBool32 consumed;

	/* A rejected target records nothing and shows nothing. */
	chain = job->chains[0];
	job->log.why = "result";
	if (job->results[0] != VK_SUCCESS)
		return WSI_ROUTE_COPY;

	/* A present that hides an overlay plane shows no pixels, so only its waits matter. */
	if (job->overlays != NULL && job->overlays[0].hide != VK_FALSE) {
		job->log.why = "waits";
		consumed = present_waits_consume(job, info);
		if (consumed == VK_FALSE)
			return WSI_ROUTE_EMPTY;

		/* Succeeded: the plane is hidden at once. */
		job->log.why = "none";
		return WSI_ROUTE_FAST;
	}

	/* A chain the application may write by other means than transfers is always copied. */
	job->log.why = "usage";
	if (chain->transfer_only == VK_FALSE)
		return WSI_ROUTE_COPY;

	/* A record that asks for GPU completion (a present fence, an identifier) needs the worker. */
	job->log.why = "next";
	for (extension = info->pNext; extension != NULL; extension = extension->pNext) {
		if (extension->sType != VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR &&
		    extension->sType != VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR)
			return WSI_ROUTE_COPY;
	}

	/* Compares the image's writes with those its last completed copy saw. */
	presentation = &chain->presentation[job->indices[0]];
	image = vulkan_image(chain->group->images[job->indices[0]].image);
	unchanged = VK_FALSE;
	pthread_mutex_lock(&swapchain_mutex);

	serial = __atomic_load_n(&image->content_serial, __ATOMIC_ACQUIRE);
	epoch = __atomic_load_n(&chain->device->content_epoch, __ATOMIC_ACQUIRE);
	if (presentation->copied_valid == VK_FALSE)
		job->log.why = "uncopied";
	else if (presentation->copied_serial != serial)
		job->log.why = "changed";
	else if (presentation->copied_epoch != epoch)
		job->log.why = "epoch";
	else
		unchanged = VK_TRUE;

	pthread_mutex_unlock(&swapchain_mutex);

	/* A changed image, or one never copied, is copied. */
	if (unchanged == VK_FALSE)
		return WSI_ROUTE_COPY;

	/* Complete waits are consumed now; waits on GPU work are left to a submission. */
	job->log.why = "waits";
	consumed = present_waits_consume(job, info);
	if (consumed == VK_FALSE)
		return WSI_ROUTE_EMPTY;

	/* Succeeded: the unchanged image is shown at once. */
	job->log.why = "none";
	return WSI_ROUTE_FAST;
}

/*
 * Consumes a present's waits when every one is a distinct, already complete
 * acquisition, as a queue submission would leave out of its native waits;
 * otherwise consumes none of them.
 */
static VkBool32
present_waits_consume(
	struct wsi_present_job *job,
	const VkPresentInfoKHR *info)
{
	struct VkDevice_T *device;
	struct vulkan_sync *sync;
	struct vulkan_sync *earlier;
	uint64_t waited;
	uint32_t index;
	uint32_t previous;
	VkBool32 complete;
	VkResult status;

	/* The device mutex protects every software payload, as the queue's classification does. */
	device = job->queue->device;
	waited = 0U;
	if (job->logged != VK_FALSE)
		waited = vulkan_wsi_cursor_clock();
	pthread_mutex_lock(&device->mutex);

	/* The time the device lock took, for the cursor log. */
	if (job->logged != VK_FALSE)
		job->log.mutex_wait = vulkan_wsi_cursor_clock() - waited;

	/* A lost device consumes nothing here; the submission reports it. */
	complete = VK_FALSE;
	status = vulkan_sync_device_status_locked(device);
	if (status == VK_SUCCESS)
		complete = VK_TRUE;

	/* Every wait must be a distinct semaphore whose acquisition already completed. */
	for (index = 0U; index < info->waitSemaphoreCount && complete != VK_FALSE; index++) {
		/* A semaphore waiting for GPU work keeps the present on the GPU queue. */
		sync = vulkan_sync_object((uint64_t)(uintptr_t)info->pWaitSemaphores[index]);
		if (sync == NULL || sync->software_signaled == VK_FALSE) {
			complete = VK_FALSE;
			break;
		}

		/* A repeated semaphore's second wait would be a native one. */
		for (previous = 0U; previous < index; previous++) {
			earlier = vulkan_sync_object((uint64_t)(uintptr_t)info->pWaitSemaphores[previous]);
			if (earlier == sync)
				complete = VK_FALSE;
		}
	}

	/* Each complete acquisition stops representing completion, exactly once. */
	if (complete != VK_FALSE) {
		for (index = 0U; index < info->waitSemaphoreCount; index++) {
			sync = vulkan_sync_object((uint64_t)(uintptr_t)info->pWaitSemaphores[index]);
			sync->software_signaled = VK_FALSE;
		}
	}

	pthread_mutex_unlock(&device->mutex);

	/* A present that consumed nothing still owns all of its waits. */
	if (complete == VK_FALSE)
		return VK_FALSE;

	/* Succeeded: the waits are consumed and the present needs no queue operation. */
	return VK_TRUE;
}

/*
 * Shows an unchanged cursor image at once, after its waits were consumed.
 *
 * No queue, device or swapchain lock is held while the output is asked.
 * When the output has no free cursor image, the job goes to the cursor
 * lane's worker, which tries again; published then reports that the worker
 * owns the job.
 */
static VkResult
present_fast(
	struct wsi_present_job *job,
	const VkPresentInfoKHR *info,
	VkBool32 *published)
{
	struct vulkan_swapchain *chain;
	struct wsi_queue_worker *worker;
	VkBool32 busy;
	VkResult error;

	/* The waits are consumed: from here on the present counts as enqueued. */
	chain = job->chains[0];
	*published = VK_FALSE;
	job->submitted = VK_TRUE;

	/* A loss published since validation stops the present before the output is asked. */
	error = __atomic_load_n(&chain->error, __ATOMIC_ACQUIRE);
	if (error == VK_SUCCESS) {
		error = __atomic_load_n(&chain->device->error, __ATOMIC_ACQUIRE);
		if (error != VK_SUCCESS)
			error = VK_ERROR_DEVICE_LOST;
	}
	if (error == VK_SUCCESS) {
		error = __atomic_load_n(&chain->device->object.context->error, __ATOMIC_ACQUIRE);
		if (error != VK_SUCCESS)
			error = VK_ERROR_DEVICE_LOST;
	}

	/*
	 * Asks the output once; a busy output is retried by the worker, not
	 * here.  An overlay present is first decided: a held one is done.
	 */
	busy = VK_FALSE;
	if (error == VK_SUCCESS && job->lane == WSI_LANE_OVERLAY) {
		present_overlay_decide(job, VK_TRUE);
		error = present_overlay_commit(job, 0U, VK_FALSE);
		if (job->overlay_outcome == VULKAN_WSI_OVERLAY_BUSY)
			busy = VK_TRUE;
	} else if (error == VK_SUCCESS) {
		error = present_cursor_show(job, 0U, VK_FALSE);
		if (job->outcome == VULKAN_WSI_CURSOR_BUSY)
			busy = VK_TRUE;
	}

	/* A busy output gets the present again from its lane's worker. */
	if (error == VK_SUCCESS && busy != VK_FALSE) {
		error = present_worker_get(job->queue, job->lane, &worker);
		if (error == VK_SUCCESS) {
			/* The caller's result storage is written before the worker may free the job. */
			if (info->pResults != NULL)
				info->pResults[0] = VK_SUCCESS;

			/* The job carries no GPU work; only its native present remains. */
			job->native_only = VK_TRUE;
			job->worker = worker;
			pthread_mutex_lock(&swapchain_mutex);

			present_publish_locked(worker, job);

			pthread_mutex_unlock(&swapchain_mutex);

			/* Succeeded: the worker shows the present, or drops it for a later one. */
			*published = VK_TRUE;
			return VK_SUCCESS;
		}
	}

	/* Consumed waits cannot be returned, so a failed allocation loses the device. */
	if (error == VK_ERROR_OUT_OF_HOST_MEMORY || error == VK_ERROR_OUT_OF_DEVICE_MEMORY)
		error = swapchain_device_lost(chain->device);

	/* A failure stays on the chain for its later operations, as a worker's would. */
	if (error < VK_SUCCESS)
		__atomic_store_n(&chain->error, error, __ATOMIC_RELEASE);
	job->results[0] = error;

	/* A logged cursor present reports itself now that every lock is released. */
	if (job->logged != VK_FALSE)
		present_cursor_log(job, "fast");

	/* Reports why the output did not show the present. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the present is shown, or a later one was. */
	return VK_SUCCESS;
}

/*
 * Shows one cursor-plane target of a job, with its ticket.
 *
 * With retry (the worker), a busy output is asked again every refresh
 * period until it takes the image, a later present takes the ticket's
 * place, or WSI_CURSOR_BUSY_LIMIT passes; the present is then dropped, not
 * failed, because a cursor shows only its latest present.
 */
static VkResult
present_cursor_show(
	struct wsi_present_job *job,
	uint32_t index,
	VkBool32 retry)
{
	struct vulkan_swapchain *chain;
	struct vulkan_wsi_cursor_times *times;
	uint64_t ticket;
	uint64_t newest;
	uint64_t started;
	uint64_t now;
	uint32_t image;
	VkResult error;

	/* The target, its image and ticket, and where the log keeps its times. */
	chain = job->chains[index];
	image = job->indices[index];
	ticket = job->tickets[index];
	times = NULL;
	if (job->logged != VK_FALSE)
		times = &job->log.times;

	/* Asks the output until it takes the image or the present gives way. */
	started = 0U;
	for (;;) {
		/* The present's position, when it named one, and its ticket. */
		error = chain->surface->platform->present_cursor(
			chain->lease,
			chain->presentation[image].native_image,
			job->has_display,
			job->display.dstRect.offset.x,
			job->display.dstRect.offset.y,
			ticket,
			&chain->frame,
			&chain->sequence,
			&job->outcome,
			times);
		if (error != VK_SUCCESS)
			return error;

		/* Shown, dropped, or busy without a retry: the caller has the outcome. */
		if (job->outcome != VULKAN_WSI_CURSOR_BUSY)
			return VK_SUCCESS;
		if (retry == VK_FALSE)
			return VK_SUCCESS;

		/* A later present of the surface will show itself; this one gives way. */
		newest = __atomic_load_n(&chain->surface->cursor_ticket, __ATOMIC_ACQUIRE);
		if (newest > ticket) {
			job->outcome = VULKAN_WSI_CURSOR_DROPPED;
			return VK_SUCCESS;
		}

		/* The time since the first busy answer bounds the retries. */
		error = swapchain_now(&now);
		if (error != VK_SUCCESS)
			return error;
		if (started == 0U)
			started = now;

		/* An output busy for this long is not waited for further. */
		if (now - started >= WSI_CURSOR_BUSY_LIMIT) {
			job->outcome = VULKAN_WSI_CURSOR_DROPPED;
			return VK_SUCCESS;
		}

		/* One refresh later a list may have left a cursor image. */
		present_cursor_pause(chain);
		job->log.retries++;
	}
}

/* Sleeps one refresh period of a cursor chain's display mode. */
static void
present_cursor_pause(
	struct vulkan_swapchain *chain)
{
	struct timespec period;
	uint64_t nanoseconds;
	uint32_t millihertz;

	/* The mode's refresh rate, or 60 Hz when it names none. */
	nanoseconds = WSI_CURSOR_DEFAULT_PERIOD;
	millihertz = 0U;
	if (chain->surface->display_mode != NULL)
		millihertz = chain->surface->display_mode->parameters.refreshRate;
	if (millihertz != 0U)
		nanoseconds = 1000000000000ULL / millihertz;

	/* An interrupted sleep only makes the next try earlier. */
	period.tv_sec = (time_t)(nanoseconds / 1000000000ULL);
	period.tv_nsec = (long)(nanoseconds % 1000000000ULL);
	(void)nanosleep(&period, NULL);

	/* Succeeded: about one refresh has passed. */
	return;
}

/* Appends a job to its worker's queue; the caller holds the swapchain mutex. */
static void
present_publish_locked(
	struct wsi_queue_worker *worker,
	struct wsi_present_job *job)
{
	struct wsi_present_job *queued;

	/* The jobs ahead of this one, and its publication time, for the cursor log. */
	if (job->logged != VK_FALSE) {
		job->log.publish = vulkan_wsi_cursor_clock();
		job->log.depth = 0U;
		if (worker->current != NULL)
			job->log.depth++;
		for (queued = worker->head; queued != NULL; queued = queued->next)
			job->log.depth++;
	}

	/* The job goes last; the worker takes jobs in publication order. */
	if (worker->tail != NULL)
		worker->tail->next = job;
	else
		worker->head = job;
	worker->tail = job;
	swapchain_notify_locked();

	/* Succeeded: only the worker may release the job from now on. */
	return;
}

/* Takes the start times of a present while the cursor log is on. */
static void
present_cursor_log_start(
	struct wsi_cursor_log *log)
{
	VkResult error;

	/* The counter, from which the line's lengths are measured. */
	log->call = vulkan_wsi_cursor_clock();

	/* The monotonic time, which pairs the line with the compositor's log. */
	error = swapchain_now(&log->call_ns);
	if (error != VK_SUCCESS)
		log->call_ns = 0U;

	/* Succeeded: the present's start is noted. */
	return;
}

/*
 * Writes one cursor present's log line to standard error.
 *
 * call_ns is the monotonic time the present was called; every later time is
 * its length from the call, by the counter (0 where the present had no such
 * step), and mutex and queue are the waits for those locks.
 */
static void
present_cursor_log(
	const struct wsi_present_job *job,
	const char *path)
{
	const struct wsi_cursor_log *log;
	const char *outcome;
	const char *why;
	char line[512];
	uint64_t stamps[5];
	uint64_t lengths[5];
	unsigned index;
	int length;

	/* The outcome's name. */
	outcome = "dropped";
	if (job->outcome == VULKAN_WSI_CURSOR_SHOWN)
		outcome = "shown";
	if (job->outcome == VULKAN_WSI_CURSOR_BUSY)
		outcome = "busy";

	/* Each step's time as a length from the call, or zero when it did not happen. */
	log = &job->log;
	why = "none";
	if (log->why != NULL)
		why = log->why;
	stamps[0] = log->publish;
	stamps[1] = log->fence;
	stamps[2] = log->times.locked;
	stamps[3] = log->times.set;
	stamps[4] = log->times.done;
	for (index = 0U; index < 5U; index++) {
		lengths[index] = 0U;
		if (stamps[index] > log->call)
			lengths[index] = stamps[index] - log->call;
	}

	/* One line, written whole by one call. */
	length = snprintf(
		line,
		sizeof(line),
		"VK CURSOR ticket=%llu path=%s result=%d outcome=%s image=%u x=%d y=%d depth=%u call_ns=%llu mutex=%llu queue=%llu publish=%llu fence=%llu lock=%llu set=%llu done=%llu retries=%u why=%s\n",
		(unsigned long long)job->tickets[0],
		path,
		(int)job->results[0],
		outcome,
		job->indices[0],
		(int)job->display.dstRect.offset.x,
		(int)job->display.dstRect.offset.y,
		log->depth,
		(unsigned long long)log->call_ns,
		(unsigned long long)log->mutex_wait,
		(unsigned long long)log->queue_wait,
		(unsigned long long)lengths[0],
		(unsigned long long)lengths[1],
		(unsigned long long)lengths[2],
		(unsigned long long)lengths[3],
		(unsigned long long)lengths[4],
		log->retries,
		why);

	/* A line that could not be formatted is not written; a long one is cut to the buffer. */
	if (length <= 0)
		return;
	if ((size_t)length >= sizeof(line))
		length = (int)sizeof(line) - 1;

	/* A diagnostic line that cannot be written is not retried. */
	(void)write(2, line, (size_t)length);

	/* Succeeded: the line is written. */
	return;
}

/*
 * Notes what each overlay-plane target of a present asks of its plane
 * (ws214-p004): the chain, the rectangles or that the plane hides, and the
 * time of the call for the overlay log.  A job without such a target, or
 * whose backend has no overlay planes, gets no requests.
 */
static VkResult
present_overlay_prepare(
	struct wsi_present_job *job)
{
	struct vulkan_swapchain *chain;
	struct vulkan_wsi_overlay_present *request;
	uint64_t now;
	size_t bytes;
	uint32_t index;
	VkBool32 found;
	VkResult error;

	/* Looks for an overlay target whose backend decides its presents. */
	found = VK_FALSE;
	for (index = 0U; index < job->count; index++) {
		/* An overlay plane of a backend with overlay planes. */
		chain = job->chains[index];
		if (chain->surface->overlay != VK_FALSE && chain->surface->platform->overlay_decide != NULL)
			found = VK_TRUE;
	}

	/* No overlay target: nothing to note. */
	if (found == VK_FALSE)
		return VK_SUCCESS;

	/* One request a target, zero for the targets that are not overlays. */
	bytes = (size_t)job->count * sizeof(*job->overlays);
	if (bytes / sizeof(*job->overlays) != job->count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	job->overlays = vulkan_allocate(&job->allocator, bytes, sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (job->overlays == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(job->overlays, 0, bytes);

	/* The call's time pairs the overlay log with the compositor's; an unreadable clock leaves zero. */
	error = swapchain_now(&now);
	if (error != VK_SUCCESS)
		now = 0U;

	/* Each overlay target's chain, rectangles and hiding. */
	for (index = 0U; index < job->count; index++) {
		/* Only the overlay targets. */
		chain = job->chains[index];
		if (chain->surface->overlay == VK_FALSE)
			continue;

		/* The chain and the time; its rectangles when the present names them. */
		request = &job->overlays[index];
		request->owner = chain->serial;
		request->call_ns = now;
		request->why = "none";
		if (job->has_display != VK_FALSE) {
			request->has_rect = VK_TRUE;
			request->source = job->display.srcRect;
			request->destination = job->display.dstRect;
			request->hide = present_overlay_hidden(&job->display);
		}
	}

	/* Succeeded: every overlay target has its request. */
	return VK_SUCCESS;
}

/*
 * Checks an overlay target's rectangles: a part of its image, shown anywhere
 * inside the output (the kernel checks the scaling limits), never
 * persistent.  Hiding, 1x1 at the origin for both, passes as any other.
 */
static VkResult
present_overlay_rects(
	const struct wsi_present_job *job,
	const struct vulkan_swapchain *chain)
{
	const VkDisplayPresentInfoKHR *display;
	VkExtent2D output;

	/* The rectangles and the output they are shown on. */
	display = &job->display;
	output = chain->surface->display_mode->parameters.visibleRegion;

	/* A persistent present is not offered. */
	if (display->persistent != VK_FALSE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Neither rectangle starts before its image or the output. */
	if (display->srcRect.offset.x < 0 ||
	    display->srcRect.offset.y < 0 ||
	    display->dstRect.offset.x < 0 ||
	    display->dstRect.offset.y < 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Both rectangles cover at least one pixel. */
	if (display->srcRect.extent.width == 0U ||
	    display->srcRect.extent.height == 0U ||
	    display->dstRect.extent.width == 0U ||
	    display->dstRect.extent.height == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The source lies inside the image. */
	if ((uint32_t)display->srcRect.offset.x > chain->extent.width ||
	    display->srcRect.extent.width > chain->extent.width - (uint32_t)display->srcRect.offset.x ||
	    (uint32_t)display->srcRect.offset.y > chain->extent.height ||
	    display->srcRect.extent.height > chain->extent.height - (uint32_t)display->srcRect.offset.y)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The destination lies inside the output. */
	if ((uint32_t)display->dstRect.offset.x > output.width ||
	    display->dstRect.extent.width > output.width - (uint32_t)display->dstRect.offset.x ||
	    (uint32_t)display->dstRect.offset.y > output.height ||
	    display->dstRect.extent.height > output.height - (uint32_t)display->dstRect.offset.y)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Succeeded: the plane can be asked to show the rectangles. */
	return VK_SUCCESS;
}

/*
 * Tells whether a present's rectangles hide its overlay plane: srcRect and
 * dstRect both 1x1 at the origin, the library's convention (README).
 */
static VkBool32
present_overlay_hidden(
	const VkDisplayPresentInfoKHR *display)
{
	/* The source is the image's first pixel alone. */
	if (display->srcRect.offset.x != 0 || display->srcRect.offset.y != 0)
		return VK_FALSE;
	if (display->srcRect.extent.width != 1U || display->srcRect.extent.height != 1U)
		return VK_FALSE;

	/* The destination is the output's first pixel alone. */
	if (display->dstRect.offset.x != 0 || display->dstRect.offset.y != 0)
		return VK_FALSE;
	if (display->dstRect.extent.width != 1U || display->dstRect.extent.height != 1U)
		return VK_FALSE;

	/* Succeeded: the present hides the plane. */
	return VK_TRUE;
}

/*
 * Decides each overlay target of an enqueued job, in call order: held for
 * a primary present, or left to its own commit.  ready says no GPU work of
 * the present remains (the fast path); otherwise the worker says so later.
 */
static void
present_overlay_decide(
	struct wsi_present_job *job,
	VkBool32 ready)
{
	struct vulkan_swapchain *chain;
	struct vulkan_wsi_overlay_present *request;
	void *image;
	uint32_t index;

	/* Every overlay target that is still to be presented. */
	for (index = 0U; index < job->count; index++) {
		/* A target that is not an overlay, or that failed already, has nothing to decide. */
		chain = job->chains[index];
		if (chain->surface->overlay == VK_FALSE || job->results[index] != VK_SUCCESS)
			continue;

		/* The route's reason, and the native image unless the plane hides. */
		request = &job->overlays[index];
		request->ready = ready;
		if (job->lane == WSI_LANE_OVERLAY && job->log.why != NULL)
			request->why = job->log.why;
		image = NULL;
		if (request->hide == VK_FALSE)
			image = chain->presentation[job->indices[index]].native_image;

		/* The output decides against every present decided before it. */
		chain->surface->platform->overlay_decide(chain->lease, image, request);
	}

	/* Succeeded: every overlay target is held or waits for its commit. */
	return;
}

/*
 * Commits one decided overlay target that was not held.
 *
 * With retry (the worker), a busy output is asked again every refresh
 * period until it records the present, or WSI_CURSOR_BUSY_LIMIT passes and
 * the present is dropped, not failed: a later present of the plane shows.
 */
static VkResult
present_overlay_commit(
	struct wsi_present_job *job,
	uint32_t index,
	VkBool32 retry)
{
	struct vulkan_swapchain *chain;
	struct vulkan_wsi_overlay_present *request;
	void *image;
	uint64_t started;
	uint64_t now;
	VkResult error;

	/* A held present is the primary present's to show. */
	chain = job->chains[index];
	request = &job->overlays[index];
	job->overlay_outcome = VULKAN_WSI_OVERLAY_DROPPED;
	if (request->held != VK_FALSE)
		return VK_SUCCESS;

	/* The native image, unless the plane hides, and where the commit runs. */
	image = NULL;
	if (request->hide == VK_FALSE)
		image = chain->presentation[job->indices[index]].native_image;
	request->on_worker = retry;

	/* Asks the output until it records the present or the present gives way. */
	started = 0U;
	for (;;) {
		/* The commit, alone. */
		error = chain->surface->platform->overlay_commit(chain->lease, image, request, &job->overlay_outcome);
		if (error != VK_SUCCESS)
			return error;

		/* Recorded, dropped, or busy without a retry: the caller has the outcome. */
		if (job->overlay_outcome != VULKAN_WSI_OVERLAY_BUSY)
			return VK_SUCCESS;
		if (retry == VK_FALSE)
			return VK_SUCCESS;

		/* The time since the first busy answer bounds the retries. */
		error = swapchain_now(&now);
		if (error != VK_SUCCESS)
			return error;
		if (started == 0U)
			started = now;

		/* An output busy for this long is not waited for further. */
		if (now - started >= WSI_CURSOR_BUSY_LIMIT) {
			job->overlay_outcome = VULKAN_WSI_OVERLAY_DROPPED;
			return VK_SUCCESS;
		}

		/* One refresh later an adoption may have freed the output's places. */
		present_cursor_pause(chain);
	}
}

/*
 * Tells the output that a job's held overlay changes have complete pixels,
 * or, with an error of its GPU work, that they are never to be shown.
 */
static void
present_overlay_settle(
	struct wsi_present_job *job,
	VkResult error)
{
	struct vulkan_swapchain *chain;
	struct vulkan_wsi_overlay_present *request;
	uint32_t index;

	/* Every held overlay target of the job. */
	for (index = 0U; index < job->count; index++) {
		/* Only a held present of an overlay target that was decided. */
		chain = job->chains[index];
		request = &job->overlays[index];
		if (chain->surface->overlay == VK_FALSE || request->held == VK_FALSE)
			continue;

		/* The change may now go with its primary present, or is dropped. */
		chain->surface->platform->overlay_ready(chain->lease, request->serial, error);
	}

	/* Succeeded: no held change of the job waits for its pixels. */
	return;
}

/*
 * Lets the overlay changes held for a primary target's ticket go their way
 * when the target does not reach present_primary: alone after a present
 * that succeeded otherwise, dropped after a failure.  The ticket is used once.
 */
static void
present_primary_skip(
	struct wsi_present_job *job,
	uint32_t index,
	VkResult error)
{
	struct vulkan_swapchain *chain;
	uint64_t ticket;

	/* Only a resolved primary target with a ticket, of a backend with overlay planes. */
	if (job->chains == NULL || job->tickets == NULL)
		return;
	chain = job->chains[index];
	ticket = job->tickets[index];
	if (chain == NULL || ticket == 0U)
		return;
	if (chain->surface->cursor != VK_FALSE || chain->surface->overlay != VK_FALSE)
		return;
	if (chain->surface->platform->primary_skip == NULL)
		return;

	/* The changes the ticket would have taken; the ticket takes nothing more. */
	chain->surface->platform->primary_skip(chain->lease, ticket, error);
	job->tickets[index] = 0U;

	/* Succeeded: no change waits for the ticket. */
	return;
}

/*
 * Waits for an overlay image of a chain to come free from the output, at
 * most until the acquisition's deadline and a second at a time.
 *
 * VK_SUCCESS asks the caller to look for an image again, VK_NOT_READY that
 * no image waits for an adoption, VK_TIMEOUT that the deadline passed.
 */
static VkResult
swapchain_reclaim(
	struct vulkan_swapchain *chain,
	uint64_t timeout,
	uint64_t started)
{
	uint64_t wait;
	uint64_t now;
	VkResult error;

	/* How long this look may wait: not at all for a timeout of zero, the time left otherwise. */
	wait = 0U;
	if (timeout != 0U) {
		/* The time already spent. */
		error = swapchain_now(&now);
		if (error != VK_SUCCESS)
			return error;

		/* The caller's deadline has passed. */
		if (timeout != UINT64_MAX && now - started >= timeout)
			return VK_TIMEOUT;

		/* A second at a time, the kernel's limit for one wait. */
		wait = 1000000000ULL;
		if (timeout != UINT64_MAX && timeout - (now - started) < wait)
			wait = timeout - (now - started);
	}

	/* The output frees an image at an adoption. */
	error = chain->surface->platform->reclaim(chain->lease, wait);

	/* The wait ended without one; the deadline is checked on the next look. */
	if (error == VK_TIMEOUT && timeout != 0U)
		return VK_SUCCESS;

	/* Nothing to wait for, a timeout of zero, or the output's failure. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: an image came free. */
	return VK_SUCCESS;
}
