/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Verify that libvulkan forwards standard readback usage to the GPU query.
 * Reuse the WS083 transport fixture; its response remains a stand-in.
 */

#define main ws083_video_main
#include "../../ws083/tests/host-libvulkan-video.c"
#undef main

/*
 * Run existing video checks and query decode output with transfer source usage.
 */
int
main(void)
{
	VkVideoDecodeH264ProfileInfoKHR h264;
	VkVideoProfileInfoKHR profile;
	VkVideoProfileListInfoKHR list;
	VkPhysicalDeviceVideoFormatInfoKHR info;
	VkResult status;
	uint32_t count;
	unsigned before;

	/* Initialize the fixture and retain the existing video regression. */
	ws083_video_main();

	/* An ordinary H.264 decode profile, with no GPU-specific identity. */
	memset(&h264, 0, sizeof(h264));
	h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
	h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
	memset(&profile, 0, sizeof(profile));
	profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	profile.pNext = &h264;
	profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	memset(&list, 0, sizeof(list));
	list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
	list.profileCount = 1U;
	list.pProfiles = &profile;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
	info.pNext = &list;
	info.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	/* The library must pass this query to the driver, retaining the usage bits. */
	before = transactions;
	status = vkGetPhysicalDeviceVideoFormatPropertiesKHR(&physical, &info, &count, NULL);
	assert(status == VK_SUCCESS);
	assert(transactions == before + 1U);
	assert(request_bytes >= 24U);
	request_cursor = request_bytes - 24U;
	expect32(info.imageUsage);
	expect64(1U);
	expect32(32U);
	expect64(32U);
	assert(request_cursor == request_bytes);

	/* Upload and sampled-image capabilities have not been added to this implementation. */
	info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	before = transactions;
	status = vkGetPhysicalDeviceVideoFormatPropertiesKHR(&physical, &info, &count, NULL);
	assert(status == VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR);
	assert(transactions == before);
	printf("WS202 standard video readback query host PASS\n");

	/* Succeeded: readback queries reach the driver and unsupported usage still fails. */
	return 0;
}
