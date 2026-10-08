/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of libvulkan's Vulkan Video and synchronization2 support (ws083-p002).
 *
 * video.c, sync2.c and external-properties.c run against a stand-in
 * transport that keeps every request and answers with replies built here by
 * hand, byte by byte, so the wire form of design.md section 4.2 is checked
 * independently of the encoders under test.  The 1.0 commands sync2.c
 * translates into are stand-ins that keep their arguments.
 */

#include "internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The session the physical device, device and command buffer belong to. */
static struct vulkan_context context;

/* The instance that owns the physical device; only its extension bits are read. */
static struct VkInstance_T instance;

/* The physical device with one graphics family and one H.264 decode family. */
static struct VkPhysicalDevice_T physical;

/* The two queue families of the physical device. */
static VkQueueFamilyProperties families[2];

/* The logical device every video object is created on. */
static struct VkDevice_T device;

/* The command buffer the video commands are recorded into. */
static struct VkCommandBuffer_T command;

/* The last request the transport saw, copied out of the library's writer. */
static uint8_t request[16384];

/* The size of the last request. */
static size_t request_bytes;

/* The read position in the last request, used by the checks below. */
static size_t request_cursor;

/* How many transactions reached the transport. */
static unsigned transactions;

/* The reply being built for the current transaction. */
static uint8_t reply[16384];

/* The size of the reply being built. */
static size_t reply_bytes;

/* How many formats the format query answers with. */
static uint32_t format_count;

/* The 1.0 commands the synchronization2 translation called, in order. */
static char calls[64][32];

/* How many 1.0 commands were called. */
static unsigned call_count;

/* The stage masks of the last 1.0 barrier or wait. */
static VkPipelineStageFlags last_source;
static VkPipelineStageFlags last_destination;

/* The barrier counts of every 1.0 barrier or wait, in order. */
static uint32_t barrier_counts[64][3];

/* The access masks of the first global memory barrier of the last call. */
static VkAccessFlags last_memory_source;
static VkAccessFlags last_memory_destination;

/* The last event and stage the event commands named. */
static VkEvent last_event;
static VkPipelineStageFlags last_stage;

/* The translated batches of the last 1.0 submission, copied out. */
static uint32_t submit_count;
static uint32_t submit_waits[4];
static VkPipelineStageFlags submit_wait_stages[4][4];
static VkSemaphore submit_wait_semaphores[4][4];
static uint32_t submit_commands[4];
static VkCommandBuffer submit_command_buffers[4][4];
static uint32_t submit_signals[4];
static VkSemaphore submit_signal_semaphores[4][4];

static void put32(uint32_t value);
static void put64(uint64_t value);
static uint32_t take32(void);
static uint64_t take64(void);
static void expect32(uint32_t value);
static void expect64(uint64_t value);
static void request_begin(uint32_t opcode, VkBool32 reply_requested);
static void record_begin(size_t *cursor, uint32_t opcode);
static void note_call(const char *name);
static void test_capabilities(void);
static void test_formats(void);
static void test_session(void);
static void test_parameters(void);
static void test_recording(void);
static void test_sync2(void);
static void test_properties(void);

/*
 * The stand-in transport: keeps the request and answers with a hand-built reply.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *ctx,
	const struct vulkan_writer *writer,
	size_t capacity,
	struct vulkan_reader *reader)
{
	uint32_t opcode;
	uint32_t index;
	uint64_t identity;

	/* Every transaction belongs to the one session. */
	assert(ctx == &context);
	assert(writer->bytes <= sizeof(request));
	memcpy(request, writer->data, writer->bytes);
	request_bytes = writer->bytes;
	request_cursor = 0;
	transactions++;

	/* Every reply starts with the opcode it answers. */
	opcode = (uint32_t)writer->data[0] | ((uint32_t)writer->data[1] << 8) | ((uint32_t)writer->data[2] << 16) | ((uint32_t)writer->data[3] << 24);
	reply_bytes = 0;
	put32(opcode);

	/* Builds the answer of each command by hand. */
	switch (opcode) {
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_CAPABILITIES:
		/* The capabilities record with the decode and H.264 records nested in its chain. */
		put32(VK_SUCCESS);
		put64(1);
		put32(VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR);
		put64(1);
		put32(VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR);
		put64(1);
		put32(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR);
		put64(0);
		put32(STD_VIDEO_H264_LEVEL_IDC_5_1);
		put32(0);
		put32(0);
		put32(VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR);
		put32(VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR);
		put64(32);
		put64(1);
		put32(16);
		put32(16);
		put32(16);
		put32(16);
		put32(4096);
		put32(4096);
		put32(17);
		put32(16);
		put64(256);
		memset(reply + reply_bytes, 0, 256);
		memcpy(reply + reply_bytes, "VK_STD_vulkan_video_codec_h264_decode", 37);
		reply_bytes += 256;
		put32(VK_MAKE_VIDEO_STD_VERSION(1, 0, 0));
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_FORMAT_PROPERTIES:
		/* NV12 in the decoder's tiling, as often as the test asked. */
		put32(VK_SUCCESS);
		put64(1);
		put32(format_count);
		put64(format_count);
		for (index = 0; index < format_count; index++) {
			put32(VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR);
			put64(0);
			put32(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
			put32(0);
			put32(0);
			put32(0);
			put32(index);
			put32(0);
			put32(VK_IMAGE_TYPE_2D);
			put32(VK_IMAGE_TILING_OPTIMAL);
			put32(VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR);
		}
		break;
	case GPU_OP_CREATE_VIDEO_SESSION:
	case GPU_OP_CREATE_VIDEO_SESSION_PARAMETERS:
		/* The created object's identity, which is the last word of the request. */
		memcpy(&identity, writer->data + writer->bytes - 8, 8);
		put32(VK_SUCCESS);
		put64(1);
		put64(identity);
		break;
	case GPU_OP_GET_VIDEO_SESSION_MEMORY_REQUIREMENTS:
		/* Three bindings: two row stores and one motion vector buffer. */
		put32(VK_SUCCESS);
		put64(1);
		put32(3);
		put64(3);
		for (index = 0; index < 3; index++) {
			put32(VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR);
			put64(0);
			put32(index);
			put64(4096 * (index + 1));
			put64(4096);
			put32(1);
		}
		break;
	case GPU_OP_BIND_VIDEO_SESSION_MEMORY:
	case GPU_OP_UPDATE_VIDEO_SESSION_PARAMETERS:
		/* The renderer accepts the change. */
		put32(VK_SUCCESS);
		break;
	default:
		/* A destroy is answered with its opcode alone. */
		break;
	}

	/* Hands the library its own copy, as the real transport does. */
	assert(reply_bytes <= capacity);
	memset(reader, 0, sizeof(*reader));
	reader->data = malloc(reply_bytes);
	assert(reader->data != NULL);
	memcpy(reader->data, reply, reply_bytes);
	reader->bytes = reply_bytes;

	/* Succeeded: the reply is complete. */
	return VK_SUCCESS;
}

/*
 * The stand-in for the ordinary recording framing of commands.c: opcode, no reply, command buffer.
 */
VkBool32
vulkan_command_record_begin(
	struct VkCommandBuffer_T *buffer,
	struct vulkan_writer *writer,
	uint32_t opcode)
{
	/* The same three words commands.c writes in front of every record. */
	vulkan_writer_init_for_object(writer, &buffer->object);
	writer->opcode = opcode;
	vulkan_write_u32(writer, opcode);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, buffer->object.wire_id);

	/* Succeeded: the record is open. */
	return VK_TRUE;
}

