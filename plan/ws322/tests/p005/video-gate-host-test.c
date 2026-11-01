/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of libvulkan's video gate (ws322-p005): which codec
 * operations each queue family keeps and which video extensions a physical
 * device names, from the native word's H.264 and H.265 decode bits and the
 * renderer's answer to GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES
 * (userland/desktop/libvulkan/instance.c, included so physical_load_video
 * can be called; the renderer is a stand-in that answers one word per
 * family).  It also checks that the device extension list holds every
 * extension at once.
 *
 *   video-gate-host-test
 */

#include "userland/desktop/libvulkan/instance.c"

#include <stdio.h>

static int passes;
static int failures;

/* The words the stand-in renderer answers, one for each family. */
static uint32_t answer_words[4];
static uint32_t answer_count;
static int asked;

static void
check(
	int ok,
	const char *what)
{
	if (ok) {
		passes++;
		return;
	}
	failures++;
	printf("FAIL %s\n", what);
}

/* The stand-in renderer: answers the family video question with the prepared words. */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reader)
{
	struct vulkan_writer reply;
	uint8_t *copy;
	uint32_t index;

	(void)context;
	(void)reply_capacity;
	asked++;
	vulkan_writer_init(&reply);
	vulkan_write_u32(&reply, writer->opcode);
	vulkan_write_u64(&reply, 1);
	vulkan_write_u32(&reply, answer_count);
	for (index = 0; index < answer_count; index++)
		vulkan_write_u32(&reply, answer_words[index]);
	copy = malloc(reply.bytes);
	memcpy(copy, reply.data, reply.bytes);
	vulkan_reader_init(reader, copy, reply.bytes);
	vulkan_writer_finish(&reply);
	return VK_SUCCESS;
}

void
vulkan_context_error(
	struct vulkan_context *context,
	VkResult error)
{
	(void)context;
	(void)error;
}

struct VkPhysicalDevice_T *
vulkan_physical_device(
	VkPhysicalDevice physical)
{
	return (struct VkPhysicalDevice_T *)physical;
}

/* Runs the family video question for one capset promise and one renderer answer. */
static void
gate(
	VkBool32 h264,
	VkBool32 h265,
	uint32_t family0,
	uint32_t family1,
	uint32_t family1_flags,
	struct VkPhysicalDevice_T *physical,
	struct vulkan_context *context,
	VkQueueFamilyProperties *families)
{
	VkResult status;

	memset(context, 0, sizeof(*context));
	context->video_h264 = h264;
	context->video_h265 = h265;
	memset(physical, 0, sizeof(*physical));
	physical->object.context = context;
	memset(families, 0, 2 * sizeof(*families));
	families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_VIDEO_DECODE_BIT_KHR;
	families[1].queueFlags = family1_flags;
	physical->queue_families = families;
	physical->queue_family_count = 2;
	answer_words[0] = family0;
	answer_words[1] = family1;
	answer_count = 2;
	asked = 0;
	status = physical_load_video(physical);
	check(status == VK_SUCCESS, "gate: the question succeeds");
}

