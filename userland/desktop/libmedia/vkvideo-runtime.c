/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Standard Vulkan Video execution and NV12 transfer readback, adapted from the repository's Zlib WS083 probe.
 * Optimal images are device-owned; host pictures are populated only through vkCmdCopyImageToBuffer.
 */
#define VK_NO_PROTOTYPES
#include "vkvideo-runtime.h"
#include "media-private.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define VKVIDEO_BINDINGS 32U
#define VKVIDEO_FAMILIES 64U
#define VKVIDEO_WAIT_NS 5000000000ULL
#define VKVIDEO_SOURCE_MAX (65U * 1024U * 1024U)

/* Standard entry points belong to a runtime's instance/device dispatch, never to a particular GPU implementation. */
struct vkvideo_functions {
	PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
	PFN_vkAllocateMemory vkAllocateMemory;
	PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
	PFN_vkBindBufferMemory vkBindBufferMemory;
	PFN_vkBindImageMemory vkBindImageMemory;
	PFN_vkBindVideoSessionMemoryKHR vkBindVideoSessionMemoryKHR;
	PFN_vkCmdBeginQuery vkCmdBeginQuery;
	PFN_vkCmdBeginVideoCodingKHR vkCmdBeginVideoCodingKHR;
	PFN_vkCmdControlVideoCodingKHR vkCmdControlVideoCodingKHR;
	PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;
	PFN_vkCmdDecodeVideoKHR vkCmdDecodeVideoKHR;
	PFN_vkCmdEndQuery vkCmdEndQuery;
	PFN_vkCmdEndVideoCodingKHR vkCmdEndVideoCodingKHR;
	PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
	PFN_vkCmdPipelineBarrier2KHR vkCmdPipelineBarrier2KHR;
	PFN_vkCmdResetQueryPool vkCmdResetQueryPool;
	PFN_vkCreateBuffer vkCreateBuffer;
	PFN_vkCreateCommandPool vkCreateCommandPool;
	PFN_vkCreateDevice vkCreateDevice;
	PFN_vkCreateFence vkCreateFence;
	PFN_vkCreateImage vkCreateImage;
	PFN_vkCreateSemaphore vkCreateSemaphore;
	PFN_vkDestroySemaphore vkDestroySemaphore;
	PFN_vkCreateImageView vkCreateImageView;
	PFN_vkCreateInstance vkCreateInstance;
	PFN_vkCreateQueryPool vkCreateQueryPool;
	PFN_vkCreateVideoSessionKHR vkCreateVideoSessionKHR;
	PFN_vkCreateVideoSessionParametersKHR vkCreateVideoSessionParametersKHR;
	PFN_vkDestroyBuffer vkDestroyBuffer;
	PFN_vkDestroyCommandPool vkDestroyCommandPool;
	PFN_vkDestroyDevice vkDestroyDevice;
	PFN_vkDestroyFence vkDestroyFence;
	PFN_vkDestroyImage vkDestroyImage;
	PFN_vkDestroyImageView vkDestroyImageView;
	PFN_vkDestroyInstance vkDestroyInstance;
	PFN_vkDestroyQueryPool vkDestroyQueryPool;
	PFN_vkDestroyVideoSessionKHR vkDestroyVideoSessionKHR;
	PFN_vkDestroyVideoSessionParametersKHR vkDestroyVideoSessionParametersKHR;
	PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
	PFN_vkEndCommandBuffer vkEndCommandBuffer;
	PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
	PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
	PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges;
	PFN_vkFreeMemory vkFreeMemory;
	PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
	PFN_vkGetDeviceQueue vkGetDeviceQueue;
	PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
	PFN_vkGetPhysicalDeviceFeatures2KHR vkGetPhysicalDeviceFeatures2KHR;
	PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR vkGetPhysicalDeviceQueueFamilyProperties2KHR;
	PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR vkGetPhysicalDeviceVideoCapabilitiesKHR;
	PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR vkGetPhysicalDeviceVideoFormatPropertiesKHR;
	PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
	PFN_vkGetVideoSessionMemoryRequirementsKHR vkGetVideoSessionMemoryRequirementsKHR;
	PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges;
	PFN_vkMapMemory vkMapMemory;
	PFN_vkQueueSubmit vkQueueSubmit;
	PFN_vkResetCommandBuffer vkResetCommandBuffer;
	PFN_vkResetFences vkResetFences;
	PFN_vkUnmapMemory vkUnmapMemory;
	PFN_vkWaitForFences vkWaitForFences;
};

/* One decoder owns its Vulkan context; submitted memory survives a failed wait until the device retires it. */
struct vkvideo_runtime {
	struct vkvideo_functions fn;
	VkInstance instance;
	VkPhysicalDevice physical;
	uint32_t family;
	VkDevice device;
	VkQueue queue;
	uint32_t transfer_family;
	VkQueue transfer_queue;
	VkCommandPool transfer_pool;
	VkCommandBuffer transfer_command;
	VkSemaphore decoded;
	VkSemaphore returned;
	int returned_pending;
	VkPhysicalDeviceMemoryProperties memory;
	VkVideoDecodeH264ProfileInfoKHR h264_profile;
	VkVideoProfileInfoKHR profile;
	VkVideoProfileListInfoKHR profile_list;
	VkVideoCapabilitiesKHR capabilities;
	VkVideoDecodeCapabilitiesKHR decode_caps;
	VkVideoDecodeH264CapabilitiesKHR h264_caps;
	StdVideoH264LevelIdc level;
	VkExtent2D extent;
	VkExtent2D image_extent;
	VkImageUsageFlags reference_usage;
	VkVideoSessionKHR session;
	VkDeviceMemory session_memory[VKVIDEO_BINDINGS];
	uint32_t session_memory_count;
	VkVideoSessionParametersKHR parameters;
	uint32_t slots;
	uint32_t max_references;
	int distinct;
	VkImage image[H264_DPB_SLOTS + 1U];
	VkDeviceMemory image_memory[H264_DPB_SLOTS + 1U];
	VkImageView view[H264_DPB_SLOTS + 1U];
	VkBuffer buffer;
	VkDeviceMemory buffer_memory;
	uint8_t *buffer_map;
	VkDeviceSize buffer_size;
	int buffer_coherent;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	uint8_t *readback_map;
	int readback_coherent;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkBool32 status_supported;
	VkQueryPool status_pool;
	int32_t status;
	int initialized;
	int reset;
	int inflight;
};

/* Entry-point names and destination offsets are immutable dispatch metadata for the process. */
struct vkvideo_symbol {
	const char *name;
	size_t offset;
	int instance;
};

