/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The server's Vulkan, and showing a surface of the backend through it.
 *
 * The instance and the device are made when the server starts, through
 * the loader's vkGetInstanceProcAddr the program gives (every Vulkan
 * function is found through it: render-vulkan.h).  The drawing layer
 * (render-vulkan.c) draws the windows and pixmaps on that device.
 *
 * Each surface shown through Vulkan has a swapchain on the VkSurfaceKHR
 * the backend makes for it.  A frame of a window whose pixels are on the
 * GPU is a blit of its image into the swapchain image it acquired (scaled
 * to the surface), recorded after the drawing and presented; a window
 * whose pixels are in memory only is copied through a staging buffer.
 * Nothing waits for a free image: without one the frame is left for a
 * later tick, as the backend's own present waits for a frame the system
 * has released.  Only the Vulkan API is used here; the system's window is
 * the backend's.
 */

#include "userland/desktop/xserver/internal.h"
#include "userland/desktop/xserver/render-vulkan.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The most swapchain images a window keeps. */
#define VULKAN_IMAGES_MAX	8U

/* The most physical devices and queue families looked at. */
#define VULKAN_DEVICES_MAX	8U
#define VULKAN_FAMILIES_MAX	16U

/* How long closing or resizing a window waits for its last copy, in nanoseconds. */
#define VULKAN_WAIT_NS		1000000000ULL

/* The alpha of every pixel shown (X's pixels have none). */
#define VULKAN_OPAQUE		0xff000000U

/* A Vulkan function's row in the table of names (its name and where it is kept). */
#define VULKAN_FUNCTION(name)	{ #name, offsetof(struct x11_vulkan_functions, name) }

/*
 * One function of the table to find: its name, and its place in the table.
 */
struct vulkan_function_name {
	const char *name;
	size_t offset;
};

/*
 * One window's swapchain: its surface and images, the size of the X
 * window it shows (the images may be the surface's scaled size), the
 * staging buffer pixels in memory go through, and what orders a frame
 * from memory (the command buffer and the fence of the last copy), the
 * acquire semaphore and one semaphore an image for its present.
 */
struct x11_vulkan_window {
	struct x11_vulkan *vulkan;
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkFormat format;
	VkExtent2D extent;
	unsigned width;
	unsigned height;
	uint32_t image_count;
	VkImage images[VULKAN_IMAGES_MAX];
	VkSemaphore rendered[VULKAN_IMAGES_MAX];

	/* Nonzero when the swapchain no longer matches the surface and is made again before the next frame. */
	int stale;

	/* The staging buffer (made with the first frame from memory), its memory and where the server writes it. */
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	uint32_t *staging_pixels;

	/* The frame from memory's command buffer, its pool, the fence of the last copy and the acquire semaphore. */
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkSemaphore acquired;
};

/*
 * The global functions, found without an instance.
 */
static const struct vulkan_function_name vulkan_global_functions[] = {
	VULKAN_FUNCTION(vkCreateInstance),
};

/*
 * The instance's and devices' functions, found with the instance.  The
 * surface's and swapchain's are left NULL when the instance or device has
 * no WSI.
 */
