/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Window mode: draws the background and every window, bottom to top, into
 * a VK_KHR_display swapchain (WS035 compositing design, D1).
 *
 * A frame is one command buffer and one render pass that clears to the
 * background and draws each window as a textured quad placed by push
 * constants.  Only one frame is in flight: its fence is exported as an fd
 * that the event loop polls, and the buffers the frame sampled and the frame
 * callbacks of the surfaces it showed are held until the fence signals.
 */

#include "compose.h"
#include "shaders.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static VkResult compose_device(struct zwl_compose *compose);
static VkResult compose_objects(struct zwl_compose *compose);
static VkResult compose_pass(struct zwl_compose *compose);
static VkResult compose_pipeline(struct zwl_compose *compose, enum zwl_draw draw, VkShaderModule vertex, VkShaderModule fragment);
static VkResult compose_targets(struct zwl_compose *compose);
static void compose_targets_destroy(struct zwl_compose *compose);
static unsigned compose_windows(struct zwl_server *server, struct zwl_object **windows, unsigned capacity);
static void compose_quad(struct zwl_server *server, VkCommandBuffer command, const struct zwl_import *import, int32_t x, int32_t y);
static const struct zwl_import *surface_image(const struct zwl_object *surface);
static void compose_cursor(struct zwl_server *server, VkCommandBuffer command);
static VkResult compose_record(struct zwl_server *server, uint32_t image, struct zwl_object **windows, unsigned count);
static VkResult compose_submit(struct zwl_server *server, uint32_t image);
static void compose_hold(struct zwl_server *server, struct zwl_object **windows, unsigned count);

/*
 * Creates the Vulkan device, the pipelines' fixed objects and the frame's
 * synchronization; the output is opened separately.
 */
int
zwl_compose_open(
	struct zwl_server *server)
{
	struct zwl_compose *compose;
	VkResult result;
	int error;

	/* The state lives as long as the compositor. */
	compose = calloc(1, sizeof(*compose));
	if (compose == NULL)
		return ENOMEM;
	server->compose = compose;

	/* The instance, the device and its graphics queue. */
	result = compose_device(compose);
	if (result != VK_SUCCESS) {
		printf("ZWL VULKAN_ERROR operation=device result=%d\n", (int)result);
		return EIO;
	}

	/* The layouts, sampler, pools and synchronization of a frame. */
	result = compose_objects(compose);
	if (result != VK_SUCCESS) {
		printf("ZWL VULKAN_ERROR operation=objects result=%d\n", (int)result);
		return EIO;
	}

	/* zdesktop's arrow cursor. */
	error = zwl_arrow_create(server);
	if (error != 0) {
		printf("ZWL VULKAN_ERROR operation=arrow\n");
		return EIO;
	}

	/* Succeeded: window mode can open its output. */
	return 0;
}

/*
 * Creates the display surface and its swapchain for window mode, and the
 * render pass and pipelines the first time the output's format is known.
 */
int
zwl_compose_output_open(
	struct zwl_server *server)
{
	struct zwl_compose *compose;
	VkResult result;

	/* An open output needs nothing. */
	compose = server->compose;
	if (compose->output_open)
		return 0;

	/* The display plane's surface at the compositor's size. */
	result = vkdemo_display_open(compose->instance, compose->physical, server->width, server->height, &compose->output);
	if (result != VK_SUCCESS) {
		printf("ZWL VULKAN_ERROR operation=display result=%d\n", (int)result);
		return EIO;
	}

	/* Its FIFO swapchain. */
	result = vkdemo_display_create_swapchain(compose->physical, compose->device, compose->family, &compose->output, 0);
	if (result != VK_SUCCESS ||
	    compose->output.image_count > ZWL_SWAPCHAIN_MAX) {
		printf("ZWL VULKAN_ERROR operation=swapchain result=%d images=%u\n", (int)result, compose->output.image_count);
		vkdemo_display_close(compose->instance, compose->device, &compose->output);
		return EIO;
	}

