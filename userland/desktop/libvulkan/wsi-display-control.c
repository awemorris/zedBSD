/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Display power, hotplug and refresh events (VK_EXT_display_control) and the
 * surface counters they depend on (VK_EXT_display_surface_counter).
 *
 * A fence registered for an event is an ordinary fence whose payload this
 * library signals once it sees the event, and which stays signaled until it
 * is reset.  No thread watches the kernel.
 *
 * A hotplug fence holds one kernel fence for each display node of the
 * renderer (GPU_DISPLAY_FENCE): the kernel signals it at the node's next
 * topology change, also one that raced the registration.  An observation
 * of the Vulkan fence polls those descriptors without waiting, and a wait
 * sleeps in poll on them, so nothing asks the kernel periodically.  Each
 * fence has registrations of its own, so one client's observation never
 * consumes another's, and no request acknowledges the topology on any open.
 *
 * A refresh fence holds one kernel fence of the display's node
 * (GPU_DISPLAY_FENCE, REFRESH): the kernel reads the display's refresh count
 * at the registration and signals the fence at the first boundary the driver
 * reports after it.  The fence is observed and waited for like a hotplug
 * fence's registrations.
 */

#include "wsi-internal.h"
#include "sync-internal.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

static VkResult event_fence_create(VkDevice device_handle, const VkAllocationCallbacks *allocator, VkFence *fence, struct vulkan_sync **sync);
static VkResult event_refresh_register(struct VkPhysicalDevice_T *physical, struct vulkan_display *display, int *fd, uint64_t *node);

/*
 * Retrieves a surface's capabilities together with its counters.
 *
 * The library offers no surface counter, so the counter set is empty.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceCapabilities2EXT(
	VkPhysicalDevice physicalDevice,
	VkSurfaceKHR surface,
	VkSurfaceCapabilities2EXT *pSurfaceCapabilities)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkResult error;

	/* Only the extension's own output record can receive the answer. */
	if (pSurfaceCapabilities == NULL ||
	    pSurfaceCapabilities->sType != VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_EXT)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The ordinary query answers every field the counters do not add. */
	error = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
	if (error != VK_SUCCESS)
		return error;

	/* Copies the ordinary capabilities into the extended record, keeping its chain. */
	pSurfaceCapabilities->minImageCount = capabilities.minImageCount;
	pSurfaceCapabilities->maxImageCount = capabilities.maxImageCount;
	pSurfaceCapabilities->currentExtent = capabilities.currentExtent;
	pSurfaceCapabilities->minImageExtent = capabilities.minImageExtent;
	pSurfaceCapabilities->maxImageExtent = capabilities.maxImageExtent;
	pSurfaceCapabilities->maxImageArrayLayers = capabilities.maxImageArrayLayers;
	pSurfaceCapabilities->supportedTransforms = capabilities.supportedTransforms;
	pSurfaceCapabilities->currentTransform = capabilities.currentTransform;
	pSurfaceCapabilities->supportedCompositeAlpha = capabilities.supportedCompositeAlpha;
	pSurfaceCapabilities->supportedUsageFlags = capabilities.supportedUsageFlags;

	/* No vertical-blank counter is kept for any surface. */
	pSurfaceCapabilities->supportedSurfaceCounters = 0U;

	/* Succeeded: the record describes the surface and its empty counter set. */
	return VK_SUCCESS;
}

