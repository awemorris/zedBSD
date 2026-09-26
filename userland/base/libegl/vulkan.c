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
 * show it (VK_KHR_display).
 *
 * A frame opens at libGLESv2's first command after a swap (or at the swap
 * itself): the image is acquired and one command buffer records the
 * frame.  Its first render pass clears the image; a later one, after a
 * readback left the pass, loads what the earlier ones drew.  Outside a
 * pass the colour image is always ready to present, so a pass, a copy
 * and the present can follow each other in any order.  eglSwapBuffers
 * submits the recording, presents, and waits for the frame before the
 * next one.
 */

#include "zegl.h"

#include <stdlib.h>
#include <string.h>

/* How long one frame may take on the GPU, in nanoseconds. */
#define ZEGL_TIMEOUT		10000000000ULL

static EGLint vulkan_display_surface(struct zegl_surface *surface);
static EGLint vulkan_swapchain(struct zegl_surface *surface);
static void vulkan_swapchain_free(struct zegl_surface *surface, int keep_swapchain);
static EGLint vulkan_depth(struct zegl_surface *surface);
static EGLint vulkan_frame_objects(struct zegl_surface *surface);
static VkFormat vulkan_depth_format(struct zegl_display *display, VkImageAspectFlags *aspects);
static EGLint vulkan_pass(struct zegl_surface *surface, VkFormat format, VkAttachmentLoadOp load, VkRenderPass *pass);
static EGLint vulkan_submit(struct zegl_surface *surface, int present);
static uint32_t vulkan_memory_type(struct zegl_display *display, uint32_t bits, VkMemoryPropertyFlags flags);

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
	if (surface->pass_load != VK_NULL_HANDLE)
		vkDestroyRenderPass(device, surface->pass_load, NULL);
	if (surface->pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(device, surface->pass, NULL);

	/* The Vulkan surface last. */
	if (surface->surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(surface->display->instance, surface->surface, NULL);
}

/*
 * Presents a window surface's frame with a context's state, and waits for
 * it: a frame nothing was drawn into is cleared to the context's clear
 * colour.  Returns EGL_SUCCESS, or EGL_BAD_SURFACE (or EGL_CONTEXT_LOST)
 * when the frame could not be shown.
 */
EGLint
zegl_surface_present(
	struct zegl_surface *surface,
	struct zegl_context *context)
{
	VkClearValue clear[2];
	EGLint error;

	/* The frame, opened now when GLES drew nothing. */
	error = zegl_frame_begin(surface);
	if (error != EGL_SUCCESS)
		return error;

	/* A frame without a pass is cleared to the clear colour, which also readies its image for presenting. */
	if (surface->passes == 0U) {
		memset(clear, 0, sizeof(clear));
		memcpy(clear[0].color.float32, context->gles.clear_color, 4U * sizeof(float));
		clear[1].depthStencil.depth = 1.0f;
		zegl_frame_pass(surface, clear);
	}

	/* Submitted, presented and waited for. */
	error = vulkan_submit(surface, 1);
	if (error != EGL_SUCCESS)
		return error;

	/* The size the window's image was attached at. */
	if (surface->window != NULL) {
		surface->window->attached_width = (int)surface->extent.width;
		surface->window->attached_height = (int)surface->extent.height;
	}

	/* GLES's per-frame resources are free again. */
	if (context->gles.frame_done != NULL)
		context->gles.frame_done(context);

	/* Succeeded: the frame is on the window. */
	surface->frames++;
	return EGL_SUCCESS;
}

/*
 * Opens a surface's frame unless it is open: a new swapchain when the
 * window was resized, the next image, and the command buffer recording.
 * Returns EGL_SUCCESS, or EGL_BAD_SURFACE when no image can be had.
 */
EGLint
zegl_frame_begin(
	struct zegl_surface *surface)
{
	VkCommandBufferBeginInfo begin;
	struct zegl_display *display;
	unsigned tries;
	VkResult result;
	EGLint error;

	/* An open frame goes on. */
	if (surface->frame_open)
		return EGL_SUCCESS;

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
		result = vkAcquireNextImageKHR(display->device, surface->swapchain, ZEGL_TIMEOUT, surface->acquired, VK_NULL_HANDLE, &surface->image);
		if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
			break;

		/* Out of date: once more with a new swapchain. */
		if (result != VK_ERROR_OUT_OF_DATE_KHR || tries >= 1U)
			return EGL_BAD_SURFACE;
		surface->stale = 1;
	}

	/* The command buffer records the frame. */
	(void)vkResetCommandBuffer(surface->command, 0U);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(surface->command, &begin);
	if (result != VK_SUCCESS)
		return EGL_BAD_SURFACE;

	/* Succeeded: the frame is open, with no pass yet. */
	surface->frame_open = 1;
	surface->in_pass = 0;
	surface->passes = 0U;
	surface->acquire_waited = 0;
	return EGL_SUCCESS;
}