	/* The pass and pipelines follow the output's format, which does not change. */
	if (compose->pass == VK_NULL_HANDLE) {
		compose->format = compose->output.format;
		result = compose_pass(compose);
		if (result != VK_SUCCESS) {
			printf("ZWL VULKAN_ERROR operation=pipelines result=%d\n", (int)result);
			vkdemo_display_close(compose->instance, compose->device, &compose->output);
			return EIO;
		}
	}

	/* A view, framebuffer and semaphore for each swapchain image. */
	result = compose_targets(compose);
	if (result != VK_SUCCESS) {
		printf("ZWL VULKAN_ERROR operation=targets result=%d\n", (int)result);
		compose_targets_destroy(compose);
		vkdemo_display_close(compose->instance, compose->device, &compose->output);
		return EIO;
	}

	/* Succeeded: window mode owns the display through the swapchain. */
	compose->output_open = 1;
	server->dirty = 1;
	printf("ZWL OUTPUT open width=%u height=%u images=%u format=%d\n", server->width, server->height, compose->output.image_count, (int)compose->format);
	return 0;
}

/*
 * Destroys the swapchain and the display surface once the device is idle,
 * which gives the display back (entering fullscreen mode, or at exit).
 */
void
zwl_compose_output_close(
	struct zwl_server *server)
{
	struct zwl_compose *compose;

	/* A closed output has nothing to destroy. */
	compose = server->compose;
	if (compose == NULL || !compose->output_open)
		return;

	/* No frame may still use the swapchain's images. */
	(void)vkDeviceWaitIdle(compose->device);

	/* The targets, then the swapchain and surface. */
	compose_targets_destroy(compose);
	vkdemo_display_close(compose->instance, compose->device, &compose->output);
	compose->output_open = 0;
	printf("ZWL OUTPUT closed\n");
}

/*
 * Draws one frame of window mode and presents it.  Returns 0 without
 * drawing while a frame is in flight or the output is closed.
 */
int
zwl_compose_draw(
	struct zwl_server *server)
{
	struct zwl_object *windows[ZWL_FRAME_WINDOWS];
	struct zwl_compose *compose;
	uint64_t mark;
	uint32_t image;
	unsigned count;
	VkResult result;

	/* One frame at a time, and only with an output. */
	compose = server->compose;
	if (compose->in_flight || !compose->output_open)
		return 0;

	/* The windows to draw, bottom to top. */
	compose->frame_start_cycles = zwl_cycles();
	compose->frame_start_ms = zwl_milliseconds();
	count = compose_windows(server, windows, ZWL_FRAME_WINDOWS);

	/* The next swapchain image (the wait for it is measured apart). */
	mark = zwl_cycles();
	result = vkAcquireNextImageKHR(compose->device, compose->output.swapchain, UINT64_MAX, compose->acquired, VK_NULL_HANDLE, &image);
	server->perf.compose_acquire_cycles += zwl_cycles() - mark;
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		printf("ZWL VULKAN_ERROR operation=acquire result=%d\n", (int)result);
		return EIO;
	}

	/* The frame's commands. */
	result = compose_record(server, image, windows, count);
	if (result != VK_SUCCESS) {
		printf("ZWL VULKAN_ERROR operation=record result=%d\n", (int)result);
		return EIO;
	}

	/* Submitted and presented; the fence fd tells the event loop when it is done. */
	mark = zwl_cycles();
	result = compose_submit(server, image);
	server->perf.compose_present_cycles += zwl_cycles() - mark;
	if (result != VK_SUCCESS) {
		printf("ZWL VULKAN_ERROR operation=submit result=%d\n", (int)result);
		return EIO;
	}

	/* The CPU time of recording and submitting the frame. */
	server->perf.compose_draw_cycles += zwl_cycles() - compose->frame_start_cycles;

	/* The frame holds what it sampled until its fence signals. */
	compose_hold(server, windows, count);
	server->dirty = 0;
	server->frame++;
	if (server->log_frames)
		printf("ZWL COMPOSE frame=%llu image=%u windows=%u\n", (unsigned long long)server->frame, image, count);

	/* Succeeded: one frame is in flight. */
	return 0;
}

