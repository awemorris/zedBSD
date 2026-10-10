/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Standard-command stand-ins inspect production decode/readback recording; they never emulate GPU decoding. */
#include "userland/desktop/libmedia/vkvideo-runtime.c"
#include <assert.h>
#include <stdio.h>

/* One serial host test owns this recording observation state until its final assertions. */
static struct vkvideo_runtime *observed;
/* Recorded transfers and queue waits are counted only for this host observation. */
static unsigned copies_seen;
static unsigned submissions;
static unsigned video_waits;
static unsigned transfer_waits;
static int wait_failure;

static void host_admission(void);
static VkResult host_capabilities(VkPhysicalDevice physical, const VkVideoProfileInfoKHR *profile, VkVideoCapabilitiesKHR *capabilities);
static VkResult host_formats(VkPhysicalDevice physical, const VkPhysicalDeviceVideoFormatInfoKHR *info, uint32_t *count, VkVideoFormatPropertiesKHR *formats);

/* Capability masks belong only to the bounded host query fixture. */
static VkVideoDecodeCapabilityFlagsKHR available_models;
static VkImageUsageFlags refused_usage;

static VkResult host_begin(VkCommandBuffer command, const VkCommandBufferBeginInfo *info);
static VkResult host_end(VkCommandBuffer command);
static VkResult host_reset_command(VkCommandBuffer command, VkCommandBufferResetFlags flags);
static VkResult host_reset_fence(VkDevice device, uint32_t count, const VkFence *fences);
static VkResult host_wait(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout);
static VkResult host_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submit, VkFence fence);
static void host_begin_video(VkCommandBuffer command, const VkVideoBeginCodingInfoKHR *info);
static void host_control(VkCommandBuffer command, const VkVideoCodingControlInfoKHR *info);
static void host_decode(VkCommandBuffer command, const VkVideoDecodeInfoKHR *info);
static void host_end_video(VkCommandBuffer command, const VkVideoEndCodingInfoKHR *info);
static void host_barrier(VkCommandBuffer command, const VkDependencyInfo *info);
static void host_copy(VkCommandBuffer command, VkImage image, VkImageLayout layout, VkBuffer buffer, uint32_t count, const VkBufferImageCopy *copy);

/*
 * Exercises coincident and distinct output recording with separate video and transfer queues.
 */
