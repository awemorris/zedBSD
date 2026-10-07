/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The presenter: the window's swapchain and the synchronization of its
 * frames, with standard Vulkan WSI on the window's surface (libkeiland's,
 * WS131 p025).  The view draws the page: the presenter lends it the
 * device, and each frame it acquires an image, has the view record the
 * drawing into the frame's command buffer
 * (browser_view_record), submits it and presents the image
 * (ws074-p055, the render target of plan/ws074/design.md §19).
 *
 * The same way of opening the device and the swapchain as files'
 * presenter, but where the file manager copies a CPU canvas onto the image,
 * the browser draws its display list there (design.md §8.4).
 */

#include "shell/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* How long an acquire may wait for the compositor to give an image back, in nanoseconds. */
#define PRESENT_TIMEOUT		10000000000ULL

/* The most swapchain images the presenter takes. */
#define PRESENT_IMAGES_MAX	8U

static VkResult present_device(struct shell_present *present);
static VkResult present_swapchain(struct shell_present *present, uint32_t width, uint32_t height, VkSwapchainKHR old);
static VkResult present_targets(struct shell_present *present);
static void present_targets_free(struct shell_present *present);
static VkResult present_commands(struct shell_present *present);
static VkResult present_wait(struct shell_present *present);

/*
 * Makes the Vulkan objects of the window: the instance and surface, the
 * device (lent to the view, which draws with it), the swapchain, and the
 * frame's command buffer.
 */
VkResult
shell_present_open(
	struct shell_present *present,
	struct shell_window *window,
	struct browser_view *view)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	VkSemaphoreCreateInfo semaphore;
	struct browser_gpu gpu;
	const char *extensions[2];
	VkResult error;
	int failed;

	/* Nothing is owned yet. */
	memset(present, 0, sizeof(*present));

	/* The instance, with the surface extensions a Wayland window needs. */
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "browser";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = 2U;
	instance.ppEnabledExtensionNames = extensions;
	present->operation = "vkCreateInstance";
	error = vkCreateInstance(&instance, NULL, &present->instance);
	if (error != VK_SUCCESS)
		return error;

	/* The window's surface (libkeiland's, WS131 p025). */
	present->operation = "kl_window_vulkan_surface";
	failed = kl_window_vulkan_surface(window->kui, present->instance, &present->surface);
	if (failed != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A device with a queue that draws and presents to the surface. */
	error = present_device(present);
	if (error != VK_SUCCESS)
		return error;

	/* The swapchain at the window's size. */
	error = present_swapchain(present, window->width, window->height, VK_NULL_HANDLE);
	if (error != VK_SUCCESS)
		return error;

	/* A view and a present semaphore for each swapchain image. */
	error = present_targets(present);
	if (error != VK_SUCCESS)
		return error;

	/* The command buffer each frame is recorded into, and its fence. */
	error = present_commands(present);
	if (error != VK_SUCCESS)
		return error;

	/* The semaphore the acquire signals. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	present->operation = "vkCreateSemaphore";
	error = vkCreateSemaphore(present->device, &semaphore, NULL, &present->acquired);
	if (error != VK_SUCCESS)
		return error;

	/* The view draws with the device from now on. */
	gpu.instance = present->instance;
	gpu.physical = present->physical;
	gpu.device = present->device;
	gpu.queue_family = present->family;
	browser_view_set_gpu(view, &gpu);

	/* Succeeded: pages can be shown. */
	return VK_SUCCESS;
}

/*
 * Replaces the swapchain with one of a new size (the view forgets the
 * framebuffers it made over the old images).
 */
