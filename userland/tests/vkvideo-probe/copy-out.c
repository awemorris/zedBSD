/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The copy-out of vkvideo-probe (--copy): a decoded picture read through
 * vkCmdCopyImageToBuffer into a buffer of plain rows (the luma plane's
 * rows of the picture's width, then the chroma plane's), so a device whose
 * optimal NV12 image has no layout the host can read (NVIDIA's block
 * linear) is read as any other.  The copy is its own submission, made
 * when the frame is output, so the pictures are read in output order, on
 * the copy-out's family (the video family when it transfers); the
 * buffer's bytes are then copied whole into the host's memory (a device's
 * memory the host sees uncached is slow to read a byte at a time, as the
 * hash reads it).
 */

#include "probe.h"
#include "copy-out.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The buffer the pictures are copied into, its memory (mapped), its size, and the host's copy of its bytes. */
struct copy_out {
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint8_t *map;
	VkDeviceSize size;
	uint8_t *host;
};

static struct copy_out copy_out;

static int copy_out_buffer(struct probe *probe);
static void copy_out_commands(struct probe *probe, uint32_t slot);

/*
 * Copies a slot's picture into the buffer and points planes at it: plain
 * rows of the probe's largest picture's width.  Returns 0, or 1 when the
 * buffer could not be made or the copy did not complete.
 */
int
probe_copy_out(
	struct probe *probe,
	uint32_t slot,
	struct frame_planes *planes)
{
	VkCommandBufferBeginInfo record;
	VkSubmitInfo submit;
	VkResult result;
	int error;

	/* The buffer, once. */
	error = copy_out_buffer(probe);
	if (error != 0)
		return error;

	/* The copy in a command buffer of its own. */
	memset(&record, 0, sizeof(record));
	record.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	record.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(probe->copy_command, &record);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkBeginCommandBuffer");
	copy_out_commands(probe, slot);
	result = vkEndCommandBuffer(probe->copy_command);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkEndCommandBuffer");

	/* Submitted and waited for; the fence and the command buffer ready for the next decode. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &probe->copy_command;
	result = vkQueueSubmit(probe->copy_queue, 1U, &submit, probe->fence);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkQueueSubmit");
	result = vkWaitForFences(probe->device, 1U, &probe->fence, VK_TRUE, PROBE_WAIT_NS);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkWaitForFences");
	result = vkResetFences(probe->device, 1U, &probe->fence);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkResetFences");
	result = vkResetCommandBuffer(probe->copy_command, 0U);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkResetCommandBuffer");

	/* The planes, in the host's copy: plain rows of the width, the chroma plane after the luma plane. */
	memcpy(copy_out.host, copy_out.map, (size_t)copy_out.size);
	planes->luma = copy_out.host;
	planes->luma_pitch = probe->extent.width;
	planes->chroma = copy_out.host + (size_t)probe->extent.width * probe->extent.height;
	planes->chroma_pitch = probe->extent.width;
	planes->linear = 1;

	/* Succeeded: the picture is in the buffer. */
	return 0;
}

/* Destroys the buffer and its memory, when they were made. */
void
probe_copy_out_close(
	struct probe *probe)
{
	/* The buffer, then its memory. */
	if (copy_out.buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(probe->device, copy_out.buffer, NULL);
	if (copy_out.memory != VK_NULL_HANDLE)
		vkFreeMemory(probe->device, copy_out.memory, NULL);
	free(copy_out.host);
	memset(&copy_out, 0, sizeof(copy_out));
}

/* Makes the buffer of a picture of the probe's largest size, with host-visible memory (mapped). */
static int
copy_out_buffer(
	struct probe *probe)
{
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkResult result;
	uint32_t type;
	void *map;

	/* Made once. */
	if (copy_out.buffer != VK_NULL_HANDLE)
		return 0;

	/* The buffer: the luma plane's bytes and half as many for the chroma plane, a copy's destination. */
	copy_out.size = (VkDeviceSize)probe->extent.width * probe->extent.height * 3U / 2U;
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = copy_out.size;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(probe->device, &buffer, NULL, &copy_out.buffer);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateBuffer");

	/* Its memory: the first host-visible type it allows. */
	vkGetBufferMemoryRequirements(probe->device, copy_out.buffer, &requirements);
	for (type = 0U; type < probe->memory.memoryTypeCount; type++) {
		if ((requirements.memoryTypeBits & (1U << type)) != 0U &&
		    (probe->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U)
			break;
	}
	if (type >= probe->memory.memoryTypeCount) {
		fprintf(stderr, "vkvideo-probe: no host-visible memory for the copy-out\n");
		return 1;
	}
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	result = vkAllocateMemory(probe->device, &allocate, NULL, &copy_out.memory);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkAllocateMemory");

	/* Bound and mapped. */
	result = vkBindBufferMemory(probe->device, copy_out.buffer, copy_out.memory, 0U);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkBindBufferMemory");
	result = vkMapMemory(probe->device, copy_out.memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkMapMemory");
	copy_out.map = map;

	/* The host's copy of its bytes. */
	copy_out.host = malloc((size_t)copy_out.size);
	if (copy_out.host == NULL) {
		fprintf(stderr, "vkvideo-probe: no memory for the copy-out\n");
		return 1;
	}

	/* Succeeded: the buffer is ready. */
	return 0;
}

/*
 * Records the copy of a slot's picture into the buffer: both planes as
 * plain rows, between barriers that take the image from the DPB layout
 * and back.
 */
static void
copy_out_commands(
	struct probe *probe,
	uint32_t slot)
{
	VkImageMemoryBarrier barrier;
	VkBufferImageCopy regions[2];

	/* From the DPB layout to the copy's. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = probe->image[slot];
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(probe->copy_command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* The luma plane's rows, then the chroma plane's (a texel of two bytes, half the width and height). */
	memset(regions, 0, sizeof(regions));
	regions[0].bufferOffset = 0U;
	regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	regions[0].imageSubresource.layerCount = 1U;
	regions[0].imageExtent.width = probe->extent.width;
	regions[0].imageExtent.height = probe->extent.height;
	regions[0].imageExtent.depth = 1U;
	regions[1].bufferOffset = (VkDeviceSize)probe->extent.width * probe->extent.height;
	regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	regions[1].imageSubresource.layerCount = 1U;
	regions[1].imageExtent.width = probe->extent.width / 2U;
	regions[1].imageExtent.height = probe->extent.height / 2U;
	regions[1].imageExtent.depth = 1U;
	vkCmdCopyImageToBuffer(probe->copy_command, probe->image[slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_out.buffer, 2U, regions);

	/* The buffer's writes seen by the host, the image back in the DPB layout. */
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
	vkCmdPipelineBarrier(probe->copy_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
}
