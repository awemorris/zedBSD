/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing of terminal: every cell of the grid as two triangles
 * that sample the glyph atlas, with standard Vulkan and Wayland WSI.
 *
 * Each frame rebuilds the vertices of the whole grid in host-visible
 * memory and draws them in one call.  The frame before is waited for
 * first (terminal_renderer_idle), not right after its present, so the
 * terminal reads its shells while the GPU draws; the host writes the atlas
 * and the vertices only after that wait.
 */

#include "terminal.h"
#include "shaders.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How long a frame may take on the GPU, in nanoseconds. */
#define RENDER_TIMEOUT		10000000000ULL

/* The glyph atlas's size in pixels. */
#define RENDER_ATLAS_WIDTH	2048U
#define RENDER_ATLAS_HEIGHT	2048U

/* One vertex: position and atlas place, then the two colours; four floats each. */
#define RENDER_VERTEX_FLOATS	12U

/* The vertices of one cell: two triangles. */
#define RENDER_CELL_VERTICES	6U

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
static VkResult render_memory(struct terminal_renderer *renderer, VkMemoryRequirements *requirements, VkDeviceMemory *memory);
static VkResult render_atlas(struct terminal_renderer *renderer, struct terminal_font *font);
static VkResult render_vertices(struct terminal_renderer *renderer);
static VkResult render_pipeline(struct terminal_renderer *renderer);
static VkResult render_module(struct terminal_renderer *renderer, const uint32_t *code, size_t size, VkShaderModule *module);
static uint32_t render_build(struct terminal_renderer *renderer, struct terminal_screen *screen, struct terminal_font *font, const struct terminal_window *window);
static void render_upload(struct terminal_renderer *renderer, uint32_t vertex_count);
static void render_store(struct terminal_renderer *renderer, size_t first, size_t count);
static int render_rows_usable(struct terminal_renderer *renderer, const struct terminal_screen *screen, const struct terminal_font *font, const struct terminal_theme *theme, unsigned blank);
static void render_row_range(const struct terminal_screen *screen, unsigned long line, struct terminal_render_row *state);
static int render_row_same(const struct terminal_renderer *renderer, const struct terminal_render_row *kept, const struct terminal_render_row *state, const struct terminal_cell *cells, unsigned columns, unsigned index);
static void render_range_add(struct terminal_renderer *renderer, size_t first, size_t count);
static float *render_preedit(struct terminal_renderer *renderer, const struct terminal_screen *screen, struct terminal_font *font, const struct terminal_window *window, float *vertex, int first_row, int last_row);
static uint32_t render_utf8_next(const char *text, size_t *at);
static float *render_quad(float *vertex, const struct terminal_font *font, float x, float y, float width, float height, unsigned slot, uint32_t foreground, uint32_t background);
static void render_record(struct terminal_renderer *renderer, VkCommandBuffer command, uint32_t image, uint32_t vertex_count, uint32_t ground);
static const struct terminal_theme *render_theme(const struct terminal_window *window);
static uint32_t render_color(const struct terminal_theme *theme, uint32_t color);
static uint64_t render_now_us(void);
static void render_perf_add(unsigned part, uint64_t started);
static void render_perf_report(void);
static float *render_search_bar(struct terminal_renderer *renderer, const struct terminal_screen *screen, struct terminal_font *font, const struct terminal_window *window, const struct terminal_theme *theme, float *vertex);

/*
 * The atlas image is PREINITIALIZED until the first frame moves it to
 * GENERAL (with the host's writes kept); nonzero once that is done.
 */
static int render_atlas_ready;

/*
 * The ZTERM PERF line's sums since it was last written: the frames drawn,
 * each part's total and longest time in microseconds (RENDER_PERF_*), and
 * when the window began (terminal_clock).  The main thread's alone.  The
 * line is a measurement's: written only when the environment's
 * TERMINAL_PERF is 1 (render_perf_on, read when the window opens).
 */
static int render_perf_on;
static unsigned render_perf_frames;
static uint64_t render_perf_total[RENDER_PERF_PARTS];
static uint64_t render_perf_most[RENDER_PERF_PARTS];
static uint64_t render_perf_since;

/*
 * The colour themes of View > Theme (ws128-p006), by TERMINAL_THEME_*:
 * the dark one the terminal always had, a light one and white on black.
 */