/*
 * Ends the frame in flight after its fence signaled: the buffers it
 * sampled are released (when nothing else holds them) and the frame
 * callbacks of the surfaces it showed are sent.
 */
int
zwl_compose_complete(
	struct zwl_server *server)
{
	struct zwl_compose *compose;
	uint64_t elapsed;
	unsigned index;
	VkResult result;

	/* Only a frame in flight completes. */
	compose = server->compose;
	if (!compose->in_flight)
		return 0;

	/* The fence fd has done its work. */
	if (server->frame_fd >= 0) {
		close(server->frame_fd);
		server->frame_fd = -1;
	}

	/* The fence is signaled (the fd was readable, or the caller waits for it); it is reset for the next frame. */
	result = vkWaitForFences(compose->device, 1U, &compose->fence, VK_TRUE, UINT64_MAX);
	if (result == VK_SUCCESS)
		result = vkResetFences(compose->device, 1U, &compose->fence);
	if (result != VK_SUCCESS) {
		printf("ZWL VULKAN_ERROR operation=fence result=%d\n", (int)result);
		return EIO;
	}

	/* The sampled buffers, and the frame callbacks. */
	for (index = 0; index < compose->held_count; index++)
		zwl_buffer_put(compose->held[index]);
	compose->held_count = 0;
	zwl_callbacks_done(&compose->callbacks);
	compose->in_flight = 0;

	/* The time from the start of the frame to its completion (reported by ZWL PERF). */
	elapsed = zwl_cycles() - compose->frame_start_cycles;
	server->perf.compose_frames++;
	server->perf.compose_cycles += elapsed;

	/* The next frame waits a moment for the windows this one told (half its time, 4 to 50 ms). */
	server->frame_done_ms = zwl_milliseconds();
	server->frame_wait_ms = (server->frame_done_ms - compose->frame_start_ms) / 2U;
	if (server->frame_wait_ms < 4U)
		server->frame_wait_ms = 4U;
	if (server->frame_wait_ms > 50U)
		server->frame_wait_ms = 50U;

	/* Succeeded: the next frame may be drawn. */
	return 0;
}

/*
 * Finishes the frame in flight now, waiting for it: before a client whose
 * buffers and callbacks it may hold is destroyed, and at exit.
 */
void
zwl_compose_quiesce(
	struct zwl_server *server)
{
	int error;

	/* Without window mode, or without a frame in flight, nothing waits. */
	if (server->compose == NULL || !server->compose->in_flight)
		return;

	/* The frame completes; a failure is the compositor's. */
	error = zwl_compose_complete(server);
	if (error != 0)
		server->failed = 1;
}

/*
 * Releases every Vulkan object, after the output (at compositor exit).
 */
void
zwl_compose_close(
	struct zwl_server *server)
{
	struct zwl_compose *compose;
	unsigned index;

	/* Nothing was opened. */
	compose = server->compose;
	if (compose == NULL)
		return;

	/* The frame in flight and the output first, then the arrow. */
	if (compose->device != VK_NULL_HANDLE)
		(void)vkDeviceWaitIdle(compose->device);
	(void)zwl_compose_complete(server);
	zwl_compose_output_close(server);
	zwl_arrow_destroy(server);