/*
 * Enters a render pass over the frame's image unless one is open.  The
 * frame's first pass clears the colour to clear[0] and the depth and
 * stencil to clear[1] (NULL: black, 1 and 0); later passes load them.
 */
void
zegl_frame_pass(
	struct zegl_surface *surface,
	const VkClearValue *clear)
{
	VkRenderPassBeginInfo pass;
	VkClearValue values[2];

	/* An open pass goes on. */
	if (surface->in_pass)
		return;

	/* The clear values of a first pass. */
	memset(values, 0, sizeof(values));
	values[1].depthStencil.depth = 1.0f;
	if (clear != NULL)
		memcpy(values, clear, sizeof(values));

	/* The first pass clears, a later one loads. */
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = surface->pass_load;
	if (surface->passes == 0U) {
		pass.renderPass = surface->pass;
		pass.clearValueCount = 2U;
		pass.pClearValues = values;
	}

	/* The pass over the image's framebuffer. */
	pass.framebuffer = surface->framebuffers[surface->image];
	pass.renderArea.extent = surface->extent;
	vkCmdBeginRenderPass(surface->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	surface->in_pass = 1;
	surface->passes++;
}

/*
 * Leaves the frame's render pass, if one is open.
 */
void
zegl_frame_leave_pass(
	struct zegl_surface *surface)
{
	/* Only an open pass ends. */
	if (!surface->in_pass)
		return;

	/* The image is ready to present again. */
	vkCmdEndRenderPass(surface->command);
	surface->in_pass = 0;
}

/*
 * Submits what the frame recorded so far and waits for it, keeping the
 * frame open for more (glReadPixels).  Returns EGL_SUCCESS, or
 * EGL_CONTEXT_LOST.
 */
EGLint
zegl_frame_flush(
	struct zegl_surface *surface)
{
	VkCommandBufferBeginInfo begin;
	VkResult result;
	EGLint error;

	/* The recording so far, done. */
	error = vulkan_submit(surface, 0);
	if (error != EGL_SUCCESS)
		return error;

	/* The command buffer records the rest of the frame. */
	(void)vkResetCommandBuffer(surface->command, 0U);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(surface->command, &begin);
	if (result != VK_SUCCESS)
		return EGL_CONTEXT_LOST;

	/* Succeeded: the frame is open again. */
	surface->frame_open = 1;
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
	VkImageView attachments[2];
	struct zegl_display *display;
	VkSwapchainKHR old;
	EGLint error;
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
	surface->readable = 0;
	if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U) {
		create.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		surface->readable = 1;
	}

	/* One queue family uses the images. */
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

	/* The depth buffer at the new size. */
	error = vulkan_depth(surface);
	if (error != EGL_SUCCESS)
		return error;

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

		/* The framebuffer over the view, and the depth buffer's when there is one. */
		attachments[0] = surface->views[index];
		attachments[1] = surface->depth_view;
		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer.renderPass = surface->pass;
		framebuffer.attachmentCount = 1U;
		if (surface->depth_format != VK_FORMAT_UNDEFINED)
			framebuffer.attachmentCount = 2U;
		framebuffer.pAttachments = attachments;
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

	/* The depth buffer goes with the size it had. */
	if (surface->depth_view != VK_NULL_HANDLE)
		vkDestroyImageView(device, surface->depth_view, NULL);
	if (surface->depth_image != VK_NULL_HANDLE)
		vkDestroyImage(device, surface->depth_image, NULL);
	if (surface->depth_memory != VK_NULL_HANDLE)
		vkFreeMemory(device, surface->depth_memory, NULL);
	surface->depth_view = VK_NULL_HANDLE;
	surface->depth_image = VK_NULL_HANDLE;
	surface->depth_memory = VK_NULL_HANDLE;

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
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	struct zegl_display *display;
	VkFormat format;
	uint32_t count;
	uint32_t index;
	VkResult result;
	EGLint error;

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

	/* The depth and stencil format, when the config has either. */
	surface->depth_format = VK_FORMAT_UNDEFINED;
	surface->depth_aspects = 0U;
	if (surface->config != NULL && (surface->config->depth > 0 || surface->config->stencil > 0))
		surface->depth_format = vulkan_depth_format(display, &surface->depth_aspects);

	/* The pass that clears, then the one that loads. */
	error = vulkan_pass(surface, format, VK_ATTACHMENT_LOAD_OP_CLEAR, &surface->pass);
	if (error != EGL_SUCCESS)
		return error;
	error = vulkan_pass(surface, format, VK_ATTACHMENT_LOAD_OP_LOAD, &surface->pass_load);
	if (error != EGL_SUCCESS)
		return error;

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

/* Makes the depth buffer at the surface's size, when its config has one. */
static EGLint
vulkan_depth(
	struct zegl_surface *surface)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	struct zegl_display *display;
	VkResult result;

	/* Nothing without a depth format. */
	display = surface->display;
	if (surface->depth_format == VK_FORMAT_UNDEFINED)
		return EGL_SUCCESS;

	/* The image, the swapchain's size. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = surface->depth_format;
	image.extent.width = surface->extent.width;
	image.extent.height = surface->extent.height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(display->device, &image, NULL, &surface->depth_image);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* Its memory, on the device. */
	vkGetImageMemoryRequirements(display->device, surface->depth_image, &requirements);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = vulkan_memory_type(display, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	result = vkAllocateMemory(display->device, &allocate, NULL, &surface->depth_memory);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* Bound to the image. */
	result = vkBindImageMemory(display->device, surface->depth_image, surface->depth_memory, 0U);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* The view the framebuffers use. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = surface->depth_image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = surface->depth_format;
	view.subresourceRange.aspectMask = surface->depth_aspects;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	result = vkCreateImageView(display->device, &view, NULL, &surface->depth_view);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* Succeeded: the depth buffer. */
	return EGL_SUCCESS;
}

/* Returns the first depth and stencil format the device can attach, and its aspects; VK_FORMAT_UNDEFINED when none. */
static VkFormat
vulkan_depth_format(
	struct zegl_display *display,
	VkImageAspectFlags *aspects)
{
	static const VkFormat formats[] = {
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D16_UNORM_S8_UINT,
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_X8_D24_UNORM_PACK32,
		VK_FORMAT_D16_UNORM
	};
	VkFormatProperties properties;
	unsigned index;

	/* The first one with a stencil the device can attach, else one without. */
	for (index = 0U; index < sizeof(formats) / sizeof(formats[0]); index++) {
		vkGetPhysicalDeviceFormatProperties(display->physical, formats[index], &properties);
		if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0U)
			continue;

		/* The first three have a stencil. */
		*aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (index < 3U)
			*aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
		return formats[index];
	}

	/* The device has none. */
	*aspects = 0U;
	return VK_FORMAT_UNDEFINED;
}

