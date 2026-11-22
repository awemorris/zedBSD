/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing of terminal with Vulkan: the quads of the frame
 * (render-build.c) drawn in one call on the window's Vulkan surface
 * (libkeiland's).
 *
 * Each frame stores the vertices built into host-visible memory and draws
 * them in one call.  The frame before is waited for
 * first (terminal_renderer_idle), not right after its present, so the
 * terminal reads its shells while the GPU draws; the host writes the atlas
 * and the vertices only after that wait.
 */

#include "render.h"
#include "shaders.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long a frame may take on the GPU, in nanoseconds. */
#define RENDER_TIMEOUT		10000000000ULL

/* The glyph atlas's size in pixels. */
#define RENDER_ATLAS_WIDTH	2048U
#define RENDER_ATLAS_HEIGHT	2048U

/*
 * A doubleword of the vertices, read and written over the floats they
 * were built as (the build compiles memcpy as a call, too slow for this).
 */
typedef uint64_t render_word __attribute__((__may_alias__));

/* The parts of a frame's drawing the ZTERM PERF line sums, and how often it is written (milliseconds). */
#define RENDER_PERF_BUILD	0U
#define RENDER_PERF_ACQUIRE	1U
#define RENDER_PERF_RECORD	2U
#define RENDER_PERF_SUBMIT	3U
#define RENDER_PERF_PRESENT	4U
#define RENDER_PERF_WAIT	5U
#define RENDER_PERF_UPLOAD	6U
#define RENDER_PERF_PARTS	7U
#define RENDER_PERF_MS		5000U

static VkResult render_device(struct terminal_renderer *renderer);
static VkResult render_swapchain(struct terminal_renderer *renderer, uint32_t width, uint32_t height, VkSwapchainKHR old);
static VkResult render_pass(struct terminal_renderer *renderer);
static VkResult render_targets(struct terminal_renderer *renderer);
static void render_targets_free(struct terminal_renderer *renderer);
static VkResult render_commands(struct terminal_renderer *renderer);
static VkResult render_memory(struct terminal_renderer *renderer, VkMemoryRequirements *requirements, VkMemoryPropertyFlags wanted, VkDeviceMemory *memory);
static VkResult render_atlas(struct terminal_renderer *renderer, struct terminal_font *font);
static VkResult render_atlas_staging(struct terminal_renderer *renderer, struct terminal_font *font);
static VkResult render_atlas_upload(struct terminal_renderer *renderer, struct terminal_font *font, int *recorded);
static VkResult render_vertices(struct terminal_renderer *renderer);
static VkResult render_pipeline(struct terminal_renderer *renderer);
static VkResult render_module(struct terminal_renderer *renderer, const uint32_t *code, size_t size, VkShaderModule *module);
static void render_upload(struct terminal_renderer *renderer, uint32_t vertex_count);
static void render_store(void *data, size_t first, size_t count);
static void render_record(struct terminal_renderer *renderer, VkCommandBuffer command, uint32_t image, uint32_t vertex_count, uint32_t ground);
static VkResult render_page_make(struct terminal_renderer *renderer, uint32_t width, uint32_t height);
static VkResult render_page_buffer(struct terminal_renderer *renderer, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory, void **map);
static void render_page_free(struct terminal_renderer *renderer);
static void render_page_record(struct terminal_renderer *renderer, uint32_t image);
static uint64_t render_now_us(void);
static void render_perf_add(unsigned part, uint64_t started);
static void render_perf_report(void);

/*
 * The ZTERM PERF line's sums since it was last written: the frames drawn,
 * each part's total and longest time in microseconds (RENDER_PERF_*), and
 * when the window began (terminal_clock).  The main thread's alone.  The
 * line is a measurement's: written only when the environment's
 * TERMINAL_PERF is 1 (terminal_perf_on).
 */
static unsigned render_perf_frames;
static uint64_t render_perf_total[RENDER_PERF_PARTS];
static uint64_t render_perf_most[RENDER_PERF_PARTS];
static uint64_t render_perf_since;

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
	const char *const *extensions;
	uint32_t extension_count;
	VkResult error;
	int failed;

	/* Nothing is owned yet. */
	memset(renderer, 0, sizeof(*renderer));

	/* The surface extensions the window needs on this system (libkeiland's). */
	renderer->operation = "kl_window_vulkan_extensions";
	failed = kl_window_vulkan_extensions(&extensions, &extension_count);
	if (failed != 0)
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* The instance, with those extensions. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "terminal";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = extension_count;
	instance.ppEnabledExtensionNames = extensions;
	renderer->operation = "vkCreateInstance";
	error = vkCreateInstance(&instance, NULL, &renderer->instance);
	if (error != VK_SUCCESS)
		return error;

	/* The window's surface (libkeiland's). */
	renderer->operation = "kl_window_vulkan_surface";
	failed = kl_window_vulkan_surface(window->kui, renderer->instance, &renderer->surface);
	if (failed != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

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
 * Draws the grid, and over it the input method's text being composed at
 * the cursor (the window's), presents it, and waits for the frame
 * to finish.
 *
 * Returns VK_ERROR_OUT_OF_DATE_KHR when the swapchain no longer matches
 * the window; the caller resizes and draws again.
 */
VkResult
terminal_renderer_draw(
	struct terminal_renderer *renderer,
	struct terminal_screen *screen,
	struct terminal_font *font,
	const struct terminal_window *window)
{
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkCommandBufferAllocateInfo allocate;
	VkCommandBuffer commands[2];
	VkPipelineStageFlags stage;
	struct terminal_target *target;
	uint32_t vertex_count;
	uint32_t ground;
	uint32_t image;
	uint64_t mark;
	int uploading;
	int perf;
	VkResult error;

	/* The frame before is finished before the host touches the vertices or the atlas again. */
	mark = render_now_us();
	error = terminal_renderer_idle(renderer);
	if (error != VK_SUCCESS)
		return error;
	render_perf_add(RENDER_PERF_WAIT, mark);

	/* The vertices of the grid and the composed text (drawing any new glyph into the atlas). */
	mark = render_now_us();
	renderer->build.width = renderer->extent.width;
	renderer->build.height = renderer->extent.height;
	vertex_count = terminal_build_frame(&renderer->build, screen, font, window);
	render_perf_add(RENDER_PERF_BUILD, mark);
	mark = render_now_us();
	render_upload(renderer, vertex_count);
	render_perf_add(RENDER_PERF_UPLOAD, mark);

	/* The image to draw into, once the compositor has given one back, however long a busy compositor takes (a closed connection ends the wait with an error). */
	mark = render_now_us();
	renderer->operation = "vkAcquireNextImageKHR";
	error = vkAcquireNextImageKHR(renderer->device, renderer->swapchain, UINT64_MAX, renderer->acquired, VK_NULL_HANDLE, &image);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;
	render_perf_add(RENDER_PERF_ACQUIRE, mark);

	/* The image's command buffer, made at its first frame. */
	mark = render_now_us();
	target = &renderer->targets[image];
	if (target->command == VK_NULL_HANDLE) {
		memset(&allocate, 0, sizeof(allocate));
		allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate.commandPool = renderer->pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1U;
		renderer->operation = "vkAllocateCommandBuffers";
		error = vkAllocateCommandBuffers(renderer->device, &allocate, &target->command);
		if (error != VK_SUCCESS)
			return error;
		target->recorded = 0;
	}

	/*
	 * Records the frame and closes the recording, unless the image's
	 * recording draws the same: the vertices are rewritten in place, so a
	 * recording of the same count and background is submitted again as it
	 * is (each Begin and End is a round trip to the GPU's driver).
	 */
	ground = terminal_build_theme(window)->background;
	if (!target->recorded || target->vertex_count != vertex_count || target->ground != ground) {
		render_record(renderer, target->command, image, vertex_count, ground);
		renderer->operation = "vkEndCommandBuffer";
		error = vkEndCommandBuffer(target->command);
		if (error != VK_SUCCESS)
			return error;
		target->vertex_count = vertex_count;
		target->ground = ground;
		target->recorded = 1;
	}

	/* The glyphs drawn since the last frame are copied into the atlas before the frame samples them. */
	error = render_atlas_upload(renderer, font, &uploading);
	if (error != VK_SUCCESS)
		return error;

	/* The frame's fence starts unsignalled. */
	renderer->operation = "vkResetFences";
	error = vkResetFences(renderer->device, 1U, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;
	render_perf_add(RENDER_PERF_RECORD, mark);

	/* Submits the atlas's copy, if any, and the frame after the acquire, signalling the image's present semaphore. */
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	commands[0] = renderer->upload;
	commands[1] = target->command;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &renderer->acquired;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &commands[1];
	if (uploading) {
		submit.commandBufferCount = 2U;
		submit.pCommandBuffers = commands;
	}
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &renderer->targets[image].rendered;
	mark = render_now_us();
	renderer->operation = "vkQueueSubmit";
	error = vkQueueSubmit(renderer->queue, 1U, &submit, renderer->fence);
	if (error != VK_SUCCESS)
		return error;
	renderer->pending = 1;
	render_perf_add(RENDER_PERF_SUBMIT, mark);

	/* Presents the image to the window. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &renderer->targets[image].rendered;
	present.swapchainCount = 1U;
	present.pSwapchains = &renderer->swapchain;
	present.pImageIndices = &image;
	mark = render_now_us();
	renderer->operation = "vkQueuePresentKHR";
	error = vkQueuePresentKHR(renderer->queue, &present);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;
	render_perf_add(RENDER_PERF_PRESENT, mark);
	terminal_perf_step("present", 0U);

	/* The frame counted, and the sums written every few seconds when asked for. */
	perf = terminal_perf_on();
	if (perf) {
		render_perf_frames++;
		render_perf_report();
	}

	/* Succeeded: the grid is on the window. */
	return VK_SUCCESS;
}

/*
 * Shows a frame painted elsewhere (width by height pixels, a row after
 * another, premultiplied 0xAARRGGBB: the swapchain's size) in the grid's
 * place: its pixels are copied into the page's image, which one quad
 * draws as a colour glyph is drawn, and the image is presented.
 *
 * Returns VK_ERROR_OUT_OF_DATE_KHR when the swapchain no longer matches
 * the window; the caller resizes and draws again.
 */
VkResult
terminal_renderer_page(
	struct terminal_renderer *renderer,
	const uint32_t *pixels,
	uint32_t width,
	uint32_t height)
{
	static const float corners[TERMINAL_QUAD_VERTICES][4] = {
		{ 0.0f, 0.0f, 0.0f, 0.0f },
		{ 1.0f, 0.0f, 1.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f, 1.0f },
		{ 1.0f, 0.0f, 1.0f, 0.0f },
		{ 1.0f, 1.0f, 1.0f, 1.0f },
		{ 0.0f, 1.0f, 0.0f, 1.0f }
	};
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkPipelineStageFlags stage;
	float *vertex;
	unsigned char *row;
	uint32_t image;
	uint32_t y;
	unsigned index;
	VkResult error;

	/* The frame before is finished before the host touches the page's buffers again. */
	error = terminal_renderer_idle(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The page's image and buffers at the frame's size. */
	if (renderer->page == VK_NULL_HANDLE || renderer->page_width != width || renderer->page_height != height) {
		error = render_page_make(renderer, width, height);
		if (error != VK_SUCCESS)
			return error;
	}

	/* The pixels, a row after another, into the buffer the image is copied from. */
	row = renderer->page_map;
	for (y = 0U; y < height; y++)
		memcpy(row + (size_t)y * width * 4U, pixels + (size_t)y * width, (size_t)width * 4U);

	/*
	 * The quad over the whole frame: its corners in pixels and their
	 * places in the image, a foreground of alpha 0 (the image's own
	 * colours, as a colour glyph's) and an opaque background under it.
	 */
	vertex = renderer->page_vertex_map;
	for (index = 0U; index < TERMINAL_QUAD_VERTICES; index++) {
		vertex[0] = corners[index][0] * (float)width;
		vertex[1] = corners[index][1] * (float)height;
		vertex[2] = corners[index][2];
		vertex[3] = corners[index][3];
		vertex[4] = 0.0f;
		vertex[5] = 0.0f;
		vertex[6] = 0.0f;
		vertex[7] = 0.0f;
		vertex[8] = 0.0f;
		vertex[9] = 0.0f;
		vertex[10] = 0.0f;
		vertex[11] = 1.0f;
		vertex += TERMINAL_VERTEX_FLOATS;
	}

	/* The image to draw into, once the compositor has given one back. */
	renderer->operation = "vkAcquireNextImageKHR";
	error = vkAcquireNextImageKHR(renderer->device, renderer->swapchain, UINT64_MAX, renderer->acquired, VK_NULL_HANDLE, &image);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* The copy and the draw, recorded for this page. */
	render_page_record(renderer, image);
	renderer->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(renderer->page_command);
	if (error != VK_SUCCESS)
		return error;

	/* The frame's fence starts unsignalled. */
	renderer->operation = "vkResetFences";
	error = vkResetFences(renderer->device, 1U, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Submits it after the acquire, signalling the image's present semaphore. */
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &renderer->acquired;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &renderer->page_command;
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &renderer->targets[image].rendered;
	renderer->operation = "vkQueueSubmit";
	error = vkQueueSubmit(renderer->queue, 1U, &submit, renderer->fence);
	if (error != VK_SUCCESS)
		return error;
	renderer->pending = 1;

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

	/* Succeeded: the page is on the window. */
	return VK_SUCCESS;
}

/*
 * Waits for the frame submitted last, if it was not waited for: after
 * this the host may write the vertices and the atlas.
 */
VkResult
terminal_renderer_idle(
	struct terminal_renderer *renderer)
{
	VkResult error;

	/* Nothing submitted since the last wait. */
	if (!renderer->pending)
		return VK_SUCCESS;

	/* The frame's fence. */
	renderer->operation = "vkWaitForFences";
	error = vkWaitForFences(renderer->device, 1U, &renderer->fence, VK_TRUE, RENDER_TIMEOUT);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: no frame reads them now. */
	renderer->pending = 0;
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

		/* The page's objects. */
		render_page_free(renderer);
		if (renderer->page_vertices != VK_NULL_HANDLE)
			vkDestroyBuffer(renderer->device, renderer->page_vertices, NULL);
		if (renderer->page_vertex_memory != VK_NULL_HANDLE)
			vkFreeMemory(renderer->device, renderer->page_vertex_memory, NULL);

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

		/* The atlas, its staging buffer and the vertices, with their memory. */
		if (renderer->atlas_view != VK_NULL_HANDLE)
			vkDestroyImageView(renderer->device, renderer->atlas_view, NULL);
		if (renderer->atlas != VK_NULL_HANDLE)
			vkDestroyImage(renderer->device, renderer->atlas, NULL);
		if (renderer->atlas_memory != VK_NULL_HANDLE)
			vkFreeMemory(renderer->device, renderer->atlas_memory, NULL);
		if (renderer->staging != VK_NULL_HANDLE)
			vkDestroyBuffer(renderer->device, renderer->staging, NULL);
		if (renderer->staging_memory != VK_NULL_HANDLE)
			vkFreeMemory(renderer->device, renderer->staging_memory, NULL);
		if (renderer->vertices != VK_NULL_HANDLE)
			vkDestroyBuffer(renderer->device, renderer->vertices, NULL);
		if (renderer->vertex_memory != VK_NULL_HANDLE)
			vkFreeMemory(renderer->device, renderer->vertex_memory, NULL);
		terminal_build_close(&renderer->build);
		free(renderer->vertex_kept);

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

	/* Each image's command buffer, semaphore, framebuffer and view, where made. */
	for (index = 0U; renderer->targets != NULL && index < renderer->count; index++) {
		if (renderer->targets[index].command != VK_NULL_HANDLE)
			vkFreeCommandBuffers(renderer->device, renderer->pool, 1U, &renderer->targets[index].command);
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
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkResult error;

	/* A pool whose buffers (one an image, made by the draw) are reset when recorded again. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = renderer->family;
	renderer->operation = "vkCreateCommandPool";
	error = vkCreateCommandPool(renderer->device, &pool, NULL, &renderer->pool);
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

/*
 * Allocates memory that meets the requirements, of the first allowed type
 * with the wanted properties: host-visible and coherent for what the host
 * writes, device-local for the atlas (which takes any allowed type when
 * the device has no such type for it).
 */
static VkResult
render_memory(
	struct terminal_renderer *renderer,
	VkMemoryRequirements *requirements,
	VkMemoryPropertyFlags wanted,
	VkDeviceMemory *memory)
{
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryAllocateInfo allocate;
	uint32_t index;
	VkResult error;

	/* The first allowed type with the wanted properties. */
	vkGetPhysicalDeviceMemoryProperties(renderer->physical, &properties);
	for (index = 0U; index < properties.memoryTypeCount; index++) {
		if ((requirements->memoryTypeBits & (1U << index)) != 0U && (properties.memoryTypes[index].propertyFlags & wanted) == wanted)
			break;
	}

	/* Memory only the device reads may be of any allowed type. */
	if (index == properties.memoryTypeCount && (wanted & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U) {
		for (index = 0U; index < properties.memoryTypeCount; index++) {
			if ((requirements->memoryTypeBits & (1U << index)) != 0U)
				break;
		}
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

	/* Succeeded: memory of the type wanted. */
	return VK_SUCCESS;
}

/*
 * Makes the glyph atlas (an optimal image the copies of the font's rows
 * fill), its staging buffer (given to the font), its sampler and
 * descriptor set.
 */
static VkResult
render_atlas(
	struct terminal_renderer *renderer,
	struct terminal_font *font)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkImageViewCreateInfo view;
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	VkResult error;

	/* The image: optimal, filled by copies, sampled by the fragment shader. */
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
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	renderer->operation = "vkCreateImage";
	error = vkCreateImage(renderer->device, &image, NULL, &renderer->atlas);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, the device's own. */
	vkGetImageMemoryRequirements(renderer->device, renderer->atlas, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &renderer->atlas_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the atlas. */
	renderer->operation = "vkBindImageMemory";
	error = vkBindImageMemory(renderer->device, renderer->atlas, renderer->atlas_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* The buffer the font draws into, and the command buffer that copies from it. */
	error = render_atlas_staging(renderer, font);
	if (error != VK_SUCCESS)
		return error;

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

	/* A pool for the atlas's set and the page's. */
	memset(&pool_size, 0, sizeof(pool_size));
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = 2U;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 2U;
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

	/* The set names the atlas in the layout the copies leave it in. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = renderer->sampler;
	image_info.imageView = renderer->atlas_view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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

/*
 * Makes the atlas's staging buffer (host-written, mapped for good, rows
 * packed at the atlas's width), gives its pixels to the font, and
 * allocates the command buffer that copies the rows the font wrote.
 */
static VkResult
render_atlas_staging(
	struct terminal_renderer *renderer,
	struct terminal_font *font)
{
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkCommandBufferAllocateInfo allocate;
	int attached;
	VkResult error;

	/* The buffer: four bytes a pixel, the copies' source. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = (VkDeviceSize)RENDER_ATLAS_WIDTH * RENDER_ATLAS_HEIGHT * 4U;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	renderer->operation = "vkCreateBuffer";
	error = vkCreateBuffer(renderer->device, &buffer, NULL, &renderer->staging);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, which the host sees. */
	vkGetBufferMemoryRequirements(renderer->device, renderer->staging, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &renderer->staging_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the buffer. */
	renderer->operation = "vkBindBufferMemory";
	error = vkBindBufferMemory(renderer->device, renderer->staging, renderer->staging_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps it for the host's writes, for good. */
	renderer->operation = "vkMapMemory";
	error = vkMapMemory(renderer->device, renderer->staging_memory, 0U, VK_WHOLE_SIZE, 0U, &renderer->staging_map);
	if (error != VK_SUCCESS)
		return error;

	/* The font draws into it, starting with its first slots. */
	attached = terminal_font_attach(font, renderer->staging_map, (size_t)RENDER_ATLAS_WIDTH * 4U, RENDER_ATLAS_WIDTH, RENDER_ATLAS_HEIGHT);
	if (attached != 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The command buffer that copies the rows the font wrote, recorded again each time. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = renderer->pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1U;
	renderer->operation = "vkAllocateCommandBuffers";
	error = vkAllocateCommandBuffers(renderer->device, &allocate, &renderer->upload);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the image has no copy yet. */
	renderer->atlas_defined = 0;
	return VK_SUCCESS;
}

/*
 * Records the copy of the box of the atlas the font wrote since the last
 * copy, from the staging buffer into the image, and empties the box.  The
 * caller has waited for the frame before, so neither the command buffer
 * nor the rows are in use.  *recorded is nonzero when there is a copy to
 * submit before the frame.
 */
static VkResult
render_atlas_upload(
	struct terminal_renderer *renderer,
	struct terminal_font *font,
	int *recorded)
{
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkBufferImageCopy region;
	VkResult error;

	/* Nothing new to copy. */
	*recorded = 0;
	if (font->dirty_right == 0U)
		return VK_SUCCESS;

	/* A recording submitted once. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	renderer->operation = "vkBeginCommandBuffer";
	error = vkBeginCommandBuffer(renderer->upload, &begin);
	if (error != VK_SUCCESS)
		return error;

	/* The image takes copies after the frames before have sampled it (its first copy keeps nothing). */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (!renderer->atlas_defined) {
		barrier.srcAccessMask = 0U;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	}
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = renderer->atlas;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(renderer->upload, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* The box's rows, read at the buffer's row length (the atlas's width). */
	memset(&region, 0, sizeof(region));
	region.bufferOffset = (VkDeviceSize)font->dirty_top * font->row_pitch + (VkDeviceSize)font->dirty_left * 4U;
	region.bufferRowLength = font->atlas_width;
	region.bufferImageHeight = 0U;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1U;
	region.imageOffset.x = (int32_t)font->dirty_left;
	region.imageOffset.y = (int32_t)font->dirty_top;
	region.imageExtent.width = font->dirty_right - font->dirty_left;
	region.imageExtent.height = font->dirty_bottom - font->dirty_top;
	region.imageExtent.depth = 1U;
	vkCmdCopyBufferToImage(renderer->upload, renderer->staging, renderer->atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);

	/* The frame samples the image after the copy. */
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(renderer->upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* Closes the recording. */
	renderer->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(renderer->upload);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the box is on its way to the image. */
	font->dirty_right = 0U;
	renderer->atlas_defined = 1;
	*recorded = 1;
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
	int failed;

	/* Where frames are built: every cell's two triangles, four rows more and the cursor's. */
	failed = terminal_build_open(&renderer->build);
	if (failed != 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* A part built when the list of parts is full is stored at once (no frame reads the vertices while they are built). */
	renderer->build.store = render_store;
	renderer->build.store_data = renderer;

	/* The buffer, as large as the build's vertices. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = renderer->build.capacity * TERMINAL_VERTEX_FLOATS * sizeof(float);
	buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	renderer->operation = "vkCreateBuffer";
	error = vkCreateBuffer(renderer->device, &buffer, NULL, &renderer->vertices);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, bound and mapped. */
	vkGetBufferMemoryRequirements(renderer->device, renderer->vertices, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &renderer->vertex_memory);
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

	/* The copy of what the mapping holds (nothing yet). */
	renderer->vertex_kept = malloc(buffer.size);
	renderer->vertex_known = 0U;
	if (renderer->vertex_kept == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

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
	binding.stride = TERMINAL_VERTEX_FLOATS * sizeof(float);
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

/*
 * Stores the parts of a frame's vertices that were built (in the build,
 * each at its place, the ranges) into the vertex buffer's mapping: only
 * the doublewords that differ from what it holds, since the processor
 * reaches the mapping around its cache and each store is a bus write (a
 * typed character changes a cell or two, and a line moved up changes only
 * the atlas places of its cells).  The doublewords past those ever
 * written are stored all.  The rows not built are in the mapping already.
 */
static void
render_upload(
	struct terminal_renderer *renderer,
	uint32_t vertex_count)
{
	unsigned index;

	/* Each part built. */
	(void)vertex_count;
	for (index = 0U; index < renderer->build.range_count; index++)
		render_store(renderer, renderer->build.ranges[index].first, renderer->build.ranges[index].count);
	renderer->build.range_count = 0U;
}

/* Stores one part of the vertices built into the mapping and its copy, the doublewords that changed. */
static void
render_store(
	void *data,
	size_t first,
	size_t count)
{
	struct terminal_renderer *renderer;
	volatile render_word *mapped;
	const render_word *built;
	render_word *kept;
	uint64_t word;
	size_t index;
	size_t end;
	size_t known;

	/* The part's doublewords (a vertex is six), and how many the mapping was ever written. */
	renderer = data;
	index = first * TERMINAL_VERTEX_FLOATS * sizeof(float) / sizeof(uint64_t);
	end = (first + count) * TERMINAL_VERTEX_FLOATS * sizeof(float) / sizeof(uint64_t);
	known = renderer->vertex_known;
	mapped = renderer->vertex_map;
	built = (const render_word *)renderer->build.vertices;
	kept = (render_word *)renderer->vertex_kept;

	/* Those the mapping holds otherwise, then those never written. */
	for (; index < end && index < known; index++) {
		word = built[index];
		if (word != kept[index]) {
			mapped[index] = word;
			kept[index] = word;
		}
	}
	for (; index < end; index++) {
		word = built[index];
		mapped[index] = word;
		kept[index] = word;
	}

	/* What was ever written. */
	if (end > renderer->vertex_known)
		renderer->vertex_known = end;
}

/* Records the frame: the cleared pass and the draw of every cell. */
static void
render_record(
	struct terminal_renderer *renderer,
	VkCommandBuffer command,
	uint32_t image,
	uint32_t vertex_count,
	uint32_t ground)
{
	VkCommandBufferBeginInfo begin;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkDeviceSize offset;
	float size[4];

	/* A recording that may be submitted again (Begin resets it: the pool lets its buffers be reset). */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = 0U;
	(void)vkBeginCommandBuffer(command, &begin);

	/* The pass, cleared to the theme's background (the padding around the grid). */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = (float)((ground >> 16) & 0xffU) / 255.0f;
	clear.color.float32[1] = (float)((ground >> 8) & 0xffU) / 255.0f;
	clear.color.float32[2] = (float)(ground & 0xffU) / 255.0f;
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = renderer->pass;
	pass.framebuffer = renderer->targets[image].framebuffer;
	pass.renderArea.extent = renderer->extent;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The whole image is the viewport. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)renderer->extent.width;
	viewport.height = (float)renderer->extent.height;
	viewport.maxDepth = 1.0f;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent = renderer->extent;
	vkCmdSetViewport(command, 0U, 1U, &viewport);
	vkCmdSetScissor(command, 0U, 1U, &scissor);

	/* The cells: the pipeline, the atlas, the vertices and the window's size. */
	size[0] = (float)renderer->extent.width;
	size[1] = (float)renderer->extent.height;
	size[2] = 0.0f;
	size[3] = 0.0f;
	offset = 0U;
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->layout, 0U, 1U, &renderer->set, 0U, NULL);
	vkCmdBindVertexBuffers(command, 0U, 1U, &renderer->vertices, &offset);
	vkCmdPushConstants(command, renderer->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(size), size);
	if (vertex_count != 0U)
		vkCmdDraw(command, vertex_count, 1U, 0U, 0U);

	/* The pass ends with the image ready to present. */
	vkCmdEndRenderPass(command);
}

/*
 * Makes the page's image, its view and its staging buffer at a size (the
 * old ones go; the GPU is idle), the descriptor set that names it, and the
 * quad's vertices and the page's command buffer the first time.
 */
static VkResult
render_page_make(
	struct terminal_renderer *renderer,
	uint32_t width,
	uint32_t height)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkImageViewCreateInfo view;
	VkDescriptorSetAllocateInfo allocate;
	VkCommandBufferAllocateInfo command;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	VkResult error;

	/* The old image and its buffer, of another size. */
	render_page_free(renderer);

	/* The image: optimal, filled by a copy, sampled by the fragment shader. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_B8G8R8A8_UNORM;
	image.extent.width = width;
	image.extent.height = height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	renderer->operation = "vkCreateImage";
	error = vkCreateImage(renderer->device, &image, NULL, &renderer->page);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, the device's own, bound to it. */
	vkGetImageMemoryRequirements(renderer->device, renderer->page, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &renderer->page_memory);
	if (error != VK_SUCCESS)
		return error;
	renderer->operation = "vkBindImageMemory";
	error = vkBindImageMemory(renderer->device, renderer->page, renderer->page_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* The view the shader samples. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = renderer->page;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	renderer->operation = "vkCreateImageView";
	error = vkCreateImageView(renderer->device, &view, NULL, &renderer->page_view);
	if (error != VK_SUCCESS)
		return error;

	/* The buffer the pixels are copied from, four bytes a pixel. */
	error = render_page_buffer(renderer, (VkDeviceSize)width * height * 4U, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &renderer->page_staging, &renderer->page_staging_memory, &renderer->page_map);
	if (error != VK_SUCCESS)
		return error;

	/* The set that names the image, made once (the atlas's layout and sampler). */
	if (renderer->page_set == VK_NULL_HANDLE) {
		memset(&allocate, 0, sizeof(allocate));
		allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocate.descriptorPool = renderer->descriptor_pool;
		allocate.descriptorSetCount = 1U;
		allocate.pSetLayouts = &renderer->set_layout;
		renderer->operation = "vkAllocateDescriptorSets";
		error = vkAllocateDescriptorSets(renderer->device, &allocate, &renderer->page_set);
		if (error != VK_SUCCESS)
			return error;
	}

	/* The set names this image's view. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = renderer->sampler;
	image_info.imageView = renderer->page_view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = renderer->page_set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(renderer->device, 1U, &write, 0U, NULL);

	/* The quad's six vertices, made once. */
	if (renderer->page_vertices == VK_NULL_HANDLE) {
		error = render_page_buffer(renderer, (VkDeviceSize)TERMINAL_QUAD_VERTICES * TERMINAL_VERTEX_FLOATS * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
					   &renderer->page_vertices, &renderer->page_vertex_memory, &renderer->page_vertex_map);
		if (error != VK_SUCCESS)
			return error;
	}

	/* The command buffer of the pages, recorded again for each. */
	if (renderer->page_command == VK_NULL_HANDLE) {
		memset(&command, 0, sizeof(command));
		command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		command.commandPool = renderer->pool;
		command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		command.commandBufferCount = 1U;
		renderer->operation = "vkAllocateCommandBuffers";
		error = vkAllocateCommandBuffers(renderer->device, &command, &renderer->page_command);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: pages of the size can be shown. */
	renderer->page_width = width;
	renderer->page_height = height;
	return VK_SUCCESS;
}

/* Makes a host-visible, coherent buffer of a size and use, bound to its memory and mapped for good. */
static VkResult
render_page_buffer(
	struct terminal_renderer *renderer,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkBuffer *buffer,
	VkDeviceMemory *memory,
	void **map)
{
	VkBufferCreateInfo create;
	VkMemoryRequirements requirements;
	VkResult error;

	/* The buffer. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create.size = size;
	create.usage = usage;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	renderer->operation = "vkCreateBuffer";
	error = vkCreateBuffer(renderer->device, &create, NULL, buffer);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, which the host sees, bound to it. */
	vkGetBufferMemoryRequirements(renderer->device, *buffer, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory);
	if (error != VK_SUCCESS)
		return error;
	renderer->operation = "vkBindBufferMemory";
	error = vkBindBufferMemory(renderer->device, *buffer, *memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Mapped for the host's writes, for good. */
	renderer->operation = "vkMapMemory";
	error = vkMapMemory(renderer->device, *memory, 0U, VK_WHOLE_SIZE, 0U, map);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the buffer. */
	return VK_SUCCESS;
}

/* Releases the page's image, its view and its staging buffer (the GPU is idle). */
static void
render_page_free(
	struct terminal_renderer *renderer)
{
	/* Each one made. */
	if (renderer->page_view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, renderer->page_view, NULL);
	if (renderer->page != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, renderer->page, NULL);
	if (renderer->page_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->page_memory, NULL);
	if (renderer->page_staging != VK_NULL_HANDLE)
		vkDestroyBuffer(renderer->device, renderer->page_staging, NULL);
	if (renderer->page_staging_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->page_staging_memory, NULL);

	/* None now. */
	renderer->page_view = VK_NULL_HANDLE;
	renderer->page = VK_NULL_HANDLE;
	renderer->page_memory = VK_NULL_HANDLE;
	renderer->page_staging = VK_NULL_HANDLE;
	renderer->page_staging_memory = VK_NULL_HANDLE;
	renderer->page_map = NULL;
	renderer->page_width = 0U;
	renderer->page_height = 0U;
}

/*
 * Records a page's frame: the copy of the pixels into the page's image
 * (whose old contents are not kept), then the pass that draws its one
 * quad into the swapchain's image.
 */
static void
render_page_record(
	struct terminal_renderer *renderer,
	uint32_t image)
{
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkBufferImageCopy region;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkDeviceSize offset;
	float size[4];

	/* A recording submitted once (Begin resets it). */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	(void)vkBeginCommandBuffer(renderer->page_command, &begin);

	/* The image takes the copy after the page before was sampled. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = renderer->page;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(renderer->page_command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* The whole frame's pixels into the image. */
	memset(&region, 0, sizeof(region));
	region.bufferRowLength = renderer->page_width;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1U;
	region.imageExtent.width = renderer->page_width;
	region.imageExtent.height = renderer->page_height;
	region.imageExtent.depth = 1U;
	vkCmdCopyBufferToImage(renderer->page_command, renderer->page_staging, renderer->page, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);

	/* The pass samples the image after the copy. */
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(renderer->page_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* The pass, cleared (the quad covers it all). */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = renderer->pass;
	pass.framebuffer = renderer->targets[image].framebuffer;
	pass.renderArea.extent = renderer->extent;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(renderer->page_command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The whole image is the viewport. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)renderer->extent.width;
	viewport.height = (float)renderer->extent.height;
	viewport.maxDepth = 1.0f;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent = renderer->extent;
	vkCmdSetViewport(renderer->page_command, 0U, 1U, &viewport);
	vkCmdSetScissor(renderer->page_command, 0U, 1U, &scissor);

	/* The quad: the cells' pipeline, the page's set and vertices, and the window's size. */
	size[0] = (float)renderer->extent.width;
	size[1] = (float)renderer->extent.height;
	size[2] = 0.0f;
	size[3] = 0.0f;
	offset = 0U;
	vkCmdBindPipeline(renderer->page_command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline);
	vkCmdBindDescriptorSets(renderer->page_command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->layout, 0U, 1U, &renderer->page_set, 0U, NULL);
	vkCmdBindVertexBuffers(renderer->page_command, 0U, 1U, &renderer->page_vertices, &offset);
	vkCmdPushConstants(renderer->page_command, renderer->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(size), size);
	vkCmdDraw(renderer->page_command, TERMINAL_QUAD_VERTICES, 1U, 0U, 0U);

	/* The pass ends with the image ready to present. */
	vkCmdEndRenderPass(renderer->page_command);
}

/* Gives the monotonic clock in microseconds for ZTERM PERF (0 when it is not asked for). */
static uint64_t
render_now_us(void)
{
	uint64_t now;

	/* The measurement's clock. */
	now = terminal_perf_now_us();

	/* Reports it. */
	return now;
}

/* Adds the time since started (render_now_us) to a part of the frame's drawing, for ZTERM PERF. */
static void
render_perf_add(
	unsigned part,
	uint64_t started)
{
	uint64_t now;
	uint64_t spent;
	int on;

	/* Only for a measurement. */
	on = terminal_perf_on();
	if (!on)
		return;

	/* The time, never backwards. */
	now = render_now_us();
	spent = 0U;
	if (now > started)
		spent = now - started;

	/* The part's total and longest. */
	render_perf_total[part] += spent;
	if (spent > render_perf_most[part])
		render_perf_most[part] = spent;
}

/*
 * Writes the ZTERM PERF line every RENDER_PERF_MS while frames are drawn:
 * how many, and each part's average and longest time in milliseconds
 * (the grid's vertices and new glyphs, the acquire of an image the
 * compositor gave back, the recording, the submission, the present, the
 * wait for the GPU, and the stores of the vertices that changed into the
 * mapping), then starts the next window.
 */
static void
render_perf_report(void)
{
	static const char *const names[RENDER_PERF_PARTS] = { "build", "acquire", "record", "submit", "present", "wait", "upload" };
	uint64_t now;
	unsigned part;

	/* Not yet time. */
	now = terminal_clock();
	if (render_perf_since == 0U)
		render_perf_since = now;
	if (now - render_perf_since < RENDER_PERF_MS)
		return;

	/* The frames, then each part's average and longest. */
	printf("ZTERM PERF frames=%u", render_perf_frames);
	for (part = 0U; part < RENDER_PERF_PARTS; part++)
		printf(" %s=%.2f/%.2f", names[part], (double)render_perf_total[part] / 1000.0 / (double)render_perf_frames, (double)render_perf_most[part] / 1000.0);
	printf(" ms (average/longest)\n");
	fflush(stdout);

	/* The next window. */
	render_perf_frames = 0U;
	memset(render_perf_total, 0, sizeof(render_perf_total));
	memset(render_perf_most, 0, sizeof(render_perf_most));
	render_perf_since = now;
}