/* This immutable table is read while opening each runtime and contains no proprietary device names. */
static const struct vkvideo_symbol vkvideo_symbols[] = {
	{"vkAllocateMemory", offsetof(struct vkvideo_functions, vkAllocateMemory), 0},
	{"vkAllocateCommandBuffers", offsetof(struct vkvideo_functions, vkAllocateCommandBuffers), 0},
	{"vkBeginCommandBuffer", offsetof(struct vkvideo_functions, vkBeginCommandBuffer), 0},
	{"vkBindBufferMemory", offsetof(struct vkvideo_functions, vkBindBufferMemory), 0},
	{"vkBindImageMemory", offsetof(struct vkvideo_functions, vkBindImageMemory), 0},
	{"vkBindVideoSessionMemoryKHR", offsetof(struct vkvideo_functions, vkBindVideoSessionMemoryKHR), 0},
	{"vkCmdBeginQuery", offsetof(struct vkvideo_functions, vkCmdBeginQuery), 0},
	{"vkCmdBeginVideoCodingKHR", offsetof(struct vkvideo_functions, vkCmdBeginVideoCodingKHR), 0},
	{"vkCmdControlVideoCodingKHR", offsetof(struct vkvideo_functions, vkCmdControlVideoCodingKHR), 0},
	{"vkCmdCopyImageToBuffer", offsetof(struct vkvideo_functions, vkCmdCopyImageToBuffer), 0},
	{"vkCmdDecodeVideoKHR", offsetof(struct vkvideo_functions, vkCmdDecodeVideoKHR), 0},
	{"vkCmdEndQuery", offsetof(struct vkvideo_functions, vkCmdEndQuery), 0},
	{"vkCmdEndVideoCodingKHR", offsetof(struct vkvideo_functions, vkCmdEndVideoCodingKHR), 0},
	{"vkCmdPipelineBarrier", offsetof(struct vkvideo_functions, vkCmdPipelineBarrier), 0},
	{"vkCmdPipelineBarrier2KHR", offsetof(struct vkvideo_functions, vkCmdPipelineBarrier2KHR), 0},
	{"vkCmdResetQueryPool", offsetof(struct vkvideo_functions, vkCmdResetQueryPool), 0},
	{"vkCreateBuffer", offsetof(struct vkvideo_functions, vkCreateBuffer), 0},
	{"vkCreateCommandPool", offsetof(struct vkvideo_functions, vkCreateCommandPool), 0},
	{"vkCreateDevice", offsetof(struct vkvideo_functions, vkCreateDevice), 1},
	{"vkCreateFence", offsetof(struct vkvideo_functions, vkCreateFence), 0},
	{"vkCreateSemaphore", offsetof(struct vkvideo_functions, vkCreateSemaphore), 0},
	{"vkDestroySemaphore", offsetof(struct vkvideo_functions, vkDestroySemaphore), 0},
	{"vkCreateImage", offsetof(struct vkvideo_functions, vkCreateImage), 0},
	{"vkCreateImageView", offsetof(struct vkvideo_functions, vkCreateImageView), 0},
	{"vkCreateInstance", offsetof(struct vkvideo_functions, vkCreateInstance), 1},
	{"vkCreateQueryPool", offsetof(struct vkvideo_functions, vkCreateQueryPool), 0},
	{"vkCreateVideoSessionKHR", offsetof(struct vkvideo_functions, vkCreateVideoSessionKHR), 0},
	{"vkCreateVideoSessionParametersKHR", offsetof(struct vkvideo_functions, vkCreateVideoSessionParametersKHR), 0},
	{"vkDestroyBuffer", offsetof(struct vkvideo_functions, vkDestroyBuffer), 0},
	{"vkDestroyCommandPool", offsetof(struct vkvideo_functions, vkDestroyCommandPool), 0},
	{"vkDestroyDevice", offsetof(struct vkvideo_functions, vkDestroyDevice), 0},
	{"vkDestroyFence", offsetof(struct vkvideo_functions, vkDestroyFence), 0},
	{"vkDestroyImage", offsetof(struct vkvideo_functions, vkDestroyImage), 0},
	{"vkDestroyImageView", offsetof(struct vkvideo_functions, vkDestroyImageView), 0},
	{"vkDestroyInstance", offsetof(struct vkvideo_functions, vkDestroyInstance), 1},
	{"vkDestroyQueryPool", offsetof(struct vkvideo_functions, vkDestroyQueryPool), 0},
	{"vkDestroyVideoSessionKHR", offsetof(struct vkvideo_functions, vkDestroyVideoSessionKHR), 0},
	{"vkDestroyVideoSessionParametersKHR", offsetof(struct vkvideo_functions, vkDestroyVideoSessionParametersKHR), 0},
	{"vkDeviceWaitIdle", offsetof(struct vkvideo_functions, vkDeviceWaitIdle), 0},
	{"vkEndCommandBuffer", offsetof(struct vkvideo_functions, vkEndCommandBuffer), 0},
	{"vkEnumerateDeviceExtensionProperties", offsetof(struct vkvideo_functions, vkEnumerateDeviceExtensionProperties), 1},
	{"vkEnumeratePhysicalDevices", offsetof(struct vkvideo_functions, vkEnumeratePhysicalDevices), 1},
	{"vkFlushMappedMemoryRanges", offsetof(struct vkvideo_functions, vkFlushMappedMemoryRanges), 0},
	{"vkFreeMemory", offsetof(struct vkvideo_functions, vkFreeMemory), 0},
	{"vkGetBufferMemoryRequirements", offsetof(struct vkvideo_functions, vkGetBufferMemoryRequirements), 0},
	{"vkGetDeviceProcAddr", offsetof(struct vkvideo_functions, vkGetDeviceProcAddr), 1},
	{"vkGetDeviceQueue", offsetof(struct vkvideo_functions, vkGetDeviceQueue), 0},
	{"vkGetImageMemoryRequirements", offsetof(struct vkvideo_functions, vkGetImageMemoryRequirements), 0},
	{"vkGetInstanceProcAddr", offsetof(struct vkvideo_functions, vkGetInstanceProcAddr), 1},
	{"vkGetPhysicalDeviceFeatures2KHR", offsetof(struct vkvideo_functions, vkGetPhysicalDeviceFeatures2KHR), 1},
	{"vkGetPhysicalDeviceMemoryProperties", offsetof(struct vkvideo_functions, vkGetPhysicalDeviceMemoryProperties), 1},
	{"vkGetPhysicalDeviceQueueFamilyProperties2KHR", offsetof(struct vkvideo_functions, vkGetPhysicalDeviceQueueFamilyProperties2KHR), 1},
	{"vkGetPhysicalDeviceVideoCapabilitiesKHR", offsetof(struct vkvideo_functions, vkGetPhysicalDeviceVideoCapabilitiesKHR), 1},
	{"vkGetPhysicalDeviceVideoFormatPropertiesKHR", offsetof(struct vkvideo_functions, vkGetPhysicalDeviceVideoFormatPropertiesKHR), 1},
	{"vkGetQueryPoolResults", offsetof(struct vkvideo_functions, vkGetQueryPoolResults), 0},
	{"vkGetVideoSessionMemoryRequirementsKHR", offsetof(struct vkvideo_functions, vkGetVideoSessionMemoryRequirementsKHR), 0},
	{"vkInvalidateMappedMemoryRanges", offsetof(struct vkvideo_functions, vkInvalidateMappedMemoryRanges), 0},
	{"vkMapMemory", offsetof(struct vkvideo_functions, vkMapMemory), 0},
	{"vkQueueSubmit", offsetof(struct vkvideo_functions, vkQueueSubmit), 0},
	{"vkResetCommandBuffer", offsetof(struct vkvideo_functions, vkResetCommandBuffer), 0},
	{"vkResetFences", offsetof(struct vkvideo_functions, vkResetFences), 0},
	{"vkUnmapMemory", offsetof(struct vkvideo_functions, vkUnmapMemory), 0},
	{"vkWaitForFences", offsetof(struct vkvideo_functions, vkWaitForFences), 0},
};

/* The optional Vulkan loader is retained for process lifetime because exported frames may outlive decoder close. */
static void *vkvideo_library;

/* Loader publication is immutable after one attempt and synchronized before any runtime uses it. */
static pthread_once_t vkvideo_loader_once = PTHREAD_ONCE_INIT;

static void vkvideo_loader(void);
static int vkvideo_dispatch(struct vkvideo_runtime *video, int instance);
static int vkvideo_instance(struct vkvideo_runtime *video);
static int vkvideo_resources(struct vkvideo_runtime *video);
static int vkvideo_device(struct vkvideo_runtime *video);
static int vkvideo_family(struct vkvideo_runtime *video);
static int vkvideo_formats(struct vkvideo_runtime *video, VkImageUsageFlags usage);
static int vkvideo_capabilities(struct vkvideo_runtime *video);
static int vkvideo_allocate(struct vkvideo_runtime *video, const VkMemoryRequirements *requirements, int host, VkDeviceMemory *memory, int *coherent);
static int vkvideo_readback(struct vkvideo_runtime *video);
static int vkvideo_picture(struct vkvideo_runtime *video, uint32_t slot);
static int vkvideo_bitstream(struct vkvideo_runtime *video, size_t bytes);
static int vkvideo_session(struct vkvideo_runtime *video);
static int vkvideo_commands(struct vkvideo_runtime *video);
static int vkvideo_status_pool(struct vkvideo_runtime *video);
static int vkvideo_failed(VkResult result, const char *operation);
static int vkvideo_mapped_range(struct vkvideo_runtime *video, VkDeviceMemory memory, int invalidate);
static void vkvideo_barrier(struct vkvideo_runtime *video, VkCommandBuffer command, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags2 source_stage, VkAccessFlags2 source_access, VkPipelineStageFlags2 destination_stage, VkAccessFlags2 destination_access);
static void vkvideo_copy_output(struct vkvideo_runtime *video, const StdVideoH264SequenceParameterSet *sps, struct media_picture *output);

/*
 * Opens a standard H.264 decode context for one progressive sequence.
 */
int
media_vkvideo_runtime_open(
	const StdVideoH264SequenceParameterSet *sps,
	struct vkvideo_runtime **runtime)
{
	struct vkvideo_runtime *video;
	int error;

