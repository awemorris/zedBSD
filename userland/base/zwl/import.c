/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Imports a client's GPU image into window mode's Vulkan device once per
 * wl_buffer (WS035 compositing design, D2): the image, its memory (the
 * standard OPAQUE_FD import of the buffer's fd), a view and a descriptor
 * set are made when the buffer is created, so a commit costs no allocation
 * and no ioctl.
 */

#include "compose.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static VkResult import_image(struct zwl_compose *compose, const struct gpu_image_descriptor *image, int descriptor, struct zwl_import *import);
static VkResult import_layout(struct zwl_compose *compose, struct zwl_import *import);
static void import_release(struct zwl_compose *compose, struct zwl_import *import);

/*
 * Imports a buffer's image for window mode.  The descriptor stays the
 * caller's; a copy of it is given to Vulkan.
 */
int
zwl_import_create(
	struct zwl_object *buffer,
	int descriptor)
{
	struct zwl_compose *compose;
	struct zwl_import *import;
	int copy;
	VkResult result;

	/* Without window mode there is nothing to import into. */
	compose = buffer->client->server->compose;
	if (compose == NULL)
		return 0;

	/* The import record, owned by the buffer. */
	import = calloc(1, sizeof(*import));
	if (import == NULL)
		return ENOMEM;

	/* Vulkan consumes the fd it imports, so it gets its own. */
	copy = dup(descriptor);
	if (copy < 0) {
		free(import);
		return errno;
	}

	/* The image bound to the imported memory, its view and descriptor set. */
	result = import_image(compose, &buffer->image.image, copy, import);
	if (result != VK_SUCCESS) {
		printf("ZWL VULKAN_IMPORT_ERROR client=%llu buffer=%u result=%d\n", (unsigned long long)buffer->client->number, buffer->id, (int)result);
		import_release(compose, import);
		free(import);
		return EINVAL;
	}

	/* Succeeded: the buffer can be drawn in window mode. */
	buffer->import = import;
	if (buffer->client->server->log_frames)
		printf("ZWL VULKAN_IMPORT client=%llu buffer=%u width=%u height=%u\n", (unsigned long long)buffer->client->number, buffer->id, import->width, import->height);
	return 0;
}

/*
 * Releases a buffer's Vulkan image; the caller guarantees that no frame in
 * flight still samples it (the frame holds the buffer until it completes).
 */
void
zwl_import_destroy(
	struct zwl_object *buffer)
{
	struct zwl_compose *compose;

	/* A buffer without an import has nothing to release. */
	if (buffer->import == NULL)
		return;

	/* The Vulkan objects, then the record. */
	compose = buffer->client->server->compose;
	if (compose != NULL)
		import_release(compose, buffer->import);
	free(buffer->import);
	buffer->import = NULL;
}

/*
 * Creates a linear image of the described layout, imports the fd as its
 * memory, and makes the view and descriptor set that sample it.
 */
static VkResult
import_image(
	struct zwl_compose *compose,
	const struct gpu_image_descriptor *image,
	int descriptor,
	struct zwl_import *import)
{
	VkExternalMemoryImageCreateInfo external;
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkImportMemoryFdInfoKHR import_info;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	VkFormat format;
	VkResult result;

	/* Only linear images of the two four-channel formats are shared today. */
	if (image->tiling != GPU_IMAGE_LINEAR) {
		close(descriptor);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* The channel order is the client's. */
	if (image->format == GPU_PIXEL_BGRA8888) {
		format = VK_FORMAT_B8G8R8A8_UNORM;
	} else if (image->format == GPU_PIXEL_RGBA8888) {
		format = VK_FORMAT_R8G8B8A8_UNORM;
	} else {
		close(descriptor);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* The image, sampled, with external memory. */
	import->width = image->width;
	import->height = image->height;
	import->draw = ZWL_DRAW_OPAQUE;
	memset(&external, 0, sizeof(external));
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.pNext = &external;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = format;
	create.extent.width = image->width;
	create.extent.height = image->height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_LINEAR;
	create.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(compose->device, &create, NULL, &import->image);
	if (result != VK_SUCCESS) {
		close(descriptor);
		return result;
	}

	/* The client's layout must be the one this image has: same memory type, rows and offset. */
	vkGetImageMemoryRequirements(compose->device, import->image, &requirements);
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(compose->device, import->image, &subresource, &layout);
	if ((requirements.memoryTypeBits & (1U << image->memory_type)) == 0U ||
	    requirements.size > image->allocation_bytes ||
	    layout.offset != image->offset ||
	    layout.rowPitch != image->stride) {
		close(descriptor);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* The memory is the client's allocation, imported through its fd (consumed on success). */
	memset(&import_info, 0, sizeof(import_info));
	import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	import_info.fd = descriptor;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext = &import_info;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = image->memory_type;
	result = vkAllocateMemory(compose->device, &allocate, NULL, &import->memory);
	if (result != VK_SUCCESS) {
		close(descriptor);
		return result;
	}

	/* The image uses that memory. */
	result = vkBindImageMemory(compose->device, import->image, import->memory, 0U);
	if (result != VK_SUCCESS)
		return result;

	/* The view the shader samples. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = import->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	result = vkCreateImageView(compose->device, &view, NULL, &import->view);
	if (result != VK_SUCCESS)
		return result;

	/* The image goes to the layout it is sampled in, once. */
	result = import_layout(compose, import);
	if (result != VK_SUCCESS)
		return result;

	/* Its descriptor set (a spare one when there is one). */
	result = zwl_compose_set_get(compose, &import->set);
	if (result != VK_SUCCESS)
		return result;
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = compose->sampler;
	image_info.imageView = import->view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = import->set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(compose->device, 1U, &write, 0U, NULL);

	/* And the same image sampled linearly. */
	result = zwl_compose_linear_set(compose, import);
	return result;
}

/*
 * Moves a new image to the general layout, where it is sampled while the
 * client keeps writing it through its own image of the same memory.
 */
static VkResult
import_layout(
	struct zwl_compose *compose,
	struct zwl_import *import)
{
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkSubmitInfo submit;
	VkCommandBuffer command;
	VkResult result;

	/* A one-time command buffer. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = compose->pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1U;
	result = vkAllocateCommandBuffers(compose->device, &allocate, &command);
	if (result != VK_SUCCESS)
		return result;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(command, &begin);

	/* The barrier to the general layout. */
	if (result == VK_SUCCESS) {
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = 0U;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = import->image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1U;
		barrier.subresourceRange.layerCount = 1U;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
		result = vkEndCommandBuffer(command);
	}

	/* Submitted and finished before the image is first drawn (once per buffer). */
	if (result == VK_SUCCESS) {
		memset(&submit, 0, sizeof(submit));
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1U;
		submit.pCommandBuffers = &command;
		result = vkQueueSubmit(compose->queue, 1U, &submit, VK_NULL_HANDLE);
	}

	/* The barrier is done before the first frame samples the image. */
	if (result == VK_SUCCESS)
		result = vkQueueWaitIdle(compose->queue);

	/* The command buffer is not kept. */
	vkFreeCommandBuffers(compose->device, compose->pool, 1U, &command);
	return result;
}

/* Destroys what an import made, whatever part of it was made. */
static void
import_release(
	struct zwl_compose *compose,
	struct zwl_import *import)
{
	/* Each object, in the reverse order of its making. */
	if (import->set != VK_NULL_HANDLE)
		zwl_compose_set_put(compose, import->set);
	if (import->linear_set != VK_NULL_HANDLE)
		zwl_compose_set_put(compose, import->linear_set);
	if (import->view != VK_NULL_HANDLE)
		vkDestroyImageView(compose->device, import->view, NULL);
	if (import->image != VK_NULL_HANDLE)
		vkDestroyImage(compose->device, import->image, NULL);
	if (import->memory != VK_NULL_HANDLE)
		vkFreeMemory(compose->device, import->memory, NULL);
	memset(import, 0, sizeof(*import));
}
