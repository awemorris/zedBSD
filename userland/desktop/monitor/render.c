/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's drawing with Vulkan and the Wayland WSI (design.md
 * section 4), on the model of Notes' (userland/desktop/notes/render.c).
 *
 * A frame is the scene's list of draws over one vertex buffer: shapes
 * (plates, lines, gradients) and glyphs from the atlas.  Two pipelines
 * share one vertex layout: the shape pipeline's fragment shader draws each
 * shape from its local point, the glyph pipeline samples the atlas.  The
 * window's pass clears to the background's colour.  Each frame is waited
 * for before the next, so the host may write the vertices between frames
 * without further synchronization.
 *
 * The atlas is a linear image in host-visible memory, written once when it
 * is drawn (and again when the window's scale changes, after the device is
 * idle).
 *
 * A swapchain that is out of date is made again at the window's size; a
 * lost surface (the WSI gives up after waiting long for the compositor,
 * design.md section 4.3) is made again with its swapchain.
 */

#include "app.h"
#include "shaders.h"

#include <stdlib.h>
#include <string.h>

/* How long a frame may take on the GPU, in nanoseconds. */
#define RENDER_TIMEOUT		10000000000ULL

/* The vertex buffer's first size, in vertices. */
#define RENDER_VERTICES_MIN	16384U

/* The colour the window is cleared to (bg.deep, design.md section 2.4), as floats. */
#define RENDER_CLEAR_RED	0.063f
#define RENDER_CLEAR_GREEN	0.082f
#define RENDER_CLEAR_BLUE	0.118f

static VkResult render_surface(struct sm_renderer *renderer, struct wl_display *display, struct wl_surface *surface);
static VkResult render_device(struct sm_renderer *renderer);
static VkResult render_swapchain(struct sm_renderer *renderer, uint32_t width, uint32_t height, VkSwapchainKHR old);
static VkResult render_pass(struct sm_renderer *renderer);
static VkResult render_targets(struct sm_renderer *renderer);
static void render_targets_free(struct sm_renderer *renderer);
static VkResult render_commands(struct sm_renderer *renderer);
static VkResult render_memory(struct sm_renderer *renderer, const VkMemoryRequirements *requirements, VkMemoryPropertyFlags wanted, VkDeviceMemory *memory);
static VkResult render_descriptors(struct sm_renderer *renderer);
static void render_atlas_free(struct sm_renderer *renderer);
static VkResult render_vertices(struct sm_renderer *renderer, size_t count);
static void render_vertices_free(struct sm_renderer *renderer);
static VkResult render_pipelines(struct sm_renderer *renderer);
static VkResult render_pipeline(struct sm_renderer *renderer, unsigned pipe, VkShaderModule vertex, VkShaderModule fragment);
static VkResult render_module(struct sm_renderer *renderer, const uint32_t *code, size_t size, VkShaderModule *module);
static void render_record(struct sm_renderer *renderer, uint32_t image, const struct sm_scene *scene);

/*
 * Makes the Vulkan objects of the window: the instance and the surface,
 * the device, the swapchain and its targets, the pass and the pipelines,
 * and the vertex buffer.  The atlas comes with sm_renderer_atlas.
 */