	/* The loader is optional; native media reports unavailable video without loading FFmpeg. */
	*runtime = NULL;
	pthread_once(&vkvideo_loader_once, vkvideo_loader);
	if (vkvideo_library == NULL)
		return MEDIA_PROBLEM_DEVICE;
	video = calloc(1U, sizeof(*video));
	if (video == NULL)
		return ENOMEM;
	video->level = sps->level_idc;
	video->extent.width = (sps->pic_width_in_mbs_minus1 + 1U) * 16U;
	video->extent.height = (sps->pic_height_in_map_units_minus1 + 1U) * 16U;
	video->max_references = sps->max_num_ref_frames;
	if (video->max_references == 0U)
		video->max_references = 1U;
	video->slots = video->max_references + 1U;

	/* Profile chains are stable for the entire lifetime of this instance's resources. */
	video->h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
	video->h264_profile.stdProfileIdc = sps->profile_idc;
	video->h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
	video->profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	video->profile.pNext = &video->h264_profile;
	video->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	video->profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	video->profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	video->profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	video->profile_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
	video->profile_list.profileCount = 1U;
	video->profile_list.pProfiles = &video->profile;

	/* One resource routine leaves every partially created object with the same owner. */
	error = vkvideo_resources(video);
	if (error != 0) {
		media_vkvideo_runtime_close(video);
		if (error == ENOTSUP)
			return MEDIA_PROBLEM_PROFILE;
		if (error == EBUSY)
			return MEDIA_PROBLEM_BUSY;
		if (error == EIO || error == ENODEV)
			return MEDIA_PROBLEM_DEVICE;
		return error;
	}

	/* Succeeded: the context owns every image, buffer and session required for standard readback. */
	*runtime = video;
	return 0;
}

/*
 * Requests a coding reset after seek without discarding reusable GPU allocations.
 */
void
media_vkvideo_runtime_reset(
	struct vkvideo_runtime *video)
{
	/* Logical references are separately emptied by the H.264 backend before the next submission. */
	video->reset = 1;
}

/*
 * Replace the session parameter object with the parser's currently published SPS and PPS sets.
 */
int
media_vkvideo_runtime_parameters(
	struct vkvideo_runtime *video,
	const struct h264_stream *stream)
{
	StdVideoH264SequenceParameterSet sps[H264_SPS_IDS];
	StdVideoH264PictureParameterSet pps[H264_PPS_IDS];
	VkVideoDecodeH264SessionParametersAddInfoKHR add;
	VkVideoDecodeH264SessionParametersCreateInfoKHR h264;
	VkVideoSessionParametersCreateInfoKHR info;
	VkResult result;
	uint32_t sps_count;
	uint32_t pps_count;
	uint32_t index;
	int error;

	/* The sets the stream has. */
	sps_count = 0U;
	for (index = 0U; index < H264_SPS_IDS; index++) {
		if (!stream->has_sps[index])
			continue;
		sps[sps_count] = stream->sps[index];
		sps_count++;
	}

	/* Collect all currently published PPS entries for the session parameter object. */
	pps_count = 0U;
	for (index = 0U; index < H264_PPS_IDS; index++) {
		if (!stream->has_pps[index])
			continue;
		pps[pps_count] = stream->pps[index];
		pps_count++;
	}

	/* A completed submission no longer uses the old parameter object. */
	if (video->parameters != VK_NULL_HANDLE) {
		video->fn.vkDestroyVideoSessionParametersKHR(video->device, video->parameters, NULL);
		video->parameters = VK_NULL_HANDLE;
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
	info.videoSession = video->session;
	result = video->fn.vkCreateVideoSessionParametersKHR(video->device, &info, NULL, &video->parameters);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateVideoSessionParametersKHR");
		return error;
	}

	/* Succeeded: the parameters. */
	return 0;
}

/*
 * Submit one admitted picture and read both standard NV12 planes after queue retirement.
 */
int
media_vkvideo_runtime_decode(
	struct vkvideo_runtime *video,
	const struct h264_stream *stream,
	const struct h264_picture *picture,
	const struct h264_dpb_plan *plan,
	struct media_picture *output)
{
	uint32_t offsets[H264_MAX_SLICES];
	VkVideoPictureResourceInfoKHR resources[H264_DPB_SLOTS];
	VkVideoReferenceSlotInfoKHR bound[H264_DPB_SLOTS * 2U];
	VkVideoReferenceSlotInfoKHR references[H264_DPB_REFERENCES];
	VkVideoDecodeH264DpbSlotInfoKHR reference_info[H264_DPB_REFERENCES];
	VkVideoDecodeH264DpbSlotInfoKHR setup_info;
	VkVideoDecodeH264PictureInfoKHR h264;
	VkVideoReferenceSlotInfoKHR setup;
	VkVideoBeginCodingInfoKHR begin;
	VkVideoCodingControlInfoKHR control;
	VkVideoDecodeInfoKHR decode;
	VkVideoEndCodingInfoKHR end;
	VkCommandBufferBeginInfo record;
	VkBufferImageCopy copies[2];
	VkBufferMemoryBarrier2 host_barrier;
	VkDependencyInfo host_dependency;
	VkVideoPictureResourceInfoKHR destination;
	VkImageLayout output_layout;
	uint32_t output_slot;
	VkDeviceSize aligned;
	VkDeviceSize alignment;
	int error;
	VkSubmitInfo submit;
	const StdVideoH264SequenceParameterSet *sps;
	VkResult result;
	size_t at;
	uint32_t slice;
	uint32_t slot;
	uint32_t index;
	uint32_t count;
	VkPipelineStageFlags wait_stage;

	/* Size and align the complete access unit before writing or submitting any part of it. */
	at = 0U;
	for (slice = 0U; slice < picture->slice_count; slice++) {
		if (picture->slice_sizes[slice] > VKVIDEO_SOURCE_MAX - at - 3U)
			return EINVAL;
		at += 3U + picture->slice_sizes[slice];
	}

	/* Pad the bitstream range to the queried device alignment. */
	alignment = video->capabilities.minBitstreamBufferSizeAlignment;
	aligned = ((at + alignment - 1U) / alignment) * alignment;
	if (aligned > VKVIDEO_SOURCE_MAX)
		return EINVAL;
	if (aligned > video->buffer_size) {
		video->fn.vkUnmapMemory(video->device, video->buffer_memory);
		video->fn.vkDestroyBuffer(video->device, video->buffer, NULL);
		video->fn.vkFreeMemory(video->device, video->buffer_memory, NULL);
		video->buffer = VK_NULL_HANDLE;
		video->buffer_memory = VK_NULL_HANDLE;
		video->buffer_map = NULL;
		error = vkvideo_bitstream(video, (size_t)aligned);
		if (error != 0)
			return error;
	}

	/* Pack complete Annex-B slice NALs into the mapped source buffer. */
	at = 0U;
	for (slice = 0U; slice < picture->slice_count; slice++) {
		offsets[slice] = (uint32_t)at;
		video->buffer_map[at] = 0U;
		video->buffer_map[at + 1U] = 0U;
		video->buffer_map[at + 2U] = 1U;
		memcpy(video->buffer_map + at + 3U, stream->data + picture->slice_offsets[slice], picture->slice_sizes[slice]);
		at += 3U + picture->slice_sizes[slice];
	}

	/* Zero only the required alignment padding after the final slice. */
	memset(video->buffer_map + at, 0, (size_t)aligned - at);
	if (!video->buffer_coherent) {
		error = vkvideo_mapped_range(video, video->buffer_memory, 0);
		if (error != 0)
			return error;
	}

	/* Every slot's picture resource: its image's coded picture of the sequence. */
	sps = &stream->sps[picture->info.seq_parameter_set_id];
	memset(resources, 0, sizeof(resources));
	for (slot = 0U; slot < video->slots; slot++) {
		resources[slot].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
		resources[slot].codedExtent.width = (sps->pic_width_in_mbs_minus1 + 1U) * 16U;
		resources[slot].codedExtent.height = (sps->pic_height_in_map_units_minus1 + 1U) * 16U;
		resources[slot].imageViewBinding = video->view[slot];
	}

