/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of libmedia's choice of pictures the GPU holds
 * (vkvideo_gpu_capabilities in userland/desktop/libmedia/vkvideo-runtime.c,
 * compiled in here): a device whose video decode family is not its
 * transfer family (the i915 executor's) gets them, as one whose one family
 * does both does; a distinct output, a device without the export
 * extensions, one without sampled NV12 decode images and a program that
 * did not ask get none.  The device's format query is a stand-in.
 * Last line: gpu-hold: PASS or gpu-hold: FAIL.
 */

#include "userland/desktop/libmedia/vkvideo-runtime.c"

#include <stdarg.h>
#include <stdio.h>

/* What the stand-in format query offers: the usage and create flags of its one NV12 format. */
static VkImageUsageFlags offered_usage;
static VkImageCreateFlags offered_flags;
static unsigned failures;
static unsigned checks;

/* The library's log, printed. */
void
media_log(
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	printf("  log: ");
	vprintf(format, arguments);
	printf("\n");
	va_end(arguments);
}

/* The stand-in of vkGetPhysicalDeviceVideoFormatPropertiesKHR: one NV12 optimal 2D format. */
static VkResult VKAPI_CALL
fake_formats(
	VkPhysicalDevice physical,
	const VkPhysicalDeviceVideoFormatInfoKHR *info,
	uint32_t *count,
	VkVideoFormatPropertiesKHR *formats)
{
	(void)physical;
	(void)info;
	if (formats == NULL) {
		*count = 1U;
		return VK_SUCCESS;
	}
	formats[0].format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	formats[0].imageType = VK_IMAGE_TYPE_2D;
	formats[0].imageTiling = VK_IMAGE_TILING_OPTIMAL;
	formats[0].imageUsageFlags = offered_usage;
	formats[0].imageCreateFlags = offered_flags;
	*count = 1U;
	return VK_SUCCESS;
}

/* Runs the choice on one device's shape and checks whether it gives pictures the GPU holds. */
static void
check(
	const char *name,
	unsigned hold,
	int extensions,
	int distinct,
	uint32_t family,
	uint32_t transfer_family,
	int sampled,
	int expected)
{
	struct vkvideo_runtime video;

	memset(&video, 0, sizeof(video));
	video.fn.vkGetPhysicalDeviceVideoFormatPropertiesKHR = fake_formats;
	video.gpu_hold = hold;
	video.gpu_extensions = extensions;
	video.distinct = distinct;
	video.family = family;
	video.transfer_family = transfer_family;
	video.codec = VKVIDEO_H264;
	video.reference_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	video.capabilities.maxDpbSlots = 17U;
	video.max_references = 4U;
	video.reorder = 2U;
	video.flights = 1U;
	offered_usage = video.reference_usage;
	offered_flags = 0U;
	if (sampled) {
		offered_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		offered_flags = VKVIDEO_PLANE_FLAGS;
	}
	vkvideo_gpu_capabilities(&video);
	checks++;
	if (video.gpu != expected) {
		failures++;
		printf("FAIL %s: gpu %d, expected %d\n", name, video.gpu, expected);
		return;
	}
	if (expected && ((video.reference_usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0U || video.slots != 4U + 2U + 1U + hold + 1U)) {
		failures++;
		printf("FAIL %s: usage 0x%x slots %u\n", name, (unsigned)video.reference_usage, video.slots);
		return;
	}
	printf("ok %s: gpu %d slots %u\n", name, video.gpu, video.slots);
}

int
main(void)
{
	check("one family", 6U, 1, 0, 0U, 0U, 1, 1);
	check("decode family apart from transfer", 6U, 1, 0, 1U, 0U, 1, 1);
	check("not asked", 0U, 1, 0, 1U, 0U, 1, 0);
	check("no export extensions", 6U, 0, 0, 1U, 0U, 1, 0);
	check("distinct output", 6U, 1, 1, 1U, 0U, 1, 0);
	check("no sampled NV12", 6U, 1, 0, 1U, 0U, 0, 0);
	printf("%u checks, %u failed\n", checks, failures);
	printf("gpu-hold: %s\n", failures == 0U ? "PASS" : "FAIL");
	return failures == 0U ? 0 : 1;
}
