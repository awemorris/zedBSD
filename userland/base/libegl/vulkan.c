/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan side of zedBSD's EGL: a display's instance and device, a
 * window surface's Vulkan surface and swapchain, and the frame that
 * eglSwapBuffers submits and presents.
 *
 * A Wayland window's surface comes from its wl_surface; a display-direct
 * window is the first display's first mode on the first plane that can
 * show it (VK_KHR_display).  A frame begins with a render pass that clears
 * the image to the context's clear colour (WS068 p002: GLES draws nothing
 * else yet), and the frame is waited for before the next one.
 */

#include "zegl.h"

#include <stdlib.h>
#include <string.h>

/* How long one frame may take on the GPU, in nanoseconds. */
#define ZEGL_TIMEOUT		10000000000ULL

static EGLint vulkan_display_surface(struct zegl_surface *surface);
static EGLint vulkan_swapchain(struct zegl_surface *surface);
static void vulkan_swapchain_free(struct zegl_surface *surface, int keep_swapchain);
static EGLint vulkan_frame_objects(struct zegl_surface *surface);
static void vulkan_record(struct zegl_surface *surface, uint32_t image, const float *color);

/*
 * Makes a display's Vulkan instance and device.  Returns EGL_SUCCESS, or
 * EGL_NOT_INITIALIZED when no device can be had.
 */
EGLint
zegl_vulkan_open(
	struct zegl_display *display)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	VkPhysicalDevice devices[8];
	VkQueueFamilyProperties families[16];
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo device;
	const char *extensions[3];
	const char *device_extension;
	float priority;
	uint32_t count;
	uint32_t family_count;
	uint32_t family;
	VkResult result;

	/* The instance, with the surface extensions of both window platforms. */
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
	extensions[2] = VK_KHR_DISPLAY_EXTENSION_NAME;
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "zedBSD EGL";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = 3U;
	instance.ppEnabledExtensionNames = extensions;
	result = vkCreateInstance(&instance, NULL, &display->instance);
	if (result != VK_SUCCESS)
		return EGL_NOT_INITIALIZED;

	/* The first physical device. */
	count = 8U;
	result = vkEnumeratePhysicalDevices(display->instance, &count, devices);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0U) {
		zegl_vulkan_close(display);
		return EGL_NOT_INITIALIZED;
	}

	/* The first one serves. */
	display->physical = devices[0];

	/* Its first family that draws. */
	family_count = 16U;
	vkGetPhysicalDeviceQueueFamilyProperties(display->physical, &family_count, families);
	for (family = 0U; family < family_count; family++) {
		if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U && families[family].queueCount != 0U)
			break;
	}

	/* A device that cannot draw serves no GLES. */
	if (family == family_count) {
		zegl_vulkan_close(display);
		return EGL_NOT_INITIALIZED;
	}

	/* That family's queue draws every frame. */
	display->family = family;

	/* The device with one queue of that family and the swapchain extension. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;
	device_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	memset(&device, 0, sizeof(device));
	device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device.queueCreateInfoCount = 1U;
	device.pQueueCreateInfos = &queue;
	device.enabledExtensionCount = 1U;
	device.ppEnabledExtensionNames = &device_extension;
	result = vkCreateDevice(display->physical, &device, NULL, &display->device);
	if (result != VK_SUCCESS) {
		zegl_vulkan_close(display);
		return EGL_NOT_INITIALIZED;
	}

	/* Succeeded: the display can make surfaces and contexts. */
	vkGetDeviceQueue(display->device, family, 0U, &display->queue);
	return EGL_SUCCESS;
}

/*
 * Releases a display's device and instance.
 */
void
zegl_vulkan_close(
	struct zegl_display *display)
{
	/* The device once it is idle, then the instance. */
	if (display->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(display->device);
		vkDestroyDevice(display->device, NULL);
	}

	/* The instance after the device. */
	if (display->instance != VK_NULL_HANDLE)
		vkDestroyInstance(display->instance, NULL);

	/* Nothing is held. */
	display->device = VK_NULL_HANDLE;
	display->instance = VK_NULL_HANDLE;
	display->physical = VK_NULL_HANDLE;
	display->queue = VK_NULL_HANDLE;
}