int
main(
	void)
{
	struct vkvideo_runtime video;
	struct h264_stream *stream;
	struct h264_picture *picture;
	struct h264_dpb_plan plan;
	struct media_picture_pool *pool;
	struct media_picture *output;
	uint8_t bytes[4];
	unsigned mode;
	unsigned frame;
	int error;

	/* Query-driven allocation must admit complete usages and honor profile limits. */
	host_admission();

	/* Production parsing and driver pixel decoding are tested elsewhere; this fixture observes API contracts. */
	stream = calloc(1U, sizeof(*stream));
	assert(stream != NULL);
	picture = calloc(1U, sizeof(*picture));
	assert(picture != NULL);
	pool = media_picture_pool_create(32U, 16U, 2U);
	assert(pool != NULL);
	output = media_picture_pool_get(pool);
	assert(output != NULL);
	bytes[0] = 0x65U;
	bytes[1] = 0x88U;
	bytes[2] = 0x80U;
	bytes[3] = 0x80U;
	stream->data = bytes;
	stream->size = sizeof(bytes);
	stream->sps[0].pic_width_in_mbs_minus1 = 1U;
	stream->sps[0].pic_height_in_map_units_minus1 = 0U;
	picture->slice_count = 1U;
	picture->slice_sizes[0] = sizeof(bytes);
	picture->info.flags.is_reference = 1;
	memset(&plan, 0, sizeof(plan));
	plan.decode = 1;
	plan.setup = 0;
	plan.reset = 1;
	copies_seen = 0U;
	submissions = 0U;
	video_waits = 0U;
	transfer_waits = 0U;
	for (mode = 0U; mode < 2U; mode++) {
		memset(&video, 0, sizeof(video));
		observed = &video;
		video.extent.width = 32U;
		video.extent.height = 16U;
		video.slots = 2U;
		video.distinct = (int)mode;
		video.capabilities.minBitstreamBufferSizeAlignment = 32U;
		video.buffer_size = 4096U;
		video.buffer_map = calloc(1U, 4096U);
		assert(video.buffer_map != NULL);
		video.readback_map = calloc(1U, 768U);
		assert(video.readback_map != NULL);
		video.buffer_coherent = 1;
		video.readback_coherent = 1;
		video.command = (VkCommandBuffer)(uintptr_t)1U;
		video.transfer_command = (VkCommandBuffer)(uintptr_t)2U;
		video.queue = (VkQueue)(uintptr_t)3U;
		video.transfer_queue = (VkQueue)(uintptr_t)4U;
		video.decoded = (VkSemaphore)5U;
		video.returned = (VkSemaphore)6U;
		video.fence = (VkFence)7U;
		video.fn.vkBeginCommandBuffer = host_begin;
		video.fn.vkEndCommandBuffer = host_end;
		video.fn.vkResetCommandBuffer = host_reset_command;
		video.fn.vkResetFences = host_reset_fence;
		video.fn.vkWaitForFences = host_wait;
		video.fn.vkQueueSubmit = host_submit;
		video.fn.vkCmdBeginVideoCodingKHR = host_begin_video;
		video.fn.vkCmdControlVideoCodingKHR = host_control;
		video.fn.vkCmdDecodeVideoKHR = host_decode;
		video.fn.vkCmdEndVideoCodingKHR = host_end_video;
		video.fn.vkCmdPipelineBarrier2KHR = host_barrier;
		video.fn.vkCmdCopyImageToBuffer = host_copy;
		for (frame = 0U; frame < 3U; frame++) {
			plan.reset = 0;
			if (frame == 0U)
				plan.reset = 1;
			error = media_vkvideo_runtime_decode(&video, stream, picture, &plan, output);
			assert(error == 0);
			assert(video.inflight == 0);
			assert(output->luma[31] == 47U);
			assert(output->chroma[0] == 100U);
			assert(output->chroma[1] == 150U);
		}

		/* A fence timeout must leave the submitted runtime resources owned and in flight. */
		wait_failure = 1;
		error = media_vkvideo_runtime_decode(&video, stream, picture, &plan, output);
		assert(error == ETIMEDOUT);
		assert(video.inflight == 1);
		wait_failure = 0;
		free(video.readback_map);
		free(video.buffer_map);
	}

	/* Each decode uses a separate transfer submission, with the reverse dependency from the preceding transfer. */
	assert(copies_seen == 8U);
	assert(submissions == 16U);
	assert(video_waits == 6U);
	assert(transfer_waits == 8U);
	media_picture_unref(output);
	media_picture_pool_close(pool);
	free(picture);
	free(stream);
	puts("Vulkan Video runtime host PASS (coincident/distinct, two queues, standard planes, timeout retention)");
	return 0;
}

/*
 * The host harness consumes library diagnostics without requiring a desktop engine.
 */
void
media_log(
	const char *format,
	...)
{
	(void)format;
}

