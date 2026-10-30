/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Draws the model through standard Vulkan 1.0 and Wayland WSI.
 *
 * Geometry lives in one device-local vertex buffer and one index buffer
 * sorted into per-material groups.  Each texture is a device-local mipmapped
 * image with its own descriptor set.  Six pipelines cover the three alpha
 * modes with and without back-face culling.  All uploads pass through one
 * reused host-visible staging buffer, because the host-visible window of the
 * Venus transport is small, and the buffer is released once the model is
 * uploaded (BUG-144).  The per-pixel shading (--shading=pixel) adds a
 * host-visible uniform buffer with the scene block, binding 1 of every
 * texture's set, rewritten before each frame is recorded.
 */

#include "mview.h"
#include "shaders.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest a GPU wait may take before the frame is reported as failed. */
#define RENDERER_GPU_TIMEOUT	10000000000ULL

/* The smallest staging buffer; larger uploads of buffers are split into pieces. */
#define RENDERER_STAGING_MIN	(1024U * 1024U)

/* The byte stride of one vertex record, matching struct mview_vertex. */
#define RENDERER_VERTEX_STRIDE	32U

/* The byte offset of the normal inside one vertex record. */
#define RENDERER_NORMAL_OFFSET	12U

/* The byte offset of the texture coordinate inside one vertex record. */
#define RENDERER_UV_OFFSET	24U

/* One copy's place in the per-instance buffer: its shift in clip space and in view space (two floats each). */
#define RENDERER_INSTANCE_STRIDE	16U

/* The kinds of render pass: a whole frame, and the first, a middle and the last part of a frame of several submissions. */
#define RENDERER_PASS_WHOLE		0U
#define RENDERER_PASS_FIRST		1U
#define RENDERER_PASS_MIDDLE		2U
#define RENDERER_PASS_LAST		3U

static VkResult renderer_device(struct mview_renderer *renderer);
static VkResult renderer_formats(struct mview_renderer *renderer);
static VkResult renderer_swapchain(struct mview_renderer *renderer, VkSwapchainKHR old);
static VkResult renderer_pass(struct mview_renderer *renderer);
static VkResult renderer_depth(struct mview_renderer *renderer);
static VkResult renderer_targets(struct mview_renderer *renderer);
static void renderer_targets_free(struct mview_renderer *renderer);
static VkResult renderer_commands(struct mview_renderer *renderer);
static VkResult renderer_layout(struct mview_renderer *renderer);
static VkResult renderer_shader(struct mview_renderer *renderer, const uint32_t *words, size_t bytes, VkShaderModule *shader);
static VkResult renderer_pipelines(struct mview_renderer *renderer);
static VkResult renderer_pipeline(struct mview_renderer *renderer, enum mview_alpha alpha, enum mview_cull cull, VkPipeline *pipeline);
static VkResult renderer_memory(struct mview_renderer *renderer, const VkMemoryRequirements *requirements, VkMemoryPropertyFlags required, VkMemoryPropertyFlags avoided, VkDeviceMemory *memory);
static VkResult renderer_image(struct mview_renderer *renderer, struct mview_image *image, VkFormat format, uint32_t width, uint32_t height, uint32_t levels, VkImageUsageFlags usage, VkImageAspectFlags aspect);
static VkResult renderer_buffer(struct mview_renderer *renderer, struct mview_buffer *buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, int host);
static VkResult renderer_begin(struct mview_renderer *renderer);
static uint64_t renderer_cycles(void);
static void renderer_stage(struct mview_renderer *renderer, enum mview_stage stage, uint64_t *mark);
static VkResult renderer_finish(struct mview_renderer *renderer);
static VkResult renderer_upload_buffer(struct mview_renderer *renderer, struct mview_buffer *buffer, const void *data, VkDeviceSize bytes, VkBufferUsageFlags usage);
static VkResult renderer_upload_texture(struct mview_renderer *renderer, struct mview_image *image, const uint8_t *pixels, uint32_t width, uint32_t height);
static VkResult renderer_descriptors(struct mview_renderer *renderer);
static void renderer_barrier(VkCommandBuffer command, VkImage image, uint32_t level, uint32_t count, VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags source_access, VkAccessFlags destination_access, VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage);
static void renderer_record(struct mview_renderer *renderer, const struct mview_model *model, const struct mview_camera *camera, uint32_t image, uint32_t part);
static VkResult renderer_pass_kind(struct mview_renderer *renderer, uint32_t kind, VkRenderPass *pass);
static void renderer_instances(struct mview_renderer *renderer, const struct mview_camera *view, uint32_t columns, uint32_t rows, float spacing);
static uint32_t renderer_levels(uint32_t width, uint32_t height);
static VkDeviceSize renderer_chain_bytes(uint32_t width, uint32_t height, uint32_t levels);
static void renderer_downsample(const uint8_t *source, uint32_t width, uint32_t height, uint8_t *destination);
static void renderer_image_free(struct mview_renderer *renderer, struct mview_image *image);
static void renderer_buffer_free(struct mview_renderer *renderer, struct mview_buffer *buffer);

/*
 * Creates the device, swapchain, pass, depth image, pipelines and commands.
 *
 * Model resources are created separately by mview_renderer_load.
 */