	/* The device's objects. */
	if (compose->device != VK_NULL_HANDLE) {
		for (index = 0; index < 2U; index++) {
			if (compose->pipelines[index] != VK_NULL_HANDLE)
				vkDestroyPipeline(compose->device, compose->pipelines[index], NULL);
		}

		/* The pass, layouts, sampler, pools and synchronization. */
		vkDestroyRenderPass(compose->device, compose->pass, NULL);
		vkDestroyPipelineLayout(compose->device, compose->layout, NULL);
		vkDestroyDescriptorSetLayout(compose->device, compose->set_layout, NULL);
		vkDestroySampler(compose->device, compose->sampler, NULL);
		vkDestroyDescriptorPool(compose->device, compose->descriptors, NULL);
		vkDestroySemaphore(compose->device, compose->acquired, NULL);
		vkDestroyFence(compose->device, compose->fence, NULL);
		vkDestroyCommandPool(compose->device, compose->pool, NULL);
		vkDestroyDevice(compose->device, NULL);
	}

	/* The instance last. */
	if (compose->instance != VK_NULL_HANDLE)
		vkDestroyInstance(compose->instance, NULL);
	free(compose);
	server->compose = NULL;
}

/* Creates the instance, picks the first device with a graphics queue, and creates the device. */
static VkResult
compose_device(
	struct zwl_compose *compose)
{
	static const char *const instance_extensions[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_DISPLAY_EXTENSION_NAME,
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
		VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME
	};
	static const char *const device_extensions[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
		VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME
	};
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	VkQueueFamilyProperties families[16];
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo device;
	float priority;
	uint32_t count;
	uint32_t index;
	VkResult result;

	/* The instance, with the display extensions and those the external fd extensions need (Vulkan 1.0). */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "zwl";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = 5U;
	instance.ppEnabledExtensionNames = instance_extensions;
	result = vkCreateInstance(&instance, NULL, &compose->instance);
	if (result != VK_SUCCESS)
		return result;

	/* The first physical device. */
	count = 1U;
	result = vkEnumeratePhysicalDevices(compose->instance, &count, &compose->physical);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Its first graphics queue family. */
	count = 16U;
	vkGetPhysicalDeviceQueueFamilyProperties(compose->physical, &count, families);
	compose->family = UINT32_MAX;
	for (index = 0; index < count; index++) {
		if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
			compose->family = index;
			break;
		}
	}

	/* A device without a graphics queue cannot draw. */
	if (compose->family == UINT32_MAX)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The device, with one queue and the swapchain and external fd extensions. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = compose->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;
	memset(&device, 0, sizeof(device));
	device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device.queueCreateInfoCount = 1U;
	device.pQueueCreateInfos = &queue;
	device.enabledExtensionCount = 5U;
	device.ppEnabledExtensionNames = device_extensions;
	result = vkCreateDevice(compose->physical, &device, NULL, &compose->device);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the queue the frames are submitted to. */
	vkGetDeviceQueue(compose->device, compose->family, 0U, &compose->queue);
	return VK_SUCCESS;
}

/* Creates the descriptor and pipeline layouts, the sampler, the pools and the frame's synchronization. */
static VkResult
compose_objects(
	struct zwl_compose *compose)
{
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkPushConstantRange range;
	VkPipelineLayoutCreateInfo layout;
	VkSamplerCreateInfo sampler;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool;
	VkCommandPoolCreateInfo command_pool;
	VkCommandBufferAllocateInfo command;
	VkExportFenceCreateInfo export;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkResult result;

	/* One sampled image for the fragment shader. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1U;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 1U;
	set_layout.pBindings = &binding;
	result = vkCreateDescriptorSetLayout(compose->device, &set_layout, NULL, &compose->set_layout);
	if (result != VK_SUCCESS)
		return result;

	/* The quad's place and texture coordinates as push constants. */
	memset(&range, 0, sizeof(range));
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	range.size = 8U * sizeof(float);
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1U;
	layout.pSetLayouts = &compose->set_layout;
	layout.pushConstantRangeCount = 1U;
	layout.pPushConstantRanges = &range;
	result = vkCreatePipelineLayout(compose->device, &layout, NULL, &compose->layout);
	if (result != VK_SUCCESS)
		return result;