VkResult
sm_renderer_open(
	struct sm_renderer *renderer,
	struct wl_display *display,
	struct wl_surface *surface,
	uint32_t width,
	uint32_t height)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	const char *extensions[2];
	VkResult error;

	/* Nothing is owned yet. */
	memset(renderer, 0, sizeof(*renderer));

	/* The instance, with the surface extensions a Wayland window needs. */
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "monitor";
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
	error = render_surface(renderer, display, surface);
	if (error != VK_SUCCESS)
		return error;

	/* A device with a queue that draws and presents to the surface. */
	error = render_device(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The swapchain at the window's size. */
	error = render_swapchain(renderer, width, height, VK_NULL_HANDLE);
	if (error != VK_SUCCESS)
		return error;

	/* The pass that draws into a swapchain image. */
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

	/* The sampler and the descriptor set of the atlas. */
	error = render_descriptors(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The vertex buffer, at its first size. */
	error = render_vertices(renderer, RENDER_VERTICES_MIN);
	if (error != VK_SUCCESS)
		return error;

	/* The pipelines. */
	error = render_pipelines(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: frames can be drawn once the atlas is given. */
	return VK_SUCCESS;
}

/*
 * Replaces the swapchain and its targets with ones of a new size.
 */
VkResult
sm_renderer_resize(
	struct sm_renderer *renderer,
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
	if (old != VK_NULL_HANDLE)
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
 * Makes the surface and the swapchain again after the WSI lost the
 * surface (design.md section 4.3); the device and the rest stay.
 */
VkResult
sm_renderer_recover(
	struct sm_renderer *renderer,
	struct wl_display *display,
	struct wl_surface *surface,
	uint32_t width,
	uint32_t height)
{
	VkBool32 supported;
	VkResult error;

	/* Nothing may still use the old images. */
	renderer->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(renderer->device);
	if (error != VK_SUCCESS)
		return error;

	/* The swapchain and the surface go. */
	render_targets_free(renderer);
	if (renderer->swapchain != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(renderer->device, renderer->swapchain, NULL);
	renderer->swapchain = VK_NULL_HANDLE;
	if (renderer->surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(renderer->instance, renderer->surface, NULL);
	renderer->surface = VK_NULL_HANDLE;

	/* A new surface over the same window, which the queue still presents to. */
	error = render_surface(renderer, display, surface);
	if (error != VK_SUCCESS)
		return error;
	supported = VK_FALSE;
	renderer->operation = "vkGetPhysicalDeviceSurfaceSupportKHR";
	error = vkGetPhysicalDeviceSurfaceSupportKHR(renderer->physical, renderer->family, renderer->surface, &supported);
	if (error != VK_SUCCESS)
		return error;
	if (supported == VK_FALSE)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* The swapchain and the targets again. */
	error = render_swapchain(renderer, width, height, VK_NULL_HANDLE);
	if (error != VK_SUCCESS)
		return error;
	error = render_targets(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: frames can be drawn again. */
	return VK_SUCCESS;
}

/*
 * Makes the atlas's image and copies the atlas's pixels into it, unless
 * this atlas (by its serial) is there already.
 */
VkResult
sm_renderer_atlas(
	struct sm_renderer *renderer,
	const struct sm_atlas *atlas)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout row_layout;
	VkImageViewCreateInfo view;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	unsigned char *rows;
	void *map;
	int row;
	VkResult error;

	/* The same atlas stands. */
	if (renderer->atlas != VK_NULL_HANDLE && renderer->atlas_serial == atlas->serial)
		return VK_SUCCESS;

	/* The old image goes once nothing uses it. */
	renderer->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(renderer->device);
	if (error != VK_SUCCESS)
		return error;
	render_atlas_free(renderer);

	/* The image: linear so that the host writes its rows, sampled by the glyph shader. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_B8G8R8A8_UNORM;
	image.extent.width = (uint32_t)atlas->width;
	image.extent.height = (uint32_t)atlas->height;
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

	/* Its memory, which the host sees. */
	vkGetImageMemoryRequirements(renderer->device, renderer->atlas, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			      &renderer->atlas_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the image. */
	renderer->operation = "vkBindImageMemory";
	error = vkBindImageMemory(renderer->device, renderer->atlas, renderer->atlas_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps it, finds where its rows start, and copies the atlas's rows in. */
	renderer->operation = "vkMapMemory";
	error = vkMapMemory(renderer->device, renderer->atlas_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (error != VK_SUCCESS)
		return error;
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(renderer->device, renderer->atlas, &subresource, &row_layout);
	rows = (unsigned char *)map + row_layout.offset;
	for (row = 0; row < atlas->height; row++) {
		memcpy(rows + (size_t)row * (size_t)row_layout.rowPitch, atlas->pixels + (size_t)row * (size_t)atlas->width,
		       (size_t)atlas->width * sizeof(uint32_t));
	}

	/* The rows are written; the memory needs no mapping any more. */
	vkUnmapMemory(renderer->device, renderer->atlas_memory);

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

	/* The set names the image in the general layout it is kept in. */
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

	/* The next frame moves the image to the general layout. */
	renderer->atlas_serial = atlas->serial;
	renderer->atlas_ready = 0;

	/* Succeeded: the atlas is uploaded. */
	return VK_SUCCESS;
}

/*
 * Draws a scene on the window and waits for it; reports how long each part
 * of the frame took in times.  VK_ERROR_OUT_OF_DATE_KHR and VK_ERROR_SURFACE_LOST_KHR
 * are the caller's to recover from (sm_renderer_resize, sm_renderer_recover).
 */
VkResult
sm_renderer_draw(
	struct sm_renderer *renderer,
	const struct sm_scene *scene,
	struct sm_frame_times *times)
{
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkPipelineStageFlags stage;
	uint64_t before;
	uint32_t image;
	VkResult waited;
	VkResult error;

	/* Vertices beyond the buffer's capacity get a larger buffer. */
	if (scene->vertex_count > renderer->vertex_capacity) {
		renderer->operation = "vkDeviceWaitIdle";
		error = vkDeviceWaitIdle(renderer->device);
		if (error != VK_SUCCESS)
			return error;
		render_vertices_free(renderer);
		error = render_vertices(renderer, scene->vertex_count);
		if (error != VK_SUCCESS)
			return error;
	}

	/* The scene's vertices. */
	if (scene->vertex_count != 0U)
		memcpy(renderer->vertex_map, scene->vertices, scene->vertex_count * SM_VERTEX_FLOATS * sizeof(float));

	/* The image to draw into, once the compositor has given one back, however long a busy compositor takes (a closed connection ends the wait with an error). */
	memset(times, 0, sizeof(*times));
	before = kl_clock_us();
	renderer->operation = "vkAcquireNextImageKHR";
	error = vkAcquireNextImageKHR(renderer->device, renderer->swapchain, UINT64_MAX, renderer->acquired, VK_NULL_HANDLE, &image);
	times->acquire_us = kl_clock_us() - before;
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* The frame's commands. */
	before = kl_clock_us();
	renderer->operation = "vkResetCommandBuffer";
	error = vkResetCommandBuffer(renderer->command, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Records the frame and closes the recording. */
	render_record(renderer, image, scene);
	renderer->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(renderer->command);
	times->record_us = kl_clock_us() - before;
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
	before = kl_clock_us();
	renderer->operation = "vkQueueSubmit";
	error = vkQueueSubmit(renderer->queue, 1U, &submit, renderer->fence);
	times->submit_us = kl_clock_us() - before;
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
	before = kl_clock_us();
	renderer->operation = "vkQueuePresentKHR";
	error = vkQueuePresentKHR(renderer->queue, &present);
	times->present_us = kl_clock_us() - before;

	/* The frame is finished before the host touches the vertices again, whatever the present said. */
	before = kl_clock_us();
	renderer->operation = "vkWaitForFences";
	waited = vkWaitForFences(renderer->device, 1U, &renderer->fence, VK_TRUE, RENDER_TIMEOUT);
	if (waited != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;
	times->wait_us = kl_clock_us() - before;

	/* The present's answer. */
	renderer->operation = "vkQueuePresentKHR";
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* Succeeded: the frame is on the window. */
	return VK_SUCCESS;
}

/*
 * Releases every Vulkan object, children before their parents.
 */
void
sm_renderer_close(
	struct sm_renderer *renderer)
{
	unsigned pipe;

	/* The device's objects, once nothing runs. */
	if (renderer->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(renderer->device);
		render_targets_free(renderer);
		render_atlas_free(renderer);
		render_vertices_free(renderer);

		/* The pipelines and what they bind. */
		for (pipe = 0; pipe < SM_PIPES; pipe++) {
			if (renderer->pipes[pipe] != VK_NULL_HANDLE)
				vkDestroyPipeline(renderer->device, renderer->pipes[pipe], NULL);
		}

		/* The layout, the set and its sampler. */
		if (renderer->layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(renderer->device, renderer->layout, NULL);
		if (renderer->descriptor_pool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(renderer->device, renderer->descriptor_pool, NULL);
		if (renderer->set_layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(renderer->device, renderer->set_layout, NULL);
		if (renderer->sampler != VK_NULL_HANDLE)
			vkDestroySampler(renderer->device, renderer->sampler, NULL);

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

/* Makes the Vulkan surface of the window's Wayland surface. */
static VkResult
render_surface(
	struct sm_renderer *renderer,
	struct wl_display *display,
	struct wl_surface *surface)
{
	VkWaylandSurfaceCreateInfoKHR create;
	VkResult error;

	/* The surface over the window. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
	create.display = display;
	create.surface = surface;
	renderer->operation = "vkCreateWaylandSurfaceKHR";
	error = vkCreateWaylandSurfaceKHR(renderer->instance, &create, NULL, &renderer->surface);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the surface. */
	return VK_SUCCESS;
}

/* Chooses a physical device and a queue family that draws and presents to the surface, and makes the device. */
static VkResult
render_device(
	struct sm_renderer *renderer)
{
	VkPhysicalDevice devices[8];
	VkPhysicalDeviceProperties properties;
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
	vkGetPhysicalDeviceMemoryProperties(renderer->physical, &renderer->memory);
	vkGetPhysicalDeviceProperties(renderer->physical, &properties);
	memcpy(renderer->device_name, properties.deviceName, sizeof(renderer->device_name));
	renderer->device_name[sizeof(renderer->device_name) - 1U] = '\0';

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

	/* The device's queue. */
	vkGetDeviceQueue(renderer->device, renderer->family, 0U, &renderer->queue);

	/* Succeeded: the device and its queue. */
	return VK_SUCCESS;
}

/* Makes the swapchain at a size, in an 8-bit UNORM format, presenting in FIFO order. */
static VkResult
render_swapchain(
	struct sm_renderer *renderer,
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

	/* The formats the surface offers. */
	count = 16U;
	renderer->operation = "vkGetPhysicalDeviceSurfaceFormatsKHR";
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->physical, renderer->surface, &count, formats);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The first of those that is 8-bit UNORM. */
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

	/* The size the surface takes. */
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

/* Makes the pass: the swapchain image cleared, drawn and stored for presenting. */
static VkResult
render_pass(
	struct sm_renderer *renderer)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference color;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo create;
	VkResult error;

	/* The swapchain image. */
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
	color.attachment = 0U;
	color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &color;

	/* The acquired image's transition waits for the acquire. */
	memset(&dependency, 0, sizeof(dependency));
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0U;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	/* The pass. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = 1U;
	create.pAttachments = &attachment;
	create.subpassCount = 1U;
	create.pSubpasses = &subpass;
	create.dependencyCount = 1U;
	create.pDependencies = &dependency;
	renderer->operation = "vkCreateRenderPass";
	error = vkCreateRenderPass(renderer->device, &create, NULL, &renderer->pass);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pass. */
	return VK_SUCCESS;
}

/* Makes each swapchain image's view, framebuffer and present semaphore. */
static VkResult
render_targets(
	struct sm_renderer *renderer)
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
	struct sm_renderer *renderer)
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
	struct sm_renderer *renderer)
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

/* Allocates memory that meets the requirements, with the wanted properties when a type has them, and counts it. */
static VkResult
render_memory(
	struct sm_renderer *renderer,
	const VkMemoryRequirements *requirements,
	VkMemoryPropertyFlags wanted,
	VkDeviceMemory *memory)
{
	VkMemoryAllocateInfo allocate;
	uint32_t chosen;
	uint32_t fallback;
	uint32_t index;
	VkResult error;

	/* The first allowed type with the properties, and the first allowed type at all. */
	chosen = UINT32_MAX;
	fallback = UINT32_MAX;
	for (index = 0U; index < renderer->memory.memoryTypeCount; index++) {
		/* A type the resource does not allow is passed. */
		if ((requirements->memoryTypeBits & (1U << index)) == 0U)
			continue;
		if (fallback == UINT32_MAX)
			fallback = index;

		/* The first with the wanted properties is chosen. */
		if ((renderer->memory.memoryTypes[index].propertyFlags & wanted) == wanted) {
			chosen = index;
			break;
		}
	}

	/* Host-visible memory must be host-visible; other wishes fall back to any allowed type. */
	if (chosen == UINT32_MAX && (wanted & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U)
		chosen = fallback;
	renderer->operation = "memory type";
	if (chosen == UINT32_MAX)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The allocation. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements->size;
	allocate.memoryTypeIndex = chosen;
	renderer->operation = "vkAllocateMemory";
	error = vkAllocateMemory(renderer->device, &allocate, NULL, memory);
	if (error != VK_SUCCESS)
		return error;

	/* The memory, counted for the log's report. */
	renderer->allocated_bytes += requirements->size;

	/* Succeeded: the memory is allocated. */
	return VK_SUCCESS;
}

/* Makes the sampler, the descriptor set layout, the pool and the atlas's set. */
static VkResult
render_descriptors(
	struct sm_renderer *renderer)
{
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkResult error;

	/* Nearest sampling: the atlas's pixels land on the window's pixels one to one. */
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

	/* Succeeded: the set is filled when the atlas is made. */
	return VK_SUCCESS;
}

/* Releases the atlas's image. */
static void
render_atlas_free(
	struct sm_renderer *renderer)
{
	/* The view, the image and the memory, where made. */
	if (renderer->atlas_view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, renderer->atlas_view, NULL);
	if (renderer->atlas != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, renderer->atlas, NULL);
	if (renderer->atlas_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->atlas_memory, NULL);
	renderer->atlas_view = VK_NULL_HANDLE;
	renderer->atlas = VK_NULL_HANDLE;
	renderer->atlas_memory = VK_NULL_HANDLE;
}

/* Makes the vertex buffer for at least a number of vertices, mapped for good. */
static VkResult
render_vertices(
	struct sm_renderer *renderer,
	size_t count)
{
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	size_t capacity;
	VkResult error;

	/* The capacity: the first size, doubled until the vertices fit. */
	capacity = RENDER_VERTICES_MIN;
	while (capacity < count)
		capacity *= 2U;

	/* The buffer. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = capacity * SM_VERTEX_FLOATS * sizeof(float);
	buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	renderer->operation = "vkCreateBuffer";
	error = vkCreateBuffer(renderer->device, &buffer, NULL, &renderer->vertices);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, which the host sees. */
	vkGetBufferMemoryRequirements(renderer->device, renderer->vertices, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			      &renderer->vertex_memory);
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

	/* The host writes each frame's vertices here. */
	renderer->vertex_capacity = capacity;

	/* Succeeded: the vertex buffer is ready. */
	return VK_SUCCESS;
}

/* Releases the vertex buffer. */
static void
render_vertices_free(
	struct sm_renderer *renderer)
{
	/* The buffer and its memory (which unmaps it), where made. */
	if (renderer->vertices != VK_NULL_HANDLE)
		vkDestroyBuffer(renderer->device, renderer->vertices, NULL);
	if (renderer->vertex_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->vertex_memory, NULL);
	renderer->vertices = VK_NULL_HANDLE;
	renderer->vertex_memory = VK_NULL_HANDLE;
	renderer->vertex_map = NULL;
	renderer->vertex_capacity = 0;
}

/* Makes the pipeline layout and the two pipelines. */
static VkResult
render_pipelines(
	struct sm_renderer *renderer)
{
	VkShaderModule vertex;
	VkShaderModule shape;
	VkShaderModule glyph;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout;
	VkResult error;

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
	if (error != VK_SUCCESS)
		return error;

	/* The vertex shader. */
	error = render_module(renderer, monitor_draw_vert, sizeof(monitor_draw_vert), &vertex);
	if (error != VK_SUCCESS)
		return error;

	/* The shape shader. */
	error = render_module(renderer, monitor_shape_frag, sizeof(monitor_shape_frag), &shape);
	if (error != VK_SUCCESS) {
		vkDestroyShaderModule(renderer->device, vertex, NULL);
		return error;
	}

	/* The glyph shader. */
	error = render_module(renderer, monitor_glyph_frag, sizeof(monitor_glyph_frag), &glyph);
	if (error != VK_SUCCESS) {
		vkDestroyShaderModule(renderer->device, vertex, NULL);
		vkDestroyShaderModule(renderer->device, shape, NULL);
		return error;
	}

	/* The two pipelines. */
	error = render_pipeline(renderer, SM_PIPE_SHAPE, vertex, shape);
	if (error == VK_SUCCESS)
		error = render_pipeline(renderer, SM_PIPE_GLYPH, vertex, glyph);

	/* The modules are not needed once the pipelines are made. */
	vkDestroyShaderModule(renderer->device, vertex, NULL);
	vkDestroyShaderModule(renderer->device, shape, NULL);
	vkDestroyShaderModule(renderer->device, glyph, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: both pipelines. */
	return VK_SUCCESS;
}

/* Makes one pipeline: four vec4 attributes, triangles, premultiplied-alpha blending, no depth. */
static VkResult
render_pipeline(
	struct sm_renderer *renderer,
	unsigned pipe,
	VkShaderModule vertex,
	VkShaderModule fragment)
{
	static const VkDynamicState dynamic[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription binding;
	VkVertexInputAttributeDescription attributes[4];
	VkPipelineVertexInputStateCreateInfo input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic_state;
	VkGraphicsPipelineCreateInfo pipeline;
	unsigned index;
	VkResult error;

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

	/* One vertex buffer of four vec4s a vertex. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.stride = SM_VERTEX_FLOATS * sizeof(float);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	memset(attributes, 0, sizeof(attributes));
	for (index = 0; index < 4U; index++) {
		attributes[index].location = index;
		attributes[index].binding = 0U;
		attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		attributes[index].offset = index * 4U * sizeof(float);
	}

	/* The vertex input and the triangles. */
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	input.vertexBindingDescriptionCount = 1U;
	input.pVertexBindingDescriptions = &binding;
	input.vertexAttributeDescriptionCount = 4U;
	input.pVertexAttributeDescriptions = attributes;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	/* The viewport and scissor are set each frame; both faces are drawn. */
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

	/*
	 * Premultiplied-alpha blending: the shaders give the colour already
	 * multiplied by its alpha, so the source is added at one.  This is the
	 * only blend the Raspberry Pi 4's driver takes, and it draws the same
	 * pixels as straight alpha did.
	 */
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.blendEnable = VK_TRUE;
	blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
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

	/*
	 * The pipeline.  It names no depth or stencil state: the render pass has
	 * no depth attachment, the scene is drawn back to front, and the
	 * Raspberry Pi 4's driver refuses a pipeline that names one.
	 */
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline.stageCount = 2U;
	pipeline.pStages = stages;
	pipeline.pVertexInputState = &input;
	pipeline.pInputAssemblyState = &assembly;
	pipeline.pViewportState = &viewport;
	pipeline.pRasterizationState = &raster;
	pipeline.pMultisampleState = &multisample;
	pipeline.pDepthStencilState = NULL;
	pipeline.pColorBlendState = &blend;
	pipeline.pDynamicState = &dynamic_state;
	pipeline.layout = renderer->layout;
	pipeline.renderPass = renderer->pass;
	renderer->operation = "vkCreateGraphicsPipelines";
	error = vkCreateGraphicsPipelines(renderer->device, VK_NULL_HANDLE, 1U, &pipeline, NULL, &renderer->pipes[pipe]);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pipeline. */
	return VK_SUCCESS;
}

/* Makes a shader module from SPIR-V words. */
static VkResult
render_module(
	struct sm_renderer *renderer,
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

/* Records the frame: the atlas's first layout change, then the window's cleared pass with every draw in order. */
static void
render_record(
	struct sm_renderer *renderer,
	uint32_t image,
	const struct sm_scene *scene)
{
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkDeviceSize offset;
	const struct sm_draw *draw;
	float size[4];
	unsigned bound;
	size_t index;

	/* One submission of this recording. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	(void)vkBeginCommandBuffer(renderer->command, &begin);

	/* A new atlas image moves from preinitialized (the host's writes kept) to general. */
	if (renderer->atlas != VK_NULL_HANDLE && renderer->atlas_ready == 0) {
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
		renderer->atlas_ready = 1;
	}

	/* The window's pass, cleared to the background. */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = RENDER_CLEAR_RED;
	clear.color.float32[1] = RENDER_CLEAR_GREEN;
	clear.color.float32[2] = RENDER_CLEAR_BLUE;
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = renderer->pass;
	pass.framebuffer = renderer->targets[image].framebuffer;
	pass.renderArea.extent = renderer->extent;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(renderer->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The whole window is the viewport and the scissor, and its size the vertex shader's. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)renderer->extent.width;
	viewport.height = (float)renderer->extent.height;
	viewport.maxDepth = 1.0f;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent = renderer->extent;
	vkCmdSetViewport(renderer->command, 0U, 1U, &viewport);
	vkCmdSetScissor(renderer->command, 0U, 1U, &scissor);
	size[0] = (float)renderer->extent.width;
	size[1] = (float)renderer->extent.height;
	size[2] = 0.0f;
	size[3] = 0.0f;
	vkCmdPushConstants(renderer->command, renderer->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(size), size);

	/* The vertices and the atlas's set. */
	offset = 0U;
	vkCmdBindVertexBuffers(renderer->command, 0U, 1U, &renderer->vertices, &offset);
	vkCmdBindDescriptorSets(renderer->command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->layout, 0U, 1U, &renderer->set, 0U, NULL);

	/* Each draw in order, binding its pipeline when it changes (glyphs only once the atlas is there). */
	bound = SM_PIPES;
	for (index = 0; index < scene->draw_count; index++) {
		draw = &scene->draws[index];
		if (draw->count == 0U)
			continue;
		if (draw->pipe == SM_PIPE_GLYPH && renderer->atlas == VK_NULL_HANDLE)
			continue;

		/* The pipeline. */
		if (draw->pipe != bound) {
			vkCmdBindPipeline(renderer->command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipes[draw->pipe]);
			bound = draw->pipe;
		}

		/* The draw. */
		vkCmdDraw(renderer->command, draw->count, 1U, draw->first, 0U);
	}

	/* The pass ends with the image ready to present. */
	vkCmdEndRenderPass(renderer->command);
}
