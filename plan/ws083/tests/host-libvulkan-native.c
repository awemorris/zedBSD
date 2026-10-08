/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of libvulkan's native video path before a device exists
 * (ws083 R-S6): what instance.c makes of a renderer's queue families and
 * their video codec operations, and what device.c accepts.
 *
 * instance.c and device.c are compiled into this file, so their static
 * functions (physical_load_queues, physical_load_video, device_validate)
 * run against a stand-in transport that answers the queue family queries
 * with records built here by hand.  Checked: with the native word's video
 * promise a decode family keeps its flag and names H.264, and the four
 * extensions are listed; without the promise a renderer's video flags are
 * withheld and nothing more is asked; a promise with no H.264 family, or
 * H.264 named on a family without the decode flag, names nothing; a
 * malformed answer loses the device; the extensions' dependency chain on
 * vkCreateDevice.  (The capability record's native word itself is
 * host-libvulkan-capset.c's.)
 */

#include "instance.c"
#include "device.c"

#include <assert.h>
#include <stdio.h>

/* The session the physical device belongs to. */
static struct vulkan_context context;

/* The instance that owns the physical device. */
static struct VkInstance_T instance;

/* The physical device whose snapshot is loaded. */
static struct VkPhysicalDevice_T physical;

/* What the stand-in renderer reports: its families' flags and the codec operations of each. */
static uint32_t renderer_flags[3];
static uint32_t renderer_operations[3];
static uint32_t renderer_count;

/* A count the video answer gives instead of the right one (0: the right one). */
static uint32_t renderer_video_count;

/* How many video property questions reached the renderer. */
static unsigned video_questions;

/* The reply being built. */
static uint8_t reply[1024];
static size_t reply_bytes;

static void put32(uint32_t value);
static void put64(uint64_t value);
static void physical_reset(VkBool32 promise);
static VkResult physical_snapshot(void);
static VkBool32 extension_listed(const char *name);
static void snapshot_free(void);
static void test_promise(void);
static void test_no_promise(void);
static void test_no_decoder(void);
static void test_malformed(void);
static void test_device_extensions(void);

/*
 * The stand-in transport: answers the queue family and video property questions.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *ctx,
	const struct vulkan_writer *writer,
	size_t capacity,
	struct vulkan_reader *reader)
{
	uint32_t opcode;
	uint32_t asked;
	uint32_t index;
	uint32_t count;

	/* Every transaction belongs to the one session. */
	assert(ctx == &context);
	memcpy(&opcode, writer->data, 4);
	reply_bytes = 0;
	put32(opcode);

	/* The answers. */
	switch (opcode) {
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES:
		/* [physical][present][count][array count]{family}: the count alone, or every family. */
		memcpy(&asked, writer->data + 24, 4);
		put64(1);
		put32(renderer_count);
		if (asked == 0) {
			put64(0);
			break;
		}
		put64(renderer_count);
		for (index = 0; index < renderer_count; index++) {
			/* flags, queues, timestamp bits, granularity. */
			put32(renderer_flags[index]);
			put32(1);
			put32(0);
			put32(1);
			put32(1);
			put32(1);
		}
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES:
		/* [present][count]{operations}. */
		video_questions++;
		count = renderer_count;
		if (renderer_video_count != 0)
			count = renderer_video_count;
		put64(1);
		put32(count);
		for (index = 0; index < count && index < 3; index++)
			put32(renderer_operations[index]);
		break;
	default:
		/* No other question is asked here. */
		assert(0);
		break;
	}

	/* Hands libvulkan its copy. */
	assert(reply_bytes <= capacity);
	vulkan_reader_init(reader, NULL, 0);
	reader->data = malloc(reply_bytes);
	assert(reader->data != NULL);
	memcpy(reader->data, reply, reply_bytes);
	reader->bytes = reply_bytes;
	return VK_SUCCESS;
}

/* The queue's teardown, which no scenario here reaches. */
void
vulkan_queue_finish(
	struct VkQueue_T *queue)
{
	/* Not reached. */
	(void)queue;
	assert(0);
}

/* The window system's device teardown, which no scenario here reaches. */
void
vulkan_wsi_device_finish(
	struct VkDevice_T *device)
{
	/* Not reached. */
	(void)device;
	assert(0);
}

/* The window system's instance teardown, which no scenario here reaches. */
void
vulkan_wsi_instance_finish(
	struct VkInstance_T *owner)
{
	/* Not reached. */
	(void)owner;
	assert(0);
}

/* Runs every scenario and reports the result on one line. */
int
main(void)
{
	/* The scenarios. */
	test_promise();
	test_no_promise();
	test_no_decoder();
	test_malformed();
	test_device_extensions();

	/* Every scenario passed. */
	printf("ws083 libvulkan native video path host test PASS\n");
	return 0;
}