VkResult
shell_present_resize(
	struct shell_present *present,
	struct browser_view *view,
	uint32_t width,
	uint32_t height)
{
	VkSwapchainKHR old;
	VkResult error;

	/* Nothing may still use the old images. */
	present->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(present->device);
	if (error != VK_SUCCESS)
		return error;
	present->in_flight = 0;

	/* The view's framebuffers over the old images go, then the images' views, and the new chain replaces the old one. */
	browser_view_release_targets(view);
	present_targets_free(present);
	old = present->swapchain;
	present->swapchain = VK_NULL_HANDLE;
	error = present_swapchain(present, width, height, old);
	vkDestroySwapchainKHR(present->device, old, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Views and semaphores for the new images. */
	error = present_targets(present);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the next frame is shown at the new size. */
	return VK_SUCCESS;
}

/*
 * Shows the view in the next swapchain image: once the last frame has
 * finished, the image is acquired, the view records its drawing into the
 * frame's command buffer, and the buffer is submitted and the image
 * presented.
 *
 * Returns VK_ERROR_OUT_OF_DATE_KHR when the swapchain no longer matches
 * the window; the caller resizes and draws again.
 */
VkResult
shell_present_frame(
	struct shell_present *present,
	struct browser_view *view)
{
	VkCommandBufferBeginInfo begin;
	struct browser_target target;
	struct browser_gpu_failure failure;
	VkPipelineStageFlags stage;
	VkPresentInfoKHR info;
	VkSubmitInfo submit;
	uint32_t image;
	VkResult error;
	int status;

	/* The last frame's work, which used the view's instances and atlas and this command buffer, has finished. */
	error = present_wait(present);
	if (error != VK_SUCCESS)
		return error;

	/* The image to draw into, once the compositor has given one back. */
	present->operation = "vkAcquireNextImageKHR";
	error = vkAcquireNextImageKHR(present->device, present->swapchain, PRESENT_TIMEOUT, present->acquired, VK_NULL_HANDLE, &image);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* The frame's command buffer, started over. */
	present->operation = "vkResetCommandBuffer";
	error = vkResetCommandBuffer(present->command, 0U);
	if (error != VK_SUCCESS)
		return error;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	present->operation = "vkBeginCommandBuffer";
	error = vkBeginCommandBuffer(present->command, &begin);
	if (error != VK_SUCCESS)
		return error;

	/* The view's drawing of the page into the image, left ready to present. */
	target.image = present->targets[image].image;
	target.view = present->targets[image].view;
	target.format = present->format;
	target.new_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	target.width = present->extent.width;
	target.height = present->extent.height;
	status = browser_view_record(view, &target, present->command);
	if (status != 0) {
		browser_view_gpu_failure(view, &failure);
		present->operation = "browser_view_record";
		if (failure.operation != NULL)
			present->operation = failure.operation;
		if (status == EIO)
			return failure.result;
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* The recording ends. */
	present->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(present->command);
	if (error != VK_SUCCESS)
		return error;

	/* Submitted after the acquire, signalling the image's present semaphore and the frame's fence. */
	present->operation = "vkResetFences";
	error = vkResetFences(present->device, 1U, &present->fence);
	if (error != VK_SUCCESS)
		return error;
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &present->acquired;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &present->command;
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &present->targets[image].rendered;
	present->operation = "vkQueueSubmit";
	error = vkQueueSubmit(present->queue, 1U, &submit, present->fence);
	if (error != VK_SUCCESS)
		return error;

	/* The frame runs; the next one waits for its fence first. */
	present->in_flight = 1;

	/* Presents the image to the window. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	info.waitSemaphoreCount = 1U;
	info.pWaitSemaphores = &present->targets[image].rendered;
	info.swapchainCount = 1U;
	info.pSwapchains = &present->swapchain;
	info.pImageIndices = &image;
	present->operation = "vkQueuePresentKHR";
	error = vkQueuePresentKHR(present->queue, &info);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* Succeeded: the frame is in the window. */
	return VK_SUCCESS;
}

/*
 * Releases every Vulkan object, children before their parents: the view
 * lets the device go first.
 */
void
shell_present_close(
	struct shell_present *present,
	struct browser_view *view)
{
	/* The device's objects, once nothing runs. */
	if (present->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(present->device);

		/* The view's renderer and framebuffers on the device, then the images' views. */
		browser_view_set_gpu(view, NULL);
		present_targets_free(present);

		/* The frame's commands and fence. */
		if (present->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(present->device, present->pool, NULL);
		if (present->fence != VK_NULL_HANDLE)
			vkDestroyFence(present->device, present->fence, NULL);

		/* The acquire semaphore, the swapchain and the device itself. */
		if (present->acquired != VK_NULL_HANDLE)
			vkDestroySemaphore(present->device, present->acquired, NULL);
		if (present->swapchain != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(present->device, present->swapchain, NULL);
		vkDestroyDevice(present->device, NULL);
	}

	/* The surface and the instance. */
	if (present->surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(present->instance, present->surface, NULL);
	if (present->instance != VK_NULL_HANDLE)
		vkDestroyInstance(present->instance, NULL);

	/* Nothing is owned any more. */
	memset(present, 0, sizeof(*present));
}

/* Chooses a physical device and a queue family that draws and presents to the surface, and makes the device. */
static VkResult
present_device(
	struct shell_present *present)
{
	VkPhysicalDevice devices[8];
	VkQueueFamilyProperties families[16];
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo create;
	const char *extension;
	float priority;
	uint32_t count;
	uint32_t family_count;
	uint32_t index;
	uint32_t family;
	VkBool32 supported;
	VkResult error;

	/* The physical devices (the first eight are enough). */
	count = 8U;
	present->operation = "vkEnumeratePhysicalDevices";
	error = vkEnumeratePhysicalDevices(present->instance, &count, devices);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The first family of any device that draws and presents to this surface. */
	for (index = 0U; index < count && present->physical == VK_NULL_HANDLE; index++) {
		family_count = 16U;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, families);
		for (family = 0U; family < family_count; family++) {
			/* A family must draw and have a queue. */
			if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U)
				continue;
			if (families[family].queueCount == 0U)
				continue;

			/* And present to this very surface. */
			supported = VK_FALSE;
			error = vkGetPhysicalDeviceSurfaceSupportKHR(devices[index], family, present->surface, &supported);
			if (error != VK_SUCCESS)
				continue;
			if (supported == VK_FALSE)
				continue;

			/* This family of this device draws the window. */
			present->physical = devices[index];
			present->family = family;
			break;
		}
	}

	/* No device can draw this window. */
	if (present->physical == VK_NULL_HANDLE) {
		present->operation = "finding a device that presents to the window";
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* One queue of that family and the swapchain extension. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = present->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;
	extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create.queueCreateInfoCount = 1U;
	create.pQueueCreateInfos = &queue;
	create.enabledExtensionCount = 1U;
	create.ppEnabledExtensionNames = &extension;
	present->operation = "vkCreateDevice";
	error = vkCreateDevice(present->physical, &create, NULL, &present->device);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the device and its queue. */
	vkGetDeviceQueue(present->device, present->family, 0U, &present->queue);
	return VK_SUCCESS;
}

/* Makes the swapchain at a size, in an 8-bit UNORM format, presenting in FIFO order. */
static VkResult
present_swapchain(
	struct shell_present *present,
	uint32_t width,
	uint32_t height,
	VkSwapchainKHR old)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[16];
	VkSwapchainCreateInfoKHR create;
	VkFormat format;
	uint32_t count;
	uint32_t index;
	VkResult error;

	/* The formats the surface offers. */
	count = 16U;
	present->operation = "vkGetPhysicalDeviceSurfaceFormatsKHR";
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(present->physical, present->surface, &count, formats);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The first of those formats that is 8-bit UNORM (not sRGB: the renderers blend the stored values). */
	format = VK_FORMAT_UNDEFINED;
	for (index = 0U; index < count; index++) {
		/* Blue first, as the compositor offers. */
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM) {
			format = formats[index].format;
			break;
		}

		/* Or red first. */
		if (formats[index].format == VK_FORMAT_R8G8B8A8_UNORM) {
			format = formats[index].format;
			break;
		}
	}

	/* A surface without one cannot show the colors as they are. */
	if (format == VK_FORMAT_UNDEFINED) {
		present->operation = "finding an 8-bit UNORM surface format";
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* A new chain keeps the format the renderer's pass was made for. */
	if (present->format != VK_FORMAT_UNDEFINED && format != present->format) {
		present->operation = "keeping the surface format";
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* The chain's format. */
	present->format = format;

	/* The surface's limits. */
	present->operation = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR";
	error = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(present->physical, present->surface, &capabilities);
	if (error != VK_SUCCESS)
		return error;

	/* The window's size, clamped to the surface's range. */
	if (width < capabilities.minImageExtent.width)
		width = capabilities.minImageExtent.width;
	if (height < capabilities.minImageExtent.height)
		height = capabilities.minImageExtent.height;
	if (width > capabilities.maxImageExtent.width)
		width = capabilities.maxImageExtent.width;
	if (height > capabilities.maxImageExtent.height)
		height = capabilities.maxImageExtent.height;
	present->extent.width = width;
	present->extent.height = height;

	/* Three images when the surface allows, replacing the old chain. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.surface = present->surface;
	create.minImageCount = 3U;
	if (create.minImageCount < capabilities.minImageCount)
		create.minImageCount = capabilities.minImageCount;
	if (capabilities.maxImageCount != 0U && create.minImageCount > capabilities.maxImageCount)
		create.minImageCount = capabilities.maxImageCount;
	create.imageFormat = present->format;
	create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	create.imageExtent = present->extent;
	create.imageArrayLayers = 1U;
	create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform = capabilities.currentTransform;
	create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	create.clipped = VK_TRUE;
	create.oldSwapchain = old;

	/* Opaque when the surface takes it; the page is opaque, so premultiplied alpha shows the same. */
	create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0U)
		create.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;

	/* The chain. */
	present->operation = "vkCreateSwapchainKHR";
	error = vkCreateSwapchainKHR(present->device, &create, NULL, &present->swapchain);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the chain at the window's size. */
	return VK_SUCCESS;
}

/* Makes a view and a present semaphore for each swapchain image (the view makes its framebuffers over them). */
static VkResult
present_targets(
	struct shell_present *present)
{
	VkImage images[PRESENT_IMAGES_MAX];
	VkImageViewCreateInfo view;
	VkSemaphoreCreateInfo semaphore;
	uint32_t index;
	VkResult error;

	/* The swapchain's images. */
	present->count = PRESENT_IMAGES_MAX;
	present->operation = "vkGetSwapchainImagesKHR";
	error = vkGetSwapchainImagesKHR(present->device, present->swapchain, &present->count, images);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* A zeroed table, so that a failure part-way leaves only made objects to release. */
	present->targets = calloc(present->count, sizeof(present->targets[0]));
	if (present->targets == NULL) {
		present->operation = "allocating the swapchain's targets";
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Each image's objects. */
	for (index = 0U; index < present->count; index++) {
		/* The view of the image. */
		present->targets[index].image = images[index];
		memset(&view, 0, sizeof(view));
		view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.image = images[index];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = present->format;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.levelCount = 1U;
		view.subresourceRange.layerCount = 1U;
		present->operation = "vkCreateImageView";
		error = vkCreateImageView(present->device, &view, NULL, &present->targets[index].view);
		if (error != VK_SUCCESS)
			return error;

		/* The semaphore its present waits for. */
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		present->operation = "vkCreateSemaphore";
		error = vkCreateSemaphore(present->device, &semaphore, NULL, &present->targets[index].rendered);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: every image can be drawn into. */
	return VK_SUCCESS;
}

/* Releases the objects of the swapchain's images (not the images, which are the swapchain's). */
static void
present_targets_free(
	struct shell_present *present)
{
	uint32_t index;

	/* Each image's semaphore and view, where made. */
	for (index = 0U; present->targets != NULL && index < present->count; index++) {
		if (present->targets[index].rendered != VK_NULL_HANDLE)
			vkDestroySemaphore(present->device, present->targets[index].rendered, NULL);
		if (present->targets[index].view != VK_NULL_HANDLE)
			vkDestroyImageView(present->device, present->targets[index].view, NULL);
	}

	/* The table itself. */
	free(present->targets);
	present->targets = NULL;
	present->count = 0U;
}

/* Makes the command pool and the one buffer each frame is recorded into, and the fence its submission signals. */
static VkResult
present_commands(
	struct shell_present *present)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkResult error;

	/* A pool whose one buffer is reset every frame. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = present->family;
	present->operation = "vkCreateCommandPool";
	error = vkCreateCommandPool(present->device, &pool, NULL, &present->pool);
	if (error != VK_SUCCESS)
		return error;

	/* The buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = present->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	present->operation = "vkAllocateCommandBuffers";
	error = vkAllocateCommandBuffers(present->device, &command, &present->command);
	if (error != VK_SUCCESS)
		return error;

	/* The fence the frame's submission signals. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	present->operation = "vkCreateFence";
	error = vkCreateFence(present->device, &fence, NULL, &present->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: frames can be recorded. */
	return VK_SUCCESS;
}

/* Waits for the frame submitted last, if one may still run. */
static VkResult
present_wait(
	struct shell_present *present)
{
	VkResult error;

	/* No frame runs. */
	if (!present->in_flight)
		return VK_SUCCESS;

	/* Its fence. */
	present->operation = "vkWaitForFences";
	error = vkWaitForFences(present->device, 1U, &present->fence, VK_TRUE, PRESENT_TIMEOUT);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the frame has finished. */
	present->in_flight = 0;
	return VK_SUCCESS;
}