	/* Nearest sampling at the edge: a window drawn at its own size is copied pixel for pixel. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = 0.0f;
	result = vkCreateSampler(compose->device, &sampler, NULL, &compose->sampler);
	if (result != VK_SUCCESS)
		return result;

	/* A descriptor set for each imported buffer, freed with it. */
	memset(&pool_size, 0, sizeof(pool_size));
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = ZWL_DESCRIPTOR_MAX;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool.maxSets = ZWL_DESCRIPTOR_MAX;
	pool.poolSizeCount = 1U;
	pool.pPoolSizes = &pool_size;
	result = vkCreateDescriptorPool(compose->device, &pool, NULL, &compose->descriptors);
	if (result != VK_SUCCESS)
		return result;

	/* One command buffer, re-recorded every frame. */
	memset(&command_pool, 0, sizeof(command_pool));
	command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	command_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	command_pool.queueFamilyIndex = compose->family;
	result = vkCreateCommandPool(compose->device, &command_pool, NULL, &compose->pool);
	if (result != VK_SUCCESS)
		return result;
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = compose->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	result = vkAllocateCommandBuffers(compose->device, &command, &compose->command);
	if (result != VK_SUCCESS)
		return result;

	/* The frame's fence, exportable as an fd the event loop polls (design D3). */
	memset(&export, 0, sizeof(export));
	export.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
	export.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence.pNext = &export;
	result = vkCreateFence(compose->device, &fence, NULL, &compose->fence);
	if (result != VK_SUCCESS)
		return result;

	/* The acquire semaphore. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	result = vkCreateSemaphore(compose->device, &semaphore, NULL, &compose->acquired);
	return result;
}

/* Creates the render pass for the output's format and the two pipelines. */
static VkResult
compose_pass(
	struct zwl_compose *compose)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo pass;
	VkShaderModuleCreateInfo module;
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkResult result;

	/* One color attachment, cleared to the background and presented. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = compose->format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	memset(&reference, 0, sizeof(reference));
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;

	/* The acquired image is written only after the acquire semaphore. */
	memset(&dependency, 0, sizeof(dependency));
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0U;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass.attachmentCount = 1U;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1U;
	pass.pSubpasses = &subpass;
	pass.dependencyCount = 1U;
	pass.pDependencies = &dependency;
	result = vkCreateRenderPass(compose->device, &pass, NULL, &compose->pass);
	if (result != VK_SUCCESS)
		return result;

	/* The shaders, needed only while the pipelines are created. */
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = sizeof(zwl_quad_vert);
	module.pCode = zwl_quad_vert;
	result = vkCreateShaderModule(compose->device, &module, NULL, &vertex);
	if (result != VK_SUCCESS)
		return result;
	module.codeSize = sizeof(zwl_quad_frag);
	module.pCode = zwl_quad_frag;
	result = vkCreateShaderModule(compose->device, &module, NULL, &fragment);
	if (result != VK_SUCCESS) {
		vkDestroyShaderModule(compose->device, vertex, NULL);
		return result;
	}

	/* The opaque and the alpha pipelines. */
	result = compose_pipeline(compose, ZWL_DRAW_OPAQUE, vertex, fragment);
	if (result == VK_SUCCESS)
		result = compose_pipeline(compose, ZWL_DRAW_ALPHA, vertex, fragment);
	vkDestroyShaderModule(compose->device, vertex, NULL);
	vkDestroyShaderModule(compose->device, fragment, NULL);
	return result;
}

