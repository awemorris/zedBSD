/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU renderer: a display list drawn with one Vulkan pipeline.
 *
 * Every rectangle, glyph and image is one instance of the unit square: its
 * exact rectangle, its color and, for a glyph or an image, its place in the
 * atlas (an image also with where it starts and its texels to a pixel).  The
 * vertex shader stretches the square over the pixels the rectangle touches
 * and the fragment shader weighs each pixel by the CPU renderer's coverage
 * rule, so the blend (straight alpha, over the canvas color the pass clears
 * to) gives the CPU renderer's picture.  The instances are drawn in the
 * list's order in one call, and Vulkan blends them in that order.
 */

#include "paint/gpu.h"
#include "paint/shaders.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* How long a frame may take on the GPU, in nanoseconds. */
#define GPU_TIMEOUT		10000000000ULL

/* The unit square: two triangles of six corners, four floats each; each item has its own six vertices. */
_Static_assert(PAINT_GPU_ATLAS_SIZE == 2048U, "paint/shaders/display.frag names the atlas size");

#define GPU_CORNERS		6U

/* The fewest instances the instance buffer is made for. */
#define GPU_INSTANCES_MIN	4096U

/* The size of the atlas's table of glyphs when the first glyph arrives. */
#define GPU_SLOTS_MIN		1024U

/* The kinds of instance, as the fragment shader reads them. */
#define GPU_KIND_RECT		0.0f
#define GPU_KIND_GLYPH		1.0f
#define GPU_KIND_IMAGE		2.0f
#define GPU_KIND_VERTICAL_GLYPH	3.0f
#define GPU_KIND_UP_GLYPH	4.0f

/*
 * One instance: the exact rectangle in pixels (left, top, right, bottom),
 * the straight color (red, green, blue, alpha from 0 to 1), the atlas
 * place with the kind (u, v in texels, kind, unused), and for an image
 * the unclipped rectangle's top left with its texels to a pixel (x, y,
 * scale x, scale y) and its size in texels (width, height, unused,
 * unused).
 */
struct gpu_instance {
	float rect[4];
	float color[4];
	float atlas[4];
	float source[4];
	float texels[4];
};

/*
 * One vertex: a corner of the unit square (the fourth component is unused)
 * and the item it belongs to, repeated at each of the item's six vertices
 * (the Raspberry Pi's V3D takes no instance-rate input).
 */
struct gpu_vertex {
	float corner[4];
	struct gpu_instance item;
};

/*
 * One image placed in the atlas: the bitmap's serial (the key) and where
 * its pixels went.
 */
struct paint_gpu_image {
	uint64_t serial;
	uint32_t x;
	uint32_t y;
};

/*
 * One glyph placed in the atlas: the bitmap it was copied from (the key,
 * which the text system keeps for its life) and where it went.
 */
struct paint_gpu_slot {
	const uint8_t *bitmap;
	uint32_t x;
	uint32_t y;
};

static VkResult gpu_memory(VkPhysicalDevice physical, VkDevice device, const VkMemoryRequirements *requirements, VkMemoryPropertyFlags wanted, VkDeviceMemory *memory, const char **operation);
static VkResult gpu_pass(struct paint_gpu *gpu, VkImageLayout final_layout);
static VkResult gpu_descriptors(struct paint_gpu *gpu);
static VkResult gpu_atlas(struct paint_gpu *gpu);
static void gpu_vertices(struct paint_gpu *gpu);
static VkResult gpu_instances(struct paint_gpu *gpu, size_t count);
static void gpu_instances_free(struct paint_gpu *gpu);
static VkResult gpu_pipeline(struct paint_gpu *gpu);
static VkResult gpu_module(struct paint_gpu *gpu, const uint32_t *code, size_t size, VkShaderModule *module);
static VkResult gpu_commands(struct paint_gpu *gpu);
static int gpu_stage(struct paint_gpu *gpu, const struct paint_list *list, struct text_system *text, layout_unit scroll_y, VkExtent2D extent);
static int gpu_stage_rect(struct paint_gpu *gpu, const struct paint_item *item, layout_unit scroll_y, VkExtent2D extent, const struct paint_clip *clip);
static int gpu_stage_text(struct paint_gpu *gpu, const struct paint_item *item, struct text_system *text, layout_unit scroll_y, VkExtent2D extent, const struct paint_clip *clip);
static int gpu_stage_image(struct paint_gpu *gpu, const struct paint_item *item, layout_unit scroll_y, VkExtent2D extent, const struct paint_clip *clip);
static int gpu_place(struct paint_gpu *gpu, const struct text_glyph *glyph, uint32_t *x, uint32_t *y);
static int gpu_place_image(struct paint_gpu *gpu, const struct img_bitmap *image, uint32_t *x, uint32_t *y);
static int gpu_shelf(struct paint_gpu *gpu, uint32_t width, uint32_t height, uint32_t *x, uint32_t *y);
static struct paint_gpu_slot *gpu_slot(struct paint_gpu *gpu, const uint8_t *bitmap);
static int gpu_slots_grow(struct paint_gpu *gpu);
static void gpu_atlas_reset(struct paint_gpu *gpu);
static void gpu_color(uint32_t color, float *out);
static void gpu_record(struct paint_gpu *gpu, VkCommandBuffer commands, VkFramebuffer framebuffer, VkExtent2D extent, uint32_t canvas);
static VkResult gpu_submit_commands(VkDevice device, VkQueue queue, VkCommandBuffer command, VkFence fence, VkSemaphore wait, VkSemaphore signal, const char **operation);
static VkResult offscreen_device(struct paint_offscreen *offscreen);
static VkResult offscreen_image(struct paint_offscreen *offscreen);
static VkResult offscreen_commands(struct paint_offscreen *offscreen);

/*
 * Makes the renderer's objects on a device: the pass for a color format
 * that ends in a layout, the pipeline, the atlas and the buffers.
 */
VkResult
paint_gpu_open(
	struct paint_gpu *gpu,
	VkInstance instance,
	VkPhysicalDevice physical,
	uint32_t family,
	VkDevice device,
	VkFormat format,
	VkImageLayout final_layout)
{
	VkResult error;

	/* Nothing is owned yet but the device's handles the caller lends. */
	memset(gpu, 0, sizeof(*gpu));
	gpu->instance = instance;
	gpu->physical = physical;
	gpu->family = family;
	gpu->device = device;
	gpu->format = format;
	vkGetDeviceQueue(device, family, 0U, &gpu->queue);
	wb_vector_init(&gpu->staged, sizeof(struct gpu_instance));
	wb_vector_init(&gpu->images, sizeof(struct paint_gpu_image));

	/* The pass that draws into the caller's images. */
	error = gpu_pass(gpu, final_layout);
	if (error != VK_SUCCESS)
		return error;

	/* The sampler and the set the atlas is bound through. */
	error = gpu_descriptors(gpu);
	if (error != VK_SUCCESS)
		return error;

	/* The atlas. */
	error = gpu_atlas(gpu);
	if (error != VK_SUCCESS)
		return error;

	/* Room for a first page's instances. */
	error = gpu_instances(gpu, GPU_INSTANCES_MIN);
	if (error != VK_SUCCESS)
		return error;

	/* The pipeline. */
	error = gpu_pipeline(gpu);
	if (error != VK_SUCCESS)
		return error;

	/* The command buffer and its fence. */
	error = gpu_commands(gpu);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: display lists can be drawn. */
	return VK_SUCCESS;
}

