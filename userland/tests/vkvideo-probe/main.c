/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * vkvideo-probe: lists what a Vulkan device offers for video decode, and
 * decodes an H.264 elementary stream with Vulkan Video, printing the
 * SHA-256 of each frame (ws083).
 *
 *   vkvideo-probe --list
 *   vkvideo-probe [--frames=N] [--expect=FILE.sha256] STREAM.h264
 *
 * The list names each queue family with its flags and video codec
 * operations and the device's video extensions; on a device without video
 * decode it shows no video family and no video extension.  The decode
 * reads the stream's parameter sets and pictures (h264.c), keeps the DPB
 * (dpb.c: one NV12 image a slot, the H.264 reference marking), decodes
 * each picture into a free slot's image with every reference picture of
 * the DPB, reads the image's planes through the image's subresource layout
 * (zedBSD's promise for an optimal NV12 image) and hashes the display
 * window as ffmpeg hashes a raw NV12 frame (frame.c).  The frames are
 * printed in display order: by their order counts, between IDR pictures.
 * With --expect, each frame is compared with the file's line, and the exit
 * status says whether all matched.  Where the video family reports result
 * status (ws083-p008), each decode is in a result status query, and a
 * decode that did not complete is named and fails the run.
 */

#include "h264.h"
#include "dpb.h"
#include "frame.h"

#include <vulkan/vulkan.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long a decode may take before the probe gives up, in nanoseconds. */
#define PROBE_WAIT_NS		5000000000ULL

/* The most session memory bindings the probe binds. */
#define PROBE_BINDINGS		32U

/* The most frames waiting for their place in the display order. */
#define PROBE_PENDING		256U

/* The most families and extensions the list shows. */
#define PROBE_FAMILIES		16U
#define PROBE_EXTENSIONS	512U

/* One decoded frame waiting for its place in the display order: its order count and hash. */
struct probe_frame {
	int32_t poc;
	char hash[65];
};

/* The frames decoded and not yet printed, what was printed and matched, and the decodes that did not complete. */
struct probe_output {
	struct probe_frame pending[PROBE_PENDING];
	uint32_t pending_count;
	uint32_t printed;
	uint32_t matched;
	uint32_t failed;
	FILE *expect;
};

/* What one run asked for. */
struct probe_options {
	int list;
	const char *stream;
	const char *expect;
	uint32_t frames;
};

/*
 * The Vulkan objects of a decode, made in order and destroyed in reverse
 * by probe_close; a handle never made is VK_NULL_HANDLE.
 */
struct probe {
	VkInstance instance;
	VkPhysicalDevice physical;
	uint32_t family;
	VkDevice device;
	VkQueue queue;
	VkPhysicalDeviceMemoryProperties memory;

	/* The H.264 decode profile, and the list of it buffers and images name. */
	VkVideoDecodeH264ProfileInfoKHR h264_profile;
	VkVideoProfileInfoKHR profile;
	VkVideoProfileListInfoKHR profile_list;

	/* The largest coded picture of the stream. */
	VkExtent2D extent;

	/* The session, its memory and its parameters. */
	VkVideoSessionKHR session;
	VkDeviceMemory session_memory[PROBE_BINDINGS];
	uint32_t session_memory_count;
	VkVideoSessionParametersKHR parameters;

	/* The pictures, one a DPB slot: each image, its memory (mapped) and its view; the references the session reads. */
	uint32_t slots;
	uint32_t max_references;
	VkImage image[DPB_SLOTS];
	VkDeviceMemory image_memory[DPB_SLOTS];
	uint8_t *image_map[DPB_SLOTS];
	VkImageView view[DPB_SLOTS];

	/* The bitstream buffer and its memory (mapped). */
	VkBuffer buffer;
	VkDeviceMemory buffer_memory;
	uint8_t *buffer_map;
	VkDeviceSize buffer_size;

	/* The command buffer of the video family, and the fence a decode is waited on with. */
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;

	/*
	 * Whether the video family reports result status, the pool of the one
	 * query each decode is in (VK_NULL_HANDLE without one), and the status
	 * of the last decode (COMPLETE without a pool).
	 */
	VkBool32 status_supported;
	VkQueryPool status_pool;
	int32_t status;
};

static int probe_arguments(int argc, char **argv, struct probe_options *options);
static int probe_instance(struct probe *probe);
static int probe_list(struct probe *probe);
static int probe_video_family(struct probe *probe, uint32_t *family);
static int probe_device(struct probe *probe);
static int probe_capabilities(struct probe *probe);
static int probe_memory_type(const struct probe *probe, uint32_t bits, uint32_t *index);
static int probe_allocate(struct probe *probe, const VkMemoryRequirements *requirements, VkDeviceMemory *memory);
static int probe_pictures(struct probe *probe);
static int probe_picture(struct probe *probe, uint32_t slot);
static int probe_bitstream(struct probe *probe, size_t bytes);
static int probe_session(struct probe *probe);
static int probe_parameters(struct probe *probe, const struct h264_stream *stream);
static int probe_commands(struct probe *probe);
static void probe_status_pool(struct probe *probe);
static int probe_decode(struct probe *probe, const struct h264_stream *stream, const struct h264_picture *picture, const struct dpb_plan *plan);
static void probe_hash(struct probe *probe, uint32_t slot, const StdVideoH264SequenceParameterSet *sps, char text[65]);
static void probe_output_add(struct probe_output *output, int32_t poc, const char *hash);
static void probe_output_flush(struct probe_output *output);
static int probe_frame_compare(const void *left, const void *right);
static int probe_run(struct probe *probe, const struct probe_options *options);
static void probe_close(struct probe *probe);
static int probe_failed(VkResult result, const char *what);
static uint8_t *probe_read(const char *path, size_t *size);

/*
 * Lists the device's video decode, or decodes a stream and prints its frames' hashes.
 */
int
main(
	int argc,
	char **argv)
{
	struct probe_options options;
	struct probe probe;
	int status;

	/* The options. */
	memset(&options, 0, sizeof(options));
	status = probe_arguments(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: vkvideo-probe --list | vkvideo-probe [--frames=N] [--expect=FILE.sha256] STREAM.h264\n");
		return 2;
	}

	/* The instance and its first physical device. */
	memset(&probe, 0, sizeof(probe));
	status = probe_instance(&probe);
	if (status != 0) {
		probe_close(&probe);
		return 1;
	}

	/* The list, or the decode. */
	if (options.list)
		status = probe_list(&probe);
	else
		status = probe_run(&probe, &options);
	probe_close(&probe);

	/* Reports a list or a decode that failed. */
	if (status != 0)
		return status;

	/* Succeeded: listed, or every frame decoded (and matched). */
	return 0;
}

/* Reads the options: --list, or a stream with --frames and --expect. */
static int
probe_arguments(
	int argc,
	char **argv,
	struct probe_options *options)
{
	const char *argument;
	char *end;
	int index;
	int differs;

	/* Each argument. */
	for (index = 1; index < argc; index++) {
		argument = argv[index];

		/* The first argument that is not an option is the stream. */
		if (argument[0] != '-' && options->stream == NULL) {
			options->stream = argument;
			continue;
		}

		/* --list. */
		differs = strcmp(argument, "--list");
		if (differs == 0) {
			options->list = 1;
			continue;
		}

		/* --frames=N: decode at most N frames. */
		differs = strncmp(argument, "--frames=", 9U);
		if (differs == 0) {
			errno = 0;
			options->frames = (uint32_t)strtoul(argument + 9, &end, 10);
			if (errno != 0 || *end != '\0')
				return -1;
			continue;
		}

		/* --expect=FILE: the reference hashes. */
		differs = strncmp(argument, "--expect=", 9U);
		if (differs == 0) {
			options->expect = argument + 9;
			continue;
		}

		/* Anything else is not an argument of the probe. */
		return -1;
	}

	/* Exactly one of the list and a stream. */
	if (options->list && options->stream != NULL)
		return -1;
	if (!options->list && options->stream == NULL)
		return -1;

	/* Succeeded: the options are read. */
	return 0;
}

/* Makes the instance (with the properties2 extension the video queries need) and takes its first device. */
static int
probe_instance(
	struct probe *probe)
{
	static const char *const extensions[] = { "VK_KHR_get_physical_device_properties2" };
	VkApplicationInfo application;
	VkInstanceCreateInfo info;
	VkResult result;
	uint32_t count;

	/* The instance. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "vkvideo-probe";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &application;
	info.enabledExtensionCount = 1U;
	info.ppEnabledExtensionNames = extensions;
	result = vkCreateInstance(&info, NULL, &probe->instance);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateInstance");

	/* The first physical device. */
	count = 1U;
	result = vkEnumeratePhysicalDevices(probe->instance, &count, &probe->physical);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0U)
		return probe_failed(result, "vkEnumeratePhysicalDevices");
	vkGetPhysicalDeviceMemoryProperties(probe->physical, &probe->memory);

	/* Succeeded: a device to ask. */
	return 0;
}