static const struct vulkan_function_name vulkan_instance_functions[] = {
	VULKAN_FUNCTION(vkDestroyInstance),
	VULKAN_FUNCTION(vkEnumeratePhysicalDevices),
	VULKAN_FUNCTION(vkGetPhysicalDeviceProperties),
	VULKAN_FUNCTION(vkGetPhysicalDeviceFeatures),
	VULKAN_FUNCTION(vkGetPhysicalDeviceFormatProperties),
	VULKAN_FUNCTION(vkGetPhysicalDeviceQueueFamilyProperties),
	VULKAN_FUNCTION(vkGetPhysicalDeviceMemoryProperties),
	VULKAN_FUNCTION(vkEnumerateDeviceExtensionProperties),
	VULKAN_FUNCTION(vkCreateDevice),
	VULKAN_FUNCTION(vkDestroyDevice),
	VULKAN_FUNCTION(vkDeviceWaitIdle),
	VULKAN_FUNCTION(vkGetDeviceQueue),
	VULKAN_FUNCTION(vkQueueSubmit),
	VULKAN_FUNCTION(vkAllocateMemory),
	VULKAN_FUNCTION(vkFreeMemory),
	VULKAN_FUNCTION(vkMapMemory),
	VULKAN_FUNCTION(vkInvalidateMappedMemoryRanges),
	VULKAN_FUNCTION(vkFlushMappedMemoryRanges),
	VULKAN_FUNCTION(vkCreateBuffer),
	VULKAN_FUNCTION(vkDestroyBuffer),
	VULKAN_FUNCTION(vkGetBufferMemoryRequirements),
	VULKAN_FUNCTION(vkBindBufferMemory),
	VULKAN_FUNCTION(vkCreateImage),
	VULKAN_FUNCTION(vkDestroyImage),
	VULKAN_FUNCTION(vkGetImageMemoryRequirements),
	VULKAN_FUNCTION(vkBindImageMemory),
	VULKAN_FUNCTION(vkCreateImageView),
	VULKAN_FUNCTION(vkDestroyImageView),
	VULKAN_FUNCTION(vkCreateSampler),
	VULKAN_FUNCTION(vkDestroySampler),
	VULKAN_FUNCTION(vkCreateShaderModule),
	VULKAN_FUNCTION(vkDestroyShaderModule),
	VULKAN_FUNCTION(vkCreateDescriptorSetLayout),
	VULKAN_FUNCTION(vkDestroyDescriptorSetLayout),
	VULKAN_FUNCTION(vkCreateDescriptorPool),
	VULKAN_FUNCTION(vkDestroyDescriptorPool),
	VULKAN_FUNCTION(vkResetDescriptorPool),
	VULKAN_FUNCTION(vkAllocateDescriptorSets),
	VULKAN_FUNCTION(vkUpdateDescriptorSets),
	VULKAN_FUNCTION(vkCreatePipelineLayout),
	VULKAN_FUNCTION(vkDestroyPipelineLayout),
	VULKAN_FUNCTION(vkCreateGraphicsPipelines),
	VULKAN_FUNCTION(vkDestroyPipeline),
	VULKAN_FUNCTION(vkCreateRenderPass),
	VULKAN_FUNCTION(vkDestroyRenderPass),
	VULKAN_FUNCTION(vkCreateFramebuffer),
	VULKAN_FUNCTION(vkDestroyFramebuffer),
	VULKAN_FUNCTION(vkCreateCommandPool),
	VULKAN_FUNCTION(vkDestroyCommandPool),
	VULKAN_FUNCTION(vkAllocateCommandBuffers),
	VULKAN_FUNCTION(vkResetCommandBuffer),
	VULKAN_FUNCTION(vkBeginCommandBuffer),
	VULKAN_FUNCTION(vkEndCommandBuffer),
	VULKAN_FUNCTION(vkCreateFence),
	VULKAN_FUNCTION(vkDestroyFence),
	VULKAN_FUNCTION(vkResetFences),
	VULKAN_FUNCTION(vkGetFenceStatus),
	VULKAN_FUNCTION(vkWaitForFences),
	VULKAN_FUNCTION(vkCreateSemaphore),
	VULKAN_FUNCTION(vkDestroySemaphore),
	VULKAN_FUNCTION(vkCmdPipelineBarrier),
	VULKAN_FUNCTION(vkCmdBeginRenderPass),
	VULKAN_FUNCTION(vkCmdEndRenderPass),
	VULKAN_FUNCTION(vkCmdBindPipeline),
	VULKAN_FUNCTION(vkCmdBindDescriptorSets),
	VULKAN_FUNCTION(vkCmdBindVertexBuffers),
	VULKAN_FUNCTION(vkCmdPushConstants),
	VULKAN_FUNCTION(vkCmdSetViewport),
	VULKAN_FUNCTION(vkCmdSetScissor),
	VULKAN_FUNCTION(vkCmdDraw),
	VULKAN_FUNCTION(vkCmdClearAttachments),
	VULKAN_FUNCTION(vkCmdClearColorImage),
	VULKAN_FUNCTION(vkCmdCopyImage),
	VULKAN_FUNCTION(vkCmdBlitImage),
	VULKAN_FUNCTION(vkCmdCopyBufferToImage),
	VULKAN_FUNCTION(vkCmdCopyImageToBuffer),
};

/*
 * The surface's and swapchain's functions, needed only to show windows.
 */
static const struct vulkan_function_name vulkan_wsi_functions[] = {
	VULKAN_FUNCTION(vkDestroySurfaceKHR),
	VULKAN_FUNCTION(vkGetPhysicalDeviceSurfaceSupportKHR),
	VULKAN_FUNCTION(vkGetPhysicalDeviceSurfaceCapabilitiesKHR),
	VULKAN_FUNCTION(vkGetPhysicalDeviceSurfaceFormatsKHR),
	VULKAN_FUNCTION(vkGetPhysicalDeviceSurfacePresentModesKHR),
	VULKAN_FUNCTION(vkCreateSwapchainKHR),
	VULKAN_FUNCTION(vkDestroySwapchainKHR),
	VULKAN_FUNCTION(vkGetSwapchainImagesKHR),
	VULKAN_FUNCTION(vkAcquireNextImageKHR),
	VULKAN_FUNCTION(vkQueuePresentKHR),
};

static int vulkan_functions(struct x11_vulkan *vulkan, VkInstance instance, const struct vulkan_function_name *names, size_t count);
static VkResult vulkan_instance(struct x11_vulkan *vulkan, const char *wsi);
static VkResult vulkan_device(struct x11_vulkan *vulkan);
static int vulkan_device_rank(struct x11_vulkan *vulkan, VkPhysicalDevice device, uint32_t *family);
static int vulkan_device_swapchain(struct x11_vulkan *vulkan, VkPhysicalDevice device);
static VkResult vulkan_swapchain(struct x11_vulkan_window *window, unsigned width, unsigned height);
static VkResult vulkan_staging(struct x11_vulkan_window *window);
static VkResult vulkan_frame_objects(struct x11_vulkan_window *window);
static void vulkan_swapchain_free(struct x11_vulkan_window *window);
static void vulkan_staging_free(struct x11_vulkan_window *window);
static void vulkan_copy_pixels(struct x11_vulkan_window *window, const uint32_t *pixels, size_t stride);
static VkResult vulkan_record(struct x11_vulkan_window *window, uint32_t image);
static void vulkan_barrier(struct x11_vulkan *vulkan, VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source, VkAccessFlags target);
static int vulkan_acquire(struct x11_vulkan_window *window, uint32_t *image);
static int vulkan_present(struct x11_vulkan_window *window, uint32_t image);

