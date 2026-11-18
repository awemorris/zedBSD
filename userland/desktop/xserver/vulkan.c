/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Showing a desktop window through standard Vulkan (WS069 p011).
 *
 * Each desktop window has a swapchain on its wl_surface.  A frame copies
 * the window's pixels into a host-visible staging buffer and from there
 * into the swapchain image it acquired, which is then presented.  Nothing
 * waits: when the last copy is still running or no image is free, the
 * frame is left for a later pass, as a wl_shm window waits for a buffer
 * the desktop has released.
 *
 * The instance and the device are shared by the connection's windows and
 * made with the first one, whose surface decides the queue family.
 */

#include "userland/desktop/xserver/internal.h"

#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <wayland-client.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most swapchain images a window keeps. */
#define VULKAN_IMAGES_MAX	8U

/* How long closing or resizing a window waits for its last copy, in nanoseconds. */
#define VULKAN_WAIT_NS		1000000000ULL

/* The alpha of every pixel shown (X's pixels have none). */
#define VULKAN_OPAQUE		0xff000000U

/*
 * The connection's Vulkan: the instance, the device and its queue, and
 * the memory types, made with the first window.
 */
struct x11_vulkan {
	VkInstance instance;
	VkPhysicalDevice physical;
	uint32_t family;
	VkDevice device;
	VkQueue queue;
	VkPhysicalDeviceMemoryProperties memory;
};

/*
 * One window's swapchain: its surface and images, the staging buffer its
 * pixels go through, and what orders a frame (the command buffer, the
 * fence of the last copy, the acquire semaphore and one semaphore an
 * image for its present).
 */
struct x11_vulkan_window {
	struct x11_vulkan *vulkan;
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkFormat format;
	VkExtent2D extent;
	uint32_t image_count;
	VkImage images[VULKAN_IMAGES_MAX];
	VkSemaphore rendered[VULKAN_IMAGES_MAX];

	/* Nonzero when the swapchain no longer matches the surface and is made again before the next frame. */
	int stale;

	/* The staging buffer, its memory and where the server writes it. */
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	uint32_t *staging_pixels;

	/* The frame's command buffer, its pool, the fence of the last copy and the acquire semaphore. */
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkSemaphore acquired;
};

static VkResult vulkan_device(struct x11_vulkan *vulkan, VkSurfaceKHR surface);
static VkResult vulkan_swapchain(struct x11_vulkan_window *window, unsigned width, unsigned height);
static VkResult vulkan_staging(struct x11_vulkan_window *window);
static VkResult vulkan_frame_objects(struct x11_vulkan_window *window);
static void vulkan_swapchain_free(struct x11_vulkan_window *window);
static void vulkan_staging_free(struct x11_vulkan_window *window);
static void vulkan_copy_pixels(struct x11_vulkan_window *window, const uint32_t *pixels);
static VkResult vulkan_record(struct x11_vulkan_window *window, uint32_t image);
static void vulkan_barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source, VkAccessFlags target);

/*
 * Opens the swapchain of a desktop window's wl_surface, making the
 * connection's Vulkan with the first window.  Returns NULL when Vulkan
 * cannot show it (the window then uses wl_shm).
 */
