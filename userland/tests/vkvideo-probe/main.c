/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * vkvideo-probe: lists what a Vulkan device offers for video decode, and
 * decodes an H.264 or H.265 elementary stream with Vulkan Video, printing
 * the SHA-256 of each frame.
 *
 *   vkvideo-probe --list
 *   vkvideo-probe [--frames=N] [--expect=FILE.sha256] [--time] [--dump=FILE] [--sampled] [--reset-at-idr] [--no-hash] STREAM.h264
 *   vkvideo-probe [--frames=N] [--expect=FILE.sha256] [--time] [--no-hash] [--raw=FILE] [--copy] [--h265] STREAM.h265
 *
 * The list names each queue family with its flags and video codec
 * operations and the device's video extensions; on a device without video
 * decode it shows no video family and no video extension.  The decode
 * reads the stream's parameter sets and pictures (h264.c), keeps the DPB
 * (dpb.c: one NV12 image a slot, the H.264 reference marking), decodes
 * each picture into a free slot's image with every reference picture of
 * the DPB, reads the image's planes through the image's subresource layout
 * (zedBSD's promise for an optimal NV12 image) and hashes the display
 * window as ffmpeg hashes a raw NV12 frame (frame.c).  The session's
 * parameters hold the stream's parameter sets in force (h264.c reads them
 * as they come); a set that is new or comes again with other content
 * makes new parameters before the next picture.  The frames are printed in
 * display order: by their order counts, between IDR pictures and pictures
 * with the memory management operation 5 (which come after every frame
 * before them); of more frames than the probe keeps waiting, the half of
 * the smaller counts is printed (as a DPB bumps them).
 * With --expect, each frame is compared with the file's line, and the exit
 * status says whether all matched.  A stream of an H.264 profile the
 * decoder does not take is refused before any decode, with its own exit
 * status (PROBE_REFUSED).  Where the video family reports result
 * status (ws083-p008), each decode is in a result status query, and a
 * decode that did not complete is named and fails the run.  With --time,
 * each decode is timed from the recording of its commands to its fence, and
 * a last line gives the count, the total, the mean and the longest
 * (ws083-p008's decode times; the hashing of the frames is not counted).
 * With --dump (ws236), each decoded picture's image is appended to FILE as
 * it is in memory, after a 512-byte text header that describes it (the
 * planes' offsets and pitches, the display window), in decode order; the
 * format of the kernel probe's /dev/mmal-probe, which
 * plan/ws236/tests/dump-compare.py compares with a reference decode.
 * With --sampled (ws236-p011), the pictures are made as a renderer makes
 * them to sample their planes (sampled usage, plane views allowed) and are
 * not mapped: after each decode its picture is copied with
 * vkCmdCopyImageToBuffer, both planes into a buffer of the image's layout,
 * in the decode's command buffer, and the hash and the dump read that
 * buffer (the decode's time then includes the copy).
 * With --reset-at-idr, every IDR picture after the first is
 * decoded behind a coding reset, as a player records one after a seek;
 * the frames are the same.  With --no-hash, the frames are decoded and
 * neither read nor printed (for the decode's time over a long stream,
 * where reading every frame would take longer than decoding it); it goes
 * with neither --expect nor --dump.
 *
 * With --h265, or a stream whose name ends in .h265, .hevc or .265, the
 * stream is H.265 and h265.c decodes it (Main and Main 10 profiles; a
 * profile, a bit depth or a size the decoder does not take is refused
 * with PROBE_REFUSED): --frames, --expect, --time and --no-hash as for
 * H.264, the frames printed in the DPB's output order.  With --raw, each
 * frame's display window is also appended to FILE as raw NV12 in that
 * order, the bytes the hash reads (frame-raw.c).  With --copy, each frame
 * is read through a copy into a buffer of plain rows when it is output
 * (copy-out.c), for a device whose optimal pictures the host cannot read.
 */

#include "h264.h"
#include "dpb.h"
#include "frame.h"
#include "probe.h"

#include <vulkan/vulkan.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int probe_arguments(int argc, char **argv, struct probe_options *options);
static int probe_instance(struct probe *probe);
static int probe_list(struct probe *probe);
static int probe_video_family(struct probe *probe, uint32_t *family);
static int probe_memory_type(const struct probe *probe, uint32_t bits, uint32_t *index);
static int probe_allocate(struct probe *probe, const VkMemoryRequirements *requirements, VkDeviceMemory *memory);
static int probe_picture(struct probe *probe, uint32_t slot);
static int probe_readback(struct probe *probe);
static void probe_copy_commands(struct probe *probe, uint32_t slot);
static int probe_parameters(struct probe *probe, const struct h264_stream *stream);
static void probe_status_pool(struct probe *probe);
static int probe_decode(struct probe *probe, const struct h264_stream *stream, const struct h264_picture *picture, const struct dpb_plan *plan);
static void probe_hash(struct probe *probe, uint32_t slot, const StdVideoH264SequenceParameterSet *sps, char text[65]);
static int probe_dump(struct probe *probe, uint32_t slot, const StdVideoH264SequenceParameterSet *sps, FILE *file, const char *stream, uint32_t picture, int32_t status, const char *hash);
static int probe_frame_compare(const void *left, const void *right);
static int probe_run(struct probe *probe, const struct probe_options *options);
static void probe_close(struct probe *probe);
static int probe_h265_name(const char *path);
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
		fprintf(stderr, "usage: vkvideo-probe --list | vkvideo-probe [--frames=N] [--expect=FILE.sha256] [--time] [--dump=FILE] [--sampled] [--reset-at-idr] [--no-hash] STREAM.h264 | vkvideo-probe [--frames=N] [--expect=FILE.sha256] [--time] [--no-hash] [--raw=FILE] [--copy] [--h265] STREAM.h265\n");
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

		/* --time: time each decode. */
		differs = strcmp(argument, "--time");
		if (differs == 0) {
			options->time = 1;
			continue;
		}

		/* --sampled: sampled pictures read back by a copy. */
		differs = strcmp(argument, "--sampled");
		if (differs == 0) {
			options->sampled = 1;
			continue;
		}

		/* --reset-at-idr: a coding reset before every IDR picture after the first. */
		differs = strcmp(argument, "--reset-at-idr");
		if (differs == 0) {
			options->reset_at_idr = 1;
			continue;
		}

		/* --no-hash: the frames are neither read nor printed. */
		differs = strcmp(argument, "--no-hash");
		if (differs == 0) {
			options->no_hash = 1;
			continue;
		}

		/* --h265: the stream is H.265 (as a name ending in .h265, .hevc or .265 says too). */
		differs = strcmp(argument, "--h265");
		if (differs == 0) {
			options->h265 = 1;
			continue;
		}

		/* --dump=FILE: the decoded images. */
		differs = strncmp(argument, "--dump=", 7U);
		if (differs == 0) {
			options->dump = argument + 7;
			continue;
		}

		/* --copy: the H.265 frames read by a copy into plain rows (copy-out.c). */
		differs = strcmp(argument, "--copy");
		if (differs == 0) {
			options->copy = 1;
			continue;
		}

		/* --raw=FILE: the H.265 frames' display windows as raw NV12. */
		differs = strncmp(argument, "--raw=", 6U);
		if (differs == 0) {
			options->raw = argument + 6;
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

	/* Exactly one of the list and a stream; --time goes with a stream. */
	if (options->list && options->stream != NULL)
		return -1;
	if (options->list && options->time)
		return -1;
	if (options->list && options->dump != NULL)
		return -1;
	if (options->list && options->sampled)
		return -1;
	if (options->no_hash && (options->expect != NULL || options->dump != NULL))
		return -1;
	if (!options->list && options->stream == NULL)
		return -1;

	/* A stream's name may say it is H.265; an H.265 decode neither dumps, nor samples, nor resets at IDR pictures. */
	if (options->stream != NULL && probe_h265_name(options->stream))
		options->h265 = 1;
	if (options->h265 && (options->list || options->dump != NULL || options->sampled || options->reset_at_idr))
		return -1;
	if (options->raw != NULL && (!options->h265 || options->no_hash))
		return -1;
	if (options->copy && !options->h265)
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
	VkPhysicalDeviceProperties properties;
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

	/* Whether its decoded planes are linear rows (every decoder but Intel's). */
	vkGetPhysicalDeviceProperties(probe->physical, &properties);
	probe->linear = 0;
	if (properties.vendorID != 0x8086U)
		probe->linear = 1;

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
	VkVideoCodecOperationFlagsKHR operation;
	uint32_t count;
	uint32_t index;
	uint32_t copy;

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

	/* The first one with video decode of the codec; the copy-out's family is it when it transfers, else the first that does. */
	operation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	if (probe->codec == PROBE_H265)
		operation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
	for (index = 0U; index < count; index++) {
		if ((families[index].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) == 0U)
			continue;
		if ((video[index].videoCodecOperations & operation) == 0U)
			continue;
		*family = index;
		probe->status_supported = status[index].queryResultStatusSupport;
		probe->copy_family = index;
		if ((families[index].queueFamilyProperties.queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0U) {
			for (copy = 0U; copy < count; copy++) {
				if ((families[copy].queueFamilyProperties.queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0U) {
					probe->copy_family = copy;
					break;
				}
			}
		}
		return 0;
	}

	/* None. */
	fprintf(stderr, "vkvideo-probe: no queue family decodes %s\n", probe->codec == PROBE_H265 ? "H.265" : "H.264");
	return 1;
}

/* Makes the device with one queue of the video family and the four video extensions. */
int
probe_device(
	struct probe *probe)
{
	static const char *const extensions[] = {
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
		VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME
	};
	static const char *const h265_extensions[] = {
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
		VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME
	};
	VkDeviceQueueCreateInfo queue[2];
	VkDeviceCreateInfo info;
	VkResult result;
	float priority;
	int error;

	/* The video family. */
	error = probe_video_family(probe, &probe->family);
	if (error != 0)
		return error;

	/* The device, its video queue and, when the copy-out's family is another, a queue of that. */
	priority = 1.0f;
	memset(queue, 0, sizeof(queue));
	queue[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue[0].queueFamilyIndex = probe->family;
	queue[0].queueCount = 1U;
	queue[0].pQueuePriorities = &priority;
	queue[1] = queue[0];
	queue[1].queueFamilyIndex = probe->copy_family;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.queueCreateInfoCount = 1U;
	if (probe->copy_family != probe->family)
		info.queueCreateInfoCount = 2U;
	info.pQueueCreateInfos = queue;
	info.enabledExtensionCount = 4U;
	info.ppEnabledExtensionNames = extensions;
	if (probe->codec == PROBE_H265)
		info.ppEnabledExtensionNames = h265_extensions;
	result = vkCreateDevice(probe->physical, &info, NULL, &probe->device);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateDevice");
	vkGetDeviceQueue(probe->device, probe->family, 0U, &probe->queue);
	vkGetDeviceQueue(probe->device, probe->copy_family, 0U, &probe->copy_queue);

	/* Succeeded: a device that decodes. */
	return 0;
}

/* Asks the device's H.264 decode capabilities and NV12 output format for the profile, and prints them. */
int
probe_capabilities(
	struct probe *probe)
{
	VkVideoDecodeH264CapabilitiesKHR h264;
	VkVideoDecodeH265CapabilitiesKHR h265;
	VkVideoDecodeCapabilitiesKHR decode;
	VkVideoCapabilitiesKHR capabilities;
	VkPhysicalDeviceVideoFormatInfoKHR format_info;
	VkVideoFormatPropertiesKHR formats[4];
	VkResult result;
	uint32_t count;
	uint32_t index;
	uint32_t level;

	/* The capabilities, with the decode and the codec's ones chained. */
	memset(&h264, 0, sizeof(h264));
	h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
	memset(&h265, 0, sizeof(h265));
	h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;
	memset(&decode, 0, sizeof(decode));
	decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
	decode.pNext = &h264;
	if (probe->codec == PROBE_H265)
		decode.pNext = &h265;
	memset(&capabilities, 0, sizeof(capabilities));
	capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
	capabilities.pNext = &decode;
	result = vkGetPhysicalDeviceVideoCapabilitiesKHR(probe->physical, &probe->profile, &capabilities);
	if (probe->codec == PROBE_H265 &&
	    (result == VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR || result == VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR)) {
		fprintf(stderr, "vkvideo-probe: refused: the decoder does not take the stream's H.265 profile (profile_idc %u, %s bit)\n",
			(unsigned)probe->h265_profile.stdProfileIdc,
			probe->profile.lumaBitDepth == VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR ? "8" : "10");
		return PROBE_REFUSED;
	}
	if (result == VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR) {
		fprintf(stderr, "vkvideo-probe: refused: the decoder does not take the stream's H.264 profile (profile_idc %u)\n",
			(unsigned)probe->h264_profile.stdProfileIdc);
		return PROBE_REFUSED;
	}
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
	level = (uint32_t)h264.maxLevelIdc;
	if (probe->codec == PROBE_H265)
		level = (uint32_t)h265.maxLevelIdc;
	printf("vkvideo-probe: capabilities max %ux%u slots %u references %u level %u flags 0x%x decode 0x%x\n",
	       capabilities.maxCodedExtent.width,
	       capabilities.maxCodedExtent.height,
	       capabilities.maxDpbSlots,
	       capabilities.maxActiveReferencePictures,
	       (unsigned)level,
	       capabilities.flags,
	       decode.flags);

	/* An H.265 stream larger than the decoder's pictures is outside its limits: refused. */
	if (probe->codec == PROBE_H265 &&
	    (probe->extent.width > capabilities.maxCodedExtent.width || probe->extent.height > capabilities.maxCodedExtent.height)) {
		fprintf(stderr, "vkvideo-probe: refused: the stream's %ux%u is larger than the decoder's %ux%u\n",
			probe->extent.width, probe->extent.height, capabilities.maxCodedExtent.width, capabilities.maxCodedExtent.height);
		return PROBE_REFUSED;
	}

	/* An H.265 session takes the slots and references the decoder offers, up to the probe's. */
	if (probe->codec == PROBE_H265) {
		if (probe->slots > capabilities.maxDpbSlots)
			probe->slots = capabilities.maxDpbSlots;
		if (probe->max_references > capabilities.maxActiveReferencePictures)
			probe->max_references = capabilities.maxActiveReferencePictures;
	}

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
int
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
	uint32_t families[2];
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
	if (probe->copy_out)
		image.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (probe->sampled) {
		/* A sampled picture, whose planes a renderer views, read back by a copy. */
		image.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
		image.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	/* One queue owns it (both families share it when the copy-out's is another), and its contents start undefined. */
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	families[0] = probe->family;
	families[1] = probe->copy_family;
	if (probe->copy_family != probe->family) {
		image.sharingMode = VK_SHARING_MODE_CONCURRENT;
		image.queueFamilyIndexCount = 2U;
		image.pQueueFamilyIndices = families;
	}
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

	/* A sampled picture is read through the readback's copy; any other through its own mapping. */
	if (probe->sampled) {
		error = probe_readback(probe);
		if (error != 0)
			return error;
		probe->image_map[slot] = probe->readback_map;
	} else {
		result = vkMapMemory(probe->device, probe->image_memory[slot], 0U, VK_WHOLE_SIZE, 0U, &map);
		if (result != VK_SUCCESS)
			return probe_failed(result, "vkMapMemory");
		probe->image_map[slot] = map;
	}

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

/* Makes the readback buffer of the pictures' layout, once, with its memory (mapped). */
static int
probe_readback(
	struct probe *probe)
{
	VkImageSubresource subresource;
	VkSubresourceLayout chroma;
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkResult result;
	void *map;
	int error;

	/* One buffer serves every picture. */
	if (probe->readback != VK_NULL_HANDLE)
		return 0;

	/* As large as a picture's layout: the chroma plane is the last. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	vkGetImageSubresourceLayout(probe->device, probe->image[0], &subresource, &chroma);
	probe->readback_size = chroma.offset + chroma.size;

	/* The buffer: a copy's destination. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = probe->readback_size;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(probe->device, &buffer, NULL, &probe->readback);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateBuffer");

	/* Its memory, bound and mapped. */
	vkGetBufferMemoryRequirements(probe->device, probe->readback, &requirements);
	error = probe_allocate(probe, &requirements, &probe->readback_memory);
	if (error != 0)
		return error;
	result = vkBindBufferMemory(probe->device, probe->readback, probe->readback_memory, 0U);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkBindBufferMemory");
	result = vkMapMemory(probe->device, probe->readback_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkMapMemory");
	probe->readback_map = map;

	/* Succeeded: the readback is ready. */
	return 0;
}

/*
 * Records the copy of a slot's picture into the readback: both planes at
 * the image layout's offsets and pitches, between barriers that take the
 * image from the DPB layout and back.
 */
static void
probe_copy_commands(
	struct probe *probe,
	uint32_t slot)
{
	VkImageSubresource subresource;
	VkSubresourceLayout luma;
	VkSubresourceLayout chroma;
	VkImageMemoryBarrier barrier;
	VkBufferImageCopy regions[2];

	/* The planes' places in the layout. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	vkGetImageSubresourceLayout(probe->device, probe->image[slot], &subresource, &luma);
	subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	vkGetImageSubresourceLayout(probe->device, probe->image[slot], &subresource, &chroma);

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
	vkCmdPipelineBarrier(probe->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* The luma plane, one byte a texel, then the chroma plane, two. */
	memset(regions, 0, sizeof(regions));
	regions[0].bufferOffset = luma.offset;
	regions[0].bufferRowLength = (uint32_t)luma.rowPitch;
	regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	regions[0].imageSubresource.layerCount = 1U;
	regions[0].imageExtent.width = probe->extent.width;
	regions[0].imageExtent.height = probe->extent.height;
	regions[0].imageExtent.depth = 1U;
	regions[1].bufferOffset = chroma.offset;
	regions[1].bufferRowLength = (uint32_t)(chroma.rowPitch / 2U);
	regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	regions[1].imageSubresource.layerCount = 1U;
	regions[1].imageExtent.width = probe->extent.width / 2U;
	regions[1].imageExtent.height = probe->extent.height / 2U;
	regions[1].imageExtent.depth = 1U;
	vkCmdCopyImageToBuffer(probe->command, probe->image[slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probe->readback, 2U, regions);

	/* Back to the DPB layout for the next decodes. */
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
	vkCmdPipelineBarrier(probe->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
}

/* Makes the bitstream buffer of a picture's bytes, and its memory (mapped). */
int
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
int
probe_session(
	struct probe *probe)
{
	static const VkExtensionProperties header = {
		VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME,
		VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION
	};
	static const VkExtensionProperties h265_header = {
		VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME,
		VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION
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
	if (probe->codec == PROBE_H265)
		info.pStdHeaderVersion = &h265_header;
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

/*
 * Makes the session's parameters from the stream's parameter sets in
 * force, in place of the parameters before (no decode is in flight: each
 * is waited for).
 */
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
	if (probe->parameters != VK_NULL_HANDLE) {
		vkDestroyVideoSessionParametersKHR(probe->device, probe->parameters, NULL);
		probe->parameters = VK_NULL_HANDLE;
	}
	result = vkCreateVideoSessionParametersKHR(probe->device, &info, NULL, &probe->parameters);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateVideoSessionParametersKHR");
	probe->parameters_generation = stream->generation;
	probe->parameters_made++;

	/* The first parameters are named; the later ones are counted at the end. */
	if (probe->parameters_made == 1U)
		printf("vkvideo-probe: parameters %u SPS, %u PPS\n", sps_count, pps_count);

	/* Succeeded: the parameters. */
	return 0;
}

/* Makes the command pool and buffer of the video family, and the fence. */
int
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

	/* The copy-out's pool and buffer: the video family's, or their own of the copy-out's family. */
	probe->copy_command = probe->command;
	if (probe->copy_family != probe->family) {
		pool.queueFamilyIndex = probe->copy_family;
		result = vkCreateCommandPool(probe->device, &pool, NULL, &probe->copy_pool);
		if (result != VK_SUCCESS)
			return probe_failed(result, "vkCreateCommandPool copy");
		command.commandPool = probe->copy_pool;
		result = vkAllocateCommandBuffers(probe->device, &command, &probe->copy_command);
		if (result != VK_SUCCESS)
			return probe_failed(result, "vkAllocateCommandBuffers copy");
	}

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

	/* A sampled picture is read back by a copy, which waits for its frame. */
	if (probe->sampled)
		probe_copy_commands(probe, (uint32_t)plan->setup);
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
	planes.linear = probe->linear;

	/* The window: the coded picture less the cropping, two pixels a crop unit (4:2:0 frames). */
	window.x = 2U * sps->frame_crop_left_offset;
	window.y = 2U * sps->frame_crop_top_offset;
	window.width = (sps->pic_width_in_mbs_minus1 + 1U) * 16U - 2U * (sps->frame_crop_left_offset + sps->frame_crop_right_offset);
	window.height = (sps->pic_height_in_map_units_minus1 + 1U) * 16U - 2U * (sps->frame_crop_top_offset + sps->frame_crop_bottom_offset);
	frame_hash(&planes, &window, text);
}

/*
 * Appends a slot's image to the dump: a 512-byte header line (padded with
 * spaces, ending in a newline), then the image's bytes from its start to
 * the end of its chroma plane.
 */
static int
probe_dump(
	struct probe *probe,
	uint32_t slot,
	const StdVideoH264SequenceParameterSet *sps,
	FILE *file,
	const char *stream,
	uint32_t picture,
	int32_t status,
	const char *hash)
{
	VkImageSubresource subresource;
	VkSubresourceLayout luma;
	VkSubresourceLayout chroma;
	char header[512];
	const char *name;
	size_t bytes;
	size_t written;
	int length;

	/* The two planes. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	vkGetImageSubresourceLayout(probe->device, probe->image[slot], &subresource, &luma);
	subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	vkGetImageSubresourceLayout(probe->device, probe->image[slot], &subresource, &chroma);
	bytes = (size_t)(chroma.offset + chroma.size);

	/* The header: the stream's file name, the picture, its planes and its display window. */
	name = strrchr(stream, '/');
	name = name != NULL ? name + 1 : stream;
	memset(header, ' ', sizeof(header));
	length = snprintf(header, sizeof(header) - 1U,
			  "zdump stream=vulkan file=%s run=v out=%u pts=%u pts_display=-1 display=-1 match=none sum=0 status=%d sha256=%s fmt=3231564e width=%u height=%u crop_x=%u crop_y=%u crop_width=%u crop_height=%u planes=2 offset0=%llu offset1=%llu pitch0=%llu pitch1=%llu length=%zu linear=%d",
			  name, (unsigned)picture, (unsigned)picture, (int)status, hash, probe->extent.width, probe->extent.height,
			  2U * sps->frame_crop_left_offset, 2U * sps->frame_crop_top_offset,
			  (sps->pic_width_in_mbs_minus1 + 1U) * 16U - 2U * (sps->frame_crop_left_offset + sps->frame_crop_right_offset),
			  (sps->pic_height_in_map_units_minus1 + 1U) * 16U - 2U * (sps->frame_crop_top_offset + sps->frame_crop_bottom_offset),
			  (unsigned long long)luma.offset, (unsigned long long)chroma.offset,
			  (unsigned long long)luma.rowPitch, (unsigned long long)chroma.rowPitch, bytes, probe->linear);
	if (length < 0)
		return 1;
	if ((size_t)length < sizeof(header) - 1U)
		header[length] = ' ';
	header[sizeof(header) - 1U] = '\n';

	/* The header and the bytes. */
	written = fwrite(header, 1U, sizeof(header), file);
	if (written == sizeof(header))
		written = fwrite(probe->image_map[slot], 1U, bytes, file);
	else
		written = 0;
	if (written != bytes) {
		fprintf(stderr, "vkvideo-probe: the dump could not be written\n");
		return 1;
	}

	/* Succeeded: the picture is in the dump. */
	return 0;
}

/* Keeps a decoded frame for its place in the display order; a full list is printed first. */
void
probe_output_add(
	struct probe_output *output,
	int32_t poc,
	const char *hash)
{
	/* Of a full list, the half of the smaller counts is printed. */
	if (output->pending_count >= PROBE_PENDING)
		probe_output_flush(output, PROBE_PENDING / 2U);

	/* The frame waits. */
	output->pending[output->pending_count].poc = poc;
	memcpy(output->pending[output->pending_count].hash, hash, sizeof(output->pending[0].hash));
	output->pending_count++;
}

/*
 * Prints the waiting frames in order of their order counts, each compared
 * with the next expected line when there is a file of them, but for the
 * `keep` of the largest counts, which go on waiting.
 */
void
probe_output_flush(
	struct probe_output *output,
	uint32_t keep)
{
	char expected[80];
	char *line;
	uint32_t index;
	uint32_t printing;
	int differs;

	/* The display order, and the frames printed now. */
	qsort(output->pending, output->pending_count, sizeof(output->pending[0]), probe_frame_compare);
	printing = 0U;
	if (output->pending_count > keep)
		printing = output->pending_count - keep;

	/* Each frame, compared with the expected line when there is one. */
	for (index = 0U; index < printing; index++) {
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

	/* The frames kept go to the front. */
	memmove(output->pending, output->pending + printing, (output->pending_count - printing) * sizeof(output->pending[0]));
	output->pending_count -= printing;
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
	struct probe_timing timing;
	const StdVideoH264SequenceParameterSet *sps;
	struct dpb_plan plan;
	const char *reason;
	uint8_t *data;
	size_t size;
	uint32_t frames;
	char text[65];
	uint64_t started;
	uint64_t elapsed;
	FILE *dump;
	int found;
	int error;

	/* The stream; an H.265 one is h265.c's to decode, which frees it. */
	data = probe_read(options->stream, &size);
	if (data == NULL)
		return 1;
	if (options->h265)
		return probe_h265_run(probe, options, data, size);

	/* Its parameter sets. */
	error = h264_open(&stream, data, size);
	if (error != 0) {
		fprintf(stderr, "vkvideo-probe: %s: a parameter set the probe cannot read\n", options->stream);
		free(data);
		return 1;
	}

	/* The profile of the first sequence set, and the largest coded picture and the most references of all of them. */
	if (!stream.has_profile) {
		fprintf(stderr, "vkvideo-probe: %s: no sequence parameter set\n", options->stream);
		free(data);
		return 1;
	}
	memset(&probe->h264_profile, 0, sizeof(probe->h264_profile));
	probe->h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
	probe->h264_profile.stdProfileIdc = (StdVideoH264ProfileIdc)stream.profile_idc;
	probe->h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
	probe->extent.width = stream.max_width;
	probe->extent.height = stream.max_height;
	probe->max_references = stream.max_references;
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
	probe->sampled = options->sampled;

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
		error = probe_commands(probe);
	if (error == PROBE_REFUSED) {
		free(data);
		return PROBE_REFUSED;
	}
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

	/* The dump, when asked for. */
	dump = NULL;
	if (options->dump != NULL) {
		dump = fopen(options->dump, "wb");
		if (dump == NULL) {
			fprintf(stderr, "vkvideo-probe: %s: %s\n", options->dump, strerror(errno));
			if (output.expect != NULL)
				fclose(output.expect);
			free(data);
			return 1;
		}
	}

	/* Picture after picture, with no decode timed yet. */
	memset(&timing, 0, sizeof(timing));
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

		/* An IDR picture and one with operation 5 come after every frame before them in display order. */
		if (picture.info.flags.IdrPicFlag || picture.restart)
			probe_output_flush(&output, 0U);

		/* Parameters with the sets in force, when they changed. */
		if (probe->parameters == VK_NULL_HANDLE || stream.generation != probe->parameters_generation) {
			error = probe_parameters(probe, &stream);
			if (error != 0)
				break;
		}

		/* The DPB's plan for it. */
		sps = &stream.sps[picture.info.seq_parameter_set_id];
		reason = dpb_plan(&dpb, sps, &picture, &plan);
		if (reason != NULL) {
			fprintf(stderr, "vkvideo-probe: picture %u: %s\n", frames, reason);
			error = 3;
			break;
		}
		if (options->reset_at_idr && picture.info.flags.IdrPicFlag && frames != 0U)
			plan.reset = 1;

		/* Decodes it and hashes the frame while its slot still holds it; a decode that did not complete is named. */
		started = probe_now();
		error = probe_decode(probe, &stream, &picture, &plan);
		elapsed = probe_now() - started;
		if (error != 0)
			break;
		if (probe->status != VK_QUERY_RESULT_STATUS_COMPLETE_KHR) {
			printf("vkvideo-probe: picture %u status %d\n", frames, (int)probe->status);
			output.failed++;
		}
		if (!options->no_hash)
			probe_hash(probe, (uint32_t)plan.setup, sps, text);
		if (dump != NULL) {
			error = probe_dump(probe, (uint32_t)plan.setup, sps, dump, options->stream, frames, (int32_t)probe->status, text);
			if (error != 0)
				break;
		}

		/* Counts the decode's time, from its commands to its fence; the hashing is not in it. */
		timing.count++;
		timing.total += elapsed;
		if (elapsed > timing.longest)
			timing.longest = elapsed;

		/* The DPB after it, and the frame's place in the display order. */
		reason = dpb_mark(&dpb, sps, &picture, &plan);
		if (reason != NULL) {
			fprintf(stderr, "vkvideo-probe: picture %u: %s\n", frames, reason);
			error = 3;
			break;
		}
		if (!options->no_hash)
			probe_output_add(&output, picture.display_poc, text);
		frames++;
	}
	probe_output_flush(&output, 0U);
	if (output.expect != NULL)
		fclose(output.expect);
	if (dump != NULL && fclose(dump) != 0 && error == 0) {
		fprintf(stderr, "vkvideo-probe: %s: %s\n", options->dump, strerror(errno));
		error = 1;
	}
	free(data);

	/* The summary: every frame decoded, and matched when expected. */
	if (probe->parameters_made > 1U)
		printf("vkvideo-probe: parameters made %u times for the sets the stream sent again\n", probe->parameters_made);
	printf("vkvideo-probe: %u frames decoded", frames);
	if (options->expect != NULL)
		printf(", %u match the reference", output.matched);
	if (output.failed != 0U)
		printf(", %u failed", output.failed);
	printf("\n");

	/* The decode times, when asked for. */
	if (options->time)
		probe_timing_print(&timing);

	/* Reports the run's failure: a stopped decode, a frame that did not match, a decode that did not complete. */
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
		if (probe->copy_pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(probe->device, probe->copy_pool, NULL);
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
		if (probe->readback != VK_NULL_HANDLE)
			vkDestroyBuffer(probe->device, probe->readback, NULL);
		if (probe->readback_memory != VK_NULL_HANDLE)
			vkFreeMemory(probe->device, probe->readback_memory, NULL);
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
int
probe_failed(
	VkResult result,
	const char *what)
{
	/* The call and its result. */
	fprintf(stderr, "vkvideo-probe: %s failed: %d\n", what, (int)result);
	return 1;
}

/* Reports whether a stream's name says it is H.265: it ends in .h265, .hevc or .265. */
static int
probe_h265_name(
	const char *path)
{
	static const char *const endings[] = { ".h265", ".hevc", ".265" };
	size_t length;
	size_t ending;
	uint32_t index;
	int differs;

	/* Each ending the name may have. */
	length = strlen(path);
	for (index = 0U; index < sizeof(endings) / sizeof(endings[0]); index++) {
		ending = strlen(endings[index]);
		if (length < ending)
			continue;
		differs = strcmp(path + length - ending, endings[index]);
		if (differs == 0)
			return 1;
	}

	/* Another name: H.264 unless --h265 says otherwise. */
	return 0;
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

/* Reads the monotonic clock, in nanoseconds. */
uint64_t
probe_now(
	void)
{
	struct timespec now;

	/* The clock; it does not fail for CLOCK_MONOTONIC. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the time in nanoseconds. */
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/* Prints the decode times: how many, the total, the mean and the longest, in milliseconds. */
void
probe_timing_print(
	const struct probe_timing *timing)
{
	uint64_t mean;

	/* A run that decoded nothing has no times. */
	if (timing->count == 0U) {
		printf("vkvideo-probe: decode time: no decode\n");
		return;
	}

	/* The mean of the decodes. */
	mean = timing->total / timing->count;

	/* The line, in milliseconds with three decimals. */
	printf("vkvideo-probe: decode time: %u decodes, total %llu.%03llu ms, mean %llu.%03llu ms, longest %llu.%03llu ms\n",
	       timing->count,
	       (unsigned long long)(timing->total / 1000000ULL),
	       (unsigned long long)(timing->total / 1000ULL % 1000ULL),
	       (unsigned long long)(mean / 1000000ULL),
	       (unsigned long long)(mean / 1000ULL % 1000ULL),
	       (unsigned long long)(timing->longest / 1000000ULL),
	       (unsigned long long)(timing->longest / 1000ULL % 1000ULL));
}