/*
 * Makes a render pass over the colour image (and the depth buffer): one
 * that clears both, or one that loads what an earlier pass left.  Both
 * leave the colour image ready to present and wait for earlier passes and
 * copies.
 */
static EGLint
vulkan_pass(
	struct zegl_surface *surface,
	VkFormat format,
	VkAttachmentLoadOp load,
	VkRenderPass *pass)
{
	VkAttachmentDescription attachments[2];
	VkAttachmentReference colour;
	VkAttachmentReference depth;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo create;
	VkResult result;

	/* The colour image: undefined before a clearing pass, ready to present before a loading one and after either. */
	memset(attachments, 0, sizeof(attachments));
	attachments[0].format = format;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = load;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	if (load == VK_ATTACHMENT_LOAD_OP_CLEAR)
		attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	colour.attachment = 0U;
	colour.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	/* The depth buffer, kept between the passes of a frame. */
	attachments[1].format = surface->depth_format;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = load;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].stencilLoadOp = load;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	if (load == VK_ATTACHMENT_LOAD_OP_CLEAR)
		attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depth.attachment = 1U;
	depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	/* One subpass drawing into both. */
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &colour;
	if (surface->depth_format != VK_FORMAT_UNDEFINED)
		subpass.pDepthStencilAttachment = &depth;

	/* Earlier passes, copies and the acquire come first. */
	memset(&dependency, 0, sizeof(dependency));
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0U;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	/* The pass. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = 1U;
	if (surface->depth_format != VK_FORMAT_UNDEFINED)
		create.attachmentCount = 2U;
	create.pAttachments = attachments;
	create.subpassCount = 1U;
	create.pSubpasses = &subpass;
	create.dependencyCount = 1U;
	create.pDependencies = &dependency;
	result = vkCreateRenderPass(surface->display->device, &create, NULL, pass);
	if (result != VK_SUCCESS)
		return EGL_BAD_ALLOC;

	/* Succeeded: the pass. */
	return EGL_SUCCESS;
}