/* Creates the pipeline that draws a quad the given way. */
static VkResult
compose_pipeline(
	struct zwl_compose *compose,
	enum zwl_draw draw,
	VkShaderModule vertex,
	VkShaderModule fragment)
{
	static const VkDynamicState dynamic[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineShaderStageCreateInfo stages[2];
	VkPipelineVertexInputStateCreateInfo input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic_state;
	VkGraphicsPipelineCreateInfo pipeline;

	/* The two shader stages. */
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment;
	stages[1].pName = "main";

	/* No vertex buffers: the shader makes the strip's four corners. */
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

	/* The viewport and scissor are set per frame. */
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
	 * Opaque windows replace what is under them; the alpha pipeline blends
	 * premultiplied colors (Wayland's convention for images with alpha).
	 */
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if (draw == ZWL_DRAW_ALPHA) {
		blend_attachment.blendEnable = VK_TRUE;
		blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
		blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}

	/* One attachment's blending, and the viewport and scissor left to the frame. */
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1U;
	blend.pAttachments = &blend_attachment;
	memset(&dynamic_state, 0, sizeof(dynamic_state));
	dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.dynamicStateCount = 2U;
	dynamic_state.pDynamicStates = dynamic;

	/* The pipeline. */
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
	pipeline.layout = compose->layout;
	pipeline.renderPass = compose->pass;
	return vkCreateGraphicsPipelines(compose->device, VK_NULL_HANDLE, 1U, &pipeline, NULL, &compose->pipelines[draw]);
}

/* Creates a view, a framebuffer and a present semaphore for each swapchain image. */
static VkResult
compose_targets(
	struct zwl_compose *compose)
{
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	VkSemaphoreCreateInfo semaphore;
	uint32_t index;
	VkResult result;

	/* Each image in turn. */
	for (index = 0; index < compose->output.image_count; index++) {
		memset(&view, 0, sizeof(view));
		view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.image = compose->output.images[index];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = compose->format;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.levelCount = 1U;
		view.subresourceRange.layerCount = 1U;
		result = vkCreateImageView(compose->device, &view, NULL, &compose->views[index]);
		if (result != VK_SUCCESS)
			return result;

		/* Its framebuffer of the output's size. */
		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer.renderPass = compose->pass;
		framebuffer.attachmentCount = 1U;
		framebuffer.pAttachments = &compose->views[index];
		framebuffer.width = compose->output.width;
		framebuffer.height = compose->output.height;
		framebuffer.layers = 1U;
		result = vkCreateFramebuffer(compose->device, &framebuffer, NULL, &compose->framebuffers[index]);
		if (result != VK_SUCCESS)
			return result;

		/* The semaphore its present waits for. */
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		result = vkCreateSemaphore(compose->device, &semaphore, NULL, &compose->rendered[index]);
		if (result != VK_SUCCESS)
			return result;
	}

	/* Succeeded. */
	return VK_SUCCESS;
}

/* Destroys the per-image views, framebuffers and semaphores. */
static void
compose_targets_destroy(
	struct zwl_compose *compose)
{
	uint32_t index;

	/* Each image's objects, those that were made. */
	for (index = 0; index < ZWL_SWAPCHAIN_MAX; index++) {
		if (compose->framebuffers[index] != VK_NULL_HANDLE)
			vkDestroyFramebuffer(compose->device, compose->framebuffers[index], NULL);
		if (compose->views[index] != VK_NULL_HANDLE)
			vkDestroyImageView(compose->device, compose->views[index], NULL);
		if (compose->rendered[index] != VK_NULL_HANDLE)
			vkDestroySemaphore(compose->device, compose->rendered[index], NULL);
		compose->framebuffers[index] = VK_NULL_HANDLE;
		compose->views[index] = VK_NULL_HANDLE;
		compose->rendered[index] = VK_NULL_HANDLE;
	}
}

/*
 * Collects the windows to draw, bottom to top: mapped surfaces of live
 * clients with a current image that window mode can sample.
 */
static unsigned
compose_windows(
	struct zwl_server *server,
	struct zwl_object **windows,
	unsigned capacity)
{
	const struct zwl_import *image;
	struct zwl_client *client;
	struct zwl_object *surface;
	unsigned count;
	unsigned index;
	unsigned at;

	/* Every drawable window. */
	count = 0;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			if (surface->kind != ZWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->current == NULL)
				continue;

			/* A window whose image is not made yet waits. */
			image = surface_image(surface);
			if (image == NULL)
				continue;

			/* Insertion by map order keeps the list bottom to top. */
			if (count == capacity)
				break;
			at = count;
			while (at > 0 && windows[at - 1]->map_order > surface->map_order)
				at--;
			for (index = count; index > at; index--)
				windows[index] = windows[index - 1];
			windows[at] = surface;
			count++;
		}
	}

	/* Succeeded: the windows to draw. */
	return count;
}