/* A command must be recorded in its own family-specific pool. */
static VkResult
host_begin(
	VkCommandBuffer command,
	const VkCommandBufferBeginInfo *info)
{
	assert(command == observed->command || command == observed->transfer_command);
	assert(info->flags == VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	return VK_SUCCESS;
}

/* End recording without executing fake commands on a physical device. */
static VkResult
host_end(
	VkCommandBuffer command)
{
	(void)command;
	return VK_SUCCESS;
}

/* Command reset occurs only after the transfer fence completed. */
static VkResult
host_reset_command(
	VkCommandBuffer command,
	VkCommandBufferResetFlags flags)
{
	(void)command;
	(void)flags;
	assert(observed->inflight == 0);
	return VK_SUCCESS;
}

/* Fence reset also requires retired memory. */
static VkResult
host_reset_fence(
	VkDevice device,
	uint32_t count,
	const VkFence *fences)
{
	(void)device;
	(void)fences;
	assert(count == 1U);
	assert(observed->inflight == 0);
	return VK_SUCCESS;
}

/* A failed wait must preserve ownership instead of exposing the recording stand-in's bytes as GPU output. */
static VkResult
host_wait(
	VkDevice device,
	uint32_t count,
	const VkFence *fences,
	VkBool32 all,
	uint64_t timeout)
{
	(void)device;
	(void)fences;
	assert(count == 1U);
	assert(all == VK_TRUE);
	assert(timeout == VKVIDEO_WAIT_NS);
	if (wait_failure)
		return VK_TIMEOUT;
	return VK_SUCCESS;
}

/* Inspect binary semaphore direction and ensure only the transfer submission signals the host fence. */
static VkResult
host_submit(
	VkQueue queue,
	uint32_t count,
	const VkSubmitInfo *submit,
	VkFence fence)
{
	assert(count == 1U);
	assert(submit->commandBufferCount == 1U);
	assert(submit->signalSemaphoreCount == 1U);
	if (queue == observed->queue) {
		assert(fence == VK_NULL_HANDLE);
		assert(submit->pCommandBuffers[0] == observed->command);
		assert(submit->pSignalSemaphores[0] == observed->decoded);
		if (submit->waitSemaphoreCount != 0U) {
			assert(submit->pWaitSemaphores[0] == observed->returned);
			video_waits++;
		}
	} else {
		assert(queue == observed->transfer_queue);
		assert(fence == observed->fence);
		assert(submit->waitSemaphoreCount == 1U);
		assert(submit->pWaitSemaphores[0] == observed->decoded);
		assert(submit->pSignalSemaphores[0] == observed->returned);
		assert(submit->pWaitDstStageMask[0] == VK_PIPELINE_STAGE_TRANSFER_BIT);
		transfer_waits++;
	}

	/* Count each observed standard queue submission. */
	submissions++;
	return VK_SUCCESS;
}

/* Video coding scope is recorded only on the video command. */
static void
host_begin_video(
	VkCommandBuffer command,
	const VkVideoBeginCodingInfoKHR *info)
{
	assert(command == observed->command);
	assert(info->referenceSlotCount >= 1U);
}

/* Initial and seek reset remain standard coding controls. */
static void
host_control(
	VkCommandBuffer command,
	const VkVideoCodingControlInfoKHR *info)
{
	assert(command == observed->command);
	assert(info->flags == VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR);
}

/* Capture aligned source range and the separate destination view used by distinct-output devices. */
static void
host_decode(
	VkCommandBuffer command,
	const VkVideoDecodeInfoKHR *info)
{
	assert(command == observed->command);
	assert(info->srcBufferOffset == 0U);
	assert(info->srcBufferRange == 32U);
	assert(info->pSetupReferenceSlot->slotIndex == 0);
	assert(info->dstPictureResource.codedExtent.width == 32U);
	assert(info->dstPictureResource.codedExtent.height == 16U);
}

/* End the decode scope before recording any transfer command. */
static void
host_end_video(
	VkCommandBuffer command,
	const VkVideoEndCodingInfoKHR *info)
{
	assert(command == observed->command);
	assert(info->sType == VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR);
}

/* Dedicated transfer commands may never name video stages; semaphore dependencies cross those queues. */
static void
host_barrier(
	VkCommandBuffer command,
	const VkDependencyInfo *info)
{
	unsigned index;

	/* Check each image barrier's stage validity for its recorded queue. */
	for (index = 0U; index < info->imageMemoryBarrierCount; index++) {
		if (command == observed->transfer_command) {
			assert((info->pImageMemoryBarriers[index].srcStageMask & VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR) == 0U);
			assert((info->pImageMemoryBarriers[index].dstStageMask & VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR) == 0U);
		}
	}

	/* Require buffer host-visibility barriers to be recorded on the transfer command. */
	if (info->bufferMemoryBarrierCount != 0U) {
		assert(command == observed->transfer_command);
		assert(info->pBufferMemoryBarriers[0].dstAccessMask == VK_ACCESS_2_HOST_READ_BIT);
	}
}

/* Populate a transfer buffer stand-in with a visible pattern after checking exact standard plane/sample coordinates. */
static void
host_copy(
	VkCommandBuffer command,
	VkImage image,
	VkImageLayout layout,
	VkBuffer buffer,
	uint32_t count,
	const VkBufferImageCopy *copy)
{
	unsigned index;

	/* Inspect exact plane-copy geometry before writing only host stand-in output. */
	(void)image;
	(void)buffer;
	assert(command == observed->transfer_command);
	assert(layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	assert(count == 2U);
	assert(copy[0].imageSubresource.aspectMask == VK_IMAGE_ASPECT_PLANE_0_BIT);
	assert(copy[0].imageExtent.width == 32U);
	assert(copy[0].imageExtent.height == 16U);
	assert(copy[1].imageSubresource.aspectMask == VK_IMAGE_ASPECT_PLANE_1_BIT);
	assert(copy[1].imageExtent.width == 16U);
	assert(copy[1].imageExtent.height == 8U);
	assert(copy[1].bufferOffset == 512U);
	for (index = 0U; index < 512U; index++)
		observed->readback_map[index] = (uint8_t)(16U + index % 32U);
	for (index = 512U; index < 768U; index += 2U) {
		observed->readback_map[index] = 100U;
		observed->readback_map[index + 1U] = 150U;
	}

	/* Count completed copy recordings independently of submission retirement. */
	copies_seen++;
}

/* Check the actual production capability/format path with standard API responses. */
static void
host_admission(
	void)
{
	struct vkvideo_runtime video;
	int error;

	/* A coincident image requires all three usages in one query. */
	memset(&video, 0, sizeof(video));
	video.fn.vkGetPhysicalDeviceVideoCapabilitiesKHR = host_capabilities;
	video.fn.vkGetPhysicalDeviceVideoFormatPropertiesKHR = host_formats;
	video.extent.width = 320U;
	video.extent.height = 192U;
	video.slots = 4U;
	video.max_references = 3U;
	video.level = STD_VIDEO_H264_LEVEL_IDC_4_1;
	available_models = VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR;
	refused_usage = 0U;
	error = vkvideo_capabilities(&video);
	assert(error == 0 && video.distinct == 0);
	assert((video.reference_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U);

	/* Reject the combined allocation, then admit distinct reference and transferable output images. */
	available_models |= VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR;
	refused_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	error = vkvideo_capabilities(&video);
	assert(error == 0 && video.distinct == 1);
	assert(video.reference_usage == VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR);

	/* A missing transferable output and a level beyond the device's maximum are profile errors, not GPU-specific fallbacks. */
	available_models = VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR;
	refused_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	error = vkvideo_capabilities(&video);
	assert(error == ENOTSUP);
	refused_usage = 0U;
	video.level = STD_VIDEO_H264_LEVEL_IDC_5_1;
	error = vkvideo_capabilities(&video);
	assert(error == ENOTSUP);
}

/* Publish one progressive eight-bit profile's standard extent, DPB and level limits. */
static VkResult
host_capabilities(
	VkPhysicalDevice physical,
	const VkVideoProfileInfoKHR *profile,
	VkVideoCapabilitiesKHR *capabilities)
{
	VkVideoDecodeCapabilitiesKHR *decode;
	VkVideoDecodeH264CapabilitiesKHR *h264;

	/* The chain is caller-owned and retains its declared structure identities. */
	(void)physical;
	(void)profile;
	decode = (VkVideoDecodeCapabilitiesKHR *)capabilities->pNext;
	h264 = (VkVideoDecodeH264CapabilitiesKHR *)decode->pNext;
	decode->flags = available_models;
	h264->maxLevelIdc = STD_VIDEO_H264_LEVEL_IDC_4_1;
	capabilities->minCodedExtent.width = 16U;
	capabilities->minCodedExtent.height = 16U;
	capabilities->maxCodedExtent.width = 4096U;
	capabilities->maxCodedExtent.height = 4096U;
	capabilities->maxDpbSlots = 17U;
	capabilities->maxActiveReferencePictures = 16U;
	capabilities->minBitstreamBufferSizeAlignment = 64U;
	capabilities->pictureAccessGranularity.width = 16U;
	capabilities->pictureAccessGranularity.height = 16U;
	return VK_SUCCESS;
}

/* Return a format only for the complete image usage requested by production code. */
static VkResult
host_formats(
	VkPhysicalDevice physical,
	const VkPhysicalDeviceVideoFormatInfoKHR *info,
	uint32_t *count,
	VkVideoFormatPropertiesKHR *formats)
{
	/* Counting and filling have identical admission semantics. */
	(void)physical;
	*count = 0U;
	if (info->imageUsage == refused_usage)
		return VK_SUCCESS;
	*count = 1U;
	if (formats == NULL)
		return VK_SUCCESS;
	formats->format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	formats->imageType = VK_IMAGE_TYPE_2D;
	formats->imageTiling = VK_IMAGE_TILING_OPTIMAL;
	formats->imageUsageFlags = info->imageUsage;
	return VK_SUCCESS;
}