	/* The command buffer from its start. */
	memset(&record, 0, sizeof(record));
	record.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	record.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = video->fn.vkBeginCommandBuffer(video->command, &record);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkBeginCommandBuffer");
		return error;
	}

	/* The status query starts unavailable, outside the scope. */
	if (video->status_pool != VK_NULL_HANDLE)
		video->fn.vkCmdResetQueryPool(video->command, video->status_pool, 0U, 1U);

	/* Initial allocation transitions occur once; coding reset after seek preserves established layouts. */
	if (!video->initialized) {
		for (slot = 0U; slot < video->slots; slot++)
			vkvideo_barrier(video, video->command, video->image[slot], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_PIPELINE_STAGE_2_NONE, 0U, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
		if (video->distinct)
			vkvideo_barrier(video, video->command, video->image[video->slots], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR, VK_PIPELINE_STAGE_2_NONE, 0U, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
	}

	/* Select either the coincident current DPB image or the distinct output image. */
	output_slot = (uint32_t)plan->setup;
	output_layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
	destination = resources[plan->setup];
	if (video->distinct) {
		output_slot = video->slots;
		output_layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
		destination.imageViewBinding = video->view[output_slot];
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

	/* Deactivate stale device slots before associating the new current picture. */
	for (index = 0U; index < plan->deactivate_count; index++) {
		bound[count].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		bound[count].slotIndex = plan->deactivate[index];
		bound[count].pPictureResource = NULL;
		count++;
	}

	/* Bind the current picture resource without an old reference-slot association. */
	bound[count].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	bound[count].slotIndex = -1;
	bound[count].pPictureResource = &resources[plan->setup];
	count++;

	/* The scope, and the reset of the first decode. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
	begin.videoSession = video->session;
	begin.videoSessionParameters = video->parameters;
	begin.referenceSlotCount = count;
	begin.pReferenceSlots = bound;
	video->fn.vkCmdBeginVideoCodingKHR(video->command, &begin);
	if (plan->reset || video->reset) {
		memset(&control, 0, sizeof(control));
		control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
		control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
		video->fn.vkCmdControlVideoCodingKHR(video->command, &control);
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
	decode.srcBuffer = video->buffer;
	decode.srcBufferOffset = 0U;
	decode.srcBufferRange = aligned;
	decode.dstPictureResource = destination;
	decode.pSetupReferenceSlot = &setup;
	decode.referenceSlotCount = plan->reference_count;
	decode.pReferenceSlots = references;
	if (video->status_pool != VK_NULL_HANDLE)
		video->fn.vkCmdBeginQuery(video->command, video->status_pool, 0U, 0U);
	video->fn.vkCmdDecodeVideoKHR(video->command, &decode);
	if (video->status_pool != VK_NULL_HANDLE)
		video->fn.vkCmdEndQuery(video->command, video->status_pool, 0U);

	/* The scope's end. */
	memset(&end, 0, sizeof(end));
	end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
	video->fn.vkCmdEndVideoCodingKHR(video->command, &end);

	/* Read both NV12 planes in their standard sample coordinates, never an optimal image's host layout. */
	vkvideo_barrier(video, video->command, video->image[output_slot], output_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_NONE, 0U);
	result = video->fn.vkEndCommandBuffer(video->command);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkEndCommandBuffer video");
		return error;
	}

	/* Begin transfer recording after the video coding scope was closed. */
	result = video->fn.vkBeginCommandBuffer(video->transfer_command, &record);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkBeginCommandBuffer transfer");
		return error;
	}

	/* Describe both NV12 planes using standard per-plane sample coordinates. */
	memset(copies, 0, sizeof(copies));
	copies[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	copies[0].imageSubresource.layerCount = 1U;
	copies[0].imageExtent.width = video->extent.width;
	copies[0].imageExtent.height = video->extent.height;
	copies[0].imageExtent.depth = 1U;
	copies[1].bufferOffset = (VkDeviceSize)video->extent.width * video->extent.height;
	copies[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	copies[1].imageSubresource.layerCount = 1U;
	copies[1].imageExtent.width = video->extent.width / 2U;
	copies[1].imageExtent.height = video->extent.height / 2U;
	copies[1].imageExtent.depth = 1U;
	video->fn.vkCmdCopyImageToBuffer(video->transfer_command, video->image[output_slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, video->readback, 2U, copies);
	vkvideo_barrier(video, video->transfer_command, video->image[output_slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output_layout, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_NONE, 0U);
	memset(&host_barrier, 0, sizeof(host_barrier));
	host_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
	host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
	host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
	host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host_barrier.buffer = video->readback;
	host_barrier.size = VK_WHOLE_SIZE;
	memset(&host_dependency, 0, sizeof(host_dependency));
	host_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	host_dependency.bufferMemoryBarrierCount = 1U;
	host_dependency.pBufferMemoryBarriers = &host_barrier;
	video->fn.vkCmdPipelineBarrier2KHR(video->transfer_command, &host_dependency);
	result = video->fn.vkEndCommandBuffer(video->transfer_command);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkEndCommandBuffer transfer");
		return error;
	}

	/* Binary semaphores carry memory dependencies between dedicated video and transfer families. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &video->command;
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &video->decoded;
	wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	if (video->returned_pending) {
		submit.waitSemaphoreCount = 1U;
		submit.pWaitSemaphores = &video->returned;
		submit.pWaitDstStageMask = &wait_stage;
	}

	/* Submit decoding after attaching the preceding transfer's reverse dependency. */
	result = video->fn.vkQueueSubmit(video->queue, 1U, &submit, VK_NULL_HANDLE);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkQueueSubmit video");
		return error;
	}

	/* Retain all submission resources as soon as decoding enters the queue. */
	video->inflight = 1;
	video->returned_pending = 0;
	wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &video->decoded;
	submit.pWaitDstStageMask = &wait_stage;
	submit.pCommandBuffers = &video->transfer_command;
	submit.pSignalSemaphores = &video->returned;
	result = video->fn.vkQueueSubmit(video->transfer_queue, 1U, &submit, video->fence);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkQueueSubmit transfer");
		return error;
	}

	/* Record the reverse semaphore dependency after transfer submission succeeded. */
	video->returned_pending = 1;
	result = video->fn.vkWaitForFences(video->device, 1U, &video->fence, VK_TRUE, VKVIDEO_WAIT_NS);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkWaitForFences readback");
		return error;
	}

	/* Mark both queue submissions retired only after the transfer fence completed. */
	video->inflight = 0;
	video->initialized = 1;
	video->reset = 0;

	/* The fence and the buffer for the next picture. */
	result = video->fn.vkResetFences(video->device, 1U, &video->fence);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkResetFences");
		return error;
	}

	/* Reset video recording after resetting the retired completion fence. */
	result = video->fn.vkResetCommandBuffer(video->command, 0U);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkResetCommandBuffer");
		return error;
	}

	/* Reset transfer recording only after its fence has retired both submissions. */
	result = video->fn.vkResetCommandBuffer(video->transfer_command, 0U);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkResetCommandBuffer transfer");
		return error;
	}

	/* Whether the decode completed: its query's status, when there is one. */
	video->status = VK_QUERY_RESULT_STATUS_COMPLETE_KHR;
	if (video->status_pool != VK_NULL_HANDLE) {
		result = video->fn.vkGetQueryPoolResults(video->device, video->status_pool, 0U, 1U, sizeof(video->status), &video->status, sizeof(video->status), VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);
		if (result != VK_SUCCESS) {
			error = vkvideo_failed(result, "vkGetQueryPoolResults");
			return error;
		}
	}

	/* A status failure is an actual decode error; no unverified pixels are exposed to the caller. */
	if (video->status != VK_QUERY_RESULT_STATUS_COMPLETE_KHR)
		return EIO;
	if (!video->readback_coherent) {
		error = vkvideo_mapped_range(video, video->readback_memory, 1);
		if (error != 0)
			return error;
	}

	/* Copy cropped linear NV12 bytes only after retirement and host visibility. */
	vkvideo_copy_output(video, sps, output);

	/* Succeeded: the cropped CPU picture is independent of decoder and GPU image lifetime. */
	return 0;
}

/*
 * Retire GPU work and release this runtime, preserving resources when retirement cannot be proven.
 */
void
media_vkvideo_runtime_close(
	struct vkvideo_runtime *video)
{
	uint32_t index;
	VkResult result;

	/* A null owner is valid during partial admission unwind. */
	if (video == NULL)
		return;