struct x11_vulkan_window *
x11_vulkan_window_open(
	struct x11_vulkan **shared,
	struct wl_display *display,
	struct wl_surface *surface,
	unsigned width,
	unsigned height)
{
	VkWaylandSurfaceCreateInfoKHR create;
	struct x11_vulkan_window *window;
	struct x11_vulkan *vulkan;
	VkBool32 supported;
	VkResult error;
	int status;

	/* The window. */
	window = calloc(1U, sizeof(*window));
	if (window == NULL)
		return NULL;

	/* The connection's Vulkan: the instance now, the device with the surface below. */
	vulkan = *shared;
	if (vulkan == NULL) {
		vulkan = x11_vulkan_open();
		if (vulkan == NULL) {
			free(window);
			return NULL;
		}

		/* Kept for the connection's later windows. */
		*shared = vulkan;
	}

	/* The window keeps it. */
	window->vulkan = vulkan;

	/* The surface of the window's wl_surface. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
	create.display = display;
	create.surface = surface;
	error = vkCreateWaylandSurfaceKHR(vulkan->instance, &create, NULL, &window->surface);
	if (error != VK_SUCCESS) {
		x11_vulkan_window_close(window);
		return NULL;
	}

	/* The device, chosen by the first surface; a later surface must be presentable by its family. */
	if (vulkan->device == VK_NULL_HANDLE) {
		error = vulkan_device(vulkan, window->surface);
		if (error != VK_SUCCESS) {
			x11_vulkan_window_close(window);
			return NULL;
		}
	}

	/* A later surface must be presentable by that family too. */
	supported = VK_FALSE;
	error = vkGetPhysicalDeviceSurfaceSupportKHR(vulkan->physical, vulkan->family, window->surface, &supported);
	if (error != VK_SUCCESS || supported == VK_FALSE) {
		x11_vulkan_window_close(window);
		return NULL;
	}

	/* The command buffer, the fence and the acquire semaphore. */
	error = vulkan_frame_objects(window);
	if (error != VK_SUCCESS) {
		x11_vulkan_window_close(window);
		return NULL;
	}

	/* The swapchain and the staging buffer at the window's size. */
	status = x11_vulkan_window_resize(window, width, height);
	if (status != 0) {
		x11_vulkan_window_close(window);
		return NULL;
	}

	/* Succeeded: the window can be shown. */
	return window;
}

/*
 * Makes the connection's instance (with the surface extensions a Wayland
 * window needs).  Returns NULL without Vulkan.
 */
struct x11_vulkan *
x11_vulkan_open(void)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo create;
	const char *extensions[2];
	struct x11_vulkan *vulkan;
	VkResult error;

	/* The state. */
	vulkan = calloc(1U, sizeof(*vulkan));
	if (vulkan == NULL)
		return NULL;

	/* The instance. */
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "xserver";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	create.pApplicationInfo = &application;
	create.enabledExtensionCount = 2U;
	create.ppEnabledExtensionNames = extensions;
	error = vkCreateInstance(&create, NULL, &vulkan->instance);
	if (error != VK_SUCCESS) {
		free(vulkan);
		return NULL;
	}

	/* Succeeded: the instance (the device comes with the first surface). */
	return vulkan;
}

/*
 * Shows a window's pixels (rows of its width, 0x00RRGGBB): copied through
 * the staging buffer into a free swapchain image and presented.  Returns
 * 0 when presented, 1 when the frame must wait (the last copy runs, or no
 * image is free), or -1 when Vulkan failed.
 */
int
x11_vulkan_window_present(
	struct x11_vulkan_window *window,
	const uint32_t *pixels)
{
	VkPresentInfoKHR present;
	VkPipelineStageFlags stage;
	VkSubmitInfo submit;
	VkDevice device;
	uint32_t image;
	VkResult error;
	int status;

	/* A swapchain that no longer matches the surface is made again. */
	device = window->vulkan->device;
	if (window->stale) {
		status = x11_vulkan_window_resize(window, window->extent.width, window->extent.height);
		if (status != 0)
			return -1;
	}

	/* The last copy must be done before the staging buffer is written again. */
	error = vkGetFenceStatus(device, window->fence);
	if (error == VK_NOT_READY)
		return 1;
	if (error != VK_SUCCESS)
		return -1;

	/* A free image, without waiting for one. */
	error = vkAcquireNextImageKHR(device, window->swapchain, 0U, window->acquired, VK_NULL_HANDLE, &image);
	if (error == VK_NOT_READY || error == VK_TIMEOUT)
		return 1;
	if (error == VK_ERROR_OUT_OF_DATE_KHR) {
		window->stale = 1;
		return 1;
	}

	/* Any other failure ends Vulkan for the window. */
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return -1;

	/* The pixels into the staging buffer, and the copy into the image recorded. */
	vulkan_copy_pixels(window, pixels);
	error = vulkan_record(window, image);
	if (error != VK_SUCCESS)
		return -1;

	/* The copy, after the acquire, signalling the image's present semaphore and the fence. */
	error = vkResetFences(device, 1U, &window->fence);
	if (error != VK_SUCCESS)
		return -1;
	stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &window->acquired;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &window->command;
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &window->rendered[image];
	error = vkQueueSubmit(window->vulkan->queue, 1U, &submit, window->fence);
	if (error != VK_SUCCESS)
		return -1;

	/* The image presented after the copy; a chain that went out of date is made again next time. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &window->rendered[image];
	present.swapchainCount = 1U;
	present.pSwapchains = &window->swapchain;
	present.pImageIndices = &image;
	error = vkQueuePresentKHR(window->vulkan->queue, &present);
	if (error == VK_SUBOPTIMAL_KHR || error == VK_ERROR_OUT_OF_DATE_KHR) {
		window->stale = 1;
		return 0;
	}

	/* Any other failure ends Vulkan for the window. */
	if (error != VK_SUCCESS)
		return -1;

	/* Succeeded: the frame is the desktop's. */
	return 0;
}