/*
 * Prints the device, each queue family with its flags and video codec
 * operations, and the device's video extensions.
 */
static int
probe_list(
	struct probe *probe)
{
	VkPhysicalDeviceProperties properties;
	VkQueueFamilyProperties2 families[PROBE_FAMILIES];
	VkQueueFamilyVideoPropertiesKHR video[PROBE_FAMILIES];
	static VkExtensionProperties extensions[PROBE_EXTENSIONS];
	VkResult result;
	const char *name;
	const char *video_word;
	uint32_t count;
	uint32_t index;
	uint32_t video_families;
	uint32_t video_extensions;
	int differs;

	/* The device. */
	vkGetPhysicalDeviceProperties(probe->physical, &properties);
	printf("vkvideo-probe: device %s\n", properties.deviceName);

	/* The queue families, each with its video properties chained. */
	count = PROBE_FAMILIES;
	memset(families, 0, sizeof(families));
	memset(video, 0, sizeof(video));
	for (index = 0U; index < PROBE_FAMILIES; index++) {
		families[index].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
		families[index].pNext = &video[index];
		video[index].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
	}
	vkGetPhysicalDeviceQueueFamilyProperties2KHR(probe->physical, &count, families);
	video_families = 0U;
	for (index = 0U; index < count; index++) {
		printf("vkvideo-probe: family %u flags 0x%x queues %u codecs 0x%x\n",
		       index,
		       families[index].queueFamilyProperties.queueFlags,
		       families[index].queueFamilyProperties.queueCount,
		       video[index].videoCodecOperations);
		if ((families[index].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) != 0U)
			video_families++;
	}

	/* The video extensions and synchronization2. */
	count = PROBE_EXTENSIONS;
	result = vkEnumerateDeviceExtensionProperties(probe->physical, NULL, &count, extensions);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return probe_failed(result, "vkEnumerateDeviceExtensionProperties");
	video_extensions = 0U;
	for (index = 0U; index < count; index++) {
		/* A video extension, or synchronization2, which the video extensions need. */
		name = extensions[index].extensionName;
		video_word = strstr(name, "video");
		differs = strcmp(name, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
		if (video_word == NULL && differs != 0)
			continue;
		printf("vkvideo-probe: extension %s %u\n", name, extensions[index].specVersion);
		if (video_word != NULL)
			video_extensions++;
	}

	/* The summary line. */
	printf("vkvideo-probe: video families %u, video extensions %u\n", video_families, video_extensions);
	return 0;
}

/* Finds the queue family that decodes H.264. */
static int
probe_video_family(
	struct probe *probe,
	uint32_t *family)
{
	VkQueueFamilyProperties2 families[PROBE_FAMILIES];
	VkQueueFamilyVideoPropertiesKHR video[PROBE_FAMILIES];
	VkQueueFamilyQueryResultStatusPropertiesKHR status[PROBE_FAMILIES];
	uint32_t count;
	uint32_t index;

	/* The families with their video properties and whether they report result status. */
	count = PROBE_FAMILIES;
	memset(families, 0, sizeof(families));
	memset(video, 0, sizeof(video));
	memset(status, 0, sizeof(status));
	for (index = 0U; index < PROBE_FAMILIES; index++) {
		families[index].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
		families[index].pNext = &video[index];
		video[index].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
		video[index].pNext = &status[index];
		status[index].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR;
	}
	vkGetPhysicalDeviceQueueFamilyProperties2KHR(probe->physical, &count, families);

	/* The first one with video decode and H.264. */
	for (index = 0U; index < count; index++) {
		if ((families[index].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) == 0U)
			continue;
		if ((video[index].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) == 0U)
			continue;
		*family = index;
		probe->status_supported = status[index].queryResultStatusSupport;
		return 0;
	}

	/* None. */
	fprintf(stderr, "vkvideo-probe: no queue family decodes H.264\n");
	return 1;
}

/* Makes the device with one queue of the video family and the four video extensions. */
static int
probe_device(
	struct probe *probe)
{
	static const char *const extensions[] = {
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
		VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME
	};
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo info;
	VkResult result;
	float priority;
	int error;

