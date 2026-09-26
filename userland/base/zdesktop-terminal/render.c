/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing of zdesktop-terminal: every cell of the grid as two triangles
 * that sample the glyph atlas, with standard Vulkan and Wayland WSI.
 *
 * Each frame rebuilds the vertices of the whole grid in host-visible
 * memory, draws them in one call and waits for the frame to finish before
 * the next one, so the host may write the atlas and the vertices between
 * frames without further synchronization.
 */

#include "terminal.h"
#include "shaders.h"

#include <stdlib.h>
#include <string.h>

/* How long a frame may take on the GPU, in nanoseconds. */
#define RENDER_TIMEOUT		10000000000ULL

/* The glyph atlas's size in pixels. */
#define RENDER_ATLAS_WIDTH	2048U
#define RENDER_ATLAS_HEIGHT	2048U

/* One vertex: position and atlas place, then the two colours; four floats each. */
#define RENDER_VERTEX_FLOATS	12U

/* The vertices of one cell: two triangles. */
#define RENDER_CELL_VERTICES	6U

static VkResult render_device(struct terminal_renderer *renderer);
static VkResult render_swapchain(struct terminal_renderer *renderer, uint32_t width, uint32_t height, VkSwapchainKHR old);
static VkResult render_pass(struct terminal_renderer *renderer);
static VkResult render_targets(struct terminal_renderer *renderer);
static void render_targets_free(struct terminal_renderer *renderer);
static VkResult render_commands(struct terminal_renderer *renderer);
static VkResult render_memory(struct terminal_renderer *renderer, VkMemoryRequirements *requirements, VkDeviceMemory *memory);
static VkResult render_atlas(struct terminal_renderer *renderer, struct terminal_font *font);
static VkResult render_vertices(struct terminal_renderer *renderer);
static VkResult render_pipeline(struct terminal_renderer *renderer);
static VkResult render_module(struct terminal_renderer *renderer, const uint32_t *code, size_t size, VkShaderModule *module);
static uint32_t render_build(struct terminal_renderer *renderer, struct terminal_screen *screen, struct terminal_font *font);
static float *render_quad(float *vertex, const struct terminal_font *font, float x, float y, float width, float height, unsigned slot, uint32_t foreground, uint32_t background);
static void render_record(struct terminal_renderer *renderer, uint32_t image, uint32_t vertex_count);

/*
 * The atlas image is PREINITIALIZED until the first frame moves it to
 * GENERAL (with the host's writes kept); nonzero once that is done.
 */
static int render_atlas_ready;

/*
 * Makes the Vulkan objects of the window: the device, the swapchain, the
 * pipeline, the glyph atlas (given to the font) and the vertex buffer.
 */