/*
 * Makes the server's Vulkan through the loader's vkGetInstanceProcAddr:
 * the instance (with the surface extensions when a WSI is named and the
 * loader has it), the device the windows and pixmaps are drawn on, and the
 * drawing layer.  Returns NULL when there is no Vulkan to draw or show
 * with (everything is then drawn in memory and shown by the backend).
 */
struct x11_vulkan *
x11_vulkan_open(
	struct x11server *server,
	x11server_vulkan_function loader,
	const char *wsi)
{
	struct x11_vulkan *vulkan;
	VkResult error;
	int found;

	/* The state, and the loader's lookup. */
	vulkan = calloc(1U, sizeof(*vulkan));
	if (vulkan == NULL)
		return NULL;
	vulkan->server = server;
	vulkan->vk.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)loader;

	/* The global functions. */
	found = vulkan_functions(vulkan, VK_NULL_HANDLE, vulkan_global_functions, sizeof(vulkan_global_functions) / sizeof(vulkan_global_functions[0]));
	if (!found) {
		x11_log(server, "X11SERVER VULKAN none (the loader lacks vkCreateInstance)");
		free(vulkan);
		return NULL;
	}

	/* The instance, with the WSI when it can have it. */
	error = vulkan_instance(vulkan, wsi);
	if (error != VK_SUCCESS) {
		x11_log(server, "X11SERVER VULKAN none (vkCreateInstance: %d)", (int)error);
		free(vulkan);
		return NULL;
	}

	/* Its functions. */
	found = vulkan_functions(vulkan, vulkan->instance, vulkan_instance_functions, sizeof(vulkan_instance_functions) / sizeof(vulkan_instance_functions[0]));
	if (!found) {
		x11_log(server, "X11SERVER VULKAN none (a function is missing)");
		x11_vulkan_close(vulkan);
		return NULL;
	}

	/* The surface's and swapchain's functions; without them windows are shown by the backend. */
	if (vulkan->wsi) {
		found = vulkan_functions(vulkan, vulkan->instance, vulkan_wsi_functions, sizeof(vulkan_wsi_functions) / sizeof(vulkan_wsi_functions[0]));
		if (!found)
			vulkan->wsi = 0;
	}

	/* The device. */
	error = vulkan_device(vulkan);
	if (error != VK_SUCCESS) {
		x11_log(server, "X11SERVER VULKAN none (no device: %d)", (int)error);
		x11_vulkan_close(vulkan);
		return NULL;
	}

	/* The drawing layer; without it, windows and pixmaps are drawn in memory (and may still be shown through Vulkan). */
	error = (VkResult)x11_render_vulkan_open(vulkan);
	if (error != 0) {
		x11_log(server, "X11SERVER RENDER memory (the device cannot draw)");
		x11_render_vulkan_close(vulkan);
	}

	/* Succeeded: drawing and showing through Vulkan. */
	x11_log(server, "X11SERVER VULKAN device ready family=%u wsi=%d", (unsigned)vulkan->family, vulkan->wsi);
	return vulkan;
}

/*
 * Ends the server's Vulkan (every window closed first): the drawing
 * layer, the device and the instance.
 */
void
x11_vulkan_close(
	struct x11_vulkan *vulkan)
{
	/* Nothing was made. */
	if (vulkan == NULL)
		return;

	/* The drawing layer, then the device after everything on it. */
	if (vulkan->device != VK_NULL_HANDLE) {
		x11_render_vulkan_close(vulkan);
		(void)vulkan->vk.vkDeviceWaitIdle(vulkan->device);
		vulkan->vk.vkDestroyDevice(vulkan->device, NULL);
	}