/*
 * The stand-in that appends a finished record to its command buffer.
 */
void
vulkan_command_record_finish(
	struct VkCommandBuffer_T *buffer,
	struct vulkan_writer *writer)
{
	/* The record must have encoded without error. */
	assert(writer->error == VK_SUCCESS);
	vulkan_write_bytes(&buffer->recording, writer->data, writer->bytes);
	vulkan_writer_finish(writer);
}

/* The 1.0 barrier the synchronization2 barrier translates into. */
VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier(
	VkCommandBuffer commandBuffer,
	VkPipelineStageFlags srcStageMask,
	VkPipelineStageFlags dstStageMask,
	VkDependencyFlags dependencyFlags,
	uint32_t memoryBarrierCount,
	const VkMemoryBarrier *pMemoryBarriers,
	uint32_t bufferMemoryBarrierCount,
	const VkBufferMemoryBarrier *pBufferMemoryBarriers,
	uint32_t imageMemoryBarrierCount,
	const VkImageMemoryBarrier *pImageMemoryBarriers)
{
	/* Keeps the masks and counts for the checks. */
	assert(commandBuffer == &command);
	assert(dependencyFlags == VK_DEPENDENCY_BY_REGION_BIT);
	barrier_counts[call_count][0] = memoryBarrierCount;
	barrier_counts[call_count][1] = bufferMemoryBarrierCount;
	barrier_counts[call_count][2] = imageMemoryBarrierCount;
	note_call("barrier");
	last_source = srcStageMask;
	last_destination = dstStageMask;
	if (memoryBarrierCount != 0) {
		last_memory_source = pMemoryBarriers[0].srcAccessMask;
		last_memory_destination = pMemoryBarriers[0].dstAccessMask;
	}

	/* An image barrier keeps its layouts and families. */
	if (imageMemoryBarrierCount != 0) {
		assert(pImageMemoryBarriers[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
		assert(pImageMemoryBarriers[0].newLayout == VK_IMAGE_LAYOUT_GENERAL);
		assert(pImageMemoryBarriers[0].srcQueueFamilyIndex == 1);
		assert(pImageMemoryBarriers[0].dstQueueFamilyIndex == 0);
		assert(pImageMemoryBarriers[0].subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
	}

	/* A buffer barrier keeps its range. */
	if (bufferMemoryBarrierCount != 0) {
		assert(pBufferMemoryBarriers[0].offset == 64);
		assert(pBufferMemoryBarriers[0].size == 128);
	}
}

/* The 1.0 wait the synchronization2 wait translates into. */
VKAPI_ATTR void VKAPI_CALL
vkCmdWaitEvents(
	VkCommandBuffer commandBuffer,
	uint32_t eventCount,
	const VkEvent *pEvents,
	VkPipelineStageFlags srcStageMask,
	VkPipelineStageFlags dstStageMask,
	uint32_t memoryBarrierCount,
	const VkMemoryBarrier *pMemoryBarriers,
	uint32_t bufferMemoryBarrierCount,
	const VkBufferMemoryBarrier *pBufferMemoryBarriers,
	uint32_t imageMemoryBarrierCount,
	const VkImageMemoryBarrier *pImageMemoryBarriers)
{
	/* Keeps the event, masks and counts for the checks; each wait names one event. */
	(void)pMemoryBarriers;
	(void)pBufferMemoryBarriers;
	(void)pImageMemoryBarriers;
	assert(commandBuffer == &command);
	assert(eventCount == 1);
	barrier_counts[call_count][0] = memoryBarrierCount;
	barrier_counts[call_count][1] = bufferMemoryBarrierCount;
	barrier_counts[call_count][2] = imageMemoryBarrierCount;
	note_call("wait");
	last_event = pEvents[0];
	last_source = srcStageMask;
	last_destination = dstStageMask;
}

/* The 1.0 signal the synchronization2 signal translates into. */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetEvent(
	VkCommandBuffer commandBuffer,
	VkEvent event,
	VkPipelineStageFlags stageMask)
{
	/* Keeps the event and stage. */
	assert(commandBuffer == &command);
	note_call("set");
	last_event = event;
	last_stage = stageMask;
}

/* The 1.0 reset the synchronization2 reset translates into. */
VKAPI_ATTR void VKAPI_CALL
vkCmdResetEvent(
	VkCommandBuffer commandBuffer,
	VkEvent event,
	VkPipelineStageFlags stageMask)
{
	/* Keeps the event and stage. */
	assert(commandBuffer == &command);
	note_call("reset");
	last_event = event;
	last_stage = stageMask;
}

/* The 1.0 timestamp the synchronization2 timestamp translates into. */
VKAPI_ATTR void VKAPI_CALL
vkCmdWriteTimestamp(
	VkCommandBuffer commandBuffer,
	VkPipelineStageFlagBits pipelineStage,
	VkQueryPool queryPool,
	uint32_t query)
{
	/* Keeps the stage; the pool and query pass through. */
	assert(commandBuffer == &command);
	assert(query == 7);
	assert(queryPool == (VkQueryPool)(uintptr_t)0x5150);
	note_call("timestamp");
	last_stage = (VkPipelineStageFlags)pipelineStage;
}

/* The 1.0 submission the synchronization2 submission translates into. */
VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(
	VkQueue queue,
	uint32_t submitCount,
	const VkSubmitInfo *pSubmits,
	VkFence fence)
{
	uint32_t index;
	uint32_t item;

	/* Copies every batch out before the translation frees its block. */
	assert(queue == (VkQueue)(uintptr_t)0x9000);
	assert(fence == (VkFence)(uintptr_t)0x9100);
	note_call("submit");
	submit_count = submitCount;
	for (index = 0; index < submitCount; index++) {
		assert(pSubmits[index].sType == VK_STRUCTURE_TYPE_SUBMIT_INFO);
		submit_waits[index] = pSubmits[index].waitSemaphoreCount;
		submit_commands[index] = pSubmits[index].commandBufferCount;
		submit_signals[index] = pSubmits[index].signalSemaphoreCount;
		for (item = 0; item < pSubmits[index].waitSemaphoreCount; item++) {
			submit_wait_semaphores[index][item] = pSubmits[index].pWaitSemaphores[item];
			submit_wait_stages[index][item] = pSubmits[index].pWaitDstStageMask[item];
		}
		for (item = 0; item < pSubmits[index].commandBufferCount; item++)
			submit_command_buffers[index][item] = pSubmits[index].pCommandBuffers[item];
		for (item = 0; item < pSubmits[index].signalSemaphoreCount; item++)
			submit_signal_semaphores[index][item] = pSubmits[index].pSignalSemaphores[item];
	}

	/* Succeeded: the stand-in accepts the batches. */
	return VK_SUCCESS;
}

/* The core feature query, which the features2 wrapper calls first. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceFeatures *pFeatures)
{
	/* No core feature matters here. */
	(void)physicalDevice;
	memset(pFeatures, 0, sizeof(*pFeatures));
}

/* The core format query, unused by the paths tested. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties(
	VkPhysicalDevice physicalDevice,
	VkFormat format,
	VkFormatProperties *pFormatProperties)
{
	/* Not reached. */
	(void)physicalDevice;
	(void)format;
	(void)pFormatProperties;
	assert(0);
}

/* The core image query, which answers a checked decode picture with its limits. */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties(
	VkPhysicalDevice physicalDevice,
	VkFormat format,
	VkImageType type,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkImageCreateFlags flags,
	VkImageFormatProperties *pImageFormatProperties)
{
	/* Only a checked decode picture reaches the renderer. */
	assert(physicalDevice == &physical);
	assert(format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	assert(type == VK_IMAGE_TYPE_2D);
	assert(tiling == VK_IMAGE_TILING_OPTIMAL);
	assert(usage == (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR));
	assert(flags == 0);
	memset(pImageFormatProperties, 0, sizeof(*pImageFormatProperties));
	pImageFormatProperties->maxExtent.width = 4096;
	pImageFormatProperties->maxExtent.height = 4096;
	pImageFormatProperties->maxExtent.depth = 1;
	pImageFormatProperties->maxMipLevels = 1;
	pImageFormatProperties->maxArrayLayers = 1;
	pImageFormatProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;

	/* Succeeded: the picture's limits. */
	return VK_SUCCESS;
}

/* The remaining core queries the properties2 wrappers call; not reached here. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
	/* Not reached. */
	(void)physicalDevice;
	(void)pMemoryProperties;
	assert(0);
}

/* The core sparse query; not reached here. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceSparseImageFormatProperties(
	VkPhysicalDevice physicalDevice,
	VkFormat format,
	VkImageType type,
	VkSampleCountFlagBits samples,
	VkImageUsageFlags usage,
	VkImageTiling tiling,
	uint32_t *pPropertyCount,
	VkSparseImageFormatProperties *pProperties)
{
	/* Not reached. */
	(void)physicalDevice;
	(void)format;
	(void)type;
	(void)samples;
	(void)usage;
	(void)tiling;
	(void)pPropertyCount;
	(void)pProperties;
	assert(0);
}

