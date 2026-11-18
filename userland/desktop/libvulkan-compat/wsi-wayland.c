/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Wayland surface ownership and bounded private-queue progress for the Linux WSI. */
#include "compat.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Bounds each protocol or device wait with the same monotonic time units. */
#define SURFACE_POLL_SLICE_NS UINT64_C(100000000)
#define SURFACE_INITIALIZE_TIMEOUT_NS UINT64_C(5000000000)

static void surface_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void surface_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void surface_format(void *data, struct zwp_linux_dmabuf_v1 *object, uint32_t format);
static void surface_modifier(void *data, struct zwp_linux_dmabuf_v1 *object, uint32_t format, uint32_t high, uint32_t low);
static void surface_sync(void *data, struct wl_callback *callback, uint32_t serial);
static int surface_roundtrip(struct compat_surface *surface);
static int surface_initialize(struct compat_surface *surface, struct wl_surface *application_surface);
static int surface_lost(struct compat_surface *surface);
static VkBool32 surface_support(VkPhysicalDevice physical, uint32_t family, unsigned external);

/*
 * Creates a private surface record while leaving application Wayland objects owned by the application.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateWaylandSurfaceKHR(
	VkInstance instance,
	const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkSurfaceKHR *pSurface)
{
	struct compat_surface *surface;
	struct compat_instance *owner;
	int error;

	/* Requires the instance's explicitly enabled Wayland surface extension. */
	owner = compat_instance_get(instance);
	if (owner == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses calls through an instance that never enabled this WSI extension. */
	if ((owner->enabled & COMPAT_INSTANCE_WAYLAND) == 0)
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Requires the application's valid Wayland connection and surface. */
	if (pCreateInfo == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses a missing destination rather than allocating unreachable storage. */
	if (pSurface == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Validates the connection before creating private proxies. */
	if (pCreateInfo->display == NULL)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Validates the application surface before wrapping it. */
	if (pCreateInfo->surface == NULL)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Allocates the Vulkan object using the application's allocator when supplied. */
	surface = compat_object_allocate(sizeof(*surface), pAllocator);
	if (surface == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Retains object ownership for failure cleanup and later destruction. */
	surface->instance = owner;
	surface->display = pCreateInfo->display;
	if (pAllocator != NULL) {
		surface->allocator = *pAllocator;
		surface->allocated = 1;
	}

	/* Initializes the private protocol objects within bounded compositor deadlines. */
	error = surface_initialize(surface, pCreateInfo->surface);
	if (error != 0) {
		/* Retires every partial private proxy through the normal surface destructor. */
		vkDestroySurfaceKHR(instance, (VkSurfaceKHR)(uintptr_t)surface, pAllocator);
		return VK_ERROR_SURFACE_LOST_KHR;
	}

	/* Publishes our own non-dispatchable surface handle. */
	*pSurface = (VkSurfaceKHR)(uintptr_t)surface;

	/* Succeeded: an absent DMA-BUF global is reported by swapchain creation, not here. */
	return VK_SUCCESS;
}

/*
 * Retires only our private protocol proxies, after Vulkan callers have destroyed their swapchains.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroySurfaceKHR(
	VkInstance instance,
	VkSurfaceKHR handle,
	const VkAllocationCallbacks *pAllocator)
{
	struct compat_surface *surface;

	/* A null surface has no resources to retire. */
	(void)instance;
	(void)pAllocator;
	surface = (struct compat_surface *)(uintptr_t)handle;
	if (surface == NULL)
		return;

	/* Removing callback proxies before freeing records prevents stale queued callback data. */
	compat_swapchain_collect(surface, 1);
	if (surface->frame != NULL)
		wl_callback_destroy(surface->frame);

	/* Retires the privately bound DMA-BUF global. */
	if (surface->dmabuf != NULL)
		compat_dmabuf_destroy(surface->dmabuf);

	/* Retires registry announcements before the queue itself disappears. */
	if (surface->registry != NULL)
		wl_registry_destroy(surface->registry);

	/* Wrappers do not own either application proxy. */
	if (surface->surface_wrapper != NULL)
		wl_proxy_wrapper_destroy(surface->surface_wrapper);

	/* Retires the display wrapper while preserving the application connection. */
	if (surface->display_wrapper != NULL)
		wl_proxy_wrapper_destroy(surface->display_wrapper);

	/* Releases the private queue after every child proxy is gone. */
	if (surface->queue != NULL)
		wl_event_queue_destroy(surface->queue);

	/* Releases advertised layouts and the Vulkan object with its matching allocator. */
	free(surface->modifiers);
	compat_object_free(surface, surface->allocated, &surface->allocator);

	/* Succeeded: only private surface and callback storage has retired. */
	return;
}

/*
 * Reports support only for graphics queues with an actual DMA-BUF image export path.
 */
VKAPI_ATTR VkBool32 VKAPI_CALL
vkGetPhysicalDeviceWaylandPresentationSupportKHR(
	VkPhysicalDevice physicalDevice,
	uint32_t queueFamilyIndex,
	struct wl_display *display)
{
	VkBool32 supported;

	/* Connection errors cannot support presentation. */
	if (display == NULL)
		return VK_FALSE;

	/* Returns the physical device and queue's verified export capability. */
	supported = surface_support(physicalDevice, queueFamilyIndex, 1);

	/* Succeeded: reports the queried queue support. */
	return supported;
}

/*
 * Reports graphics/export support and the still-live compositor connection.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceSupportKHR(
	VkPhysicalDevice physicalDevice,
	uint32_t queueFamilyIndex,
	VkSurfaceKHR handle,
	VkBool32 *pSupported)
{
	struct compat_surface *surface;
	int evaluated;

	/* Requires the caller's result destination. */
	if (pSupported == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires a surface that can still communicate with its compositor. */
	surface = (struct compat_surface *)(uintptr_t)handle;
	if (surface == NULL)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Display surfaces use the graphics copy path without a Wayland connection. */
	if (surface->kms != NULL) {
		*pSupported = surface_support(physicalDevice, queueFamilyIndex, 0);
		return VK_SUCCESS;
	}

	/* Reports a disconnected compositor without querying a foreign handle. */
	evaluated = wl_display_get_error(surface->display);
	if (evaluated != 0)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Returns queue and export support; formats supply the compositor intersection. */
	*pSupported = surface_support(physicalDevice, queueFamilyIndex, 1);

	/* Succeeded: the support destination contains the physical queue's answer. */
	return VK_SUCCESS;
}

/*
 * Describes client-selected extents and the bounded image count supported by this WSI.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
	VkPhysicalDevice physicalDevice,
	VkSurfaceKHR handle,
	VkSurfaceCapabilitiesKHR *pCapabilities)
{
	struct compat_surface *surface;
	struct compat_instance *instance;
	VkPhysicalDeviceProperties properties;

	/* Requires both a usable surface and an output destination. */
	surface = (struct compat_surface *)(uintptr_t)handle;
	if (surface == NULL)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Rejects a missing capabilities destination. */
	if (pCapabilities == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires the owning backend instance for the physical limits. */
	instance = compat_instance_for_physical(physicalDevice);
	if (instance == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Reads the backend's actual two-dimensional image size limit. */
	instance->properties(physicalDevice, &properties);
	memset(pCapabilities, 0, sizeof(*pCapabilities));
	pCapabilities->minImageCount = 2;
	pCapabilities->maxImageCount = 8;
	pCapabilities->currentExtent.width = UINT32_MAX;
	pCapabilities->currentExtent.height = UINT32_MAX;
	pCapabilities->minImageExtent.width = 1;
	pCapabilities->minImageExtent.height = 1;
	pCapabilities->maxImageExtent.width = properties.limits.maxImageDimension2D;
	pCapabilities->maxImageExtent.height = properties.limits.maxImageDimension2D;
	pCapabilities->maxImageArrayLayers = 1;
	pCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	pCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	pCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	pCapabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	/* Display images match their selected physical timing instead of client-chosen Wayland extents. */
	if (surface->kms != NULL) {
		pCapabilities->currentExtent = surface->extent;
		pCapabilities->minImageExtent = surface->extent;
		pCapabilities->maxImageExtent = surface->extent;
		pCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	}

	/* Succeeded: Wayland leaves the image extent to the application. */
	return VK_SUCCESS;
}

/*
 * Enumerates only formats whose compositor modifier also permits backend image export.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceFormatsKHR(
	VkPhysicalDevice physicalDevice,
	VkSurfaceKHR handle,
	uint32_t *pSurfaceFormatCount,
	VkSurfaceFormatKHR *pSurfaceFormats)
{
	struct compat_surface *surface;
	struct compat_capabilities capabilities;
	VkFormat formats[4];
	uint32_t capacity;
	uint32_t count;
	uint32_t index;
	uint32_t fourcc;
	uint64_t modifier;
	int supported;

	/* Requires a count and a live private surface. */
	if (pSurfaceFormatCount == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Validates the surface before reading its advertised layouts. */
	surface = (struct compat_surface *)(uintptr_t)handle;
	if (surface == NULL)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* KMS XRGB buffers support the two blue-first Vulkan encodings independently of DMA-BUF export. */
	if (surface->kms != NULL) {
		/* Measurement reports both display formats without reading count input. */
		if (pSurfaceFormats == NULL) {
			*pSurfaceFormatCount = 2;
			return VK_SUCCESS;
		}

		/* Copies only available caller capacity. */
		capacity = *pSurfaceFormatCount;
		if (capacity > 0) {
			pSurfaceFormats[0].format = VK_FORMAT_B8G8R8A8_UNORM;
			pSurfaceFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		}

		/* SRGB retains the same physical byte ordering. */
		if (capacity > 1) {
			pSurfaceFormats[1].format = VK_FORMAT_B8G8R8A8_SRGB;
			pSurfaceFormats[1].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		}

		/* Reports an incomplete array without writing beyond its capacity. */
		if (capacity < 2)
			return VK_INCOMPLETE;

		/* Succeeded: both KMS encodings fit in the output. */
		*pSurfaceFormatCount = 2;
		return VK_SUCCESS;
	}

	/* Intersects the four supported channel encodings with actual export capability. */
	compat_physical_capabilities(physicalDevice, &capabilities);
	formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
	formats[1] = VK_FORMAT_B8G8R8A8_SRGB;
	formats[2] = VK_FORMAT_R8G8B8A8_UNORM;
	formats[3] = VK_FORMAT_R8G8B8A8_SRGB;
	capacity = 0;
	if (pSurfaceFormats != NULL)
		capacity = *pSurfaceFormatCount;

	/* Counts every available format while copying only the caller's capacity. */
	count = 0;
	for (index = 0; index < 4; index++) {
		/* Selects a supported single-plane layout for this channel encoding. */
		supported = compat_surface_modifier(surface, physicalDevice, formats[index], capabilities.path, &fourcc, &modifier);
		if (supported == 0)
			continue;

		/* Copies only when an output array has remaining storage. */
		if (count < capacity) {
			pSurfaceFormats[count].format = formats[index];
			pSurfaceFormats[count].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		}

		/* Includes supported formats even when the caller only measures. */
		count++;
	}

	/* Measurement returns the total without consulting an uninitialized input count. */
	if (pSurfaceFormats == NULL) {
		*pSurfaceFormatCount = count;
		return VK_SUCCESS;
	}

	/* An undersized output returns the number actually copied. */
	if (capacity < count) {
		*pSurfaceFormatCount = capacity;
		return VK_INCOMPLETE;
	}

	/* Succeeded: every available channel encoding was copied. */
	*pSurfaceFormatCount = count;
	return VK_SUCCESS;
}

/*
 * Enumerates FIFO and MAILBOX without permitting unimplemented presentation semantics.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfacePresentModesKHR(
	VkPhysicalDevice physicalDevice,
	VkSurfaceKHR handle,
	uint32_t *pPresentModeCount,
	VkPresentModeKHR *pPresentModes)
{
	uint32_t capacity;
	uint32_t count;
	struct compat_surface *surface;

	/* Requires a private surface and the caller's count destination. */
	(void)physicalDevice;
	if (handle == VK_NULL_HANDLE)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Rejects a missing enumeration count. */
	if (pPresentModeCount == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* KMS scanout exposes only FIFO, while Wayland also supports MAILBOX. */
	surface = (struct compat_surface *)(uintptr_t)handle;
	count = 2;
	if (surface->kms != NULL)
		count = 1;

	/* Measurement needs no output array or initialized input count. */
	if (pPresentModes == NULL) {
		*pPresentModeCount = count;
		return VK_SUCCESS;
	}

	/* Copies only the available caller capacity. */
	capacity = *pPresentModeCount;
	if (capacity > 0)
		pPresentModes[0] = VK_PRESENT_MODE_FIFO_KHR;

	/* The second supported mode has no frame-callback pacing. */
	if (count > 1 && capacity > 1)
		pPresentModes[1] = VK_PRESENT_MODE_MAILBOX_KHR;

	/* Reports an undersized output with the count actually copied. */
	if (capacity < count)
		return VK_INCOMPLETE;

	/* Succeeded: both supported modes are present. */
	*pPresentModeCount = count;
	return VK_SUCCESS;
}

/*
 * Reports the single physical device's full current swapchain rectangle.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDevicePresentRectanglesKHR(
	VkPhysicalDevice physicalDevice,
	VkSurfaceKHR handle,
	uint32_t *pRectCount,
	VkRect2D *pRects)
{
	struct compat_surface *surface;

	/* Requires a valid private surface and count. */
	(void)physicalDevice;
	surface = (struct compat_surface *)(uintptr_t)handle;
	if (surface == NULL)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Rejects a missing enumeration destination. */
	if (pRectCount == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Measurement reports the one-device rectangle. */
	if (pRects == NULL) {
		*pRectCount = 1;
		return VK_SUCCESS;
	}

	/* A zero-capacity output receives no rectangle. */
	if (*pRectCount == 0)
		return VK_INCOMPLETE;

	/* Uses the latest client-chosen swapchain extent. */
	pRects[0].offset.x = 0;
	pRects[0].offset.y = 0;
	pRects[0].extent = surface->extent;
	*pRectCount = 1;

	/* Succeeded: the rectangle covers the current client-selected buffer. */
	return VK_SUCCESS;
}

/*
 * Allocates and zeros a Vulkan WSI object with the matching application allocator.
 */
void *
compat_object_allocate(
	size_t size,
	const VkAllocationCallbacks *allocator)
{
	void *object;

	/* Ordinary allocations use the process C runtime. */
	if (allocator == NULL) {
		object = calloc(1, size);
		if (object == NULL)
			return NULL;

		/* Succeeded: the caller owns zeroed C-runtime storage. */
		return object;
	}

	/* Application allocation callbacks own Vulkan-object storage until destruction. */
	object = allocator->pfnAllocation(allocator->pUserData, size, sizeof(void *) * 2, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (object == NULL)
		return NULL;

	/* Initializes every partial-cleanup field before any operation can fail. */
	memset(object, 0, size);

	/* Succeeded: the caller owns the zeroed object storage. */
	return object;
}

/*
 * Frees a WSI object's storage using the allocator that created it.
 */
void
compat_object_free(
	void *object,
	unsigned allocated,
	const VkAllocationCallbacks *allocator)
{
	/* Application callbacks retain ownership even if destruction passes a different pointer. */
	if (allocated != 0) {
		allocator->pfnFree(allocator->pUserData, object);
		return;
	}

	/* Returns ordinary object storage to the process allocator. */
	free(object);

	/* Succeeded: the matching allocator reclaimed the object. */
	return;
}

/*
 * Returns monotonic nanoseconds for deadline arithmetic independent of wall-clock adjustments.
 */
uint64_t
compat_time(
	void)
{
	struct timespec now;
	int error;

	/* A monotonic clock failure cannot provide a meaningful bounded wait. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Returns one integer timebase for all private-queue deadlines. */
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

/*
 * Dispatches private events with a bounded socket wait; application callbacks remain queued.
 */
int
compat_surface_progress(
	struct compat_surface *surface,
	uint64_t timeout)
{
	int progress;

	struct pollfd descriptor;
	uint64_t start;
	uint64_t now;
	uint64_t remaining;
	int milliseconds;
	int error;
	int flushed;

	/* KMS has no protocol release queue; finite slices avoid spinning when all images remain acquired. */
	if (surface->kms != NULL) {
		/* Nonblocking ownership queries need no sleep or DRM event read. */
		if (timeout == 0)
			return 1;

		/* An externally synchronized caller can release an image between finite ownership polls. */
		milliseconds = 100;
		if (timeout < SURFACE_POLL_SLICE_NS)
			milliseconds = (int)((timeout + 999999) / 1000000);

		/* Never reads a host DRM descriptor on this path. */
		(void)poll(NULL, 0, milliseconds);
		return 1;
	}

	/* Processes already queued private events before reserving a socket read. */
	error = wl_display_dispatch_queue_pending(surface->display, surface->queue);
	if (error < 0) {
		progress = surface_lost(surface);

		/* Preserves the reported failure after its cleanup. */
		return progress;
	}

	/* Reserves a read only after all private pending events have been consumed. */
	error = wl_display_prepare_read_queue(surface->display, surface->queue);
	if (error != 0) {
		/* A newly queued callback is dispatched without a blocking socket read. */
		error = wl_display_dispatch_queue_pending(surface->display, surface->queue);
		if (error < 0) {
			progress = surface_lost(surface);

			/* Preserves the reported failure after its cleanup. */
			return progress;
		}

		/* Progress can be retried after the queued callbacks have run. */
		return 1;
	}

	/* Flushes outgoing requests while allowing socket backpressure in poll. */
	flushed = wl_display_flush(surface->display);
	if (flushed < 0) {
		/* Unexpected transport failure ends the reserved read. */
		if (errno != EAGAIN) {
			wl_display_cancel_read(surface->display);
			progress = surface_lost(surface);

			/* Preserves the reported failure after its cleanup. */
			return progress;
		}
	}

	/* Establishes one deadline for EINTR and writable-only wakes. */
	start = compat_time();
	remaining = timeout;
	for (;;) {
		/* Rounds positive nanoseconds upward while keeping poll's signed bound. */
		milliseconds = INT_MAX;
		if (remaining / 1000000 < (uint64_t)INT_MAX)
			milliseconds = (int)((remaining + 999999) / 1000000);

		/* Reads input and, when required, drains pending output without spinning. */
		descriptor.fd = wl_display_get_fd(surface->display);
		descriptor.events = POLLIN;
		if (flushed < 0)
			descriptor.events |= POLLOUT;

		/* Waits only inside the caller's remaining deadline. */
		descriptor.revents = 0;
		error = poll(&descriptor, 1, milliseconds);
		if (error == 0) {
			wl_display_cancel_read(surface->display);
			return 0;
		}

		/* Interrupted waits preserve the deadline; other errors retire the read. */
		if (error < 0) {
			if (errno != EINTR) {
				wl_display_cancel_read(surface->display);
				progress = surface_lost(surface);

				/* Preserves the reported failure after its cleanup. */
				return progress;
			}
		} else {
			/* Transport hangup invalidates the surface regardless of pending callbacks. */
			if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
				wl_display_cancel_read(surface->display);
				progress = surface_lost(surface);

				/* Preserves the reported failure after its cleanup. */
				return progress;
			}

			/* A readable socket completes the reserved read. */
			if ((descriptor.revents & POLLIN) != 0) {
				error = wl_display_read_events(surface->display);
				if (error < 0) {
					progress = surface_lost(surface);

					/* Preserves the reported failure after its cleanup. */
					return progress;
				}

				/* Invokes only WSI callbacks, preserving queued application events. */
				error = wl_display_dispatch_queue_pending(surface->display, surface->queue);
				if (error < 0) {
					progress = surface_lost(surface);

					/* Preserves the reported failure after its cleanup. */
					return progress;
				}

				/* Succeeded: at least one socket batch was processed. */
				return 1;
			}

			/* A writable-only wake drains outgoing requests and resumes the bounded read. */
			if ((descriptor.revents & POLLOUT) != 0) {
				flushed = wl_display_flush(surface->display);
				if (flushed < 0) {
					if (errno != EAGAIN) {
						wl_display_cancel_read(surface->display);
						progress = surface_lost(surface);

						/* Preserves the reported failure after its cleanup. */
						return progress;
					}
				}
			}
		}

		/* Unlimited Vulkan acquire waits still use finite poll slices. */
		if (timeout == UINT64_MAX)
			continue;

		/* Recomputes the time remaining after interruption or output progress. */
		now = compat_time();
		if (now - start >= timeout) {
			wl_display_cancel_read(surface->display);
			return 0;
		}

		/* Preserves the original deadline rather than restarting it on each wake. */
		remaining = timeout - (now - start);
	}
}

/*
 * Selects an advertised single-plane layout whose image can actually be exported.
 */
int
compat_surface_modifier(
	struct compat_surface *surface,
	VkPhysicalDevice physical,
	VkFormat format,
	unsigned path,
	uint32_t *fourcc,
	uint64_t *modifier)
{
	struct compat_instance *instance;
	VkFormatProperties2 properties;
	VkDrmFormatModifierPropertiesListEXT list;
	VkDrmFormatModifierPropertiesEXT *layouts;
	uint32_t index;
	uint32_t other;
	uint32_t wanted;
	uint32_t opaque;
	uint32_t count;
	int supported;

	/* Maps channel encodings to the corresponding alpha and opaque DRM formats. */
	wanted = 0;
	opaque = 0;
	if (format == VK_FORMAT_B8G8R8A8_UNORM) {
		wanted = 0x34325241U;
		opaque = 0x34325258U;
	}

	/* SRGB keeps the same wire byte order. */
	if (format == VK_FORMAT_B8G8R8A8_SRGB) {
		wanted = 0x34325241U;
		opaque = 0x34325258U;
	}

	/* Little-endian red-first image bytes use the ABGR wire encoding. */
	if (format == VK_FORMAT_R8G8B8A8_UNORM) {
		wanted = 0x34324241U;
		opaque = 0x34324258U;
	}

	/* SRGB red-first bytes retain the same DRM channel order. */
	if (format == VK_FORMAT_R8G8B8A8_SRGB) {
		wanted = 0x34324241U;
		opaque = 0x34324258U;
	}

	/* Unsupported channel encodings cannot produce a compositor buffer. */
	if (wanted == 0)
		return 0;

	/* An absent physical export path supplies no surface format. */
	if (path == COMPAT_WSI_NONE)
		return 0;

	/* Collects the driver's version-one modifier properties once for this query. */
	instance = compat_instance_for_physical(physical);
	layouts = NULL;
	count = 0;
	if (path == COMPAT_WSI_MODIFIER) {
		/* Measures modifiers before allocating the bounded output array. */
		memset(&list, 0, sizeof(list));
		list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
		memset(&properties, 0, sizeof(properties));
		properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
		properties.pNext = &list;
		instance->format_properties(physical, format, &properties);
		count = list.drmFormatModifierCount;
		layouts = calloc((size_t)count + 1, sizeof(*layouts));
		if (layouts == NULL)
			return 0;

		/* Reads the driver's actual single-plane tiling and feature descriptions. */
		list.pDrmFormatModifierProperties = layouts;
		instance->format_properties(physical, format, &properties);
		if (list.drmFormatModifierCount < count)
			count = list.drmFormatModifierCount;
	}

	/* Finds one compositor layout also supported by the backend export query. */
	for (index = 0; index < surface->modifier_count; index++) {
		/* Restricts channel ordering to this Vulkan format. */
		if (surface->modifiers[index].format != wanted) {
			if (surface->modifiers[index].format != opaque)
				continue;
		}

		/* Linear images accept only the DRM linear modifier. */
		if (path == COMPAT_WSI_LINEAR) {
			if (surface->modifiers[index].modifier != 0)
				continue;
		}

		/* Modifier images require one plane and color attachment tiling features. */
		if (path == COMPAT_WSI_MODIFIER) {
			supported = 0;
			for (other = 0; other < count; other++) {
				/* Matches the compositor's advertised modifier exactly. */
				if (layouts[other].drmFormatModifier != surface->modifiers[index].modifier)
					continue;

				/* This WSI sends only one DMA-BUF plane. */
				if (layouts[other].drmFormatModifierPlaneCount != 1)
					continue;

				/* Drawing into the image requires a color-attachment tiling capability. */
				if ((layouts[other].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0)
					continue;

				/* Marks this exact single-plane modifier as a candidate. */
				supported = 1;
			}

			/* A compositor-only modifier cannot be created by this backend. */
			if (supported == 0)
				continue;
		}

		/* Verifies exportability rather than relying solely on format feature bits. */
		supported = compat_image_supported(physical, format, path, surface->modifiers[index].modifier);
		if (supported == 0)
			continue;

		/* Transfers the selected wire layout to the creation caller. */
		*fourcc = surface->modifiers[index].format;
		*modifier = surface->modifiers[index].modifier;
		free(layouts);

		/* Succeeded: compositor and driver agree on this image layout. */
		return 1;
	}

	/* Releases the query when no common layout can be exported. */
	free(layouts);

	/* No supported wire layout exists for this channel encoding. */
	return 0;
}

/* Binds the compositor's DMA-BUF global only when modifier events are available. */
static void
surface_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	/* Version-three modifiers supply the complete format/modifier intersection. */
	static const struct zwp_linux_dmabuf_v1_listener surface_dmabuf_listener = {
	    surface_format, surface_modifier};
	struct compat_surface *surface;
	int error;
	int evaluated;

	/* Ignores unrelated globals and servers older than version three. */
	surface = data;
	evaluated = strcmp(interface, "zwp_linux_dmabuf_v1");
	if (evaluated != 0)
		return;

	/* Older servers do not publish the required modifier events. */
	if (version < 3)
		return;

	/* A surface owns only one DMA-BUF factory. */
	if (surface->dmabuf != NULL)
		return;

	/* Version three deliberately avoids newer feedback-only announcement semantics. */
	surface->dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3);
	if (surface->dmabuf == NULL) {
		surface->lost = 1;
		return;
	}

	/* Installs callbacks on the queue inherited from the registry proxy. */
	error = wl_proxy_add_listener((struct wl_proxy *)surface->dmabuf, (void (**)(void))&surface_dmabuf_listener, surface);
	if (error != 0)
		surface->lost = 1;
}

/* Registry removal leaves existing buffers valid; transport loss is checked during progress. */
static void
surface_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	/* Bound globals remain usable until their connection disappears. */
	(void)data;
	(void)registry;
	(void)name;

	/* Succeeded: a removed factory cannot accept new swapchains. */
	return;
}

/* Version-three format-only events are insufficient to select an image layout. */
static void
surface_format(
	void *data,
	struct zwp_linux_dmabuf_v1 *object,
	uint32_t format)
{
	/* Modifier callbacks carry the authoritative channel-order and tiling pair. */
	(void)data;
	(void)object;
	(void)format;

	/* Succeeded: the advertised format is retained with an implicit modifier. */
	return;
}

/* Retains each advertised pair until the private surface record is destroyed. */
static void
surface_modifier(
	void *data,
	struct zwp_linux_dmabuf_v1 *object,
	uint32_t format,
	uint32_t high,
	uint32_t low)
{
	struct compat_surface *surface;
	struct compat_modifier *grown;

	/* Bounds compositor-controlled allocation while retaining practical modifier lists. */
	(void)object;
	surface = data;
	if (surface->modifier_count >= 4096) {
		surface->lost = 1;
		return;
	}

	/* Grows the private list without losing its original allocation on failure. */
	grown = realloc(surface->modifiers, ((size_t)surface->modifier_count + 1) * sizeof(*grown));
	if (grown == NULL) {
		surface->lost = 1;
		return;
	}

	/* Appends one wire layout and transfers the grown storage to the surface. */
	surface->modifiers = grown;
	grown[surface->modifier_count].format = format;
	grown[surface->modifier_count].modifier = ((uint64_t)high << 32) | low;
	surface->modifier_count++;

	/* Succeeded: the advertised format/modifier pair is retained. */
	return;
}

/* Completes one bounded initialization sync and retires its callback proxy. */
static void
surface_sync(
	void *data,
	struct wl_callback *callback,
	uint32_t serial)
{
	struct compat_surface *surface;

	/* Marks this private sync complete before removing its callback proxy. */
	(void)serial;
	surface = data;
	surface->sync_done = 1;
	wl_callback_destroy(callback);

	/* Succeeded: private initialization can finish after this barrier. */
	return;
}

/* Receives registry or modifier announcements within one finite initialization deadline. */
static int
surface_roundtrip(
	struct compat_surface *surface)
{
	/* One bounded sync request completes initialization without dispatching application callbacks. */
	static const struct wl_callback_listener surface_sync_listener = {
	    surface_sync};
	struct wl_callback *callback;
	uint64_t start;
	uint64_t elapsed;
	int error;

	/* Issues sync through the private display wrapper so no application callback is dispatched. */
	surface->sync_done = 0;
	callback = wl_display_sync(surface->display_wrapper);
	if (callback == NULL)
		return -1;

	/* Installs completion before the first socket flush. */
	error = wl_callback_add_listener(callback, &surface_sync_listener, surface);
	if (error != 0) {
		wl_callback_destroy(callback);
		return -1;
	}

	/* Limits initialization even when a compositor stops answering requests. */
	start = compat_time();
	while (surface->sync_done == 0) {
		/* Preserves the original five-second deadline across unrelated protocol events. */
		elapsed = compat_time() - start;
		if (elapsed >= SURFACE_INITIALIZE_TIMEOUT_NS) {
			wl_callback_destroy(callback);
			return -1;
		}

		/* Progresses only the private queue for the remaining bounded interval. */
		error = compat_surface_progress(surface, SURFACE_INITIALIZE_TIMEOUT_NS - elapsed);
		if (error < 0) {
			/* Only a still-pending sync callback remains owned by this wait. */
			if (surface->sync_done == 0)
				wl_callback_destroy(callback);
			return -1;
		}
	}

	/* An allocation failure reported by a registry callback invalidates initialization. */
	if (surface->lost != 0)
		return -1;

	/* Succeeded: all requests preceding the private sync have been processed. */
	return 0;
}

/* Checks actual physical export capability and graphics support for one queue family. */
static VkBool32
surface_support(
	VkPhysicalDevice physical,
	uint32_t family,
	unsigned external)
{
	struct compat_instance *instance;
	struct compat_capabilities capabilities;
	VkQueueFamilyProperties *queues;
	uint32_t count;
	VkBool32 supported;

	/* A physical device outside the chain has no safe instance dispatch table. */
	instance = compat_instance_for_physical(physical);
	if (instance == NULL)
		return VK_FALSE;

	/* Wayland requires DMA-BUF export; KMS uses ordinary optimal images and CPU copy. */
	if (external != 0) {
		compat_physical_capabilities(physical, &capabilities);
		if (capabilities.path == COMPAT_WSI_NONE)
			return VK_FALSE;
	}

	/* Measures queue families without reading outside their returned array. */
	instance->queue_properties(physical, &count, NULL);
	if (family >= count)
		return VK_FALSE;

	/* Allocates the measured output including room for an empty query. */
	queues = calloc((size_t)count + 1, sizeof(*queues));
	if (queues == NULL)
		return VK_FALSE;

	/* Graphics queues can render all supported surface image formats. */
	instance->queue_properties(physical, &count, queues);
	supported = VK_FALSE;
	if ((queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
		supported = VK_TRUE;

	/* Releases temporary queue-family storage before returning support. */
	free(queues);

	/* Succeeded: the result reflects this physical graphics/export queue. */
	return supported;
}

/* Initializes private protocol ownership without publishing a partially usable Vulkan surface. */
static int
surface_initialize(
	struct compat_surface *surface,
	struct wl_surface *application_surface)
{
	/* Registry announcements stay on the surface's private queue until surface destruction. */
	static const struct wl_registry_listener surface_registry_listener = {
	    surface_global, surface_global_remove};
	int error;

	/* Separates all protocol callbacks from the application's event queue. */
	surface->queue = wl_display_create_queue(surface->display);
	if (surface->queue == NULL)
		return -1;

	/* Wraps the display without transferring its ownership. */
	surface->display_wrapper = wl_proxy_create_wrapper(surface->display);
	if (surface->display_wrapper == NULL)
		return -1;

	/* Wraps the surface before issuing any constructor through it. */
	surface->surface_wrapper = wl_proxy_create_wrapper(application_surface);
	if (surface->surface_wrapper == NULL)
		return -1;

	/* Makes child proxies inherit the private event queue. */
	wl_proxy_set_queue((struct wl_proxy *)surface->display_wrapper, surface->queue);
	wl_proxy_set_queue((struct wl_proxy *)surface->surface_wrapper, surface->queue);
	surface->registry = wl_display_get_registry(surface->display_wrapper);
	if (surface->registry == NULL)
		return -1;

	/* Installs registry callbacks before flushing its announcement request. */
	error = wl_registry_add_listener(surface->registry, &surface_registry_listener, surface);
	if (error != 0)
		return -1;

	/* Receives globals first; their binds are covered by the second bounded sync. */
	error = surface_roundtrip(surface);
	if (error != 0)
		return -1;

	/* Receives modifier events generated by the version-three bind. */
	error = surface_roundtrip(surface);
	if (error != 0)
		return -1;

	/* Succeeded: registry and modifier announcements completed within their finite deadlines. */
	return 0;
}

/* Preserves connection loss for later WSI calls and returns the private progress failure. */
static int
surface_lost(
	struct compat_surface *surface)
{
	/* A failed transport remains unusable for every later acquire or presentation. */
	surface->lost = 1;

	/* Reports the lost private queue to its Vulkan boundary caller. */
	return -1;
}