	/* The video family. */
	error = probe_video_family(probe, &probe->family);
	if (error != 0)
		return error;

	/* The device and its one video queue. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = probe->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.queueCreateInfoCount = 1U;
	info.pQueueCreateInfos = &queue;
	info.enabledExtensionCount = 4U;
	info.ppEnabledExtensionNames = extensions;
	result = vkCreateDevice(probe->physical, &info, NULL, &probe->device);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateDevice");
	vkGetDeviceQueue(probe->device, probe->family, 0U, &probe->queue);

	/* Succeeded: a device that decodes. */
	return 0;
}

/* Asks the device's H.264 decode capabilities and NV12 output format for the profile, and prints them. */
static int
probe_capabilities(
	struct probe *probe)
{
	VkVideoDecodeH264CapabilitiesKHR h264;
	VkVideoDecodeCapabilitiesKHR decode;
	VkVideoCapabilitiesKHR capabilities;
	VkPhysicalDeviceVideoFormatInfoKHR format_info;
	VkVideoFormatPropertiesKHR formats[4];
	VkResult result;
	uint32_t count;
	uint32_t index;

	/* The capabilities, with the decode and H.264 ones chained. */
	memset(&h264, 0, sizeof(h264));
	h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
	memset(&decode, 0, sizeof(decode));
	decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
	decode.pNext = &h264;
	memset(&capabilities, 0, sizeof(capabilities));
	capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
	capabilities.pNext = &decode;
	result = vkGetPhysicalDeviceVideoCapabilitiesKHR(probe->physical, &probe->profile, &capabilities);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
	printf("vkvideo-probe: capabilities max %ux%u slots %u references %u level %u flags 0x%x decode 0x%x\n",
	       capabilities.maxCodedExtent.width,
	       capabilities.maxCodedExtent.height,
	       capabilities.maxDpbSlots,
	       capabilities.maxActiveReferencePictures,
	       (unsigned)h264.maxLevelIdc,
	       capabilities.flags,
	       decode.flags);

	/* The stream must fit. */
	if (probe->extent.width > capabilities.maxCodedExtent.width || probe->extent.height > capabilities.maxCodedExtent.height) {
		fprintf(stderr, "vkvideo-probe: the stream's %ux%u is larger than the decoder's\n", probe->extent.width, probe->extent.height);
		return 1;
	}