/*
 * Makes a window's swapchain and staging buffer again at a size, after
 * its last copy.  Returns 0, or -1 when Vulkan failed.
 */
int
x11_vulkan_window_resize(
	struct x11_vulkan_window *window,
	unsigned width,
	unsigned height)
{
	VkResult error;

	/* The last copy finishes first (it reads the staging buffer and writes an image). */
	(void)vkWaitForFences(window->vulkan->device, 1U, &window->fence, VK_TRUE, VULKAN_WAIT_NS);

	/* A new chain replacing the old one, and its present semaphores. */
	error = vulkan_swapchain(window, width, height);
	if (error != VK_SUCCESS)
		return -1;

	/* The pixels come in rows of the X window's width: a chain the surface made another size cannot take them. */
	if (window->extent.width != width || window->extent.height != height)
		return -1;

	/* The staging buffer of the new size. */
	vulkan_staging_free(window);
	error = vulkan_staging(window);
	if (error != VK_SUCCESS)
		return -1;

	/* Succeeded: the next frame is at the new size. */
	window->stale = 0;
	return 0;
}

/*
 * Closes a window's swapchain and surface, after its last copy.
 */
void
x11_vulkan_window_close(
	struct x11_vulkan_window *window)
{
	VkDevice device;

	/* The last copy finishes first. */
	device = window->vulkan->device;
	if (device != VK_NULL_HANDLE && window->fence != VK_NULL_HANDLE)
		(void)vkWaitForFences(device, 1U, &window->fence, VK_TRUE, VULKAN_WAIT_NS);

	/* The staging buffer and the chain. */
	if (device != VK_NULL_HANDLE) {
		vulkan_staging_free(window);
		vulkan_swapchain_free(window);
	}

	/* The frame's objects. */
	if (window->acquired != VK_NULL_HANDLE)
		vkDestroySemaphore(device, window->acquired, NULL);
	if (window->fence != VK_NULL_HANDLE)
		vkDestroyFence(device, window->fence, NULL);
	if (window->pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(device, window->pool, NULL);

	/* The surface, and the window itself. */
	if (window->surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(window->vulkan->instance, window->surface, NULL);
	free(window);
}

/*
 * Ends the connection's Vulkan (every window closed first).
 */
void
x11_vulkan_close(
	struct x11_vulkan *vulkan)
{
	/* Nothing was made. */
	if (vulkan == NULL)
		return;

	/* The device, after everything on it. */
	if (vulkan->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(vulkan->device);
		vkDestroyDevice(vulkan->device, NULL);
	}

	/* The instance. */
	vkDestroyInstance(vulkan->instance, NULL);
	free(vulkan);
}

/* Chooses the first device and queue family that copies and presents to a surface, and makes the device with the swapchain extension. */
static VkResult
vulkan_device(
	struct x11_vulkan *vulkan,
	VkSurfaceKHR surface)
{
	VkPhysicalDevice devices[8];
	VkQueueFamilyProperties families[16];
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo create;
	const char *extension;
	uint32_t family_count;
	uint32_t count;
	uint32_t index;
	uint32_t family;
	VkBool32 supported;
	float priority;
	VkResult error;

	/* The physical devices (the first eight are enough). */
	count = 8U;
	error = vkEnumeratePhysicalDevices(vulkan->instance, &count, devices);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The first family of any device that copies (a graphics family does) and presents to the surface. */
	for (index = 0U; index < count && vulkan->physical == VK_NULL_HANDLE; index++) {
		family_count = 16U;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, families);
		for (family = 0U; family < family_count; family++) {
			if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U || families[family].queueCount == 0U)
				continue;

			/* It must present to this surface. */
			supported = VK_FALSE;
			error = vkGetPhysicalDeviceSurfaceSupportKHR(devices[index], family, surface, &supported);
			if (error != VK_SUCCESS || supported == VK_FALSE)
				continue;

			/* This family of this device shows the windows. */
			vulkan->physical = devices[index];
			vulkan->family = family;
			break;
		}
	}