/* Runs every scenario and reports the result on one line. */
int
main(void)
{
	/* A session whose capability record carried the native word's video bit. */
	memset(&context, 0, sizeof(context));
	context.fd = -1;
	context.video_h264 = VK_TRUE;

	/* An instance with the extensible physical-device queries enabled. */
	memset(&instance, 0, sizeof(instance));
	instance.enabled_extensions = VULKAN_INSTANCE_PROPERTIES2;

	/* Family 0 draws; family 1 decodes H.264. */
	memset(families, 0, sizeof(families));
	families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
	families[0].queueCount = 1;
	families[1].queueFlags = VK_QUEUE_VIDEO_DECODE_BIT_KHR;
	families[1].queueCount = 1;

	/* The physical device names the video extensions and synchronization2. */
	memset(&physical, 0, sizeof(physical));
	physical.object.kind = VULKAN_OBJECT_PHYSICAL_DEVICE;
	physical.object.context = &context;
	physical.object.wire_id = 0x1111;
	physical.instance = &instance;
	physical.queue_families = families;
	physical.queue_family_count = 2;
	physical.queue_video_operations[1] = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	physical.supported_extensions = VULKAN_DEVICE_SYNCHRONIZATION2 | VULKAN_DEVICE_VIDEO_QUEUE | VULKAN_DEVICE_VIDEO_DECODE_QUEUE | VULKAN_DEVICE_VIDEO_DECODE_H264;

	/* The logical device that enabled them. */
	memset(&device, 0, sizeof(device));
	device.object.kind = VULKAN_OBJECT_DEVICE;
	device.object.context = &context;
	device.object.wire_id = 0x2222;
	device.physical = &physical;
	device.enabled_extensions = physical.supported_extensions;

	/* A command buffer in the recording state. */
	memset(&command, 0, sizeof(command));
	command.object.kind = VULKAN_OBJECT_COMMAND_BUFFER;
	command.object.context = &context;
	command.object.wire_id = 0x3333;
	vulkan_writer_init(&command.recording);

	/* The scenarios. */
	test_capabilities();
	test_formats();
	test_session();
	test_parameters();
	test_recording();
	test_sync2();
	test_properties();

	/* Every scenario passed. */
	vulkan_writer_finish(&command.recording);
	printf("ws083 libvulkan video host test PASS\n");
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

/* Reads the next word of the last request. */
static uint32_t
take32(void)
{
	uint32_t value;

	/* The request must hold the word. */
	assert(request_cursor + 4 <= request_bytes);
	value = (uint32_t)request[request_cursor] |
	        ((uint32_t)request[request_cursor + 1] << 8) |
	        ((uint32_t)request[request_cursor + 2] << 16) |
	        ((uint32_t)request[request_cursor + 3] << 24);
	request_cursor += 4;

	/* Reports the word. */
	return value;
}

/* Reads the next double word of the last request. */
static uint64_t
take64(void)
{
	uint64_t low;
	uint64_t high;

	/* The low word first. */
	low = take32();
	high = take32();

	/* Reports the double word. */
	return low | (high << 32);
}

/* Requires the next word of the last request to be this value. */
static void
expect32(
	uint32_t value)
{
	uint32_t actual;

	/* Prints the position of a mismatch before failing. */
	actual = take32();
	if (actual != value) {
		fprintf(stderr, "word at %lu: 0x%x, expected 0x%x\n", (unsigned long)(request_cursor - 4), actual, value);
		abort();
	}
}

/* Requires the next double word of the last request to be this value. */
static void
expect64(
	uint64_t value)
{
	uint64_t actual;

	/* Prints the position of a mismatch before failing. */
	actual = take64();
	if (actual != value) {
		fprintf(stderr, "double word at %lu: 0x%llx, expected 0x%llx\n", (unsigned long)(request_cursor - 8), (unsigned long long)actual, (unsigned long long)value);
		abort();
	}
}

/* Requires the last request to start with this command header. */
static void
request_begin(
	uint32_t opcode,
	VkBool32 reply_requested)
{
	/* The opcode and the reply flag. */
	request_cursor = 0;
	expect32(opcode);
	expect32(reply_requested ? 1 : 0);
}

/* Requires a recorded command to start at the cursor with this header. */
static void
record_begin(
	size_t *cursor,
	uint32_t opcode)
{
	/* Points the request reader at the record within the command buffer. */
	memcpy(request, command.recording.data, command.recording.bytes);
	request_bytes = command.recording.bytes;
	request_cursor = *cursor;
	expect32(opcode);
	expect32(0);
	expect64(command.object.wire_id);
}

/* Remembers the name of one translated 1.0 call. */
static void
note_call(
	const char *name)
{
	/* The log is long enough for every scenario. */
	assert(call_count < 64);
	strcpy(calls[call_count], name);
	call_count++;
}

/* The profile checks and the capability query's wire form and answer. */
static void
test_capabilities(void)
{
	VkVideoDecodeH264ProfileInfoKHR h264;
	VkVideoProfileInfoKHR profile;
	VkVideoDecodeH264CapabilitiesKHR h264_capabilities;
	VkVideoDecodeCapabilitiesKHR decode_capabilities;
	VkVideoCapabilitiesKHR capabilities;
	VkResult status;
	unsigned before;

	/* An H.264 Main progressive profile, 8-bit 4:2:0. */
	memset(&h264, 0, sizeof(h264));
	h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
	h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
	h264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
	memset(&profile, 0, sizeof(profile));
	profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	profile.pNext = &h264;
	profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

	/* The caller's output chain: capabilities, decode, H.264. */
	memset(&h264_capabilities, 0, sizeof(h264_capabilities));
	h264_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
	memset(&decode_capabilities, 0, sizeof(decode_capabilities));
	decode_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
	decode_capabilities.pNext = &h264_capabilities;
	memset(&capabilities, 0, sizeof(capabilities));
	capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
	capabilities.pNext = &decode_capabilities;

	/* Unsupported profiles are refused locally, each with its standard reason. */
	before = transactions;
	profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
	status = vkGetPhysicalDeviceVideoCapabilitiesKHR(&physical, &profile, &capabilities);
	assert(status == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR);
	profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
	status = vkGetPhysicalDeviceVideoCapabilitiesKHR(&physical, &profile, &capabilities);
	assert(status == VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR);
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	h264.pictureLayout = 1;
	status = vkGetPhysicalDeviceVideoCapabilitiesKHR(&physical, &profile, &capabilities);
	assert(status == VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR);
	h264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
	h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE;
	status = vkGetPhysicalDeviceVideoCapabilitiesKHR(&physical, &profile, &capabilities);
	assert(status == VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR);
	h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
	profile.pNext = NULL;
	status = vkGetPhysicalDeviceVideoCapabilitiesKHR(&physical, &profile, &capabilities);
	assert(status == VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR);
	profile.pNext = &h264;
	assert(transactions == before);

	/* A physical device without video decode supports no profile. */
	physical.supported_extensions &= ~(uint64_t)VULKAN_DEVICE_VIDEO_DECODE_H264;
	status = vkGetPhysicalDeviceVideoCapabilitiesKHR(&physical, &profile, &capabilities);
	assert(status == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR);
	physical.supported_extensions |= VULKAN_DEVICE_VIDEO_DECODE_H264;

	/* The supported profile reaches the renderer. */
	status = vkGetPhysicalDeviceVideoCapabilitiesKHR(&physical, &profile, &capabilities);
	assert(status == VK_SUCCESS);
	assert(transactions == before + 1);

	/* The request: physical, the profile with its H.264 record chained, then the output shape. */
	request_begin(GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_CAPABILITIES, VK_TRUE);
	expect64(0x1111);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR);
	expect64(0);
	expect32(STD_VIDEO_H264_PROFILE_IDC_MAIN);
	expect32(VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR);
	expect32(VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);
	expect32(VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
	expect32(VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR);
	expect32(VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR);
	expect64(0);
	assert(request_cursor == request_bytes);

	/* The answer fills every record and keeps the caller's chain links. */
	assert(capabilities.pNext == &decode_capabilities);
	assert(decode_capabilities.pNext == &h264_capabilities);
	assert(capabilities.flags == VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR);
	assert(capabilities.minBitstreamBufferOffsetAlignment == 32);
	assert(capabilities.minBitstreamBufferSizeAlignment == 1);
	assert(capabilities.pictureAccessGranularity.width == 16);
	assert(capabilities.minCodedExtent.height == 16);
	assert(capabilities.maxCodedExtent.width == 4096);
	assert(capabilities.maxCodedExtent.height == 4096);
	assert(capabilities.maxDpbSlots == 17);
	assert(capabilities.maxActiveReferencePictures == 16);
	assert(strcmp(capabilities.stdHeaderVersion.extensionName, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME) == 0);
	assert(capabilities.stdHeaderVersion.specVersion == VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION);
	assert(decode_capabilities.flags == VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR);
	assert(h264_capabilities.maxLevelIdc == STD_VIDEO_H264_LEVEL_IDC_5_1);
}

/* The format query's local checks, wire form and truncation. */
static void
test_formats(void)
{
	VkVideoDecodeH264ProfileInfoKHR h264;
	VkVideoProfileInfoKHR profile;
	VkVideoProfileListInfoKHR list;
	VkPhysicalDeviceVideoFormatInfoKHR info;
	VkVideoFormatPropertiesKHR formats[2];
	VkResult status;
	uint32_t count;
	unsigned before;

	/* An H.264 Baseline profile in a one-profile list. */
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
	list.profileCount = 1;
	list.pProfiles = &profile;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
	info.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_SAMPLED_BIT;

	/* Without a profile list, and with a usage beyond decode, the query is refused locally. */
	before = transactions;
	status = vkGetPhysicalDeviceVideoFormatPropertiesKHR(&physical, &info, &count, NULL);
	assert(status == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR);
	info.pNext = &list;
	status = vkGetPhysicalDeviceVideoFormatPropertiesKHR(&physical, &info, &count, NULL);
	assert(status == VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR);
	assert(transactions == before);

	/* The count query. */
	info.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
	format_count = 2;
	status = vkGetPhysicalDeviceVideoFormatPropertiesKHR(&physical, &info, &count, NULL);
	assert(status == VK_SUCCESS);
	assert(count == 2);

	/* The request: physical, the format info with the profile list chained, then the capacity. */
	request_begin(GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_FORMAT_PROPERTIES, VK_TRUE);
	expect64(0x1111);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR);
	expect64(0);
	expect32(1);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR);
	expect64(0);
	expect32(STD_VIDEO_H264_PROFILE_IDC_BASELINE);
	expect32(0);
	expect32(VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);
	expect32(VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
	expect32(VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR);
	expect32(VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR);
	expect32(VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR);
	expect64(1);
	expect32(32);
	expect64(32);
	assert(request_cursor == request_bytes);

	/* A one-element array receives the first format and VK_INCOMPLETE; its header is kept. */
	memset(formats, 0, sizeof(formats));
	formats[0].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
	formats[0].pNext = &h264;
	count = 1;
	status = vkGetPhysicalDeviceVideoFormatPropertiesKHR(&physical, &info, &count, formats);
	assert(status == VK_INCOMPLETE);
	assert(count == 1);
	assert(formats[0].pNext == &h264);
	assert(formats[0].format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	assert(formats[0].imageTiling == VK_IMAGE_TILING_OPTIMAL);
	assert(formats[0].imageUsageFlags == (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR));

	/* A two-element array receives both. */
	formats[1].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
	count = 2;
	status = vkGetPhysicalDeviceVideoFormatPropertiesKHR(&physical, &info, &count, formats);
	assert(status == VK_SUCCESS);
	assert(count == 2);
	assert(formats[1].componentMapping.a == 1);
}

/* A session's creation, memory requirements, binding and destruction. */
static void
test_session(void)
{
	VkVideoDecodeH264ProfileInfoKHR h264;
	VkVideoProfileInfoKHR profile;
	VkExtensionProperties header;
	VkVideoSessionCreateInfoKHR create;
	VkVideoSessionKHR session;
	VkVideoSessionMemoryRequirementsKHR requirements[3];
	VkBindVideoSessionMemoryInfoKHR binds[2];
	struct vulkan_object *object;
	VkResult status;
	uint32_t count;
	uint64_t identity;

	/* An H.264 High session of 1920x1088 with seventeen slots. */
	memset(&h264, 0, sizeof(h264));
	h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
	h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
	memset(&profile, 0, sizeof(profile));
	profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	profile.pNext = &h264;
	profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	memset(&header, 0, sizeof(header));
	strcpy(header.extensionName, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME);
	header.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
	create.queueFamilyIndex = 1;
	create.pVideoProfile = &profile;
	create.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	create.maxCodedExtent.width = 1920;
	create.maxCodedExtent.height = 1088;
	create.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	create.maxDpbSlots = 17;
	create.maxActiveReferencePictures = 16;
	create.pStdHeaderVersion = &header;

	/* The renderer creates the session under the reserved identity. */
	status = vkCreateVideoSessionKHR(&device, &create, NULL, &session);
	assert(status == VK_SUCCESS);
	object = vulkan_nondispatchable_object((uint64_t)(uintptr_t)session);
	assert(object->kind == VULKAN_OBJECT_VIDEO_SESSION);
	identity = object->wire_id;
	assert(identity != 0);

	/* The request: device, the create record with the profile and header, no allocator, the identity. */
	request_begin(GPU_OP_CREATE_VIDEO_SESSION, VK_TRUE);
	expect64(0x2222);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR);
	expect64(0);
	expect32(1);
	expect32(0);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR);
	expect64(0);
	expect32(STD_VIDEO_H264_PROFILE_IDC_HIGH);
	expect32(0);
	expect32(VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);
	expect32(VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
	expect32(VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR);
	expect32(VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR);
	expect32(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	expect32(1920);
	expect32(1088);
	expect32(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	expect32(17);
	expect32(16);
	expect64(1);
	expect64(VK_MAX_EXTENSION_NAME_SIZE);
	assert(memcmp(request + request_cursor, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME, 37) == 0);
	request_cursor += VK_MAX_EXTENSION_NAME_SIZE;
	expect32(VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION);
	expect64(0);
	expect64(1);
	expect64(identity);
	assert(request_cursor == request_bytes);

	/* The memory requirements: a count query, then a truncated array. */
	status = vkGetVideoSessionMemoryRequirementsKHR(&device, session, &count, NULL);
	assert(status == VK_SUCCESS);
	assert(count == 3);
	request_begin(GPU_OP_GET_VIDEO_SESSION_MEMORY_REQUIREMENTS, VK_TRUE);
	expect64(0x2222);
	expect64(identity);
	expect64(1);
	expect32(32);
	expect64(32);
	assert(request_cursor == request_bytes);
	memset(requirements, 0, sizeof(requirements));
	requirements[0].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
	requirements[1].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
	count = 2;
	status = vkGetVideoSessionMemoryRequirementsKHR(&device, session, &count, requirements);
	assert(status == VK_INCOMPLETE);
	assert(count == 2);
	assert(requirements[1].memoryBindIndex == 1);
	assert(requirements[1].memoryRequirements.size == 8192);
	assert(requirements[1].memoryRequirements.alignment == 4096);
	assert(requirements[1].memoryRequirements.memoryTypeBits == 1);

	/* Two bindings go to the renderer in order. */
	memset(binds, 0, sizeof(binds));
	binds[0].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
	binds[0].memoryBindIndex = 0;
	binds[0].memory = VK_NULL_HANDLE;
	binds[0].memoryOffset = 0;
	binds[0].memorySize = 4096;
	binds[1] = binds[0];
	binds[1].memoryBindIndex = 2;
	binds[1].memoryOffset = 8192;
	binds[1].memorySize = 12288;
	status = vkBindVideoSessionMemoryKHR(&device, session, 2, binds);
	assert(status == VK_SUCCESS);
	request_begin(GPU_OP_BIND_VIDEO_SESSION_MEMORY, VK_TRUE);
	expect64(0x2222);
	expect64(identity);
	expect32(2);
	expect64(2);
	expect32(VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR);
	expect64(0);
	expect32(0);
	expect64(0);
	expect64(0);
	expect64(4096);
	expect32(VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR);
	expect64(0);
	expect32(2);
	expect64(0);
	expect64(8192);
	expect64(12288);
	assert(request_cursor == request_bytes);

	/* Destruction withdraws the renderer session by identity. */
	vkDestroyVideoSessionKHR(&device, session, NULL);
	request_begin(GPU_OP_DESTROY_VIDEO_SESSION, VK_TRUE);
	expect64(0x2222);
	expect64(identity);
	expect64(0);
	assert(request_cursor == request_bytes);
}

/* Parameter sets: their wire form, the key checks of an update and destruction. */
static void
test_parameters(void)
{
	StdVideoH264ScalingLists scaling;
	StdVideoH264SequenceParameterSet sps;
	StdVideoH264PictureParameterSet pps[2];
	int32_t offsets[3];
	VkVideoDecodeH264SessionParametersAddInfoKHR add;
	VkVideoDecodeH264SessionParametersCreateInfoKHR h264;
	VkVideoSessionParametersCreateInfoKHR create;
	VkVideoSessionParametersUpdateInfoKHR update;
	VkVideoSessionParametersKHR parameters;
	VkVideoSessionParametersKHR copy;
	struct vulkan_object *object;
	VkResult status;
	uint64_t identity;
	uint32_t index;
	unsigned before;

	/* Asymmetric scaling lists, every coefficient different. */
	memset(&scaling, 0, sizeof(scaling));
	scaling.scaling_list_present_mask = 0x0f3;
	scaling.use_default_scaling_matrix_mask = 0x002;
	for (index = 0; index < sizeof(scaling.ScalingList4x4); index++)
		((uint8_t *)scaling.ScalingList4x4)[index] = (uint8_t)(index + 1);
	for (index = 0; index < sizeof(scaling.ScalingList8x8); index++)
		((uint8_t *)scaling.ScalingList8x8)[index] = (uint8_t)(255 - index);

	/* A sequence parameter set with flags 0, 8 and 15, three cycle offsets and the lists. */
	memset(&sps, 0, sizeof(sps));
	sps.flags.constraint_set0_flag = 1;
	sps.flags.frame_mbs_only_flag = 1;
	sps.flags.vui_parameters_present_flag = 1;
	sps.profile_idc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
	sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_4_1;
	sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
	sps.seq_parameter_set_id = 3;
	sps.log2_max_frame_num_minus4 = 2;
	sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_1;
	sps.offset_for_non_ref_pic = -5;
	sps.offset_for_top_to_bottom_field = 9;
	sps.num_ref_frames_in_pic_order_cnt_cycle = 3;
	sps.max_num_ref_frames = 4;
	sps.pic_width_in_mbs_minus1 = 119;
	sps.pic_height_in_map_units_minus1 = 67;
	sps.frame_crop_bottom_offset = 4;
	offsets[0] = -1;
	offsets[1] = 2;
	offsets[2] = -3;
	sps.pOffsetForRefFrame = offsets;
	sps.pScalingLists = &scaling;
	sps.pSequenceParameterSetVui = (const StdVideoH264SequenceParameterSetVui *)(uintptr_t)0x1;

	/* Two picture parameter sets of the sequence, one with negative offsets. */
	memset(pps, 0, sizeof(pps));
	pps[0].flags.transform_8x8_mode_flag = 1;
	pps[0].flags.pic_scaling_matrix_present_flag = 1;
	pps[0].seq_parameter_set_id = 3;
	pps[0].pic_parameter_set_id = 0;
	pps[0].num_ref_idx_l0_default_active_minus1 = 2;
	pps[0].weighted_bipred_idc = STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_IMPLICIT;
	pps[0].pic_init_qp_minus26 = -26;
	pps[0].chroma_qp_index_offset = -12;
	pps[0].second_chroma_qp_index_offset = 12;
	pps[0].pScalingLists = &scaling;
	pps[1] = pps[0];
	pps[1].pic_parameter_set_id = 200;
	pps[1].pScalingLists = NULL;

	/* A create record that adds the sequence and the first picture set. */
	memset(&add, 0, sizeof(add));
	add.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
	add.stdSPSCount = 1;
	add.pStdSPSs = &sps;
	add.stdPPSCount = 1;
	add.pStdPPSs = &pps[0];
	memset(&h264, 0, sizeof(h264));
	h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
	h264.maxStdSPSCount = 32;
	h264.maxStdPPSCount = 256;
	h264.pParametersAddInfo = &add;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
	create.pNext = &h264;
	create.videoSession = (VkVideoSessionKHR)(uintptr_t)0;

	/* The renderer creates the object. */
	status = vkCreateVideoSessionParametersKHR(&device, &create, NULL, &parameters);
	assert(status == VK_SUCCESS);
	object = vulkan_nondispatchable_object((uint64_t)(uintptr_t)parameters);
	assert(object->kind == VULKAN_OBJECT_VIDEO_SESSION_PARAMETERS);
	identity = object->wire_id;

	/* The request: the create record, the H.264 record, the add record and the sets. */
	request_begin(GPU_OP_CREATE_VIDEO_SESSION_PARAMETERS, VK_TRUE);
	expect64(0x2222);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR);
	expect64(0);
	expect32(32);
	expect32(256);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR);
	expect64(0);
	expect32(1);
	expect64(1);

	/* The sequence parameter set, flags packed by name. */
	expect32((1U << 0) | (1U << 8) | (1U << 15));
	expect32(STD_VIDEO_H264_PROFILE_IDC_HIGH);
	expect32(STD_VIDEO_H264_LEVEL_IDC_4_1);
	expect32(STD_VIDEO_H264_CHROMA_FORMAT_IDC_420);
	expect32(3);
	expect32(0);
	expect32(0);
	expect32(2);
	expect32(STD_VIDEO_H264_POC_TYPE_1);
	expect32((uint32_t)-5);
	expect32(9);
	expect32(0);
	expect32(3);
	expect32(4);
	expect32(0);
	expect32(119);
	expect32(67);
	expect32(0);
	expect32(0);
	expect32(0);
	expect32(4);
	expect32(0);
	expect64(3);
	expect32((uint32_t)-1);
	expect32(2);
	expect32((uint32_t)-3);
	expect64(1);
	expect32(0x0f3);
	expect32(0x002);
	expect64(96);
	assert(memcmp(request + request_cursor, scaling.ScalingList4x4, 96) == 0);
	request_cursor += 96;
	expect64(384);
	assert(memcmp(request + request_cursor, scaling.ScalingList8x8, 384) == 0);
	request_cursor += 384;
	expect64(0);

	/* The picture parameter set, signed offsets widened with their sign. */
	expect32(1);
	expect64(1);
	expect32((1U << 0) | (1U << 7));
	expect32(3);
	expect32(0);
	expect32(2);
	expect32(0);
	expect32(STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_IMPLICIT);
	expect32((uint32_t)-26);
	expect32(0);
	expect32((uint32_t)-12);
	expect32(12);
	expect64(1);
	request_cursor += 4 + 4 + 8 + 96 + 8 + 384;

	/* The create record's own fields, no allocator and the identity. */
	expect32(0);
	expect64(0);
	expect64(0);
	expect64(0);
	expect64(1);
	expect64(identity);
	assert(request_cursor == request_bytes);

	/* An update that adds a picture set already present is refused before the renderer. */
	memset(&update, 0, sizeof(update));
	update.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_UPDATE_INFO_KHR;
	update.pNext = &add;
	update.updateSequenceCount = 1;
	add.stdSPSCount = 0;
	add.stdPPSCount = 2;
	add.pStdPPSs = pps;
	before = transactions;
	status = vkUpdateVideoSessionParametersKHR(&device, parameters, &update);
	assert(status == VK_ERROR_INITIALIZATION_FAILED);
	assert(transactions == before);

	/* An id outside H.264 is refused too. */
	pps[1].seq_parameter_set_id = 32;
	add.stdPPSCount = 1;
	add.pStdPPSs = &pps[1];
	status = vkUpdateVideoSessionParametersKHR(&device, parameters, &update);
	assert(status == VK_ERROR_INITIALIZATION_FAILED);
	assert(transactions == before);

	/* A new key reaches the renderer and is then held. */
	pps[1].seq_parameter_set_id = 3;
	status = vkUpdateVideoSessionParametersKHR(&device, parameters, &update);
	assert(status == VK_SUCCESS);
	assert(transactions == before + 1);
	request_begin(GPU_OP_UPDATE_VIDEO_SESSION_PARAMETERS, VK_TRUE);
	expect64(0x2222);
	expect64(identity);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_UPDATE_INFO_KHR);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR);
	expect64(0);
	expect32(0);
	expect64(0);
	expect32(1);
	expect64(1);
	expect32((1U << 0) | (1U << 7));
	expect32(3);
	expect32(200);
	request_cursor += 4 * 7;
	expect64(0);
	expect32(1);
	assert(request_cursor == request_bytes);
	status = vkUpdateVideoSessionParametersKHR(&device, parameters, &update);
	assert(status == VK_ERROR_INITIALIZATION_FAILED);

	/* A copy of the object through a template starts with its keys, which it may replace. */
	create.videoSessionParametersTemplate = parameters;
	add.stdSPSCount = 1;
	add.pStdSPSs = &sps;
	add.stdPPSCount = 2;
	add.pStdPPSs = pps;
	status = vkCreateVideoSessionParametersKHR(&device, &create, NULL, &copy);
	assert(status == VK_SUCCESS);
	update.updateSequenceCount = 1;
	add.stdSPSCount = 0;
	add.stdPPSCount = 1;
	add.pStdPPSs = &pps[1];
	before = transactions;
	status = vkUpdateVideoSessionParametersKHR(&device, copy, &update);
	assert(status == VK_ERROR_INITIALIZATION_FAILED);
	assert(transactions == before);

	/* Destruction withdraws both objects by identity. */
	vkDestroyVideoSessionParametersKHR(&device, copy, NULL);
	vkDestroyVideoSessionParametersKHR(&device, parameters, NULL);
	request_begin(GPU_OP_DESTROY_VIDEO_SESSION_PARAMETERS, VK_TRUE);
	expect64(0x2222);
	expect64(identity);
	expect64(0);
	assert(request_cursor == request_bytes);
}