	/* The instance. */
	if (vulkan->instance != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyInstance(vulkan->instance, NULL);
	free(vulkan);
}

/*
 * Tells whether windows can be shown through Vulkan swapchains.
 */
int
x11_vulkan_shows(
	const struct x11_vulkan *vulkan)
{
	/* No Vulkan, or none that presents. */
	if (vulkan == NULL || !vulkan->wsi || vulkan->lost)
		return 0;

	/* Succeeded: swapchains can be made. */
	return 1;
}

/*
 * Tells whether the device was lost (or failed): the server then ends its
 * clients and goes on without Vulkan.
 */
int
x11_vulkan_lost(
	const struct x11_vulkan *vulkan)
{
	/* No Vulkan to lose. */
	if (vulkan == NULL)
		return 0;

	/* Succeeded: whether it failed. */
	return vulkan->lost;
}

/*
 * Gives a memory type of those allowed with every one of the wanted
 * properties, or UINT32_MAX.
 */
uint32_t
x11_vulkan_memory_type(
	const struct x11_vulkan *vulkan,
	uint32_t allowed,
	VkMemoryPropertyFlags wanted)
{
	uint32_t type;

	/* The first allowed type that has them. */
	for (type = 0U; type < vulkan->memory.memoryTypeCount; type++) {
		if ((allowed & (1U << type)) == 0U)
			continue;
		if ((vulkan->memory.memoryTypes[type].propertyFlags & wanted) != wanted)
			continue;
		return type;
	}

	/* None. */
	return UINT32_MAX;
}

/*
 * Gives the device up after a call failed: nothing more is drawn or
 * shown on it, and the log says which call and why.
 */
void
x11_vulkan_failed(
	struct x11_vulkan *vulkan,
	VkResult error,
	const char *what)
{
	/* Said once. */
	if (vulkan->lost)
		return;

	/* Succeeded: the device is given up. */
	vulkan->lost = 1;
	x11_log(vulkan->server, "X11SERVER VULKAN lost (%s: %d)", what, (int)error);
}

/*
 * Opens the swapchain of a surface, on a VkSurfaceKHR the backend makes.
 * Returns NULL when Vulkan cannot show it (the backend's present then
 * does).
 */
struct x11_vulkan_window *
x11_vulkan_window_open(
	struct x11server *server,
	struct x11_surface *surface,
	unsigned width,
	unsigned height)
{
	struct x11_vulkan_window *window;
	struct x11_vulkan *vulkan;
	VkBool32 supported;
	VkResult error;
	int shows;
	int status;

	/* A backend that makes no Vulkan surface, or a server without Vulkan's windows, shows its windows itself. */
	vulkan = server->vulkan;
	shows = x11_vulkan_shows(vulkan);
	if (server->ops->vulkan_surface == NULL || !shows)
		return NULL;

	/* The window. */
	window = calloc(1U, sizeof(*window));
	if (window == NULL)
		return NULL;
	window->vulkan = vulkan;

	/* The Vulkan surface of the backend's window. */
	status = server->ops->vulkan_surface(server->backend, surface, (void *)vulkan->instance, &window->surface);
	if (status != 0) {
		window->surface = VK_NULL_HANDLE;
		x11_vulkan_window_close(window);
		return NULL;
	}

	/* The device's queue must present to it. */
	supported = VK_FALSE;
	error = vulkan->vk.vkGetPhysicalDeviceSurfaceSupportKHR(vulkan->physical, vulkan->family, window->surface, &supported);
	if (error != VK_SUCCESS || supported == VK_FALSE) {
		x11_log(server, "X11SERVER VULKAN the device does not present to the surface");
		x11_vulkan_window_close(window);
		return NULL;
	}

	/* The command buffer, the fence and the acquire semaphore. */
	error = vulkan_frame_objects(window);
	if (error != VK_SUCCESS) {
		x11_vulkan_window_close(window);
		return NULL;
	}

	/* The swapchain at the window's size. */
	status = x11_vulkan_window_resize(window, width, height);
	if (status != 0) {
		x11_vulkan_window_close(window);
		return NULL;
	}

	/* Succeeded: the window can be shown. */
	return window;
}

/*
 * Shows a window's pixels in memory (rows of its width, stride pixels
 * apart, 0x00RRGGBB): copied through the staging buffer into a free
 * swapchain image and presented.  Returns 0 when presented, 1 when the
 * frame must wait (the last copy runs, or no image is free), or -1 when
 * Vulkan failed or the surface is not the window's size.
 */
int
x11_vulkan_window_present(
	struct x11_vulkan_window *window,
	const uint32_t *pixels,
	size_t stride)
{
	struct x11_vulkan *vulkan;
	VkPipelineStageFlags stage;
	VkSubmitInfo submit;
	uint32_t image;
	VkResult error;
	int status;

	/* A chain of another size than the window (a scaled surface) cannot take the pixels as they are. */
	vulkan = window->vulkan;
	if (window->extent.width != window->width || window->extent.height != window->height)
		return -1;

	/* The staging buffer, made the first time. */
	if (window->staging == VK_NULL_HANDLE) {
		error = vulkan_staging(window);
		if (error != VK_SUCCESS)
			return -1;
	}

	/* The last copy must be done before the staging buffer is written again. */
	error = vulkan->vk.vkGetFenceStatus(vulkan->device, window->fence);
	if (error == VK_NOT_READY)
		return 1;
	if (error != VK_SUCCESS)
		return -1;

	/* A free image, without waiting for one. */
	status = vulkan_acquire(window, &image);
	if (status != 0)
		return status;

	/* The pixels into the staging buffer, and the copy into the image recorded. */
	vulkan_copy_pixels(window, pixels, stride);
	error = vulkan_record(window, image);
	if (error != VK_SUCCESS)
		return -1;

	/* The copy, after the acquire, signalling the image's present semaphore and the fence. */
	error = vulkan->vk.vkResetFences(vulkan->device, 1U, &window->fence);
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
	error = vulkan->vk.vkQueueSubmit(vulkan->queue, 1U, &submit, window->fence);
	if (error != VK_SUCCESS) {
		x11_vulkan_failed(vulkan, error, "vkQueueSubmit");
		return -1;
	}

	/* Succeeded or not as the present is. */
	return vulkan_present(window, image);
}

/*
 * Shows a window whose pixels are on the GPU: its image blitted into a
 * free swapchain image (scaled to the surface: nearest at a whole scale,
 * linear otherwise) after everything drawn, and presented.  Returns 0
 * when presented, 1 when the frame must wait (no image is free), or -1
 * when Vulkan failed.
 */
int
x11_vulkan_window_present_image(
	struct x11_vulkan_window *window,
	const struct x11_image *pixels)
{
	struct x11_vulkan *vulkan;
	VkCommandBuffer command;
	VkImageBlit blit;
	VkFilter filter;
	VkImage source;
	uint32_t image;
	VkResult error;
	int status;

	/* The window's image on the GPU. */
	vulkan = window->vulkan;
	source = x11_render_vulkan_image(pixels);
	if (source == VK_NULL_HANDLE)
		return -1;

	/* A free image, without waiting for one. */
	status = vulkan_acquire(window, &image);
	if (status != 0)
		return status;

	/* Recorded after the drawing. */
	command = x11_render_vulkan_command(vulkan);
	if (command == VK_NULL_HANDLE)
		return -1;

	/* The whole window into the whole chain image: nearest when the scale is whole, else linear. */
	memset(&blit, 0, sizeof(blit));
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1U;
	blit.srcOffsets[1].x = pixels->width;
	blit.srcOffsets[1].y = pixels->height;
	blit.srcOffsets[1].z = 1;
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.layerCount = 1U;
	blit.dstOffsets[1].x = (int32_t)window->extent.width;
	blit.dstOffsets[1].y = (int32_t)window->extent.height;
	blit.dstOffsets[1].z = 1;
	filter = VK_FILTER_LINEAR;
	if (window->extent.width % (uint32_t)pixels->width == 0U && window->extent.height % (uint32_t)pixels->height == 0U)
		filter = VK_FILTER_NEAREST;
	vulkan_barrier(vulkan, command, window->images[image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0U, VK_ACCESS_TRANSFER_WRITE_BIT);
	vulkan->vk.vkCmdBlitImage(command, source, VK_IMAGE_LAYOUT_GENERAL, window->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &blit, filter);
	vulkan_barrier(vulkan, command, window->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0U);

	/* Submitted after the acquire, signalling the image's present semaphore. */
	error = x11_render_vulkan_submit(vulkan, window->acquired, window->rendered[image]);
	if (error != VK_SUCCESS)
		return -1;

	/* Succeeded or not as the present is. */
	return vulkan_present(window, image);
}

/*
 * Makes a window's swapchain again for a window of a size (the images
 * the surface's size when it has one, the window's otherwise), after
 * everything that may use the old one.  Returns 0, or -1 when Vulkan
 * failed.
 */
int
x11_vulkan_window_resize(
	struct x11_vulkan_window *window,
	unsigned width,
	unsigned height)
{
	struct x11_vulkan *vulkan;
	VkResult error;

	/* Everything that may use the old chain or the staging buffer finishes first. */
	vulkan = window->vulkan;
	(void)vulkan->vk.vkDeviceWaitIdle(vulkan->device);

	/* A new chain replacing the old one, and its present semaphores. */
	window->width = width;
	window->height = height;
	error = vulkan_swapchain(window, width, height);
	if (error != VK_SUCCESS)
		return -1;

	/* The staging buffer is made again at the new size when a frame from memory needs it. */
	vulkan_staging_free(window);

	/* Succeeded: the next frame is at the new size. */
	window->stale = 0;
	return 0;
}

/*
 * Closes a window's swapchain and surface, after everything that may use
 * them.
 */
void
x11_vulkan_window_close(
	struct x11_vulkan_window *window)
{
	struct x11_vulkan *vulkan;
	VkDevice device;

	/* Everything that may use them finishes first. */
	vulkan = window->vulkan;
	device = vulkan->device;
	(void)vulkan->vk.vkDeviceWaitIdle(device);

	/* The staging buffer and the chain. */
	vulkan_staging_free(window);
	vulkan_swapchain_free(window);

	/* The frame's objects. */
	if (window->acquired != VK_NULL_HANDLE)
		vulkan->vk.vkDestroySemaphore(device, window->acquired, NULL);
	if (window->fence != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyFence(device, window->fence, NULL);
	if (window->pool != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyCommandPool(device, window->pool, NULL);

	/* The surface, and the window itself. */
	if (window->surface != VK_NULL_HANDLE)
		vulkan->vk.vkDestroySurfaceKHR(vulkan->instance, window->surface, NULL);
	free(window);
}

/*
 * Finds functions of a table of names through the loader (with the
 * instance, or none for the global ones).  Returns nonzero when every one
 * was found.
 */
static int
vulkan_functions(
	struct x11_vulkan *vulkan,
	VkInstance instance,
	const struct vulkan_function_name *names,
	size_t count)
{
	PFN_vkVoidFunction found;
	size_t index;

	/* Each function, kept at its place in the table. */
	for (index = 0U; index < count; index++) {
		found = vulkan->vk.vkGetInstanceProcAddr(instance, names[index].name);
		if (found == NULL)
			return 0;
		memcpy((char *)&vulkan->vk + names[index].offset, &found, sizeof(found));
	}

	/* Succeeded: all found. */
	return 1;
}

/*
 * Makes the instance: with VK_KHR_surface and the backend's WSI extension
 * when one is named, else (or when the loader has not got them) without.
 */
static VkResult
vulkan_instance(
	struct x11_vulkan *vulkan,
	const char *wsi)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo create;
	const char *extensions[2];
	VkResult error;

	/* The application, of Vulkan 1.0. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "xserver";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	create.pApplicationInfo = &application;

	/* With the surface extensions first, when a WSI is named. */
	if (wsi != NULL) {
		extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
		extensions[1] = wsi;
		create.enabledExtensionCount = 2U;
		create.ppEnabledExtensionNames = extensions;
		error = vulkan->vk.vkCreateInstance(&create, NULL, &vulkan->instance);
		if (error == VK_SUCCESS) {
			vulkan->wsi = 1;
			return VK_SUCCESS;
		}
		x11_log(vulkan->server, "X11SERVER VULKAN no %s (%d): windows are shown by the backend", wsi, (int)error);
	}

	/* Without. */
	create.enabledExtensionCount = 0U;
	create.ppEnabledExtensionNames = NULL;
	error = vulkan->vk.vkCreateInstance(&create, NULL, &vulkan->instance);
	if (error != VK_SUCCESS) {
		vulkan->instance = VK_NULL_HANDLE;
		return error;
	}

	/* Succeeded: an instance that draws only. */
	return VK_SUCCESS;
}

/*
 * Chooses the device (the best ranked: a GPU before a processor, one that
 * presents when windows are shown) and makes it with its graphics queue,
 * the swapchain extension when windows are shown, and logic operations
 * when it has them.
 */
static VkResult
vulkan_device(
	struct x11_vulkan *vulkan)
{
	VkPhysicalDevice devices[VULKAN_DEVICES_MAX];
	VkPhysicalDeviceProperties properties;
	VkPhysicalDeviceFeatures features;
	VkPhysicalDeviceFeatures enabled;
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo create;
	const char *extension;
	uint32_t count;
	uint32_t index;
	uint32_t family;
	float priority;
	VkResult error;
	int best;
	int rank;
	int swapchain;

	/* The physical devices (the first few are enough). */
	count = VULKAN_DEVICES_MAX;
	error = vulkan->vk.vkEnumeratePhysicalDevices(vulkan->instance, &count, devices);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The best ranked one with a graphics queue. */
	best = -1;
	for (index = 0U; index < count; index++) {
		rank = vulkan_device_rank(vulkan, devices[index], &family);
		if (rank <= best)
			continue;
		best = rank;
		vulkan->physical = devices[index];
		vulkan->family = family;
	}
	if (best < 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Windows are shown only when it has the swapchain extension. */
	swapchain = 0;
	if (vulkan->wsi)
		swapchain = vulkan_device_swapchain(vulkan, vulkan->physical);
	if (!swapchain)
		vulkan->wsi = 0;

	/* What it can do that the drawing asks of it. */
	vulkan->vk.vkGetPhysicalDeviceProperties(vulkan->physical, &properties);
	vulkan->vk.vkGetPhysicalDeviceFeatures(vulkan->physical, &features);
	memset(&enabled, 0, sizeof(enabled));
	if (features.logicOp) {
		enabled.logicOp = VK_TRUE;
		vulkan->logic_op = 1;
	}
	vulkan->image_side_max = properties.limits.maxImageDimension2D;

	/* One queue of the family, and the swapchain extension when windows are shown. */
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
	if (vulkan->wsi) {
		create.enabledExtensionCount = 1U;
		create.ppEnabledExtensionNames = &extension;
	}
	create.pEnabledFeatures = &enabled;
	error = vulkan->vk.vkCreateDevice(vulkan->physical, &create, NULL, &vulkan->device);
	if (error != VK_SUCCESS) {
		vulkan->device = VK_NULL_HANDLE;
		return error;
	}

	/* Its queue and memory types. */
	vulkan->vk.vkGetDeviceQueue(vulkan->device, vulkan->family, 0U, &vulkan->queue);
	vulkan->vk.vkGetPhysicalDeviceMemoryProperties(vulkan->physical, &vulkan->memory);

	/* Succeeded: the device. */
	x11_log(vulkan->server, "X11SERVER VULKAN device \"%s\" type=%d", properties.deviceName, (int)properties.deviceType);
	return VK_SUCCESS;
}

/*
 * Ranks a device for the server (higher is better; a GPU before a
 * processor, one with the swapchain extension when windows are shown)
 * and gives its first graphics queue family.  Returns -1 when it has no
 * graphics queue or cannot draw and sample the server's format.
 */
static int
vulkan_device_rank(
	struct x11_vulkan *vulkan,
	VkPhysicalDevice device,
	uint32_t *family)
{
	VkQueueFamilyProperties families[VULKAN_FAMILIES_MAX];
	VkPhysicalDeviceProperties properties;
	VkFormatProperties format;
	VkFormatFeatureFlags wanted;
	uint32_t count;
	uint32_t index;
	int rank;
	int swapchain;

	/* Its first graphics family. */
	count = VULKAN_FAMILIES_MAX;
	vulkan->vk.vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);
	for (index = 0U; index < count; index++) {
		if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U && families[index].queueCount != 0U)
			break;
	}
	if (index == count)
		return -1;
	*family = index;

	/* It draws into, samples, blits and copies B8G8R8A8_UNORM. */
	vulkan->vk.vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_B8G8R8A8_UNORM, &format);
	wanted = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
	if ((format.optimalTilingFeatures & wanted) != wanted)
		return -1;

	/* The kind of device. */
	vulkan->vk.vkGetPhysicalDeviceProperties(device, &properties);
	rank = 1;
	if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		rank = 4;
	else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
		rank = 3;
	else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU)
		rank = 2;
	else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
		rank = 0;