	/* The device's objects, then the device. */
	if (video->device != VK_NULL_HANDLE) {
		result = VK_SUCCESS;
		if (video->fn.vkDeviceWaitIdle != NULL)
			result = video->fn.vkDeviceWaitIdle(video->device);
		if (
			video->inflight &&
			result != VK_SUCCESS &&
			result != VK_ERROR_DEVICE_LOST) {
			media_log("Vulkan Video retains an unretired context after failed idle (%d)", (int)result);
			return;
		}

		/* Destroy optional command and query resources after device retirement. */
		if (video->status_pool != VK_NULL_HANDLE)
			video->fn.vkDestroyQueryPool(video->device, video->status_pool, NULL);
		if (video->fence != VK_NULL_HANDLE)
			video->fn.vkDestroyFence(video->device, video->fence, NULL);
		if (video->decoded != VK_NULL_HANDLE)
			video->fn.vkDestroySemaphore(video->device, video->decoded, NULL);
		if (video->returned != VK_NULL_HANDLE)
			video->fn.vkDestroySemaphore(video->device, video->returned, NULL);
		if (video->transfer_pool != VK_NULL_HANDLE)
			video->fn.vkDestroyCommandPool(video->device, video->transfer_pool, NULL);
		if (video->pool != VK_NULL_HANDLE)
			video->fn.vkDestroyCommandPool(video->device, video->pool, NULL);
		if (video->parameters != VK_NULL_HANDLE)
			video->fn.vkDestroyVideoSessionParametersKHR(video->device, video->parameters, NULL);
		if (video->session != VK_NULL_HANDLE)
			video->fn.vkDestroyVideoSessionKHR(video->device, video->session, NULL);
		for (index = 0U; index < video->session_memory_count; index++)
			video->fn.vkFreeMemory(video->device, video->session_memory[index], NULL);
		if (video->readback_map != NULL)
			video->fn.vkUnmapMemory(video->device, video->readback_memory);
		if (video->readback != VK_NULL_HANDLE)
			video->fn.vkDestroyBuffer(video->device, video->readback, NULL);
		if (video->readback_memory != VK_NULL_HANDLE)
			video->fn.vkFreeMemory(video->device, video->readback_memory, NULL);
		if (video->buffer_map != NULL)
			video->fn.vkUnmapMemory(video->device, video->buffer_memory);
		if (video->buffer != VK_NULL_HANDLE)
			video->fn.vkDestroyBuffer(video->device, video->buffer, NULL);
		if (video->buffer_memory != VK_NULL_HANDLE)
			video->fn.vkFreeMemory(video->device, video->buffer_memory, NULL);
		for (index = 0U; index < H264_DPB_SLOTS + 1U; index++) {
			if (video->view[index] != VK_NULL_HANDLE)
				video->fn.vkDestroyImageView(video->device, video->view[index], NULL);
			if (video->image[index] != VK_NULL_HANDLE)
				video->fn.vkDestroyImage(video->device, video->image[index], NULL);
			if (video->image_memory[index] != VK_NULL_HANDLE)
				video->fn.vkFreeMemory(video->device, video->image_memory[index], NULL);
		}

		/* Destroy a partially admitted device only when dispatch supplied its destructor. */
		if (video->fn.vkDestroyDevice != NULL)
			video->fn.vkDestroyDevice(video->device, NULL);
	}

	/* The instance. */
	if (video->instance != VK_NULL_HANDLE && video->fn.vkDestroyInstance != NULL)
		video->fn.vkDestroyInstance(video->instance, NULL);
	free(video);
}

/* Create one session and its standard decode/readback resources in dependency order. */
static int
vkvideo_resources(
	struct vkvideo_runtime *video)
{
	unsigned slot;
	int error;

	/* All resources are created in dependency order and unwind through one retirement-aware owner. */
	error = vkvideo_instance(video);
	if (error != 0)
		return error;
	error = vkvideo_device(video);
	if (error != 0)
		return error;
	error = vkvideo_session(video);
	if (error != 0)
		return error;
	for (slot = 0U; slot < video->slots; slot++) {
		error = vkvideo_picture(video, slot);
		if (error != 0)
			return error;
	}

	/* Allocate a separate destination only for a distinct-output device. */
	if (video->distinct) {
		error = vkvideo_picture(video, video->slots);
		if (error != 0)
			return error;
	}

	/* Create the host source buffer before the staging readback buffer. */
	error = vkvideo_bitstream(video, 1024U * 1024U);
	if (error != 0)
		return error;
	error = vkvideo_readback(video);
	if (error != 0)
		return error;
	error = vkvideo_commands(video);
	if (error != 0)
		return error;

	/* Success transfers no ownership; the caller already owns the complete resource set. */
	return 0;
}

/* Allocate one profile-compatible optimal NV12 image and its decode resource view. */
static int
vkvideo_picture(
	struct vkvideo_runtime *video,
	uint32_t slot)
{
	VkImageCreateInfo image;
	VkImageViewCreateInfo view;
	uint32_t families[2];
	VkMemoryRequirements requirements;
	VkResult result;
	int error;

	/* The image: NV12, optimal, the decoder's output and reference picture, of the profile. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.pNext = &video->profile_list;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	image.extent.width = video->image_extent.width;
	image.extent.height = video->image_extent.height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = video->reference_usage;
	if (slot == video->slots)
		image.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	families[0] = video->family;
	families[1] = video->transfer_family;
	if (video->family != video->transfer_family) {
		image.sharingMode = VK_SHARING_MODE_CONCURRENT;
		image.queueFamilyIndexCount = 2U;
		image.pQueueFamilyIndices = families;
	}

	/* Create every image from an undefined initial layout. */
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = video->fn.vkCreateImage(video->device, &image, NULL, &video->image[slot]);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateImage");
		return error;
	}

	/* Its memory, bound and mapped. */
	video->fn.vkGetImageMemoryRequirements(video->device, video->image[slot], &requirements);
	error = vkvideo_allocate(video, &requirements, 0, &video->image_memory[slot], NULL);
	if (error != 0)
		return error;
	result = video->fn.vkBindImageMemory(video->device, video->image[slot], video->image_memory[slot], 0U);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkBindImageMemory");
		return error;
	}

	/* Its view. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = video->image[slot];
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	result = video->fn.vkCreateImageView(video->device, &view, NULL, &video->view[slot]);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateImageView");
		return error;
	}

	/* Succeeded: the slot's picture. */
	return 0;
}

/* Grow the host-visible compressed source buffer without changing in-flight allocations. */
static int
vkvideo_bitstream(
	struct vkvideo_runtime *video,
	size_t bytes)
{
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkResult result;
	void *map;
	int error;

	/* The buffer: whole pages, a decode's source, of the profile. */
	video->buffer_size = ((VkDeviceSize)bytes + 4095U) & ~(VkDeviceSize)4095U;
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.pNext = &video->profile_list;
	buffer.size = video->buffer_size;
	buffer.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = video->fn.vkCreateBuffer(video->device, &buffer, NULL, &video->buffer);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateBuffer");
		return error;
	}

	/* Its memory, bound and mapped. */
	video->fn.vkGetBufferMemoryRequirements(video->device, video->buffer, &requirements);
	error = vkvideo_allocate(video, &requirements, 1, &video->buffer_memory, &video->buffer_coherent);
	if (error != 0)
		return error;
	result = video->fn.vkBindBufferMemory(video->device, video->buffer, video->buffer_memory, 0U);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkBindBufferMemory");
		return error;
	}

	/* Map the newly bound compressed source allocation for host writes. */
	result = video->fn.vkMapMemory(video->device, video->buffer_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkMapMemory");
		return error;
	}

	/* Publish the source mapping only after mapping succeeded. */
	video->buffer_map = map;

	/* Succeeded: the buffer. */
	return 0;
}

/* Create and bind a standard H.264 session for the admitted sequence capacity. */
static int
vkvideo_session(
	struct vkvideo_runtime *video)
{
	static const VkExtensionProperties header = {
	    VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME,
	    VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION};
	VkVideoSessionCreateInfoKHR info;
	VkVideoSessionMemoryRequirementsKHR requirements[VKVIDEO_BINDINGS];
	VkBindVideoSessionMemoryInfoKHR binds[VKVIDEO_BINDINGS];
	VkResult result;
	uint32_t count;
	uint32_t index;
	int error;