/*
 * Powers a display this device presents to on, off or into suspend.
 *
 * The device needs a swapchain on the display, whose lease the kernel
 * request names; without one, or on an output without power control, the
 * request is refused.  The release of the lease powers the display on again.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkDisplayPowerControlEXT(
	VkDevice device,
	VkDisplayKHR display,
	const VkDisplayPowerInfoEXT *pDisplayPowerInfo)
{
	struct VkDevice_T *owner;
	struct vulkan_display *target;
	uint32_t state;
	VkResult error;

	/* Resolves the device and the display the request names. */
	owner = vulkan_device(device);
	target = vulkan_wsi_display(display);
	if (owner == NULL ||
	    target == NULL ||
	    pDisplayPowerInfo == NULL)
		return VK_ERROR_UNKNOWN;

	/* A display of another physical device or a foreign record names nothing here. */
	if (target->physical != owner->physical ||
	    pDisplayPowerInfo->sType != VK_STRUCTURE_TYPE_DISPLAY_POWER_INFO_EXT)
		return VK_ERROR_UNKNOWN;

	/* Translates the standard power state into the kernel's. */
	switch (pDisplayPowerInfo->powerState) {
	case VK_DISPLAY_POWER_STATE_OFF_EXT:
		state = GPU_DISPLAY_POWER_OFF;
		break;
	case VK_DISPLAY_POWER_STATE_SUSPEND_EXT:
		state = GPU_DISPLAY_POWER_SUSPEND;
		break;
	case VK_DISPLAY_POWER_STATE_ON_EXT:
		state = GPU_DISPLAY_POWER_ON;
		break;
	default:
		return VK_ERROR_UNKNOWN;
	}

	/* Asks the kernel under this device's lease on the display. */
	error = vulkan_wsi_display_power(owner, target, state);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the display is in the requested power state. */
	return VK_SUCCESS;
}