/* The three forms of a begin record, a reset, a decode and an end, as recorded. */
static void
test_recording(void)
{
	VkVideoPictureResourceInfoKHR pictures[2];
	StdVideoDecodeH264ReferenceInfo reference;
	VkVideoDecodeH264DpbSlotInfoKHR dpb;
	VkVideoReferenceSlotInfoKHR slots[3];
	VkVideoReferenceSlotInfoKHR setup;
	VkVideoBeginCodingInfoKHR begin;
	VkVideoCodingControlInfoKHR control;
	StdVideoDecodeH264PictureInfo std_picture;
	uint32_t slice_offsets[2];
	VkVideoDecodeH264PictureInfoKHR picture;
	VkVideoDecodeInfoKHR decode;
	VkVideoEndCodingInfoKHR end;
	size_t cursor;

	/* Two pictures, each a whole 64x64 view. */
	memset(pictures, 0, sizeof(pictures));
	pictures[0].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
	pictures[0].codedExtent.width = 64;
	pictures[0].codedExtent.height = 64;
	pictures[1] = pictures[0];

	/* Slot 2 holds the first picture as a long-term reference. */
	memset(&reference, 0, sizeof(reference));
	reference.flags.used_for_long_term_reference = 1;
	reference.FrameNum = 5;
	reference.PicOrderCnt[0] = 10;
	reference.PicOrderCnt[1] = -2;
	memset(&dpb, 0, sizeof(dpb));
	dpb.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
	dpb.pStdReferenceInfo = &reference;

	/* The begin record binds slot 2, a picture without a slot, and deactivates slot 4. */
	memset(slots, 0, sizeof(slots));
	slots[0].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	slots[0].pNext = &dpb;
	slots[0].slotIndex = 2;
	slots[0].pPictureResource = &pictures[0];
	slots[1].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	slots[1].slotIndex = -1;
	slots[1].pPictureResource = &pictures[1];
	slots[2].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	slots[2].slotIndex = 4;
	slots[2].pPictureResource = NULL;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
	begin.referenceSlotCount = 3;
	begin.pReferenceSlots = slots;
	cursor = command.recording.bytes;
	vkCmdBeginVideoCodingKHR(&command, &begin);

	/* The begin record as it lies in the command buffer. */
	record_begin(&cursor, GPU_OP_CMD_BEGIN_VIDEO_CODING);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR);
	expect64(0);
	expect32(0);
	expect64(0);
	expect64(0);
	expect32(3);
	expect64(3);
	expect32(VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
	expect64(0);
	expect64(1);
	expect32(1U << 2);
	expect32(5);
	expect32(0);
	expect64(2);
	expect32(10);
	expect32((uint32_t)-2);
	expect32(2);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
	expect64(0);
	expect32(0);
	expect32(0);
	expect32(64);
	expect32(64);
	expect32(0);
	expect64(0);
	expect32(VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR);
	expect64(0);
	expect32((uint32_t)-1);
	expect64(1);
	request_cursor += 4 + 8 + 4 * 5 + 8;
	expect32(VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR);
	expect64(0);
	expect32(4);
	expect64(0);
	assert(request_cursor == request_bytes);

	/* A reset of the session. */
	memset(&control, 0, sizeof(control));
	control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
	control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
	cursor = command.recording.bytes;
	vkCmdControlVideoCodingKHR(&command, &control);
	record_begin(&cursor, GPU_OP_CMD_CONTROL_VIDEO_CODING);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR);
	expect64(0);
	expect32(VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR);
	assert(request_cursor == request_bytes);

	/* A reference IDR picture of two slices, decoded into slot 2. */
	memset(&std_picture, 0, sizeof(std_picture));
	std_picture.flags.is_intra = 1;
	std_picture.flags.IdrPicFlag = 1;
	std_picture.flags.is_reference = 1;
	std_picture.seq_parameter_set_id = 3;
	std_picture.frame_num = 0;
	std_picture.idr_pic_id = 7;
	std_picture.PicOrderCnt[0] = 0;
	std_picture.PicOrderCnt[1] = 1;
	slice_offsets[0] = 0;
	slice_offsets[1] = 300;
	memset(&picture, 0, sizeof(picture));
	picture.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR;
	picture.pStdPictureInfo = &std_picture;
	picture.sliceCount = 2;
	picture.pSliceOffsets = slice_offsets;
	memset(&setup, 0, sizeof(setup));
	setup.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	setup.slotIndex = 2;
	setup.pPictureResource = &pictures[0];
	memset(&decode, 0, sizeof(decode));
	decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
	decode.pNext = &picture;
	decode.srcBufferOffset = 64;
	decode.srcBufferRange = 640;
	decode.dstPictureResource = pictures[0];
	decode.pSetupReferenceSlot = &setup;
	decode.referenceSlotCount = 0;
	cursor = command.recording.bytes;
	vkCmdDecodeVideoKHR(&command, &decode);

	/* The decode record: the H.264 picture record first, then the decode's own fields. */
	record_begin(&cursor, GPU_OP_CMD_DECODE_VIDEO);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR);
	expect64(0);
	expect64(1);
	expect32((1U << 1) | (1U << 2) | (1U << 4));
	expect32(3);
	expect32(0);
	expect32(0);
	expect32(0);
	expect32(0);
	expect32(7);
	expect64(2);
	expect32(0);
	expect32(1);
	expect32(2);
	expect64(2);
	expect32(0);
	expect32(300);
	expect32(0);
	expect64(0);
	expect64(64);
	expect64(640);
	expect32(VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
	expect64(0);
	request_cursor += 4 * 5 + 8;
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR);
	expect64(0);
	expect32(2);
	expect64(1);
	request_cursor += 4 + 8 + 4 * 5 + 8;
	expect32(0);
	expect64(0);
	assert(request_cursor == request_bytes);

	/* The end of the scope. */
	memset(&end, 0, sizeof(end));
	end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
	cursor = command.recording.bytes;
	vkCmdEndVideoCodingKHR(&command, &end);
	record_begin(&cursor, GPU_OP_CMD_END_VIDEO_CODING);
	expect64(1);
	expect32(VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR);
	expect64(0);
	expect32(0);
	assert(request_cursor == request_bytes);
}