	/* The session. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
	info.queueFamilyIndex = video->family;
	info.pVideoProfile = &video->profile;
	info.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	info.maxCodedExtent = video->extent;
	info.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	info.maxDpbSlots = video->slots;
	info.maxActiveReferencePictures = video->max_references;
	info.pStdHeaderVersion = &header;
	result = video->fn.vkCreateVideoSessionKHR(video->device, &info, NULL, &video->session);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateVideoSessionKHR");
		return error;
	}

	/* What memory it asks for. */
	memset(requirements, 0, sizeof(requirements));
	for (index = 0U; index < VKVIDEO_BINDINGS; index++)
		requirements[index].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
	count = VKVIDEO_BINDINGS;
	result = video->fn.vkGetVideoSessionMemoryRequirementsKHR(video->device, video->session, &count, requirements);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkGetVideoSessionMemoryRequirementsKHR");
		return error;
	}

	/* One allocation for each binding, then all bound at once. */
	memset(binds, 0, sizeof(binds));
	for (index = 0U; index < count; index++) {
		error = vkvideo_allocate(video, &requirements[index].memoryRequirements, 0, &video->session_memory[index], NULL);
		if (error != 0)
			return error;
		video->session_memory_count = index + 1U;
		binds[index].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
		binds[index].memoryBindIndex = requirements[index].memoryBindIndex;
		binds[index].memory = video->session_memory[index];
		binds[index].memoryOffset = 0U;
		binds[index].memorySize = requirements[index].memoryRequirements.size;
	}

	/* Bind all allocated session memory ranges in one standard call. */
	result = video->fn.vkBindVideoSessionMemoryKHR(video->device, video->session, count, binds);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkBindVideoSessionMemoryKHR");
		return error;
	}

	/* Succeeded: the session is bound. */
	return 0;
}

/* Create family-specific command pools, completion fence and cross-queue semaphores. */
static int
vkvideo_commands(
	struct vkvideo_runtime *video)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkResult result;
	int error;

	/* The pool, whose buffer is reset before each picture. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = video->family;
	result = video->fn.vkCreateCommandPool(video->device, &pool, NULL, &video->pool);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateCommandPool");
		return error;
	}

	/* The buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = video->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	result = video->fn.vkAllocateCommandBuffers(video->device, &command, &video->command);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkAllocateCommandBuffers");
		return error;
	}

	/* The fence. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = video->fn.vkCreateFence(video->device, &fence, NULL, &video->fence);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateFence");
		return error;
	}

	/* A separate transfer command permits drivers with a dedicated video-only queue. */
	pool.queueFamilyIndex = video->transfer_family;
	result = video->fn.vkCreateCommandPool(video->device, &pool, NULL, &video->transfer_pool);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateCommandPool transfer");
		return error;
	}

	/* Allocate the transfer command from the transfer family's pool. */
	command.commandPool = video->transfer_pool;
	result = video->fn.vkAllocateCommandBuffers(video->device, &command, &video->transfer_command);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkAllocateCommandBuffers transfer");
		return error;
	}

	/* Create the decode-to-transfer semaphore before its reverse counterpart. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	result = video->fn.vkCreateSemaphore(video->device, &semaphore, NULL, &video->decoded);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateSemaphore decoded");
		return error;
	}

	/* Create the transfer-to-next-decode semaphore independently. */
	result = video->fn.vkCreateSemaphore(video->device, &semaphore, NULL, &video->returned);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateSemaphore returned");
		return error;
	}

	/* The result status query, where the family has it. */
	error = vkvideo_status_pool(video);
	if (error != 0)
		return error;

	/* Succeeded: the commands. */
	return 0;
}

/* Create a decode-status query only when the selected queue family exposes it. */
static int
vkvideo_status_pool(
	struct vkvideo_runtime *video)
{
	VkQueryPoolCreateInfo info;
	VkResult result;
	int error;

	/* No status: every decode counts as complete. */
	video->status = VK_QUERY_RESULT_STATUS_COMPLETE_KHR;
	if (!video->status_supported)
		return 0;

	/* One query of the decode's profile. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	info.pNext = &video->profile;
	info.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
	info.queryCount = 1U;
	result = video->fn.vkCreateQueryPool(video->device, &info, NULL, &video->status_pool);
	if (result != VK_SUCCESS) {
		video->status_pool = VK_NULL_HANDLE;
		error = vkvideo_failed(result, "vkCreateQueryPool");
		return error;
	}

	/* Succeeded: a supported status query cannot silently disappear after allocation failure. */
	return 0;
}

/* Publish the optional standard loader once without using a codec library as a media backend. */
static void
vkvideo_loader(
	void)
{
	/* Distribution SONAMEs are tried in their normal order and retained for process lifetime. */
	vkvideo_library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (vkvideo_library == NULL)
		vkvideo_library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
}

/* Load only the entry points whose instance or device dispatch is already available. */
static int
vkvideo_dispatch(
	struct vkvideo_runtime *video,
	int instance)
{
	PFN_vkVoidFunction address;
	void *symbol;
	unsigned index;
	int missing;

	/* The loader's function-pointer representation is transferred explicitly rather than cast through ISO C. */
	if (video->fn.vkGetInstanceProcAddr == NULL) {
		symbol = dlsym(vkvideo_library, "vkGetInstanceProcAddr");
		if (symbol == NULL)
			return ENODEV;
		if (sizeof(symbol) != sizeof(video->fn.vkGetInstanceProcAddr))
			return ENODEV;
		memcpy(&video->fn.vkGetInstanceProcAddr, &symbol, sizeof(symbol));
	}

	/* Load every dispatch symbol even when an earlier entry was unavailable. */
	missing = 0;
	for (index = 0U; index < sizeof(vkvideo_symbols) / sizeof(vkvideo_symbols[0]); index++) {
		if (vkvideo_symbols[index].instance != instance)
			continue;
		if (instance)
			address = video->fn.vkGetInstanceProcAddr(video->instance, vkvideo_symbols[index].name);
		else
			address = video->fn.vkGetDeviceProcAddr(video->device, vkvideo_symbols[index].name);
		if (address == NULL)
			missing = 1;
		memcpy((uint8_t *)&video->fn + vkvideo_symbols[index].offset, &address, sizeof(address));
	}

	/* Reject incomplete dispatch after recording all available cleanup functions. */
	if (missing)
		return ENODEV;

	/* Succeeded: every operation required by this dispatch phase is present. */
	return 0;
}

/* Create a version-one instance with the properties-two extension used by standard video queries. */
static int
vkvideo_instance(
	struct vkvideo_runtime *video)
{
	/* The required instance extension is immutable and defined by the Vulkan API, not the GPU vendor. */
	static const char *const extensions[] = {"VK_KHR_get_physical_device_properties2"};
	VkApplicationInfo application;
	VkInstanceCreateInfo info;
	PFN_vkVoidFunction address;
	VkResult result;
	int error;
	void *symbol;

	/* Obtain the global creation operation before an instance exists. */
	if (video->fn.vkGetInstanceProcAddr == NULL) {
		symbol = dlsym(vkvideo_library, "vkGetInstanceProcAddr");
		if (symbol == NULL)
			return ENODEV;
		memcpy(&video->fn.vkGetInstanceProcAddr, &symbol, sizeof(symbol));
	}

	/* Resolve instance creation from the process-wide Vulkan loader. */
	address = video->fn.vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
	if (address == NULL)
		return ENODEV;
	memcpy(&video->fn.vkCreateInstance, &address, sizeof(address));
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "libmedia";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &application;
	info.enabledExtensionCount = 1U;
	info.ppEnabledExtensionNames = extensions;
	result = video->fn.vkCreateInstance(&info, NULL, &video->instance);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateInstance");
		return error;
	}

	/* Resolve instance dispatch before probing any physical device. */
	error = vkvideo_dispatch(video, 1);
	if (error != 0)
		return error;

	/* Succeeded: the instance can enumerate standard video-capable physical devices. */
	return 0;
}

/* Choose a queue supporting both decode and the transfer operations required for CPU presentation. */
static int
vkvideo_family(
	struct vkvideo_runtime *video)
{
	VkQueueFamilyProperties2 families[VKVIDEO_FAMILIES];
	VkQueueFamilyVideoPropertiesKHR properties[VKVIDEO_FAMILIES];
	VkQueueFamilyQueryResultStatusPropertiesKHR status[VKVIDEO_FAMILIES];
	uint32_t count;
	unsigned index;
	int transfer;
	int decoder;

	/* Query the count first so a truncated fixed array can never be indexed past its storage. */
	count = 0U;
	video->fn.vkGetPhysicalDeviceQueueFamilyProperties2KHR(video->physical, &count, NULL);
	if (count == 0U || count > VKVIDEO_FAMILIES)
		return ENODEV;
	memset(families, 0, sizeof(families));
	memset(properties, 0, sizeof(properties));
	memset(status, 0, sizeof(status));
	for (index = 0U; index < count; index++) {
		families[index].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
		families[index].pNext = &properties[index];
		properties[index].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
		properties[index].pNext = &status[index];
		status[index].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR;
	}