/*
 * Registers a fence signaled when a display of the device is plugged or unplugged.
 *
 * Every display node of the renderer that reports topology events gets a
 * kernel registration now; the fence is signaled once one of them is, and
 * stays signaled until it is reset.  A client registers a new fence before
 * it enumerates the displays again, so a change between the two is not
 * missed.  A renderer without such a node gets a fence nothing signals.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkRegisterDeviceEventEXT(
	VkDevice device,
	const VkDeviceEventInfoEXT *pDeviceEventInfo,
	const VkAllocationCallbacks *pAllocator,
	VkFence *pFence)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;
	struct vulkan_sync_event_fence *entries;
	uint32_t capacity;
	uint32_t count;
	VkResult error;

	/* Resolves the device; the hotplug is the only device event. */
	owner = vulkan_device(device);
	if (owner == NULL ||
	    pDeviceEventInfo == NULL ||
	    pFence == NULL)
		return VK_ERROR_UNKNOWN;

	/* A foreign record or another event type names nothing this library reports. */
	if (pDeviceEventInfo->sType != VK_STRUCTURE_TYPE_DEVICE_EVENT_INFO_EXT ||
	    pDeviceEventInfo->deviceEvent != VK_DEVICE_EVENT_TYPE_DISPLAY_HOTPLUG_EXT)
		return VK_ERROR_UNKNOWN;

	/* The registrations needed: one for each display node of the renderer. */
	error = vulkan_wsi_display_node_count(owner->physical, &capacity);
	if (error != VK_SUCCESS)
		return error;

	/* Creates the ordinary unsignaled fence the event will signal. */
	error = event_fence_create(device, pAllocator, pFence, &sync);
	if (error != VK_SUCCESS)
		return error;

	/* No other thread knows the fence yet, so the field needs no lock. */
	sync->event = VULKAN_SYNC_EVENT_HOTPLUG;

	/* A renderer without display nodes has no registration to hold. */
	if (capacity == 0U)
		return VK_SUCCESS;

	/* The registrations' storage follows the fence's allocation policy and lifetime. */
	entries = vulkan_allocate(&sync->object.allocator, (size_t)capacity * sizeof(*entries), sizeof(uint64_t), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (entries == NULL) {
		vkDestroyFence(device, *pFence, pAllocator);
		*pFence = VK_NULL_HANDLE;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Asks each display node for a fence its next topology change signals. */
	error = vulkan_wsi_display_node_hotplug(owner->physical, capacity, entries, &count);
	if (error != VK_SUCCESS) {
		vulkan_free(&sync->object.allocator, entries);
		vkDestroyFence(device, *pFence, pAllocator);
		*pFence = VK_NULL_HANDLE;
		return error;
	}

	/* No node reports topology events: the fence holds nothing. */
	if (count == 0U) {
		vulkan_free(&sync->object.allocator, entries);
		return VK_SUCCESS;
	}

	/* The fence owns the registrations until it is destroyed. */
	sync->event_fences = entries;
	sync->event_fence_count = count;

	/* Succeeded: the caller owns a fence the next hotplug signals. */
	return VK_SUCCESS;
}

/*
 * Registers a fence signaled by a display's next refresh boundary.
 *
 * The first-pixel-out event is the first boundary after the registration,
 * which the kernel signals on the display's node.  A display that does not
 * scan out makes no boundary.  A display whose node reports no boundaries,
 * or one that is gone, gets a fence nothing signals, as a renderer without
 * display nodes gets for a hotplug.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkRegisterDisplayEventEXT(
	VkDevice device,
	VkDisplayKHR display,
	const VkDisplayEventInfoEXT *pDisplayEventInfo,
	const VkAllocationCallbacks *pAllocator,
	VkFence *pFence)
{
	struct VkDevice_T *owner;
	struct vulkan_display *target;
	struct vulkan_sync *sync;
	struct vulkan_sync_event_fence *entry;
	uint64_t node;
	VkResult error;
	int fd;

	/* Resolves the device and the display the event belongs to. */
	owner = vulkan_device(device);
	target = vulkan_wsi_display(display);
	if (owner == NULL ||
	    target == NULL ||
	    pDisplayEventInfo == NULL ||
	    pFence == NULL)
		return VK_ERROR_UNKNOWN;

	/* A display of another physical device or another event type is not reported. */
	if (target->physical != owner->physical ||
	    pDisplayEventInfo->sType != VK_STRUCTURE_TYPE_DISPLAY_EVENT_INFO_EXT ||
	    pDisplayEventInfo->displayEvent != VK_DISPLAY_EVENT_TYPE_FIRST_PIXEL_OUT_EXT)
		return VK_ERROR_UNKNOWN;

	/* Creates the ordinary unsignaled fence the boundary will signal. */
	error = event_fence_create(device, pAllocator, pFence, &sync);
	if (error != VK_SUCCESS)
		return error;

	/* No other thread knows the fence yet, so the field needs no lock. */
	sync->event = VULKAN_SYNC_EVENT_REFRESH;

	/* Asks the display's node for a fence its next boundary signals. */
	error = event_refresh_register(owner->physical, target, &fd, &node);
	if (error != VK_SUCCESS) {
		vkDestroyFence(device, *pFence, pAllocator);
		*pFence = VK_NULL_HANDLE;
		return error;
	}

	/* A display that reports no boundary: the fence holds nothing. */
	if (fd < 0)
		return VK_SUCCESS;

	/* The registration's storage follows the fence's allocation policy and lifetime. */
	entry = vulkan_allocate(&sync->object.allocator, sizeof(*entry), sizeof(uint64_t), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (entry == NULL) {
		vulkan_wsi_display_node_fence_cancel(owner->physical, node, fd);
		close(fd);
		vkDestroyFence(device, *pFence, pAllocator);
		*pFence = VK_NULL_HANDLE;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* The fence owns the registration until it is destroyed. */
	entry->node = node;
	entry->fd = fd;
	sync->event_fences = entry;
	sync->event_fence_count = 1U;

	/* Succeeded: the caller owns a fence the display's next boundary signals. */
	return VK_SUCCESS;
}

/*
 * Reads a swapchain's surface counter.
 *
 * No surface offers a counter, so no swapchain is created with one and the
 * value cannot be read.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetSwapchainCounterEXT(
	VkDevice device,
	VkSwapchainKHR swapchain,
	VkSurfaceCounterFlagBitsEXT counter,
	uint64_t *pCounterValue)
{
	/* No counter exists to read; the value is left as it was. */
	(void)device;
	(void)swapchain;
	(void)counter;
	(void)pCounterValue;

	/* The swapchain has no such counter: an out-of-date answer, not a device loss. */
	return VK_ERROR_OUT_OF_DATE_KHR;
}

/*
 * Reports whether the kernel has signaled one of a display-event fence's
 * registrations.
 *
 * Each descriptor is looked at without waiting: the kernel is not asked
 * anything.  POLLIN is the signal; POLLERR comes with it when the node was
 * withdrawn or failed, or when the display of a refresh fence changed its
 * generation, which also ends the wait: the client enumerates the displays
 * again and sees the change.  The registrations stay fixed while the fence
 * lives, so no lock is needed.
 */
VkBool32
vulkan_display_event_signaled(
	struct vulkan_sync *sync)
{
	struct pollfd descriptor;
	uint32_t index;
	int ready;

	/* Looks at each registration without waiting. */
	for (index = 0U; index < sync->event_fence_count; index++) {
		descriptor.fd = sync->event_fences[index].fd;
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		ready = poll(&descriptor, 1U, 0);
		if (ready <= 0)
			continue;

		/* Any readiness of the descriptor is the end of the registration. */
		if (descriptor.revents != 0)
			return VK_TRUE;
	}

	/* Succeeded: no registration of the fence has been signaled. */
	return VK_FALSE;
}

/*
 * Retires a display-event fence's observation as vkResetFences resets it.
 *
 * The caller owns the device mutex.  A display-event fence whose event has
 * happened -- observed already, or signaled by the kernel and not observed
 * yet -- is spent: it observes nothing again, and its registrations are
 * withdrawn from the kernel.  A pending one is unsignaled already, so its
 * reset changes nothing and it keeps waiting for the event.  The
 * descriptors stay open until the fence is destroyed, because a wait on
 * another thread may be polling them.
 */
void
vulkan_display_event_reset_locked(
	struct VkDevice_T *device,
	struct vulkan_sync *sync)
{
	VkBool32 fired;
	uint32_t index;

	/* A fence of no display event, or a spent one, has nothing to retire. */
	if (sync->event == VULKAN_SYNC_EVENT_NONE || sync->event == VULKAN_SYNC_EVENT_SPENT)
		return;

	/* An unobserved event the kernel has signaled makes the fence signaled at the reset. */
	fired = sync->software_signaled;
	if (!fired)
		fired = vulkan_display_event_signaled(sync);

	/* A pending event fence keeps observing; the reset leaves it as it was. */
	if (!fired)
		return;

	/* Withdraws the registrations a node has not signaled yet. */
	for (index = 0U; index < sync->event_fence_count; index++)
		vulkan_wsi_display_node_fence_cancel(device->physical, sync->event_fences[index].node, sync->event_fences[index].fd);

	/* The signaled event is not replayed after the reset. */
	sync->event = VULKAN_SYNC_EVENT_SPENT;

	/* Succeeded: the fence is an ordinary unsignaled fence from now on. */
	return;
}

/*
 * Releases a display-event fence's registrations as the fence is destroyed.
 *
 * Each registration is withdrawn from its node, so the node's limit of
 * registrations is not held for a fence nobody can wait on, and its
 * descriptor is closed.
 */
void
vulkan_display_event_finish(
	struct vulkan_sync *sync)
{
	struct VkDevice_T *device;
	uint32_t index;

	/* A fence without registrations owns nothing here. */
	if (sync->event_fences == NULL)
		return;

	/* Withdraws and closes every registration; a spent fence's withdrawals answer that nothing is left. */
	device = (struct VkDevice_T *)sync->object.parent;
	for (index = 0U; index < sync->event_fence_count; index++) {
		vulkan_wsi_display_node_fence_cancel(device->physical, sync->event_fences[index].node, sync->event_fences[index].fd);
		close(sync->event_fences[index].fd);
	}

	/* The storage goes with the fence's allocation policy. */
	vulkan_free(&sync->object.allocator, sync->event_fences);
	sync->event_fences = NULL;
	sync->event_fence_count = 0U;

	/* Succeeded: no node holds a registration of the destroyed fence. */
	return;
}

/* Creates one unsignaled ordinary fence for an event registration. */
static VkResult
event_fence_create(
	VkDevice device_handle,
	const VkAllocationCallbacks *allocator,
	VkFence *fence,
	struct vulkan_sync **sync)
{
	VkFenceCreateInfo create;
	VkResult error;

	/* An unsignaled fence with no chain: only the event signals it. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	create.pNext = NULL;
	create.flags = 0U;
	error = vkCreateFence(device_handle, &create, allocator, fence);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the caller fills in the fence's event before publishing it. */
	*sync = vulkan_sync_object((uint64_t)(uintptr_t)*fence);
	return VK_SUCCESS;
}

/*
 * Registers a refresh fence with the display's node: fd gets the kernel's
 * fence, or -1 when the display reports no boundaries (its node has no
 * display control, or the display is gone or not connected).  An old
 * snapshot is refreshed and the registration made once more.
 */
static VkResult
event_refresh_register(
	struct VkPhysicalDevice_T *physical,
	struct vulkan_display *display,
	int *fd,
	uint64_t *node)
{
	struct vulkan_wsi_output output;
	struct gpu_display_fence request;
	unsigned attempt;
	VkResult error;
	int status;
	int native_error;

	/* Nothing is registered until the node answers. */
	*fd = -1;
	*node = 0U;

	/* At most one more try after an old generation's snapshot was refreshed. */
	native_error = 0;
	for (attempt = 0U; attempt < 2U; attempt++) {
		/* The display's current identity and generation name the output to the kernel. */
		error = vulkan_wsi_display_snapshot(display, &output);
		if (error != VK_SUCCESS)
			return VK_SUCCESS;

		/* A fence the display's next boundary signals; the descriptor is not inherited by a new program. */
		memset(&request, 0, sizeof(request));
		request.version = GPU_ABI_VERSION;
		request.size = sizeof(request);
		request.flags = GPU_HANDLE_CLOEXEC;
		request.event = GPU_DISPLAY_FENCE_REFRESH;
		request.display_id = (uint32_t)output.identifier;
		request.generation = output.generation;
		request.fd = -1;
		status = vulkan_wsi_display_node_ioctl(physical, &output, GPU_DISPLAY_FENCE, &request);
		native_error = errno;
		if (status == 0) {
			/* Succeeded: the caller owns the kernel's fence of the display's node. */
			*fd = request.fd;
			*node = output.device_identifier;
			return VK_SUCCESS;
		}

		/* Anything but an old generation is decided below. */
		if (native_error != ESTALE)
			break;

		/* An old generation: the snapshot is refreshed and the registration made again. */
		error = vulkan_wsi_display_refresh(display);
		if (error != VK_SUCCESS)
			return VK_SUCCESS;
	}

	/* Tells a shortage of memory from a display that reports no boundaries. */
	if (native_error == ENOSPC ||
	    native_error == ENOMEM ||
	    native_error == EMFILE ||
	    native_error == ENFILE) {
		/* The open's registrations or the process's descriptors ran out. */
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	} else if (native_error == EOPNOTSUPP ||
		   native_error == ENOENT ||
		   native_error == ENXIO ||
		   native_error == ENODEV ||
		   native_error == ESTALE) {
		/* A display that reports no boundaries now, or one that is gone: no registration. */
		return VK_SUCCESS;
	}

	/* Any other refusal leaves the display unable to report its boundaries. */
	return VK_ERROR_UNKNOWN;
}