	/* One that shows windows first, when they are shown. */
	if (vulkan->wsi) {
		swapchain = vulkan_device_swapchain(vulkan, device);
		if (swapchain)
			rank += 8;
	}

	/* Succeeded: its rank. */
	return rank;
}

/* Tells whether a device has the swapchain extension. */
static int
vulkan_device_swapchain(
	struct x11_vulkan *vulkan,
	VkPhysicalDevice device)
{
	VkExtensionProperties *extensions;
	uint32_t count;
	uint32_t index;
	VkResult error;
	int found;

	/* How many extensions it has. */
	count = 0U;
	error = vulkan->vk.vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL);
	if (error != VK_SUCCESS || count == 0U)
		return 0;

	/* Their names. */
	extensions = calloc(count, sizeof(*extensions));
	if (extensions == NULL)
		return 0;
	error = vulkan->vk.vkEnumerateDeviceExtensionProperties(device, NULL, &count, extensions);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE) {
		free(extensions);
		return 0;
	}

	/* The swapchain's among them. */
	found = 0;
	for (index = 0U; index < count && !found; index++) {
		if (strcmp(extensions[index].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
			found = 1;
	}
	free(extensions);

	/* Succeeded: whether it has it. */
	return found;
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
	error = vulkan->vk.vkGetPhysicalDeviceSurfaceFormatsKHR(vulkan->physical, window->surface, &count, formats);
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

	/* The surface's size when it has one (the window scaled), else the window's, kept inside what the surface takes. */
	error = vulkan->vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vulkan->physical, window->surface, &capabilities);
	if (error != VK_SUCCESS)
		return error;
	if (capabilities.currentExtent.width != UINT32_MAX && capabilities.currentExtent.width != 0U) {
		width = capabilities.currentExtent.width;
		height = capabilities.currentExtent.height;
	}
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

	/* MAILBOX when the surface offers it: a present then never waits for the system's last frame, which would hold every client. */
	mode = VK_PRESENT_MODE_FIFO_KHR;
	count = 8U;
	error = vulkan->vk.vkGetPhysicalDeviceSurfacePresentModesKHR(vulkan->physical, window->surface, &count, modes);
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
	error = vulkan->vk.vkCreateSwapchainKHR(vulkan->device, &create, NULL, &window->swapchain);
	if (error != VK_SUCCESS) {
		window->swapchain = old;
		return error;
	}

	/* The old chain and its semaphores go. */
	for (index = 0U; index < window->image_count; index++) {
		vulkan->vk.vkDestroySemaphore(vulkan->device, window->rendered[index], NULL);
		window->rendered[index] = VK_NULL_HANDLE;
	}

	/* Then the old chain itself. */
	window->image_count = 0U;
	if (old != VK_NULL_HANDLE)
		vulkan->vk.vkDestroySwapchainKHR(vulkan->device, old, NULL);

	/* The new chain's images. */
	count = VULKAN_IMAGES_MAX;
	error = vulkan->vk.vkGetSwapchainImagesKHR(vulkan->device, window->swapchain, &count, window->images);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* A present semaphore for each image. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for (index = 0U; index < count; index++) {
		error = vulkan->vk.vkCreateSemaphore(vulkan->device, &semaphore, NULL, &window->rendered[index]);
		if (error != VK_SUCCESS)
			return error;
		window->image_count = index + 1U;
	}

	/* Succeeded: the chain at the surface's size. */
	return VK_SUCCESS;
}