/*
 * Returns the image window mode samples for a surface: its GPU buffer's
 * import, or its copy of a wl_shm image; NULL when there is none yet.
 */
static const struct zwl_import *
surface_image(
	const struct zwl_object *surface)
{
	/* A wl_shm buffer is drawn from the surface's copy. */
	if (surface->current == NULL)
		return NULL;
	if (surface->current->shm != NULL)
		return surface->shm_image;

	/* A GPU buffer from its own import. */
	return surface->current->import;
}

/* Draws an image as a quad at a place on the output, its own size. */
static void
compose_quad(
	struct zwl_server *server,
	VkCommandBuffer command,
	const struct zwl_import *import,
	int32_t x,
	int32_t y)
{
	float constants[8];
	float width;
	float height;

	/* The rectangle in normalized device coordinates, and the whole image. */
	width = (float)server->width;
	height = (float)server->height;
	constants[0] = 2.0f * (float)x / width - 1.0f;
	constants[1] = 2.0f * (float)y / height - 1.0f;
	constants[2] = 2.0f * (float)(x + (int32_t)import->width) / width - 1.0f;
	constants[3] = 2.0f * (float)(y + (int32_t)import->height) / height - 1.0f;
	constants[4] = 0.0f;
	constants[5] = 0.0f;
	constants[6] = 1.0f;
	constants[7] = 1.0f;

	/* The pipeline for the window's way of drawing, its image, and the strip. */
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, server->compose->pipelines[import->draw]);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, server->compose->layout, 0U, 1U, &import->set, 0U, NULL);
	vkCmdPushConstants(command, server->compose->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(constants), constants);
	vkCmdDraw(command, 4U, 1U, 0U, 0U);
}

/* Records a frame: the background, then each window from the bottom. */
static VkResult
compose_record(
	struct zwl_server *server,
	uint32_t image,
	struct zwl_object **windows,
	unsigned count)
{
	struct zwl_compose *compose;
	VkCommandBufferBeginInfo begin;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	unsigned index;
	VkResult result;

	/* A fresh recording. */
	compose = server->compose;
	result = vkResetCommandBuffer(compose->command, 0U);
	if (result != VK_SUCCESS)
		return result;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(compose->command, &begin);
	if (result != VK_SUCCESS)
		return result;

	/* The pass clears the image to the background. */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = ZWL_BACKGROUND_RED;
	clear.color.float32[1] = ZWL_BACKGROUND_GREEN;
	clear.color.float32[2] = ZWL_BACKGROUND_BLUE;
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = compose->pass;
	pass.framebuffer = compose->framebuffers[image];
	pass.renderArea.extent.width = compose->output.width;
	pass.renderArea.extent.height = compose->output.height;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(compose->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The whole output is drawn (the first implementation, design D4). */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)compose->output.width;
	viewport.height = (float)compose->output.height;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(compose->command, 0U, 1U, &viewport);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = compose->output.width;
	scissor.extent.height = compose->output.height;
	vkCmdSetScissor(compose->command, 0U, 1U, &scissor);

	/* The windows, bottom to top (painter's order), then the cursor over them. */
	for (index = 0; index < count; index++)
		compose_quad(server, compose->command, surface_image(windows[index]), windows[index]->x, windows[index]->y);
	compose_cursor(server, compose->command);
	vkCmdEndRenderPass(compose->command);

	/* The recording is complete. */
	return vkEndCommandBuffer(compose->command);
}

/*
 * Draws the cursor at the pointer: the client's cursor surface (its hotspot
 * at the pointer), or zdesktop's arrow (its tip), with alpha; nothing when
 * hidden (design D8).
 */