	/* Fill the initialized per-family capability chains. */
	video->fn.vkGetPhysicalDeviceQueueFamilyProperties2KHR(video->physical, &count, families);
	transfer = -1;
	decoder = -1;
	for (index = 0U; index < count; index++) {
		if ((families[index].queueFamilyProperties.queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0U)
			transfer = (int)index;
		if ((families[index].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) == 0U)
			continue;
		if ((properties[index].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) == 0U)
			continue;
		decoder = (int)index;
		video->status_supported = status[index].queryResultStatusSupport;
	}

	/* Require both an H.264 decode family and a transfer-capable family. */
	if (decoder >= 0 && transfer >= 0) {
		video->family = (uint32_t)decoder;
		video->transfer_family = (uint32_t)transfer;
		return 0;
	}

	/* No family exposes the full standard decode-and-readback operation set. */
	return ENODEV;
}

/* Admit an optimal NV12 image only after querying its complete combined usage. */
static int
vkvideo_formats(
	struct vkvideo_runtime *video,
	VkImageUsageFlags usage)
{
	VkPhysicalDeviceVideoFormatInfoKHR info;
	VkVideoFormatPropertiesKHR formats[32];
	VkResult result;
	uint32_t count;
	unsigned index;
	int error;

	/* Combined usage is essential: independent capability flags do not prove one image supports all operations. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
	info.pNext = &video->profile_list;
	info.imageUsage = usage;
	count = 0U;
	result = video->fn.vkGetPhysicalDeviceVideoFormatPropertiesKHR(video->physical, &info, &count, NULL);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkGetPhysicalDeviceVideoFormatPropertiesKHR count");
		return error;
	}

	/* A successful query with no bounded NV12 candidates is an unsupported allocation model. */
	if (count == 0U || count > 32U)
		return ENOTSUP;
	memset(formats, 0, sizeof(formats));
	for (index = 0U; index < count; index++)
		formats[index].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
	result = video->fn.vkGetPhysicalDeviceVideoFormatPropertiesKHR(video->physical, &info, &count, formats);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkGetPhysicalDeviceVideoFormatPropertiesKHR");
		return error;
	}

	/* Select a format which supports every requested usage and the optimal two-plane image model. */
	for (index = 0U; index < count; index++) {
		if (formats[index].format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
			continue;
		if (formats[index].imageType != VK_IMAGE_TYPE_2D || formats[index].imageTiling != VK_IMAGE_TILING_OPTIMAL)
			continue;
		if ((formats[index].imageUsageFlags & usage) != usage)
			continue;
		return 0;
	}

	/* No standard NV12 allocation supports this complete usage combination. */
	return ENOTSUP;
}

/* Query sequence limits and support both coincident and distinct output/reference image models. */
static int
vkvideo_capabilities(
	struct vkvideo_runtime *video)
{
	VkResult result;
	VkExtent2D granularity;
	int error;

	/* Each candidate begins with an independent image-model choice. */
	video->distinct = 0;

	/* The profile query provides bitstream alignment, extent, reference capacity and image-model flags. */
	video->h264_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
	video->decode_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
	video->decode_caps.pNext = &video->h264_caps;
	video->capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
	video->capabilities.pNext = &video->decode_caps;
	result = video->fn.vkGetPhysicalDeviceVideoCapabilitiesKHR(video->physical, &video->profile, &video->capabilities);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
		return error;
	}

	/* The sequence must fit both coded-extent limits and the advertised profile level. */
	if (video->extent.width > video->capabilities.maxCodedExtent.width || video->extent.height > video->capabilities.maxCodedExtent.height)
		return ENOTSUP;
	if (video->extent.width < video->capabilities.minCodedExtent.width || video->extent.height < video->capabilities.minCodedExtent.height)
		return ENOTSUP;
	if (video->level > video->h264_caps.maxLevelIdc)
		return ENOTSUP;
	if (video->slots > video->capabilities.maxDpbSlots || video->max_references > video->capabilities.maxActiveReferencePictures)
		return ENOTSUP;
	if (video->capabilities.minBitstreamBufferSizeAlignment == 0U || video->capabilities.minBitstreamBufferSizeAlignment > VKVIDEO_SOURCE_MAX)
		return ENODEV;
	video->reference_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
	error = ENOTSUP;
	if ((video->decode_caps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) != 0U) {
		video->reference_usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		error = vkvideo_formats(video, video->reference_usage);
	}

	/* Try distinct output only when coincident allocation was unavailable. */
	if (error != 0) {
		if (error != ENOTSUP)
			return error;
		if ((video->decode_caps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR) == 0U)
			return ENOTSUP;
		video->distinct = 1;
		video->reference_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
		error = vkvideo_formats(video, video->reference_usage);
		if (error != 0)
			return error;
		error = vkvideo_formats(video, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		if (error != 0)
			return error;
	}

	/* Image allocation extent follows picture-access granularity, while coded extent remains the actual sequence size. */
	granularity = video->capabilities.pictureAccessGranularity;
	if (granularity.width == 0U || granularity.height == 0U)
		return ENODEV;
	video->image_extent.width = ((video->extent.width + granularity.width - 1U) / granularity.width) * granularity.width;
	video->image_extent.height = ((video->extent.height + granularity.height - 1U) / granularity.height) * granularity.height;
	if ((uint64_t)video->image_extent.width * video->image_extent.height * (video->slots + video->distinct) * 3U / 2U > 256U * 1024U * 1024U)
		return ENOTSUP;

	/* Succeeded: all standard capability and format requirements are admitted. */
	return 0;
}

/* Enumerate every physical device instead of assuming the first enumerated GPU can decode video. */
static int
vkvideo_device(
	struct vkvideo_runtime *video)
{
	/* Required standard extensions are process-immutable names supplied to a selected physical device. */
	static const char *const extensions[] = {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME};
	VkPhysicalDevice devices[32];
	VkExtensionProperties *available;
	VkDeviceQueueCreateInfo queue[2];
	VkDeviceCreateInfo info;
	VkPhysicalDeviceFeatures2 features;
	VkPhysicalDeviceSynchronization2Features synchronization;
	VkResult result;
	uint32_t count;
	uint32_t extension_count;
	unsigned index;
	unsigned extension;
	unsigned candidate;
	unsigned matched;
	int comparison;
	int error;
	float priority;

	/* Bound enumeration before filling storage with driver-reported device handles. */
	count = 0U;
	result = video->fn.vkEnumeratePhysicalDevices(video->instance, &count, NULL);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkEnumeratePhysicalDevices count");
		return error;
	}

	/* Empty or excessive enumeration cannot supply this runtime's bounded device selection. */
	if (count == 0U || count > 32U)
		return ENODEV;
	result = video->fn.vkEnumeratePhysicalDevices(video->instance, &count, devices);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkEnumeratePhysicalDevices");
		return error;
	}

	/* Probe every enumerated physical device independently. */
	for (index = 0U; index < count; index++) {
		video->physical = devices[index];
		error = vkvideo_family(video);
		if (error != 0)
			continue;
		extension_count = 0U;
		result = video->fn.vkEnumerateDeviceExtensionProperties(video->physical, NULL, &extension_count, NULL);
		if (result != VK_SUCCESS) {
			error = vkvideo_failed(result, "vkEnumerateDeviceExtensionProperties count");
			return error;
		}

		/* Reject excessive extension enumeration before allocating its temporary list. */
		if (extension_count > 1024U)
			continue;
		available = calloc(extension_count + 1U, sizeof(*available));
		if (available == NULL)
			return ENOMEM;
		result = video->fn.vkEnumerateDeviceExtensionProperties(video->physical, NULL, &extension_count, available);
		if (result != VK_SUCCESS) {
			free(available);
			error = vkvideo_failed(result, "vkEnumerateDeviceExtensionProperties");
			return error;
		}

		/* Match every required extension before creating a device. */
		matched = 0U;
		if (result == VK_SUCCESS) {
			for (extension = 0U; extension < 4U; extension++) {
				for (candidate = 0U; candidate < extension_count; candidate++) {
					comparison = strcmp(available[candidate].extensionName, extensions[extension]);
					if (comparison == 0) {
						matched++;
						break;
					}
				}
			}
		}

		/* Release temporary extension enumeration before deciding whether this device is admitted. */
		free(available);
		if (matched != 4U)
			continue;
		error = vkvideo_capabilities(video);
		if (error == ENOTSUP || error == ENODEV)
			continue;
		if (error != 0)
			return error;
		memset(&features, 0, sizeof(features));
		memset(&synchronization, 0, sizeof(synchronization));
		features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features.pNext = &synchronization;
		synchronization.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
		video->fn.vkGetPhysicalDeviceFeatures2KHR(video->physical, &features);
		if (!synchronization.synchronization2)
			continue;
		priority = 1.0f;
		memset(queue, 0, sizeof(queue));
		queue[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queue[0].queueFamilyIndex = video->family;
		queue[0].queueCount = 1U;
		queue[0].pQueuePriorities = &priority;
		queue[1] = queue[0];
		queue[1].queueFamilyIndex = video->transfer_family;
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		info.pNext = &synchronization;
		info.queueCreateInfoCount = 1U;
		if (video->transfer_family != video->family)
			info.queueCreateInfoCount = 2U;
		info.pQueueCreateInfos = queue;
		info.enabledExtensionCount = 4U;
		info.ppEnabledExtensionNames = extensions;
		result = video->fn.vkCreateDevice(video->physical, &info, NULL, &video->device);
		if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
			return ENOMEM;
		if (result != VK_SUCCESS)
			continue;
		error = vkvideo_dispatch(video, 0);
		if (error != 0)
			return error;
		video->fn.vkGetPhysicalDeviceMemoryProperties(video->physical, &video->memory);
		video->fn.vkGetDeviceQueue(video->device, video->family, 0U, &video->queue);
		video->fn.vkGetDeviceQueue(video->device, video->transfer_family, 0U, &video->transfer_queue);
		return 0;
	}

	/* No device exposes the queried profile, extensions and transfer-capable decode queue. */
	return ENODEV;
}