/* Makes the staging buffer of the chain's size in host-visible, coherent memory, mapped for the server's writes. */
static VkResult
vulkan_staging(
	struct x11_vulkan_window *window)
{
	VkMemoryRequirements requirements;
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
	error = vulkan->vk.vkCreateBuffer(vulkan->device, &create, NULL, &window->staging);
	if (error != VK_SUCCESS)
		return error;

	/* The first memory type it may use that the host sees and keeps coherent. */
	vulkan->vk.vkGetBufferMemoryRequirements(vulkan->device, window->staging, &requirements);
	type = x11_vulkan_memory_type(vulkan, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	/* Without one the host cannot write the pixels. */
	if (type == UINT32_MAX)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The memory. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	error = vulkan->vk.vkAllocateMemory(vulkan->device, &allocate, NULL, &window->staging_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Bound to the buffer. */
	error = vulkan->vk.vkBindBufferMemory(vulkan->device, window->staging, window->staging_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Mapped for good. */
	error = vulkan->vk.vkMapMemory(vulkan->device, window->staging_memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
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
	struct x11_vulkan *vulkan;
	VkDevice device;
	VkResult error;

	/* The pool, whose one buffer is recorded again each frame. */
	vulkan = window->vulkan;
	device = vulkan->device;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = window->vulkan->family;
	error = vulkan->vk.vkCreateCommandPool(device, &pool, NULL, &window->pool);
	if (error != VK_SUCCESS)
		return error;

	/* The command buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = window->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	error = vulkan->vk.vkAllocateCommandBuffers(device, &command, &window->command);
	if (error != VK_SUCCESS)
		return error;

	/* The fence, signalled so the first frame does not wait. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	error = vulkan->vk.vkCreateFence(device, &fence, NULL, &window->fence);
	if (error != VK_SUCCESS)
		return error;

	/* The acquire semaphore. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	error = vulkan->vk.vkCreateSemaphore(device, &semaphore, NULL, &window->acquired);
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
	struct x11_vulkan *vulkan;
	uint32_t index;

	/* The present semaphores. */
	vulkan = window->vulkan;
	for (index = 0U; index < window->image_count; index++)
		vulkan->vk.vkDestroySemaphore(window->vulkan->device, window->rendered[index], NULL);
	window->image_count = 0U;

	/* The chain. */
	if (window->swapchain != VK_NULL_HANDLE)
		vulkan->vk.vkDestroySwapchainKHR(window->vulkan->device, window->swapchain, NULL);
	window->swapchain = VK_NULL_HANDLE;
}

/* Destroys a window's staging buffer and its memory. */
static void
vulkan_staging_free(
	struct x11_vulkan_window *window)
{
	struct x11_vulkan *vulkan;

	/* The buffer. */
	vulkan = window->vulkan;
	if (window->staging != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyBuffer(window->vulkan->device, window->staging, NULL);
	window->staging = VK_NULL_HANDLE;

	/* Its memory (unmapped with it). */
	if (window->staging_memory != VK_NULL_HANDLE)
		vulkan->vk.vkFreeMemory(window->vulkan->device, window->staging_memory, NULL);
	window->staging_memory = VK_NULL_HANDLE;
	window->staging_pixels = NULL;
}

/* Writes the window's pixels (rows stride pixels apart) into the staging buffer, opaque, in the chain's byte order. */
static void
vulkan_copy_pixels(
	struct x11_vulkan_window *window,
	const uint32_t *pixels,
	size_t stride)
{
	const uint32_t *line;
	uint32_t *target;
	uint32_t pixel;
	size_t width;
	size_t row;
	size_t column;

	/* Each row, each pixel with its alpha made full; an RGBA chain takes red first (the red and blue bytes change places). */
	target = window->staging_pixels;
	width = window->extent.width;
	for (row = 0U; row < window->extent.height; row++) {
		line = pixels + row * stride;
		for (column = 0U; column < width; column++) {
			pixel = line[column];
			if (window->format != VK_FORMAT_B8G8R8A8_UNORM)
				pixel = ((pixel & 0xffU) << 16) | (pixel & 0xff00U) | ((pixel >> 16) & 0xffU);
			target[row * width + column] = pixel | VULKAN_OPAQUE;
		}
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
	struct x11_vulkan *vulkan;
	VkResult error;

	/* The buffer, recorded afresh. */
	vulkan = window->vulkan;
	error = vulkan->vk.vkResetCommandBuffer(window->command, 0U);
	if (error != VK_SUCCESS)
		return error;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	error = vulkan->vk.vkBeginCommandBuffer(window->command, &begin);
	if (error != VK_SUCCESS)
		return error;

	/* The image made ready for the copy (its old contents do not matter: the copy covers it). */
	vulkan_barrier(vulkan, window->command, window->images[image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0U, VK_ACCESS_TRANSFER_WRITE_BIT);

	/* The whole image from the buffer, rows of the image's width. */
	memset(&region, 0, sizeof(region));
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1U;
	region.imageExtent.width = window->extent.width;
	region.imageExtent.height = window->extent.height;
	region.imageExtent.depth = 1U;
	vulkan->vk.vkCmdCopyBufferToImage(window->command, window->staging, window->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);

	/* Then ready to present. */
	vulkan_barrier(vulkan, window->command, window->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0U);

	/* The recording ends. */
	error = vulkan->vk.vkEndCommandBuffer(window->command);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the frame's copy is recorded. */
	return VK_SUCCESS;
}

/* Records an image's change of layout around the transfer. */
static void
vulkan_barrier(
	struct x11_vulkan *vulkan,
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
	vulkan->vk.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
}

/*
 * Acquires a free image of a window's chain, without waiting (made again
 * first when it went out of date).  Returns 0, 1 when the frame must wait
 * (no image is free, or the chain is out of date), or -1 when Vulkan
 * failed.
 */
static int
vulkan_acquire(
	struct x11_vulkan_window *window,
	uint32_t *image)
{
	struct x11_vulkan *vulkan;
	VkResult error;
	int status;

	/* A chain that no longer matches the surface is made again. */
	vulkan = window->vulkan;
	if (vulkan->lost)
		return -1;
	if (window->stale) {
		status = x11_vulkan_window_resize(window, window->width, window->height);
		if (status != 0)
			return -1;
	}

	/* A free image, without waiting for one. */
	error = vulkan->vk.vkAcquireNextImageKHR(vulkan->device, window->swapchain, 0U, window->acquired, VK_NULL_HANDLE, image);
	if (error == VK_NOT_READY || error == VK_TIMEOUT)
		return 1;
	if (error == VK_ERROR_OUT_OF_DATE_KHR) {
		window->stale = 1;
		return 1;
	}

	/* Any other failure ends Vulkan for the window. */
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return -1;

	/* Succeeded: the image is the server's until presented. */
	return 0;
}

/*
 * Presents an image of a window's chain after its frame.  Returns 0 (a
 * chain that went out of date is made again next time), or -1 when
 * Vulkan failed.
 */
static int
vulkan_present(
	struct x11_vulkan_window *window,
	uint32_t image)
{
	struct x11_vulkan *vulkan;
	VkPresentInfoKHR present;
	VkResult error;

	/* The image presented after the frame's semaphore. */
	vulkan = window->vulkan;
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &window->rendered[image];
	present.swapchainCount = 1U;
	present.pSwapchains = &window->swapchain;
	present.pImageIndices = &image;
	error = vulkan->vk.vkQueuePresentKHR(vulkan->queue, &present);
	if (error == VK_SUBOPTIMAL_KHR || error == VK_ERROR_OUT_OF_DATE_KHR) {
		window->stale = 1;
		return 0;
	}

	/* Any other failure ends Vulkan for the window. */
	if (error != VK_SUCCESS)
		return -1;

	/* Succeeded: the frame is the system's. */
	return 0;
}