	/* No device can show the windows. */
	if (vulkan->physical == VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* One queue of that family and the swapchain extension. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = vulkan->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;
	extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create.queueCreateInfoCount = 1U;
	create.pQueueCreateInfos = &queue;
	create.enabledExtensionCount = 1U;
	create.ppEnabledExtensionNames = &extension;
	error = vkCreateDevice(vulkan->physical, &create, NULL, &vulkan->device);
	if (error != VK_SUCCESS)
		return error;

	/* Its queue and memory types, and a line in the log that windows are shown through Vulkan. */
	vkGetDeviceQueue(vulkan->device, vulkan->family, 0U, &vulkan->queue);
	vkGetPhysicalDeviceMemoryProperties(vulkan->physical, &vulkan->memory);
	fprintf(stderr, "X11SERVER VULKAN device ready family=%u\n", (unsigned)vulkan->family);

	/* Succeeded: the device can show windows. */
	return VK_SUCCESS;
}

/* Makes a window's swapchain at a size (an 8-bit UNORM format, MAILBOX when offered, three images, written by copies), replacing the old one, and its present semaphores. */
static VkResult
vulkan_swapchain(
	struct x11_vulkan_window *window,
	unsigned width,
	unsigned height)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[16];
	VkPresentModeKHR modes[8];
	VkPresentModeKHR mode;
	VkSwapchainCreateInfoKHR create;
	VkSemaphoreCreateInfo semaphore;
	struct x11_vulkan *vulkan;
	VkSwapchainKHR old;
	uint32_t count;
	uint32_t index;
	VkResult error;

	/* An 8-bit UNORM format the surface offers, blue first when there is one (X's pixels are laid out so). */
	vulkan = window->vulkan;
	count = 16U;
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(vulkan->physical, window->surface, &count, formats);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;
	window->format = VK_FORMAT_UNDEFINED;
	for (index = 0U; index < count; index++) {
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM) {
			window->format = formats[index].format;
			break;
		}