	/* The output and reference format: NV12. */
	memset(&format_info, 0, sizeof(format_info));
	format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
	format_info.pNext = &probe->profile_list;
	format_info.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
	memset(formats, 0, sizeof(formats));
	for (index = 0U; index < 4U; index++)
		formats[index].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
	count = 4U;
	result = vkGetPhysicalDeviceVideoFormatPropertiesKHR(probe->physical, &format_info, &count, formats);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return probe_failed(result, "vkGetPhysicalDeviceVideoFormatPropertiesKHR");
	for (index = 0U; index < count; index++) {
		if (formats[index].format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
			return 0;
	}

	/* No NV12. */
	fprintf(stderr, "vkvideo-probe: the decoder does not write NV12\n");
	return 1;
}

/* Finds a host-visible memory type among the allowed ones. */
static int
probe_memory_type(
	const struct probe *probe,
	uint32_t bits,
	uint32_t *index)
{
	uint32_t type;

	/* The first allowed host-visible type. */
	for (type = 0U; type < probe->memory.memoryTypeCount; type++) {
		if ((bits & (1U << type)) == 0U)
			continue;
		if ((probe->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U)
			continue;
		*index = type;
		return 0;
	}

	/* None. */
	fprintf(stderr, "vkvideo-probe: no host-visible memory type in 0x%x\n", bits);
	return 1;
}

/* Allocates memory for a requirement. */
static int
probe_allocate(
	struct probe *probe,
	const VkMemoryRequirements *requirements,
	VkDeviceMemory *memory)
{
	VkMemoryAllocateInfo info;
	VkResult result;
	uint32_t type;
	int error;

	/* A host-visible type. */
	error = probe_memory_type(probe, requirements->memoryTypeBits, &type);
	if (error != 0)
		return error;

	/* The allocation. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	info.allocationSize = requirements->size;
	info.memoryTypeIndex = type;
	result = vkAllocateMemory(probe->device, &info, NULL, memory);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkAllocateMemory");

	/* Succeeded: the memory. */
	return 0;
}

/* Makes the NV12 picture of every DPB slot. */
static int
probe_pictures(
	struct probe *probe)
{
	uint32_t slot;
	int error;

	/* One picture a slot. */
	for (slot = 0U; slot < probe->slots; slot++) {
		error = probe_picture(probe, slot);
		if (error != 0)
			return error;
	}

	/* Succeeded: every slot has its picture. */
	return 0;
}

/* Makes the NV12 picture of one slot: the image, its memory (mapped) and its view. */
static int
probe_picture(
	struct probe *probe,
	uint32_t slot)
{
	VkImageCreateInfo image;
	VkImageViewCreateInfo view;
	VkMemoryRequirements requirements;
	VkResult result;
	void *map;
	int error;

	/* The image: NV12, optimal, the decoder's output and reference picture, of the profile. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.pNext = &probe->profile_list;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	image.extent.width = probe->extent.width;
	image.extent.height = probe->extent.height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(probe->device, &image, NULL, &probe->image[slot]);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateImage");

	/* Its memory, bound and mapped. */
	vkGetImageMemoryRequirements(probe->device, probe->image[slot], &requirements);
	error = probe_allocate(probe, &requirements, &probe->image_memory[slot]);
	if (error != 0)
		return error;
	result = vkBindImageMemory(probe->device, probe->image[slot], probe->image_memory[slot], 0U);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkBindImageMemory");
	result = vkMapMemory(probe->device, probe->image_memory[slot], 0U, VK_WHOLE_SIZE, 0U, &map);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkMapMemory");
	probe->image_map[slot] = map;

	/* Its view. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = probe->image[slot];
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	result = vkCreateImageView(probe->device, &view, NULL, &probe->view[slot]);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateImageView");

	/* Succeeded: the slot's picture. */
	return 0;
}

/* Makes the bitstream buffer of a picture's bytes, and its memory (mapped). */
static int
probe_bitstream(
	struct probe *probe,
	size_t bytes)
{
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkResult result;
	void *map;
	int error;

	/* The buffer: whole pages, a decode's source, of the profile. */
	probe->buffer_size = ((VkDeviceSize)bytes + 4095U) & ~(VkDeviceSize)4095U;
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.pNext = &probe->profile_list;
	buffer.size = probe->buffer_size;
	buffer.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(probe->device, &buffer, NULL, &probe->buffer);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateBuffer");

	/* Its memory, bound and mapped. */
	vkGetBufferMemoryRequirements(probe->device, probe->buffer, &requirements);
	error = probe_allocate(probe, &requirements, &probe->buffer_memory);
	if (error != 0)
		return error;
	result = vkBindBufferMemory(probe->device, probe->buffer, probe->buffer_memory, 0U);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkBindBufferMemory");
	result = vkMapMemory(probe->device, probe->buffer_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkMapMemory");
	probe->buffer_map = map;

	/* Succeeded: the buffer. */
	return 0;
}

/* Makes the video session (the DPB's slots and references) and binds every memory it asks for. */
static int
probe_session(
	struct probe *probe)
{
	static const VkExtensionProperties header = {
		VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME,
		VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION
	};
	VkVideoSessionCreateInfoKHR info;
	VkVideoSessionMemoryRequirementsKHR requirements[PROBE_BINDINGS];
	VkBindVideoSessionMemoryInfoKHR binds[PROBE_BINDINGS];
	VkResult result;
	uint32_t count;
	uint32_t index;
	int error;

	/* The session. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
	info.queueFamilyIndex = probe->family;
	info.pVideoProfile = &probe->profile;
	info.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	info.maxCodedExtent = probe->extent;
	info.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	info.maxDpbSlots = probe->slots;
	info.maxActiveReferencePictures = probe->max_references;
	info.pStdHeaderVersion = &header;
	result = vkCreateVideoSessionKHR(probe->device, &info, NULL, &probe->session);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateVideoSessionKHR");

	/* What memory it asks for. */
	memset(requirements, 0, sizeof(requirements));
	for (index = 0U; index < PROBE_BINDINGS; index++)
		requirements[index].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
	count = PROBE_BINDINGS;
	result = vkGetVideoSessionMemoryRequirementsKHR(probe->device, probe->session, &count, requirements);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkGetVideoSessionMemoryRequirementsKHR");

	/* One allocation for each binding, then all bound at once. */
	memset(binds, 0, sizeof(binds));
	for (index = 0U; index < count; index++) {
		error = probe_allocate(probe, &requirements[index].memoryRequirements, &probe->session_memory[index]);
		if (error != 0)
			return error;
		probe->session_memory_count = index + 1U;
		binds[index].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
		binds[index].memoryBindIndex = requirements[index].memoryBindIndex;
		binds[index].memory = probe->session_memory[index];
		binds[index].memoryOffset = 0U;
		binds[index].memorySize = requirements[index].memoryRequirements.size;
	}
	result = vkBindVideoSessionMemoryKHR(probe->device, probe->session, count, binds);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkBindVideoSessionMemoryKHR");
	printf("vkvideo-probe: session %ux%u, %u slots, %u references, %u memory bindings\n",
	       probe->extent.width,
	       probe->extent.height,
	       probe->slots,
	       probe->max_references,
	       count);

	/* Succeeded: the session is bound. */
	return 0;
}

/* Makes the session's parameters from every parameter set of the stream. */
static int
probe_parameters(
	struct probe *probe,
	const struct h264_stream *stream)
{
	static StdVideoH264SequenceParameterSet sps[H264_SPS_IDS];
	static StdVideoH264PictureParameterSet pps[H264_PPS_IDS];
	VkVideoDecodeH264SessionParametersAddInfoKHR add;
	VkVideoDecodeH264SessionParametersCreateInfoKHR h264;
	VkVideoSessionParametersCreateInfoKHR info;
	VkResult result;
	uint32_t sps_count;
	uint32_t pps_count;
	uint32_t index;

	/* The sets the stream has. */
	sps_count = 0U;
	for (index = 0U; index < H264_SPS_IDS; index++) {
		if (!stream->has_sps[index])
			continue;
		sps[sps_count] = stream->sps[index];
		sps_count++;
	}
	pps_count = 0U;
	for (index = 0U; index < H264_PPS_IDS; index++) {
		if (!stream->has_pps[index])
			continue;
		pps[pps_count] = stream->pps[index];
		pps_count++;
	}

	/* The parameters object holding them. */
	memset(&add, 0, sizeof(add));
	add.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
	add.stdSPSCount = sps_count;
	add.pStdSPSs = sps;
	add.stdPPSCount = pps_count;
	add.pStdPPSs = pps;
	memset(&h264, 0, sizeof(h264));
	h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
	h264.maxStdSPSCount = sps_count;
	h264.maxStdPPSCount = pps_count;
	h264.pParametersAddInfo = &add;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
	info.pNext = &h264;
	info.videoSession = probe->session;
	result = vkCreateVideoSessionParametersKHR(probe->device, &info, NULL, &probe->parameters);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateVideoSessionParametersKHR");
	printf("vkvideo-probe: parameters %u SPS, %u PPS\n", sps_count, pps_count);

	/* Succeeded: the parameters. */
	return 0;
}

/* Makes the command pool and buffer of the video family, and the fence. */
static int
probe_commands(
	struct probe *probe)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkResult result;

	/* The pool, whose buffer is reset before each picture. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = probe->family;
	result = vkCreateCommandPool(probe->device, &pool, NULL, &probe->pool);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateCommandPool");

	/* The buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = probe->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	result = vkAllocateCommandBuffers(probe->device, &command, &probe->command);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkAllocateCommandBuffers");

	/* The fence. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(probe->device, &fence, NULL, &probe->fence);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateFence");

	/* The result status query, where the family has it. */
	probe_status_pool(probe);

	/* Succeeded: the commands. */
	return 0;
}

/*
 * Makes the pool of the one result status query each decode is in, where
 * the video family reports result status; without it (or when the pool
 * cannot be made) the decodes run without a query.
 */
static void
probe_status_pool(
	struct probe *probe)
{
	VkQueryPoolCreateInfo info;
	VkResult result;

	/* No status: every decode counts as complete. */
	probe->status = VK_QUERY_RESULT_STATUS_COMPLETE_KHR;
	if (!probe->status_supported)
		return;

	/* One query of the decode's profile. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	info.pNext = &probe->profile;
	info.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
	info.queryCount = 1U;
	result = vkCreateQueryPool(probe->device, &info, NULL, &probe->status_pool);
	if (result != VK_SUCCESS) {
		probe->status_pool = VK_NULL_HANDLE;
		printf("vkvideo-probe: no result status query (%d); decoding without\n", (int)result);
	}
}

/*
 * Decodes one picture as the DPB planned it, and waits for it: the slices
 * copied into the buffer each behind a three-byte start code; a coding
 * scope binding every reference with its slot, deactivating the slots that
 * hold no reference any more and binding the written slot's picture without
 * a slot; the session's reset on the first decode; the decode with the
 * references and the written slot set up; the scope's end.  The first
 * decode moves every slot's image into the DPB layout.
 */
static int
probe_decode(
	struct probe *probe,
	const struct h264_stream *stream,
	const struct h264_picture *picture,
	const struct dpb_plan *plan)
{
	static uint32_t offsets[H264_MAX_SLICES];
	VkVideoPictureResourceInfoKHR resources[DPB_SLOTS];
	VkVideoReferenceSlotInfoKHR bound[DPB_SLOTS * 2U];
	VkVideoReferenceSlotInfoKHR references[DPB_REFERENCES];
	VkVideoDecodeH264DpbSlotInfoKHR reference_info[DPB_REFERENCES];
	VkVideoDecodeH264DpbSlotInfoKHR setup_info;
	VkVideoDecodeH264PictureInfoKHR h264;
	VkVideoReferenceSlotInfoKHR setup;
	VkVideoBeginCodingInfoKHR begin;
	VkVideoCodingControlInfoKHR control;
	VkVideoDecodeInfoKHR decode;
	VkVideoEndCodingInfoKHR end;
	VkCommandBufferBeginInfo record;
	VkImageMemoryBarrier barriers[DPB_SLOTS];
	VkSubmitInfo submit;
	const StdVideoH264SequenceParameterSet *sps;
	VkResult result;
	size_t at;
	uint32_t slice;
	uint32_t slot;
	uint32_t index;
	uint32_t count;

	/* The slices, each behind a start code, into the buffer. */
	at = 0U;
	for (slice = 0U; slice < picture->slice_count; slice++) {
		if (at + 3U + picture->slice_sizes[slice] > probe->buffer_size) {
			fprintf(stderr, "vkvideo-probe: a picture larger than the buffer\n");
			return 1;
		}
		offsets[slice] = (uint32_t)at;
		probe->buffer_map[at] = 0U;
		probe->buffer_map[at + 1U] = 0U;
		probe->buffer_map[at + 2U] = 1U;
		memcpy(probe->buffer_map + at + 3U, stream->data + picture->slice_offsets[slice], picture->slice_sizes[slice]);
		at += 3U + picture->slice_sizes[slice];
	}

	/* Every slot's picture resource: its image's coded picture of the sequence. */
	sps = &stream->sps[picture->info.seq_parameter_set_id];
	memset(resources, 0, sizeof(resources));
	for (slot = 0U; slot < probe->slots; slot++) {
		resources[slot].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
		resources[slot].codedExtent.width = (sps->pic_width_in_mbs_minus1 + 1U) * 16U;
		resources[slot].codedExtent.height = (sps->pic_height_in_map_units_minus1 + 1U) * 16U;
		resources[slot].imageViewBinding = probe->view[slot];
	}

	/* The command buffer from its start. */
	memset(&record, 0, sizeof(record));
	record.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	record.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(probe->command, &record);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkBeginCommandBuffer");

	/* The status query starts unavailable, outside the scope. */
	if (probe->status_pool != VK_NULL_HANDLE)
		vkCmdResetQueryPool(probe->command, probe->status_pool, 0U, 1U);

	/* The first decode moves every slot's image into the DPB layout. */
	if (plan->reset) {
		memset(barriers, 0, sizeof(barriers));
		for (slot = 0U; slot < probe->slots; slot++) {
			barriers[slot].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barriers[slot].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barriers[slot].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
			barriers[slot].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[slot].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[slot].image = probe->image[slot];
			barriers[slot].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barriers[slot].subresourceRange.levelCount = 1U;
			barriers[slot].subresourceRange.layerCount = 1U;
		}
		vkCmdPipelineBarrier(probe->command,
				     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				     0U,
				     0U,
				     NULL,
				     0U,
				     NULL,
				     probe->slots,
				     barriers);
	}

	/* The scope's slots: the references, the deactivated slots, the written picture without a slot. */
	memset(bound, 0, sizeof(bound));
	count = 0U;
	for (index = 0U; index < plan->reference_count; index++) {
		bound[count].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		bound[count].slotIndex = plan->references[index];
		bound[count].pPictureResource = &resources[plan->references[index]];
		count++;
	}
	for (index = 0U; index < plan->deactivate_count; index++) {
		bound[count].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		bound[count].slotIndex = plan->deactivate[index];
		bound[count].pPictureResource = NULL;
		count++;
	}
	bound[count].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	bound[count].slotIndex = -1;
	bound[count].pPictureResource = &resources[plan->setup];
	count++;

	/* The scope, and the reset of the first decode. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
	begin.videoSession = probe->session;
	begin.videoSessionParameters = probe->parameters;
	begin.referenceSlotCount = count;
	begin.pReferenceSlots = bound;
	vkCmdBeginVideoCodingKHR(probe->command, &begin);
	if (plan->reset) {
		memset(&control, 0, sizeof(control));
		control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
		control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
		vkCmdControlVideoCodingKHR(probe->command, &control);
	}

	/* The references with their information. */
	memset(references, 0, sizeof(references));
	memset(reference_info, 0, sizeof(reference_info));
	for (index = 0U; index < plan->reference_count; index++) {
		reference_info[index].sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
		reference_info[index].pStdReferenceInfo = &plan->info[index];
		references[index].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		references[index].pNext = &reference_info[index];
		references[index].slotIndex = plan->references[index];
		references[index].pPictureResource = &resources[plan->references[index]];
	}

	/* The written slot, with the picture's own information. */
	memset(&setup_info, 0, sizeof(setup_info));
	setup_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
	setup_info.pStdReferenceInfo = &plan->setup_info;
	memset(&setup, 0, sizeof(setup));
	setup.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	setup.pNext = &setup_info;
	setup.slotIndex = plan->setup;
	setup.pPictureResource = &resources[plan->setup];

	/* The decode: the slices into the written slot's picture. */
	memset(&h264, 0, sizeof(h264));
	h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR;
	h264.pStdPictureInfo = &picture->info;
	h264.sliceCount = picture->slice_count;
	h264.pSliceOffsets = offsets;
	memset(&decode, 0, sizeof(decode));
	decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
	decode.pNext = &h264;
	decode.srcBuffer = probe->buffer;
	decode.srcBufferOffset = 0U;
	decode.srcBufferRange = at;
	decode.dstPictureResource = resources[plan->setup];
	decode.pSetupReferenceSlot = &setup;
	decode.referenceSlotCount = plan->reference_count;
	decode.pReferenceSlots = references;
	if (probe->status_pool != VK_NULL_HANDLE)
		vkCmdBeginQuery(probe->command, probe->status_pool, 0U, 0U);
	vkCmdDecodeVideoKHR(probe->command, &decode);
	if (probe->status_pool != VK_NULL_HANDLE)
		vkCmdEndQuery(probe->command, probe->status_pool, 0U);

	/* The scope's end. */
	memset(&end, 0, sizeof(end));
	end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
	vkCmdEndVideoCodingKHR(probe->command, &end);
	result = vkEndCommandBuffer(probe->command);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkEndCommandBuffer");

	/* Submits it and waits for it. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &probe->command;
	result = vkQueueSubmit(probe->queue, 1U, &submit, probe->fence);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkQueueSubmit");
	result = vkWaitForFences(probe->device, 1U, &probe->fence, VK_TRUE, PROBE_WAIT_NS);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkWaitForFences");

	/* The fence and the buffer for the next picture. */
	result = vkResetFences(probe->device, 1U, &probe->fence);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkResetFences");
	result = vkResetCommandBuffer(probe->command, 0U);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkResetCommandBuffer");

	/* Whether the decode completed: its query's status, when there is one. */
	probe->status = VK_QUERY_RESULT_STATUS_COMPLETE_KHR;
	if (probe->status_pool != VK_NULL_HANDLE) {
		result = vkGetQueryPoolResults(probe->device, probe->status_pool, 0U, 1U, sizeof(probe->status), &probe->status, sizeof(probe->status), VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);
		if (result != VK_SUCCESS)
			return probe_failed(result, "vkGetQueryPoolResults");
	}

	/* Succeeded: the picture is in its slot's image, or its status says why not. */
	return 0;
}

/* Hashes a slot's image's display window of a sequence: its planes from the image's layout, the cropping of the sequence. */
static void
probe_hash(
	struct probe *probe,
	uint32_t slot,
	const StdVideoH264SequenceParameterSet *sps,
	char text[65])
{
	VkImageSubresource subresource;
	VkSubresourceLayout luma;
	VkSubresourceLayout chroma;
	struct frame_planes planes;
	struct frame_window window;

	/* The two planes. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	vkGetImageSubresourceLayout(probe->device, probe->image[slot], &subresource, &luma);
	subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	vkGetImageSubresourceLayout(probe->device, probe->image[slot], &subresource, &chroma);
	planes.luma = probe->image_map[slot] + luma.offset;
	planes.luma_pitch = (size_t)luma.rowPitch;
	planes.chroma = probe->image_map[slot] + chroma.offset;
	planes.chroma_pitch = (size_t)chroma.rowPitch;

	/* The window: the coded picture less the cropping, two pixels a crop unit (4:2:0 frames). */
	window.x = 2U * sps->frame_crop_left_offset;
	window.y = 2U * sps->frame_crop_top_offset;
	window.width = (sps->pic_width_in_mbs_minus1 + 1U) * 16U - 2U * (sps->frame_crop_left_offset + sps->frame_crop_right_offset);
	window.height = (sps->pic_height_in_map_units_minus1 + 1U) * 16U - 2U * (sps->frame_crop_top_offset + sps->frame_crop_bottom_offset);
	frame_hash(&planes, &window, text);
}

/* Keeps a decoded frame for its place in the display order; a full list is printed first. */
static void
probe_output_add(
	struct probe_output *output,
	int32_t poc,
	const char *hash)
{
	/* A list that is full is printed as it is. */
	if (output->pending_count >= PROBE_PENDING)
		probe_output_flush(output);

	/* The frame waits. */
	output->pending[output->pending_count].poc = poc;
	memcpy(output->pending[output->pending_count].hash, hash, sizeof(output->pending[0].hash));
	output->pending_count++;
}

/*
 * Prints the waiting frames in order of their order counts, each compared
 * with the next expected line when there is a file of them.
 */
static void
probe_output_flush(
	struct probe_output *output)
{
	char expected[80];
	char *line;
	uint32_t index;
	int differs;

	/* The display order. */
	qsort(output->pending, output->pending_count, sizeof(output->pending[0]), probe_frame_compare);

	/* Each frame, compared with the expected line when there is one. */
	for (index = 0U; index < output->pending_count; index++) {
		line = NULL;
		if (output->expect != NULL)
			line = fgets(expected, sizeof(expected), output->expect);
		if (line != NULL) {
			expected[strcspn(expected, "\n")] = '\0';
			differs = strcmp(expected, output->pending[index].hash);
			if (differs == 0) {
				output->matched++;
				printf("vkvideo-probe: frame %u %s match\n", output->printed, output->pending[index].hash);
			} else {
				printf("vkvideo-probe: frame %u %s MISMATCH (expected %s)\n", output->printed, output->pending[index].hash, expected);
			}
		} else {
			printf("vkvideo-probe: frame %u %s\n", output->printed, output->pending[index].hash);
		}
		output->printed++;
	}
	output->pending_count = 0U;
}

/* Orders two waiting frames by their order counts. */
static int
probe_frame_compare(
	const void *left,
	const void *right)
{
	const struct probe_frame *first;
	const struct probe_frame *second;

	/* The smaller order count first. */
	first = left;
	second = right;
	if (first->poc < second->poc)
		return -1;
	if (first->poc > second->poc)
		return 1;

	/* Succeeded: the same count. */
	return 0;
}

/*
 * Decodes the stream: the device, the decoder's objects, then picture
 * after picture as the DPB plans it, printing the frames' hashes in
 * display order (and whether each matches the expected one).  With
 * --frames=N only the first N pictures in decode order are decoded.
 */
static int
probe_run(
	struct probe *probe,
	const struct probe_options *options)
{
	static struct h264_stream stream;
	static struct h264_picture picture;
	static struct dpb dpb;
	static struct probe_output output;
	const StdVideoH264SequenceParameterSet *sps;
	struct dpb_plan plan;
	const char *reason;
	uint8_t *data;
	size_t size;
	uint32_t index;
	uint32_t frames;
	char text[65];
	int found;
	int first_sps;
	int error;

	/* The stream and its parameter sets. */
	data = probe_read(options->stream, &size);
	if (data == NULL)
		return 1;
	error = h264_open(&stream, data, size);
	if (error != 0) {
		fprintf(stderr, "vkvideo-probe: %s: a parameter set the probe cannot read\n", options->stream);
		free(data);
		return 1;
	}

	/* The profile of the first sequence set, and the largest coded picture of all of them. */
	first_sps = -1;
	for (index = 0U; index < H264_SPS_IDS; index++) {
		if (!stream.has_sps[index])
			continue;
		if (first_sps < 0)
			first_sps = (int)index;
		if ((stream.sps[index].pic_width_in_mbs_minus1 + 1U) * 16U > probe->extent.width)
			probe->extent.width = (stream.sps[index].pic_width_in_mbs_minus1 + 1U) * 16U;
		if ((stream.sps[index].pic_height_in_map_units_minus1 + 1U) * 16U > probe->extent.height)
			probe->extent.height = (stream.sps[index].pic_height_in_map_units_minus1 + 1U) * 16U;
		if (stream.sps[index].max_num_ref_frames > probe->max_references)
			probe->max_references = stream.sps[index].max_num_ref_frames;
	}
	if (first_sps < 0) {
		fprintf(stderr, "vkvideo-probe: %s: no sequence parameter set\n", options->stream);
		free(data);
		return 1;
	}
	memset(&probe->h264_profile, 0, sizeof(probe->h264_profile));
	probe->h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
	probe->h264_profile.stdProfileIdc = stream.sps[first_sps].profile_idc;
	probe->h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
	memset(&probe->profile, 0, sizeof(probe->profile));
	probe->profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	probe->profile.pNext = &probe->h264_profile;
	probe->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	probe->profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	probe->profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	probe->profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	memset(&probe->profile_list, 0, sizeof(probe->profile_list));
	probe->profile_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
	probe->profile_list.profileCount = 1U;
	probe->profile_list.pProfiles = &probe->profile;
	printf("vkvideo-probe: stream %s profile %u, %ux%u\n", options->stream, (unsigned)probe->h264_profile.stdProfileIdc, probe->extent.width, probe->extent.height);

	/* The DPB: the references the sequences allow and one slot for the picture decoded. */
	dpb_init(&dpb, probe->max_references);
	probe->max_references = dpb.max_references;
	probe->slots = dpb.slots;

	/* The device and the decoder's objects. */
	error = probe_device(probe);
	if (error == 0)
		error = probe_capabilities(probe);
	if (error == 0)
		error = probe_pictures(probe);
	if (error == 0)
		error = probe_bitstream(probe, size + 3U * H264_MAX_SLICES);
	if (error == 0)
		error = probe_session(probe);
	if (error == 0)
		error = probe_parameters(probe, &stream);
	if (error == 0)
		error = probe_commands(probe);
	if (error != 0) {
		free(data);
		return 1;
	}

	/* The expected hashes, when given. */
	memset(&output, 0, sizeof(output));
	if (options->expect != NULL) {
		output.expect = fopen(options->expect, "r");
		if (output.expect == NULL) {
			fprintf(stderr, "vkvideo-probe: %s: %s\n", options->expect, strerror(errno));
			free(data);
			return 1;
		}
	}

	/* Picture after picture. */
	frames = 0U;
	for (;;) {
		/* The next picture, or the end; a picture the probe cannot decode stops it. */
		if (options->frames != 0U && frames >= options->frames)
			break;
		found = h264_next_picture(&stream, &picture, &reason);
		if (found == 0)
			break;
		if (found < 0) {
			fprintf(stderr, "vkvideo-probe: picture %u: %s\n", frames, reason);
			error = 3;
			break;
		}

		/* An IDR picture comes after every frame before it in display order. */
		if (picture.info.flags.IdrPicFlag)
			probe_output_flush(&output);

		/* The DPB's plan for it. */
		sps = &stream.sps[picture.info.seq_parameter_set_id];
		reason = dpb_plan(&dpb, sps, &picture, &plan);
		if (reason != NULL) {
			fprintf(stderr, "vkvideo-probe: picture %u: %s\n", frames, reason);
			error = 3;
			break;
		}

		/* Decodes it and hashes the frame while its slot still holds it; a decode that did not complete is named. */
		error = probe_decode(probe, &stream, &picture, &plan);
		if (error != 0)
			break;
		if (probe->status != VK_QUERY_RESULT_STATUS_COMPLETE_KHR) {
			printf("vkvideo-probe: picture %u status %d\n", frames, (int)probe->status);
			output.failed++;
		}
		probe_hash(probe, (uint32_t)plan.setup, sps, text);

		/* The DPB after it, and the frame's place in the display order. */
		reason = dpb_mark(&dpb, sps, &picture, &plan);
		if (reason != NULL) {
			fprintf(stderr, "vkvideo-probe: picture %u: %s\n", frames, reason);
			error = 3;
			break;
		}
		probe_output_add(&output, picture.info.PicOrderCnt[0], text);
		frames++;
	}
	probe_output_flush(&output);
	if (output.expect != NULL)
		fclose(output.expect);
	free(data);

	/* The summary: every frame decoded, and matched when expected. */
	printf("vkvideo-probe: %u frames decoded", frames);
	if (options->expect != NULL)
		printf(", %u match the reference", output.matched);
	if (output.failed != 0U)
		printf(", %u failed", output.failed);
	printf("\n");
	if (error != 0)
		return error;
	if (options->expect != NULL && (output.matched != frames || frames == 0U))
		return 4;
	if (output.failed != 0U)
		return 5;

	/* Succeeded: the stream is decoded. */
	return 0;
}

/* Destroys what a run made, in reverse. */
static void
probe_close(
	struct probe *probe)
{
	uint32_t index;

	/* The device's objects, then the device. */
	if (probe->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(probe->device);
		if (probe->status_pool != VK_NULL_HANDLE)
			vkDestroyQueryPool(probe->device, probe->status_pool, NULL);
		if (probe->fence != VK_NULL_HANDLE)
			vkDestroyFence(probe->device, probe->fence, NULL);
		if (probe->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(probe->device, probe->pool, NULL);
		if (probe->parameters != VK_NULL_HANDLE)
			vkDestroyVideoSessionParametersKHR(probe->device, probe->parameters, NULL);
		if (probe->session != VK_NULL_HANDLE)
			vkDestroyVideoSessionKHR(probe->device, probe->session, NULL);
		for (index = 0U; index < probe->session_memory_count; index++)
			vkFreeMemory(probe->device, probe->session_memory[index], NULL);
		if (probe->buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(probe->device, probe->buffer, NULL);
		if (probe->buffer_memory != VK_NULL_HANDLE)
			vkFreeMemory(probe->device, probe->buffer_memory, NULL);
		for (index = 0U; index < DPB_SLOTS; index++) {
			if (probe->view[index] != VK_NULL_HANDLE)
				vkDestroyImageView(probe->device, probe->view[index], NULL);
			if (probe->image[index] != VK_NULL_HANDLE)
				vkDestroyImage(probe->device, probe->image[index], NULL);
			if (probe->image_memory[index] != VK_NULL_HANDLE)
				vkFreeMemory(probe->device, probe->image_memory[index], NULL);
		}
		vkDestroyDevice(probe->device, NULL);
	}

	/* The instance. */
	if (probe->instance != VK_NULL_HANDLE)
		vkDestroyInstance(probe->instance, NULL);
}

/* Reports a failed Vulkan call and gives the run's failure status. */
static int
probe_failed(
	VkResult result,
	const char *what)
{
	/* The call and its result. */
	fprintf(stderr, "vkvideo-probe: %s failed: %d\n", what, (int)result);
	return 1;
}

/* Reads a whole file; NULL (with a message) when it cannot. */
static uint8_t *
probe_read(
	const char *path,
	size_t *size)
{
	FILE *file;
	uint8_t *data;
	long length;
	size_t got;

	/* Opens it and finds its length. */
	file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "vkvideo-probe: %s: %s\n", path, strerror(errno));
		return NULL;
	}
	fseek(file, 0L, SEEK_END);
	length = ftell(file);
	fseek(file, 0L, SEEK_SET);
	if (length <= 0) {
		fprintf(stderr, "vkvideo-probe: %s: empty\n", path);
		fclose(file);
		return NULL;
	}

	/* Its bytes. */
	data = malloc((size_t)length);
	if (data == NULL) {
		fclose(file);
		return NULL;
	}
	got = fread(data, 1U, (size_t)length, file);
	fclose(file);
	if (got != (size_t)length) {
		free(data);
		return NULL;
	}

	/* Succeeded: the file's bytes. */
	*size = got;
	return data;
}