/*
 * Makes a frame ready to record: a display list, scrolled up by scroll_y,
 * turned into the instances of a target of an extent, with any new glyph
 * or image copied into the atlas.  The last frame that used the atlas and
 * the instances must have finished on the GPU.
 */
VkResult
paint_gpu_prepare(
	struct paint_gpu *gpu,
	const struct paint_list *list,
	struct text_system *text,
	layout_unit scroll_y,
	VkExtent2D extent)
{
	int status;
	VkResult error;

	/* An atlas that filled up in the last frame, or whose glyphs were forgotten, starts over. */
	if (gpu->full)
		gpu_atlas_reset(gpu);

	/* The frame's instances, with any new glyph copied into the atlas. */
	status = gpu_stage(gpu, list, text, scroll_y, extent);
	if (status != 0) {
		gpu->operation = "staging the display list";
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* A buffer large enough for them. */
	if (gpu->staged.count > gpu->instance_capacity) {
		error = gpu_instances(gpu, gpu->staged.count * 2U);
		if (error != VK_SUCCESS)
			return error;
	}

	/* The instances into the buffer. */
	if (gpu->staged.count != 0)
		gpu_vertices(gpu);

	/* Succeeded: the frame can be recorded. */
	return VK_SUCCESS;
}

/*
 * Records the frame paint_gpu_prepare made ready into a command buffer
 * the caller began, drawing into a framebuffer of the renderer's pass over
 * the list's canvas color; the caller submits it.
 */
void
paint_gpu_record(
	struct paint_gpu *gpu,
	VkCommandBuffer commands,
	const struct paint_list *list,
	VkFramebuffer framebuffer,
	VkExtent2D extent)
{
	uint32_t canvas;

	/* The pass over the canvas color, with the frame's instances. */
	canvas = paint_canvas_pixel(list->canvas_color);
	gpu_record(gpu, commands, framebuffer, extent, canvas);
}

/*
 * Draws a display list, scrolled up by scroll_y, into a framebuffer of an
 * extent made for the renderer's pass, and waits for it to finish.
 *
 * wait (or VK_NULL_HANDLE) is waited for before drawing and signal (or
 * VK_NULL_HANDLE) is signalled after, as a swapchain's acquire and present
 * need.
 */
VkResult
paint_gpu_draw(
	struct paint_gpu *gpu,
	const struct paint_list *list,
	struct text_system *text,
	layout_unit scroll_y,
	VkFramebuffer framebuffer,
	VkExtent2D extent,
	VkSemaphore wait,
	VkSemaphore signal)
{
	VkCommandBufferBeginInfo begin;
	VkResult error;

	/* The frame's instances and atlas. */
	error = paint_gpu_prepare(gpu, list, text, scroll_y, extent);
	if (error != VK_SUCCESS)
		return error;

	/* The renderer's own command buffer, started over for one submission. */
	gpu->operation = "vkResetCommandBuffer";
	error = vkResetCommandBuffer(gpu->command, 0U);
	if (error != VK_SUCCESS)
		return error;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	gpu->operation = "vkBeginCommandBuffer";
	error = vkBeginCommandBuffer(gpu->command, &begin);
	if (error != VK_SUCCESS)
		return error;

	/* Records the frame into it. */
	paint_gpu_record(gpu, gpu->command, list, framebuffer, extent);
	gpu->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(gpu->command);
	if (error != VK_SUCCESS)
		return error;

	/* Submits it and waits for it. */
	error = gpu_submit_commands(gpu->device, gpu->queue, gpu->command, gpu->fence, wait, signal, &gpu->operation);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the framebuffer holds the page. */
	return VK_SUCCESS;
}

/*
 * Forgets the glyphs placed in the atlas (the text system they came from
 * was closed, and its bitmaps' memory may be used again): the atlas starts
 * over at the next frame.
 */
void
paint_gpu_forget_glyphs(
	struct paint_gpu *gpu)
{
	/* The next frame resets the atlas before it stages anything. */
	gpu->full = 1;
}

/*
 * Releases the renderer's objects (not the device, which is the caller's).
 */
void
paint_gpu_close(
	struct paint_gpu *gpu)
{
	/* The device's objects, once nothing runs. */
	if (gpu->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(gpu->device);
		gpu_instances_free(gpu);

		/* The drawing objects. */
		if (gpu->pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(gpu->device, gpu->pipeline, NULL);
		if (gpu->layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(gpu->device, gpu->layout, NULL);
		if (gpu->descriptor_pool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(gpu->device, gpu->descriptor_pool, NULL);
		if (gpu->set_layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(gpu->device, gpu->set_layout, NULL);
		if (gpu->sampler != VK_NULL_HANDLE)
			vkDestroySampler(gpu->device, gpu->sampler, NULL);
		if (gpu->pass != VK_NULL_HANDLE)
			vkDestroyRenderPass(gpu->device, gpu->pass, NULL);

		/* The atlas. */
		if (gpu->atlas_view != VK_NULL_HANDLE)
			vkDestroyImageView(gpu->device, gpu->atlas_view, NULL);
		if (gpu->atlas != VK_NULL_HANDLE)
			vkDestroyImage(gpu->device, gpu->atlas, NULL);
		if (gpu->atlas_memory != VK_NULL_HANDLE)
			vkFreeMemory(gpu->device, gpu->atlas_memory, NULL);

		/* The commands and the fence. */
		if (gpu->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(gpu->device, gpu->pool, NULL);
		if (gpu->fence != VK_NULL_HANDLE)
			vkDestroyFence(gpu->device, gpu->fence, NULL);
	}

	/* The host's tables. */
	wb_vector_release(&gpu->staged);
	wb_vector_release(&gpu->images);
	free(gpu->slots);
	memset(gpu, 0, sizeof(*gpu));
}

/*
 * Makes a device and an image of the renderer's own, width by height, for
 * drawing without a window: the caller draws into the image (in the
 * transfer source layout when it is done) and reads it back.
 *
 * On failure offscreen->operation names the Vulkan call that failed, and
 * paint_offscreen_close releases what was made.
 */
VkResult
paint_offscreen_open(
	struct paint_offscreen *offscreen,
	uint32_t width,
	uint32_t height)
{
	VkResult error;

	/* Nothing is owned yet. */
	memset(offscreen, 0, sizeof(*offscreen));
	offscreen->format = VK_FORMAT_B8G8R8A8_UNORM;
	offscreen->extent.width = width;
	offscreen->extent.height = height;

	/* The instance, the device and its queue. */
	error = offscreen_device(offscreen);
	if (error != VK_SUCCESS)
		return error;

	/* The image with its memory and its view. */
	error = offscreen_image(offscreen);
	if (error != VK_SUCCESS)
		return error;

	/* The command buffer and the fence of the read back. */
	error = offscreen_commands(offscreen);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the image can be drawn into. */
	return VK_SUCCESS;
}

/*
 * Copies the offscreen image (drawn, in the transfer source layout) into
 * 0xAARRGGBB pixels whose rows are stride bytes apart, through a
 * host-visible buffer.
 */
VkResult
paint_offscreen_read(
	struct paint_offscreen *offscreen,
	uint32_t *pixels,
	size_t stride)
{
	VkBufferCreateInfo buffer_info;
	VkMemoryRequirements requirements;
	VkCommandBufferBeginInfo begin;
	VkBufferImageCopy region;
	VkBufferMemoryBarrier barrier;
	VkBuffer buffer;
	VkDeviceMemory memory;
	const unsigned char *texel;
	uint32_t *row_start;
	uint32_t row;
	uint32_t column;
	void *map;
	VkResult error;

	/* The buffer the image is copied into: four bytes a pixel, rows packed. */
	buffer = VK_NULL_HANDLE;
	memory = VK_NULL_HANDLE;
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = (VkDeviceSize)offscreen->extent.width * (VkDeviceSize)offscreen->extent.height * 4U;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	offscreen->operation = "vkCreateBuffer";
	error = vkCreateBuffer(offscreen->device, &buffer_info, NULL, &buffer);

	/* Its memory, which the host reads. */
	if (error == VK_SUCCESS) {
		vkGetBufferMemoryRequirements(offscreen->device, buffer, &requirements);
		error = gpu_memory(
			offscreen->physical,
			offscreen->device,
			&requirements,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&memory,
			&offscreen->operation);
	}

	/* Binds the memory to the buffer. */
	if (error == VK_SUCCESS) {
		offscreen->operation = "vkBindBufferMemory";
		error = vkBindBufferMemory(offscreen->device, buffer, memory, 0U);
	}

	/* Starts the command buffer over. */
	if (error == VK_SUCCESS) {
		offscreen->operation = "vkResetCommandBuffer";
		error = vkResetCommandBuffer(offscreen->command, 0U);
	}

	/* Records the copy and the host's read after it. */
	if (error == VK_SUCCESS) {
		memset(&begin, 0, sizeof(begin));
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		(void)vkBeginCommandBuffer(offscreen->command, &begin);
		memset(&region, 0, sizeof(region));
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1U;
		region.imageExtent.width = offscreen->extent.width;
		region.imageExtent.height = offscreen->extent.height;
		region.imageExtent.depth = 1U;
		vkCmdCopyImageToBuffer(offscreen->command, offscreen->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1U, &region);
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer = buffer;
		barrier.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(offscreen->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, NULL, 1U, &barrier, 0U, NULL);
		offscreen->operation = "vkEndCommandBuffer";
		error = vkEndCommandBuffer(offscreen->command);
	}

	/* Runs the copy and waits for it. */
	if (error == VK_SUCCESS) {
		error = gpu_submit_commands(
			offscreen->device,
			offscreen->queue,
			offscreen->command,
			offscreen->fence,
			VK_NULL_HANDLE,
			VK_NULL_HANDLE,
			&offscreen->operation);
	}

	/* Maps the copy. */
	map = NULL;
	if (error == VK_SUCCESS) {
		offscreen->operation = "vkMapMemory";
		error = vkMapMemory(offscreen->device, memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	}

	/* Each pixel's blue, green, red and alpha bytes become one 0xAARRGGBB word in the caller's row. */
	if (error == VK_SUCCESS) {
		texel = map;
		for (row = 0U; row < offscreen->extent.height; row++) {
			row_start = (uint32_t *)(void *)((unsigned char *)pixels + (size_t)row * stride);
			for (column = 0U; column < offscreen->extent.width; column++) {
				row_start[column] = (uint32_t)texel[3] << 24;
				row_start[column] |= (uint32_t)texel[2] << 16;
				row_start[column] |= (uint32_t)texel[1] << 8;
				row_start[column] |= (uint32_t)texel[0];
				texel += 4;
			}
		}

		/* The host is done reading. */
		vkUnmapMemory(offscreen->device, memory);
	}

	/* The buffer and its memory go. */
	if (buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(offscreen->device, buffer, NULL);
	if (memory != VK_NULL_HANDLE)
		vkFreeMemory(offscreen->device, memory, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pixels hold the picture. */
	return VK_SUCCESS;
}

/*
 * Releases the offscreen image, its device and its instance, children
 * before their parents.
 */
void
paint_offscreen_close(
	struct paint_offscreen *offscreen)
{
	/* The device's objects, once nothing runs. */
	if (offscreen->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(offscreen->device);

		/* The read back's commands and fence. */
		if (offscreen->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(offscreen->device, offscreen->pool, NULL);
		if (offscreen->fence != VK_NULL_HANDLE)
			vkDestroyFence(offscreen->device, offscreen->fence, NULL);

		/* The image, its view and its memory. */
		if (offscreen->view != VK_NULL_HANDLE)
			vkDestroyImageView(offscreen->device, offscreen->view, NULL);
		if (offscreen->image != VK_NULL_HANDLE)
			vkDestroyImage(offscreen->device, offscreen->image, NULL);
		if (offscreen->memory != VK_NULL_HANDLE)
			vkFreeMemory(offscreen->device, offscreen->memory, NULL);

		/* The device. */
		vkDestroyDevice(offscreen->device, NULL);
	}

	/* The instance. */
	if (offscreen->instance != VK_NULL_HANDLE)
		vkDestroyInstance(offscreen->instance, NULL);

	/* Nothing is owned any more. */
	memset(offscreen, 0, sizeof(*offscreen));
}

/* Allocates memory of the first allowed type that has the wanted properties. */
static VkResult
gpu_memory(
	VkPhysicalDevice physical,
	VkDevice device,
	const VkMemoryRequirements *requirements,
	VkMemoryPropertyFlags wanted,
	VkDeviceMemory *memory,
	const char **operation)
{
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryAllocateInfo allocate;
	uint32_t index;
	VkResult error;

	/* The first allowed type with every wanted property. */
	vkGetPhysicalDeviceMemoryProperties(physical, &properties);
	for (index = 0U; index < properties.memoryTypeCount; index++) {
		/* A type the resource cannot live in. */
		if ((requirements->memoryTypeBits & (1U << index)) == 0U)
			continue;

		/* A type with the properties wanted. */
		if ((properties.memoryTypes[index].propertyFlags & wanted) == wanted)
			break;
	}

	/* No type fits. */
	if (index == properties.memoryTypeCount) {
		*operation = "choosing a memory type";
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	/* The allocation. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements->size;
	allocate.memoryTypeIndex = index;
	*operation = "vkAllocateMemory";
	error = vkAllocateMemory(device, &allocate, NULL, memory);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the memory. */
	return VK_SUCCESS;
}

/* Makes the pass: one color attachment, cleared to the canvas, left in the final layout. */
static VkResult
gpu_pass(
	struct paint_gpu *gpu,
	VkImageLayout final_layout)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo create;
	VkResult error;

	/* The target, cleared at the start and kept at the end. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = gpu->format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = final_layout;

	/* The one subpass. */
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;

	/* The drawing finishes before a copy out of the image starts. */
	memset(&dependency, 0, sizeof(dependency));
	dependency.srcSubpass = 0U;
	dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	/* The pass. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = 1U;
	create.pAttachments = &attachment;
	create.subpassCount = 1U;
	create.pSubpasses = &subpass;
	create.dependencyCount = 1U;
	create.pDependencies = &dependency;
	gpu->operation = "vkCreateRenderPass";
	error = vkCreateRenderPass(gpu->device, &create, NULL, &gpu->pass);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pass. */
	return VK_SUCCESS;
}

/* Makes the sampler, the set layout, the pool and the one set the atlas is bound through. */
static VkResult
gpu_descriptors(
	struct paint_gpu *gpu)
{
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkResult error;

	/* Nearest sampling (the shader fetches texels by their index). */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	gpu->operation = "vkCreateSampler";
	error = vkCreateSampler(gpu->device, &sampler, NULL, &gpu->sampler);
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
	gpu->operation = "vkCreateDescriptorSetLayout";
	error = vkCreateDescriptorSetLayout(gpu->device, &set_layout, NULL, &gpu->set_layout);
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
	gpu->operation = "vkCreateDescriptorPool";
	error = vkCreateDescriptorPool(gpu->device, &pool, NULL, &gpu->descriptor_pool);
	if (error != VK_SUCCESS)
		return error;

	/* The set, pointed at the atlas once it exists. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = gpu->descriptor_pool;
	allocate.descriptorSetCount = 1U;
	allocate.pSetLayouts = &gpu->set_layout;
	gpu->operation = "vkAllocateDescriptorSets";
	error = vkAllocateDescriptorSets(gpu->device, &allocate, &gpu->set);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the atlas can be bound. */
	return VK_SUCCESS;
}

/* Makes the atlas (linear, host-written, mapped for good, cleared) and binds it to the set. */
static VkResult
gpu_atlas(
	struct paint_gpu *gpu)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkImageViewCreateInfo view;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	uint32_t row;
	void *map;
	VkResult error;

	/* The image: linear so the host writes its rows, sampled by the fragment shader. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_B8G8R8A8_UNORM;
	image.extent.width = PAINT_GPU_ATLAS_SIZE;
	image.extent.height = PAINT_GPU_ATLAS_SIZE;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_LINEAR;
	image.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	gpu->operation = "vkCreateImage";
	error = vkCreateImage(gpu->device, &image, NULL, &gpu->atlas);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, which the host sees and keeps coherent. */
	vkGetImageMemoryRequirements(gpu->device, gpu->atlas, &requirements);
	error = gpu_memory(gpu->physical, gpu->device, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &gpu->atlas_memory, &gpu->operation);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the image. */
	gpu->operation = "vkBindImageMemory";
	error = vkBindImageMemory(gpu->device, gpu->atlas, gpu->atlas_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps it for the host's writes, for good. */
	gpu->operation = "vkMapMemory";
	error = vkMapMemory(gpu->device, gpu->atlas_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (error != VK_SUCCESS)
		return error;

	/* Where the rows start and how far apart they are. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(gpu->device, gpu->atlas, &subresource, &layout);
	gpu->atlas_map = (unsigned char *)map + layout.offset;
	gpu->atlas_pitch = (size_t)layout.rowPitch;
	gpu->atlas_ready = 0;

	/* Clears it: no glyph is placed yet. */
	for (row = 0U; row < PAINT_GPU_ATLAS_SIZE; row++)
		memset(gpu->atlas_map + (size_t)row * gpu->atlas_pitch, 0, (size_t)PAINT_GPU_ATLAS_SIZE * 4U);

	/* The view the shader samples. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = gpu->atlas;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	gpu->operation = "vkCreateImageView";
	error = vkCreateImageView(gpu->device, &view, NULL, &gpu->atlas_view);
	if (error != VK_SUCCESS)
		return error;

	/* The set names the atlas in the general layout it is kept in. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = gpu->sampler;
	image_info.imageView = gpu->atlas_view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = gpu->set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(gpu->device, 1U, &write, 0U, NULL);

	/* Succeeded: glyphs can be copied into the atlas. */
	return VK_SUCCESS;
}

/* Writes the staged items into the buffer, six vertices each. */
static void
gpu_vertices(
	struct paint_gpu *gpu)
{
	static const float square[GPU_CORNERS][2] = {
		{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f},
		{1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}
	};
	const struct gpu_instance *items;
	struct gpu_vertex *vertex;
	size_t index;
	size_t corner;

	items = gpu->staged.items;
	vertex = gpu->instance_map;
	for (index = 0; index < gpu->staged.count; index++) {
		for (corner = 0; corner < GPU_CORNERS; corner++) {
			vertex->corner[0] = square[corner][0];
			vertex->corner[1] = square[corner][1];
			vertex->corner[2] = 0.0f;
			vertex->corner[3] = 0.0f;
			vertex->item = items[index];
			vertex++;
		}
	}
}

/* Makes the instance buffer for a number of instances, replacing the old one. */
static VkResult
gpu_instances(
	struct paint_gpu *gpu,
	size_t count)
{
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkResult error;

	/* The old buffer goes once nothing uses it. */
	if (gpu->instances != VK_NULL_HANDLE) {
		gpu->operation = "vkDeviceWaitIdle";
		error = vkDeviceWaitIdle(gpu->device);
		if (error != VK_SUCCESS)
			return error;
		gpu_instances_free(gpu);
	}

	/* The buffer. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = (VkDeviceSize)(count * GPU_CORNERS * sizeof(struct gpu_vertex));
	buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	gpu->operation = "vkCreateBuffer";
	error = vkCreateBuffer(gpu->device, &buffer, NULL, &gpu->instances);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory. */
	vkGetBufferMemoryRequirements(gpu->device, gpu->instances, &requirements);
	error = gpu_memory(gpu->physical, gpu->device, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &gpu->instance_memory, &gpu->operation);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the buffer. */
	gpu->operation = "vkBindBufferMemory";
	error = vkBindBufferMemory(gpu->device, gpu->instances, gpu->instance_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps it for good. */
	gpu->operation = "vkMapMemory";
	error = vkMapMemory(gpu->device, gpu->instance_memory, 0U, VK_WHOLE_SIZE, 0U, &gpu->instance_map);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: room for count instances. */
	gpu->instance_capacity = count;
	return VK_SUCCESS;
}

/* Releases the instance buffer and its memory. */
static void
gpu_instances_free(
	struct paint_gpu *gpu)
{
	/* The buffer and the memory, where made. */
	if (gpu->instances != VK_NULL_HANDLE)
		vkDestroyBuffer(gpu->device, gpu->instances, NULL);
	if (gpu->instance_memory != VK_NULL_HANDLE)
		vkFreeMemory(gpu->device, gpu->instance_memory, NULL);

	/* Nothing of it is left. */
	gpu->instances = VK_NULL_HANDLE;
	gpu->instance_memory = VK_NULL_HANDLE;
	gpu->instance_map = NULL;
	gpu->instance_capacity = 0;
}

/* Makes the pipeline: a corner and its item per vertex, premultiplied blending. */
static VkResult
gpu_pipeline(
	struct paint_gpu *gpu)
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
	VkVertexInputBindingDescription bindings[1];
	VkVertexInputAttributeDescription attributes[6];
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

	/* The vertex shader module. */
	error = gpu_module(gpu, paint_display_vert, sizeof(paint_display_vert), &vertex);
	if (error != VK_SUCCESS)
		return error;

	/* The fragment module; the vertex module goes when it cannot be made. */
	error = gpu_module(gpu, paint_display_frag, sizeof(paint_display_frag), &fragment);
	if (error != VK_SUCCESS) {
		vkDestroyShaderModule(gpu->device, vertex, NULL);
		return error;
	}

	/* The layout: the atlas's set and the target's size. */
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push.offset = 0U;
	push.size = 4U * sizeof(float);
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1U;
	layout.pSetLayouts = &gpu->set_layout;
	layout.pushConstantRangeCount = 1U;
	layout.pPushConstantRanges = &push;
	gpu->operation = "vkCreatePipelineLayout";
	error = vkCreatePipelineLayout(gpu->device, &layout, NULL, &gpu->layout);

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

	/* One binding: a vertex is its corner (a vec4) and its item's five vec4. */
	memset(bindings, 0, sizeof(bindings));
	bindings[0].binding = 0U;
	bindings[0].stride = sizeof(struct gpu_vertex);
	bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	/* The corner at location 0, and the item's rectangle, color, atlas place, image source and texels at 1 to 5. */
	memset(attributes, 0, sizeof(attributes));
	for (index = 0U; index < 6U; index++) {
		attributes[index].location = index;
		attributes[index].binding = 0U;
		attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		attributes[index].offset = index * 4U * (uint32_t)sizeof(float);
	}

	/* The input state: that buffer, drawn as a list of triangles. */
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	input.vertexBindingDescriptionCount = 1U;
	input.pVertexBindingDescriptions = bindings;
	input.vertexAttributeDescriptionCount = 6U;
	input.pVertexAttributeDescriptions = attributes;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	/* The viewport and scissor follow the target, set each frame. */
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

	/* The color leaves the shader premultiplied: over the target; the target stays opaque. */
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
		pipeline.layout = gpu->layout;
		pipeline.renderPass = gpu->pass;
		gpu->operation = "vkCreateGraphicsPipelines";
		error = vkCreateGraphicsPipelines(gpu->device, VK_NULL_HANDLE, 1U, &pipeline, NULL, &gpu->pipeline);
	}

	/* The modules are not needed once the pipeline is made. */
	vkDestroyShaderModule(gpu->device, vertex, NULL);
	vkDestroyShaderModule(gpu->device, fragment, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pipeline. */
	return VK_SUCCESS;
}

/* Makes a shader module from SPIR-V words. */
static VkResult
gpu_module(
	struct paint_gpu *gpu,
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
	gpu->operation = "vkCreateShaderModule";
	error = vkCreateShaderModule(gpu->device, &create, NULL, module);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the module. */
	return VK_SUCCESS;
}

/* Makes the command pool and buffer and the frame's fence. */
static VkResult
gpu_commands(
	struct paint_gpu *gpu)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkResult error;

	/* A pool whose one buffer is reset every frame. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = gpu->family;
	gpu->operation = "vkCreateCommandPool";
	error = vkCreateCommandPool(gpu->device, &pool, NULL, &gpu->pool);
	if (error != VK_SUCCESS)
		return error;

	/* The buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = gpu->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	gpu->operation = "vkAllocateCommandBuffers";
	error = vkAllocateCommandBuffers(gpu->device, &command, &gpu->command);
	if (error != VK_SUCCESS)
		return error;

	/* The fence the submission's end signals. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	gpu->operation = "vkCreateFence";
	error = vkCreateFence(gpu->device, &fence, NULL, &gpu->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: one submission at a time can be recorded and waited for. */
	return VK_SUCCESS;
}

/* Turns a display list into the frame's instances; nonzero when the host ran out of memory. */
static int
gpu_stage(
	struct paint_gpu *gpu,
	const struct paint_list *list,
	struct text_system *text,
	layout_unit scroll_y,
	VkExtent2D extent)
{
	const struct paint_item *item;
	struct paint_clips clips;
	size_t index;
	int status;

	/* Starts the frame's instances empty. */
	wb_vector_clear(&gpu->staged);

	/* Each item in painting order, cut to the clips the list starts and ends. */
	paint_clips_init(&clips, (int)extent.width, (int)extent.height);
	for (index = 0; index < list->items.count; index++) {
		item = wb_vector_at(&list->items, index);

		/* A clip starts. */
		if (item->kind == PAINT_CLIP) {
			paint_clips_push(&clips, item, scroll_y);
			continue;
		}

		/* The end of a clip. */
		if (item->kind == PAINT_UNCLIP) {
			paint_clips_pop(&clips);
			continue;
		}

		/* A backdrop filter works on pixels already drawn; the GPU path draws none of it. */
		if (item->kind == PAINT_BACKDROP || item->kind == PAINT_GROUP || item->kind == PAINT_GROUP_END)
			continue;

		/* A rectangle is one instance. */
		if (item->kind == PAINT_RECT) {
			status = gpu_stage_rect(gpu, item, scroll_y, extent, paint_clips_top(&clips));
			if (status != 0)
				return status;
			continue;
		}

		/* An image is one instance. */
		if (item->kind == PAINT_IMAGE) {
			status = gpu_stage_image(gpu, item, scroll_y, extent, paint_clips_top(&clips));
			if (status != 0)
				return status;
			continue;
		}

		/* A text run is one instance a glyph. */
		status = gpu_stage_text(gpu, item, text, scroll_y, extent, paint_clips_top(&clips));
		if (status != 0)
			return status;
	}

	/* Succeeded: the instances are staged. */
	return 0;
}

/* Stages a rectangle, unless it is outside the target. */
static int
gpu_stage_rect(
	struct paint_gpu *gpu,
	const struct paint_item *item,
	layout_unit scroll_y,
	VkExtent2D extent,
	const struct paint_clip *clip)
{
	struct gpu_instance instance;
	int error;

	/* The rectangle in pixels, scrolled, as the CPU renderer measures it. */
	memset(&instance, 0, sizeof(instance));
	instance.rect[0] = floorf(layout_to_px(item->x) + 0.5f);
	instance.rect[1] = floorf(layout_to_px(item->y - scroll_y) + 0.5f);
	instance.rect[2] = floorf(layout_to_px(item->x + item->width) + 0.5f);
	instance.rect[3] = floorf(layout_to_px(item->y - scroll_y + item->height) + 0.5f);

	/* Cut to the clip; a rectangle outside it draws nothing. */
	if (instance.rect[0] < clip->left)
		instance.rect[0] = clip->left;
	if (instance.rect[1] < clip->top)
		instance.rect[1] = clip->top;
	if (instance.rect[2] > clip->right)
		instance.rect[2] = clip->right;
	if (instance.rect[3] > clip->bottom)
		instance.rect[3] = clip->bottom;
	if (instance.rect[2] <= instance.rect[0] || instance.rect[3] <= instance.rect[1])
		return 0;

	/* A rectangle above or below the target draws nothing. */
	if (instance.rect[3] <= 0.0f || instance.rect[1] >= (float)extent.height)
		return 0;

	/* Its color and kind. */
	gpu_color(item->color, instance.color);
	instance.atlas[2] = GPU_KIND_RECT;

	/* Adds it. */
	error = wb_vector_push(&gpu->staged, &instance);
	if (error != 0)
		return error;

	/* Succeeded: the rectangle is staged. */
	return 0;
}

/* Stages each glyph of a text run on whole pixels, copying new glyphs into the atlas. */
static int
gpu_stage_text(
	struct paint_gpu *gpu,
	const struct paint_item *item,
	struct text_system *text,
	layout_unit scroll_y,
	VkExtent2D extent,
	const struct paint_clip *clip)
{
	struct gpu_instance instance;
	struct text_glyph glyph;
	uint32_t atlas_x;
	uint32_t atlas_y;
	int baseline;
	int origin_x;
	int origin_y;
	int left;
	int top;
	int right;
	int bottom;
	size_t index;
	int placed;
	int error;

	/* The baseline on a whole pixel, as the CPU renderer puts it. */
	baseline = (int)floorf(layout_to_px(item->y - scroll_y) + 0.5f);

	/* A run whose line is far outside the target draws nothing. */
	if (!item->vertical && baseline + (int)item->font.pixels * 2 < 0)
		return 0;
	if (!item->vertical && baseline - (int)item->font.pixels * 2 > (int)extent.height)
		return 0;

	/* Each glyph with ink. */
	memset(&instance, 0, sizeof(instance));
	gpu_color(item->color, instance.color);
	instance.atlas[2] = item->vertical == 2 ? GPU_KIND_UP_GLYPH : item->vertical ? GPU_KIND_VERTICAL_GLYPH : GPU_KIND_GLYPH;
	for (index = 0; index < item->glyph_count; index++) {
		error = text_glyph(text, &item->font, item->glyphs[index].code_point, 1, &glyph);
		if (error != 0)
			return error;

		/* A glyph without ink (a space) draws nothing. */
		if (glyph.bitmap == NULL)
			continue;

		/* Its place in the atlas; a glyph the full atlas cannot take is left out of this frame. */
		placed = gpu_place(gpu, &glyph, &atlas_x, &atlas_y);
		if (!placed)
			continue;

		/* The bitmap's top left, from the pen and the baseline, on whole pixels. */
		if (item->vertical == 2) {
			origin_x = (int)floorf(layout_to_px(item->x) + 0.5f) - glyph.top;
			origin_y = baseline -
			    (int)floorf(layout_to_px(item->glyphs[index].x) + 0.5f) - glyph.left - glyph.width;
		} else if (item->vertical) {
			origin_x = (int)floorf(layout_to_px(item->x) + 0.5f) + glyph.top - glyph.height;
			origin_y = baseline +
			    (int)floorf(layout_to_px(item->glyphs[index].x) + 0.5f) + glyph.left;
		} else {
			origin_x = (int)floorf(layout_to_px(item->x + item->glyphs[index].x) + 0.5f) + glyph.left;
			origin_y = baseline - glyph.top;
		}

		/* The glyph's pixels inside the clip (on whole pixels); none inside draws nothing. */
		left = origin_x;
		top = origin_y;
		right = origin_x + (item->vertical ? glyph.height : glyph.width);
		bottom = origin_y + (item->vertical ? glyph.width : glyph.height);
		if (left < clip->pixel_left)
			left = clip->pixel_left;
		if (top < clip->pixel_top)
			top = clip->pixel_top;
		if (right > clip->pixel_right)
			right = clip->pixel_right;
		if (bottom > clip->pixel_bottom)
			bottom = clip->pixel_bottom;
		if (right <= left || bottom <= top)
			continue;

		/* The cut rectangle; sideways glyphs retain the uncut origin for rotation. */
		instance.rect[0] = (float)left;
		instance.rect[1] = (float)top;
		instance.rect[2] = (float)right;
		instance.rect[3] = (float)bottom;
		if (item->vertical) {
			instance.atlas[0] = (float)atlas_x;
			instance.atlas[1] = (float)atlas_y;
			instance.source[0] = (float)origin_x;
			instance.source[1] = (float)origin_y;
			instance.source[2] = (float)(item->vertical == 2 ? glyph.width : glyph.height);
		} else {
			instance.atlas[0] = (float)(atlas_x + (uint32_t)(left - origin_x));
			instance.atlas[1] = (float)(atlas_y + (uint32_t)(top - origin_y));
		}
		error = wb_vector_push(&gpu->staged, &instance);
		if (error != 0)
			return error;
	}

	/* Succeeded: the run's glyphs are staged. */
	return 0;
}

/*
 * Finds a glyph's place in the atlas, copying its bitmap there the first
 * time; zero when the atlas has no room (it starts over next frame).
 */
static int
gpu_place(
	struct paint_gpu *gpu,
	const struct text_glyph *glyph,
	uint32_t *x,
	uint32_t *y)
{
	struct paint_gpu_slot *slot;
	const uint8_t *source;
	uint8_t *target;
	uint32_t width;
	uint32_t height;
	uint32_t left;
	uint32_t top;
	uint32_t row;
	uint32_t column;
	int placed;
	int error;

	/* Grows the table when adding would fill more than half of it. */
	if ((gpu->slot_count + 1U) * 2U > gpu->slot_capacity) {
		error = gpu_slots_grow(gpu);
		if (error != 0)
			return 0;
	}

	/* A glyph placed before is where it was put. */
	slot = gpu_slot(gpu, glyph->bitmap);
	if (slot->bitmap != NULL) {
		*x = slot->x;
		*y = slot->y;
		return 1;
	}

	/* Room on the shelves; a glyph that does not fit waits for the atlas to start over. */
	width = (uint32_t)glyph->width;
	height = (uint32_t)glyph->height;
	placed = gpu_shelf(gpu, width, height, &left, &top);
	if (!placed)
		return 0;

	/* Copies the coverage into every channel of the texels. */
	for (row = 0U; row < height; row++) {
		source = glyph->bitmap + (size_t)row * width;
		target = gpu->atlas_map + (size_t)(top + row) * gpu->atlas_pitch + (size_t)left * 4U;
		for (column = 0U; column < width; column++)
			memset(target + (size_t)column * 4U, source[column], 4U);
	}

	/* Records the place. */
	slot->bitmap = glyph->bitmap;
	slot->x = left;
	slot->y = top;
	gpu->slot_count++;

	/* Succeeded: the glyph is in the atlas. */
	*x = slot->x;
	*y = slot->y;
	return 1;
}

/*
 * Stages an image item as one instance: its rectangle cut to the clip,
 * its place in the atlas, and the unclipped rectangle's top left with the
 * image's texels to a pixel, from which the shader finds the texel under
 * each pixel as the CPU renderer does.
 */
static int
gpu_stage_image(
	struct paint_gpu *gpu,
	const struct paint_item *item,
	layout_unit scroll_y,
	VkExtent2D extent,
	const struct paint_clip *clip)
{
	struct gpu_instance instance;
	uint32_t atlas_x;
	uint32_t atlas_y;
	int placed;
	int error;

	/* The rectangle in pixels, scrolled, as the CPU renderer measures it. */
	memset(&instance, 0, sizeof(instance));
	instance.rect[0] = floorf(layout_to_px(item->x) + 0.5f);
	instance.rect[1] = floorf(layout_to_px(item->y - scroll_y) + 0.5f);
	instance.rect[2] = floorf(layout_to_px(item->x + item->width) + 0.5f);
	instance.rect[3] = floorf(layout_to_px(item->y - scroll_y + item->height) + 0.5f);

	/* Where the image starts and its texels to a pixel, before the cut. */
	instance.source[0] = instance.rect[0];
	instance.source[1] = instance.rect[1];
	instance.source[2] = instance.rect[2] > instance.rect[0] ? (float)item->image->width / (instance.rect[2] - instance.rect[0]) : 0.0f;
	instance.source[3] = instance.rect[3] > instance.rect[1] ? (float)item->image->height / (instance.rect[3] - instance.rect[1]) : 0.0f;
	instance.texels[0] = (float)item->image->width;
	instance.texels[1] = (float)item->image->height;

	/* Cut to the clip; an image outside it draws nothing. */
	if (instance.rect[0] < clip->left)
		instance.rect[0] = clip->left;
	if (instance.rect[1] < clip->top)
		instance.rect[1] = clip->top;
	if (instance.rect[2] > clip->right)
		instance.rect[2] = clip->right;
	if (instance.rect[3] > clip->bottom)
		instance.rect[3] = clip->bottom;
	if (instance.rect[2] <= instance.rect[0] || instance.rect[3] <= instance.rect[1])
		return 0;

	/* An image above or below the target draws nothing. */
	if (instance.rect[3] <= 0.0f || instance.rect[1] >= (float)extent.height)
		return 0;

	/* Its pixels in the atlas; an image the atlas cannot take is left out of this frame. */
	placed = gpu_place_image(gpu, item->image, &atlas_x, &atlas_y);
	if (!placed)
		return 0;
	instance.atlas[0] = (float)atlas_x;
	instance.atlas[1] = (float)atlas_y;
	instance.atlas[2] = GPU_KIND_IMAGE;
	instance.color[3] = 1.0f;

	/* Adds it. */
	error = wb_vector_push(&gpu->staged, &instance);
	if (error != 0)
		return error;

	/* Succeeded: the image is staged. */
	return 0;
}

/*
 * Finds an image's place in the atlas, copying its pixels there the first
 * time; 0 when it does not fit (the atlas then starts over before the next
 * frame, unless the image is larger than the atlas).
 */
static int
gpu_place_image(
	struct paint_gpu *gpu,
	const struct img_bitmap *image,
	uint32_t *x,
	uint32_t *y)
{
	struct paint_gpu_image *known;
	struct paint_gpu_image place;
	uint32_t width;
	uint32_t height;
	uint32_t row;
	size_t index;
	int placed;
	int error;

	/* An image placed before is where it was put. */
	for (index = 0; index < gpu->images.count; index++) {
		known = wb_vector_at(&gpu->images, index);
		if (known->serial == image->serial) {
			*x = known->x;
			*y = known->y;
			return 1;
		}
	}

	/* An image larger than the atlas never fits (drawing it is left for later). */
	width = (uint32_t)image->width;
	height = (uint32_t)image->height;
	if (width > PAINT_GPU_ATLAS_SIZE || height > PAINT_GPU_ATLAS_SIZE)
		return 0;

	/* Room on the shelves. */
	placed = gpu_shelf(gpu, width, height, &place.x, &place.y);
	if (!placed)
		return 0;

	/* Its pixels, 0xAARRGGBB, are the atlas's B8G8R8A8 texels as they lie in a little-endian memory (amd64). */
	for (row = 0U; row < height; row++)
		memcpy(gpu->atlas_map + (size_t)(place.y + row) * gpu->atlas_pitch + (size_t)place.x * 4U, image->pixels + (size_t)row * width, (size_t)width * 4U);

	/* Records the place. */
	place.serial = image->serial;
	error = wb_vector_push(&gpu->images, &place);
	if (error != 0)
		return 0;

	/* Succeeded: the image is in the atlas. */
	*x = place.x;
	*y = place.y;
	return 1;
}

/*
 * Takes a rectangle of the atlas on its shelves: left to right on the
 * current shelf, or on a new shelf below; 0 when the atlas is full (it
 * starts over before the next frame).
 */
static int
gpu_shelf(
	struct paint_gpu *gpu,
	uint32_t width,
	uint32_t height,
	uint32_t *x,
	uint32_t *y)
{
	/* Nothing larger than the atlas ever fits. */
	if (width > PAINT_GPU_ATLAS_SIZE || height > PAINT_GPU_ATLAS_SIZE)
		return 0;

	/* A rectangle past the shelf's end starts the next shelf. */
	if (gpu->shelf_x + width > PAINT_GPU_ATLAS_SIZE) {
		gpu->shelf_y += gpu->shelf_height;
		gpu->shelf_x = 0U;
		gpu->shelf_height = 0U;
	}

	/* A rectangle below the last shelf does not fit until the atlas starts over. */
	if (gpu->shelf_y + height > PAINT_GPU_ATLAS_SIZE) {
		gpu->full = 1;
		return 0;
	}

	/* The place, and the shelf moves along it. */
	*x = gpu->shelf_x;
	*y = gpu->shelf_y;
	gpu->shelf_x += width;
	if (height > gpu->shelf_height)
		gpu->shelf_height = height;

	/* Succeeded: the rectangle is taken. */
	return 1;
}

/* Finds the table's slot of a bitmap: the one holding it, or the empty one where it goes. */
static struct paint_gpu_slot *
gpu_slot(
	struct paint_gpu *gpu,
	const uint8_t *bitmap)
{
	uintptr_t key;
	size_t slot;

	/* Probes linearly from the pointer's hash. */
	key = (uintptr_t)bitmap;
	slot = (size_t)(((uint64_t)key * 0x9e3779b97f4a7c15ULL) >> 20) & (gpu->slot_capacity - 1U);
	while (gpu->slots[slot].bitmap != NULL) {
		/* The bitmap's own slot. */
		if (gpu->slots[slot].bitmap == bitmap)
			return &gpu->slots[slot];

		/* The next slot of the probe. */
		slot = (slot + 1U) & (gpu->slot_capacity - 1U);
	}

	/* The empty slot where the bitmap goes. */
	return &gpu->slots[slot];
}

/* Doubles the table of placed glyphs and moves every entry. */
static int
gpu_slots_grow(
	struct paint_gpu *gpu)
{
	struct paint_gpu_slot *old;
	struct paint_gpu_slot *slot;
	size_t old_capacity;
	size_t index;

	/* Takes the larger, empty table. */
	old = gpu->slots;
	old_capacity = gpu->slot_capacity;
	gpu->slot_capacity = old_capacity * 2U;
	if (gpu->slot_capacity < GPU_SLOTS_MIN)
		gpu->slot_capacity = GPU_SLOTS_MIN;
	gpu->slots = calloc(gpu->slot_capacity, sizeof(*gpu->slots));
	if (gpu->slots == NULL) {
		gpu->slots = old;
		gpu->slot_capacity = old_capacity;
		return ENOMEM;
	}

	/* Moves the entries. */
	for (index = 0; index < old_capacity; index++) {
		if (old[index].bitmap == NULL)
			continue;

		/* Into the slot its bitmap probes to. */
		slot = gpu_slot(gpu, old[index].bitmap);
		*slot = old[index];
	}

	/* The old table is no longer needed. */
	free(old);

	/* Succeeded: the table has room. */
	return 0;
}

/* Starts the atlas over: no glyph is placed and the shelves are empty. */
static void
gpu_atlas_reset(
	struct paint_gpu *gpu)
{
	/* Forgets every place (the texels are written again as glyphs and images come back). */
	if (gpu->slots != NULL)
		memset(gpu->slots, 0, gpu->slot_capacity * sizeof(*gpu->slots));
	gpu->slot_count = 0;
	wb_vector_clear(&gpu->images);
	gpu->shelf_x = 0U;
	gpu->shelf_y = 0U;
	gpu->shelf_height = 0U;
	gpu->full = 0;
}

/* Converts a 0xAARRGGBB color to red, green, blue and alpha from 0 to 1. */
static void
gpu_color(
	uint32_t color,
	float *out)
{
	/* Each channel over 255. */
	out[0] = (float)((color >> 16) & 0xffU) / 255.0f;
	out[1] = (float)((color >> 8) & 0xffU) / 255.0f;
	out[2] = (float)(color & 0xffU) / 255.0f;
	out[3] = (float)(color >> 24) / 255.0f;
}

/* Records a frame into a command buffer the caller began: the atlas made visible to the shader, then the pass with every instance. */
static void
gpu_record(
	struct paint_gpu *gpu,
	VkCommandBuffer commands,
	VkFramebuffer framebuffer,
	VkExtent2D extent,
	uint32_t canvas)
{
	VkImageMemoryBarrier barrier;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkBuffer buffers[1];
	VkDeviceSize offsets[1];
	float size[4];

	/* The host's writes to the atlas before the shader reads it (the first frame also leaves the preinitialized layout). */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	if (!gpu->atlas_ready)
		barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = gpu->atlas;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
	gpu->atlas_ready = 1;

	/* The pass, cleared to the canvas pixel (exact in an 8-bit target). */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = (float)((canvas >> 16) & 0xffU) / 255.0f;
	clear.color.float32[1] = (float)((canvas >> 8) & 0xffU) / 255.0f;
	clear.color.float32[2] = (float)(canvas & 0xffU) / 255.0f;
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = gpu->pass;
	pass.framebuffer = framebuffer;
	pass.renderArea.extent = extent;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The whole target is the viewport. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)extent.width;
	viewport.height = (float)extent.height;
	viewport.maxDepth = 1.0f;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent = extent;
	vkCmdSetViewport(commands, 0U, 1U, &viewport);
	vkCmdSetScissor(commands, 0U, 1U, &scissor);

	/* Every instance of the square, in the list's order. */
	if (gpu->staged.count != 0) {
		size[0] = (float)extent.width;
		size[1] = (float)extent.height;
		size[2] = 0.0f;
		size[3] = 0.0f;
		buffers[0] = gpu->instances;
		offsets[0] = 0U;
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, gpu->pipeline);
		vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, gpu->layout, 0U, 1U, &gpu->set, 0U, NULL);
		vkCmdBindVertexBuffers(commands, 0U, 1U, buffers, offsets);
		vkCmdPushConstants(commands, gpu->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(size), size);
		vkCmdDraw(commands, (uint32_t)(gpu->staged.count * GPU_CORNERS), 1U, 0U, 0U);
	}

	/* The pass ends with the image in the final layout. */
	vkCmdEndRenderPass(commands);
}

/* Submits a command buffer after wait, signalling signal, and waits for its fence. */
static VkResult
gpu_submit_commands(
	VkDevice device,
	VkQueue queue,
	VkCommandBuffer command,
	VkFence fence,
	VkSemaphore wait,
	VkSemaphore signal,
	const char **operation)
{
	VkSubmitInfo submit;
	VkPipelineStageFlags stage;
	VkResult error;

	/* The fence starts unsignalled. */
	*operation = "vkResetFences";
	error = vkResetFences(device, 1U, &fence);
	if (error != VK_SUCCESS)
		return error;

	/* The submission, with the semaphores that were given. */
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	if (wait != VK_NULL_HANDLE) {
		submit.waitSemaphoreCount = 1U;
		submit.pWaitSemaphores = &wait;
		submit.pWaitDstStageMask = &stage;
	}

	/* The one command buffer. */
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &command;

	/* The semaphore the end signals, when one was given. */
	if (signal != VK_NULL_HANDLE) {
		submit.signalSemaphoreCount = 1U;
		submit.pSignalSemaphores = &signal;
	}

	/* Submits it. */
	*operation = "vkQueueSubmit";
	error = vkQueueSubmit(queue, 1U, &submit, fence);
	if (error != VK_SUCCESS)
		return error;

	/* Waits for it, so the host may write again. */
	*operation = "vkWaitForFences";
	error = vkWaitForFences(device, 1U, &fence, VK_TRUE, GPU_TIMEOUT);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the commands ran. */
	return VK_SUCCESS;
}

/* Makes an instance without surfaces, and a device with one queue of the first family of any device that draws. */
static VkResult
offscreen_device(
	struct paint_offscreen *offscreen)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_info;
	VkPhysicalDevice devices[8];
	VkQueueFamilyProperties families[16];
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo create;
	uint32_t count;
	uint32_t family_count;
	uint32_t index;
	uint32_t family;
	float priority;
	VkResult error;

	/* The instance, without any surface extension. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "browser";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance_info, 0, sizeof(instance_info));
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &application;
	offscreen->operation = "vkCreateInstance";
	error = vkCreateInstance(&instance_info, NULL, &offscreen->instance);
	if (error != VK_SUCCESS)
		return error;

	/* The physical devices (the first eight are enough). */
	count = 8U;
	offscreen->operation = "vkEnumeratePhysicalDevices";
	error = vkEnumeratePhysicalDevices(offscreen->instance, &count, devices);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The first family of any device that draws. */
	for (index = 0U; index < count && offscreen->physical == VK_NULL_HANDLE; index++) {
		family_count = 16U;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, families);
		for (family = 0U; family < family_count; family++) {
			/* A family that draws and has a queue. */
			if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U)
				continue;
			if (families[family].queueCount == 0U)
				continue;

			/* This family of this device draws the page. */
			offscreen->physical = devices[index];
			offscreen->family = family;
			break;
		}
	}

	/* No device can draw. */
	if (offscreen->physical == VK_NULL_HANDLE) {
		offscreen->operation = "finding a device that draws";
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* One queue of that family. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = offscreen->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create.queueCreateInfoCount = 1U;
	create.pQueueCreateInfos = &queue;
	offscreen->operation = "vkCreateDevice";
	error = vkCreateDevice(offscreen->physical, &create, NULL, &offscreen->device);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the device and its queue. */
	vkGetDeviceQueue(offscreen->device, offscreen->family, 0U, &offscreen->queue);
	return VK_SUCCESS;
}

/* Makes the image drawn into and copied out of, its memory and the view a framebuffer draws through. */
static VkResult
offscreen_image(
	struct paint_offscreen *offscreen)
{
	VkImageCreateInfo image_info;
	VkImageViewCreateInfo view_info;
	VkMemoryRequirements requirements;
	VkResult error;

	/* The image: drawn into, then copied out. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = offscreen->format;
	image_info.extent.width = offscreen->extent.width;
	image_info.extent.height = offscreen->extent.height;
	image_info.extent.depth = 1U;
	image_info.mipLevels = 1U;
	image_info.arrayLayers = 1U;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	offscreen->operation = "vkCreateImage";
	error = vkCreateImage(offscreen->device, &image_info, NULL, &offscreen->image);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, of any type the image takes. */
	vkGetImageMemoryRequirements(offscreen->device, offscreen->image, &requirements);
	error = gpu_memory(offscreen->physical, offscreen->device, &requirements, 0U, &offscreen->memory, &offscreen->operation);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the image. */
	offscreen->operation = "vkBindImageMemory";
	error = vkBindImageMemory(offscreen->device, offscreen->image, offscreen->memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* The view a framebuffer draws through. */
	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = offscreen->image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = offscreen->format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1U;
	view_info.subresourceRange.layerCount = 1U;
	offscreen->operation = "vkCreateImageView";
	error = vkCreateImageView(offscreen->device, &view_info, NULL, &offscreen->view);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the image can be drawn into and read back. */
	return VK_SUCCESS;
}

/* Makes the command pool and buffer and the fence the read back uses. */
static VkResult
offscreen_commands(
	struct paint_offscreen *offscreen)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkResult error;

	/* A pool whose one buffer is reset for each read. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = offscreen->family;
	offscreen->operation = "vkCreateCommandPool";
	error = vkCreateCommandPool(offscreen->device, &pool, NULL, &offscreen->pool);
	if (error != VK_SUCCESS)
		return error;

	/* The buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = offscreen->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	offscreen->operation = "vkAllocateCommandBuffers";
	error = vkAllocateCommandBuffers(offscreen->device, &command, &offscreen->command);
	if (error != VK_SUCCESS)
		return error;

	/* The fence the copy's end signals. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	offscreen->operation = "vkCreateFence";
	error = vkCreateFence(offscreen->device, &fence, NULL, &offscreen->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the image can be read back. */
	return VK_SUCCESS;
}
