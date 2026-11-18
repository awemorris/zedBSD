/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exported Vulkan images, compositor release ownership, and implicit-sync presentation. */
#include "compat.h"
#include "dma-sync.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Bounds each protocol or device wait with the same monotonic time units. */
#define CHAIN_RELEASE_TIMEOUT_NS UINT64_C(100000000)

static VkResult chain_image_create(struct compat_swapchain *chain, uint32_t index, const VkSwapchainCreateInfoKHR *create);
static void chain_free(struct compat_swapchain *chain);
static void chain_gpu_free(struct compat_swapchain *chain);
static void chain_release(void *data, struct wl_buffer *buffer);
static void chain_frame(void *data, struct wl_callback *callback, uint32_t serial);
static VkResult chain_commands(struct compat_swapchain *chain, struct compat_queue *queue);
static VkResult chain_acquire_signal(struct compat_swapchain *chain, uint32_t index, VkSemaphore semaphore, VkFence fence);
static VkResult chain_present(struct compat_swapchain *chain, struct compat_queue *queue, uint32_t index, uint32_t wait_count, const VkSemaphore *waits);
static VkResult chain_record(struct compat_swapchain *chain, uint32_t index, unsigned acquire, VkCommandBuffer command);
static VkResult chain_display_image(struct compat_swapchain *chain, uint32_t index, const VkSwapchainCreateInfoKHR *create);
static VkResult chain_display_readback(struct compat_swapchain *chain);