static void
compose_cursor(
	struct zwl_server *server,
	VkCommandBuffer command)
{
	const struct zwl_import *image;
	struct zwl_object *surface;
	struct zwl_import alpha;

	/* A hidden cursor is not drawn. */
	if (server->cursor_hidden)
		return;

	/* The client's surface, when it has an image. */
	surface = server->cursor_surface;
	if (surface != NULL && !surface->dead) {
		image = surface_image(surface);
		if (image == NULL)
			return;
		alpha = *image;
		alpha.draw = ZWL_DRAW_ALPHA;
		compose_quad(server, command, &alpha, server->pointer_x - server->cursor_hotspot_x, server->pointer_y - server->cursor_hotspot_y);
		return;
	}

	/* Otherwise the arrow, its tip at the pointer. */
	if (server->arrow != NULL)
		compose_quad(server, command, server->arrow, server->pointer_x, server->pointer_y);
}

/* Submits the frame, presents it, and exports its fence as the fd the event loop polls. */
static VkResult
compose_submit(
	struct zwl_server *server,
	uint32_t image)
{
	struct zwl_compose *compose;
	VkPipelineStageFlags stage;
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkFenceGetFdInfoKHR fd_info;
	VkResult result;
	int fd;

	/* The commands wait for the acquired image and signal its present semaphore. */
	compose = server->compose;
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &compose->acquired;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &compose->command;
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &compose->rendered[image];
	result = vkQueueSubmit(compose->queue, 1U, &submit, compose->fence);
	if (result != VK_SUCCESS)
		return result;

	/* The image goes to the display after the commands. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &compose->rendered[image];
	present.swapchainCount = 1U;
	present.pSwapchains = &compose->output.swapchain;
	present.pImageIndices = &image;
	result = vkQueuePresentKHR(compose->queue, &present);
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		return result;

	/* The fence as an fd: the event loop learns of completion without waiting (design D3). */
	memset(&fd_info, 0, sizeof(fd_info));
	fd_info.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
	fd_info.fence = compose->fence;
	fd_info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
	fd = -1;
	result = vkGetFenceFdKHR(compose->device, &fd_info, &fd);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the frame is in flight. */
	server->frame_fd = fd;
	compose->in_flight = 1;
	return VK_SUCCESS;
}

/*
 * Holds the buffers a frame sampled, and takes the frame callbacks of the
 * surfaces it showed, until the frame completes.
 */
static void
compose_hold(
	struct zwl_server *server,
	struct zwl_object **windows,
	unsigned count)
{
	struct zwl_compose *compose;
	struct zwl_object *surface;
	struct zwl_object **tail;
	unsigned index;

	/* Each window's current buffer is held by the frame. */
	compose = server->compose;
	for (index = 0; index < count; index++) {
		zwl_buffer_get(windows[index]->current);
		compose->held[compose->held_count++] = windows[index]->current;
		windows[index]->fresh = 0;

		/* A window with frame callbacks is awaited after the frame (frame pacing). */
		if (windows[index]->committed_callbacks != NULL && !windows[index]->awaited) {
			windows[index]->awaited = 1;
			server->awaiting++;
		}

		/* Its frame callbacks wait for the frame, in request order. */
		tail = &compose->callbacks;
		while (*tail != NULL)
			tail = &(*tail)->callback_next;
		*tail = windows[index]->committed_callbacks;
		windows[index]->committed_callbacks = NULL;
	}

	/* A client's cursor surface is held and told too. */
	surface = server->cursor_surface;
	if (surface != NULL && !surface->dead && surface->current != NULL) {
		zwl_buffer_get(surface->current);
		compose->held[compose->held_count++] = surface->current;

		/* Its frame callbacks after the windows'. */
		tail = &compose->callbacks;
		while (*tail != NULL)
			tail = &(*tail)->callback_next;
		*tail = surface->committed_callbacks;
		surface->committed_callbacks = NULL;
	}
}