/*
 * Makes a window surface's Vulkan surface, swapchain and frame objects.
 * Returns EGL_SUCCESS, EGL_BAD_NATIVE_WINDOW when the window cannot be
 * shown, or EGL_BAD_ALLOC.
 */
EGLint
zegl_surface_open(
	struct zegl_surface *surface)
{
	VkWaylandSurfaceCreateInfoKHR wayland;
	struct zegl_display *display;
	VkBool32 supported;
	VkResult result;
	EGLint error;

	/* The Vulkan surface: the Wayland window's wl_surface, or the display's plane. */
	display = surface->display;
	if (display->platform == ZEGL_PLATFORM_WAYLAND) {
		memset(&wayland, 0, sizeof(wayland));
		wayland.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
		wayland.display = display->native;
		wayland.surface = surface->window->surface;
		result = vkCreateWaylandSurfaceKHR(display->instance, &wayland, NULL, &surface->surface);
		if (result != VK_SUCCESS)
			return EGL_BAD_NATIVE_WINDOW;
		surface->window_generation = surface->window->generation;
	} else {
		error = vulkan_display_surface(surface);
		if (error != EGL_SUCCESS)
			return error;
	}

	/* The queue must present to it. */
	supported = VK_FALSE;
	result = vkGetPhysicalDeviceSurfaceSupportKHR(display->physical, display->family, surface->surface, &supported);
	if (result != VK_SUCCESS || supported == VK_FALSE)
		return EGL_BAD_NATIVE_WINDOW;

	/* The frame's objects, then the swapchain and its framebuffers. */
	error = vulkan_frame_objects(surface);
	if (error != EGL_SUCCESS)
		return error;

	/* The swapchain and its framebuffers. */
	error = vulkan_swapchain(surface);
	if (error != EGL_SUCCESS)
		return error;

	/* Succeeded: the surface can present frames. */
	return EGL_SUCCESS;
}

/*
 * Releases a window surface's Vulkan objects, children first.
 */
void
zegl_surface_close(
	struct zegl_surface *surface)
{
	VkDevice device;

	/* Nothing may still run on them. */
	device = surface->display->device;
	if (device != VK_NULL_HANDLE)
		(void)vkDeviceWaitIdle(device);

	/* The swapchain with its framebuffers and views. */
	vulkan_swapchain_free(surface, 0);

