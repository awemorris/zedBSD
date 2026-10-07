/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Imports a client's GPU image into window mode's Vulkan device once per
 * wl_buffer: the OS module creates the image and its bound memory; this
 * file adopts them and makes the view and descriptor sets.  A commit
 * costs no allocation and no ioctl.
 *
 * The image's move to the general layout is not submitted and waited for
 * at the import (a client's swapchain of three images waited three times
 * for the frame in flight, ws094-p009): it is recorded at the start of the
 * next frame, which is the first that can sample the image (ws099-p016).
 */

#include "compose.h"

#include <stdlib.h>
#include <string.h>

static VkResult import_image(struct kwl_compose *compose, VkFormat format, struct kwl_import *import);
static VkResult import_layout(struct kwl_compose *compose, struct kwl_import *import);
static void import_release(struct kwl_compose *compose, struct kwl_import *import);

/*
 * Adopts an image and its bound memory for a GPU buffer.
 *
 * The OS module creates them; this function makes the view the shader
 * samples, the move to the general layout and the descriptor sets.  The
 * image and the memory are taken: on failure they are destroyed with
 * whatever else was made.
 */
VkResult
kwl_import_adopt(
	struct kwl_object *buffer,
	VkImage image,
	VkDeviceMemory memory,
	uint32_t width,
	uint32_t height,
	VkFormat format)
{
	struct kwl_compose *compose;
	struct kwl_import *import;
	VkResult status;

	/* The compositor's Vulkan device the image belongs to. */
	compose = buffer->client->server->compose;

	/* The import record, owned by the buffer; without it the image and its memory go. */
	import = calloc(1, sizeof(*import));
	if (import == NULL) {
		vkDestroyImage(compose->device, image, NULL);
		vkFreeMemory(compose->device, memory, NULL);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* The image, its memory and its size, drawn opaque until set_alpha says otherwise. */
	import->image = image;
	import->memory = memory;
	import->width = width;
	import->height = height;
	import->draw = KWL_DRAW_OPAQUE;

	/* The view and descriptor sets that sample it, and its move to the general layout. */
	status = import_image(compose, format, import);
	if (status != VK_SUCCESS) {
		import_release(compose, import);
		free(import);
		return status;
	}

	/* Succeeded: the buffer can be drawn in window mode. */
	buffer->import = import;
	return VK_SUCCESS;
}

/*
 * Sets how window mode draws a GPU buffer's image: covering what is under it (alpha 0) or blended by its premultiplied alpha (alpha 1).
 *
 * A buffer without an image has nothing to change.
 */
void
kwl_import_set_alpha(
	struct kwl_object *buffer,
	uint32_t alpha)
{
	/* No image, nothing to draw differently. */
	if (buffer->import == NULL)
		return;

	/* The drawing's blending. */
	buffer->import->draw = KWL_DRAW_OPAQUE;

	/* Selects blending only for the premultiplied-alpha protocol value. */
	if (alpha == 1U)
		buffer->import->draw = KWL_DRAW_ALPHA;

	/* Succeeded: the imported image has the requested drawing mode. */
	return;
}

/*
 * Releases a buffer's Vulkan image; the caller guarantees that no frame in flight still samples it (the frame holds the buffer until it completes).
 */
void
kwl_import_destroy(
	struct kwl_object *buffer)
{
	struct kwl_compose *compose;

	/* A buffer without an import has nothing to release. */
	if (buffer->import == NULL)
		return;

	/* The Vulkan objects of the client's server, then the record. */
	compose = buffer->client->server->compose;
	kwl_import_destroy_with(compose, buffer);
}

/*
 * Releases a buffer's import through a compositor given (a gone client's
 * buffer, whose client record is no longer there, BUG-239).
 */
void
kwl_import_destroy_with(
	struct kwl_compose *compose,
	struct kwl_object *buffer)
{
	/* A buffer without an import has nothing to release. */
	if (buffer->import == NULL)
		return;

	/* The Vulkan objects, then the record. */
	if (compose != NULL)
		import_release(compose, buffer->import);

	/* Retires the buffer-owned record after its Vulkan objects are released. */
	free(buffer->import);
	buffer->import = NULL;
}

/*
 * Records the move of the images imported since the last frame to the general layout, where they are sampled while their clients keep writing them through their own images of the same memory.
 *
 * It is recorded before the frame's pass, which is the first to sample them.
 */
void
kwl_import_layouts_record(
	struct kwl_compose *compose,
	VkCommandBuffer command)
{
	VkImageMemoryBarrier barriers[KWL_LAYOUTS_MAX];
	unsigned index;

	/* No image waits. */
	if (compose->layout_count == 0U)
		return;

	/* One barrier for each waiting image. */
	memset(barriers, 0, sizeof(barriers));
	for (index = 0; index < compose->layout_count; index++) {
		/* Describes one waiting image before the frame starts sampling it. */
		barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[index].srcAccessMask = 0U;
		barriers[index].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barriers[index].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[index].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[index].image = compose->layouts[index]->image;
		barriers[index].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[index].subresourceRange.levelCount = 1U;
		barriers[index].subresourceRange.layerCount = 1U;
	}

	/* All of them before the fragment shaders sample anything. */
	vkCmdPipelineBarrier(command,
			     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			     0U,
			     0U,
			     NULL,
			     0U,
			     NULL,
			     compose->layout_count,
			     barriers);

	/* Succeeded: the waiting images have barriers in this command buffer. */
	return;
}

/*
 * Forgets the images whose move was recorded, once the frame that records it is submitted (a frame not submitted records them again next time).
 */
void
kwl_import_layouts_done(
	struct kwl_compose *compose)
{
	unsigned index;

	/* Each image is in the general layout from now on. */
	for (index = 0; index < compose->layout_count; index++)
		compose->layouts[index]->layout_pending = 0U;

	/* Publishes an empty queue after all submitted imports left it. */
	compose->layout_count = 0U;

	/* Succeeded: the submitted frame owns no pending image layouts. */
	return;
}

/* Makes the view and descriptor sets that sample an imported image. */
static VkResult
import_image(
	struct kwl_compose *compose,
	VkFormat format,
	struct kwl_import *import)
{
	VkImageViewCreateInfo view;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	VkResult status;

	/* The view the shader samples. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = import->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;

	/* Creates the view used by both sampling modes. */
	status = vkCreateImageView(compose->device, &view, NULL, &import->view);
	if (status != VK_SUCCESS)
		return status;

	/*
	 * The image goes to the layout it is sampled in, once: in the next
	 * frame's commands, or now when too many images wait.
	 */
	if (compose->layout_count < KWL_LAYOUTS_MAX) {
		compose->layouts[compose->layout_count] = import;
		compose->layout_count++;
		import->layout_pending = 1U;
	} else {
		status = import_layout(compose, import);
		if (status != VK_SUCCESS)
			return status;
	}

	/* Its descriptor set (a spare one when there is one). */
	status = kwl_compose_set_get(compose, &import->set);
	if (status != VK_SUCCESS)
		return status;

	/* Describes the sampler and view bound to the retained descriptor set. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = compose->sampler;
	image_info.imageView = import->view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

	/* Bind the sampled image to the buffer's descriptor set. */
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = import->set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(compose->device, 1U, &write, 0U, NULL);

	/* And the same image sampled linearly. */
	status = kwl_compose_linear_set(compose, import);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: both sampling modes have descriptor sets. */
	return VK_SUCCESS;
}

/* Moves an imported image to the shader-readable general layout. */
static VkResult
import_layout(
	struct kwl_compose *compose,
	struct kwl_import *import)
{
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkSubmitInfo submit;
	VkCommandBuffer command;
	VkResult status;

	/* A one-time command buffer. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = compose->pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1U;

	/* Allocates the temporary command buffer for this immediate transition. */
	status = vkAllocateCommandBuffers(compose->device, &allocate, &command);
	if (status != VK_SUCCESS)
		return status;

	/* Describes the one-time recording begun on that command buffer. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	/* Begins the transition; failure skips recording and still releases the command. */
	status = vkBeginCommandBuffer(command, &begin);

	/* The barrier to the general layout. */
	if (status == VK_SUCCESS) {
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

		/* Finishes the barrier recording before it can be submitted. */
		status = vkEndCommandBuffer(command);
	}

	/* Submitted and finished before the image is first drawn (once per buffer). */
	if (status == VK_SUCCESS) {
		memset(&submit, 0, sizeof(submit));
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1U;
		submit.pCommandBuffers = &command;

		/* Submits the finished barrier before the first sampling frame. */
		status = vkQueueSubmit(compose->queue, 1U, &submit, VK_NULL_HANDLE);
	}

	/* The barrier is done before the first frame samples the image. */
	if (status == VK_SUCCESS)
		status = vkQueueWaitIdle(compose->queue);

	/* The command buffer is not kept. */
	vkFreeCommandBuffers(compose->device, compose->pool, 1U, &command);

	/* Propagates a layout transition that failed before the buffer was released. */
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the image is ready for sampling in the general layout. */
	return VK_SUCCESS;
}

/* Destroys what an import made, whatever part of it was made. */
static void
import_release(
	struct kwl_compose *compose,
	struct kwl_import *import)
{
	unsigned index;

	/* An image still waiting for its layout is no longer waited for. */
	if (import->layout_pending) {
		/* Removes this retiring import from the pending layout collection. */
		for (index = 0; index < compose->layout_count; index++) {
			/* Skips queued images whose lifetime is unaffected. */
			if (compose->layouts[index] != import)
				continue;

			/* Fills the retired slot with the last queued image. */
			compose->layout_count--;
			compose->layouts[index] = compose->layouts[compose->layout_count];
			break;
		}

		/* It waits no more. */
		import->layout_pending = 0U;
	}

	/* Each object, in the reverse order of its making. */
	if (import->set != VK_NULL_HANDLE)
		kwl_compose_set_put(compose, import->set);

	/* Returns the spare descriptor for linear sampling. */
	if (import->linear_set != VK_NULL_HANDLE)
		kwl_compose_set_put(compose, import->linear_set);

	/* Destroys the view before its image. */
	if (import->view != VK_NULL_HANDLE)
		vkDestroyImageView(compose->device, import->view, NULL);

	/* Destroys the image before its memory. */
	if (import->image != VK_NULL_HANDLE)
		vkDestroyImage(compose->device, import->image, NULL);

	/* Releases the dedicated memory last. */
	if (import->memory != VK_NULL_HANDLE)
		vkFreeMemory(compose->device, import->memory, NULL);

	/* Clears the record so partial cleanup remains harmless. */
	memset(import, 0, sizeof(*import));

	/* Succeeded: the import retains no layout queue entry or Vulkan object. */
	return;
}