int
main(void)
{
	struct VkPhysicalDevice_T physical;
	struct vulkan_context context;
	VkQueueFamilyProperties families[2];
	VkExtensionProperties names[16];
	uint32_t count;
	uint32_t index;
	uint64_t video;
	VkResult status;
	int h265_named;

	video = VULKAN_DEVICE_SYNCHRONIZATION2 | VULKAN_DEVICE_VIDEO_QUEUE | VULKAN_DEVICE_VIDEO_DECODE_QUEUE;

	/* Neither bit: the renderer is not asked and nothing is named. */
	gate(VK_FALSE, VK_FALSE, 3, 3, VK_QUEUE_VIDEO_DECODE_BIT_KHR, &physical, &context, families);
	check(asked == 0, "none: not asked");
	check(physical.supported_extensions == 0, "none: no extension");

	/* H.264 only, with a renderer that answers both: H.265 is dropped. */
	gate(VK_TRUE, VK_FALSE, 3, 3, VK_QUEUE_VIDEO_DECODE_BIT_KHR, &physical, &context, families);
	check(physical.queue_video_operations[0] == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "h264: family 0 keeps H.264");
	check(physical.queue_video_operations[1] == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "h264: family 1 keeps H.264");
	check(physical.supported_extensions == (video | VULKAN_DEVICE_VIDEO_DECODE_H264), "h264: the H.264 extensions");

	/* H.265 only. */
	gate(VK_FALSE, VK_TRUE, 3, 3, VK_QUEUE_VIDEO_DECODE_BIT_KHR, &physical, &context, families);
	check(physical.queue_video_operations[0] == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, "h265: family 0 keeps H.265");
	check(physical.supported_extensions == (video | VULKAN_DEVICE_VIDEO_DECODE_H265), "h265: the H.265 extensions");

	/* Both bits, the families answering one codec each; encode bits are dropped. */
	gate(VK_TRUE, VK_TRUE, 1 | 0x10000, 2 | 0x20000, VK_QUEUE_VIDEO_DECODE_BIT_KHR, &physical, &context, families);
	check(physical.queue_video_operations[0] == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "both: family 0 H.264");
	check(physical.queue_video_operations[1] == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, "both: family 1 H.265");
	check(physical.supported_extensions == (video | VULKAN_DEVICE_VIDEO_DECODE_H264 | VULKAN_DEVICE_VIDEO_DECODE_H265), "both: both extensions");

	/* Both bits, but the only H.265 family lacks the decode flag: only H.264 is named. */
	gate(VK_TRUE, VK_TRUE, 1, 2, VK_QUEUE_GRAPHICS_BIT, &physical, &context, families);
	check(physical.queue_video_operations[1] == 0, "flagless: family 1 has none");
	check(physical.supported_extensions == (video | VULKAN_DEVICE_VIDEO_DECODE_H264), "flagless: H.264 only");

	/* H.265 promised, but the renderer answers H.264 only: nothing, and the video flags go. */
	gate(VK_FALSE, VK_TRUE, 1, 1, VK_QUEUE_VIDEO_DECODE_BIT_KHR, &physical, &context, families);
	check(physical.supported_extensions == 0, "unanswered: no extension");
	check((families[0].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) == 0, "unanswered: family 0 flag withheld");
	check((families[0].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0, "unanswered: graphics kept");

	/* The promise helper: either bit. */
	context.video_h264 = VK_FALSE;
	context.video_h265 = VK_TRUE;
	check(physical_video_promised(&physical) == VK_TRUE, "promised: H.265 alone");
	context.video_h265 = VK_FALSE;
	check(physical_video_promised(&physical) == VK_FALSE, "promised: neither");

	/* Every device extension at once fits the list, H.265 decode named last. */
	physical.supported_extensions = ~(uint64_t)0;
	status = vkEnumerateDeviceExtensionProperties((VkPhysicalDevice)&physical, NULL, &count, NULL);
	check(status == VK_SUCCESS && count == 15U, "extensions: fifteen");
	count = 16U;
	status = vkEnumerateDeviceExtensionProperties((VkPhysicalDevice)&physical, NULL, &count, names);
	h265_named = 0;
	for (index = 0; index < count; index++) {
		if (strcmp(names[index].extensionName, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME) == 0 &&
		    names[index].specVersion == VK_KHR_VIDEO_DECODE_H265_SPEC_VERSION)
			h265_named = 1;
	}
	check(status == VK_SUCCESS && h265_named, "extensions: H.265 decode named");
	physical.supported_extensions = video | VULKAN_DEVICE_VIDEO_DECODE_H264;
	status = vkEnumerateDeviceExtensionProperties((VkPhysicalDevice)&physical, NULL, &count, NULL);
	check(status == VK_SUCCESS && count == 4U, "extensions: H.264 device names four");

	printf("video-gate: %d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