VkResult
mview_renderer_open(
	struct mview_renderer *renderer,
	struct mview_window *window,
	int pixel_shading,
	VkPresentModeKHR present_mode,
	uint32_t copies,
	int each_draw,
	uint32_t submits,
	uint32_t inflight,
	int cull_all,
	uint32_t images)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	VkWaylandSurfaceCreateInfoKHR surface;
	const char *extensions[2];
	VkResult error;

	/* Initializes renderer ownership, the configured native image extent and the shading. */
	memset(renderer, 0, sizeof(*renderer));
	renderer->extent.width = window->width;
	renderer->extent.height = window->height;
	renderer->pixel_shading = pixel_shading;
	renderer->wanted_present = present_mode;
	renderer->copies = copies;
	renderer->each_draw = each_draw;
	renderer->submits = submits;
	renderer->inflight = inflight;
	renderer->slot = 0U;
	renderer->cull_all = cull_all;
	renderer->images = images;

	/* The application requests only standard instance extensions. */
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;

	/* Declares the application and its standard Vulkan version. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "mview";
	application.apiVersion = VK_API_VERSION_1_0;

	/* Requests the standard surface and Wayland instance extensions. */
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = 2U;
	instance.ppEnabledExtensionNames = extensions;
	renderer->operation = "vkCreateInstance";
	error = vkCreateInstance(&instance, NULL, &renderer->instance);
	if (error != VK_SUCCESS)
		return error;

	/* This is the only native-window description supplied to Vulkan. */
	memset(&surface, 0, sizeof(surface));
	surface.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
	surface.display = window->display;
	surface.surface = window->surface;
	renderer->operation = "vkCreateWaylandSurfaceKHR";
	error = vkCreateWaylandSurfaceKHR(renderer->instance, &surface, NULL, &renderer->surface);
	if (error != VK_SUCCESS)
		return error;

	/* Selects a graphics queue with presentation support for this surface. */
	error = renderer_device(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Creates the first chain using the native window configuration. */
	error = renderer_swapchain(renderer, VK_NULL_HANDLE);
	if (error != VK_SUCCESS)
		return error;

	/* Chooses depth and texture formats the device supports. */
	error = renderer_formats(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Creates the colour-and-depth pass every frame uses. */
	error = renderer_pass(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Creates the depth image shared by every swapchain image. */
	error = renderer_depth(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Builds application attachments for every borrowed swapchain image. */
	error = renderer_targets(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Creates reusable command and synchronization owners. */
	error = renderer_commands(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Creates the sampler, descriptor layout and pipeline layout. */
	error = renderer_layout(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Creates the six material pipelines. */
	error = renderer_pipelines(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the renderer can draw once the model is loaded. */
	return VK_SUCCESS;
}

/*
 * Uploads the model's geometry and textures to device-local memory.
 *
 * Every upload goes through one staging buffer that is sized for the largest
 * single texture and reused, waiting for each copy before the next.
 */
VkResult
mview_renderer_load(
	struct mview_renderer *renderer,
	const struct mview_model *model)
{
	static const uint8_t white[4] = { 255U, 255U, 255U, 255U };
	VkDeviceSize staging;
	VkDeviceSize needed;
	uint32_t index;
	uint32_t levels;
	VkResult error;

	/* A texture larger than the device allows cannot be created. */
	for (index = 0U; index < model->texture_count; index++) {
		if (model->textures[index].width > renderer->max_texture_side ||
		    model->textures[index].height > renderer->max_texture_side) {
			renderer->operation = "maxImageDimension2D";
			return VK_ERROR_FORMAT_NOT_SUPPORTED;
		}
	}

	/* The staging buffer holds the largest single texture upload. */
	staging = RENDERER_STAGING_MIN;
	for (index = 0U; index < model->texture_count; index++) {
		/* A GPU mip chain uploads level 0 only; a CPU chain uploads every level. */
		levels = 1U;
		if (renderer->blit_mipmaps == 0)
			levels = renderer_levels(model->textures[index].width, model->textures[index].height);

		/* The largest upload decides the buffer's size. */
		needed = renderer_chain_bytes(model->textures[index].width, model->textures[index].height, levels);
		if (needed > staging)
			staging = needed;
	}

	/* Creates the one mapped staging buffer. */
	error = renderer_buffer(renderer, &renderer->staging, staging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1);
	if (error != VK_SUCCESS)
		return error;

	/* Uploads every vertex of every mesh. */
	error = renderer_upload_buffer(
		renderer,
		&renderer->vertices,
		model->vertices,
		(VkDeviceSize)model->vertex_count * sizeof(struct mview_vertex),
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	if (error != VK_SUCCESS)
		return error;

	/* Uploads the group-sorted triangle indices. */
	error = renderer_upload_buffer(
		renderer,
		&renderer->indices,
		model->indices,
		(VkDeviceSize)model->triangle_count * 3U * sizeof(uint32_t),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	if (error != VK_SUCCESS)
		return error;

	/* One image per model texture, plus a white one for untextured materials. */
	renderer->textures = calloc(model->texture_count + 1U, sizeof(*renderer->textures));
	if (renderer->textures == NULL) {
		renderer->operation = "calloc";
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	renderer->texture_count = model->texture_count + 1U;

	/* Uploads and mipmaps each model texture. */
	for (index = 0U; index < model->texture_count; index++) {
		error = renderer_upload_texture(
			renderer,
			&renderer->textures[index],
			model->textures[index].pixels,
			model->textures[index].width,
			model->textures[index].height);
		if (error != VK_SUCCESS)
			return error;
	}

	/* The last image is one white texel, so an untextured material samples its colour alone. */
	error = renderer_upload_texture(renderer, &renderer->textures[model->texture_count], white, 1U, 1U);
	if (error != VK_SUCCESS)
		return error;

	/* Binds each image to its own descriptor set. */
	error = renderer_descriptors(renderer);
	if (error != VK_SUCCESS)
		return error;

	/*
	 * Every upload has finished (each waits for its submission), so the
	 * staging buffer goes now: it is host-visible, the scarce kind of
	 * memory on Venus (BUG-144, the 4 to 5 MiB it held for the viewer's
	 * whole life counted against the guest's 256 MiB window).
	 */
	renderer_buffer_free(renderer, &renderer->staging);

	/* Succeeded: the model is resident on the device. */
	return VK_SUCCESS;
}

/*
 * Draws and presents one frame of the model from the camera.
 *
 * VK_ERROR_OUT_OF_DATE_KHR means nothing was presented and the swapchain must
 * be recreated; VK_SUBOPTIMAL_KHR means the frame was presented but the
 * swapchain should be recreated before the next one.
 */
VkResult
mview_renderer_draw(
	struct mview_renderer *renderer,
	const struct mview_model *model,
	const struct mview_camera *camera)
{
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkPipelineStageFlags stage;
	VkResult acquired;
	VkResult presented;
	VkResult error;
	VkFence fence;
	uint64_t mark;
	uint32_t image;
	uint32_t part;

	/*
	 * With frames in flight, the next slot's previous frame must finish
	 * before its command buffers and fence are used again (the wait counts
	 * as the fence stage).
	 */
	mark = renderer_cycles();
	if (renderer->inflight > 1U) {
		renderer->slot = (renderer->slot + 1U) % renderer->inflight;
		renderer->fence = renderer->slot_fences[renderer->slot];
		renderer->acquired = renderer->slot_acquired[renderer->slot];
		renderer->operation = "vkWaitForFences";
		error = vkWaitForFences(renderer->device, 1U, &renderer->fence, VK_TRUE, RENDERER_GPU_TIMEOUT);
		if (error != VK_SUCCESS)
			return error;
		renderer_stage(renderer, MVIEW_STAGE_FENCE, &mark);
	}

	/* The acquire semaphore is signaled only after the compositor releases an image. */
	renderer->operation = "vkAcquireNextImageKHR";
	acquired = vkAcquireNextImageKHR(renderer->device, renderer->swapchain, RENDERER_GPU_TIMEOUT, renderer->acquired, VK_NULL_HANDLE, &image);
	if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
		return acquired;
	renderer_stage(renderer, MVIEW_STAGE_ACQUIRE, &mark);

	/* The fence of the previous frame has been waited on and can be reused. */
	renderer->operation = "vkResetFences";
	error = vkResetFences(renderer->device, 1U, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/*
	 * Records and submits each part of the frame in order (one part unless
	 * --submits asks for more): the first waits for the acquired image,
	 * the last signals the image's present semaphore and the fence.
	 */
	for (part = 0U; part < renderer->submits; part++) {
		/* Retires the part's prior recording before describing the next one. */
		renderer->command = renderer->commands[renderer->slot * renderer->submits + part];
		error = renderer_begin(renderer);
		if (error != VK_SUCCESS)
			return error;

		/* Records the pass that draws the part's copies of every material group. */
		renderer_record(renderer, model, camera, image, part);

		/* Finishes the render stream before it can be submitted. */
		renderer->operation = "vkEndCommandBuffer";
		error = vkEndCommandBuffer(renderer->command);
		if (error != VK_SUCCESS)
			return error;
		renderer_stage(renderer, MVIEW_STAGE_RECORD, &mark);

		/* Acquire must finish before the colour attachment is first written. */
		stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		memset(&submit, 0, sizeof(submit));
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		if (part == 0U) {
			submit.waitSemaphoreCount = 1U;
			submit.pWaitSemaphores = &renderer->acquired;
			submit.pWaitDstStageMask = &stage;
		}
		submit.commandBufferCount = 1U;
		submit.pCommandBuffers = &renderer->command;
		fence = VK_NULL_HANDLE;
		if (part + 1U == renderer->submits) {
			submit.signalSemaphoreCount = 1U;
			submit.pSignalSemaphores = &renderer->targets[image].rendered;
			fence = renderer->fence;
		}

		/* Submits the part; the queue runs the parts in order. */
		renderer->operation = "vkQueueSubmit";
		error = vkQueueSubmit(renderer->queue, 1U, &submit, fence);
		if (error != VK_SUCCESS)
			return error;
		renderer_stage(renderer, MVIEW_STAGE_SUBMIT, &mark);
	}
	renderer->command = renderer->commands[renderer->slot * renderer->submits];

	/* Transfers the completed acquired image to the surface. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &renderer->targets[image].rendered;
	present.swapchainCount = 1U;
	present.pSwapchains = &renderer->swapchain;
	present.pImageIndices = &image;
	renderer->operation = "vkQueuePresentKHR";
	presented = vkQueuePresentKHR(renderer->queue, &present);
	renderer_stage(renderer, MVIEW_STAGE_PRESENT, &mark);

	/* Command-buffer reuse is ordered by the rendering fence whatever the present did (with one frame in flight, here). */
	if (renderer->inflight <= 1U) {
		renderer->operation = "vkWaitForFences";
		error = vkWaitForFences(renderer->device, 1U, &renderer->fence, VK_TRUE, RENDERER_GPU_TIMEOUT);
		if (error != VK_SUCCESS)
			return error;
		renderer_stage(renderer, MVIEW_STAGE_FENCE, &mark);
	}
	renderer->stage_frames++;

	/* A lost or failed present is reported after the fence is idle. */
	renderer->operation = "vkQueuePresentKHR";
	if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR)
		return presented;

	/* A presented frame on a mismatched chain asks the caller to recreate it. */
	if (presented == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR)
		return VK_SUBOPTIMAL_KHR;

	/* Succeeded: the frame is presented and the command buffer is idle. */
	return VK_SUCCESS;
}

/*
 * Replaces the swapchain, depth image and framebuffers for a new extent.
 */
VkResult
mview_renderer_recreate(
	struct mview_renderer *renderer,
	uint32_t width,
	uint32_t height)
{
	VkSwapchainKHR old;
	VkResult error;

	/* All application references to old image views retire before their swapchain. */
	renderer->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(renderer->device);
	if (error != VK_SUCCESS)
		return error;

	/* Retires attachments and the depth image sized for the old extent. */
	old = renderer->swapchain;
	renderer->swapchain = VK_NULL_HANDLE;
	renderer_targets_free(renderer);
	renderer_image_free(renderer, &renderer->depth);
	renderer->extent.width = width;
	renderer->extent.height = height;

	/* Consumes the retired chain on either creation outcome without losing that outcome. */
	error = renderer_swapchain(renderer, old);
	if (error != VK_SUCCESS) {
		vkDestroySwapchainKHR(renderer->device, old, NULL);
		return error;
	}

	/* The replacement owns its images, allowing the retired chain to release its storage. */
	vkDestroySwapchainKHR(renderer->device, old, NULL);

	/* Creates the depth image at the new extent. */
	error = renderer_depth(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Builds application attachments for every borrowed swapchain image. */
	error = renderer_targets(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the renderer draws at the new extent. */
	return VK_SUCCESS;
}

/*
 * Consumes every application owner in dependency order even after partial initialization.
 */
VkResult
mview_renderer_close(
	struct mview_renderer *renderer)
{
	uint32_t index;
	VkResult error;

	/* Preserve a real completion failure while still releasing process-local owners. */
	error = VK_SUCCESS;
	if (renderer->device != VK_NULL_HANDLE) {
		/* Retains a completion error while teardown still consumes every local owner. */
		error = vkDeviceWaitIdle(renderer->device);

		/* Pipelines reference the layout and shader modules. */
		for (index = 0U; index < MVIEW_PIPELINE_COUNT; index++) {
			if (renderer->pipelines[index] != VK_NULL_HANDLE)
				vkDestroyPipeline(renderer->device, renderer->pipelines[index], NULL);
		}

		/* The shader modules are only needed while pipelines are created. */
		if (renderer->vertex_shader != VK_NULL_HANDLE)
			vkDestroyShaderModule(renderer->device, renderer->vertex_shader, NULL);

		/* See the vertex shader. */
		if (renderer->fragment_shader != VK_NULL_HANDLE)
			vkDestroyShaderModule(renderer->device, renderer->fragment_shader, NULL);

		/* See the vertex shader. */
		if (renderer->cutout_shader != VK_NULL_HANDLE)
			vkDestroyShaderModule(renderer->device, renderer->cutout_shader, NULL);

		/* The pool owns every descriptor set. */
		if (renderer->descriptor_pool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(renderer->device, renderer->descriptor_pool, NULL);

		/* The set handles died with their pool. */
		free(renderer->sets);
		renderer->sets = NULL;

		/* No pipeline uses the layout any more. */
		if (renderer->layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(renderer->device, renderer->layout, NULL);

		/* No layout or set refers to the set layout any more. */
		if (renderer->set_layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(renderer->device, renderer->set_layout, NULL);

		/* No descriptor refers to the sampler any more. */
		if (renderer->sampler != VK_NULL_HANDLE)
			vkDestroySampler(renderer->device, renderer->sampler, NULL);

		/* Textures, including a partially created table. */
		if (renderer->textures != NULL) {
			for (index = 0U; index < renderer->texture_count; index++)
				renderer_image_free(renderer, &renderer->textures[index]);
		}

		free(renderer->textures);
		renderer->textures = NULL;

		/* Geometry, staging and scene buffers. */
		renderer_buffer_free(renderer, &renderer->vertices);
		renderer_buffer_free(renderer, &renderer->indices);
		renderer_buffer_free(renderer, &renderer->staging);
		renderer_buffer_free(renderer, &renderer->scene);
		renderer_buffer_free(renderer, &renderer->instances);

		/* Releases attachments before the objects referenced by their recorded rendering. */
		renderer_targets_free(renderer);
		renderer_image_free(renderer, &renderer->depth);

		/* The pool owns the application's sole command buffer. */
		if (renderer->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(renderer->device, renderer->pool, NULL);

		/* The pass no longer has live application framebuffers. */
		if (renderer->pass != VK_NULL_HANDLE)
			vkDestroyRenderPass(renderer->device, renderer->pass, NULL);
		for (index = 0U; index < 3U; index++) {
			if (renderer->part_passes[index] != VK_NULL_HANDLE)
				vkDestroyRenderPass(renderer->device, renderer->part_passes[index], NULL);
		}
		free(renderer->commands);

		/* The acquire semaphores have no future render submission to serve; the final completion wait consumed the fences' purpose. */
		for (index = 0U; index < MVIEW_INFLIGHT_MAX; index++) {
			if (renderer->slot_acquired[index] != VK_NULL_HANDLE)
				vkDestroySemaphore(renderer->device, renderer->slot_acquired[index], NULL);
			if (renderer->slot_fences[index] != VK_NULL_HANDLE)
				vkDestroyFence(renderer->device, renderer->slot_fences[index], NULL);
		}

		/* Drops the protocol image owners before retiring their device context. */
		if (renderer->swapchain != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(renderer->device, renderer->swapchain, NULL);

		/* No child object retains this application device. */
		vkDestroyDevice(renderer->device, NULL);
	}

	/* The instance surface only borrows the application's native Wayland objects. */
	if (renderer->surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(renderer->instance, renderer->surface, NULL);

	/* Releases the instance after its only surface and device have retired. */
	if (renderer->instance != VK_NULL_HANDLE)
		vkDestroyInstance(renderer->instance, NULL);

	/* Clears stale application identities after consuming every reachable owner. */
	memset(renderer, 0, sizeof(*renderer));
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: no application GPU or native-surface owner remains. */
	return VK_SUCCESS;
}

/* Chooses a real physical device and queue supporting graphics and this surface. */
static VkResult
renderer_device(
	struct mview_renderer *renderer)
{
	VkPhysicalDevice *devices;
	VkQueueFamilyProperties *families;
	VkPhysicalDeviceProperties properties;
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

	/* Enumeration storage is local and never mistaken for driver-owned identities. */
	renderer->operation = "vkEnumeratePhysicalDevices";
	count = 0U;
	error = vkEnumeratePhysicalDevices(renderer->instance, &count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* No physical device can provide the requested renderer. */
	if (count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Allocates temporary storage for the finite physical-device inventory. */
	devices = calloc(count, sizeof(*devices));
	if (devices == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Copies physical identities while retaining no ownership over those handles. */
	error = vkEnumeratePhysicalDevices(renderer->instance, &count, devices);
	if (error != VK_SUCCESS) {
		free(devices);
		return error;
	}

	/* Searches each physical device until one exposes a usable graphics/present queue. */
	for (index = 0U; index < count; index++) {
		/* Enumerates this device's queue families before allocating their descriptions. */
		family_count = 0U;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, NULL);
		if (family_count == 0U)
			continue;

		/* Holds this device's queue descriptions only for the current search iteration. */
		families = calloc(family_count, sizeof(*families));
		if (families == NULL) {
			free(devices);
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		/* Obtains the actual queue flags and counts advertised by the device. */
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, families);

		/* Accepts only queues with actual graphics capacity and native surface support. */
		for (family = 0U; family < family_count; family++) {
			/* A graphics queue with no queue instances cannot serve this renderer. */
			if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U || families[family].queueCount == 0U)
				continue;

			/* Queries support for this exact native surface before choosing its queue. */
			supported = VK_FALSE;
			error = vkGetPhysicalDeviceSurfaceSupportKHR(devices[index], family, renderer->surface, &supported);
			if (error == VK_SUCCESS && supported != VK_FALSE) {
				renderer->physical = devices[index];
				renderer->family = family;
				break;
			}
		}

		/* A selected physical handle survives disposal of its temporary descriptions. */
		free(families);
		if (renderer->physical != VK_NULL_HANDLE)
			break;
	}

	/* Enumeration storage is no longer needed after selecting or exhausting the inventory. */
	free(devices);
	if (renderer->physical == VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Memory types and image limits guide every later allocation. */
	vkGetPhysicalDeviceMemoryProperties(renderer->physical, &renderer->memory);
	vkGetPhysicalDeviceProperties(renderer->physical, &properties);
	renderer->max_texture_side = properties.limits.maxImageDimension2D;

	/* The standard type table has a fixed capacity that a device may not exceed. */
	if (renderer->memory.memoryTypeCount > VK_MAX_MEMORY_TYPES)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requests exactly one queue from the selected graphics/presentation family. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = renderer->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;

	/* Enables the standard device swapchain extension used by the application. */
	extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

	/* Describes the device with its one requested queue and standard swapchain extension. */
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

	/* Obtains the queue created for this graphics and surface pair. */
	vkGetDeviceQueue(renderer->device, renderer->family, 0U, &renderer->queue);

	/* Succeeded: the renderer has one graphics queue for this surface. */
	return VK_SUCCESS;
}

/*
 * Chooses the depth format and decides how texture mipmaps are made.
 *
 * Textures use the swapchain's encoding: an sRGB swapchain samples sRGB
 * textures, a UNORM swapchain samples the same bytes as UNORM so the texture
 * colours reach the screen unchanged.
 */
static VkResult
renderer_formats(
	struct mview_renderer *renderer)
{
	static const VkFormat depth_formats[3] = {
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D32_SFLOAT_S8_UINT
	};
	VkFormatProperties properties;
	VkFormatFeatureFlags blit;
	uint32_t index;

	/* The first depth format usable as an optimal-tiled attachment is chosen. */
	renderer->depth_format = VK_FORMAT_UNDEFINED;
	for (index = 0U; index < 3U; index++) {
		vkGetPhysicalDeviceFormatProperties(renderer->physical, depth_formats[index], &properties);
		if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U) {
			renderer->depth_format = depth_formats[index];
			break;
		}
	}

	/* Without a depth attachment the model cannot be drawn in order. */
	renderer->operation = "depth format";
	if (renderer->depth_format == VK_FORMAT_UNDEFINED)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The texture format must be sampleable. */
	vkGetPhysicalDeviceFormatProperties(renderer->physical, VK_FORMAT_R8G8B8A8_UNORM, &properties);
	renderer->operation = "texture format";
	if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Linear sampling is used where the format allows it, nearest otherwise. */
	renderer->linear_filter = 0;
	if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0U)
		renderer->linear_filter = 1;

	/* Mipmaps are blitted on the GPU only when linear blits of the format are supported. */
	blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	renderer->blit_mipmaps = 0;
	if ((properties.optimalTilingFeatures & blit) == blit)
		renderer->blit_mipmaps = 1;

	/* Succeeded: depth and texture formats are chosen. */
	return VK_SUCCESS;
}

/* Creates a swapchain solely from surface capabilities and advertised formats. */
static VkResult
renderer_swapchain(
	struct mview_renderer *renderer,
	VkSwapchainKHR old)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR *formats;
	VkSwapchainCreateInfoKHR create;
	VkPresentModeKHR *modes;
	uint32_t count;
	uint32_t index;
	VkResult error;

	/* The surface's formats first. */
	renderer->operation = "vkGetPhysicalDeviceSurfaceFormatsKHR";
	count = 0U;
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->physical, renderer->surface, &count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Holds the finite format and colour-space inventory for this surface. */
	formats = calloc(count, sizeof(*formats));
	if (formats == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Failed enumeration leaves no format entries safe to inspect. */
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->physical, renderer->surface, &count, formats);
	if (error != VK_SUCCESS) {
		free(formats);
		return error;
	}

	/*
	 * Selects a BGRA UNORM format in the standard sRGB colour space, the
	 * displays' own (a compositor can show it straight), or else RGBA.
	 */
	renderer->format = VK_FORMAT_UNDEFINED;
	for (index = 0U; index < count; index++) {
		if (formats[index].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			continue;
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM) {
			renderer->format = formats[index].format;
			break;
		}
		if (formats[index].format == VK_FORMAT_R8G8B8A8_UNORM && renderer->format == VK_FORMAT_UNDEFINED)
			renderer->format = formats[index].format;
	}

	/* Retains the chosen format while releasing its enumeration storage. */
	free(formats);

	/* The viewer's colour handling assumes an 8-bit UNORM target. */
	if (renderer->format == VK_FORMAT_UNDEFINED)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* FIFO is the one presentation mode every Vulkan surface supports; another is used when the surface offers it. */
	renderer->present_mode = VK_PRESENT_MODE_FIFO_KHR;
	renderer->operation = "vkGetPhysicalDeviceSurfacePresentModesKHR";
	count = 0U;
	error = vkGetPhysicalDeviceSurfacePresentModesKHR(renderer->physical, renderer->surface, &count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Holds the surface's presentation modes. */
	modes = calloc(count + 1U, sizeof(*modes));
	if (modes == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Failed enumeration leaves no mode safe to inspect. */
	error = vkGetPhysicalDeviceSurfacePresentModesKHR(renderer->physical, renderer->surface, &count, modes);
	if (error != VK_SUCCESS) {
		free(modes);
		return error;
	}

	/* Takes the asked mode when it is among them. */
	for (index = 0U; index < count; index++) {
		if (modes[index] == renderer->wanted_present) {
			renderer->present_mode = renderer->wanted_present;
			break;
		}
	}

	/* The chosen mode is kept; the list goes. */
	free(modes);

	/* Obtains the native extent and object-count limits before creation. */
	renderer->operation = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR";
	error = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(renderer->physical, renderer->surface, &capabilities);
	if (error != VK_SUCCESS)
		return error;

	/* The viewer does not create a differently sized native window behind the compositor's back. */
	if (renderer->extent.width < 16U ||
	    renderer->extent.height < 16U ||
	    renderer->extent.width < capabilities.minImageExtent.width ||
	    renderer->extent.height < capabilities.minImageExtent.height ||
	    renderer->extent.width > capabilities.maxImageExtent.width ||
	    renderer->extent.height > capabilities.maxImageExtent.height)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires the attachment usage and opacity actually used by the viewer. */
	if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0U ||
	    (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0U)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Starts the native chain description with the required minimum image count. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.surface = renderer->surface;
	create.minImageCount = capabilities.minImageCount;

	/* Prefers triple buffering within the advertised native image-count range. */
	if (create.minImageCount < 3U)
		create.minImageCount = 3U;

	/* An image count asked for (--images) replaces it, within the advertised range. */
	if (renderer->images != 0U) {
		create.minImageCount = renderer->images;
		if (create.minImageCount < capabilities.minImageCount)
			create.minImageCount = capabilities.minImageCount;
	}

	/* A nonzero maximum also constrains implementations that permit only two images. */
	if (capabilities.maxImageCount != 0U && create.minImageCount > capabilities.maxImageCount)
		create.minImageCount = capabilities.maxImageCount;

	/* Presents the configured window using its native transform and opaque colour attachment. */
	create.imageFormat = renderer->format;
	create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	create.imageExtent = renderer->extent;
	create.imageArrayLayers = 1U;
	create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform = capabilities.currentTransform;
	create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	create.presentMode = renderer->present_mode;
	create.clipped = VK_TRUE;
	create.oldSwapchain = old;

	/* Creation alone publishes the new owner; a failed call leaves no replacement handle. */
	renderer->swapchain = VK_NULL_HANDLE;
	renderer->operation = "vkCreateSwapchainKHR";
	error = vkCreateSwapchainKHR(renderer->device, &create, NULL, &renderer->swapchain);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the presentation chain belongs to the renderer. */
	return VK_SUCCESS;
}

/*
 * Creates the pass that draws a frame whole, and with more than one
 * submission a frame the passes of its parts: the first, the middle ones
 * and the last.
 */
static VkResult
renderer_pass(
	struct mview_renderer *renderer)
{
	uint32_t kind;
	VkResult error;

	/* The whole frame's pass: clear, draw, present. */
	error = renderer_pass_kind(renderer, RENDERER_PASS_WHOLE, &renderer->pass);
	if (error != VK_SUCCESS)
		return error;

	/* A frame of one submission needs no other. */
	if (renderer->submits <= 1U)
		return VK_SUCCESS;

	/* The parts' passes. */
	for (kind = RENDERER_PASS_FIRST; kind <= RENDERER_PASS_LAST; kind++) {
		error = renderer_pass_kind(renderer, kind, &renderer->part_passes[kind - RENDERER_PASS_FIRST]);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: every pass a frame uses exists. */
	return VK_SUCCESS;
}

/*
 * Creates one pass of a kind: the whole frame (clear colour and depth,
 * keep colour for presentation), the first part (clear, keep both for the
 * next part), a middle part (load and keep both) or the last part (load,
 * keep colour for presentation).  Passes of every kind are compatible, so
 * the framebuffers serve them all.
 */
static VkResult
renderer_pass_kind(
	struct mview_renderer *renderer,
	uint32_t kind,
	VkRenderPass *pass)
{
	VkAttachmentDescription attachments[2];
	VkAttachmentReference color;
	VkAttachmentReference depth;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo create;
	VkResult error;

	/* The swapchain image is cleared, drawn, and left ready to present. */
	memset(attachments, 0, sizeof(attachments));
	attachments[0].format = renderer->format;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	/* Depth is cleared every frame and never read afterwards. */
	attachments[1].format = renderer->depth_format;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	/* A part after the first loads what the part before kept. */
	if (kind == RENDERER_PASS_MIDDLE || kind == RENDERER_PASS_LAST) {
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	}

	/* A part before the last keeps both attachments for the next. */
	if (kind == RENDERER_PASS_FIRST || kind == RENDERER_PASS_MIDDLE) {
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	}

	/* The subpass writes both attachments. */
	color.attachment = 0U;
	color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	depth.attachment = 1U;
	depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &color;
	subpass.pDepthStencilAttachment = &depth;

	/*
	 * The acquired image's layout transition waits for the acquire semaphore's
	 * stage, and the shared depth image is not cleared while the previous
	 * frame still tests against it.
	 */
	memset(&dependency, 0, sizeof(dependency));
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0U;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	/* Publishes both attachments and the single subpass as one render pass. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = 2U;
	create.pAttachments = attachments;
	create.subpassCount = 1U;
	create.pSubpasses = &subpass;
	create.dependencyCount = 1U;
	create.pDependencies = &dependency;
	renderer->operation = "vkCreateRenderPass";
	error = vkCreateRenderPass(renderer->device, &create, NULL, pass);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the render pass of the kind exists. */
	return VK_SUCCESS;
}

/* Creates the depth image at the current extent. */
static VkResult
renderer_depth(
	struct mview_renderer *renderer)
{
	VkImageAspectFlags aspect;
	VkResult error;

	/* A combined depth-stencil attachment is viewed with both of its aspects. */
	aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (renderer->depth_format != VK_FORMAT_D32_SFLOAT)
		aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

	/* One depth image serves every swapchain image because frames never overlap. */
	error = renderer_image(
		renderer,
		&renderer->depth,
		renderer->depth_format,
		renderer->extent.width,
		renderer->extent.height,
		1U,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		aspect);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the depth attachment exists at the current extent. */
	return VK_SUCCESS;
}

/* Wraps each swapchain image in image-view and framebuffer objects. */
static VkResult
renderer_targets(
	struct mview_renderer *renderer)
{
	VkImage *images;
	VkImageView attachments[2];
	VkImageViewCreateInfo view;
	VkSemaphoreCreateInfo semaphore;
	VkFramebufferCreateInfo framebuffer;
	uint32_t index;
	VkResult error;

	/* Partial target acquisition remains reachable through the renderer for cleanup. */
	renderer->operation = "vkGetSwapchainImagesKHR";
	renderer->count = 0U;
	error = vkGetSwapchainImagesKHR(renderer->device, renderer->swapchain, &renderer->count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* An empty chain has no image that the renderer can acquire. */
	if (renderer->count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Holds only the borrowed image identities returned by enumeration. */
	images = calloc(renderer->count, sizeof(*images));
	if (images == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Publishes a zeroed attachment table so partial creation remains reclaimable. */
	renderer->targets = calloc(renderer->count, sizeof(*renderer->targets));
	if (renderer->targets == NULL) {
		free(images);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Copies the borrowed image inventory before creating any dependent views. */
	error = vkGetSwapchainImagesKHR(renderer->device, renderer->swapchain, &renderer->count, images);
	if (error != VK_SUCCESS) {
		free(images);
		return error;
	}

	/* Constructs independently reclaimable attachment owners for each borrowed image. */
	for (index = 0U; index < renderer->count; index++) {
		/* Each view refers to the full colour subresource of its borrowed image. */
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
			break;

		/* Binds this image view and the shared depth view as the framebuffer. */
		attachments[0] = renderer->targets[index].view;
		attachments[1] = renderer->depth.view;
		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer.renderPass = renderer->pass;
		framebuffer.attachmentCount = 2U;
		framebuffer.pAttachments = attachments;
		framebuffer.width = renderer->extent.width;
		framebuffer.height = renderer->extent.height;
		framebuffer.layers = 1U;
		renderer->operation = "vkCreateFramebuffer";
		error = vkCreateFramebuffer(renderer->device, &framebuffer, NULL, &renderer->targets[index].framebuffer);
		if (error != VK_SUCCESS)
			break;

		/* Reacquiring this image proves its previous present wait consumed this signal. */
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		renderer->operation = "vkCreateSemaphore";
		error = vkCreateSemaphore(renderer->device, &semaphore, NULL, &renderer->targets[index].rendered);
		if (error != VK_SUCCESS)
			break;
	}

	/* Releases enumeration storage independently of partially created attachments. */
	free(images);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: every swapchain image has its own view, framebuffer and semaphore. */
	return VK_SUCCESS;
}

/* Releases app-created attachments without destroying borrowed swapchain images. */
static void
renderer_targets_free(
	struct mview_renderer *renderer)
{
	uint32_t index;

	/* Both arrays can be partially populated after a failed view or framebuffer creation. */
	if (renderer->targets != NULL) {
		/* Consumes only the owners that completed creation in each table entry. */
		for (index = 0U; index < renderer->count; index++) {
			/* The semaphore belongs to this image's presentation cycle. */
			if (renderer->targets[index].rendered != VK_NULL_HANDLE)
				vkDestroySemaphore(renderer->device, renderer->targets[index].rendered, NULL);

			/* The framebuffer must retire before its image view. */
			if (renderer->targets[index].framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(renderer->device, renderer->targets[index].framebuffer, NULL);

			/* Dropping a view leaves the borrowed swapchain image itself intact. */
			if (renderer->targets[index].view != VK_NULL_HANDLE)
				vkDestroyImageView(renderer->device, renderer->targets[index].view, NULL);
		}
	}

	/* Clears the attachment collection so later cleanup cannot reuse stale identities. */
	free(renderer->targets);
	renderer->targets = NULL;
	renderer->count = 0U;

	/* Succeeded: all application image attachments have retired. */
	return;
}

/* Allocates reusable command and synchronization objects with explicit creation checks. */
static VkResult
renderer_commands(
	struct mview_renderer *renderer)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	uint32_t index;
	VkResult error;

	/* The pool's family is the same graphics/presentation family selected for the device. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = renderer->family;
	renderer->operation = "vkCreateCommandPool";
	error = vkCreateCommandPool(renderer->device, &pool, NULL, &renderer->pool);
	if (error != VK_SUCCESS)
		return error;

	/*
	 * One primary command buffer a submission of a frame, allocated one
	 * by one (a driver may bound how many one call makes); the first also
	 * records the uploads.
	 */
	renderer->commands = calloc((size_t)renderer->submits * renderer->inflight, sizeof(*renderer->commands));
	if (renderer->commands == NULL) {
		renderer->operation = "calloc";
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = renderer->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	renderer->operation = "vkAllocateCommandBuffers";
	for (index = 0U; index < renderer->submits * renderer->inflight; index++) {
		error = vkAllocateCommandBuffers(renderer->device, &command, &renderer->commands[index]);
		if (error != VK_SUCCESS)
			return error;
	}
	renderer->command = renderer->commands[0];

	/*
	 * Creates each slot's completion fence, used before command reuse
	 * (signaled from the start past the first slot, whose fence the
	 * upload signals), and acquire semaphore; the first slot's are the
	 * current ones.
	 */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for (index = 0U; index < renderer->inflight; index++) {
		fence.flags = 0U;
		if (index != 0U)
			fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		renderer->operation = "vkCreateFence";
		error = vkCreateFence(renderer->device, &fence, NULL, &renderer->slot_fences[index]);
		if (error != VK_SUCCESS)
			return error;
		renderer->operation = "vkCreateSemaphore";
		error = vkCreateSemaphore(renderer->device, &semaphore, NULL, &renderer->slot_acquired[index]);
		if (error != VK_SUCCESS)
			return error;
	}
	renderer->fence = renderer->slot_fences[0];
	renderer->acquired = renderer->slot_acquired[0];

	/* Succeeded: the renderer can submit and wait for one batch at a time. */
	return VK_SUCCESS;
}

/*
 * Creates the sampler, the one-texture descriptor layout and the pipeline layout.
 *
 * The push-constant block is shared by both stages: 112 bytes of transforms
 * for the vertex stage and the material colour at offset 112 for the
 * fragment stage.  The per-pixel shading adds binding 1, the scene's
 * uniform buffer for both stages, and makes that buffer.
 */
static VkResult
renderer_layout(
	struct mview_renderer *renderer)
{
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding bindings[2];
	VkDescriptorSetLayoutCreateInfo set;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout;
	VkResult error;

	/* Trilinear filtering with repeat addressing: texture coordinates may wrap. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sampler.maxAnisotropy = 1.0f;
	sampler.compareOp = VK_COMPARE_OP_ALWAYS;
	sampler.minLod = 0.0f;
	sampler.maxLod = 16.0f;
	sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	if (renderer->linear_filter == 0) {
		/* A format without linear filtering is sampled nearest at every level. */
		sampler.magFilter = VK_FILTER_NEAREST;
		sampler.minFilter = VK_FILTER_NEAREST;
		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	}

	renderer->operation = "vkCreateSampler";
	error = vkCreateSampler(renderer->device, &sampler, NULL, &renderer->sampler);
	if (error != VK_SUCCESS)
		return error;

	/* Binding zero supplies one combined texture and sampler to the fragment stage. */
	memset(bindings, 0, sizeof(bindings));
	bindings[0].binding = 0U;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1U;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	/* Binding one, for the per-pixel shading, supplies the scene block to both stages. */
	bindings[1].binding = 1U;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[1].descriptorCount = 1U;
	bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	/* Every material set has the texture binding, and the scene binding when lit per pixel. */
	memset(&set, 0, sizeof(set));
	set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set.bindingCount = 1U;
	if (renderer->pixel_shading != 0)
		set.bindingCount = 2U;
	set.pBindings = bindings;
	renderer->operation = "vkCreateDescriptorSetLayout";
	error = vkCreateDescriptorSetLayout(renderer->device, &set, NULL, &renderer->set_layout);
	if (error != VK_SUCCESS)
		return error;

	/* One 128-byte push-constant range is visible to both stages. */
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0U;
	push.size = (uint32_t)sizeof(struct mview_push);

	/* Links the texture set and the push constants into the pipeline contract. */
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

	/* The per-pixel shading's scene block lives in one mapped uniform buffer. */
	if (renderer->pixel_shading != 0) {
		error = renderer_buffer(renderer, &renderer->scene, sizeof(struct mview_scene), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1);
		if (error != VK_SUCCESS)
			return error;
	}

	/* The copies' places: one mapped per-instance vertex buffer, written when the grid's shift changes. */
	error = renderer_buffer(renderer, &renderer->instances, (VkDeviceSize)renderer->copies * RENDERER_INSTANCE_STRIDE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
	if (error != VK_SUCCESS)
		return error;
	renderer->instance_basis[0] = -1.0f;

	/* Succeeded: descriptors and push constants have a layout. */
	return VK_SUCCESS;
}

/* Wraps one checked-in SPIR-V module. */
static VkResult
renderer_shader(
	struct mview_renderer *renderer,
	const uint32_t *words,
	size_t bytes,
	VkShaderModule *shader)
{
	VkShaderModuleCreateInfo create;
	VkResult error;

	/* The words are the offline-compiled module, passed unchanged. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create.codeSize = bytes;
	create.pCode = words;
	renderer->operation = "vkCreateShaderModule";
	error = vkCreateShaderModule(renderer->device, &create, NULL, shader);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the caller owns the module. */
	return VK_SUCCESS;
}

/* Creates the shader modules and the six material pipelines. */
static VkResult
renderer_pipelines(
	struct mview_renderer *renderer)
{
	const uint32_t *vertex_words;
	const uint32_t *fragment_words;
	const uint32_t *cutout_words;
	size_t vertex_bytes;
	size_t fragment_bytes;
	size_t cutout_bytes;
	uint32_t alpha;
	uint32_t cull;
	VkResult error;

	/* The per-vertex shading's modules, or the per-pixel shading's. */
	if (renderer->pixel_shading != 0) {
		vertex_words = mview_pixel_vertex_shader;
		vertex_bytes = sizeof(mview_pixel_vertex_shader);
		fragment_words = mview_pixel_fragment_shader;
		fragment_bytes = sizeof(mview_pixel_fragment_shader);
		cutout_words = mview_pixel_cutout_shader;
		cutout_bytes = sizeof(mview_pixel_cutout_shader);
	} else {
		vertex_words = mview_vertex_shader;
		vertex_bytes = sizeof(mview_vertex_shader);
		fragment_words = mview_fragment_shader;
		fragment_bytes = sizeof(mview_fragment_shader);
		cutout_words = mview_cutout_shader;
		cutout_bytes = sizeof(mview_cutout_shader);
	}

	/* The vertex stage is shared by every pipeline. */
	error = renderer_shader(renderer, vertex_words, vertex_bytes, &renderer->vertex_shader);
	if (error != VK_SUCCESS)
		return error;

	/* Opaque and blended materials use the plain fragment stage. */
	error = renderer_shader(renderer, fragment_words, fragment_bytes, &renderer->fragment_shader);
	if (error != VK_SUCCESS)
		return error;

	/* Cut-out materials discard transparent fragments. */
	error = renderer_shader(renderer, cutout_words, cutout_bytes, &renderer->cutout_shader);
	if (error != VK_SUCCESS)
		return error;

	/* One pipeline per alpha mode and cull mode, indexed alpha * 2 + cull. */
	for (alpha = MVIEW_ALPHA_OPAQUE; alpha <= MVIEW_ALPHA_BLEND; alpha++) {
		for (cull = MVIEW_CULL_BACK; cull <= MVIEW_CULL_NONE; cull++) {
			error = renderer_pipeline(renderer, (enum mview_alpha)alpha, (enum mview_cull)cull, &renderer->pipelines[alpha * 2U + cull]);
			if (error != VK_SUCCESS)
				return error;
		}
	}

	/* Succeeded: every material can be drawn. */
	return VK_SUCCESS;
}

/*
 * Creates one material pipeline.
 *
 * Opaque and cutout pipelines write depth without blending; the blend
 * pipeline tests depth without writing it and blends with straight alpha.
 * Front faces are counter-clockwise on screen: the projection flips y so the
 * model's counter-clockwise front faces stay counter-clockwise in the
 * framebuffer.
 */
static VkResult
renderer_pipeline(
	struct mview_renderer *renderer,
	enum mview_alpha alpha,
	enum mview_cull cull,
	VkPipeline *pipeline)
{
	static const VkDynamicState dynamic_states[2] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription bindings[2];
	VkVertexInputAttributeDescription attributes[4];
	VkPipelineVertexInputStateCreateInfo vertices;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo view;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo samples;
	VkPipelineDepthStencilStateCreateInfo depth;
	VkPipelineColorBlendAttachmentState attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic;
	VkGraphicsPipelineCreateInfo create;
	VkResult error;

	/* The vertex stage transforms and lights; the fragment stage depends on alpha. */
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = renderer->vertex_shader;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = renderer->fragment_shader;
	stages[1].pName = "main";
	if (alpha == MVIEW_ALPHA_CUTOUT)
		stages[1].module = renderer->cutout_shader;

	/* One interleaved 32-byte record supplies each vertex. */
	memset(bindings, 0, sizeof(bindings));
	bindings[0].binding = 0U;
	bindings[0].stride = RENDERER_VERTEX_STRIDE;
	bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	/* Each copy's place, one record an instance. */
	bindings[1].binding = 1U;
	bindings[1].stride = RENDERER_INSTANCE_STRIDE;
	bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

	/* Position, normal and texture coordinate, in record order. */
	memset(attributes, 0, sizeof(attributes));
	attributes[0].location = 0U;
	attributes[0].binding = 0U;
	attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
	attributes[0].offset = 0U;
	attributes[1].location = 1U;
	attributes[1].binding = 0U;
	attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
	attributes[1].offset = RENDERER_NORMAL_OFFSET;
	attributes[2].location = 2U;
	attributes[2].binding = 0U;
	attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[2].offset = RENDERER_UV_OFFSET;
	attributes[3].location = 3U;
	attributes[3].binding = 1U;
	attributes[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributes[3].offset = 0U;

	/* Matches vertex fetch to the model's vertex records. */
	memset(&vertices, 0, sizeof(vertices));
	vertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertices.vertexBindingDescriptionCount = 2U;
	vertices.pVertexBindingDescriptions = bindings;
	vertices.vertexAttributeDescriptionCount = 4U;
	vertices.pVertexAttributeDescriptions = attributes;

	/* Indices name independent triangles. */
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	/* The viewport and scissor are set per frame so a resize needs no new pipeline. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	view.viewportCount = 1U;
	view.scissorCount = 1U;

	/* Filled triangles, culled per material. */
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_BACK_BIT;
	if (cull == MVIEW_CULL_NONE)
		raster.cullMode = VK_CULL_MODE_NONE;

	/* Both faces culled when asked (--cull=all): only the vertex work runs. */
	if (renderer->cull_all != 0)
		raster.cullMode = VK_CULL_MODE_FRONT_AND_BACK;

	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;

	/* One sample per pixel. */
	memset(&samples, 0, sizeof(samples));
	samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	/* Nearer fragments win; blended surfaces do not hide what is behind them. */
	memset(&depth, 0, sizeof(depth));
	depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth.depthTestEnable = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	if (alpha == MVIEW_ALPHA_BLEND)
		depth.depthWriteEnable = VK_FALSE;

	depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	depth.front.compareOp = VK_COMPARE_OP_ALWAYS;
	depth.back.compareOp = VK_COMPARE_OP_ALWAYS;
	depth.maxDepthBounds = 1.0f;

	/* Opaque colour replaces the target; blended colour mixes by its straight alpha. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.blendEnable = VK_FALSE;
	attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
	attachment.colorBlendOp = VK_BLEND_OP_ADD;
	attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if (alpha == MVIEW_ALPHA_BLEND) {
		attachment.blendEnable = VK_TRUE;
		attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	}

	/* One colour attachment, no logic operation. */
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.logicOp = VK_LOGIC_OP_COPY;
	blend.attachmentCount = 1U;
	blend.pAttachments = &attachment;

	/* Viewport and scissor come from the command buffer. */
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2U;
	dynamic.pDynamicStates = dynamic_states;

	/* Binds every state group to the render pass and the shared layout. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	create.stageCount = 2U;
	create.pStages = stages;
	create.pVertexInputState = &vertices;
	create.pInputAssemblyState = &assembly;
	create.pViewportState = &view;
	create.pRasterizationState = &raster;
	create.pMultisampleState = &samples;
	create.pDepthStencilState = &depth;
	create.pColorBlendState = &blend;
	create.pDynamicState = &dynamic;
	create.layout = renderer->layout;
	create.renderPass = renderer->pass;
	create.basePipelineIndex = -1;
	renderer->operation = "vkCreateGraphicsPipelines";
	error = vkCreateGraphicsPipelines(renderer->device, VK_NULL_HANDLE, 1U, &create, NULL, pipeline);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the caller owns the pipeline. */
	return VK_SUCCESS;
}

/*
 * Allocates memory of a permitted type that has every required property.
 *
 * A type with none of the avoided properties is preferred: device-local
 * resources avoid host-visible types so they do not use up the small
 * host-visible window.  If only such types remain, the first one is used.
 */
static VkResult
renderer_memory(
	struct mview_renderer *renderer,
	const VkMemoryRequirements *requirements,
	VkMemoryPropertyFlags required,
	VkMemoryPropertyFlags avoided,
	VkDeviceMemory *memory)
{
	VkMemoryAllocateInfo allocation;
	VkMemoryPropertyFlags available;
	uint32_t fallback;
	uint32_t chosen;
	uint32_t index;
	VkResult error;

	/* Finds the first ideal type and the first merely acceptable one. */
	chosen = UINT32_MAX;
	fallback = UINT32_MAX;
	for (index = 0U; index < renderer->memory.memoryTypeCount; index++) {
		/* The resource's own requirements may exclude the type. */
		if ((requirements->memoryTypeBits & (1U << index)) == 0U)
			continue;

		/* Every required property must be present. */
		available = renderer->memory.memoryTypes[index].propertyFlags;
		if ((available & required) != required)
			continue;

		/* The first acceptable type is kept in case no ideal one exists. */
		if (fallback == UINT32_MAX)
			fallback = index;

		/* An ideal type has none of the avoided properties. */
		if ((available & avoided) == 0U) {
			chosen = index;
			break;
		}
	}

	/* Falls back to an acceptable type; none at all is a missing feature. */
	if (chosen == UINT32_MAX)
		chosen = fallback;

	renderer->operation = "memory type";
	if (chosen == UINT32_MAX)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Allocates the exact size the resource requires. */
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements->size;
	allocation.memoryTypeIndex = chosen;
	renderer->operation = "vkAllocateMemory";
	error = vkAllocateMemory(renderer->device, &allocation, NULL, memory);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the caller owns an allocation of a compatible type. */
	return VK_SUCCESS;
}

/* Creates one optimal-tiled device-local image with its memory and a full view. */
static VkResult
renderer_image(
	struct mview_renderer *renderer,
	struct mview_image *image,
	VkFormat format,
	uint32_t width,
	uint32_t height,
	uint32_t levels,
	VkImageUsageFlags usage,
	VkImageAspectFlags aspect)
{
	VkImageCreateInfo create;
	VkImageViewCreateInfo view;
	VkMemoryRequirements requirements;
	VkResult error;

	/* Describes a single-sample, single-layer two-dimensional image. */
	memset(image, 0, sizeof(*image));
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = format;
	create.extent.width = width;
	create.extent.height = height;
	create.extent.depth = 1U;
	create.mipLevels = levels;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = usage;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	renderer->operation = "vkCreateImage";
	error = vkCreateImage(renderer->device, &create, NULL, &image->image);
	if (error != VK_SUCCESS)
		return error;

	/* Backs the image with device-local memory outside the host-visible window. */
	vkGetImageMemoryRequirements(renderer->device, image->image, &requirements);
	error = renderer_memory(renderer, &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &image->memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the image only after its compatible allocation exists. */
	renderer->operation = "vkBindImageMemory";
	error = vkBindImageMemory(renderer->device, image->image, image->memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Exposes every level of the one aspect. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = image->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.subresourceRange.aspectMask = aspect;
	view.subresourceRange.levelCount = levels;
	view.subresourceRange.layerCount = 1U;
	renderer->operation = "vkCreateImageView";
	error = vkCreateImageView(renderer->device, &view, NULL, &image->view);
	if (error != VK_SUCCESS)
		return error;

	/* The level count drives the upload's barriers. */
	image->levels = levels;

	/* Succeeded: the image is backed and viewable, still in its undefined layout. */
	return VK_SUCCESS;
}

/*
 * Creates one buffer: host-visible and mapped for staging, device-local otherwise.
 */
static VkResult
renderer_buffer(
	struct mview_renderer *renderer,
	struct mview_buffer *buffer,
	VkDeviceSize bytes,
	VkBufferUsageFlags usage,
	int host)
{
	VkBufferCreateInfo create;
	VkMemoryRequirements requirements;
	VkResult error;

	/* Describes only the usage this buffer needs. */
	memset(buffer, 0, sizeof(*buffer));
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create.size = bytes;
	create.usage = usage;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	renderer->operation = "vkCreateBuffer";
	error = vkCreateBuffer(renderer->device, &create, NULL, &buffer->buffer);
	if (error != VK_SUCCESS)
		return error;

	/* Staging needs coherent host access; geometry prefers memory outside the host window. */
	vkGetBufferMemoryRequirements(renderer->device, buffer->buffer, &requirements);
	if (host != 0) {
		error = renderer_memory(renderer, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0U, &buffer->memory);
	} else {
		error = renderer_memory(renderer, &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &buffer->memory);
	}

	/* Without memory the buffer cannot be bound. */
	if (error != VK_SUCCESS)
		return error;

	/* Binding precedes both mapping and all GPU references. */
	renderer->operation = "vkBindBufferMemory";
	error = vkBindBufferMemory(renderer->device, buffer->buffer, buffer->memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* The staging buffer stays mapped for its whole life. */
	buffer->bytes = bytes;
	if (host != 0) {
		renderer->operation = "vkMapMemory";
		error = vkMapMemory(renderer->device, buffer->memory, 0U, VK_WHOLE_SIZE, 0U, &buffer->mapping);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: the buffer is backed, and mapped when it is a staging buffer. */
	return VK_SUCCESS;
}

/* Resets and begins the one command buffer for a single submission. */
static VkResult
renderer_begin(
	struct mview_renderer *renderer)
{
	VkCommandBufferBeginInfo begin;
	VkResult error;

	/* The previous submission has completed, so its recording can be discarded. */
	renderer->operation = "vkResetCommandBuffer";
	error = vkResetCommandBuffer(renderer->command, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Each recording is submitted exactly once. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	renderer->operation = "vkBeginCommandBuffer";
	error = vkBeginCommandBuffer(renderer->command, &begin);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the command buffer is recording. */
	return VK_SUCCESS;
}

/* Ends, submits and waits for an upload recording, so the staging buffer is free again. */
static VkResult
renderer_finish(
	struct mview_renderer *renderer)
{
	VkSubmitInfo submit;
	VkResult error;

	/* Closes the recording. */
	renderer->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(renderer->command);
	if (error != VK_SUCCESS)
		return error;

	/* The fence is unsignaled before the submission it will report. */
	renderer->operation = "vkResetFences";
	error = vkResetFences(renderer->device, 1U, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Submits the upload with no semaphores. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &renderer->command;
	renderer->operation = "vkQueueSubmit";
	error = vkQueueSubmit(renderer->queue, 1U, &submit, renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* The staging buffer may be overwritten only after the copy has read it. */
	renderer->operation = "vkWaitForFences";
	error = vkWaitForFences(renderer->device, 1U, &renderer->fence, VK_TRUE, RENDERER_GPU_TIMEOUT);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the upload has completed on the GPU. */
	return VK_SUCCESS;
}

/* Creates a device-local buffer and fills it through the staging buffer, piece by piece. */
static VkResult
renderer_upload_buffer(
	struct mview_renderer *renderer,
	struct mview_buffer *buffer,
	const void *data,
	VkDeviceSize bytes,
	VkBufferUsageFlags usage)
{
	VkBufferCopy copy;
	VkBufferMemoryBarrier barrier;
	VkDeviceSize offset;
	VkDeviceSize piece;
	VkAccessFlags access;
	VkResult error;

	/* The destination receives transfers and then serves as geometry. */
	error = renderer_buffer(renderer, buffer, bytes, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
	if (error != VK_SUCCESS)
		return error;

	/* Copies at most one staging buffer's worth per submission. */
	for (offset = 0U; offset < bytes; offset += piece) {
		piece = bytes - offset;
		if (piece > renderer->staging.bytes)
			piece = renderer->staging.bytes;

		/* The coherent mapping makes the CPU copy visible to the transfer. */
		memcpy(renderer->staging.mapping, (const uint8_t *)data + offset, (size_t)piece);

		/* Records one copy into the destination range. */
		error = renderer_begin(renderer);
		if (error != VK_SUCCESS)
			return error;

		/* The piece lands at its offset in the destination. */
		memset(&copy, 0, sizeof(copy));
		copy.srcOffset = 0U;
		copy.dstOffset = offset;
		copy.size = piece;
		vkCmdCopyBuffer(renderer->command, renderer->staging.buffer, buffer->buffer, 1U, &copy);

		/* Makes the copied bytes visible to vertex fetch or index fetch. */
		access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
		if ((usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0U)
			access = VK_ACCESS_INDEX_READ_BIT;

		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = access;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer = buffer->buffer;
		barrier.offset = offset;
		barrier.size = piece;
		vkCmdPipelineBarrier(renderer->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0U, 0U, NULL, 1U, &barrier, 0U, NULL);

		/* The staging buffer is reused only after this piece has been copied. */
		error = renderer_finish(renderer);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: the device-local buffer holds all the bytes. */
	return VK_SUCCESS;
}

/*
 * Creates a mipmapped texture image and uploads its pixels.
 *
 * With GPU blits the base level is copied and each smaller level is blitted
 * from the one above; otherwise the whole chain is box-filtered on the CPU
 * and copied level by level.  The image ends in the shader-read layout.
 */
static VkResult
renderer_upload_texture(
	struct mview_renderer *renderer,
	struct mview_image *image,
	const uint8_t *pixels,
	uint32_t width,
	uint32_t height)
{
	VkBufferImageCopy copy;
	VkImageBlit blit;
	uint8_t *chain;
	uint8_t *source;
	VkDeviceSize offset;
	VkDeviceSize bytes;
	uint32_t levels;
	uint32_t level;
	uint32_t level_width;
	uint32_t level_height;
	VkImageUsageFlags usage;
	VkResult error;

	/* The full chain down to one texel. */
	levels = renderer_levels(width, height);
	usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (renderer->blit_mipmaps != 0)
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	/* Creates the device-local image with every level. */
	error = renderer_image(renderer, image, VK_FORMAT_R8G8B8A8_UNORM, width, height, levels, usage, VK_IMAGE_ASPECT_COLOR_BIT);
	if (error != VK_SUCCESS)
		return error;

	/* Stages the pixels: the base level for blits, or the CPU-filtered chain. */
	bytes = (VkDeviceSize)width * height * 4U;
	if (renderer->blit_mipmaps != 0) {
		memcpy(renderer->staging.mapping, pixels, (size_t)bytes);
	} else {
		/* The chain is built in ordinary memory, which is fast to read back. */
		chain = malloc((size_t)renderer_chain_bytes(width, height, levels));
		if (chain == NULL) {
			renderer->operation = "malloc";
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		/* Each level is the box-filtered half of the level above. */
		memcpy(chain, pixels, (size_t)bytes);
		source = chain;
		level_width = width;
		level_height = height;
		for (level = 1U; level < levels; level++) {
			renderer_downsample(source, level_width, level_height, source + (size_t)level_width * level_height * 4U);
			source += (size_t)level_width * level_height * 4U;
			if (level_width > 1U)
				level_width /= 2U;

			/* Height halves independently of width. */
			if (level_height > 1U)
				level_height /= 2U;
		}

		/* One copy into the mapped staging buffer publishes the whole chain. */
		memcpy(renderer->staging.mapping, chain, (size_t)renderer_chain_bytes(width, height, levels));
		free(chain);
	}

	/* Records the upload into the whole image. */
	error = renderer_begin(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Every level becomes a transfer destination. */
	renderer_barrier(
		renderer->command,
		image->image,
		0U,
		levels,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		0U,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT);

	/* Copies the staged level or levels; blits need only the base level. */
	offset = 0U;
	level_width = width;
	level_height = height;
	for (level = 0U; level < levels; level++) {
		memset(&copy, 0, sizeof(copy));
		copy.bufferOffset = offset;
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel = level;
		copy.imageSubresource.layerCount = 1U;
		copy.imageExtent.width = level_width;
		copy.imageExtent.height = level_height;
		copy.imageExtent.depth = 1U;
		vkCmdCopyBufferToImage(renderer->command, renderer->staging.buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &copy);

		/* The GPU makes the smaller levels itself. */
		if (renderer->blit_mipmaps != 0)
			break;

		/* The next level follows this one in the staged chain. */
		offset += (VkDeviceSize)level_width * level_height * 4U;
		if (level_width > 1U)
			level_width /= 2U;

		/* Height halves independently of width. */
		if (level_height > 1U)
			level_height /= 2U;
	}

	/* Blits each level from the one above, then hands the finished level to the shader. */
	level_width = width;
	level_height = height;
	for (level = 1U; level < levels && renderer->blit_mipmaps != 0; level++) {
		/* The level above becomes the blit source. */
		renderer_barrier(
			renderer->command,
			image->image,
			level - 1U,
			1U,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_TRANSFER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT);

		/* Filters the level above down to this level. */
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.mipLevel = level - 1U;
		blit.srcSubresource.layerCount = 1U;
		blit.srcOffsets[1].x = (int32_t)level_width;
		blit.srcOffsets[1].y = (int32_t)level_height;
		blit.srcOffsets[1].z = 1;
		if (level_width > 1U)
			level_width /= 2U;

		/* Height halves independently of width. */
		if (level_height > 1U)
			level_height /= 2U;

		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.mipLevel = level;
		blit.dstSubresource.layerCount = 1U;
		blit.dstOffsets[1].x = (int32_t)level_width;
		blit.dstOffsets[1].y = (int32_t)level_height;
		blit.dstOffsets[1].z = 1;
		vkCmdBlitImage(
			renderer->command,
			image->image,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			image->image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1U,
			&blit,
			VK_FILTER_LINEAR);

		/* The level above is finished and becomes readable by the fragment stage. */
		renderer_barrier(
			renderer->command,
			image->image,
			level - 1U,
			1U,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_READ_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	/* The levels still in the destination layout become readable by the fragment stage. */
	level = 0U;
	if (renderer->blit_mipmaps != 0)
		level = levels - 1U;

	renderer_barrier(
		renderer->command,
		image->image,
		level,
		levels - level,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	/* The staging buffer is reused only after this texture has been copied. */
	error = renderer_finish(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the texture and all its levels are ready for sampling. */
	return VK_SUCCESS;
}

/* Allocates one descriptor set per texture image and points it at that image. */
static VkResult
renderer_descriptors(
	struct mview_renderer *renderer)
{
	VkDescriptorPoolSize sizes[2];
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocation;
	VkDescriptorImageInfo image;
	VkDescriptorBufferInfo scene;
	VkWriteDescriptorSet writes[2];
	uint32_t count;
	uint32_t index;
	VkResult error;

	/* Exactly one combined image sampler per texture, and one scene block per set when lit per pixel. */
	memset(sizes, 0, sizeof(sizes));
	sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[0].descriptorCount = renderer->texture_count;
	sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sizes[1].descriptorCount = renderer->texture_count;
	count = 1U;
	if (renderer->pixel_shading != 0)
		count = 2U;

	/* The pool owns every set until the renderer closes. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = renderer->texture_count;
	pool.poolSizeCount = count;
	pool.pPoolSizes = sizes;
	renderer->operation = "vkCreateDescriptorPool";
	error = vkCreateDescriptorPool(renderer->device, &pool, NULL, &renderer->descriptor_pool);
	if (error != VK_SUCCESS)
		return error;

	/* Holds the set handles, indexed like the texture images. */
	renderer->sets = calloc(renderer->texture_count, sizeof(*renderer->sets));
	if (renderer->sets == NULL) {
		renderer->operation = "calloc";
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Allocates and writes each texture's set. */
	for (index = 0U; index < renderer->texture_count; index++) {
		memset(&allocation, 0, sizeof(allocation));
		allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocation.descriptorPool = renderer->descriptor_pool;
		allocation.descriptorSetCount = 1U;
		allocation.pSetLayouts = &renderer->set_layout;
		renderer->operation = "vkAllocateDescriptorSets";
		error = vkAllocateDescriptorSets(renderer->device, &allocation, &renderer->sets[index]);
		if (error != VK_SUCCESS)
			return error;

		/* The upload left the image in the shader-read layout. */
		memset(&image, 0, sizeof(image));
		image.sampler = renderer->sampler;
		image.imageView = renderer->textures[index].view;
		image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		/* The whole scene block, for the per-pixel shading. */
		memset(&scene, 0, sizeof(scene));
		scene.buffer = renderer->scene.buffer;
		scene.offset = 0U;
		scene.range = sizeof(struct mview_scene);

		/* Publishes the texture binding of the set, and the scene binding when lit per pixel. */
		memset(writes, 0, sizeof(writes));
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = renderer->sets[index];
		writes[0].dstBinding = 0U;
		writes[0].descriptorCount = 1U;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[0].pImageInfo = &image;
		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = renderer->sets[index];
		writes[1].dstBinding = 1U;
		writes[1].descriptorCount = 1U;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[1].pBufferInfo = &scene;
		vkUpdateDescriptorSets(renderer->device, count, writes, 0U, NULL);
	}

	/* Succeeded: every texture can be bound by its set. */
	return VK_SUCCESS;
}

/* Records one image layout transition over a range of mip levels. */
static void
renderer_barrier(
	VkCommandBuffer command,
	VkImage image,
	uint32_t level,
	uint32_t count,
	VkImageLayout old_layout,
	VkImageLayout new_layout,
	VkAccessFlags source_access,
	VkAccessFlags destination_access,
	VkPipelineStageFlags source_stage,
	VkPipelineStageFlags destination_stage)
{
	VkImageMemoryBarrier barrier;

	/* Describes the transition of the colour levels [level, level + count). */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = source_access;
	barrier.dstAccessMask = destination_access;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = level;
	barrier.subresourceRange.levelCount = count;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(command, source_stage, destination_stage, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* Succeeded: the transition is recorded. */
	return;
}

/*
 * Records the frame: clear, then each material group with its pipeline,
 * texture and colour.  Groups are already ordered opaque, cutout, blend.
 */
static void
renderer_record(
	struct mview_renderer *renderer,
	const struct mview_model *model,
	const struct mview_camera *camera,
	uint32_t image,
	uint32_t part)
{
	VkRenderPassBeginInfo pass;
	VkClearValue clear[2];
	VkViewport viewport;
	VkRect2D scissor;
	VkBuffer buffers[2];
	VkDeviceSize offsets[2];
	VkPipeline bound;
	struct mview_push push;
	struct mview_camera view;
	const struct mview_material *material;
	const struct mview_group *group;
	uint32_t pipeline;
	uint32_t texture;
	uint32_t index;
	uint32_t copy;
	uint32_t columns;
	uint32_t rows;
	uint32_t draws;
	uint32_t instances;
	uint32_t first;
	uint32_t last;
	float spacing;

	/* A neutral dark grey background and the far depth plane. */
	memset(clear, 0, sizeof(clear));
	clear[0].color.float32[0] = 0.2f;
	clear[0].color.float32[1] = 0.2f;
	clear[0].color.float32[2] = 0.2f;
	clear[0].color.float32[3] = 1.0f;
	clear[1].depthStencil.depth = 1.0f;

	/* Begins the pass on the acquired image. */
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = renderer->pass;
	if (renderer->submits > 1U) {
		/* A part of a frame of several submissions: the first clears, the last presents. */
		pass.renderPass = renderer->part_passes[1];
		if (part == 0U)
			pass.renderPass = renderer->part_passes[0];
		if (part + 1U == renderer->submits)
			pass.renderPass = renderer->part_passes[2];
	}
	pass.framebuffer = renderer->targets[image].framebuffer;
	pass.renderArea.extent = renderer->extent;
	pass.clearValueCount = 2U;
	pass.pClearValues = clear;
	vkCmdBeginRenderPass(renderer->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The viewport and scissor cover the whole current extent. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)renderer->extent.width;
	viewport.height = (float)renderer->extent.height;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(renderer->command, 0U, 1U, &viewport);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent = renderer->extent;
	vkCmdSetScissor(renderer->command, 0U, 1U, &scissor);

	/* The model's vertices, the copies' places and one index buffer serve every group. */
	buffers[0] = renderer->vertices.buffer;
	buffers[1] = renderer->instances.buffer;
	offsets[0] = 0U;
	offsets[1] = 0U;
	vkCmdBindVertexBuffers(renderer->command, 0U, 2U, buffers, offsets);
	vkCmdBindIndexBuffer(renderer->command, renderer->indices.buffer, 0U, VK_INDEX_TYPE_UINT32);

	/*
	 * The copies' grid (--copies): as many columns as the square root asks,
	 * the rows that fill, the viewer moved back so the whole grid shows.
	 */
	view = *camera;
	columns = 1U;
	while (columns * columns < renderer->copies)
		columns++;
	rows = (renderer->copies + columns - 1U) / columns;
	if (columns > 1U)
		view.distance = camera->distance * (float)columns;
	spacing = 2.0f * camera->radius;

	/*
	 * The per-pixel shading's scene block for this view; the previous frame's
	 * fence was waited on, so no GPU work reads the buffer while it changes.
	 */
	if (renderer->pixel_shading != 0)
		mview_camera_scene(&view, renderer->extent.width, renderer->extent.height, (struct mview_scene *)renderer->scene.mapping);

	/* The copies' places, written again only when the grid's shift changed. */
	renderer_instances(renderer, &view, columns, rows, spacing);

	/* The transforms are the same for every group and copy; the colour changes per material. */
	mview_camera_push(&view, renderer->extent.width, renderer->extent.height, &push);

	/*
	 * One instanced draw a group draws every copy; with --draws=each every
	 * copy is its own run of draws, its instance the first and only one.
	 */
	first = (uint32_t)((uint64_t)renderer->copies * part / renderer->submits);
	last = (uint32_t)((uint64_t)renderer->copies * (part + 1U) / renderer->submits);
	draws = first + 1U;
	instances = last - first;
	if (renderer->each_draw != 0) {
		draws = last;
		instances = 1U;
	}

	/* Draws each group with its material's pipeline, texture and colour (the part's copies only). */
	bound = VK_NULL_HANDLE;
	if (last == first)
		draws = first;
	for (copy = first; copy < draws; copy++) {
		for (index = 0U; index < model->group_count; index++) {
			group = &model->groups[index];
			material = &model->materials[group->material];

			/* Consecutive groups of the same kind keep the bound pipeline. */
			pipeline = (uint32_t)material->alpha * 2U + (uint32_t)material->cull;
			if (renderer->pipelines[pipeline] != bound) {
				bound = renderer->pipelines[pipeline];
				vkCmdBindPipeline(renderer->command, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
			}

			/* An untextured material samples the white texture after the model's own. */
			texture = model->texture_count;
			if (material->texture >= 0)
				texture = (uint32_t)material->texture;

			/* Binds the material's texture. */
			vkCmdBindDescriptorSets(renderer->command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->layout, 0U, 1U, &renderer->sets[texture], 0U, NULL);

			/* Pushes the transforms with this material's colour. */
			memcpy(push.color, material->color, sizeof(push.color));
			vkCmdPushConstants(
				renderer->command,
				renderer->layout,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0U,
				(uint32_t)sizeof(push),
				&push);

			/* Draws the group's triangles for the copies (or the one copy). */
			vkCmdDrawIndexed(renderer->command, group->count, instances, group->first, 0, copy);
		}
	}

	/* Ends the pass, which leaves the image ready to present. */
	vkCmdEndRenderPass(renderer->command);

	/* Succeeded: the frame is recorded. */
	return;
}

/*
 * Writes the copies' places into the per-instance buffer: each copy's
 * column and row about the grid's centre, as a shift of the clip position
 * (the clip shift of one column and of one row, taken from the transforms
 * of the view moved by one spacing) and of the view-space position.  The
 * buffer is written again only when those shifts changed; the previous
 * frame's fence was waited on, so no GPU work reads it meanwhile.
 */
static void
renderer_instances(
	struct mview_renderer *renderer,
	const struct mview_camera *view,
	uint32_t columns,
	uint32_t rows,
	float spacing)
{
	struct mview_camera moved;
	struct mview_push base;
	struct mview_push shifted;
	float basis[6];
	float column;
	float row;
	float *place;
	uint32_t copy;
	int same;

	/* The clip position of the view as it is. */
	mview_camera_push(view, renderer->extent.width, renderer->extent.height, &base);

	/* One column to the right: its clip shift. */
	moved = *view;
	moved.pan[0] += spacing;
	mview_camera_push(&moved, renderer->extent.width, renderer->extent.height, &shifted);
	basis[0] = shifted.clip[12] - base.clip[12];
	basis[1] = shifted.clip[13] - base.clip[13];

	/* One row down: its clip shift. */
	moved = *view;
	moved.pan[1] -= spacing;
	mview_camera_push(&moved, renderer->extent.width, renderer->extent.height, &shifted);
	basis[2] = shifted.clip[12] - base.clip[12];
	basis[3] = shifted.clip[13] - base.clip[13];
	basis[4] = spacing;
	basis[5] = (float)columns;

	/* Unchanged shifts leave the places as they are. */
	same = memcmp(basis, renderer->instance_basis, sizeof(basis));
	if (same == 0)
		return;
	memcpy(renderer->instance_basis, basis, sizeof(basis));

	/* Each copy's column and row about the centre, in clip space and in view space. */
	place = (float *)renderer->instances.mapping;
	for (copy = 0U; copy < renderer->copies; copy++) {
		column = (float)(copy % columns) - 0.5f * (float)(columns - 1U);
		row = (float)(copy / columns) - 0.5f * (float)(rows - 1U);
		place[copy * 4U + 0U] = column * basis[0] + row * basis[2];
		place[copy * 4U + 1U] = column * basis[1] + row * basis[3];
		place[copy * 4U + 2U] = column * spacing;
		place[copy * 4U + 3U] = -row * spacing;
	}

	/* Succeeded: the places match the view. */
	return;
}

/* Reports the number of mip levels down to one texel. */
static uint32_t
renderer_levels(
	uint32_t width,
	uint32_t height)
{
	uint32_t levels;

	/* Each level halves the larger side until it is one texel. */
	levels = 1U;
	while (width > 1U || height > 1U) {
		width /= 2U;
		height /= 2U;
		levels++;
	}

	/* Reports the count including the base level. */
	return levels;
}

/* Reports the bytes of the first levels of an RGBA8 mip chain. */
static VkDeviceSize
renderer_chain_bytes(
	uint32_t width,
	uint32_t height,
	uint32_t levels)
{
	VkDeviceSize bytes;
	uint32_t level;

	/* Sums the levels, each at least one texel on each side. */
	bytes = 0U;
	for (level = 0U; level < levels; level++) {
		bytes += (VkDeviceSize)width * height * 4U;
		if (width > 1U)
			width /= 2U;

		/* Height halves independently of width. */
		if (height > 1U)
			height /= 2U;
	}

	/* Reports the chain size. */
	return bytes;
}

/*
 * Box-filters one RGBA8 level into the next smaller one.
 *
 * Each destination texel averages the up to four source texels it covers,
 * rounding to nearest; an odd last row or column is reused at the edge.
 */
static void
renderer_downsample(
	const uint8_t *source,
	uint32_t width,
	uint32_t height,
	uint8_t *destination)
{
	uint32_t next_width;
	uint32_t next_height;
	uint32_t x;
	uint32_t y;
	uint32_t x0;
	uint32_t x1;
	uint32_t y0;
	uint32_t y1;
	uint32_t channel;
	uint32_t sum;

	/* The next level halves each side, never below one texel. */
	next_width = width / 2U;
	if (next_width == 0U)
		next_width = 1U;

	/* Height halves the same way, independently of width. */
	next_height = height / 2U;
	if (next_height == 0U)
		next_height = 1U;

	/* Averages each 2x2 source block into one destination texel. */
	for (y = 0U; y < next_height; y++) {
		/* The two source rows; an odd last row is paired with itself. */
		y0 = y * 2U;
		y1 = y0 + 1U;
		if (y1 >= height)
			y1 = height - 1U;

		/* Walks the destination row. */
		for (x = 0U; x < next_width; x++) {
			/* The two source columns; an odd last column is paired with itself. */
			x0 = x * 2U;
			x1 = x0 + 1U;
			if (x1 >= width)
				x1 = width - 1U;

			/* Each channel is averaged independently. */
			for (channel = 0U; channel < 4U; channel++) {
				sum = source[((size_t)y0 * width + x0) * 4U + channel];
				sum += source[((size_t)y0 * width + x1) * 4U + channel];
				sum += source[((size_t)y1 * width + x0) * 4U + channel];
				sum += source[((size_t)y1 * width + x1) * 4U + channel];
				destination[((size_t)y * next_width + x) * 4U + channel] = (uint8_t)((sum + 2U) / 4U);
			}
		}
	}

	/* Succeeded: the smaller level is written. */
	return;
}

/* Releases one image, its view and its memory; partial images are allowed. */
static void
renderer_image_free(
	struct mview_renderer *renderer,
	struct mview_image *image)
{
	/* The view refers to the image and goes first. */
	if (image->view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, image->view, NULL);

	/* The image no longer has a view. */
	if (image->image != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, image->image, NULL);

	/* The memory no longer backs an image. */
	if (image->memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, image->memory, NULL);

	/* Clears the handles so a second release is harmless. */
	memset(image, 0, sizeof(*image));

	/* Succeeded: the image is released. */
	return;
}

/* Releases one buffer and its memory; freeing mapped memory also unmaps it. */
static void
renderer_buffer_free(
	struct mview_renderer *renderer,
	struct mview_buffer *buffer)
{
	/* The buffer is released before the memory that backs it. */
	if (buffer->buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(renderer->device, buffer->buffer, NULL);

	/* Freeing a mapped allocation implicitly unmaps it. */
	if (buffer->memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, buffer->memory, NULL);

	/* Clears the handles so a second release is harmless. */
	memset(buffer, 0, sizeof(*buffer));

	/* Succeeded: the buffer is released. */
	return;
}

/*
 * Reads the processor's cycle counter; the clock a user program can read
 * has only tick resolution.  On arm64 (the Linux arm64 package) the virtual
 * counter of the generic timer, on riscv64 the time counter.
 */
static uint64_t
renderer_cycles(
	void)
{
#if defined(__aarch64__)
	uint64_t value;

	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));

	/* Succeeded: the counter. */
	return value;
#elif defined(__riscv)
	uint64_t value;

	/* The time counter, which the kernel lets user mode read (scounteren.TM). */
	__asm__ volatile("rdtime %0" : "=r"(value));

	/* Succeeded: the counter. */
	return value;
#else
	uint32_t low;
	uint32_t high;

	__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));

	/* Succeeded: the counter as one 64-bit value. */
	return ((uint64_t)high << 32) | low;
#endif
}

/* Adds the cycles since `mark` to one stage and moves the mark to now. */
static void
renderer_stage(
	struct mview_renderer *renderer,
	enum mview_stage stage,
	uint64_t *mark)
{
	uint64_t now;

	now = renderer_cycles();
	renderer->stage_cycles[stage] += now - *mark;
	renderer->stage_last[stage] = now - *mark;
	*mark = now;
}

/*
 * Reports the share of the drawn frames' time each stage took, and clears the
 * counters.  `seconds` is how long the frames took in all, so the cycle
 * counter can be scaled to milliseconds.
 */
void
mview_renderer_report_stages(
	struct mview_renderer *renderer,
	const char *token,
	double seconds)
{
	static const char *const names[MVIEW_STAGE_COUNT] = { "acquire", "record", "submit", "present", "fence" };
	uint64_t total;
	double per_cycle;
	unsigned stage;

	/* The cycle counter's rate over the frames counted: their cycles over their seconds. */
	total = 0U;
	for (stage = 0U; stage < MVIEW_STAGE_COUNT; stage++)
		total += renderer->stage_cycles[stage];
	if (total == 0U || renderer->stage_frames == 0U || seconds <= 0.0 || renderer->span_cycles == 0U)
		return;
	per_cycle = 1000.0 * seconds / (double)renderer->span_cycles;

	printf("MVIEW STAGES run=%s frames=%u frame=%.2fms", token, renderer->stage_frames,
	    per_cycle * (double)renderer->span_cycles / (double)renderer->stage_frames);
	for (stage = 0U; stage < MVIEW_STAGE_COUNT; stage++) {
		printf(" %s=%.2fms",
		    names[stage],
		    per_cycle * (double)renderer->stage_cycles[stage] / (double)renderer->stage_frames);
	}
	printf(" other=%.2fms\n", per_cycle * (double)(renderer->span_cycles - total) / (double)renderer->stage_frames);
	fflush(stdout);

	memset(renderer->stage_cycles, 0, sizeof(renderer->stage_cycles));
	renderer->stage_frames = 0U;
	renderer->span_cycles = 0U;
}

/* Marks the start or the end of the span the stages are reported over. */
void
mview_renderer_span(
	struct mview_renderer *renderer,
	int end)
{
	uint64_t now;

	now = renderer_cycles();
	if (end)
		renderer->span_cycles = now - renderer->span_start;
	else
		renderer->span_start = now;
}