/*
 * Creates exportable images and their compositor buffers without using backend WSI.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSwapchainKHR(
	VkDevice device,
	const VkSwapchainCreateInfoKHR *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkSwapchainKHR *pSwapchain)
{
	VkResult submission;
	struct compat_device *owner;
	struct compat_surface *surface;
	struct compat_swapchain *chain;
	VkResult error;
	uint32_t count;
	uint32_t index;
	int supported;

	/* Requires the application's swapchain-enabled device. */
	owner = compat_device_get(device);
	if (owner == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses a device whose application never enabled our WSI. */
	if (owner->swapchain == 0)
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Requires valid creation and output destinations. */
	if (pCreateInfo == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Rejects unreachable output storage. */
	if (pSwapchain == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires the version-three DMA-BUF factory owned by the surface. */
	surface = (struct compat_surface *)(uintptr_t)pCreateInfo->surface;
	if (surface == NULL)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* KMS surfaces allocate ordinary optimal images and portable CPU scanout storage. */
	if (surface->kms != NULL) {
		submission = compat_display_swapchain_create(owner, pCreateInfo, pAllocator, pSwapchain);
		if (submission != VK_SUCCESS)
			return submission;

		/* Succeeded: the application owns its display swapchain. */
		return VK_SUCCESS;
	}

	/* An older or absent protocol is diagnosed once per failed creation. */
	if (surface->dmabuf == NULL) {
		fprintf(stderr, "libvulkan-compat: compositor has no version-three linux-dmabuf\n");
		return VK_ERROR_SURFACE_LOST_KHR;
	}

	/* A disconnected connection cannot accept a new buffer. */
	if (surface->lost != 0)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Supports only the two advertised present modes. */
	if (pCreateInfo->presentMode != VK_PRESENT_MODE_FIFO_KHR) {
		if (pCreateInfo->presentMode != VK_PRESENT_MODE_MAILBOX_KHR)
			return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Reserves enough images for FIFO or MAILBOX while honoring the requested minimum. */
	count = 3;
	if (pCreateInfo->presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
		count = 4;

	/* The bounded image array has the same maximum advertised in capabilities. */
	if (pCreateInfo->minImageCount > count)
		count = pCreateInfo->minImageCount;

	/* Refuses invalid counts before allocating private resources. */
	if (count > 8)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Allocates the Vulkan object's storage with the matching application callback. */
	chain = compat_object_allocate(sizeof(*chain), pAllocator);
	if (chain == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Initializes resource ownership before any image creation can fail. */
	chain->surface = surface;
	chain->device = owner;
	chain->count = count;
	chain->format = pCreateInfo->imageFormat;
	chain->extent = pCreateInfo->imageExtent;
	chain->mode = pCreateInfo->presentMode;
	chain->path = owner->path;
	chain->fallback = 0;
	if (owner->implicit_sync == 0)
		chain->fallback = 1;
	if (pCreateInfo->imageSharingMode == VK_SHARING_MODE_EXCLUSIVE)
		chain->foreign = owner->foreign;
	if (pAllocator != NULL) {
		chain->allocator = *pAllocator;
		chain->allocated = 1;
	}

	/* Every unopened descriptor must remain distinguishable from descriptor zero. */
	for (index = 0; index < count; index++) {
		/* A partially created image can always use the common destructor. */
		chain->images[index].fd = -1;
		chain->images[index].chain = chain;
	}

	/* Selects a single-plane compositor/driver modifier intersection. */
	supported = compat_surface_modifier(surface, owner->physical, chain->format, chain->path, &chain->fourcc, &chain->modifier);
	if (supported == 0) {
		/* An available linear export may serve a compositor without the preferred modifier. */
		chain->path = COMPAT_WSI_LINEAR;
		supported = compat_surface_modifier(surface, owner->physical, chain->format, chain->path, &chain->fourcc, &chain->modifier);
	}

	/* No common export format means no usable swapchain. */
	if (supported == 0) {
		chain_free(chain);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* Creates each exported image before publishing the chain handle. */
	for (index = 0; index < count; index++) {
		/* Partial failures retire every earlier image and descriptor. */
		error = chain_image_create(chain, index, pCreateInfo);
		if (error != VK_SUCCESS) {
			chain_free(chain);
			return error;
		}
	}

	/* Keeps callback records of previously destroyed chains alive until their buffers release. */
	compat_swapchain_collect(surface, 0);
	surface->extent = chain->extent;
	*pSwapchain = (VkSwapchainKHR)(uintptr_t)chain;

	/* Succeeded: the application owns the new chain and destroys oldSwapchain itself. */
	return VK_SUCCESS;
}

/*
 * Defers busy compositor buffers while immediately retiring an idle swapchain.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR(
	VkDevice device,
	VkSwapchainKHR swapchain,
	const VkAllocationCallbacks *pAllocator)
{
	struct compat_swapchain *chain;

	/* A null handle has no images or protocol callbacks. */
	(void)device;
	(void)pAllocator;
	chain = (struct compat_swapchain *)(uintptr_t)swapchain;
	if (chain == NULL)
		return;

	/* Backend resources cannot retire while our final private submission remains in flight. */
	if (chain->queue != VK_NULL_HANDLE)
		(void)chain->device->queue_idle(chain->queue);

	/* Vulkan resource retirement must precede a later device destruction even if release is delayed. */
	chain_gpu_free(chain);

	/* Moves callback storage to the surface's deferred collection list. */
	chain->retired = 1;
	chain->next = chain->surface->retired;
	chain->surface->retired = chain;

	/* Receives already available release events without waiting on the compositor. */
	(void)compat_surface_progress(chain->surface, 0);
	compat_swapchain_collect(chain->surface, 0);

	/* Succeeded: GPU ownership retired and callbacks remain on the surface retirement list. */
	return;
}

/*
 * Enumerates the unchanged backend image handles allocated for the private swapchain.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetSwapchainImagesKHR(
	VkDevice device,
	VkSwapchainKHR swapchain,
	uint32_t *pSwapchainImageCount,
	VkImage *pSwapchainImages)
{
	struct compat_swapchain *chain;
	uint32_t count;
	uint32_t index;
	VkResult result;

	/* Requires a live chain and the caller's count destination. */
	(void)device;
	chain = (struct compat_swapchain *)(uintptr_t)swapchain;
	if (chain == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses an absent count before accessing its input capacity. */
	if (pSwapchainImageCount == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Measurement returns the actual bounded image count. */
	if (pSwapchainImages == NULL) {
		*pSwapchainImageCount = chain->count;
		return VK_SUCCESS;
	}

	/* Copies only the caller's capacity and reports incomplete enumeration. */
	count = chain->count;
	result = VK_SUCCESS;
	if (*pSwapchainImageCount < count) {
		count = *pSwapchainImageCount;
		result = VK_INCOMPLETE;
	}

	/* Dispatchable and image backend handles are never wrapped. */
	for (index = 0; index < count; index++) {
		/* Exposes one backend-owned image allocated with DMA-BUF export capability. */
		pSwapchainImages[index] = chain->images[index].image;
	}

	/* Returns the number actually written, rather than the total available. */
	*pSwapchainImageCount = count;

	/* Reports an incomplete image enumeration when the array is too small. */
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the caller received every swapchain image. */
	return VK_SUCCESS;
}

/*
 * Acquires only an unused or compositor-released image and honors the caller's exact deadline.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImageKHR(
	VkDevice device,
	VkSwapchainKHR swapchain,
	uint64_t timeout,
	VkSemaphore semaphore,
	VkFence fence,
	uint32_t *pImageIndex)
{
	struct compat_swapchain *chain;
	uint64_t start;
	uint64_t elapsed;
	uint64_t remaining;
	uint32_t index;
	int progress;
	VkResult error;

	/* Requires a live chain and an image-index destination. */
	(void)device;
	chain = (struct compat_swapchain *)(uintptr_t)swapchain;
	if (chain == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Rejects an absent output before any semaphore is signaled. */
	if (pImageIndex == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A retired chain cannot hand out new images. */
	if (chain->retired != 0)
		return VK_ERROR_OUT_OF_DATE_KHR;

	/* Establishes one deadline across all private protocol events. */
	start = compat_time();
	for (;;) {
		/* Receives pending release events without blocking before examining ownership. */
		progress = compat_surface_progress(chain->surface, 0);
		if (progress < 0)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* Reclaims callback records whose old compositor buffers are now idle. */
		compat_swapchain_collect(chain->surface, 0);
		for (index = 0; index < chain->count; index++) {
			/* Busy buffers remain exclusively readable by the compositor. */
			if (chain->images[index].busy != 0)
				continue;

			/* Already acquired buffers remain owned by the application's current frame. */
			if (chain->images[index].acquired != 0)
				continue;

			/* Imports compositor completion or signals an empty backend submission. */
			error = chain_acquire_signal(chain, index, semaphore, fence);
			if (error != VK_SUCCESS)
				return error;

			/* Transfers this idle image to the application exactly once. */
			chain->images[index].acquired = 1;
			*pImageIndex = index;

			/* Succeeded: synchronization protects the application's next image writes. */
			return VK_SUCCESS;
		}

		/* Zero timeout is a nonblocking availability query. */
		if (timeout == 0)
			return VK_NOT_READY;

		/* Finite waits preserve the original nanosecond deadline. */
		remaining = UINT64_MAX;
		if (timeout != UINT64_MAX) {
			elapsed = compat_time() - start;
			if (elapsed >= timeout)
				return VK_TIMEOUT;

			/* Only the still-unspent interval reaches the socket wait. */
			remaining = timeout - elapsed;
		}

		/* Waits solely for the private event queue's buffer release callbacks. */
		progress = compat_surface_progress(chain->surface, remaining);
		if (progress < 0)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* A timed-out socket wait cannot fabricate a free image. */
		if (progress == 0)
			return VK_TIMEOUT;
	}
}

/*
 * Uses the single-device acquire contract while rejecting unsupported device masks.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImage2KHR(
	VkDevice device,
	const VkAcquireNextImageInfoKHR *pAcquireInfo,
	uint32_t *pImageIndex)
{
	VkResult submission;

	/* The supported device group contains only physical device zero. */
	if (pAcquireInfo == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* No second physical device can own a swapchain image. */
	if (pAcquireInfo->deviceMask != 1)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Returns the same bounded acquire and synchronization result. */
	submission = vkAcquireNextImageKHR(device, pAcquireInfo->swapchain, pAcquireInfo->timeout, pAcquireInfo->semaphore, pAcquireInfo->fence, pImageIndex);
	if (submission != VK_SUCCESS)
		return submission;

	/* Succeeded: the acquired image and its requested synchronization are ready. */
	return VK_SUCCESS;
}

/*
 * Presents each private chain and reports its individual result as well as the aggregate result.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(
	VkQueue queue,
	const VkPresentInfoKHR *pPresentInfo)
{
	struct compat_queue *owner;
	struct compat_swapchain *chain;
	VkResult result;
	VkResult error;
	uint32_t index;
	uint32_t wait_count;

	/* Requires a recorded backend queue and a valid presentation description. */
	owner = compat_queue_get(queue);
	if (owner == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses missing image and chain arrays before submitting any work. */
	if (pPresentInfo == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Queue order shares the one application semaphore wait across all presented chains. */
	result = VK_SUCCESS;
	wait_count = pPresentInfo->waitSemaphoreCount;
	for (index = 0; index < pPresentInfo->swapchainCount; index++) {
		/* Presentation operates only on this device's own swapchain storage. */
		chain = (struct compat_swapchain *)(uintptr_t)pPresentInfo->pSwapchains[index];
		error = VK_ERROR_INITIALIZATION_FAILED;
		if (chain != NULL) {
			/* A backend queue cannot present images belonging to another device. */
			if (chain->device == owner->device)
				error = chain_present(chain, owner, pPresentInfo->pImageIndices[index], wait_count, pPresentInfo->pWaitSemaphores);
		}

		/* A successful first submission consumes the shared wait semaphores once. */
		if (error == VK_SUCCESS)
			wait_count = 0;

		/* Supplies per-chain outcomes when the application requests them. */
		if (pPresentInfo->pResults != NULL)
			pPresentInfo->pResults[index] = error;

		/* Preserves the first failure in the aggregate return. */
		if (result == VK_SUCCESS)
			result = error;
	}

	/* Reports the first failed presentation; per-chain results remain available. */
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: every requested swapchain was presented. */
	return VK_SUCCESS;
}

/*
 * Reports a group containing exactly one local physical device.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetDeviceGroupPresentCapabilitiesKHR(
	VkDevice device,
	VkDeviceGroupPresentCapabilitiesKHR *pCapabilities)
{
	struct compat_device *queried_device;

	/* Requires the application's swapchain-enabled device and output storage. */
	queried_device = compat_device_get(device);
	if (queried_device == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses a missing capabilities destination. */
	if (pCapabilities == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Preserves the caller's sType and pNext while replacing only the defined output fields. */
	memset(pCapabilities->presentMask, 0, sizeof(pCapabilities->presentMask));
	pCapabilities->presentMask[0] = 1;
	pCapabilities->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;

	/* Succeeded: only local single-device presentation is supported. */
	return VK_SUCCESS;
}

/*
 * Reports the same local-only group contract for one surface.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetDeviceGroupSurfacePresentModesKHR(
	VkDevice device,
	VkSurfaceKHR surface,
	VkDeviceGroupPresentModeFlagsKHR *pModes)
{
	struct compat_device *queried_device;

	/* Requires known device and surface ownership. */
	queried_device = compat_device_get(device);
	if (queried_device == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A null private surface has no group presentation modes. */
	if (surface == VK_NULL_HANDLE)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Refuses a missing output destination. */
	if (pModes == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Returns the supported single-device mode. */
	*pModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;

	/* Succeeded: no multi-device image sharing is claimed. */
	return VK_SUCCESS;
}

/*
 * Frees retired callback records only after every compositor buffer has released, or surface destruction.
 */
void
compat_swapchain_collect(
	struct compat_surface *surface,
	unsigned force)
{
	struct compat_swapchain **link;
	struct compat_swapchain *chain;
	uint32_t index;
	unsigned busy;

	/* Keeps unreleased callback storage reachable from its live surface. */
	link = &surface->retired;
	while (*link != NULL) {
		/* Determines whether any compositor still retains this chain's images. */
		chain = *link;
		busy = 0;
		for (index = 0; index < chain->count; index++) {
			/* A release callback alone returns compositor ownership. */
			if (chain->images[index].busy != 0)
				busy = 1;
		}

		/* Surface destruction retires proxies so queued callbacks cannot access freed storage. */
		if (force != 0)
			busy = 0;

		/* Preserves an unreleased chain for the next private-queue progress point. */
		if (busy != 0) {
			link = &chain->next;
			continue;
		}

		/* Removes an idle chain before freeing all of its GPU and protocol objects. */
		*link = chain->next;
		chain_free(chain);
	}
}

/*
 * Creates a display copy swapchain whose rendering images need no external-memory extension.
 */
VkResult
compat_display_swapchain_create(
	struct compat_device *device,
	const VkSwapchainCreateInfoKHR *create,
	const VkAllocationCallbacks *allocator,
	VkSwapchainKHR *swapchain)
{
	struct compat_surface *surface;
	struct compat_swapchain *chain;
	struct compat_swapchain *old;
	VkResult error;
	uint32_t index;
	uint32_t count;

	/* Requires the advertised KMS FIFO and blue-first storage contract. */
	surface = (struct compat_surface *)(uintptr_t)create->surface;
	if (create->presentMode != VK_PRESENT_MODE_FIFO_KHR)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Scanout byte ordering is XRGB/BGRA regardless of transfer-function interpretation. */
	if (create->imageFormat != VK_FORMAT_B8G8R8A8_UNORM) {
		if (create->imageFormat != VK_FORMAT_B8G8R8A8_SRGB)
			return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* The copy path requires an exact full-plane image extent. */
	if (create->imageExtent.width != surface->extent.width)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Both visible dimensions must match the selected timing. */
	if (create->imageExtent.height != surface->extent.height)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Keeps the same minimum-three bounded image ring as the FIFO Wayland path. */
	count = create->minImageCount;
	if (count < 3)
		count = 3;

	/* No image count exceeds the advertised fixed maximum. */
	if (count > 8)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Allocates callback-independent Vulkan object storage through the caller's allocator. */
	chain = compat_object_allocate(sizeof(*chain), allocator);
	if (chain == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Initializes every field required by shared partial-resource cleanup. */
	chain->surface = surface;
	chain->device = device;
	chain->extent = create->imageExtent;
	chain->format = create->imageFormat;
	chain->mode = VK_PRESENT_MODE_FIFO_KHR;
	chain->fallback = 1;
	chain->count = count;
	chain->dumb[0].fd = -1;
	chain->dumb[1].fd = -1;
	if (allocator != NULL) {
		chain->allocator = *allocator;
		chain->allocated = 1;
	}

	/* Display images own no external descriptor or Wayland release callback. */
	for (index = 0; index < count; index++) {
		/* The shared image destructor must never close process descriptor zero. */
		chain->images[index].fd = -1;
		chain->images[index].chain = chain;
	}

	/* Root applications acquire directly, while an explicit seat acquisition remains owned by the seat's lifecycle. */
	if (surface->kms->master_fd < 0) {
		error = compat_kms_acquire(surface->kms, -1);
		if (error != VK_SUCCESS) {
			chain_free(chain);
			return error;
		}

		/* This chain must release the ownership it acquired itself on destruction. */
		chain->master_owned = 1;
	}

	/* Ordinary optimal images are copied out after rendering instead of exported to the display driver. */
	for (index = 0; index < count; index++) {
		/* Partial allocation failure uses the same device-before-callback cleanup as Wayland. */
		error = chain_display_image(chain, index, create);
		if (error != VK_SUCCESS) {
			chain_free(chain);
			return error;
		}
	}

	/* Creates one tightly packed, coherent CPU readback allocation for the synchronous present copy. */
	error = chain_display_readback(chain);
	if (error != VK_SUCCESS) {
		chain_free(chain);
		return error;
	}

	/* Double-buffered scanout never overwrites the framebuffer still displayed by KMS. */
	for (index = 0; index < 2; index++) {
		/* Each dumb buffer retains its own creating file across later master-descriptor replacement. */
		error = compat_kms_dumb_create(surface->kms, chain->extent, &chain->dumb[index]);
		if (error != VK_SUCCESS) {
			chain_free(chain);
			return error;
		}
	}

	/* A replacement inherits automatic master ownership only after successful resource creation. */
	old = (struct compat_swapchain *)(uintptr_t)create->oldSwapchain;
	if (old != NULL) {
		/* Explicit seat acquisition remains owned by its independent lifecycle. */
		if (old->master_owned != 0) {
			chain->master_owned = 1;
			old->master_owned = 0;
		}
	}

	/* Publishes the new chain only after every render, readback and scanout resource exists. */
	*swapchain = (VkSwapchainKHR)(uintptr_t)chain;

	/* Succeeded: the first present performs the modeset, and later presents perform bounded flips. */
	return VK_SUCCESS;
}

/*
 * Restores scanout before removing dumb buffers, then retires the coherent copy allocation.
 */
void
compat_display_chain_free(
	struct compat_swapchain *chain)
{
	struct compat_device *device;
	struct compat_display *display;
	uint32_t index;

	/* An old chain cannot restore the console over a newer chain's active scanout. */
	device = chain->device;
	display = chain->surface->kms;
	if (display->scanout_chain == chain)
		compat_kms_restore(display);

	/* All scanout references have retired before framebuffer and dumb-handle removal. */
	for (index = 0; index < 2; index++) {
		/* A failed partial creation may own only its duplicated file or dumb handle. */
		compat_kms_dumb_destroy(display, &chain->dumb[index]);
	}

	/* Unmaps the backend readback allocation before freeing its memory. */
	if (chain->readback_mapping != NULL) {
		device->unmap_memory(device->handle, chain->readback_memory);
		chain->readback_mapping = NULL;
	}

	/* The buffer retires before the memory that backs it. */
	if (chain->readback != VK_NULL_HANDLE) {
		device->destroy_buffer(device->handle, chain->readback, NULL);
		chain->readback = VK_NULL_HANDLE;
	}

	/* Partial allocation failure may have no readback memory to retire. */
	if (chain->readback_memory != VK_NULL_HANDLE) {
		device->free_memory(device->handle, chain->readback_memory, NULL);
		chain->readback_memory = VK_NULL_HANDLE;
	}

	/* Direct applications drop only the display ownership acquired by this chain. */
	if (chain->master_owned != 0) {
		compat_kms_release(display);
		chain->master_owned = 0;
	}
}

/* Creates one dedicated exportable image and its immediate one-plane Wayland buffer. */
static VkResult
chain_image_create(
	struct compat_swapchain *chain,
	uint32_t index,
	const VkSwapchainCreateInfoKHR *create)
{
	/* Release callbacks own no buffer storage and stay valid until the retired chain is collected. */
	static const struct wl_buffer_listener chain_buffer_listener = {
	    chain_release};
	struct compat_device *device;
	struct compat_image *image;
	struct zwp_linux_buffer_params_v1 *params;
	VkExternalMemoryImageCreateInfo external;
	VkImageDrmFormatModifierListCreateInfoEXT modifier;
	VkImageCreateInfo info;
	VkImageMemoryRequirementsInfo2 query;
	VkMemoryRequirements2 requirements;
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryDedicatedAllocateInfo dedicated;
	VkExportMemoryAllocateInfo export;
	VkMemoryAllocateInfo allocation;
	VkMemoryGetFdInfoKHR fd_info;
	VkImageDrmFormatModifierPropertiesEXT actual;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkResult error;
	uint32_t type;
	int sent;
	int flags;

	/* Creates precisely the selected external-memory tiling with the application's usage. */
	device = chain->device;
	image = &chain->images[index];
	memset(&external, 0, sizeof(external));
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	memset(&modifier, 0, sizeof(modifier));
	modifier.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
	modifier.drmFormatModifierCount = 1;
	modifier.pDrmFormatModifiers = &chain->modifier;
	if (chain->path == COMPAT_WSI_MODIFIER)
		external.pNext = &modifier;

	/* The external-memory description belongs only to this private image creation. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.pNext = &external;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = chain->format;
	info.extent.width = chain->extent.width;
	info.extent.height = chain->extent.height;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_LINEAR;
	if (chain->path == COMPAT_WSI_MODIFIER)
		info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

	/* Preserves usage and queue sharing requested by the application. */
	info.usage = create->imageUsage;
	info.sharingMode = create->imageSharingMode;
	info.queueFamilyIndexCount = create->queueFamilyIndexCount;
	info.pQueueFamilyIndices = create->pQueueFamilyIndices;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	error = device->create_image(device->handle, &info, NULL, &image->image);
	if (error != VK_SUCCESS)
		return error;

	/* Queries the external image's exact memory requirements through the effective API spelling. */
	memset(&query, 0, sizeof(query));
	query.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
	query.image = image->image;
	memset(&requirements, 0, sizeof(requirements));
	requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
	device->image_requirements(device->handle, &query, &requirements);
	device->instance->memory_properties(device->physical, &properties);

	/* Selects any allowed device memory type; CPU mapping is the compositor's responsibility. */
	for (type = 0; type < properties.memoryTypeCount; type++) {
		/* The backend memory type mask supplies the required compatibility. */
		if ((requirements.memoryRequirements.memoryTypeBits & (1U << type)) != 0)
			break;
	}

	/* A missing compatible memory type cannot back this exported image. */
	if (type == properties.memoryTypeCount)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* Chains dedicated allocation and DMA-BUF export for this exact image. */
	memset(&dedicated, 0, sizeof(dedicated));
	dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	dedicated.image = image->image;
	memset(&export, 0, sizeof(export));
	export.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
	export.pNext = &dedicated;
	export.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.pNext = &export;
	allocation.allocationSize = requirements.memoryRequirements.size;
	allocation.memoryTypeIndex = type;
	error = device->allocate_memory(device->handle, &allocation, NULL, &image->memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the dedicated allocation before asking the driver to export it. */
	error = device->bind_image(device->handle, image->image, image->memory, 0);
	if (error != VK_SUCCESS)
		return error;

	/* Exports the descriptor retained for all implicit synchronization ioctls. */
	memset(&fd_info, 0, sizeof(fd_info));
	fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
	fd_info.memory = image->memory;
	fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	error = device->memory_fd(device->handle, &fd_info, &image->fd);
	if (error != VK_SUCCESS)
		return error;

	/* Prevents descriptors from leaking into later application launches. */
	flags = fcntl(image->fd, F_GETFD);
	if (flags < 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Exported descriptors are not assumed to have close-on-exec set by the driver. */
	flags = fcntl(image->fd, F_SETFD, flags | FD_CLOEXEC);
	if (flags < 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Reads the actual modifier and the physical memory plane's offset and stride. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	if (chain->path == COMPAT_WSI_MODIFIER) {
		/* The selected one-plane modifier is verified against the actual created image. */
		memset(&actual, 0, sizeof(actual));
		actual.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
		error = device->image_modifier(device->handle, image->image, &actual);
		if (error != VK_SUCCESS)
			return error;

		/* A driver-selected different modifier cannot match the compositor's advertised layout. */
		if (actual.drmFormatModifier != chain->modifier)
			return VK_ERROR_FORMAT_NOT_SUPPORTED;

		/* Modifier memory planes use their explicit memory aspect. */
		subresource.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
	}

	/* The protocol uses 32-bit plane offsets and strides. */
	device->image_layout(device->handle, image->image, &subresource, &layout);
	if (layout.offset > UINT32_MAX)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Oversized strides cannot be represented in the version-three wire request. */
	if (layout.rowPitch > UINT32_MAX)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Creates one short-lived plane description on the private event queue. */
	params = compat_dmabuf_params(chain->surface->dmabuf);
	if (params == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Transmits a duplicated descriptor and retains the original for synchronization. */
	sent = fcntl(image->fd, F_DUPFD_CLOEXEC, 0);
	if (sent < 0) {
		compat_params_destroy(params);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* The marshaller keeps its own transmission descriptor, so this duplicate can close now. */
	compat_params_add(params, sent, (uint32_t)layout.offset, (uint32_t)layout.rowPitch, chain->modifier);
	(void)close(sent);
	image->buffer = compat_params_buffer(params, (int)chain->extent.width, (int)chain->extent.height, chain->fourcc);
	compat_params_destroy(params);
	if (image->buffer == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Installs release ownership before the first buffer can be committed. */
	flags = wl_buffer_add_listener(image->buffer, &chain_buffer_listener, image);
	if (flags != 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Succeeded: this image owns backend memory, one descriptor and one private buffer proxy. */
	return VK_SUCCESS;
}

/* Retires every fully or partially created resource before freeing callback storage. */
static void
chain_free(
	struct compat_swapchain *chain)
{
	struct compat_image *image;
	uint32_t index;

	/* Protocol objects retire before their callback data or exported memory disappears. */
	for (index = 0; index < chain->count; index++) {
		/* Removes this image's release callback proxy before releasing its record. */
		image = &chain->images[index];
		if (image->buffer != NULL)
			wl_buffer_destroy(image->buffer);

		/* The retained descriptor belongs solely to this image. */
		if (image->fd >= 0)
			(void)close(image->fd);
	}

	/* Partially initialized chains may still own Vulkan resources. */
	chain_gpu_free(chain);

	/* Returns Vulkan-object storage to its creating allocator. */
	compat_object_free(chain, chain->allocated, &chain->allocator);

	/* Succeeded: all swapchain allocations and descriptors have retired. */
	return;
}

/* A compositor release transfers this image from display ownership to reusable idle storage. */
static void
chain_release(
	void *data,
	struct wl_buffer *buffer)
{
	struct compat_image *image;

	/* Callback storage remains on its surface until after this proxy is retired. */
	(void)buffer;
	image = data;
	image->busy = 0;

	/* Succeeded: the released image is available for acquisition. */
	return;
}

/* Completes and retires the current FIFO pacing callback. */
static void
chain_frame(
	void *data,
	struct wl_callback *callback,
	uint32_t serial)
{
	struct compat_surface *surface;

	/* Clears pacing ownership before removing the completed callback. */
	(void)serial;
	surface = data;
	surface->frame = NULL;
	wl_callback_destroy(callback);

	/* Succeeded: the completed frame no longer blocks FIFO presentation. */
	return;
}

/* Creates private command buffers and reusable completion objects for one presentation queue family. */
static VkResult
chain_commands(
	struct compat_swapchain *chain,
	struct compat_queue *queue)
{
	struct compat_device *device;
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo allocation;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkExportSemaphoreCreateInfo export;
	VkResult error;
	uint32_t index;

	/* Keeps ownership transfers on the same externally synchronized queue family. */
	device = chain->device;
	if (chain->queue != VK_NULL_HANDLE) {
		/* A different family would invalidate the pre-recorded ownership barriers. */
		if (chain->family != queue->family)
			return VK_ERROR_INITIALIZATION_FAILED;

		/* Reuses fully initialized private submission objects. */
		return VK_SUCCESS;
	}

	/* A prior partial initialization remains owned for destruction and cannot be retried in place. */
	if (chain->pool != VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Allocates a pool for foreign ownership release and acquire barriers. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.queueFamilyIndex = queue->family;
	error = device->create_pool(device->handle, &pool, NULL, &chain->pool);
	if (error != VK_SUCCESS)
		return error;

	/* Stores the owning family before recording either transfer direction. */
	chain->family = queue->family;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = chain->pool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = chain->count;
	error = device->allocate_commands(device->handle, &allocation, chain->release_command);
	if (error != VK_SUCCESS)
		return error;

	/* A second command per image re-acquires ownership before the application can reuse it. */
	error = device->allocate_commands(device->handle, &allocation, chain->acquire_command);
	if (error != VK_SUCCESS)
		return error;

	/* Builds reusable image-specific GPU synchronization objects. */
	for (index = 0; index < chain->count; index++) {
		/* Records the release half only when the backend supports foreign ownership. */
		error = chain_record(chain, index, 0, chain->release_command[index]);
		if (error != VK_SUCCESS)
			return error;

		/* Records the acquire half paired with the same queue family and image. */
		error = chain_record(chain, index, 1, chain->acquire_command[index]);
		if (error != VK_SUCCESS)
			return error;

		/* Initially signaled fences make the first reuse wait nonblocking. */
		memset(&fence, 0, sizeof(fence));
		fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		error = device->create_fence(device->handle, &fence, NULL, &chain->fence[index]);
		if (error != VK_SUCCESS)
			return error;

		/* SYNC_FD export transfers the semaphore payload so later frames can reuse the object. */
		memset(&export, 0, sizeof(export));
		export.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
		export.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		if (device->implicit_sync != 0)
			semaphore.pNext = &export;

		/* Each image has a separately reusable binary synchronization payload. */
		error = device->create_semaphore(device->handle, &semaphore, NULL, &chain->semaphore[index]);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Publishes complete submission resources only after every allocation succeeds. */
	chain->queue = queue->handle;

	/* Succeeded: later acquire and present calls reuse these image-specific objects. */
	return VK_SUCCESS;
}

/* Records one foreign ownership transfer while preserving the application's presentation layout. */
static VkResult
chain_record(
	struct compat_swapchain *chain,
	uint32_t index,
	unsigned acquire,
	VkCommandBuffer command)
{
	VkResult submission;
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkBufferMemoryBarrier visible;
	VkBufferImageCopy copy;
	VkResult error;

	/* Begins an ordinary reusable private command buffer. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	error = chain->device->begin_command(command, &begin);
	if (error != VK_SUCCESS)
		return error;

	/* KMS presentation copies the ordinary rendered image into coherent readback storage. */
	if (chain->surface->kms != NULL) {
		/* Acquire has no display ownership command; only present performs the readback. */
		if (acquire == 0) {
			/* Transfers completed color writes into a source layout for one tightly packed image copy. */
			memset(&barrier, 0, sizeof(barrier));
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = chain->images[index].image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.layerCount = 1;
			chain->device->barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

			/* Zero row length and image height specify tightly packed four-byte pixels. */
			memset(&copy, 0, sizeof(copy));
			copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy.imageSubresource.layerCount = 1;
			copy.imageExtent.width = chain->extent.width;
			copy.imageExtent.height = chain->extent.height;
			copy.imageExtent.depth = 1;
			chain->device->copy_image(command, chain->images[index].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chain->readback, 1, &copy);

			/* Restores the presentation layout before the application's next acquisition. */
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			chain->device->barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

			/* Host coherency alone does not replace the transfer-to-host memory dependency. */
			memset(&visible, 0, sizeof(visible));
			visible.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			visible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			visible.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			visible.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			visible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			visible.buffer = chain->readback;
			visible.size = VK_WHOLE_SIZE;
			chain->device->barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &visible, 0, NULL);
		}
	}

	/* Foreign ownership transfer is required only for drivers advertising its extension. */
	if (chain->foreign != 0) {
		/* The barrier leaves the application's final layout unchanged. */
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = 0;
		barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.srcQueueFamilyIndex = chain->family;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
		barrier.image = chain->images[index].image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		if (acquire != 0) {
			/* The paired acquire establishes ownership before the app's next semaphore wait completes. */
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
			barrier.dstQueueFamilyIndex = chain->family;
		}

		/* All prior writes precede display ownership, and later accesses follow acquire ownership. */
		chain->device->barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
	}

	/* Returns command recording's final backend status. */
	submission = chain->device->end_command(command);
	if (submission != VK_SUCCESS)
		return submission;

	/* Succeeded: the ownership-transfer command buffer is recorded. */
	return VK_SUCCESS;
}

/* Signals acquire outputs only after the compositor's read completion and optional foreign ownership transfer. */
static VkResult
chain_acquire_signal(
	struct compat_swapchain *chain,
	uint32_t index,
	VkSemaphore semaphore,
	VkFence fence)
{
	struct compat_device *device;
	struct compat_queue *queue;
	VkImportSemaphoreFdInfoKHR sem_import;
	VkImportFenceFdInfoKHR fence_import;
	VkSubmitInfo submit;
	VkPipelineStageFlags stage;
	VkResult error;
	int fd;
	int duplicate;
	int answer;
	unsigned transfer;

	/* Initial acquire needs a device queue only for CPU fallback or foreign re-acquire. */
	device = chain->device;
	transfer = 0;
	if (chain->foreign != 0)
		transfer = chain->images[index].presented;

	/* A private submission also establishes the resource pool used by present. */
	queue = compat_device_queue(device);
	if (queue == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Sets up the chain's private backend queue resources before the first acquisition. */
	error = chain_commands(chain, queue);
	if (error != VK_SUCCESS)
		return error;

	/* Reusing the image's private semaphore and command buffers requires the prior submit to finish. */
	error = device->wait_fences(device->handle, 1, &chain->fence[index], VK_TRUE, UINT64_MAX);
	if (error != VK_SUCCESS)
		return error;

	/* Release means CPU fallback already has a completed compositor payload. */
	fd = -1;
	if (chain->fallback == 0) {
		/* Exports both reader and writer fences to protect the application's next write. */
		answer = compat_dma_sync_export(chain->images[index].fd, COMPAT_DMA_SYNC_WRITE, &fd);
		if (answer != 0) {
			/* Unsupported synchronization permanently selects the safe CPU path for this chain. */
			if (errno != ENOTTY) {
				if (errno != EINVAL) {
					if (errno != EPERM)
						return VK_ERROR_SURFACE_LOST_KHR;
				}
			}

			/* Release ownership proves the compositor has finished before the fallback acquire. */
			chain->fallback = 1;
		}
	}

	/* Direct imports avoid a private submission when no foreign barrier is required. */
	if (device->implicit_sync != 0) {
		/* Re-acquire imports into our private semaphore before signaling application outputs. */
		memset(&sem_import, 0, sizeof(sem_import));
		sem_import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
		sem_import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
		sem_import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		sem_import.semaphore = semaphore;
		if (transfer != 0)
			sem_import.semaphore = chain->semaphore[index];

		/* When a direct fence also needs the payload, duplicate before either import consumes it. */
		duplicate = -1;
		if (transfer == 0) {
			if (fence != VK_NULL_HANDLE) {
				if (fd >= 0) {
					duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
					if (duplicate < 0) {
						(void)close(fd);
						return VK_ERROR_OUT_OF_HOST_MEMORY;
					}
				}
			}
		}

		/* Semaphore import consumes its descriptor only on success. */
		if (sem_import.semaphore != VK_NULL_HANDLE) {
			sem_import.fd = fd;
			error = device->import_semaphore(device->handle, &sem_import);
			if (error != VK_SUCCESS) {
				/* Retains no descriptor on a failed ownership transfer. */
				if (fd >= 0)
					(void)close(fd);

				/* A failed semaphore import must also close the pending fence duplicate. */
				if (duplicate >= 0)
					(void)close(duplicate);

				/* Returns the backend import failure without acquiring the image. */
				return error;
			}

			/* Successful import has transferred the original descriptor to Vulkan. */
			fd = -1;
		}

		/* A direct fence import receives the same completed compositor payload. */
		if (transfer == 0) {
			if (fence != VK_NULL_HANDLE) {
				/* Fence-only acquisition consumes the original export rather than an unused duplicate. */
				if (semaphore == VK_NULL_HANDLE) {
					if (duplicate >= 0)
						(void)close(duplicate);

					/* Moves the sole remaining descriptor to the fence import. */
					duplicate = fd;
					fd = -1;
				}

				/* Temporary SYNC_FD imports leave permanent fence payload ownership unchanged. */
				memset(&fence_import, 0, sizeof(fence_import));
				fence_import.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR;
				fence_import.fence = fence;
				fence_import.flags = VK_FENCE_IMPORT_TEMPORARY_BIT;
				fence_import.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
				fence_import.fd = duplicate;
				error = device->import_fence(device->handle, &fence_import);
				if (error != VK_SUCCESS) {
					if (duplicate >= 0)
						(void)close(duplicate);

					/* Returns the failure without publishing an acquired index. */
					return error;
				}
			}

			/* No output consumed a descriptor only when the application passed neither sync object. */
			if (fd >= 0)
				(void)close(fd);

			/* Succeeded: direct imports establish the application's acquire synchronization. */
			return VK_SUCCESS;
		}
	}

	/* CPU fallback submits no commands until foreign ownership must be re-acquired. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	if (transfer != 0) {
		/* An imported reader-completion payload precedes the foreign ownership acquire barrier. */
		if (device->implicit_sync != 0) {
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &chain->semaphore[index];
			submit.pWaitDstStageMask = &stage;
		}

		/* The acquire barrier completes before application semaphore and fence signals. */
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &chain->acquire_command[index];
	}

	/* Empty submissions provide binary semaphore signals when descriptor imports are unavailable. */
	if (semaphore != VK_NULL_HANDLE) {
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &semaphore;
	}

	/* Queue order places this acquire before the application's subsequent rendering work. */
	error = device->submit(chain->queue, 1, &submit, fence);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the application receives completion after safe image re-acquisition. */
	return VK_SUCCESS;
}

/* Publishes one rendered buffer only after its writer fence or CPU completion protects display reads. */
static VkResult
chain_present(
	struct compat_swapchain *chain,
	struct compat_queue *queue,
	uint32_t index,
	uint32_t wait_count,
	const VkSemaphore *waits)
{
	/* FIFO has at most one pacing callback, owned by the surface until done or the next deadline. */
	static const struct wl_callback_listener chain_frame_listener = {
	    chain_frame};
	struct compat_device *device;
	struct compat_surface *surface;
	VkSubmitInfo submit;
	VkSemaphoreGetFdInfoKHR export;
	VkPipelineStageFlags *stages;
	VkResult error;
	uint32_t other;
	int fd;
	int answer;
	int progress;
	uint64_t start;
	uint64_t elapsed;

	/* Requires an image currently owned by this application's acquired frame. */
	device = chain->device;
	surface = chain->surface;
	if (index >= chain->count)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The application cannot present an image still retained by the compositor. */
	if (chain->images[index].acquired == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Rejects a disconnected compositor before consuming application semaphores. */
	if (surface->lost != 0)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Private ownership barriers use the presentation queue's recorded family. */
	error = chain_commands(chain, queue);
	if (error != VK_SUCCESS)
		return error;

	/* Waits for the image's previous private fence before reusing its submission objects. */
	error = device->wait_fences(device->handle, 1, &chain->fence[index], VK_TRUE, UINT64_MAX);
	if (error != VK_SUCCESS)
		return error;

	/* Builds one explicit stage mask for every application wait semaphore. */
	stages = calloc((size_t)wait_count + 1, sizeof(*stages));
	if (stages == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Resets only after the previous completed payload has been observed. */
	error = device->reset_fences(device->handle, 1, &chain->fence[index]);
	if (error != VK_SUCCESS) {
		free(stages);
		return error;
	}

	/* Rendering completion gates every command or exported writer payload. */
	for (other = 0; other < wait_count; other++) {
		/* No image write can bypass the application's render completion semaphore. */
		stages[other] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	}

	/* Transfers foreign ownership when supported, otherwise submits only synchronization. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = wait_count;
	submit.pWaitSemaphores = waits;
	submit.pWaitDstStageMask = stages;
	if (chain->foreign != 0 || surface->kms != NULL) {
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &chain->release_command[index];
	}

	/* CPU fallback needs only its completion fence, never an unconsumed binary semaphore. */
	if (chain->fallback == 0) {
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &chain->semaphore[index];
	}

	/* Every submission has a fence so an ioctl failure can fall back safely in this same frame. */
	error = device->submit(queue->handle, 1, &submit, chain->fence[index]);
	free(stages);
	if (error != VK_SUCCESS)
		return error;

	/* Exports the writer completion payload and attaches it to the DMA-BUF reservation object. */
	if (chain->fallback == 0) {
		/* SYNC_FD export consumes the binary semaphore payload for safe object reuse. */
		memset(&export, 0, sizeof(export));
		export.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
		export.semaphore = chain->semaphore[index];
		export.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		error = device->semaphore_fd(device->handle, &export, &fd);
		if (error != VK_SUCCESS)
			return error;

		/* Descriptor minus one already denotes completed GPU work. */
		if (fd >= 0) {
			/* DMA-BUF import retains its own payload and does not consume our descriptor. */
			answer = compat_dma_sync_import(chain->images[index].fd, COMPAT_DMA_SYNC_WRITE, fd);
			if (answer != 0) {
				/* Only supported capability failures may choose the permanent CPU path. */
				if (errno != ENOTTY) {
					if (errno != EINVAL) {
						if (errno != EPERM) {
							(void)close(fd);
							return VK_ERROR_SURFACE_LOST_KHR;
						}
					}
				}

				/* This frame's fence is already submitted, so safe fallback requires no resubmission. */
				chain->fallback = 1;
			}

			/* The sync descriptor belongs to this function on both import outcomes. */
			(void)close(fd);
		}
	}

	/* Capability fallback waits for the already submitted writer before the compositor can read it. */
	if (chain->fallback != 0) {
		error = device->wait_fences(device->handle, 1, &chain->fence[index], VK_TRUE, UINT64_MAX);
		if (error != VK_SUCCESS)
			return error;
	}

	/* The synchronous KMS path copies completed pixels and returns image ownership after scanout submission. */
	if (surface->kms != NULL) {
		error = compat_kms_present(chain);
		if (error != VK_SUCCESS)
			return error;

		/* CPU readback has finished, so the Vulkan image can be acquired again. */
		chain->images[index].acquired = 0;
		chain->images[index].presented = 1;
		return VK_SUCCESS;
	}

	/* FIFO waits no longer than 100 ms for the prior frame callback. */
	start = compat_time();
	if (chain->mode == VK_PRESENT_MODE_FIFO_KHR) {
		/* Keeps hidden surfaces from blocking presentation indefinitely. */
		while (surface->frame != NULL) {
			/* The original callback deadline is never extended by unrelated events. */
			elapsed = compat_time() - start;
			if (elapsed >= CHAIN_RELEASE_TIMEOUT_NS)
				break;

			/* Receives only WSI callbacks during the remaining pacing interval. */
			progress = compat_surface_progress(surface, CHAIN_RELEASE_TIMEOUT_NS - elapsed);
			if (progress < 0)
				return VK_ERROR_SURFACE_LOST_KHR;
		}
	}

	/* Retires a timed-out or obsolete callback before creating the next pacing request. */
	if (surface->frame != NULL) {
		wl_callback_destroy(surface->frame);
		surface->frame = NULL;
	}

	/* Transfers this buffer's ownership to the compositor until wl_buffer.release. */
	chain->images[index].busy = 1;
	chain->images[index].acquired = 0;
	chain->images[index].presented = 1;
	wl_surface_attach(surface->surface_wrapper, chain->images[index].buffer, 0, 0);
	wl_surface_damage_buffer(surface->surface_wrapper, 0, 0, (int32_t)chain->extent.width, (int32_t)chain->extent.height);
	if (chain->mode == VK_PRESENT_MODE_FIFO_KHR) {
		/* The wrapper makes this callback inherit the private presentation queue. */
		surface->frame = wl_surface_frame(surface->surface_wrapper);
		if (surface->frame == NULL)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Installs pacing completion before committing the surface. */
		answer = wl_callback_add_listener(surface->frame, &chain_frame_listener, surface);
		if (answer != 0)
			return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Commits the selected buffer and flushes while preserving asynchronous output backpressure. */
	wl_surface_commit(surface->surface_wrapper);
	answer = wl_display_flush(surface->display);
	if (answer < 0) {
		/* EAGAIN is handled by the next bounded private-queue progress call. */
		if (errno != EAGAIN) {
			surface->lost = 1;
			return VK_ERROR_SURFACE_LOST_KHR;
		}
	}

	/* Succeeded: implicit completion or CPU waiting protects the compositor's next read. */
	return VK_SUCCESS;
}

/* Retires backend resources while keeping compositor release callback records alive. */
static void
chain_gpu_free(
	struct compat_swapchain *chain)
{
	struct compat_device *device;
	struct compat_image *image;
	uint32_t index;

	/* Callback-only retired records do not keep a backend device alive. */
	device = chain->device;
	if (device == NULL)
		return;

	/* Clears each handle after retirement so partial cleanup remains idempotent. */
	for (index = 0; index < chain->count; index++) {
		/* The same image records remain valid for future wl_buffer.release callbacks. */
		image = &chain->images[index];

		/* Destroys the image before releasing the memory that backs it. */
		if (image->image != VK_NULL_HANDLE) {
			device->destroy_image(device->handle, image->image, NULL);
			image->image = VK_NULL_HANDLE;
		}

		/* A partially created image may not yet have an allocation. */
		if (image->memory != VK_NULL_HANDLE) {
			device->free_memory(device->handle, image->memory, NULL);
			image->memory = VK_NULL_HANDLE;
		}

		/* Private submission fences retire only after queue completion was established. */
		if (chain->fence[index] != VK_NULL_HANDLE) {
			device->destroy_fence(device->handle, chain->fence[index], NULL);
			chain->fence[index] = VK_NULL_HANDLE;
		}

		/* The semaphore has no outstanding submission when a chain is collected. */
		if (chain->semaphore[index] != VK_NULL_HANDLE) {
			device->destroy_semaphore(device->handle, chain->semaphore[index], NULL);
			chain->semaphore[index] = VK_NULL_HANDLE;
		}
	}

	/* Destroying the pool also frees both directions' private command buffers. */
	if (chain->pool != VK_NULL_HANDLE) {
		device->destroy_pool(device->handle, chain->pool, NULL);
		chain->pool = VK_NULL_HANDLE;
	}

	/* KMS copy allocations and scanout retire while the creating device remains live. */
	if (chain->surface->kms != NULL)
		compat_display_chain_free(chain);

	/* Deferred protocol records retain no backend device dependency. */
	chain->device = NULL;

	/* Succeeded: no private GPU operation can retain image or readback storage. */
	return;
}

/* Creates one ordinary optimal-tiled image with transfer-source usage for portable KMS copying. */
static VkResult
chain_display_image(
	struct compat_swapchain *chain,
	uint32_t index,
	const VkSwapchainCreateInfoKHR *create)
{
	VkResult submission;
	struct compat_device *device;
	struct compat_image *image;
	VkImageCreateInfo info;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryAllocateInfo allocation;
	VkResult error;
	uint32_t type;

	/* Rendering images need no tiling modifier or external-memory export capability. */
	device = chain->device;
	image = &chain->images[index];
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = chain->format;
	info.extent.width = chain->extent.width;
	info.extent.height = chain->extent.height;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = create->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = create->imageSharingMode;
	info.queueFamilyIndexCount = create->queueFamilyIndexCount;
	info.pQueueFamilyIndices = create->pQueueFamilyIndices;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	error = device->create_image(device->handle, &info, NULL, &image->image);
	if (error != VK_SUCCESS)
		return error;

	/* Reads mandatory API-1.0 memory requirements for the unchanged backend image. */
	device->image_requirements1(device->handle, image->image, &requirements);
	device->instance->memory_properties(device->physical, &properties);
	for (type = 0; type < properties.memoryTypeCount; type++) {
		/* Any compatible backend image memory can supply the source of the GPU copy. */
		if ((requirements.memoryTypeBits & (1U << type)) != 0)
			break;
	}

	/* A missing compatible type cannot back this rendering image. */
	if (type == properties.memoryTypeCount)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* Allocates the exact image size from one compatible memory type. */
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = type;
	error = device->allocate_memory(device->handle, &allocation, NULL, &image->memory);
	if (error != VK_SUCCESS)
		return error;

	/* Returns the final backend bind result without wrapping either handle. */
	submission = device->bind_image(device->handle, image->image, image->memory, 0);
	if (submission != VK_SUCCESS)
		return submission;

	/* Succeeded: the display image is bound to its allocated memory. */
	return VK_SUCCESS;
}

/* Allocates tightly packed coherent host-visible storage for the completed image copy. */
static VkResult
chain_display_readback(
	struct compat_swapchain *chain)
{
	VkResult submission;
	struct compat_device *device;
	VkBufferCreateInfo info;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryAllocateInfo allocation;
	VkMemoryPropertyFlags flags;
	VkDeviceSize bytes;
	VkResult error;
	uint32_t type;

	/* Copy sizes use 64-bit arithmetic even when display extents use 32-bit fields. */
	device = chain->device;
	bytes = (VkDeviceSize)chain->extent.width * chain->extent.height * 4;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = bytes;
	info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	error = device->create_buffer(device->handle, &info, NULL, &chain->readback);
	if (error != VK_SUCCESS)
		return error;

	/* Readback requires both CPU visibility and coherency before the post-fence memcpy. */
	device->buffer_requirements(device->handle, chain->readback, &requirements);
	device->instance->memory_properties(device->physical, &properties);
	flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (type = 0; type < properties.memoryTypeCount; type++) {
		/* The buffer's allowed type mask is independent of the optimal image's allocation. */
		if ((requirements.memoryTypeBits & (1U << type)) == 0)
			continue;

		/* Both visibility and coherency are required for this copy backend. */
		if ((properties.memoryTypes[type].propertyFlags & flags) == flags)
			break;
	}

	/* An absent coherent host type cannot implement the advertised portable copy path. */
	if (type == properties.memoryTypeCount)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* Allocates exactly the backend's buffer requirements before mapping the copy region. */
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = type;
	error = device->allocate_memory(device->handle, &allocation, NULL, &chain->readback_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binding precedes all GPU and CPU access to the readback storage. */
	error = device->bind_buffer(device->handle, chain->readback, chain->readback_memory, 0);
	if (error != VK_SUCCESS)
		return error;

	/* Returns the actual host mapping result, retained until chain retirement. */
	submission = device->map_memory(device->handle, chain->readback_memory, 0, bytes, 0, &chain->readback_mapping);
	if (submission != VK_SUCCESS)
		return submission;

	/* Succeeded: the chain owns mapped CPU readback storage. */
	return VK_SUCCESS;
}