VkResult
terminal_renderer_open(
	struct terminal_renderer *renderer,
	struct terminal_window *window,
	struct terminal_font *font)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	VkWaylandSurfaceCreateInfoKHR surface;
	const char *extensions[2];
	VkResult error;

	/* Nothing is owned yet. */
	memset(renderer, 0, sizeof(*renderer));
	render_atlas_ready = 0;

	/* The instance, with the surface extensions a Wayland window needs. */
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "zdesktop-terminal";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = 2U;
	instance.ppEnabledExtensionNames = extensions;
	renderer->operation = "vkCreateInstance";
	error = vkCreateInstance(&instance, NULL, &renderer->instance);
	if (error != VK_SUCCESS)
		return error;

	/* The window's surface. */
	memset(&surface, 0, sizeof(surface));
	surface.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
	surface.display = window->display;
	surface.surface = window->surface;
	renderer->operation = "vkCreateWaylandSurfaceKHR";
	error = vkCreateWaylandSurfaceKHR(renderer->instance, &surface, NULL, &renderer->surface);
	if (error != VK_SUCCESS)
		return error;

	/* A device with a queue that draws and presents to the surface. */
	error = render_device(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The swapchain at the window's size. */
	error = render_swapchain(renderer, window->width, window->height, VK_NULL_HANDLE);
	if (error != VK_SUCCESS)
		return error;

	/* The pass that draws into the swapchain's images. */
	error = render_pass(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* A framebuffer for each swapchain image. */
	error = render_targets(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The command buffer and the frame's synchronization. */
	error = render_commands(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The glyph atlas, which the font draws into. */
	error = render_atlas(renderer, font);
	if (error != VK_SUCCESS)
		return error;

	/* The vertex buffer, large enough for the largest grid. */
	error = render_vertices(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The pipeline that draws cells. */
	error = render_pipeline(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: frames can be drawn. */
	return VK_SUCCESS;
}

/*
 * Replaces the swapchain with one of a new size.
 */
VkResult
terminal_renderer_resize(
	struct terminal_renderer *renderer,
	uint32_t width,
	uint32_t height)
{
	VkSwapchainKHR old;
	VkResult error;

	/* Nothing may still use the old images. */
	renderer->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(renderer->device);
	if (error != VK_SUCCESS)
		return error;

	/* The old targets go, and the new chain replaces the old one. */
	render_targets_free(renderer);
	old = renderer->swapchain;
	renderer->swapchain = VK_NULL_HANDLE;
	error = render_swapchain(renderer, width, height, old);
	vkDestroySwapchainKHR(renderer->device, old, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Framebuffers for the new images. */
	error = render_targets(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the next frame is drawn at the new size. */
	return VK_SUCCESS;
}

/*
 * Draws the grid and presents it, and waits for the frame to finish.
 *
 * Returns VK_ERROR_OUT_OF_DATE_KHR when the swapchain no longer matches
 * the window; the caller resizes and draws again.
 */
VkResult
terminal_renderer_draw(
	struct terminal_renderer *renderer,
	struct terminal_screen *screen,
	struct terminal_font *font)
{
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkPipelineStageFlags stage;
	uint32_t vertex_count;
	uint32_t image;
	VkResult error;

	/* The vertices of the grid (drawing any new glyph into the atlas). */
	vertex_count = render_build(renderer, screen, font);

	/* The image to draw into, once the compositor has given one back. */
	renderer->operation = "vkAcquireNextImageKHR";
	error = vkAcquireNextImageKHR(renderer->device, renderer->swapchain, RENDER_TIMEOUT, renderer->acquired, VK_NULL_HANDLE, &image);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* The frame's commands. */
	renderer->operation = "vkResetCommandBuffer";
	error = vkResetCommandBuffer(renderer->command, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Records the frame and closes the recording. */
	render_record(renderer, image, vertex_count);
	renderer->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(renderer->command);
	if (error != VK_SUCCESS)
		return error;

	/* The frame's fence starts unsignalled. */
	renderer->operation = "vkResetFences";
	error = vkResetFences(renderer->device, 1U, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Submits the frame after the acquire, signalling the image's present semaphore. */
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &renderer->acquired;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &renderer->command;
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &renderer->targets[image].rendered;
	renderer->operation = "vkQueueSubmit";
	error = vkQueueSubmit(renderer->queue, 1U, &submit, renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Presents the image to the window. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &renderer->targets[image].rendered;
	present.swapchainCount = 1U;
	present.pSwapchains = &renderer->swapchain;
	present.pImageIndices = &image;
	renderer->operation = "vkQueuePresentKHR";
	error = vkQueuePresentKHR(renderer->queue, &present);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* The frame is finished before the host touches the vertices or the atlas again. */
	renderer->operation = "vkWaitForFences";
	error = vkWaitForFences(renderer->device, 1U, &renderer->fence, VK_TRUE, RENDER_TIMEOUT);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the grid is on the window. */
	return VK_SUCCESS;
}

/*
 * Releases every Vulkan object, children before their parents.
 */
void
terminal_renderer_close(
	struct terminal_renderer *renderer)
{
	/* The device's objects, once nothing runs. */
	if (renderer->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(renderer->device);
		render_targets_free(renderer);

		/* The drawing objects. */
		if (renderer->pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(renderer->device, renderer->pipeline, NULL);
		if (renderer->layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(renderer->device, renderer->layout, NULL);
		if (renderer->descriptor_pool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(renderer->device, renderer->descriptor_pool, NULL);
		if (renderer->set_layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(renderer->device, renderer->set_layout, NULL);
		if (renderer->sampler != VK_NULL_HANDLE)
			vkDestroySampler(renderer->device, renderer->sampler, NULL);

		/* The atlas and the vertices, with their memory. */
		if (renderer->atlas_view != VK_NULL_HANDLE)
			vkDestroyImageView(renderer->device, renderer->atlas_view, NULL);
		if (renderer->atlas != VK_NULL_HANDLE)
			vkDestroyImage(renderer->device, renderer->atlas, NULL);
		if (renderer->atlas_memory != VK_NULL_HANDLE)
			vkFreeMemory(renderer->device, renderer->atlas_memory, NULL);
		if (renderer->vertices != VK_NULL_HANDLE)
			vkDestroyBuffer(renderer->device, renderer->vertices, NULL);
		if (renderer->vertex_memory != VK_NULL_HANDLE)
			vkFreeMemory(renderer->device, renderer->vertex_memory, NULL);

		/* The commands and the synchronization. */
		if (renderer->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(renderer->device, renderer->pool, NULL);
		if (renderer->fence != VK_NULL_HANDLE)
			vkDestroyFence(renderer->device, renderer->fence, NULL);
		if (renderer->acquired != VK_NULL_HANDLE)
			vkDestroySemaphore(renderer->device, renderer->acquired, NULL);

		/* The pass, the swapchain and the device itself. */
		if (renderer->pass != VK_NULL_HANDLE)
			vkDestroyRenderPass(renderer->device, renderer->pass, NULL);
		if (renderer->swapchain != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(renderer->device, renderer->swapchain, NULL);
		vkDestroyDevice(renderer->device, NULL);
	}

	/* The surface and the instance. */
	if (renderer->surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(renderer->instance, renderer->surface, NULL);
	if (renderer->instance != VK_NULL_HANDLE)
		vkDestroyInstance(renderer->instance, NULL);

	/* Nothing is owned any more. */
	memset(renderer, 0, sizeof(*renderer));
}

/* Chooses a physical device and a queue family that draws and presents to the surface, and makes the device. */
static VkResult
render_device(
	struct terminal_renderer *renderer)
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
	renderer->operation = "vkEnumeratePhysicalDevices";
	error = vkEnumeratePhysicalDevices(renderer->instance, &count, devices);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The first family of any device that draws and presents to this surface. */
	for (index = 0U; index < count && renderer->physical == VK_NULL_HANDLE; index++) {
		family_count = 16U;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, families);
		for (family = 0U; family < family_count; family++) {
			/* A family must draw and have a queue. */
			if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U || families[family].queueCount == 0U)
				continue;

			/* And present to this very surface. */
			supported = VK_FALSE;
			error = vkGetPhysicalDeviceSurfaceSupportKHR(devices[index], family, renderer->surface, &supported);
			if (error != VK_SUCCESS || supported == VK_FALSE)
				continue;

			/* This family of this device draws the window. */
			renderer->physical = devices[index];
			renderer->family = family;
			break;
		}
	}

	/* No device can draw this window. */
	if (renderer->physical == VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* One queue of that family and the swapchain extension. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = renderer->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;
	extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create.queueCreateInfoCount = 1U;
	create.pQueueCreateInfos = &queue;
	create.enabledExtensionCount = 1U;
	create.ppEnabledExtensionNames = &extension;
	renderer->operation = "vkCreateDevice";
	error = vkCreateDevice(renderer->physical, &create, NULL, &renderer->device);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the device and its queue. */
	vkGetDeviceQueue(renderer->device, renderer->family, 0U, &renderer->queue);
	return VK_SUCCESS;
}

/* Makes the swapchain at a size, in an 8-bit UNORM format, presenting in FIFO order. */
static VkResult
render_swapchain(
	struct terminal_renderer *renderer,
	uint32_t width,
	uint32_t height,
	VkSwapchainKHR old)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[16];
	VkSwapchainCreateInfoKHR create;
	uint32_t count;
	uint32_t index;
	VkResult error;

	/* An 8-bit UNORM format the surface offers. */
	count = 16U;
	renderer->operation = "vkGetPhysicalDeviceSurfaceFormatsKHR";
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->physical, renderer->surface, &count, formats);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The first of those formats that is 8-bit UNORM. */
	renderer->format = VK_FORMAT_UNDEFINED;
	for (index = 0U; index < count; index++) {
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM || formats[index].format == VK_FORMAT_R8G8B8A8_UNORM) {
			renderer->format = formats[index].format;
			break;
		}
	}

	/* A surface without one cannot show the colours as they are. */
	if (renderer->format == VK_FORMAT_UNDEFINED)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The size, kept inside what the surface takes. */
	renderer->operation = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR";
	error = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(renderer->physical, renderer->surface, &capabilities);
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
	renderer->extent.width = width;
	renderer->extent.height = height;

	/* Three images when the surface allows, opaque, replacing the old chain. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.surface = renderer->surface;
	create.minImageCount = 3U;
	if (create.minImageCount < capabilities.minImageCount)
		create.minImageCount = capabilities.minImageCount;
	if (capabilities.maxImageCount != 0U && create.minImageCount > capabilities.maxImageCount)
		create.minImageCount = capabilities.maxImageCount;
	create.imageFormat = renderer->format;
	create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	create.imageExtent = renderer->extent;
	create.imageArrayLayers = 1U;
	create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform = capabilities.currentTransform;
	create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	create.clipped = VK_TRUE;
	create.oldSwapchain = old;
	renderer->operation = "vkCreateSwapchainKHR";
	error = vkCreateSwapchainKHR(renderer->device, &create, NULL, &renderer->swapchain);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the chain at the window's size. */
	return VK_SUCCESS;
}

/* Makes the pass: one colour attachment, cleared to the background and left for presenting. */
static VkResult
render_pass(
	struct terminal_renderer *renderer)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo create;
	VkResult error;

	/* The swapchain image, cleared and stored. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = renderer->format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	/* The single subpass that draws into it. */
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;

	/* The pass. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = 1U;
	create.pAttachments = &attachment;
	create.subpassCount = 1U;
	create.pSubpasses = &subpass;
	renderer->operation = "vkCreateRenderPass";
	error = vkCreateRenderPass(renderer->device, &create, NULL, &renderer->pass);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pass. */
	return VK_SUCCESS;
}

/* Makes a view, a framebuffer and a present semaphore for each swapchain image. */
static VkResult
render_targets(
	struct terminal_renderer *renderer)
{
	VkImage images[8];
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	VkSemaphoreCreateInfo semaphore;
	uint32_t index;
	VkResult error;

	/* The swapchain's images (at most eight). */
	renderer->count = 8U;
	renderer->operation = "vkGetSwapchainImagesKHR";
	error = vkGetSwapchainImagesKHR(renderer->device, renderer->swapchain, &renderer->count, images);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* A zeroed table, so that a failure part-way leaves only made objects to release. */
	renderer->targets = calloc(renderer->count, sizeof(renderer->targets[0]));
	if (renderer->targets == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Each image's objects. */
	for (index = 0U; index < renderer->count; index++) {
		/* The view of the image. */
		renderer->targets[index].image = images[index];
		memset(&view, 0, sizeof(view));
		view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.image = images[index];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = renderer->format;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.levelCount = 1U;
		view.subresourceRange.layerCount = 1U;
		renderer->operation = "vkCreateImageView";
		error = vkCreateImageView(renderer->device, &view, NULL, &renderer->targets[index].view);
		if (error != VK_SUCCESS)
			return error;

		/* The framebuffer over it. */
		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer.renderPass = renderer->pass;
		framebuffer.attachmentCount = 1U;
		framebuffer.pAttachments = &renderer->targets[index].view;
		framebuffer.width = renderer->extent.width;
		framebuffer.height = renderer->extent.height;
		framebuffer.layers = 1U;
		renderer->operation = "vkCreateFramebuffer";
		error = vkCreateFramebuffer(renderer->device, &framebuffer, NULL, &renderer->targets[index].framebuffer);
		if (error != VK_SUCCESS)
			return error;

		/* The semaphore its present waits for. */
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		renderer->operation = "vkCreateSemaphore";
		error = vkCreateSemaphore(renderer->device, &semaphore, NULL, &renderer->targets[index].rendered);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: every image can be drawn into. */
	return VK_SUCCESS;
}

/* Releases the objects of the swapchain's images (not the images, which are the swapchain's). */
static void
render_targets_free(
	struct terminal_renderer *renderer)
{
	uint32_t index;

	/* Each image's semaphore, framebuffer and view, where made. */
	for (index = 0U; renderer->targets != NULL && index < renderer->count; index++) {
		if (renderer->targets[index].rendered != VK_NULL_HANDLE)
			vkDestroySemaphore(renderer->device, renderer->targets[index].rendered, NULL);
		if (renderer->targets[index].framebuffer != VK_NULL_HANDLE)
			vkDestroyFramebuffer(renderer->device, renderer->targets[index].framebuffer, NULL);
		if (renderer->targets[index].view != VK_NULL_HANDLE)
			vkDestroyImageView(renderer->device, renderer->targets[index].view, NULL);
	}

	/* The table itself. */
	free(renderer->targets);
	renderer->targets = NULL;
	renderer->count = 0U;
}

/* Makes the command pool and buffer, the frame's fence and the acquire semaphore. */
static VkResult
render_commands(
	struct terminal_renderer *renderer)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkResult error;

	/* A pool whose one buffer is reset every frame. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = renderer->family;
	renderer->operation = "vkCreateCommandPool";
	error = vkCreateCommandPool(renderer->device, &pool, NULL, &renderer->pool);
	if (error != VK_SUCCESS)
		return error;

	/* The buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = renderer->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	renderer->operation = "vkAllocateCommandBuffers";
	error = vkAllocateCommandBuffers(renderer->device, &command, &renderer->command);
	if (error != VK_SUCCESS)
		return error;

	/* The fence the frame's end signals. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	renderer->operation = "vkCreateFence";
	error = vkCreateFence(renderer->device, &fence, NULL, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* The semaphore the acquire signals. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	renderer->operation = "vkCreateSemaphore";
	error = vkCreateSemaphore(renderer->device, &semaphore, NULL, &renderer->acquired);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: one frame at a time can be recorded and waited for. */
	return VK_SUCCESS;
}

/* Allocates host-visible, coherent memory that meets the requirements. */
static VkResult
render_memory(
	struct terminal_renderer *renderer,
	VkMemoryRequirements *requirements,
	VkDeviceMemory *memory)
{
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryAllocateInfo allocate;
	VkMemoryPropertyFlags wanted;
	uint32_t index;
	VkResult error;

	/* The first allowed type that the host sees and keeps coherent. */
	vkGetPhysicalDeviceMemoryProperties(renderer->physical, &properties);
	wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (index = 0U; index < properties.memoryTypeCount; index++) {
		if ((requirements->memoryTypeBits & (1U << index)) != 0U && (properties.memoryTypes[index].propertyFlags & wanted) == wanted)
			break;
	}

	/* Without one the host cannot write the atlas or the vertices. */
	if (index == properties.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The allocation. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements->size;
	allocate.memoryTypeIndex = index;
	renderer->operation = "vkAllocateMemory";
	error = vkAllocateMemory(renderer->device, &allocate, NULL, memory);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: host-visible memory. */
	return VK_SUCCESS;
}

/* Makes the glyph atlas (a linear, host-written image), its sampler and descriptor set, and gives its pixels to the font. */
static VkResult
render_atlas(
	struct terminal_renderer *renderer,
	struct terminal_font *font)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkImageViewCreateInfo view;
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	void *map;
	VkResult error;
	int attached;

	/* The image: linear so that the host writes its rows, sampled by the fragment shader. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_B8G8R8A8_UNORM;
	image.extent.width = RENDER_ATLAS_WIDTH;
	image.extent.height = RENDER_ATLAS_HEIGHT;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_LINEAR;
	image.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	renderer->operation = "vkCreateImage";
	error = vkCreateImage(renderer->device, &image, NULL, &renderer->atlas);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, bound and mapped for good. */
	vkGetImageMemoryRequirements(renderer->device, renderer->atlas, &requirements);
	error = render_memory(renderer, &requirements, &renderer->atlas_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the atlas. */
	renderer->operation = "vkBindImageMemory";
	error = vkBindImageMemory(renderer->device, renderer->atlas, renderer->atlas_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps it for the host's writes, for good. */
	renderer->operation = "vkMapMemory";
	error = vkMapMemory(renderer->device, renderer->atlas_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (error != VK_SUCCESS)
		return error;

	/* Where the rows start, which the font needs to draw into it. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(renderer->device, renderer->atlas, &subresource, &layout);
	attached = terminal_font_attach(font, (unsigned char *)map + layout.offset, (size_t)layout.rowPitch, RENDER_ATLAS_WIDTH, RENDER_ATLAS_HEIGHT);
	if (attached != 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The view the shader samples. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = renderer->atlas;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	renderer->operation = "vkCreateImageView";
	error = vkCreateImageView(renderer->device, &view, NULL, &renderer->atlas_view);
	if (error != VK_SUCCESS)
		return error;

	/* Nearest sampling: cells land on whole pixels, so each texel maps to one pixel. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	renderer->operation = "vkCreateSampler";
	error = vkCreateSampler(renderer->device, &sampler, NULL, &renderer->sampler);
	if (error != VK_SUCCESS)
		return error;

	/* One combined image sampler for the fragment shader. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1U;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 1U;
	set_layout.pBindings = &binding;
	renderer->operation = "vkCreateDescriptorSetLayout";
	error = vkCreateDescriptorSetLayout(renderer->device, &set_layout, NULL, &renderer->set_layout);
	if (error != VK_SUCCESS)
		return error;

	/* A pool for the one set. */
	memset(&pool_size, 0, sizeof(pool_size));
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = 1U;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 1U;
	pool.poolSizeCount = 1U;
	pool.pPoolSizes = &pool_size;
	renderer->operation = "vkCreateDescriptorPool";
	error = vkCreateDescriptorPool(renderer->device, &pool, NULL, &renderer->descriptor_pool);
	if (error != VK_SUCCESS)
		return error;

	/* The set. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = renderer->descriptor_pool;
	allocate.descriptorSetCount = 1U;
	allocate.pSetLayouts = &renderer->set_layout;
	renderer->operation = "vkAllocateDescriptorSets";
	error = vkAllocateDescriptorSets(renderer->device, &allocate, &renderer->set);
	if (error != VK_SUCCESS)
		return error;

	/* The set names the atlas in the general layout it is kept in. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = renderer->sampler;
	image_info.imageView = renderer->atlas_view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = renderer->set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(renderer->device, 1U, &write, 0U, NULL);

	/* Succeeded: the font draws into the atlas and the shader samples it. */
	return VK_SUCCESS;
}

/* Makes the vertex buffer, large enough for the largest grid and the cursor, mapped for good. */
static VkResult
render_vertices(
	struct terminal_renderer *renderer)
{
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkResult error;

	/* Every cell's two triangles and the cursor's. */
	renderer->vertex_capacity = ((size_t)TERMINAL_MAX_COLUMNS * TERMINAL_MAX_ROWS + 1U) * RENDER_CELL_VERTICES;
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = renderer->vertex_capacity * RENDER_VERTEX_FLOATS * sizeof(float);
	buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	renderer->operation = "vkCreateBuffer";
	error = vkCreateBuffer(renderer->device, &buffer, NULL, &renderer->vertices);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, bound and mapped. */
	vkGetBufferMemoryRequirements(renderer->device, renderer->vertices, &requirements);
	error = render_memory(renderer, &requirements, &renderer->vertex_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the buffer. */
	renderer->operation = "vkBindBufferMemory";
	error = vkBindBufferMemory(renderer->device, renderer->vertices, renderer->vertex_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps it for the host's writes, for good. */
	renderer->operation = "vkMapMemory";
	error = vkMapMemory(renderer->device, renderer->vertex_memory, 0U, VK_WHOLE_SIZE, 0U, &renderer->vertex_map);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the host writes each frame's vertices here. */
	return VK_SUCCESS;
}

/* Makes the pipeline that draws cells: three vec4 attributes, the atlas, the window's size as a push constant. */
static VkResult
render_pipeline(
	struct terminal_renderer *renderer)
{
	static const VkDynamicState dynamic[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout;
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription binding;
	VkVertexInputAttributeDescription attributes[3];
	VkPipelineVertexInputStateCreateInfo input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic_state;
	VkGraphicsPipelineCreateInfo pipeline;
	uint32_t index;
	VkResult error;

	/* The two shader modules. */
	error = render_module(renderer, terminal_cell_vert, sizeof(terminal_cell_vert), &vertex);
	if (error != VK_SUCCESS)
		return error;

	/* The fragment module; the vertex module goes when it cannot be made. */
	error = render_module(renderer, terminal_cell_frag, sizeof(terminal_cell_frag), &fragment);
	if (error != VK_SUCCESS) {
		vkDestroyShaderModule(renderer->device, vertex, NULL);
		return error;
	}

	/* The layout: the atlas's set and the window's size. */
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push.offset = 0U;
	push.size = 4U * sizeof(float);
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1U;
	layout.pSetLayouts = &renderer->set_layout;
	layout.pushConstantRangeCount = 1U;
	layout.pPushConstantRanges = &push;
	renderer->operation = "vkCreatePipelineLayout";
	error = vkCreatePipelineLayout(renderer->device, &layout, NULL, &renderer->layout);

	/* The two stages. */
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment;
	stages[1].pName = "main";

	/* One vertex buffer of three vec4s a vertex. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.stride = RENDER_VERTEX_FLOATS * sizeof(float);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	memset(attributes, 0, sizeof(attributes));
	for (index = 0U; index < 3U; index++) {
		attributes[index].location = index;
		attributes[index].binding = 0U;
		attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		attributes[index].offset = index * 4U * sizeof(float);
	}

	/* The input state: that buffer and those attributes, drawn as a list of triangles. */
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	input.vertexBindingDescriptionCount = 1U;
	input.pVertexBindingDescriptions = &binding;
	input.vertexAttributeDescriptionCount = 3U;
	input.pVertexAttributeDescriptions = attributes;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	/* The viewport and scissor follow the swapchain, set each frame. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1U;
	viewport.scissorCount = 1U;
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	/* Cells are opaque: no blending. */
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1U;
	blend.pAttachments = &blend_attachment;
	memset(&dynamic_state, 0, sizeof(dynamic_state));
	dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.dynamicStateCount = 2U;
	dynamic_state.pDynamicStates = dynamic;

	/* The pipeline, when the layout was made. */
	if (error == VK_SUCCESS) {
		memset(&pipeline, 0, sizeof(pipeline));
		pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipeline.stageCount = 2U;
		pipeline.pStages = stages;
		pipeline.pVertexInputState = &input;
		pipeline.pInputAssemblyState = &assembly;
		pipeline.pViewportState = &viewport;
		pipeline.pRasterizationState = &raster;
		pipeline.pMultisampleState = &multisample;
		pipeline.pColorBlendState = &blend;
		pipeline.pDynamicState = &dynamic_state;
		pipeline.layout = renderer->layout;
		pipeline.renderPass = renderer->pass;
		renderer->operation = "vkCreateGraphicsPipelines";
		error = vkCreateGraphicsPipelines(renderer->device, VK_NULL_HANDLE, 1U, &pipeline, NULL, &renderer->pipeline);
	}

	/* The modules are not needed once the pipeline is made. */
	vkDestroyShaderModule(renderer->device, vertex, NULL);
	vkDestroyShaderModule(renderer->device, fragment, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pipeline. */
	return VK_SUCCESS;
}

/* Makes a shader module from SPIR-V words. */
static VkResult
render_module(
	struct terminal_renderer *renderer,
	const uint32_t *code,
	size_t size,
	VkShaderModule *module)
{
	VkShaderModuleCreateInfo create;
	VkResult error;

	/* The module over the words. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create.codeSize = size;
	create.pCode = code;
	renderer->operation = "vkCreateShaderModule";
	error = vkCreateShaderModule(renderer->device, &create, NULL, module);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the module. */
	return VK_SUCCESS;
}

/* Writes the vertices of every cell and of the cursor, and returns how many there are. */
static uint32_t
render_build(
	struct terminal_renderer *renderer,
	struct terminal_screen *screen,
	struct terminal_font *font)
{
	struct terminal_cell *cell;
	float *vertex;
	float *start;
	uint32_t foreground;
	uint32_t background;
	unsigned column;
	unsigned row;
	unsigned slot;
	unsigned blank;
	float x;
	float y;

	/* The blank glyph (a space), which empty cells and the right halves of wide characters use. */
	blank = terminal_font_slot(font, ' ');

	/* Each cell, row after row, from the padded top left. */
	start = renderer->vertex_map;
	vertex = start;
	for (row = 0U; row < screen->rows; row++) {
		for (column = 0U; column < screen->columns; column++) {
			cell = terminal_screen_cell(screen, column, row);

			/* The glyph's slot: the blank for spaces and continuations. */
			slot = blank;
			if (cell->continuation == 0 && cell->codepoint != ' ' && cell->codepoint != 0U)
				slot = terminal_font_slot(font, cell->codepoint);

			/* The cursor's cell is drawn with its colours swapped: a block cursor. */
			foreground = cell->foreground;
			background = cell->background;
			if (screen->cursor_visible && column == screen->cursor_column && row == screen->cursor_row) {
				foreground = cell->background;
				background = cell->foreground;
			}

			/* The cell's two triangles. */
			x = (float)(TERMINAL_PADDING + column * font->cell_width);
			y = (float)(TERMINAL_PADDING + row * font->cell_height);
			vertex = render_quad(vertex, font, x, y, (float)font->cell_width, (float)font->cell_height, slot, foreground, background);
		}
	}

	/* Reports how many vertices the frame draws. */
	return (uint32_t)((size_t)(vertex - start) / RENDER_VERTEX_FLOATS);
}

/* Writes the six vertices of one quad showing a slot of the atlas, and returns where the next quad goes. */
static float *
render_quad(
	float *vertex,
	const struct terminal_font *font,
	float x,
	float y,
	float width,
	float height,
	unsigned slot,
	uint32_t foreground,
	uint32_t background)
{
	static const float corners[RENDER_CELL_VERTICES][2] = {
		{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f },
		{ 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f }
	};
	unsigned per_row;
	unsigned index;
	float u;
	float v;
	float slot_width;
	float slot_height;

	/* Where the slot is in the atlas, in texture coordinates. */
	per_row = font->atlas_width / font->cell_width;
	u = (float)((slot % per_row) * font->cell_width) / (float)font->atlas_width;
	v = (float)((slot / per_row) * font->cell_height) / (float)font->atlas_height;
	slot_width = (float)font->cell_width / (float)font->atlas_width;
	slot_height = (float)font->cell_height / (float)font->atlas_height;

	/* Each corner: position and atlas place, then the two colours. */
	for (index = 0U; index < RENDER_CELL_VERTICES; index++) {
		vertex[0] = x + corners[index][0] * width;
		vertex[1] = y + corners[index][1] * height;
		vertex[2] = u + corners[index][0] * slot_width;
		vertex[3] = v + corners[index][1] * slot_height;
		vertex[4] = (float)((foreground >> 16) & 0xffU) / 255.0f;
		vertex[5] = (float)((foreground >> 8) & 0xffU) / 255.0f;
		vertex[6] = (float)(foreground & 0xffU) / 255.0f;
		vertex[7] = 1.0f;
		vertex[8] = (float)((background >> 16) & 0xffU) / 255.0f;
		vertex[9] = (float)((background >> 8) & 0xffU) / 255.0f;
		vertex[10] = (float)(background & 0xffU) / 255.0f;
		vertex[11] = 1.0f;
		vertex += RENDER_VERTEX_FLOATS;
	}

	/* Reports where the next quad starts. */
	return vertex;
}

/* Records the frame: the atlas's first layout change, the cleared pass and the draw of every cell. */
static void
render_record(
	struct terminal_renderer *renderer,
	uint32_t image,
	uint32_t vertex_count)
{
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkDeviceSize offset;
	float size[4];

	/* One submission of this recording. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	(void)vkBeginCommandBuffer(renderer->command, &begin);

	/* The first frame moves the atlas from preinitialized (the host's writes kept) to general. */
	if (render_atlas_ready == 0) {
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = renderer->atlas;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1U;
		barrier.subresourceRange.layerCount = 1U;
		vkCmdPipelineBarrier(renderer->command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
		render_atlas_ready = 1;
	}

	/* The pass, cleared to the background (the padding around the grid). */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = (float)((TERMINAL_BACKGROUND >> 16) & 0xffU) / 255.0f;
	clear.color.float32[1] = (float)((TERMINAL_BACKGROUND >> 8) & 0xffU) / 255.0f;
	clear.color.float32[2] = (float)(TERMINAL_BACKGROUND & 0xffU) / 255.0f;
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = renderer->pass;
	pass.framebuffer = renderer->targets[image].framebuffer;
	pass.renderArea.extent = renderer->extent;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(renderer->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The whole image is the viewport. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)renderer->extent.width;
	viewport.height = (float)renderer->extent.height;
	viewport.maxDepth = 1.0f;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent = renderer->extent;
	vkCmdSetViewport(renderer->command, 0U, 1U, &viewport);
	vkCmdSetScissor(renderer->command, 0U, 1U, &scissor);

	/* The cells: the pipeline, the atlas, the vertices and the window's size. */
	size[0] = (float)renderer->extent.width;
	size[1] = (float)renderer->extent.height;
	size[2] = 0.0f;
	size[3] = 0.0f;
	offset = 0U;
	vkCmdBindPipeline(renderer->command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline);
	vkCmdBindDescriptorSets(renderer->command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->layout, 0U, 1U, &renderer->set, 0U, NULL);
	vkCmdBindVertexBuffers(renderer->command, 0U, 1U, &renderer->vertices, &offset);
	vkCmdPushConstants(renderer->command, renderer->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(size), size);
	if (vertex_count != 0U)
		vkCmdDraw(renderer->command, vertex_count, 1U, 0U, 0U);

	/* The pass ends with the image ready to present. */
	vkCmdEndRenderPass(renderer->command);
}