	/* The frame's objects and the pass. */
	if (surface->rendered != VK_NULL_HANDLE)
		vkDestroySemaphore(device, surface->rendered, NULL);
	if (surface->acquired != VK_NULL_HANDLE)
		vkDestroySemaphore(device, surface->acquired, NULL);
	if (surface->fence != VK_NULL_HANDLE)
		vkDestroyFence(device, surface->fence, NULL);
	if (surface->pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(device, surface->pool, NULL);
	if (surface->pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(device, surface->pass, NULL);

	/* The Vulkan surface last. */
	if (surface->surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(surface->display->instance, surface->surface, NULL);
}

/*
 * Draws and presents one frame of a window surface with a context's state,
 * and waits for it.  Returns EGL_SUCCESS, or EGL_BAD_SURFACE (or
 * EGL_CONTEXT_LOST) when the frame could not be shown.
 */
EGLint
zegl_surface_present(
	struct zegl_surface *surface,
	struct zegl_context *context)
{
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkPipelineStageFlags stage;
	struct zegl_display *display;
	uint32_t image;
	unsigned tries;
	VkResult result;
	EGLint error;

	/* A resized Wayland window gets a new swapchain first. */
	display = surface->display;
	if (surface->window != NULL && surface->window->generation != surface->window_generation)
		surface->stale = 1;

	/* The image to draw into; a swapchain out of date is made again once. */
	for (tries = 0U; ; tries++) {
		/* A stale swapchain is replaced. */
		if (surface->stale) {
			error = vulkan_swapchain(surface);
			if (error != EGL_SUCCESS)
				return error;
		}

		/* The next image the compositor gives back. */
		result = vkAcquireNextImageKHR(display->device, surface->swapchain, ZEGL_TIMEOUT, surface->acquired, VK_NULL_HANDLE, &image);
		if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
			break;

		/* Out of date: once more with a new swapchain. */
		if (result != VK_ERROR_OUT_OF_DATE_KHR || tries >= 1U)
			return EGL_BAD_SURFACE;
		surface->stale = 1;
	}

	/* The frame: the image cleared to the context's colour. */
	vulkan_record(surface, image, context->gles.clear_color);
	context->gles.clear_pending = 0;

	/* Submitted after the acquire, signalling the present's semaphore. */
	result = vkResetFences(display->device, 1U, &surface->fence);
	if (result != VK_SUCCESS)
		return EGL_CONTEXT_LOST;

	/* The frame's submission, waiting for the acquire before the image is written. */
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &surface->acquired;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &surface->command;
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &surface->rendered;
	result = vkQueueSubmit(display->queue, 1U, &submit, surface->fence);
	if (result != VK_SUCCESS)
		return EGL_CONTEXT_LOST;

	/* Presented; a swapchain that no longer matches is made again at the next frame. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &surface->rendered;
	present.swapchainCount = 1U;
	present.pSwapchains = &surface->swapchain;
	present.pImageIndices = &image;
	result = vkQueuePresentKHR(display->queue, &present);
	if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR)
		surface->stale = 1;
	else if (result != VK_SUCCESS)
		return EGL_BAD_SURFACE;

	/* The frame is done before the command buffer is recorded again. */
	result = vkWaitForFences(display->device, 1U, &surface->fence, VK_TRUE, ZEGL_TIMEOUT);
	if (result != VK_SUCCESS)
		return EGL_CONTEXT_LOST;

	/* The size the window's image was attached at. */
	if (surface->window != NULL) {
		surface->window->attached_width = (int)surface->extent.width;
		surface->window->attached_height = (int)surface->extent.height;
	}

	/* Succeeded: the frame is on the window. */
	surface->frames++;
	return EGL_SUCCESS;
}

/* Makes a display-direct surface: the first display's first mode on the first plane that can show it. */
static EGLint
vulkan_display_surface(
	struct zegl_surface *surface)
{
	VkDisplayPropertiesKHR displays[4];
	VkDisplayModePropertiesKHR modes[16];
	VkDisplayPlanePropertiesKHR planes[8];
	VkDisplayKHR supported[8];
	VkDisplaySurfaceCreateInfoKHR create;
	struct zegl_display *display;
	uint32_t display_count;
	uint32_t mode_count;
	uint32_t plane_count;
	uint32_t supported_count;
	uint32_t plane;
	uint32_t index;
	VkResult result;

	/* The first display and its first (preferred) mode. */
	display = surface->display;
	display_count = 4U;
	result = vkGetPhysicalDeviceDisplayPropertiesKHR(display->physical, &display_count, displays);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || display_count == 0U)
		return EGL_BAD_NATIVE_WINDOW;

	/* Its first mode, which the display prefers. */
	mode_count = 16U;
	result = vkGetDisplayModePropertiesKHR(display->physical, displays[0].display, &mode_count, modes);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || mode_count == 0U)
		return EGL_BAD_NATIVE_WINDOW;

	/* A plane that can show that display. */
	plane_count = 8U;
	result = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(display->physical, &plane_count, planes);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || plane_count == 0U)
		return EGL_BAD_NATIVE_WINDOW;

	/* The first plane whose supported displays include it. */
	for (plane = 0U; plane < plane_count; plane++) {
		supported_count = 8U;
		result = vkGetDisplayPlaneSupportedDisplaysKHR(display->physical, plane, &supported_count, supported);
		if (result != VK_SUCCESS && result != VK_INCOMPLETE)
			continue;
		for (index = 0U; index < supported_count; index++) {
			if (supported[index] == displays[0].display)
				break;
		}

		/* This plane shows the display. */
		if (index < supported_count)
			break;
	}

	/* No plane shows it. */
	if (plane == plane_count)
		return EGL_BAD_NATIVE_WINDOW;

	/* The whole mode, opaque, unrotated. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
	create.displayMode = modes[0].displayMode;
	create.planeIndex = plane;
	create.planeStackIndex = planes[plane].currentStackIndex;
	create.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	create.globalAlpha = 1.0f;
	create.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	create.imageExtent = modes[0].parameters.visibleRegion;
	result = vkCreateDisplayPlaneSurfaceKHR(display->instance, &create, NULL, &surface->surface);
	if (result != VK_SUCCESS)
		return EGL_BAD_NATIVE_WINDOW;

	/* Succeeded: the surface covers the display. */
	return EGL_SUCCESS;
}