/* The synchronization2 commands and the 1.0 commands they become. */
static void
test_sync2(void)
{
	VkMemoryBarrier2 memory;
	VkBufferMemoryBarrier2 buffer;
	VkImageMemoryBarrier2 images[20];
	VkDependencyInfo dependency;
	VkDependencyInfo dependencies[2];
	VkEvent events[2];
	VkSemaphoreSubmitInfo waits[2];
	VkSemaphoreSubmitInfo signal;
	VkCommandBufferSubmitInfo buffers[2];
	VkSubmitInfo2 submits[2];
	VkResult status;
	uint32_t index;

	/* A global barrier from video decode writes to fragment shader reads. */
	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	memory.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
	memory.srcAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
	memory.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	memory.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
	memset(&dependency, 0, sizeof(dependency));
	dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers = &memory;
	call_count = 0;
	vkCmdPipelineBarrier2KHR(&command, &dependency);

	/* Video decode has no 1.0 stage or access, so it widens to all commands and all memory. */
	assert(call_count == 1);
	assert(strcmp(calls[0], "barrier") == 0);
	assert(last_source == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	assert(last_destination == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	assert(last_memory_source == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
	assert(last_memory_destination == VK_ACCESS_SHADER_READ_BIT);

	/* A dependency of no barrier is still one execution dependency, from the top to the bottom. */
	dependency.memoryBarrierCount = 0;
	call_count = 0;
	vkCmdPipelineBarrier2KHR(&command, &dependency);
	assert(call_count == 1);
	assert(last_source == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
	assert(last_destination == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	/* Twenty image barriers and one buffer barrier go out in chunks of sixteen. */
	memset(images, 0, sizeof(images));
	for (index = 0; index < 20; index++) {
		images[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		images[index].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		images[index].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		images[index].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		images[index].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		images[index].srcQueueFamilyIndex = 1;
		images[index].dstQueueFamilyIndex = 0;
		images[index].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
	buffer.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
	buffer.offset = 64;
	buffer.size = 128;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers = &buffer;
	dependency.imageMemoryBarrierCount = 20;
	dependency.pImageMemoryBarriers = images;
	call_count = 0;
	vkCmdPipelineBarrier2KHR(&command, &dependency);
	assert(call_count == 2);
	assert(barrier_counts[0][1] == 1);
	assert(barrier_counts[0][2] == 16);
	assert(barrier_counts[1][1] == 0);
	assert(barrier_counts[1][2] == 4);
	assert(last_source == (VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT));
	assert(last_destination == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

	/* A signal happens after the source stages of its dependency. */
	dependency.bufferMemoryBarrierCount = 0;
	dependency.imageMemoryBarrierCount = 0;
	dependency.memoryBarrierCount = 1;
	memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	call_count = 0;
	vkCmdSetEvent2KHR(&command, (VkEvent)(uintptr_t)0x7000, &dependency);
	assert(strcmp(calls[0], "set") == 0);
	assert(last_event == (VkEvent)(uintptr_t)0x7000);
	assert(last_stage == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	/* A reset with no stage starts at the top. */
	vkCmdResetEvent2KHR(&command, (VkEvent)(uintptr_t)0x7001, VK_PIPELINE_STAGE_2_NONE);
	assert(strcmp(calls[1], "reset") == 0);
	assert(last_stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

	/* Two events are waited for one at a time, each with its own dependency. */
	events[0] = (VkEvent)(uintptr_t)0x7002;
	events[1] = (VkEvent)(uintptr_t)0x7003;
	dependencies[0] = dependency;
	dependencies[1] = dependency;
	dependencies[1].memoryBarrierCount = 0;
	call_count = 0;
	vkCmdWaitEvents2KHR(&command, 2, events, dependencies);
	assert(call_count == 2);
	assert(strcmp(calls[0], "wait") == 0);
	assert(barrier_counts[0][0] == 1);
	assert(barrier_counts[1][0] == 0);
	assert(last_event == events[1]);
	assert(last_source == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

	/* A timestamp at a stage 1.0 cannot name lands at all commands. */
	call_count = 0;
	vkCmdWriteTimestamp2KHR(&command, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, (VkQueryPool)(uintptr_t)0x5150, 7);
	assert(last_stage == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	vkCmdWriteTimestamp2KHR(&command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, (VkQueryPool)(uintptr_t)0x5150, 7);
	assert(last_stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	/* Two batches: one waits on two semaphores and runs two buffers, one signals. */
	memset(waits, 0, sizeof(waits));
	waits[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	waits[0].semaphore = (VkSemaphore)(uintptr_t)0x8000;
	waits[0].stageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
	waits[1] = waits[0];
	waits[1].semaphore = (VkSemaphore)(uintptr_t)0x8001;
	waits[1].stageMask = VK_PIPELINE_STAGE_2_NONE;
	memset(&signal, 0, sizeof(signal));
	signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	signal.semaphore = (VkSemaphore)(uintptr_t)0x8002;
	memset(buffers, 0, sizeof(buffers));
	buffers[0].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	buffers[0].commandBuffer = (VkCommandBuffer)(uintptr_t)0xa000;
	buffers[1] = buffers[0];
	buffers[1].commandBuffer = (VkCommandBuffer)(uintptr_t)0xa001;
	memset(submits, 0, sizeof(submits));
	submits[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submits[0].waitSemaphoreInfoCount = 2;
	submits[0].pWaitSemaphoreInfos = waits;
	submits[0].commandBufferInfoCount = 2;
	submits[0].pCommandBufferInfos = buffers;
	submits[1].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submits[1].signalSemaphoreInfoCount = 1;
	submits[1].pSignalSemaphoreInfos = &signal;
	status = vkQueueSubmit2KHR((VkQueue)(uintptr_t)0x9000, 2, submits, (VkFence)(uintptr_t)0x9100);
	assert(status == VK_SUCCESS);

	/* The 1.0 batches carry the same semaphores and buffers; the stages are widened. */
	assert(submit_count == 2);
	assert(submit_waits[0] == 2);
	assert(submit_wait_semaphores[0][1] == (VkSemaphore)(uintptr_t)0x8001);
	assert(submit_wait_stages[0][0] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	assert(submit_wait_stages[0][1] == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	assert(submit_commands[0] == 2);
	assert(submit_command_buffers[0][1] == (VkCommandBuffer)(uintptr_t)0xa001);
	assert(submit_signals[0] == 0);
	assert(submit_waits[1] == 0);
	assert(submit_commands[1] == 0);
	assert(submit_signals[1] == 1);
	assert(submit_signal_semaphores[1][0] == (VkSemaphore)(uintptr_t)0x8002);
}

/* The extensible physical-device answers: the feature, the family records and the picture query. */
static void
test_properties(void)
{
	VkPhysicalDeviceSynchronization2Features synchronization;
	VkPhysicalDeviceFeatures2 features;
	VkQueueFamilyQueryResultStatusPropertiesKHR result_status;
	VkQueueFamilyQueryResultStatusPropertiesKHR graphics_status;
	VkQueueFamilyVideoPropertiesKHR video[2];
	VkQueueFamilyProperties2 properties[2];
	VkVideoDecodeH264ProfileInfoKHR h264;
	VkVideoProfileInfoKHR profile;
	VkVideoProfileListInfoKHR list;
	VkPhysicalDeviceImageFormatInfo2 info;
	VkImageFormatProperties2 image;
	VkResult status;
	uint32_t count;

	/* Synchronization2 is reported as a feature wherever the caller chains it. */
	memset(&synchronization, 0, sizeof(synchronization));
	synchronization.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
	memset(&features, 0, sizeof(features));
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &synchronization;
	vkGetPhysicalDeviceFeatures2KHR(&physical, &features);
	assert(synchronization.synchronization2 == VK_TRUE);

	/* Each family reports its codec operations; the video family reports result status (ws083-p008), the other not. */
	memset(video, 0, sizeof(video));
	memset(&result_status, 0, sizeof(result_status));
	result_status.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR;
	result_status.queryResultStatusSupport = VK_FALSE;
	memset(&graphics_status, 0, sizeof(graphics_status));
	graphics_status.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR;
	graphics_status.queryResultStatusSupport = VK_TRUE;
	video[0].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
	video[0].pNext = &graphics_status;
	video[0].videoCodecOperations = 0xffff;
	video[1].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
	video[1].pNext = &result_status;
	memset(properties, 0, sizeof(properties));
	properties[0].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
	properties[0].pNext = &video[0];
	properties[1].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
	properties[1].pNext = &video[1];
	count = 2;
	vkGetPhysicalDeviceQueueFamilyProperties2KHR(&physical, &count, properties);
	assert(count == 2);
	assert(properties[1].queueFamilyProperties.queueFlags == VK_QUEUE_VIDEO_DECODE_BIT_KHR);
	assert(video[0].videoCodecOperations == 0);
	assert(video[1].videoCodecOperations == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);
	assert(result_status.queryResultStatusSupport == VK_TRUE);
	assert(graphics_status.queryResultStatusSupport == VK_FALSE);

	/* A decode picture with a profile list is checked locally, then given the renderer's limits. */
	memset(&h264, 0, sizeof(h264));
	h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
	h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
	memset(&profile, 0, sizeof(profile));
	profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	profile.pNext = &h264;
	profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	memset(&list, 0, sizeof(list));
	list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
	list.profileCount = 1;
	list.pProfiles = &profile;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
	info.pNext = &list;
	info.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	info.type = VK_IMAGE_TYPE_2D;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	status = vkGetPhysicalDeviceImageFormatProperties2KHR(&physical, &info, &image);
	assert(status == VK_SUCCESS);
	assert(image.imageFormatProperties.maxExtent.width == 4096);

	/* Sampling a decode picture, a linear one and an unsupported profile are refused. */
	info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	status = vkGetPhysicalDeviceImageFormatProperties2KHR(&physical, &info, &image);
	assert(status == VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR);
	info.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
	info.tiling = VK_IMAGE_TILING_LINEAR;
	status = vkGetPhysicalDeviceImageFormatProperties2KHR(&physical, &info, &image);
	assert(status == VK_ERROR_FORMAT_NOT_SUPPORTED);
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
	status = vkGetPhysicalDeviceImageFormatProperties2KHR(&physical, &info, &image);
	assert(status == VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR);
	list.profileCount = 0;
	status = vkGetPhysicalDeviceImageFormatProperties2KHR(&physical, &info, &image);
	assert(status == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR);
}