		/* Red first will do, swapping bytes, when blue first is not offered. */
		if (formats[index].format == VK_FORMAT_R8G8B8A8_UNORM)
			window->format = formats[index].format;
	}

	/* A surface without one cannot show the colours as they are. */
	if (window->format == VK_FORMAT_UNDEFINED)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The size, kept inside what the surface takes. */
	error = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vulkan->physical, window->surface, &capabilities);
	if (error != VK_SUCCESS)
		return error;
	if (width < capabilities.minImageExtent.width)
		width = capabilities.minImageExtent.width;
	if (height < capabilities.minImageExtent.height)
		height = capabilities.minImageExtent.height;
	if (width > capabilities.maxImageExtent.width)
		width = capabilities.maxImageExtent.width;
	if (height > capabilities.maxImageExtent.height)
		height = capabilities.maxImageExtent.height;
	window->extent.width = width;
	window->extent.height = height;

	/* MAILBOX when the surface offers it: a present then never waits for the desktop's last frame, which would hold every client. */
	mode = VK_PRESENT_MODE_FIFO_KHR;
	count = 8U;
	error = vkGetPhysicalDeviceSurfacePresentModesKHR(vulkan->physical, window->surface, &count, modes);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;
	for (index = 0U; index < count; index++) {
		if (modes[index] == VK_PRESENT_MODE_MAILBOX_KHR)
			mode = modes[index];
	}

	/* Three images when the surface allows, opaque, written by copies, replacing the old chain. */
	old = window->swapchain;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.surface = window->surface;
	create.minImageCount = 3U;
	if (create.minImageCount < capabilities.minImageCount)
		create.minImageCount = capabilities.minImageCount;
	if (capabilities.maxImageCount != 0U && create.minImageCount > capabilities.maxImageCount)
		create.minImageCount = capabilities.maxImageCount;
	create.imageFormat = window->format;
	create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	create.imageExtent = window->extent;
	create.imageArrayLayers = 1U;
	create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform = capabilities.currentTransform;
	create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	create.presentMode = mode;
	create.clipped = VK_TRUE;
	create.oldSwapchain = old;
	error = vkCreateSwapchainKHR(vulkan->device, &create, NULL, &window->swapchain);
	if (error != VK_SUCCESS) {
		window->swapchain = old;
		return error;
	}

	/* The old chain and its semaphores go. */
	for (index = 0U; index < window->image_count; index++) {
		vkDestroySemaphore(vulkan->device, window->rendered[index], NULL);
		window->rendered[index] = VK_NULL_HANDLE;
	}

	/* Then the old chain itself. */
	window->image_count = 0U;
	if (old != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(vulkan->device, old, NULL);

	/* The new chain's images. */
	count = VULKAN_IMAGES_MAX;
	error = vkGetSwapchainImagesKHR(vulkan->device, window->swapchain, &count, window->images);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* A present semaphore for each image. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for (index = 0U; index < count; index++) {
		error = vkCreateSemaphore(vulkan->device, &semaphore, NULL, &window->rendered[index]);
		if (error != VK_SUCCESS)
			return error;
		window->image_count = index + 1U;
	}

	/* Succeeded: the chain at the window's size. */
	return VK_SUCCESS;
}

/* Makes the staging buffer of the chain's size in host-visible, coherent memory, mapped for the server's writes. */
static VkResult
vulkan_staging(
	struct x11_vulkan_window *window)
{
	VkMemoryRequirements requirements;
	VkMemoryPropertyFlags wanted;
	VkBufferCreateInfo create;
	VkMemoryAllocateInfo allocate;
	struct x11_vulkan *vulkan;
	uint32_t type;
	void *mapped;
	VkResult error;

	/* The buffer, one pixel of four bytes for each of the image's. */
	vulkan = window->vulkan;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create.size = (VkDeviceSize)window->extent.width * window->extent.height * sizeof(uint32_t);
	create.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	error = vkCreateBuffer(vulkan->device, &create, NULL, &window->staging);
	if (error != VK_SUCCESS)
		return error;

	/* The first memory type it may use that the host sees and keeps coherent. */
	vkGetBufferMemoryRequirements(vulkan->device, window->staging, &requirements);
	wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (type = 0U; type < vulkan->memory.memoryTypeCount; type++) {
		if ((requirements.memoryTypeBits & (1U << type)) != 0U && (vulkan->memory.memoryTypes[type].propertyFlags & wanted) == wanted)
			break;
	}