/* Makes (or makes again) the swapchain at the window's size, and a view and a framebuffer per image. */
static EGLint
vulkan_swapchain(
	struct zegl_surface *surface)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[16];
	VkPresentModeKHR modes[8];
	VkSwapchainCreateInfoKHR create;
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	struct zegl_display *display;
	VkSwapchainKHR old;
	VkPresentModeKHR mode;
	uint32_t count;
	uint32_t index;
	VkResult result;

	/* The old framebuffers go; the old swapchain is handed to the new one. */
	display = surface->display;
	(void)vkDeviceWaitIdle(display->device);
	vulkan_swapchain_free(surface, 1);
	old = surface->swapchain;

	/* An 8-bit UNORM format the surface takes. */
	count = 16U;
	result = vkGetPhysicalDeviceSurfaceFormatsKHR(display->physical, surface->surface, &count, formats);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return EGL_BAD_NATIVE_WINDOW;
	surface->format = VK_FORMAT_UNDEFINED;
	for (index = 0U; index < count; index++) {
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM || formats[index].format == VK_FORMAT_R8G8B8A8_UNORM) {
			surface->format = formats[index].format;
			break;
		}
	}

	/* Without one the colours cannot be shown as GLES writes them. */
	if (surface->format == VK_FORMAT_UNDEFINED)
		return EGL_BAD_NATIVE_WINDOW;

	/* The size: the Wayland window's, or the surface's own. */
	result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(display->physical, surface->surface, &capabilities);
	if (result != VK_SUCCESS)
		return EGL_BAD_NATIVE_WINDOW;
	surface->extent = capabilities.currentExtent;
	if (surface->window != NULL) {
		surface->extent.width = (uint32_t)surface->window->width;
		surface->extent.height = (uint32_t)surface->window->height;
		surface->window_generation = surface->window->generation;
	}

	/* FIFO, or MAILBOX for a swap interval of 0 when the surface has it. */
	mode = VK_PRESENT_MODE_FIFO_KHR;
	count = 8U;
	result = vkGetPhysicalDeviceSurfacePresentModesKHR(display->physical, surface->surface, &count, modes);
	for (index = 0U; surface->interval == 0 && (result == VK_SUCCESS || result == VK_INCOMPLETE) && index < count; index++) {
		if (modes[index] == VK_PRESENT_MODE_MAILBOX_KHR)
			mode = VK_PRESENT_MODE_MAILBOX_KHR;
	}

	/* The swapchain: three images when allowed, opaque. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.surface = surface->surface;
	create.minImageCount = 3U;
	if (create.minImageCount < capabilities.minImageCount)
		create.minImageCount = capabilities.minImageCount;
	if (capabilities.maxImageCount != 0U && create.minImageCount > capabilities.maxImageCount)
		create.minImageCount = capabilities.maxImageCount;
	create.imageFormat = surface->format;
	create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	create.imageExtent = surface->extent;
	create.imageArrayLayers = 1U;
	create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform = capabilities.currentTransform;
	create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	create.presentMode = mode;
	create.clipped = VK_TRUE;
	create.oldSwapchain = old;
	surface->swapchain = VK_NULL_HANDLE;
	result = vkCreateSwapchainKHR(display->device, &create, NULL, &surface->swapchain);
	if (old != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(display->device, old, NULL);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* Its images. */
	surface->image_count = ZEGL_IMAGES;
	result = vkGetSwapchainImagesKHR(display->device, surface->swapchain, &surface->image_count, surface->images);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return EGL_BAD_ALLOC;

	/* A view and a framebuffer for each. */
	for (index = 0U; index < surface->image_count; index++) {
		memset(&view, 0, sizeof(view));
		view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.image = surface->images[index];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = surface->format;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.levelCount = 1U;
		view.subresourceRange.layerCount = 1U;
		result = vkCreateImageView(display->device, &view, NULL, &surface->views[index]);
		if (result != VK_SUCCESS)
			return EGL_BAD_ALLOC;

		/* The framebuffer over the view. */
		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer.renderPass = surface->pass;
		framebuffer.attachmentCount = 1U;
		framebuffer.pAttachments = &surface->views[index];
		framebuffer.width = surface->extent.width;
		framebuffer.height = surface->extent.height;
		framebuffer.layers = 1U;
		result = vkCreateFramebuffer(display->device, &framebuffer, NULL, &surface->framebuffers[index]);
		if (result != VK_SUCCESS)
			return EGL_BAD_ALLOC;
	}

	/* Succeeded: the swapchain matches the window. */
	surface->stale = 0;
	return EGL_SUCCESS;
}

