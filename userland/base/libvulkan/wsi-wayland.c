/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements Wayland WSI using private event queues and shared GPU image fds.
 */

#include "wsi-internal.h"

#include <wayland-client.h>
#include "userland/base/libwayland/zed-gpu-buffer-v1-client-protocol.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>

/* The longest one sleep on the connection lasts while waiting for frame progress, in milliseconds. */
#define WAYLAND_WAIT_SLICE_MS	50

/* A stalled FIFO frame cannot hold presentation forever. */
#define WAYLAND_WAIT_NS 10000000000ULL

/* The initial native shared-image backend accepts these finite scanout extents. */
#define WAYLAND_IMAGE_MIN_EXTENT 16U
#define WAYLAND_IMAGE_MAX_EXTENT 4096U

struct wayland_lease;

/* Borrows the application's connection/surface, owning only wrappers and a queue. */
struct wayland_surface {
	struct vulkan_surface *surface;
	struct wl_display *display;
	struct wl_display *display_wrapper;
	struct wl_surface *native;
	struct wl_event_queue *queue;
	struct wl_registry *registry;
	struct zed_gpu_buffer_v1 *factory;
	struct wayland_lease *active;
	pthread_mutex_t mutex;
	VkBool32 mutex_ready;
	VkBool32 lost;
	uint32_t factory_name;
	uint32_t factory_version;
};

/* A wl_buffer retains a capability on the server until its own protocol retirement. */
struct wayland_image {
	struct wayland_image *next;
	struct wayland_lease *lease;
	struct wl_buffer *buffer;
	struct gpu_image_descriptor descriptor;
	VkBool32 busy;
};

/* One frame callback denotes presentation progress, never GPU memory release. */
struct wayland_frame {
	struct wayland_frame *next;
	struct wayland_lease *lease;
	struct wl_callback *callback;
	uint64_t sequence;
};

/* Owns one swapchain's imported protocol objects through replacement and teardown. */
struct wayland_lease {
	struct wayland_surface *surface;
	struct wayland_image *images;
	struct wayland_frame *frames;
	uint64_t submitted;
	uint64_t completed;
};

/* Callback names must be declared before the immutable listener and operation initializers below. */
static VkBool32 wayland_display_supported(struct wl_display *display);
static VkResult wayland_capabilities(struct vulkan_surface *surface, struct VkPhysicalDevice_T *physical, VkSurfaceCapabilitiesKHR *capabilities);
static VkResult wayland_formats(struct vulkan_surface *surface, struct VkPhysicalDevice_T *physical, uint32_t *count, VkSurfaceFormatKHR *formats);
static VkResult wayland_modes(struct vulkan_surface *surface, struct VkPhysicalDevice_T *physical, uint32_t *count, VkPresentModeKHR *modes);
static VkResult wayland_claim(struct vulkan_surface *surface, struct VkDevice_T *device, void **result);
static VkResult wayland_release(void *private_lease);
static VkResult wayland_wait(void *private_lease, uint64_t sequence, uint64_t timeout);
static void wayland_destroy(struct vulkan_surface *surface);
static VkResult wayland_import(void *private_lease, int fd, const struct gpu_image_descriptor *descriptor, void **result);
static VkResult wayland_present(void *private_lease, void *private_image, VkPresentModeKHR mode, uint64_t *sequence);
static VkResult wayland_present_sync(void *private_lease, void *private_image, VkPresentModeKHR mode, uint64_t *sequence, int wait_fd, uint64_t wait_generation);
static VkResult wayland_commit(void *private_lease, void *private_image, VkPresentModeKHR mode, uint64_t *sequence, int wait_fd, uint64_t wait_generation);
static VkBool32 wayland_commit_early(void *private_lease);
static VkResult wayland_progress(void *private_lease);
static VkBool32 wayland_available(void *private_image);
static void wayland_destroy_image(void *private_image);
static VkResult wayland_dispatch(struct wayland_surface *surface);
static VkResult wayland_surface_initialize(struct vulkan_surface *surface, const VkWaylandSurfaceCreateInfoKHR *info);
static void wayland_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void wayland_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void wayland_buffer_release(void *data, struct wl_buffer *buffer);
static void wayland_frame_done(void *data, struct wl_callback *callback, uint32_t milliseconds);

/* This immutable process-lifetime table selects shared-image operations for Wayland surfaces. */
static const struct vulkan_wsi_platform_ops wayland_platform = {
	wayland_capabilities, wayland_formats, wayland_modes,
	wayland_claim, wayland_release, NULL, wayland_wait, wayland_destroy,
	wayland_import, wayland_present, wayland_progress, wayland_available, wayland_destroy_image, NULL, wayland_present_sync, NULL, NULL,
	wayland_commit_early
};

/* Registry discovery and buffer ownership are delivered only on the WSI queue. */
static const struct wl_registry_listener wayland_registry_listener = {
	wayland_global, wayland_global_remove
};