/* Appends one little-endian word to the reply. */
static void
put32(
	uint32_t value)
{
	/* The protocol is little-endian. */
	reply[reply_bytes++] = (uint8_t)value;
	reply[reply_bytes++] = (uint8_t)(value >> 8);
	reply[reply_bytes++] = (uint8_t)(value >> 16);
	reply[reply_bytes++] = (uint8_t)(value >> 24);
}

/* Appends one little-endian double word to the reply. */
static void
put64(
	uint64_t value)
{
	/* The low word first. */
	put32((uint32_t)value);
	put32((uint32_t)(value >> 32));
}

/* A fresh physical device of a session with or without the native word's video promise. */
static void
physical_reset(
	VkBool32 promise)
{
	/* A session that may make devices (strict queue, quiescence, jobs). */
	memset(&context, 0, sizeof(context));
	context.fd = -1;
	context.video_h264 = promise;
	context.strict_queue = VK_TRUE;
	context.native_quiescence = VK_TRUE;
	context.capabilities = GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY;

	/* An instance with the extensible physical-device queries. */
	memset(&instance, 0, sizeof(instance));
	instance.enabled_extensions = VULKAN_INSTANCE_PROPERTIES2;

	/* The physical device, before its snapshot. */
	memset(&physical, 0, sizeof(physical));
	physical.object.kind = VULKAN_OBJECT_PHYSICAL_DEVICE;
	physical.object.context = &context;
	physical.object.wire_id = 0x1111;
	physical.instance = &instance;
	video_questions = 0;
	renderer_video_count = 0;
}

/* Loads the queue families and their video operations, as physical_load does after the properties. */
static VkResult
physical_snapshot(void)
{
	VkResult status;

	/* The families, then the video operations. */
	status = physical_load_queues(&physical);
	if (status != VK_SUCCESS)
		return status;
	status = physical_load_video(&physical);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the snapshot. */
	return VK_SUCCESS;
}

/* Frees the snapshot's families (the physical device itself is this file's, not allocated). */
static void
snapshot_free(void)
{
	/* The families, when the snapshot got that far. */
	vulkan_free(&physical.object.allocator, physical.queue_families);
	physical.queue_families = NULL;
	physical.queue_family_count = 0;
}

/* Tells whether the physical device lists a device extension. */
static VkBool32
extension_listed(
	const char *name)
{
	VkExtensionProperties properties[16];
	uint32_t count;
	uint32_t index;
	VkResult status;

	/* Every extension. */
	count = 16;
	status = vkEnumerateDeviceExtensionProperties(&physical, NULL, &count, properties);
	assert(status == VK_SUCCESS);
	for (index = 0; index < count; index++) {
		if (strcmp(properties[index].extensionName, name) == 0)
			return VK_TRUE;
	}

	/* Not listed. */
	return VK_FALSE;
}

/* The native promise with a graphics family and an H.264 decode family. */
static void
test_promise(void)
{
	VkResult status;

	/* Family 0 draws, family 1 decodes H.264 (and the renderer says nothing of encode). */
	physical_reset(VK_TRUE);
	renderer_count = 2;
	renderer_flags[0] = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
	renderer_flags[1] = VK_QUEUE_VIDEO_DECODE_BIT_KHR;
	renderer_operations[0] = 0;
	renderer_operations[1] = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	status = physical_snapshot();
	assert(status == VK_SUCCESS);
	assert(video_questions == 1);

	/* The decode family keeps its flag and names H.264; the four extensions are listed. */
	assert(physical.queue_family_count == 2);
	assert(physical.queue_families[1].queueFlags == VK_QUEUE_VIDEO_DECODE_BIT_KHR);
	assert(physical.queue_video_operations[0] == 0);
	assert(physical.queue_video_operations[1] == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);
	assert(extension_listed(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME));
	assert(extension_listed(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME));
	assert(extension_listed(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME));
	assert(extension_listed(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME));
	snapshot_free();
}

/* Without the promise (Venus over a host driver with video): the flags are withheld and nothing is asked. */
static void
test_no_promise(void)
{
	VkResult status;

	/* The host driver's family 1 has the decode flag (and the encode bit 0x40). */
	physical_reset(VK_FALSE);
	renderer_count = 2;
	renderer_flags[0] = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_VIDEO_DECODE_BIT_KHR;
	renderer_flags[1] = VK_QUEUE_VIDEO_DECODE_BIT_KHR | 0x40U | VK_QUEUE_TRANSFER_BIT;
	status = physical_snapshot();
	assert(status == VK_SUCCESS);
	assert(video_questions == 0);
	assert(physical.queue_families[0].queueFlags == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT));
	assert(physical.queue_families[1].queueFlags == VK_QUEUE_TRANSFER_BIT);
	assert(!extension_listed(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME));
	assert(!extension_listed(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME));
	snapshot_free();
}