/* Allocate host-visible or device-local memory according to its actual usage and coherent properties. */
static int
vkvideo_allocate(
	struct vkvideo_runtime *video,
	const VkMemoryRequirements *requirements,
	int host,
	VkDeviceMemory *memory,
	int *coherent)
{
	VkMemoryAllocateInfo info;
	VkResult result;
	unsigned index;
	int chosen;
	unsigned score;
	unsigned best;
	VkMemoryPropertyFlags flags;
	int error;

	/* Coherent host allocations avoid cache calls; device images never require host visibility. */
	chosen = -1;
	best = 0U;
	for (index = 0U; index < video->memory.memoryTypeCount; index++) {
		if ((requirements->memoryTypeBits & (1U << index)) == 0U)
			continue;
		flags = video->memory.memoryTypes[index].propertyFlags;
		if (host && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U)
			continue;
		score = 1U;
		if (host && (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U)
			score = 2U;
		if (!host && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U)
			score = 2U;
		if (score > best) {
			best = score;
			chosen = (int)index;
		}
	}

	/* Reject memory requirements with no compatible permitted memory type. */
	if (chosen < 0)
		return ENODEV;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	info.allocationSize = requirements->size;
	info.memoryTypeIndex = (uint32_t)chosen;
	result = video->fn.vkAllocateMemory(video->device, &info, NULL, memory);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkAllocateMemory");
		return error;
	}

	/* Report coherence from the selected memory type to the buffer owner. */
	if (coherent != NULL) {
		*coherent = 0;
		if ((video->memory.memoryTypes[chosen].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U)
			*coherent = 1;
	}

	/* Succeeded: the resource's requirements and host-cache contract are satisfied. */
	return 0;
}

/* Allocate a linear transfer destination with standard host visibility, independent of image tiling. */
static int
vkvideo_readback(
	struct vkvideo_runtime *video)
{
	VkBufferCreateInfo info;
	VkMemoryRequirements requirements;
	VkResult result;
	void *map;
	int error;

	/* The two plane copies pack full coded Y and UV consecutively into an ordinary buffer. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = (VkDeviceSize)video->extent.width * video->extent.height * 3U / 2U;
	info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = video->fn.vkCreateBuffer(video->device, &info, NULL, &video->readback);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateBuffer");
		return error;
	}

	/* Query staging-buffer memory before allocating its host-visible storage. */
	video->fn.vkGetBufferMemoryRequirements(video->device, video->readback, &requirements);
	error = vkvideo_allocate(video, &requirements, 1, &video->readback_memory, &video->readback_coherent);
	if (error != 0)
		return error;
	result = video->fn.vkBindBufferMemory(video->device, video->readback, video->readback_memory, 0U);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkBindBufferMemory");
		return error;
	}

	/* Map the newly bound staging allocation for host reads. */
	result = video->fn.vkMapMemory(video->device, video->readback_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkMapMemory");
		return error;
	}

	/* Publish the readback mapping only after mapping succeeded. */
	video->readback_map = map;

	/* Succeeded: the CPU can inspect the transfer result after its fence and cache invalidation. */
	return 0;
}

/* Flush or invalidate a complete mapped allocation without assuming cache-line alignment. */
static int
vkvideo_mapped_range(
	struct vkvideo_runtime *video,
	VkDeviceMemory memory,
	int invalidate)
{
	VkMappedMemoryRange range;
	VkResult result;
	int error;

	/* Whole-allocation ranges satisfy noncoherent atom alignment without a physical-device-specific stride. */
	memset(&range, 0, sizeof(range));
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = memory;
	range.size = VK_WHOLE_SIZE;
	if (invalidate)
		result = video->fn.vkInvalidateMappedMemoryRanges(video->device, 1U, &range);
	else
		result = video->fn.vkFlushMappedMemoryRanges(video->device, 1U, &range);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "mapped memory visibility");
		return error;
	}

	/* Succeeded: the mapped allocation is visible in the direction required by its owner. */
	return 0;
}

/* Record an image transition with video-decode stages expressed through synchronization two. */
static void
vkvideo_barrier(
	struct vkvideo_runtime *video,
	VkCommandBuffer command,
	VkImage image,
	VkImageLayout old_layout,
	VkImageLayout new_layout,
	VkPipelineStageFlags2 source_stage,
	VkAccessFlags2 source_access,
	VkPipelineStageFlags2 destination_stage,
	VkAccessFlags2 destination_access)
{
	VkImageMemoryBarrier2 barrier;
	VkDependencyInfo dependency;

	/* Optimal-plane layout and visibility stay a standard image resource responsibility. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	barrier.srcStageMask = source_stage;
	barrier.srcAccessMask = source_access;
	barrier.dstStageMask = destination_stage;
	barrier.dstAccessMask = destination_access;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	memset(&dependency, 0, sizeof(dependency));
	dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dependency.imageMemoryBarrierCount = 1U;
	dependency.pImageMemoryBarriers = &barrier;
	video->fn.vkCmdPipelineBarrier2KHR(command, &dependency);
}

/* Copy the cropped coded planes into an independently retained CPU picture after GPU retirement. */
static void
vkvideo_copy_output(
	struct vkvideo_runtime *video,
	const StdVideoH264SequenceParameterSet *sps,
	struct media_picture *output)
{
	unsigned row;
	unsigned x;
	unsigned y;
	const uint8_t *luma;
	const uint8_t *chroma;

	/* Progressive 4:2:0 crop units are two luma samples in each direction. */
	x = sps->frame_crop_left_offset * 2U;
	y = sps->frame_crop_top_offset * 2U;
	luma = video->readback_map + (size_t)y * video->extent.width + x;
	chroma = video->readback_map + (size_t)video->extent.width * video->extent.height + (size_t)(y / 2U) * video->extent.width + x;
	for (row = 0U; row < output->height; row++)
		memcpy(output->luma + (size_t)row * output->pitch, luma + (size_t)row * video->extent.width, output->width);
	for (row = 0U; row < (output->height + 1U) / 2U; row++)
		memcpy(output->chroma + (size_t)row * output->pitch, chroma + (size_t)row * video->extent.width, (output->width + 1U) & ~1U);
}

/* Classify standard Vulkan admission and execution failures without hiding allocation or input errors. */
static int
vkvideo_failed(
	VkResult result,
	const char *operation)
{
	/* Diagnostics stay in the library log and do not leak implementation details into product notices. */
	media_log("Vulkan Video %s failed (%d)", operation, (int)result);
	if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
		return ENOMEM;
	if (result == VK_ERROR_TOO_MANY_OBJECTS)
		return EBUSY;
	if (
		result == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR ||
		result == VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR ||
		result == VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR ||
		result == VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR ||
		result == VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR ||
		result == VK_ERROR_FORMAT_NOT_SUPPORTED)
		return ENOTSUP;
	if (result == VK_ERROR_DEVICE_LOST)
		return EIO;
	if (result == VK_TIMEOUT)
		return ETIMEDOUT;

	/* An execution failure cannot be mistaken for a successfully decoded picture. */
	return EIO;
}