/* Releases the framebuffers and views, and the swapchain unless it is kept to hand to the next one. */
static void
vulkan_swapchain_free(
	struct zegl_surface *surface,
	int keep_swapchain)
{
	VkDevice device;
	uint32_t index;

	/* Each image's framebuffer and view. */
	device = surface->display->device;
	for (index = 0U; index < ZEGL_IMAGES; index++) {
		if (surface->framebuffers[index] != VK_NULL_HANDLE)
			vkDestroyFramebuffer(device, surface->framebuffers[index], NULL);
		if (surface->views[index] != VK_NULL_HANDLE)
			vkDestroyImageView(device, surface->views[index], NULL);
		surface->framebuffers[index] = VK_NULL_HANDLE;
		surface->views[index] = VK_NULL_HANDLE;
	}

	/* No images until the next swapchain. */
	surface->image_count = 0U;

	/* The swapchain itself, when it is not handed on. */
	if (!keep_swapchain && surface->swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(device, surface->swapchain, NULL);
		surface->swapchain = VK_NULL_HANDLE;
	}
}

/* Makes the frame's render pass, command buffer, fence and semaphores. */
static EGLint
vulkan_frame_objects(
	struct zegl_surface *surface)
{
	VkSurfaceFormatKHR formats[16];
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo pass;
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	struct zegl_display *display;
	VkFormat format;
	uint32_t count;
	uint32_t index;
	VkResult result;

	/* The format the swapchain will have (the pass is made once, before it). */
	display = surface->display;
	count = 16U;
	result = vkGetPhysicalDeviceSurfaceFormatsKHR(display->physical, surface->surface, &count, formats);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return EGL_BAD_NATIVE_WINDOW;
	format = VK_FORMAT_UNDEFINED;
	for (index = 0U; index < count; index++) {
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM || formats[index].format == VK_FORMAT_R8G8B8A8_UNORM) {
			format = formats[index].format;
			break;
		}
	}

	/* Without an 8-bit UNORM format there is no frame. */
	if (format == VK_FORMAT_UNDEFINED)
		return EGL_BAD_NATIVE_WINDOW;

	/* The pass: the image cleared, stored and left for presenting. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass.attachmentCount = 1U;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1U;
	pass.pSubpasses = &subpass;
	result = vkCreateRenderPass(display->device, &pass, NULL, &surface->pass);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* A pool whose one buffer is recorded again each frame. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = display->family;
	result = vkCreateCommandPool(display->device, &pool, NULL, &surface->pool);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* The command buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = surface->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	result = vkAllocateCommandBuffers(display->device, &command, &surface->command);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* The fence of a frame and the two semaphores. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(display->device, &fence, NULL, &surface->fence);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* The semaphore the acquire signals, and the one the present waits for. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	result = vkCreateSemaphore(display->device, &semaphore, NULL, &surface->acquired);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* The second of the two. */
	result = vkCreateSemaphore(display->device, &semaphore, NULL, &surface->rendered);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* Succeeded: frames can be recorded. */
	return EGL_SUCCESS;
}

/* Records a frame: the pass over an image, cleared to a colour. */
static void
vulkan_record(
	struct zegl_surface *surface,
	uint32_t image,
	const float *color)
{
	VkCommandBufferBeginInfo begin;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;

	/* One submission of this recording. */
	(void)vkResetCommandBuffer(surface->command, 0U);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	(void)vkBeginCommandBuffer(surface->command, &begin);

	/* The pass clears the whole image to the colour. */
	memset(&clear, 0, sizeof(clear));
	memcpy(clear.color.float32, color, 4U * sizeof(float));
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = surface->pass;
	pass.framebuffer = surface->framebuffers[image];
	pass.renderArea.extent = surface->extent;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(surface->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(surface->command);

	/* The recording is complete. */
	(void)vkEndCommandBuffer(surface->command);
}