/* Buffer release publishes allocation availability only to WSI-owned image state. */
static const struct wl_buffer_listener wayland_buffer_listener = {
	wayland_buffer_release
};

/* Frame completion advances the owning lease's pacing sequence on the private queue. */
static const struct wl_callback_listener wayland_frame_listener = {
	wayland_frame_done
};

/*
 * Creates a standard surface while preserving the application's native queues.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateWaylandSurfaceKHR(
	VkInstance instance_handle,
	const VkWaylandSurfaceCreateInfoKHR *info,
	const VkAllocationCallbacks *allocator,
	VkSurfaceKHR *result)
{
	struct VkInstance_T *instance;
	struct vulkan_object *object;
	struct vulkan_surface *surface;
	VkResult error;

	/* No partially constructed surface handle escapes on an early failure. */
	if (result == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Resolves the enabled instance before borrowing any native object. */
	*result = VK_NULL_HANDLE;
	instance = vulkan_instance(instance_handle);
	if (instance == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The creation record must exist before its native object references are read. */
	if (info == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The application retains both native objects throughout this Vulkan surface. */
	if (info->display == NULL || info->surface == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Only the selected standard Wayland creation record has defined semantics. */
	if (info->sType != VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR || info->flags != 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Instance creation must have enabled this platform's public extension. */
	if ((instance->enabled_extensions & VULKAN_INSTANCE_WAYLAND) == 0U)
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Allocation callbacks own the standard object and its platform-private storage. */
	error = vulkan_object_alloc(
		sizeof(*surface),
		__alignof__(struct vulkan_surface),
		VULKAN_OBJECT_SURFACE,
		&instance->object,
		NULL,
		allocator,
		VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
		&object);
	if (error != VK_SUCCESS)
		return error;

	/* Establishes generic ownership before platform initialization can fail. */
	surface = (struct vulkan_surface *)object;
	surface->instance = instance;
	surface->platform = &wayland_platform;

	/* Failed native setup leaves all partial owners reachable by platform teardown. */
	error = wayland_surface_initialize(surface, info);
	if (error != VK_SUCCESS) {
		wayland_destroy(surface);
		vulkan_object_free(object);
		return error;
	}

	/* Publishes only a fully initialized object in the common lifetime registry. */
	error = vulkan_object_publish(object);
	if (error != VK_SUCCESS) {
		wayland_destroy(surface);
		vulkan_object_free(object);
		return error;
	}

	/* Instance teardown sees both Wayland and direct-display surface owners. */
	error = vulkan_wsi_surface_publish(surface, result);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the application owns a registered Vulkan Wayland surface. */
	return VK_SUCCESS;
}

/*
 * Queries the renderer's ability to supply native Wayland presentation images.
 */
VKAPI_ATTR VkBool32 VKAPI_CALL
vkGetPhysicalDeviceWaylandPresentationSupportKHR(
	VkPhysicalDevice physical_handle,
	uint32_t family,
	struct wl_display *display)
{
	struct VkPhysicalDevice_T *physical;
	VkQueueFlags flags;
	VkBool32 supported;
	int error;

	/* Resolves the physical device before inspecting its queue families. */
	physical = vulkan_physical_device(physical_handle);
	if (physical == NULL)
		return VK_FALSE;

	/* A real native connection and existing queue family are both required. */
	if (display == NULL || family >= physical->queue_family_count)
		return VK_FALSE;

	/* A disconnected display cannot receive new image capabilities. */
	error = wl_display_get_error(display);
	if (error != 0)
		return VK_FALSE;

	/* The GPU must expose the shared-allocation contract used by this backend. */
	if ((physical->object.context->capabilities & GPU_CAP_SHARE) == 0U)
		return VK_FALSE;

	/* Presentation requires a queue capable of recording image transfers. */
	flags = physical->queue_families[family].queueFlags;
	if ((flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT)) == 0U)
		return VK_FALSE;

	/* Discovers the image factory without dispatching application event queues. */
	supported = wayland_display_supported(display);
	if (supported == VK_FALSE)
		return VK_FALSE;

	/* Succeeded: this connection advertises the initial shared-image transport. */
	return VK_TRUE;
}

/* Reports application-selected Wayland extents and the minimal opaque presentation contract. */
static VkResult
wayland_capabilities(
	struct vulkan_surface *surface,
	struct VkPhysicalDevice_T *physical,
	VkSurfaceCapabilitiesKHR *capabilities)
{
	struct wayland_surface *native;
	uint32_t maximum;
	VkBool32 lost;
	int error;

	/* A fatal connection error invalidates every associated swapchain. */
	native = surface->platform_private;
	error = wl_display_get_error(native->display);
	if (error != 0)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* A removed factory or expired watchdog remains terminal across later queries. */
	lost = __atomic_load_n(&native->lost, __ATOMIC_ACQUIRE);
	if (lost != VK_FALSE)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Only GPUs with the native allocation-sharing contract support this backend. */
	if ((physical->object.context->capabilities & GPU_CAP_SHARE) == 0U)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Intersects renderer image limits with the native export and scanout contract. */
	maximum = physical->properties.limits.maxImageDimension2D;
	if (maximum > WAYLAND_IMAGE_MAX_EXTENT)
		maximum = WAYLAND_IMAGE_MAX_EXTENT;

	/* An empty intersection cannot advertise a usable surface extent. */
	if (maximum < WAYLAND_IMAGE_MIN_EXTENT)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Reports only dimensions accepted by both the renderer and native backend. */
	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->minImageCount = 3U;
	capabilities->currentExtent.width = UINT32_MAX;
	capabilities->currentExtent.height = UINT32_MAX;
	capabilities->minImageExtent.width = WAYLAND_IMAGE_MIN_EXTENT;
	capabilities->minImageExtent.height = WAYLAND_IMAGE_MIN_EXTENT;
	capabilities->maxImageExtent.width = maximum;
	capabilities->maxImageExtent.height = maximum;
	capabilities->maxImageArrayLayers = 1U;
	capabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	capabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	capabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	capabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	    VK_IMAGE_USAGE_SAMPLED_BIT;

	/* Succeeded: the caller can choose an extent inside the supported intersection. */
	return VK_SUCCESS;
}

/* Enumerates the packed formats whose actual linear row layouts travel with the fd. */
static VkResult
wayland_formats(
	struct vulkan_surface *surface,
	struct VkPhysicalDevice_T *physical,
	uint32_t *count,
	VkSurfaceFormatKHR *formats)
{
	uint32_t capacity;

	(void)surface;
	(void)physical;

	/* The caller must provide a count even when requesting only discovery. */
	if (count == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A discovery-only call reports both initial packed channel orders. */
	if (formats == NULL) {
		*count = 2U;
		return VK_SUCCESS;
	}

	/* Copies the first format only when its output slot belongs to the caller. */
	capacity = *count;
	*count = 0U;
	if (capacity > 0U) {
		formats[0].format = VK_FORMAT_R8G8B8A8_UNORM;
		formats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		*count = 1U;
	}

	/* The second slot describes the alternate packed channel order. */
	if (capacity > 1U) {
		formats[1].format = VK_FORMAT_B8G8R8A8_UNORM;
		formats[1].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		*count = 2U;
	}

	/* A short output array contains a valid prefix of the native formats. */
	if (capacity < 2U)
		return VK_INCOMPLETE;

	/* Succeeded: every initial Wayland surface format was copied. */
	return VK_SUCCESS;
}

/* FIFO waits for frame progress; MAILBOX permits replacement of pending commits. */
static VkResult
wayland_modes(
	struct vulkan_surface *surface,
	struct VkPhysicalDevice_T *physical,
	uint32_t *count,
	VkPresentModeKHR *modes)
{
	uint32_t capacity;

	(void)surface;
	(void)physical;

	/* The caller must provide a count even when requesting only discovery. */
	if (count == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Both modes retain allocation ownership until wl_buffer.release. */
	if (modes == NULL) {
		*count = 2U;
		return VK_SUCCESS;
	}

	/* Copies the FIFO mode only when its output slot belongs to the caller. */
	capacity = *count;
	*count = 0U;
	if (capacity > 0U) {
		modes[0] = VK_PRESENT_MODE_FIFO_KHR;
		*count = 1U;
	}

	/* The second slot exposes replacement without waiting for an earlier frame. */
	if (capacity > 1U) {
		modes[1] = VK_PRESENT_MODE_MAILBOX_KHR;
		*count = 2U;
	}

	/* A short output array contains a valid prefix of the native modes. */
	if (capacity < 2U)
		return VK_INCOMPLETE;

	/* Succeeded: both supported Wayland presentation modes were copied. */
	return VK_SUCCESS;
}

/* Creates independent swapchain bookkeeping without changing application surface state. */
static VkResult
wayland_claim(
	struct vulkan_surface *surface,
	struct VkDevice_T *device,
	void **result)
{
	struct wayland_lease *lease;

	(void)device;

	/* No partial lease escapes if its allocation callback refuses ownership. */
	*result = NULL;
	lease = vulkan_allocate(&surface->object.allocator, sizeof(*lease), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (lease == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Borrows native surface state without changing application mapping or roles. */
	memset(lease, 0, sizeof(*lease));
	lease->surface = surface->platform_private;
	*result = lease;

	/* Succeeded: this swapchain owns independent native bookkeeping. */
	return VK_SUCCESS;
}

/* Retires this swapchain's protocol owners while leaving replacement chains intact. */
static VkResult
wayland_release(
	void *private_lease)
{
	struct wayland_lease *lease;
	struct wayland_surface *surface;
	struct wayland_frame *frame;
	struct wayland_image *image;
	int error;
	int socket_error;

	/* Preserves application mapping and configure state while retiring this lease. */
	lease = private_lease;
	surface = lease->surface;
	pthread_mutex_lock(&surface->mutex);

	/* The compositor holds its front allocation until replacement or surface destruction. */
	if (surface->active == lease)
		surface->active = NULL;

	/* Suppresses queued frame callbacks before freeing their listener data. */
	while (lease->frames != NULL) {
		frame = lease->frames;
		lease->frames = frame->next;
		wl_callback_destroy(frame->callback);
		vulkan_free(&surface->surface->object.allocator, frame);
	}

	/* Retires buffer protocol owners without unmapping the application's surface. */
	while (lease->images != NULL) {
		image = lease->images;
		lease->images = image->next;
		wl_buffer_destroy(image->buffer);
		vulkan_free(&surface->surface->object.allocator, image);
	}

	/* Saves transport failure before any allocator callback can change errno. */
	error = wl_display_flush(surface->display);
	socket_error = errno;

	pthread_mutex_unlock(&surface->mutex);

	/* Local lease ownership ends even when the peer can no longer receive its destructors. */
	vulkan_free(&surface->surface->object.allocator, lease);
	if (error < 0 && socket_error != EAGAIN)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Succeeded: protocol destructors were sent or retained by normal output backpressure. */
	return VK_SUCCESS;
}

/* Waits for WSI frame progress while dispatching no application-owned queue. */
static VkResult
wayland_wait(
	void *private_lease,
	uint64_t sequence,
	uint64_t timeout)
{
	struct wayland_lease *lease;
	struct pollfd descriptor;
	struct timespec started;
	struct timespec now;
	uint64_t elapsed;
	uint64_t completed;
	VkResult error;
	int remaining;
	int status;

	/* A monotonic deadline distinguishes compositor stalling from GPU completion. */
	lease = private_lease;
	status = clock_gettime(CLOCK_MONOTONIC, &started);
	if (status != 0)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Polls private progress without busy waiting or dispatching application callbacks. */
	for (;;) {
		/* Receives releases and pacing updates owned by this surface's queue. */
		error = wayland_progress(lease);
		if (error != VK_SUCCESS)
			return error;

		/* Pacing completes only when this sequence's frame callback has arrived. */
		completed = __atomic_load_n(&lease->completed, __ATOMIC_ACQUIRE);
		if (completed >= sequence)
			break;

		/* Samples elapsed time only while the requested frame remains pending. */
		status = clock_gettime(CLOCK_MONOTONIC, &now);
		if (status != 0)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* A finite deadline can expire without authorizing allocation reuse. */
		elapsed = (uint64_t)(now.tv_sec - started.tv_sec) * 1000000000ULL;
		elapsed += now.tv_nsec - started.tv_nsec;
		if (timeout != UINT64_MAX && elapsed >= timeout)
			return VK_TIMEOUT;

		/*
		 * Sleeps until the compositor writes to the connection, or for a bounded
		 * slice of the remaining deadline.  A timed sleep would round up to the
		 * scheduler tick (10 ms), far longer than one frame takes.
		 */
		remaining = WAYLAND_WAIT_SLICE_MS;
		if (timeout != UINT64_MAX && (timeout - elapsed) / 1000000ULL < (uint64_t)remaining)
			remaining = (int)((timeout - elapsed) / 1000000ULL) + 1;
		descriptor.fd = wl_display_get_fd(lease->surface->display);
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		status = poll(&descriptor, 1U, remaining);
		if (status < 0 && errno != EINTR)
			return VK_ERROR_SURFACE_LOST_KHR;
	}

	/* Succeeded: the selected frame sequence completed on the private queue. */
	return VK_SUCCESS;
}

/* Releases only WSI-owned wrappers, globals and event queue. */
static void
wayland_destroy(
	struct vulkan_surface *generic)
{
	struct wayland_surface *surface;

	/* An allocation failure before platform setup has no native owners to retire. */
	surface = generic->platform_private;
	if (surface == NULL)
		return;

	/* Ends the image factory before retiring its registry and queue. */
	if (surface->factory != NULL)
		zed_gpu_buffer_v1_destroy(surface->factory);

	/* Suppresses registry events before their surface listener data is freed. */
	if (surface->registry != NULL)
		wl_registry_destroy(surface->registry);

	/* Drops only the WSI wrapper around the application's surface. */
	if (surface->native != NULL)
		wl_proxy_wrapper_destroy(surface->native);

	/* The display wrapper owns a queue override, never the actual connection. */
	if (surface->display_wrapper != NULL)
		wl_proxy_wrapper_destroy(surface->display_wrapper);

	/* All proxies assigned to this queue have now been retired. */
	if (surface->queue != NULL)
		wl_event_queue_destroy(surface->queue);

	/* Failed mutex initialization must not be followed by mutex destruction. */
	if (surface->mutex_ready != VK_FALSE)
		pthread_mutex_destroy(&surface->mutex);

	/* Clears the generic owner only after its platform storage is released. */
	vulkan_free(&generic->object.allocator, surface);
	generic->platform_private = NULL;

	/* Succeeded: the application still owns its original connection and surface. */
	return;
}

/* Sends a retained fd and immutable metadata without exposing GPU handles to the app. */
static VkResult
wayland_import(
	void *private_lease,
	int fd,
	const struct gpu_image_descriptor *descriptor,
	void **result)
{
	struct wayland_lease *lease;
	struct wayland_surface *surface;
	struct wayland_image *image;
	struct wl_array metadata;
	int status;

	/* No partial image owner escapes if allocation fails before fd marshalling. */
	*result = NULL;
	lease = private_lease;
	surface = lease->surface;
	image = vulkan_allocate(&surface->surface->object.allocator, sizeof(*image), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (image == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The lease retains immutable metadata for the lifetime of its native image. */
	memset(image, 0, sizeof(*image));
	image->lease = lease;
	image->descriptor = *descriptor;

	/* The transport treats GPU metadata as opaque array bytes. */
	metadata.size = sizeof(*descriptor);
	metadata.alloc = metadata.size;
	metadata.data = &image->descriptor;

	/* Serializes native buffer construction with private-queue listener dispatch. */
	pthread_mutex_lock(&surface->mutex);

	/* The marshaller duplicates fd; the caller keeps its independent original. */
	image->buffer = zed_gpu_buffer_v1_create_buffer(surface->factory, fd, &metadata);
	if (image->buffer == NULL) {
		pthread_mutex_unlock(&surface->mutex);
		vulkan_free(&surface->surface->object.allocator, image);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Release notifications must reach initialized image state before any commit. */
	status = wl_buffer_add_listener(image->buffer, &wayland_buffer_listener, image);
	if (status != 0) {
		wl_buffer_destroy(image->buffer);
		pthread_mutex_unlock(&surface->mutex);
		vulkan_free(&surface->surface->object.allocator, image);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Partial-chain and ordinary teardown now find this buffer through its lease. */
	image->next = lease->images;
	lease->images = image;

	pthread_mutex_unlock(&surface->mutex);

	/* Succeeded: the native image is owned by this lease and initially available. */
	*result = image;
	return VK_SUCCESS;
}

/* Commits a completed GPU image and separates frame pacing from allocation reuse. */
static VkResult
wayland_present(
	void *private_lease,
	void *private_image,
	VkPresentModeKHR mode,
	uint64_t *sequence)
{
	VkResult error;

	/* No fence: the image is complete. */
	error = wayland_commit(private_lease, private_image, mode, sequence, -1, 0U);
	return error;
}

/*
 * Commits a GPU image with the fence of its rendering when the compositor
 * takes acquire fences (zed_gpu_buffer_v1 revision two); otherwise the
 * caller waited for the fence and the image is complete.
 */
static VkResult
wayland_present_sync(
	void *private_lease,
	void *private_image,
	VkPresentModeKHR mode,
	uint64_t *sequence,
	int wait_fd,
	uint64_t wait_generation)
{
	struct wayland_lease *lease;
	VkResult error;

	/* A revision-one compositor gets no fence. */
	lease = private_lease;
	if (lease->surface->factory_version < 2U)
		wait_fd = -1;
	error = wayland_commit(private_lease, private_image, mode, sequence, wait_fd, wait_generation);
	return error;
}

/* Reports whether this surface's compositor takes acquire fences, so a commit may come before completion. */
static VkBool32
wayland_commit_early(
	void *private_lease)
{
	struct wayland_lease *lease;

	/* Revision two of the factory. */
	lease = private_lease;
	if (lease->surface->factory_version >= 2U)
		return VK_TRUE;
	return VK_FALSE;
}

/*
 * Commits a GPU image, with its acquire fence when one is given, and
 * separates frame pacing from allocation reuse.
 */
static VkResult
wayland_commit(
	void *private_lease,
	void *private_image,
	VkPresentModeKHR mode,
	uint64_t *sequence,
	int wait_fd,
	uint64_t wait_generation)
{
	struct wayland_lease *lease;
	struct wayland_surface *surface;
	struct wayland_image *image;
	struct wayland_frame *frame;
	VkResult error;
	int status;
	int socket_error;

	/* FIFO waits for an earlier frame; MAILBOX may replace pending commits immediately. */
	lease = private_lease;
	surface = lease->surface;
	image = private_image;
	if (mode == VK_PRESENT_MODE_FIFO_KHR) {
		/* A stalled compositor becomes terminal after the finite native watchdog. */
		error = wayland_wait(lease, lease->submitted, WAYLAND_WAIT_NS);
		if (error == VK_TIMEOUT) {
			__atomic_store_n(&surface->lost, VK_TRUE, __ATOMIC_RELEASE);
			return VK_ERROR_SURFACE_LOST_KHR;
		}

		/* Other native failures prevent submission of another frame. */
		if (error != VK_SUCCESS)
			return error;
	}

	/* Allocates pacing state before changing the image's compositor ownership. */
	frame = vulkan_allocate(&surface->surface->object.allocator, sizeof(*frame), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (frame == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Listener data borrows the lease through the callback's lifetime. */
	memset(frame, 0, sizeof(*frame));
	frame->lease = lease;

	/* Orders callback creation, image ownership and commit against private dispatch. */
	pthread_mutex_lock(&surface->mutex);

	/* Requests a frame callback without committing the surface yet. */
	frame->callback = wl_surface_frame(surface->native);
	if (frame->callback == NULL) {
		pthread_mutex_unlock(&surface->mutex);
		vulkan_free(&surface->surface->object.allocator, frame);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* The initialized listener must exist before the frame can reach the compositor. */
	status = wl_callback_add_listener(frame->callback, &wayland_frame_listener, frame);
	if (status != 0) {
		wl_callback_destroy(frame->callback);
		pthread_mutex_unlock(&surface->mutex);
		vulkan_free(&surface->surface->object.allocator, frame);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Busy prevents acquire from handing storage back before compositor release. */
	__atomic_store_n(&image->busy, VK_TRUE, __ATOMIC_RELEASE);

	/* The submitted sequence names this pacing callback independently of buffer release. */
	lease->submitted++;
	frame->sequence = lease->submitted;
	frame->next = lease->frames;
	lease->frames = frame;

	/* The fence the compositor waits for before it uses this commit. */
	if (wait_fd >= 0)
		zed_gpu_buffer_v1_set_acquire_fence(surface->factory, surface->native, wait_fd, wait_generation);

	/* Commits the GPU image with full-surface damage on the native wrapper. */
	wl_surface_attach(surface->native, image->buffer, 0, 0);
	wl_surface_damage(surface->native, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface->native);
	surface->active = lease;
	*sequence = lease->submitted;

	/* Preserves socket failure while requests remain ordered under the surface mutex. */
	status = wl_display_flush(surface->display);
	socket_error = errno;

	pthread_mutex_unlock(&surface->mutex);

	/* Backpressure is retryable, but loss of the native connection is terminal. */
	if (status < 0 && socket_error != EAGAIN)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Succeeded: the compositor owns the committed image until buffer release. */
	return VK_SUCCESS;
}

/* Serializes WSI callbacks without holding the Vulkan swapchain state mutex. */
static VkResult
wayland_progress(
	void *private_lease)
{
	struct wayland_lease *lease;
	VkResult error;

	/* Serializes WSI listeners while leaving all application queues untouched. */
	lease = private_lease;
	pthread_mutex_lock(&lease->surface->mutex);

	/* Dispatches private events and latches surface loss for future acquisition. */
	error = wayland_dispatch(lease->surface);
	if (error == VK_ERROR_SURFACE_LOST_KHR)
		__atomic_store_n(&lease->surface->lost, VK_TRUE, __ATOMIC_RELEASE);

	pthread_mutex_unlock(&lease->surface->mutex);

	/* The terminal latch remains visible to later acquire and capability queries. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: private callbacks were dispatched without consuming application events. */
	return VK_SUCCESS;
}

/* Reports compositor ownership without reading or dispatching a socket under K/U state locks. */
static VkBool32
wayland_available(
	void *private_image)
{
	struct wayland_image *image;
	VkBool32 busy;

	/* Buffer release publishes availability only after compositor use has ended. */
	image = private_image;
	busy = __atomic_load_n(&image->busy, __ATOMIC_ACQUIRE);
	if (busy != VK_FALSE)
		return VK_FALSE;

	/* Succeeded: the compositor no longer retains this image for rendering or scanout. */
	return VK_TRUE;
}

/* Retires one buffer protocol identity before its image group releases the native lease. */
static void
wayland_destroy_image(
	void *private_image)
{
	struct wayland_image *image;
	struct wayland_image **link;
	struct wayland_surface *surface;

	/* The group's surviving lease keeps listener serialization and allocator state alive. */
	image = private_image;
	surface = image->lease->surface;
	pthread_mutex_lock(&surface->mutex);

	/* Remove exactly this group-owned image without touching a replacement chain's buffers. */
	link = &image->lease->images;
	while (*link != image)
		link = &(*link)->next;
	*link = image->next;

	/* Buffer destruction leaves the application surface mapped and the compositor's allocation alive. */
	wl_buffer_destroy(image->buffer);

	pthread_mutex_unlock(&surface->mutex);
	vulkan_free(&surface->surface->object.allocator, image);

	/* Succeeded: no local listener or protocol identity borrows this native image wrapper. */
	return;
}

/* Reads only immediately available bytes using Wayland's coordinated multi-reader protocol. */
static VkResult
wayland_dispatch(
	struct wayland_surface *surface)
{
	struct pollfd descriptor;
	VkBool32 lost;
	int status;
	int flushed;

	/* Drains this queue before preparing a read, as required by reader coordination. */
	for (;;) {
		/* A dispatch error makes the connection unusable before another read is prepared. */
		status = wl_display_dispatch_queue_pending(surface->display, surface->queue);
		if (status < 0)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* Factory removal and watchdog expiry remain terminal without a socket error. */
		lost = __atomic_load_n(&surface->lost, __ATOMIC_ACQUIRE);
		if (lost != VK_FALSE)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* A successful preparation owns one read intention until read or cancellation. */
		status = wl_display_prepare_read_queue(surface->display, surface->queue);
		if (status == 0)
			break;

		/* Pending private callbacks are retryable; other preparation failures are terminal. */
		if (errno != EAGAIN)
			return VK_ERROR_SURFACE_LOST_KHR;
	}

	/* Output failure is resolved after the prepared read has been consumed or canceled. */
	flushed = wl_display_flush(surface->display);

	/* Polls only immediately available input so acquire timeout zero remains nonblocking. */
	descriptor.fd = wl_display_get_fd(surface->display);
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	status = poll(&descriptor, 1U, 0);
	if (status > 0 && (descriptor.revents & POLLIN) != 0) {
		/* Reading consumes the preparation and queues events for each object's owner. */
		status = wl_display_read_events(surface->display);
		if (status < 0)
			return VK_ERROR_SURFACE_LOST_KHR;
	} else {
		/* Cancellation releases the preparation even when poll was interrupted. */
		wl_display_cancel_read(surface->display);
		if (status < 0 && errno != EINTR)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* A disconnected or invalid socket cannot make future native progress. */
		if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			return VK_ERROR_SURFACE_LOST_KHR;
	}

	/* Reads merely enqueued foreign events; only WSI callbacks run here. */
	status = wl_display_dispatch_queue_pending(surface->display, surface->queue);
	if (status < 0)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* A registry callback may have removed the factory during this dispatch. */
	lost = __atomic_load_n(&surface->lost, __ATOMIC_ACQUIRE);
	if (lost != VK_FALSE)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Propagates deferred transport failure after retiring the read intention. */
	if (flushed < 0) {
		status = wl_display_get_error(surface->display);
		if (status != 0)
			return VK_ERROR_SURFACE_LOST_KHR;
	}

	/* Succeeded: all immediately available private progress has been delivered. */
	return VK_SUCCESS;
}

/* Discovers the private fd transport through a standard Wayland registry roundtrip. */
static VkResult
wayland_surface_initialize(
	struct vulkan_surface *generic,
	const VkWaylandSurfaceCreateInfoKHR *info)
{
	struct wayland_surface *surface;
	int status;

	/* Allocates the platform owner before creating any queue or proxy. */
	surface = vulkan_allocate(&generic->object.allocator, sizeof(*surface), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (surface == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Generic teardown can reach every partially initialized platform resource. */
	memset(surface, 0, sizeof(*surface));
	generic->platform_private = surface;
	surface->surface = generic;
	surface->display = info->display;

	/* Initializes listener serialization before any callback can borrow this object. */
	status = pthread_mutex_init(&surface->mutex, NULL);
	if (status != 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The destructor may destroy this mutex only after successful initialization. */
	surface->mutex_ready = VK_TRUE;

	/* Allocates the queue that separates native progress from application dispatch. */
	surface->queue = wl_display_create_queue(info->display);
	if (surface->queue == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Borrows the display identity through an independently owned queue override. */
	surface->display_wrapper = wl_proxy_create_wrapper(info->display);
	if (surface->display_wrapper == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Borrows the surface without changing its application-selected queue. */
	surface->native = wl_proxy_create_wrapper(info->surface);
	if (surface->native == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* New registry, buffer and callback proxies inherit only the WSI queue. */
	wl_proxy_set_queue((struct wl_proxy *)surface->display_wrapper, surface->queue);
	wl_proxy_set_queue((struct wl_proxy *)surface->native, surface->queue);

	/* Creates a private registry proxy before installing its listener. */
	surface->registry = wl_display_get_registry(surface->display_wrapper);
	if (surface->registry == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Factory advertisements are interpreted only by WSI-owned listener data. */
	status = wl_registry_add_listener(surface->registry, &wayland_registry_listener, surface);
	if (status != 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Completes registry discovery while application callbacks remain queued. */
	status = wl_display_roundtrip_queue(surface->display, surface->queue);
	if (status < 0)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* A compositor without this native transport cannot import shared images. */
	if (surface->factory == NULL)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Succeeded: the surface owns a private queue and an advertised image factory. */
	return VK_SUCCESS;
}

/* Binds exactly the private image factory required by this transport backend. */
static void
wayland_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct wayland_surface *surface;
	int match;

	/* Other advertised interfaces remain the application's concern. */
	surface = data;
	match = strcmp(interface, "zed_gpu_buffer_v1");
	if (match != 0)
		return;

	/* Retains at most one factory for this surface. */
	if (version < 1U || surface->factory != NULL)
		return;

	/* Revision two adds acquire fences; revision one commits completed images only. */
	if (version > 2U)
		version = 2U;

	/* Binds the selected interface through the registry's inherited private queue. */
	surface->factory = wl_registry_bind(registry, name, &zed_gpu_buffer_v1_interface, version);
	if (surface->factory == NULL)
		return;
	surface->factory_version = version;

	/* The global name identifies removal of this specific transport advertisement. */
	surface->factory_name = name;

	/* Succeeded: future buffer imports can use the negotiated native factory. */
	return;
}

/* Factory removal makes future import unsupported instead of silently faking presentation. */
static void
wayland_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	struct wayland_surface *surface;

	(void)registry;

	/* Removal of this factory permanently prevents new imports on the surface. */
	surface = data;
	if (name == surface->factory_name)
		__atomic_store_n(&surface->lost, VK_TRUE, __ATOMIC_RELEASE);

	/* Succeeded: existing protocol owners can retire through ordinary teardown. */
	return;
}

/* Allocation reuse follows release, not merely a frame callback or socket send. */
static void
wayland_buffer_release(
	void *data,
	struct wl_buffer *buffer)
{
	struct wayland_image *image;

	(void)buffer;

	/* The compositor has stopped using this allocation for rendering and scanout. */
	image = data;
	__atomic_store_n(&image->busy, VK_FALSE, __ATOMIC_RELEASE);

	/* Wakes acquirers after publishing that no compositor still borrows this image. */
	vulkan_wsi_image_notify();

	/* Succeeded: a later acquire may hand this image back to the producer. */
	return;
}

/* Records pacing progress and retires the one-shot callback independently of the buffer. */
static void
wayland_frame_done(
	void *data,
	struct wl_callback *callback,
	uint32_t milliseconds)
{
	struct wayland_frame *frame;
	struct wayland_frame **link;
	struct wayland_lease *lease;
	uint64_t completed;

	(void)milliseconds;

	/* Frame completion advances pacing independently of any image-release callback. */
	frame = data;
	lease = frame->lease;
	completed = __atomic_load_n(&lease->completed, __ATOMIC_RELAXED);
	if (frame->sequence > completed)
		__atomic_store_n(&lease->completed, frame->sequence, __ATOMIC_RELEASE);

	/* Finds this one-shot callback in the lease while the private queue mutex is held. */
	link = &lease->frames;
	while (*link != NULL && *link != frame)
		link = &(*link)->next;

	/* Teardown must no longer visit this callback's listener data. */
	if (*link == frame)
		*link = frame->next;

	/* Suppresses any queued event before returning callback storage to the allocator. */
	wl_callback_destroy(callback);
	vulkan_free(&lease->surface->surface->object.allocator, frame);

	/* Succeeded: the lease records pacing progress without releasing an image. */
	return;
}

/* Probes the private buffer factory without assigning any application proxy to a new queue. */
static VkBool32
wayland_display_supported(
	struct wl_display *display)
{
	struct wayland_surface native;
	VkBool32 supported;
	int status;

	/* Temporary native state owns discovery proxies but never the supplied connection. */
	memset(&native, 0, sizeof(native));
	native.display = display;
	supported = VK_FALSE;

	/* Allocates an isolated queue before creating any discovery proxy. */
	native.queue = wl_display_create_queue(display);
	if (native.queue == NULL)
		goto cleanup;

	/* A wrapper preserves the application's original display queue assignment. */
	native.display_wrapper = wl_proxy_create_wrapper(display);
	if (native.display_wrapper == NULL)
		goto cleanup;

	/* Registry children inherit this temporary discovery queue. */
	wl_proxy_set_queue((struct wl_proxy *)native.display_wrapper, native.queue);

	/* Every partial registry owner is reachable by the single forward cleanup path. */
	native.registry = wl_display_get_registry(native.display_wrapper);
	if (native.registry == NULL)
		goto cleanup;

	/* Captures only the image-factory advertisement needed by this transport. */
	status = wl_registry_add_listener(native.registry, &wayland_registry_listener, &native);
	if (status != 0)
		goto cleanup;

	/* Reads ordered globals without invoking application callbacks. */
	status = wl_display_roundtrip_queue(display, native.queue);
	if (status < 0)
		goto cleanup;

	/* A successful roundtrip alone does not establish native image support. */
	if (native.factory != NULL)
		supported = VK_TRUE;

cleanup:
	/* Retires the temporary binding before discarding its discovery queue. */
	if (native.factory != NULL)
		zed_gpu_buffer_v1_destroy(native.factory);

	/* Suppresses pending registry callbacks before stack-owned listener data expires. */
	if (native.registry != NULL)
		wl_registry_destroy(native.registry);

	/* Only the wrapper's queue override is released; the application keeps the display. */
	if (native.display_wrapper != NULL)
		wl_proxy_wrapper_destroy(native.display_wrapper);

	/* No live discovery proxy remains assigned to this temporary queue. */
	if (native.queue != NULL)
		wl_event_queue_destroy(native.queue);

	/* Missing advertisements and failed discovery both refuse native support. */
	if (supported == VK_FALSE)
		return VK_FALSE;

	/* Succeeded: the connection advertises the required shared-image factory. */
	return VK_TRUE;
}