/* A promise but no family that decodes H.264, and H.264 named on a family without the decode flag: nothing named. */
static void
test_no_decoder(void)
{
	VkResult status;

	/* Family 0 (no decode flag) names H.264; family 1 has the flag and names nothing. */
	physical_reset(VK_TRUE);
	renderer_count = 2;
	renderer_flags[0] = VK_QUEUE_GRAPHICS_BIT;
	renderer_flags[1] = VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_TRANSFER_BIT;
	renderer_operations[0] = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	renderer_operations[1] = 0;
	status = physical_snapshot();
	assert(status == VK_SUCCESS);
	assert(video_questions == 1);
	assert(physical.queue_video_operations[0] == 0);
	assert(physical.queue_video_operations[1] == 0);
	assert(physical.queue_families[1].queueFlags == VK_QUEUE_TRANSFER_BIT);
	assert(!extension_listed(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME));
	snapshot_free();
}

/* An answer for another number of families loses the device. */
static void
test_malformed(void)
{
	VkResult status;

	/* Two families, one answer. */
	physical_reset(VK_TRUE);
	renderer_count = 2;
	renderer_flags[0] = VK_QUEUE_GRAPHICS_BIT;
	renderer_flags[1] = VK_QUEUE_VIDEO_DECODE_BIT_KHR;
	renderer_operations[0] = 0;
	renderer_operations[1] = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	renderer_video_count = 1;
	status = physical_snapshot();
	assert(status == VK_ERROR_DEVICE_LOST);
	snapshot_free();
}

/* vkCreateDevice's extension checks: the four together, each without the one below, and without the promise. */
static void
test_device_extensions(void)
{
	static const char *const all[] = {
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
		VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME
	};
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo info;
	uint64_t extensions;
	uint32_t queues;
	float priority;
	VkResult status;

	/* The physical device with the decode family. */
	physical_reset(VK_TRUE);
	renderer_count = 2;
	renderer_flags[0] = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
	renderer_flags[1] = VK_QUEUE_VIDEO_DECODE_BIT_KHR;
	renderer_operations[0] = 0;
	renderer_operations[1] = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	status = physical_snapshot();
	assert(status == VK_SUCCESS);

	/* One queue of the decode family. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = 1;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.queueCreateInfoCount = 1;
	info.pQueueCreateInfos = &queue;

	/* The four together. */
	info.enabledExtensionCount = 4;
	info.ppEnabledExtensionNames = all;
	status = device_validate(&physical, &info, &extensions, &queues);
	assert(status == VK_SUCCESS);
	assert(extensions == (VULKAN_DEVICE_SYNCHRONIZATION2 | VULKAN_DEVICE_VIDEO_QUEUE | VULKAN_DEVICE_VIDEO_DECODE_QUEUE | VULKAN_DEVICE_VIDEO_DECODE_H264));
	assert(queues == 1);

	/* H.264 decode without the decode queue, the decode queue without the video queue, the video queue without synchronization2. */
	info.enabledExtensionCount = 1;
	info.ppEnabledExtensionNames = all + 3;
	assert(device_validate(&physical, &info, &extensions, &queues) == VK_ERROR_EXTENSION_NOT_PRESENT);
	info.enabledExtensionCount = 2;
	info.ppEnabledExtensionNames = all + 2;
	assert(device_validate(&physical, &info, &extensions, &queues) == VK_ERROR_EXTENSION_NOT_PRESENT);
	info.enabledExtensionCount = 3;
	info.ppEnabledExtensionNames = all + 1;
	assert(device_validate(&physical, &info, &extensions, &queues) == VK_ERROR_EXTENSION_NOT_PRESENT);

	/* synchronization2 without the instance's extensible queries. */
	instance.enabled_extensions = 0;
	info.enabledExtensionCount = 4;
	info.ppEnabledExtensionNames = all;
	assert(device_validate(&physical, &info, &extensions, &queues) == VK_ERROR_EXTENSION_NOT_PRESENT);
	instance.enabled_extensions = VULKAN_INSTANCE_PROPERTIES2;
	snapshot_free();

	/* Without the promise the four are not the device's. */
	physical_reset(VK_FALSE);
	renderer_count = 2;
	renderer_flags[0] = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
	renderer_flags[1] = VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_TRANSFER_BIT;
	status = physical_snapshot();
	assert(status == VK_SUCCESS);
	queue.queueFamilyIndex = 0;
	assert(device_validate(&physical, &info, &extensions, &queues) == VK_ERROR_EXTENSION_NOT_PRESENT);
	snapshot_free();
}