	/* Without one the host cannot write the pixels. */
	if (type == vulkan->memory.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The memory. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	error = vkAllocateMemory(vulkan->device, &allocate, NULL, &window->staging_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Bound to the buffer. */
	error = vkBindBufferMemory(vulkan->device, window->staging, window->staging_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Mapped for good. */
	error = vkMapMemory(vulkan->device, window->staging_memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
	if (error != VK_SUCCESS)
		return error;
	window->staging_pixels = mapped;

	/* Succeeded: the pixels can be written. */
	return VK_SUCCESS;
}

/* Makes a window's command pool and buffer, the fence of its copies (signalled, as if a copy had finished) and the acquire semaphore. */
static VkResult
vulkan_frame_objects(
	struct x11_vulkan_window *window)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkDevice device;
	VkResult error;

	/* The pool, whose one buffer is recorded again each frame. */
	device = window->vulkan->device;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = window->vulkan->family;
	error = vkCreateCommandPool(device, &pool, NULL, &window->pool);
	if (error != VK_SUCCESS)
		return error;

	/* The command buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = window->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	error = vkAllocateCommandBuffers(device, &command, &window->command);
	if (error != VK_SUCCESS)
		return error;

	/* The fence, signalled so the first frame does not wait. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	error = vkCreateFence(device, &fence, NULL, &window->fence);
	if (error != VK_SUCCESS)
		return error;

	/* The acquire semaphore. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	error = vkCreateSemaphore(device, &semaphore, NULL, &window->acquired);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: frames can be recorded. */
	return VK_SUCCESS;
}

/* Destroys a window's chain and its present semaphores. */
static void
vulkan_swapchain_free(
	struct x11_vulkan_window *window)
{
	uint32_t index;

	/* The present semaphores. */
	for (index = 0U; index < window->image_count; index++)
		vkDestroySemaphore(window->vulkan->device, window->rendered[index], NULL);
	window->image_count = 0U;

	/* The chain. */
	if (window->swapchain != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(window->vulkan->device, window->swapchain, NULL);
	window->swapchain = VK_NULL_HANDLE;
}

/* Destroys a window's staging buffer and its memory. */
static void
vulkan_staging_free(
	struct x11_vulkan_window *window)
{
	/* The buffer. */
	if (window->staging != VK_NULL_HANDLE)
		vkDestroyBuffer(window->vulkan->device, window->staging, NULL);
	window->staging = VK_NULL_HANDLE;

	/* Its memory (unmapped with it). */
	if (window->staging_memory != VK_NULL_HANDLE)
		vkFreeMemory(window->vulkan->device, window->staging_memory, NULL);
	window->staging_memory = VK_NULL_HANDLE;
	window->staging_pixels = NULL;
}

/* Writes the window's pixels into the staging buffer, opaque, in the chain's byte order. */
static void
vulkan_copy_pixels(
	struct x11_vulkan_window *window,
	const uint32_t *pixels)
{
	uint32_t *target;
	uint32_t pixel;
	size_t count;
	size_t index;

	/* Each pixel, with its alpha made full. */
	target = window->staging_pixels;
	count = (size_t)window->extent.width * window->extent.height;
	if (window->format == VK_FORMAT_B8G8R8A8_UNORM) {
		for (index = 0U; index < count; index++)
			target[index] = pixels[index] | VULKAN_OPAQUE;
		return;
	}

	/* An RGBA chain takes red first: the red and blue bytes change places. */
	for (index = 0U; index < count; index++) {
		pixel = pixels[index];
		target[index] = VULKAN_OPAQUE | ((pixel & 0xffU) << 16) | (pixel & 0xff00U) | ((pixel >> 16) & 0xffU);
	}
}

/* Records the copy of the staging buffer into a chain image, left ready to present. */
static VkResult
vulkan_record(
	struct x11_vulkan_window *window,
	uint32_t image)
{
	VkCommandBufferBeginInfo begin;
	VkBufferImageCopy region;
	VkResult error;

	/* The buffer, recorded afresh. */
	error = vkResetCommandBuffer(window->command, 0U);
	if (error != VK_SUCCESS)
		return error;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	error = vkBeginCommandBuffer(window->command, &begin);
	if (error != VK_SUCCESS)
		return error;

	/* The image made ready for the copy (its old contents do not matter: the copy covers it). */
	vulkan_barrier(window->command, window->images[image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0U, VK_ACCESS_TRANSFER_WRITE_BIT);

	/* The whole image from the buffer, rows of the image's width. */
	memset(&region, 0, sizeof(region));
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1U;
	region.imageExtent.width = window->extent.width;
	region.imageExtent.height = window->extent.height;
	region.imageExtent.depth = 1U;
	vkCmdCopyBufferToImage(window->command, window->staging, window->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);

	/* Then ready to present. */
	vulkan_barrier(window->command, window->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0U);

	/* The recording ends. */
	error = vkEndCommandBuffer(window->command);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the frame's copy is recorded. */
	return VK_SUCCESS;
}

/* Records an image's change of layout around the transfer. */
static void
vulkan_barrier(
	VkCommandBuffer command,
	VkImage image,
	VkImageLayout from,
	VkImageLayout to,
	VkAccessFlags source,
	VkAccessFlags target)
{
	VkImageMemoryBarrier barrier;

	/* The whole colour image. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = source;
	barrier.dstAccessMask = target;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;

	/* Around the transfer stage. */
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
}