static const struct terminal_theme render_themes[TERMINAL_THEMES] = {
	{ TERMINAL_FOREGROUND, TERMINAL_BACKGROUND, TERMINAL_SELECTION, 0x6b5a1eU, 0xe8b830U, 0x101010U, 0x2c3446U },
	{ 0x1f2328U, 0xfafafaU, 0xb6d2f5U, 0xf3e08cU, 0xf0a020U, 0x101010U, 0xe2e6ecU },
	{ 0xffffffU, 0x000000U, 0x1f5fffU, 0x7a5c00U, 0xffd000U, 0x000000U, 0x303030U }
};

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
	const char *extensions[2];
	const char *perf;
	VkResult error;
	int failed;

	/* Nothing is owned yet. */
	memset(renderer, 0, sizeof(*renderer));

	/* The ZTERM PERF line, when a measurement asks for it. */
	perf = getenv("TERMINAL_PERF");
	render_perf_on = 0;
	if (perf != NULL && strcmp(perf, "1") == 0)
		render_perf_on = 1;

	/* The atlas is moved to its layout by the first frame. */
	render_atlas_ready = 0;

	/* The instance, with the surface extensions a Wayland window needs. */
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "terminal";
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

	/* The window's surface (libkeiland's, WS131 p018). */
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
 * the cursor (the window's, BUG-155), presents it, and waits for the frame
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
	VkPipelineStageFlags stage;
	struct terminal_target *target;
	uint32_t vertex_count;
	uint32_t ground;
	uint32_t image;
	uint64_t mark;
	int kept;
	VkResult error;

	/* The frame before is finished before the host touches the vertices or the atlas again. */
	mark = render_now_us();
	error = terminal_renderer_idle(renderer);
	if (error != VK_SUCCESS)
		return error;
	render_perf_add(RENDER_PERF_WAIT, mark);

	/* The vertices of the grid and the composed text (drawing any new glyph into the atlas). */
	mark = render_now_us();
	vertex_count = render_build(renderer, screen, font, window);
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
	 * is (each Begin and End is a round trip to the GPU's driver).  The
	 * first frame's recording moves the atlas too, and is not kept.
	 */
	ground = render_theme(window)->background;
	if (!target->recorded || target->vertex_count != vertex_count || target->ground != ground) {
		kept = render_atlas_ready;
		render_record(renderer, target->command, image, vertex_count, ground);
		renderer->operation = "vkEndCommandBuffer";
		error = vkEndCommandBuffer(target->command);
		if (error != VK_SUCCESS)
			return error;
		target->vertex_count = vertex_count;
		target->ground = ground;
		target->recorded = kept;
	}

	/* The frame's fence starts unsignalled. */
	renderer->operation = "vkResetFences";
	error = vkResetFences(renderer->device, 1U, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;
	render_perf_add(RENDER_PERF_RECORD, mark);

	/* Submits the frame after the acquire, signalling the image's present semaphore. */
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &renderer->acquired;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &target->command;
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
	if (render_perf_on) {
		render_perf_frames++;
		render_perf_report();
	}

	/* Succeeded: the grid is on the window. */
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
		free(renderer->vertex_build);
		free(renderer->vertex_kept);
		free(renderer->rows);
		free(renderer->row_cells);

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

	/* Every cell's two triangles (four rows more, for a view between lines and the padding, ws081-p011) and the cursor's. */
	renderer->vertex_capacity = ((size_t)TERMINAL_MAX_COLUMNS * (TERMINAL_MAX_ROWS + 4U) + 1U) * RENDER_CELL_VERTICES;
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

	/* Where frames are built, and the copy of what the mapping holds (nothing yet). */
	renderer->vertex_build = malloc(buffer.size);
	renderer->vertex_kept = malloc(buffer.size);
	renderer->vertex_known = 0U;
	renderer->range_count = 0U;
	if (renderer->vertex_build == NULL || renderer->vertex_kept == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The rows as they are drawn, none yet. */
	renderer->rows = calloc(TERMINAL_RENDER_ROWS, sizeof(*renderer->rows));
	renderer->row_cells = malloc((size_t)TERMINAL_RENDER_ROWS * TERMINAL_MAX_COLUMNS * sizeof(*renderer->row_cells));
	renderer->row_frame = 0U;
	memset(&renderer->row_key, 0, sizeof(renderer->row_key));
	if (renderer->rows == NULL || renderer->row_cells == NULL)
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

/* Writes the vertices of every cell, of the cursor and of the composed text, and returns how many there are. */
static uint32_t
render_build(
	struct terminal_renderer *renderer,
	struct terminal_screen *screen,
	struct terminal_font *font,
	const struct terminal_window *window)
{
	static const struct terminal_cell empty = { ' ', TERMINAL_FOREGROUND, TERMINAL_BACKGROUND, 0 };
	static unsigned char marks[TERMINAL_MAX_COLUMNS];
	const struct terminal_theme *theme;
	const struct terminal_cell *cell;
	const struct terminal_cell *next;
	uint32_t query[TERMINAL_SEARCH_LENGTH];
	size_t query_count;
	int marked;
	int current;
	float *vertex;
	float *start;
	uint32_t foreground;
	uint32_t background;
	unsigned long line;
	unsigned long cursor_line;
	long long signed_line;
	long long oldest;
	long long newest;
	long long top;
	size_t used;
	unsigned column;
	int row;
	int first_row;
	int last_row;
	int inside;
	int selected;
	int wide;
	int cursor_here;
	unsigned cells;
	unsigned slot;
	unsigned blank;
	float x;
	float y;
	const struct terminal_cell *base;
	struct terminal_render_row *kept;
	struct terminal_render_row state;
	float *row_start;
	size_t first;
	int keep;
	int store;
	int same;
	int index;

	/* The blank glyph (a space), which empty cells and the right halves of wide characters use. */
	blank = terminal_font_slot(font, ' ');

	/* The theme's colours, and the text the search bar looks for (ws128-p006). */
	theme = render_theme(window);
	query_count = 0;
	if (window != NULL && window->search_open)
		query_count = terminal_search_decode(window->search_query, window->search_length, query, TERMINAL_SEARCH_LENGTH);

	/*
	 * Rows drawn in the frame before may be kept as the mapping holds
	 * them, unless what every row depends on changed; while the search
	 * marks its matches (its marks depend on the query and the lines
	 * around), every row is built and none is kept.
	 */
	renderer->row_frame++;
	renderer->range_count = 0U;
	keep = render_rows_usable(renderer, screen, font, theme, blank);
	store = 1;
	if (query_count != 0U || (window != NULL && window->search_found))
		store = 0;
	if (!store)
		keep = 0;

	/* The cursor's line, which the window shows only when the view reaches it (ws035-p114). */
	cursor_line = screen->scrolled + screen->cursor_row;

	/*
	 * The rows the view shows, moved down by its offset within a line
	 * (ws081-p011): the rows that meet the window, of the lines the
	 * terminal keeps (past either end nothing is drawn).
	 */
	first_row = 0;
	last_row = (int)screen->rows - 1;
	if (screen->view_offset > 0)
		first_row = -((screen->view_offset + (int)font->cell_height - 1) / (int)font->cell_height);
	if (screen->view_offset < 0)
		last_row = (int)screen->rows - 1 + (-screen->view_offset + (int)font->cell_height - 1) / (int)font->cell_height;
	oldest = (long long)screen->scrolled - (long long)screen->history_count;
	newest = (long long)screen->scrolled + (long long)screen->rows - 1;
	top = (long long)screen->scrolled - (long long)screen->view;

	/* Each cell of those lines, row after row, from the padded top left. */
	start = renderer->vertex_build;
	vertex = start;
	for (row = first_row; row <= last_row; row++) {
		/* A line the terminal keeps, whose row meets the window. */
		signed_line = top + (long long)row;
		if (signed_line < oldest || signed_line > newest)
			continue;
		y = (float)((int)TERMINAL_PADDING + row * (int)font->cell_height + screen->view_offset);
		if (y + (float)font->cell_height <= 0.0f || y >= (float)renderer->extent.height)
			continue;

		/* A row that would not fit the vertex buffer is not drawn (the buffer holds the most rows a window shows). */
		used = (size_t)(vertex - start) / RENDER_VERTEX_FLOATS;
		if (used + (size_t)screen->columns * RENDER_CELL_VERTICES > renderer->vertex_capacity)
			break;
		line = (unsigned long)signed_line;

		/*
		 * The row's own state: its place, the cursor on it, the
		 * selections on it.  A row the same as in the frame before, at
		 * the same place, has its vertices in the mapping already.
		 */
		memset(&state, 0, sizeof(state));
		state.valid = 1;
		state.frame = renderer->row_frame;
		state.first = used;
		state.y = y;
		state.cursor = -1;
		if (screen->cursor_visible && line == cursor_line)
			state.cursor = (long)screen->cursor_column;
		state.selected = 0;
		if (screen->selected && line >= screen->scrolled)
			state.selected = 1;
		render_row_range(screen, line, &state);
		base = terminal_screen_line_cell(screen, 0U, line);
		kept = NULL;
		index = row + 2;
		if (base != NULL && index >= 0 && index < (int)TERMINAL_RENDER_ROWS)
			kept = &renderer->rows[index];
		if (keep && kept != NULL) {
			same = render_row_same(renderer, kept, &state, base, screen->columns, (unsigned)index);
			if (same) {
				kept->frame = renderer->row_frame;
				vertex += kept->count * RENDER_VERTEX_FLOATS;
				continue;
			}
		}
		row_start = vertex;

		/* The cells the search's matches cover on this line. */
		marked = 0;
		if (query_count != 0)
			marked = terminal_search_line(screen, query, query_count, line, marks);
		for (column = 0U; column < screen->columns; column++) {
			/* The line's cell; a line the terminal no longer keeps shows blanks. */
			cell = terminal_screen_line_cell(screen, column, line);
			if (cell == NULL)
				cell = &empty;

			/*
			 * A character whose right half is the next cell is drawn once,
			 * two cells wide, and the right half is not drawn on its own
			 * (ws128-p009).  A left half whose right half was written over
			 * is drawn in its one cell.
			 */
			wide = 0;
			if (cell->continuation == 0 &&
			    cell->codepoint != 0U &&
			    column + 1U < screen->columns) {
				/* The cell to the right, which is this one's right half when it is a continuation. */
				next = terminal_screen_line_cell(screen, column + 1U, line);
				if (next != NULL && next->continuation != 0)
					wide = 1;
			}

			/* The glyph's slot: the blank for spaces and continuations, two slots for a wide character. */
			cells = 1U;
			slot = blank;
			if (wide) {
				cells = 2U;
				slot = terminal_font_wide_slot(font, cell->codepoint);
			} else if (cell->continuation == 0 &&
				   cell->codepoint != ' ' &&
				   cell->codepoint != 0U) {
				slot = terminal_font_slot(font, cell->codepoint);
			}

			/*
			 * A selected screen (Edit > Select All, the live screen's lines)
			 * or cell (the pointer's range, either half of a wide
			 * character) has the selection's background.
			 */
			foreground = render_color(theme, cell->foreground);
			background = render_color(theme, cell->background);
			inside = terminal_screen_in_range(screen, column, line);
			if (!inside && wide)
				inside = terminal_screen_in_range(screen, column + 1U, line);
			selected = 0;
			if (screen->selected && line >= screen->scrolled)
				selected = 1;
			if (selected || inside)
				background = theme->selection;

			/* A match of the search is marked, the one found most strongly (ws128-p006). */
			current = 0;
			if (window != NULL &&
			    window->search_found &&
			    line == window->search_line &&
			    column >= window->search_column &&
			    column < window->search_column + window->search_cells)
				current = 1;
			if (current) {
				background = theme->current;
				foreground = theme->match_text;
			} else if (marked != 0 && marks[column] != 0) {
				background = theme->match;
			}

			/* Whether the cursor is on this cell, or on the right half of this wide character. */
			cursor_here = 0;
			if (screen->cursor_visible && line == cursor_line) {
				/* The cursor's column is this cell, or the right half drawn with it. */
				if (column == screen->cursor_column)
					cursor_here = 1;
				else if (wide && column + 1U == screen->cursor_column)
					cursor_here = 1;
			}

			/* The cursor's cell is drawn with its colours swapped: a block cursor. */
			if (cursor_here) {
				foreground = render_color(theme, cell->background);
				background = render_color(theme, cell->foreground);
			}

			/* The cell's two triangles, over both cells of a wide character. */
			x = (float)(TERMINAL_PADDING + column * font->cell_width);
			vertex = render_quad(vertex, font, x, y, (float)(cells * font->cell_width), (float)font->cell_height, slot, foreground, background);

			/* The right half of a wide character is drawn with its left. */
			if (wide)
				column++;
		}

		/* The row's vertices are stored into the mapping; the row is kept for the next frame, or not. */
		state.count = (size_t)(vertex - row_start) / RENDER_VERTEX_FLOATS;
		render_range_add(renderer, state.first, state.count);
		if (kept != NULL && store) {
			*kept = state;
			memcpy(renderer->row_cells + (size_t)index * TERMINAL_MAX_COLUMNS, base, (size_t)screen->columns * sizeof(*base));
		} else if (kept != NULL) {
			kept->valid = 0;
		}
	}

	/* What follows the rows is built every frame. */
	first = (size_t)(vertex - start) / RENDER_VERTEX_FLOATS;

	/* The search bar over the last row, while it is open (ws128-p006). */
	vertex = render_search_bar(renderer, screen, font, window, theme, vertex);

	/* The input method's text being composed, over the cells from the cursor (BUG-155), or from the search bar's caret while it is open (ws090-p022). */
	vertex = render_preedit(renderer, screen, font, window, vertex, first_row, last_row);
	render_range_add(renderer, first, (size_t)(vertex - start) / RENDER_VERTEX_FLOATS - first);

	/* Reports how many vertices the frame draws. */
	return (uint32_t)((size_t)(vertex - start) / RENDER_VERTEX_FLOATS);
}

/*
 * Tells whether the rows kept from the frame before may be used: what
 * every row depends on beside its own state is as it was (render_key).
 * When it is not, it is kept as it is now and every row is dropped.
 */
static int
render_rows_usable(
	struct terminal_renderer *renderer,
	const struct terminal_screen *screen,
	const struct terminal_font *font,
	const struct terminal_theme *theme,
	unsigned blank)
{
	struct terminal_render_key key;
	unsigned index;
	int same;

	/* What the rows depend on now (an atlas holding fewer glyphs started again). */
	memset(&key, 0, sizeof(key));
	key.theme = theme;
	key.font = font;
	key.face = font->face;
	key.pixels_size = font->pixels_size;
	key.cell_width = font->cell_width;
	key.cell_height = font->cell_height;
	key.atlas_width = font->atlas_width;
	key.atlas_height = font->atlas_height;
	key.used = font->used;
	key.blank = blank;
	key.columns = screen->columns;
	key.rows = screen->rows;
	key.width = renderer->extent.width;
	key.height = renderer->extent.height;
	same = 1;
	if (key.theme != renderer->row_key.theme ||
	    key.font != renderer->row_key.font ||
	    key.face != renderer->row_key.face ||
	    key.pixels_size != renderer->row_key.pixels_size ||
	    key.cell_width != renderer->row_key.cell_width ||
	    key.cell_height != renderer->row_key.cell_height ||
	    key.atlas_width != renderer->row_key.atlas_width ||
	    key.atlas_height != renderer->row_key.atlas_height ||
	    key.used < renderer->row_key.used ||
	    key.blank != renderer->row_key.blank ||
	    key.columns != renderer->row_key.columns ||
	    key.rows != renderer->row_key.rows ||
	    key.width != renderer->row_key.width ||
	    key.height != renderer->row_key.height)
		same = 0;
	renderer->row_key = key;

	/* Changed: no row is kept. */
	if (!same) {
		for (index = 0U; index < TERMINAL_RENDER_ROWS; index++)
			renderer->rows[index].valid = 0;
	}
	return same;
}

/*
 * Fills a row's columns of the pointer's range on a line: from its first
 * column to its last, or none (from past to).
 */
static void
render_row_range(
	const struct terminal_screen *screen,
	unsigned long line,
	struct terminal_render_row *state)
{
	/* None. */
	state->range_from = 1U;
	state->range_to = 0U;
	if (!screen->range || line < screen->range_from[1] || line > screen->range_to[1])
		return;

	/* The whole line, but from the range's first cell on its first line and up to its last on its last. */
	state->range_from = 0U;
	state->range_to = ULONG_MAX;
	if (line == screen->range_from[1])
		state->range_from = screen->range_from[0];
	if (line == screen->range_to[1])
		state->range_to = screen->range_to[0];
}

/*
 * Tells whether a row is drawn as it was in the frame before, at the same
 * place: kept then, with the same state and the same cells.
 */
static int
render_row_same(
	const struct terminal_renderer *renderer,
	const struct terminal_render_row *kept,
	const struct terminal_render_row *state,
	const struct terminal_cell *cells,
	unsigned columns,
	unsigned index)
{
	const struct terminal_cell *was;
	unsigned column;

	/* Kept in the frame before, at the same place, in the same state. */
	if (!kept->valid || kept->frame + 1U != renderer->row_frame)
		return 0;
	if (kept->first != state->first ||
	    kept->y != state->y ||
	    kept->cursor != state->cursor ||
	    kept->selected != state->selected ||
	    kept->range_from != state->range_from ||
	    kept->range_to != state->range_to)
		return 0;

	/* The same cells. */
	was = renderer->row_cells + (size_t)index * TERMINAL_MAX_COLUMNS;
	for (column = 0U; column < columns; column++) {
		if (was[column].codepoint != cells[column].codepoint ||
		    was[column].foreground != cells[column].foreground ||
		    was[column].background != cells[column].background ||
		    was[column].continuation != cells[column].continuation)
			return 0;
	}

	/* Succeeded: drawn as it was. */
	return 1;
}

/* Adds a part of the frame's vertices that was built, joined to the part before it when they meet. */
static void
render_range_add(
	struct terminal_renderer *renderer,
	size_t first,
	size_t count)
{
	struct terminal_render_range *last;

	/* Nothing built. */
	if (count == 0U)
		return;

	/* After the part before it. */
	if (renderer->range_count != 0U) {
		last = &renderer->ranges[renderer->range_count - 1U];
		if (last->first + last->count == first) {
			last->count += count;
			return;
		}
	}

	/* A new part; with the list full it is stored at once (no frame reads the vertices while they are built). */
	if (renderer->range_count == sizeof(renderer->ranges) / sizeof(renderer->ranges[0])) {
		render_store(renderer, first, count);
		return;
	}
	renderer->ranges[renderer->range_count].first = first;
	renderer->ranges[renderer->range_count].count = count;
	renderer->range_count++;
}

/*
 * Stores the parts of a frame's vertices that were built (in vertex_build,
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
	for (index = 0U; index < renderer->range_count; index++)
		render_store(renderer, renderer->ranges[index].first, renderer->ranges[index].count);
	renderer->range_count = 0U;
}

/* Stores one part of the vertices built into the mapping and its copy, the doublewords that changed. */
static void
render_store(
	struct terminal_renderer *renderer,
	size_t first,
	size_t count)
{
	volatile render_word *mapped;
	const render_word *built;
	render_word *kept;
	uint64_t word;
	size_t index;
	size_t end;
	size_t known;

	/* The part's doublewords (a vertex is six), and how many the mapping was ever written. */
	index = first * RENDER_VERTEX_FLOATS * sizeof(float) / sizeof(uint64_t);
	end = (first + count) * RENDER_VERTEX_FLOATS * sizeof(float) / sizeof(uint64_t);
	known = renderer->vertex_known;
	mapped = renderer->vertex_map;
	built = (const render_word *)renderer->vertex_build;
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

/*
 * Writes the vertices of the input method's text being composed: from the
 * cursor's cell rightward, a wide character over two cells, on the
 * selection's background with the segment being converted in reverse, cut
 * at the right edge of the grid.  The cursor's block is drawn over.
 * Returns where the next quad goes.
 */
static float *
render_preedit(
	struct terminal_renderer *renderer,
	const struct terminal_screen *screen,
	struct terminal_font *font,
	const struct terminal_window *window,
	float *vertex,
	int first_row,
	int last_row)
{
	const float *start;
	uint32_t codepoint;
	uint32_t foreground;
	uint32_t background;
	size_t at;
	size_t used;
	long long row;
	unsigned column;
	unsigned cells;
	unsigned slot;
	int in_segment;
	int wide;
	float x;
	float y;

	/* Nothing is being composed. */
	if (window == NULL || window->preedit[0] == '\0')
		return vertex;

	/* While the search bar is open: from its caret, on the grid's last row (as the bar draws it). */
	if (window->search_open && screen->rows != 0U) {
		y = (float)(TERMINAL_PADDING + (screen->rows - 1U) * font->cell_height);
		column = terminal_search_bar_column(window->search_query, window->search_length, screen->ambiguous_wide);
	} else {
		/* The cursor's row in the view; a cursor the view does not show shows no composed text. */
		row = (long long)screen->cursor_row + (long long)screen->view;
		if (row < (long long)first_row || row > (long long)last_row)
			return vertex;
		y = (float)((int)TERMINAL_PADDING + (int)row * (int)font->cell_height + screen->view_offset);
		column = screen->cursor_column;
	}

	/* Each character, from there, while it fits the grid and the vertex buffer. */
	start = renderer->vertex_build;
	at = 0U;
	while (window->preedit[at] != '\0') {
		/* The character, and where the next one starts. */
		in_segment = 0;
		if (window->preedit_begin >= 0 &&
		    window->preedit_end > window->preedit_begin &&
		    at >= (size_t)window->preedit_begin &&
		    at < (size_t)window->preedit_end)
			in_segment = 1;
		codepoint = render_utf8_next(window->preedit, &at);

		/* Its width in cells, as the grid would place it. */
		cells = 1U;
		wide = terminal_width_wide(codepoint, screen->ambiguous_wide);
		if (wide)
			cells = 2U;

		/* Past the grid's right edge the rest is not drawn. */
		if (column + cells > screen->columns)
			break;

		/* A full vertex buffer draws no more. */
		used = (size_t)(vertex - start) / RENDER_VERTEX_FLOATS;
		if (used + RENDER_CELL_VERTICES > renderer->vertex_capacity)
			break;

		/* The glyph: two slots for a wide character. */
		if (cells == 2U) {
			slot = terminal_font_wide_slot(font, codepoint);
		} else {
			slot = terminal_font_slot(font, codepoint);
		}

		/* The segment being converted in reverse, the rest on the selection's background. */
		if (in_segment) {
			foreground = render_theme(window)->background;
			background = render_theme(window)->foreground;
		} else {
			foreground = render_theme(window)->foreground;
			background = render_theme(window)->selection;
		}

		/* The character's quad, and the next cell. */
		x = (float)(TERMINAL_PADDING + column * font->cell_width);
		vertex = render_quad(vertex, font, x, y, (float)(cells * font->cell_width), (float)font->cell_height, slot, foreground, background);
		column += cells;
	}

	/* Succeeded: the composed text is drawn. */
	return vertex;
}

/*
 * Writes the vertices of the search bar (ws128-p006) over the grid's last
 * row while it is open: "Find:", the text looked for, a block where the
 * next character goes, and "not found" when the text is nowhere.  Returns
 * where the next quad goes.
 */
static float *
render_search_bar(
	struct terminal_renderer *renderer,
	const struct terminal_screen *screen,
	struct terminal_font *font,
	const struct terminal_window *window,
	const struct terminal_theme *theme,
	float *vertex)
{
	char text[TERMINAL_SEARCH_BYTES + 48];
	const float *start;
	uint32_t codepoint;
	uint32_t foreground;
	uint32_t background;
	size_t at;
	size_t used;
	size_t caret;
	unsigned column;
	unsigned cells;
	unsigned slot;
	int wide;
	float x;
	float y;

	/* Only while the bar is open, on a grid with a row for it. */
	if (window == NULL ||
	    !window->search_open ||
	    screen->rows == 0U)
		return vertex;

	/* The bar's words; the block goes after the text looked for. */
	snprintf(text, sizeof(text), "Find: %s", window->search_query);
	caret = strlen(text);
	if (window->search_length != 0U && !window->search_found)
		snprintf(text + caret, sizeof(text) - caret, "   not found");

	/* The last row of the grid, from its first column. */
	y = (float)(TERMINAL_PADDING + (screen->rows - 1U) * font->cell_height);
	start = renderer->vertex_build;
	column = 0U;
	at = 0;

	/* Each cell of the row: a character of the words, the block, or the bar's background. */
	while (column < screen->columns) {
		/* A full vertex buffer draws no more. */
		used = (size_t)(vertex - start) / RENDER_VERTEX_FLOATS;
		if (used + RENDER_CELL_VERTICES > renderer->vertex_capacity)
			break;

		/* The next character of the words, the block at the caret, or a blank past them. */
		foreground = theme->foreground;
		background = theme->bar;
		codepoint = ' ';
		if (at == caret) {
			background = theme->foreground;
			at++;
			caret = (size_t)-1;
		} else if (at < sizeof(text) && text[at] != '\0') {
			codepoint = render_utf8_next(text, &at);
		}

		/* Its width in cells; a wide character that does not fit ends the row. */
		cells = 1U;
		wide = terminal_width_wide(codepoint, screen->ambiguous_wide);
		if (wide)
			cells = 2U;
		if (column + cells > screen->columns)
			break;

		/* Its glyph. */
		if (cells == 2U) {
			slot = terminal_font_wide_slot(font, codepoint);
		} else {
			slot = terminal_font_slot(font, codepoint);
		}

		/* The cell's quad, and the next cell. */
		x = (float)(TERMINAL_PADDING + column * font->cell_width);
		vertex = render_quad(vertex, font, x, y, (float)(cells * font->cell_width), (float)font->cell_height, slot, foreground, background);
		column += cells;
	}

	/* Succeeded: the bar is drawn. */
	return vertex;
}

/* Reports the colours of the window's theme (the dark one without a window). */
static const struct terminal_theme *
render_theme(
	const struct terminal_window *window)
{
	/* No window, or a theme the table does not have, is the dark one. */
	if (window == NULL || window->theme >= TERMINAL_THEMES)
		return &render_themes[TERMINAL_THEME_DARK];

	/* The window's theme. */
	return &render_themes[window->theme];
}

/*
 * Reports the colour a cell's colour is drawn in: the text's default
 * colours (the dark theme's, which the cells keep) become the theme's;
 * any other colour is itself.
 */
static uint32_t
render_color(
	const struct terminal_theme *theme,
	uint32_t color)
{
	/* The default foreground. */
	if (color == TERMINAL_FOREGROUND)
		return theme->foreground;

	/* The default background. */
	if (color == TERMINAL_BACKGROUND)
		return theme->background;

	/* Any other colour. */
	return color;
}

/*
 * Reads one UTF-8 character of a text at *at and moves *at past it; a
 * byte that does not start a whole character is read as U+FFFD alone.
 */
static uint32_t
render_utf8_next(
	const char *text,
	size_t *at)
{
	const unsigned char *bytes;
	uint32_t codepoint;
	unsigned remaining;
	unsigned index;

	/* The lead byte says how many continuation bytes follow. */
	bytes = (const unsigned char *)text + *at;
	if (bytes[0] < 0x80U) {
		*at += 1U;
		return bytes[0];
	}

	/* Two, three or four bytes by the lead byte's high bits; any other lead byte is a replacement alone. */
	if ((bytes[0] & 0xe0U) == 0xc0U) {
		codepoint = bytes[0] & 0x1fU;
		remaining = 1U;
	} else if ((bytes[0] & 0xf0U) == 0xe0U) {
		codepoint = bytes[0] & 0x0fU;
		remaining = 2U;
	} else if ((bytes[0] & 0xf8U) == 0xf0U) {
		codepoint = bytes[0] & 0x07U;
		remaining = 3U;
	} else {
		*at += 1U;
		return 0xfffdU;
	}

	/* Each continuation byte; a missing one makes the lead byte a replacement alone. */
	for (index = 1U; index <= remaining; index++) {
		if ((bytes[index] & 0xc0U) != 0x80U) {
			*at += 1U;
			return 0xfffdU;
		}

		/* Its six bits join the character. */
		codepoint = (codepoint << 6) | (bytes[index] & 0x3fU);
	}

	/* Succeeded: the character, and the bytes it took. */
	*at += 1U + remaining;
	return codepoint;
}

/*
 * Writes the six vertices of one quad showing a slot of the atlas (as wide
 * as the quad: two slots side by side for a wide character), and returns
 * where the next quad goes.
 */
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
	slot_width = width / (float)font->atlas_width;
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
	VkCommandBuffer command,
	uint32_t image,
	uint32_t vertex_count,
	uint32_t ground)
{
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
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
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
		render_atlas_ready = 1;
	}

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

/* Gives the monotonic clock in microseconds for ZTERM PERF (0 when it is not asked for or cannot be read). */
static uint64_t
render_now_us(void)
{
	struct timespec now;
	int status;

	/* Only for a measurement. */
	if (!render_perf_on)
		return 0U;

	/* The monotonic clock. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0U;

	/* Succeeded: microseconds. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/*
 * Writes a ZTERM LAT line, one step of a typed key's way to the screen
 * (key, commit, write, read, present) with its bytes and the monotonic
 * time in microseconds, when the measurement is on (TERMINAL_PERF=1).
 */
void
terminal_perf_step(
	const char *step,
	unsigned bytes)
{
	uint64_t now;

	/* Only for a measurement. */
	now = render_now_us();
	if (now == 0U)
		return;

	/* The step's line. */
	printf("ZTERM LAT %s bytes=%u at_us=%llu\n", step, bytes, (unsigned long long)now);
	fflush(stdout);
}

/* Adds the time since started (render_now_us) to a part of the frame's drawing, for ZTERM PERF. */
static void
render_perf_add(
	unsigned part,
	uint64_t started)
{
	uint64_t now;
	uint64_t spent;

	/* Only for a measurement. */
	if (!render_perf_on)
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