/*
 * Ends and submits the frame's recording and waits for it: the first
 * submission of a frame waits for the acquire; the present's also signals
 * the semaphore the present waits for, and then presents and closes the
 * frame.
 */
static EGLint
vulkan_submit(
	struct zegl_surface *surface,
	int present)
{
	VkSubmitInfo submit;
	VkPresentInfoKHR presenting;
	VkPipelineStageFlags stage;
	struct zegl_display *display;
	VkResult result;

	/* The recording ends outside any pass. */
	display = surface->display;
	zegl_frame_leave_pass(surface);
	result = vkEndCommandBuffer(surface->command);
	surface->frame_open = 0;
	if (result != VK_SUCCESS)
		return EGL_CONTEXT_LOST;

	/* The fence of this submission. */
	result = vkResetFences(display->device, 1U, &surface->fence);
	if (result != VK_SUCCESS)
		return EGL_CONTEXT_LOST;

	/* The submission; the frame's first waits for the acquire before anything touches the image. */
	stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	if (!surface->acquire_waited) {
		submit.waitSemaphoreCount = 1U;
		submit.pWaitSemaphores = &surface->acquired;
		submit.pWaitDstStageMask = &stage;
		surface->acquire_waited = 1;
	}

	/* The command buffer, and the semaphore the present waits for when this is the present's submission. */
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &surface->command;
	if (present) {
		submit.signalSemaphoreCount = 1U;
		submit.pSignalSemaphores = &surface->rendered;
	}

	/* Submitted. */
	result = vkQueueSubmit(display->queue, 1U, &submit, surface->fence);
	if (result != VK_SUCCESS)
		return EGL_CONTEXT_LOST;

	/* Presented; a swapchain that no longer matches is made again at the next frame. */
	if (present) {
		memset(&presenting, 0, sizeof(presenting));
		presenting.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presenting.waitSemaphoreCount = 1U;
		presenting.pWaitSemaphores = &surface->rendered;
		presenting.swapchainCount = 1U;
		presenting.pSwapchains = &surface->swapchain;
		presenting.pImageIndices = &surface->image;
		result = vkQueuePresentKHR(display->queue, &presenting);
		if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR)
			surface->stale = 1;
		else if (result != VK_SUCCESS)
			return EGL_BAD_SURFACE;
	}

	/* The recording is done before the command buffer is recorded again. */
	result = vkWaitForFences(display->device, 1U, &surface->fence, VK_TRUE, ZEGL_TIMEOUT);
	if (result != VK_SUCCESS)
		return EGL_CONTEXT_LOST;

	/* Succeeded: the GPU finished the recording. */
	return EGL_SUCCESS;
}

/* Returns the first memory type of a set that has the properties asked for (else the set's first). */
static uint32_t
vulkan_memory_type(
	struct zegl_display *display,
	uint32_t bits,
	VkMemoryPropertyFlags flags)
{
	VkPhysicalDeviceMemoryProperties properties;
	uint32_t first;
	uint32_t index;

	/* The device's memory types. */
	vkGetPhysicalDeviceMemoryProperties(display->physical, &properties);

	/* The first allowed type with the properties, remembering the first allowed one. */
	first = 0U;
	for (index = properties.memoryTypeCount; index > 0U; index--) {
		if ((bits & (1U << (index - 1U))) != 0U)
			first = index - 1U;
	}

	/* Then the first with the properties. */
	for (index = 0U; index < properties.memoryTypeCount; index++) {
		if ((bits & (1U << index)) == 0U)
			continue;
		if ((properties.memoryTypes[index].propertyFlags & flags) == flags)
			return index;
	}

	/* None has them: the first allowed type. */
	return first;
}
