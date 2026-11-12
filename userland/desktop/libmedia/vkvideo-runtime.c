/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Standard Vulkan Video execution and NV12 transfer readback, adapted from the repository's Zlib WS083 probe.
 * Optimal images are device-owned; host pictures are populated only through vkCmdCopyImageToBuffer.
 *
 * When the program asked for pictures the GPU holds (media_decoder_gpu_pictures,
 * ws236-p011) and the device can give them -- sampled NV12 decode images whose
 * planes may be viewed, memory exported as an opaque fd -- nothing is read
 * back: the images' memory is exported once, each picture holds its image
 * until it is freed (no picture is decoded into a held image), and a picture
 * is complete when its decode's fence signals (the device signals it once the
 * frame is in the image).  The slots grow by the pictures held.
 */
#define VK_NO_PROTOTYPES
#include "vkvideo-runtime.h"
#include "media-private.h"
#include "media-os.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VKVIDEO_BINDINGS 32U
#define VKVIDEO_FAMILIES 64U
#define VKVIDEO_WAIT_NS 5000000000ULL
#define VKVIDEO_SOURCE_MAX (65U * 1024U * 1024U)

/*
 * The most decode slots a context makes (H.265: the sixteen pictures of
 * its DPB, those in flight and those the program holds), and the most
 * slice segments of one picture (H.265's limit; H.264's is lower).
 */
#define VKVIDEO_SLOTS MEDIA_GPU_SLOTS
#define VKVIDEO_SLICES 600U

/*
 * The pictures submitted ahead of the one being read back, at most.  The Raspberry Pi 4's firmware decoder, known by its vendor, takes
 * the next pictures while one is read back; on other devices one picture
 * is decoded, read back and copied before the next is submitted.
 */
#define VKVIDEO_FLIGHTS 4U

/*
 * The vendor the Raspberry Pi 4's Vulkan driver reports (Broadcom's PCI
 * vendor ID) in VkPhysicalDeviceProperties.
 */
#define VKVIDEO_FIRMWARE_VENDOR 0x14e4U

/* The create flags of an image whose planes are viewed in their own formats. */
#define VKVIDEO_PLANE_FLAGS (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)

/* The completed pictures whose results are kept for the backend's later questions. */
#define VKVIDEO_RESULTS 64U

/* How often the cost line is logged while pictures complete, and how many lines a runtime logs at most. */
#define VKVIDEO_STATS_NS 2000000000ULL
#define VKVIDEO_STATS_LINES 32U

/* A picture's submission: free, decoding (the decode submitted, its readback not yet), or reading (both submitted). */
enum vkvideo_state {
	VKVIDEO_FREE,
	VKVIDEO_DECODING,
	VKVIDEO_READING
};

/*
 * One picture's submission and the resources it uses until its readback
 * retires: the bitstream buffer, the decode and transfer command buffers,
 * the semaphore between them, the readback buffer and its fence, and the
 * status query of its own index.  ticket numbers the picture; slot is the
 * image it decodes into; output receives the cropped picture.
 */
struct vkvideo_flight {
	enum vkvideo_state state;
	uint32_t ticket;
	uint32_t slot;
	uint32_t crop_x;
	uint32_t crop_y;
	struct media_picture *output;
	VkBuffer buffer;
	VkDeviceMemory buffer_memory;
	uint8_t *buffer_map;
	VkDeviceSize buffer_size;
	int buffer_coherent;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	uint8_t *readback_map;
	int readback_coherent;
	VkCommandBuffer command;
	VkCommandBuffer transfer_command;
	VkSemaphore decoded;
	VkFence fence;
};

/* One completed picture's result: zero, EBADMSG for a failed decode, or the error that ended it. */
struct vkvideo_result {
	uint32_t ticket;
	int error;
};

/*
 * What the pictures of the last few seconds cost on the host, for the
 * line logged every VKVIDEO_STATS_NS (ws236 p008): the time submit took,
 * the wait for a readback's fence and the copy of the cropped picture out
 * of the readback buffer, in nanoseconds.
 */
struct vkvideo_stats {
	uint64_t window_start;
	uint32_t lines;
	uint32_t pictures;
	uint64_t submit_ns;
	uint64_t wait_ns;
	uint64_t copy_ns;
};

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
	PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
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
	VkPhysicalDeviceMemoryProperties memory;
	unsigned codec;
	VkVideoDecodeH264ProfileInfoKHR h264_profile;
	VkVideoDecodeH265ProfileInfoKHR h265_profile;
	VkVideoProfileInfoKHR profile;
	VkVideoProfileListInfoKHR profile_list;
	VkVideoCapabilitiesKHR capabilities;
	VkVideoDecodeCapabilitiesKHR decode_caps;
	VkVideoDecodeH264CapabilitiesKHR h264_caps;
	VkVideoDecodeH265CapabilitiesKHR h265_caps;
	uint32_t level;
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
	VkImage image[VKVIDEO_SLOTS + 1U];
	VkDeviceMemory image_memory[VKVIDEO_SLOTS + 1U];
	VkImageView view[VKVIDEO_SLOTS + 1U];
	VkCommandPool pool;
	VkBool32 status_supported;
	VkQueryPool status_pool;
	int initialized;
	int reset;
	uint32_t flights;
	uint32_t lag;
	int late_barrier;
	/*
	 * What the device takes (vkvideo_firmware): the pictures in flight at
	 * most, and whether a picture whose decode failed is skipped.
	 */
	uint32_t flight_limit;
	int skip_failed;
	/*
	 * Pictures the GPU holds: the pictures the program keeps at most (0:
	 * none asked), the stream's reorder depth (the decoded pictures that
	 * wait for their turn), whether the device has the external memory extensions,
	 * whether this context gives them, the shared record of its images, and
	 * the entry point that exports an image's memory.
	 */
	unsigned gpu_hold;
	unsigned reorder;
	int gpu_extensions;
	int gpu;
	struct media_gpu_slots *gpu_slots;
	PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR;
	struct vkvideo_flight flight[VKVIDEO_FLIGHTS];
	uint32_t tickets;
	struct vkvideo_result results[VKVIDEO_RESULTS];
	int failed;
	struct vkvideo_stats stats;
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
	{"vkGetPhysicalDeviceProperties", offsetof(struct vkvideo_functions, vkGetPhysicalDeviceProperties), 1},
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
static struct media_os_library *vkvideo_library;

/* Loader publication is immutable after one attempt and synchronized before any runtime uses it. */
static pthread_once_t vkvideo_loader_once = PTHREAD_ONCE_INIT;

/*
 * The identities of the contexts whose pictures the GPU holds; each takes the
 * next, from 1.  The counter is 32 bits so that its atomic increment is
 * lock-free on every target, including 32-bit x86.
 */
static uint32_t vkvideo_gpu_identity;

static void vkvideo_loader(void);
static int vkvideo_runtime_open(const struct vkvideo_shape *shape, unsigned reorder, struct vkvideo_runtime **runtime);
static int vkvideo_runtime_make(const struct vkvideo_shape *shape, unsigned reorder, unsigned hold, struct vkvideo_runtime **runtime);
static int vkvideo_parameters(struct vkvideo_runtime *video, const void *create);
static int vkvideo_submit(struct vkvideo_runtime *video, const struct vkvideo_job *job, struct media_picture *output, uint32_t *ticket);
static int vkvideo_dispatch(struct vkvideo_runtime *video, int instance);
static int vkvideo_instance(struct vkvideo_runtime *video);
static int vkvideo_resources(struct vkvideo_runtime *video);
static int vkvideo_device(struct vkvideo_runtime *video);
static void vkvideo_firmware(struct vkvideo_runtime *video);
static int vkvideo_family(struct vkvideo_runtime *video);
static int vkvideo_formats(struct vkvideo_runtime *video, VkImageUsageFlags usage);
static int vkvideo_formats_flags(struct vkvideo_runtime *video, VkImageUsageFlags usage, VkImageCreateFlags flags);
static void vkvideo_gpu_capabilities(struct vkvideo_runtime *video);
static int vkvideo_gpu_export(struct vkvideo_runtime *video, uint32_t slot, const VkMemoryRequirements *requirements);
static int vkvideo_submit_gpu(struct vkvideo_runtime *video, struct vkvideo_flight *flight);
static int vkvideo_capabilities(struct vkvideo_runtime *video);
static int vkvideo_allocate(struct vkvideo_runtime *video, const VkMemoryRequirements *requirements, int host, VkDeviceMemory *memory, int *coherent);
static int vkvideo_readback(struct vkvideo_runtime *video, struct vkvideo_flight *flight);
static int vkvideo_picture(struct vkvideo_runtime *video, uint32_t slot);
static int vkvideo_bitstream(struct vkvideo_runtime *video, struct vkvideo_flight *flight, size_t bytes);
static int vkvideo_session(struct vkvideo_runtime *video);
static int vkvideo_commands(struct vkvideo_runtime *video);
static int vkvideo_flight_commands(struct vkvideo_runtime *video, struct vkvideo_flight *flight);
static void vkvideo_flight_free(struct vkvideo_runtime *video, struct vkvideo_flight *flight);
static struct vkvideo_flight *vkvideo_oldest(struct vkvideo_runtime *video, enum vkvideo_state state);
static int vkvideo_record_decode(struct vkvideo_runtime *video, struct vkvideo_flight *flight, const struct vkvideo_job *job, VkDeviceSize range, const uint32_t *offsets);
static int vkvideo_record_readback(struct vkvideo_runtime *video, struct vkvideo_flight *flight, VkImageLayout output_layout);
static int vkvideo_submit_readback(struct vkvideo_runtime *video, struct vkvideo_flight *flight);
static int vkvideo_complete(struct vkvideo_runtime *video, struct vkvideo_flight *flight, int discard);
static int vkvideo_complete_all(struct vkvideo_runtime *video, int discard);
static void vkvideo_store(struct vkvideo_runtime *video, uint32_t ticket, int error);
static uint64_t vkvideo_now_ns(void);
static void vkvideo_report(struct vkvideo_runtime *video, uint64_t now);
static int vkvideo_status_pool(struct vkvideo_runtime *video);
static int vkvideo_failed(VkResult result, const char *operation);
static int vkvideo_mapped_range(struct vkvideo_runtime *video, VkDeviceMemory memory, int invalidate);
static void vkvideo_barrier(struct vkvideo_runtime *video, VkCommandBuffer command, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags2 source_stage, VkAccessFlags2 source_access, VkPipelineStageFlags2 destination_stage, VkAccessFlags2 destination_access);
static void vkvideo_copy_output(struct vkvideo_runtime *video, const struct vkvideo_flight *flight);

/*
 * Opens a standard H.264 decode context for one progressive sequence.
 */
int
media_vkvideo_runtime_open(
	const StdVideoH264SequenceParameterSet *sps,
	unsigned reorder,
	struct vkvideo_runtime **runtime)
{
	struct vkvideo_shape shape;

	/* The sequence's profile, level, coded size in macroblocks and references (at least one). */
	memset(&shape, 0, sizeof(shape));
	shape.codec = VKVIDEO_H264;
	shape.profile = (uint32_t)sps->profile_idc;
	shape.level = (uint32_t)sps->level_idc;
	shape.width = (sps->pic_width_in_mbs_minus1 + 1U) * 16U;
	shape.height = (sps->pic_height_in_map_units_minus1 + 1U) * 16U;
	shape.references = sps->max_num_ref_frames;
	if (shape.references == 0U)
		shape.references = 1U;
	return vkvideo_runtime_open(&shape, reorder, runtime);
}

/*
 * Opens a standard H.265 decode context for one sequence of the shape
 * (its StdVideoH265ProfileIdc, StdVideoH265LevelIdc, coded size in luma
 * samples and the references its DPB keeps at most).
 */
int
media_vkvideo_runtime_open_h265(
	const struct vkvideo_shape *shape,
	unsigned reorder,
	struct vkvideo_runtime **runtime)
{
	struct vkvideo_shape copy;

	/* The H.265 shape, with one reference at least. */
	copy = *shape;
	copy.codec = VKVIDEO_H265;
	if (copy.references == 0U)
		copy.references = 1U;
	return vkvideo_runtime_open(&copy, reorder, runtime);
}

/* Opens a context of a shape: the one the program asked for, else one whose pictures are read back. */
static int
vkvideo_runtime_open(
	const struct vkvideo_shape *shape,
	unsigned reorder,
	struct vkvideo_runtime **runtime)
{
	unsigned hold;
	int error;

	/* The loader is optional; native media reports unavailable video without loading FFmpeg. */
	*runtime = NULL;
	pthread_once(&vkvideo_loader_once, vkvideo_loader);
	if (vkvideo_library == NULL) {
		media_log("Vulkan Video: no Vulkan loader");
		return MEDIA_PROBLEM_DEVICE;
	}

	/* The context the program asked for. */
	hold = media_decoder_gpu_hold();
	error = vkvideo_runtime_make(shape, reorder, hold, runtime);

	/* A context whose pictures the GPU would hold but that could not be made is made again, read back. */
	if (error != 0 && hold != 0U) {
		media_log("Vulkan Video: a context of pictures the GPU holds failed (%d); pictures are read back", error);
		error = vkvideo_runtime_make(shape, reorder, 0U, runtime);
	}

	/* Reports a context that could not be made. */
	if (error != 0)
		return error;

	/* Succeeded: the context owns every image, buffer and session it decodes with. */
	return 0;
}

/*
 * Makes a context sized for the shape, whose pictures the GPU holds
 * (hold of them held by the program at once) when hold is not zero and the
 * device can.  Returns 0 or a problem (MEDIA_PROBLEM_*) or errno value.
 */
static int
vkvideo_runtime_make(
	const struct vkvideo_shape *shape,
	unsigned reorder,
	unsigned hold,
	struct vkvideo_runtime **runtime)
{
	struct vkvideo_runtime *video;
	int error;

	/* The context, sized for the sequence. */
	*runtime = NULL;
	video = calloc(1U, sizeof(*video));
	if (video == NULL)
		return ENOMEM;
	video->codec = shape->codec;
	video->level = shape->level;
	video->extent.width = shape->width;
	video->extent.height = shape->height;
	video->max_references = shape->references;
	video->slots = video->max_references + 1U;
	video->gpu_hold = hold;
	video->reorder = reorder;

	/* Profile chains are stable for the entire lifetime of this instance's resources. */
	video->profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	if (video->codec == VKVIDEO_H265) {
		video->h265_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
		video->h265_profile.stdProfileIdc = (StdVideoH265ProfileIdc)shape->profile;
		video->profile.pNext = &video->h265_profile;
		video->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
	} else {
		video->h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
		video->h264_profile.stdProfileIdc = (StdVideoH264ProfileIdc)shape->profile;
		video->h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
		video->profile.pNext = &video->h264_profile;
		video->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	}
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

	/* Succeeded: the context owns every image, buffer and session it decodes with. */
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

	/* Collect all currently published PPS entries for the session parameter object. */
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
	return vkvideo_parameters(video, &h264);
}

/*
 * Replace the session parameter object with the H.265 sets of an add
 * record (every VPS, SPS and PPS the stream has published).
 */
int
media_vkvideo_runtime_parameters_h265(
	struct vkvideo_runtime *video,
	const VkVideoDecodeH265SessionParametersAddInfoKHR *add)
{
	VkVideoDecodeH265SessionParametersCreateInfoKHR h265;

	/* Room for the add's sets. */
	memset(&h265, 0, sizeof(h265));
	h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR;
	h265.maxStdVPSCount = add->stdVPSCount;
	h265.maxStdSPSCount = add->stdSPSCount;
	h265.maxStdPPSCount = add->stdPPSCount;
	h265.pParametersAddInfo = add;
	return vkvideo_parameters(video, &h265);
}

/* Makes the session's parameters object from a codec's create record, in place of the one before. */
static int
vkvideo_parameters(
	struct vkvideo_runtime *video,
	const void *create)
{
	VkVideoSessionParametersCreateInfoKHR info;
	VkResult result;
	int error;

	/* The pictures in flight complete first: no submission may still use the old parameter object. */
	error = vkvideo_complete_all(video, 0);
	if (error != 0)
		return error;
	if (video->parameters != VK_NULL_HANDLE) {
		video->fn.vkDestroyVideoSessionParametersKHR(video->device, video->parameters, NULL);
		video->parameters = VK_NULL_HANDLE;
	}

	/* The parameters object holding the record's sets. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
	info.pNext = create;
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
 * Makes a submission free for the next picture, completing the oldest
 * picture when every one is taken.
 */
int
media_vkvideo_runtime_reserve(
	struct vkvideo_runtime *video)
{
	struct vkvideo_flight *oldest;
	uint32_t index;
	int error;

	/* A runtime that lost its device takes nothing more. */
	if (video->failed != 0)
		return video->failed;

	/* A free submission is enough. */
	for (index = 0U; index < video->flights; index++) {
		if (video->flight[index].state == VKVIDEO_FREE)
			return 0;
	}

	/* The oldest picture completes; its result waits for the backend's question. */
	oldest = vkvideo_oldest(video, VKVIDEO_READING);
	if (oldest == NULL)
		oldest = vkvideo_oldest(video, VKVIDEO_DECODING);
	error = vkvideo_complete(video, oldest, 0);
	if (error != 0)
		return error;

	/* Succeeded: one submission is free. */
	return 0;
}

/*
 * Submits one admitted H.264 picture's decode as the DPB planned it; see
 * vkvideo_submit.
 */
int
media_vkvideo_runtime_submit(
	struct vkvideo_runtime *video,
	const struct h264_stream *stream,
	const struct h264_picture *picture,
	const struct h264_dpb_plan *plan,
	struct media_picture *output,
	uint32_t *ticket)
{
	const StdVideoH264SequenceParameterSet *sps;
	struct vkvideo_job job;

	/* The picture's slices, its slots and records, its sequence's coded size and crop. */
	sps = &stream->sps[picture->info.seq_parameter_set_id];
	memset(&job, 0, sizeof(job));
	job.data = stream->data;
	job.slice_count = picture->slice_count;
	job.slice_offsets = picture->slice_offsets;
	job.slice_sizes = picture->slice_sizes;
	job.setup = plan->setup;
	job.reset = plan->reset;
	job.reference_count = plan->reference_count;
	job.references = plan->references;
	job.reference_info = plan->info;
	job.deactivate_count = plan->deactivate_count;
	job.deactivate = plan->deactivate;
	job.setup_info = &plan->setup_info;
	job.picture_info = &picture->info;
	job.width = (sps->pic_width_in_mbs_minus1 + 1U) * 16U;
	job.height = (sps->pic_height_in_map_units_minus1 + 1U) * 16U;
	job.crop_x = sps->frame_crop_left_offset * 2U;
	job.crop_y = sps->frame_crop_top_offset * 2U;
	return vkvideo_submit(video, &job, output, ticket);
}

/*
 * Submits one admitted H.265 picture's decode (job's records are
 * StdVideoDecodeH265PictureInfo and StdVideoDecodeH265ReferenceInfo); see
 * vkvideo_submit.
 */
int
media_vkvideo_runtime_submit_h265(
	struct vkvideo_runtime *video,
	const struct vkvideo_job *job,
	struct media_picture *output,
	uint32_t *ticket)
{
	/* An H.265 context only. */
	if (video->codec != VKVIDEO_H265)
		return EINVAL;
	return vkvideo_submit(video, job, output, ticket);
}

/*
 * Submits one admitted picture's decode, and the readbacks of earlier
 * pictures that are due; ticket receives the picture's number for
 * media_vkvideo_runtime_finish.
 */
static int
vkvideo_submit(
	struct vkvideo_runtime *video,
	const struct vkvideo_job *job,
	struct media_picture *output,
	uint32_t *ticket)
{
	uint32_t offsets[VKVIDEO_SLICES];
	struct vkvideo_flight *flight;
	VkSubmitInfo submit;
	VkDeviceSize aligned;
	VkDeviceSize alignment;
	VkImageLayout output_layout;
	VkResult result;
	uint64_t started;
	uint32_t decoding;
	uint32_t index;
	uint32_t slice;
	size_t at;
	int error;

	/* A free submission (the oldest picture completes when none is), and the time from here. */
	*ticket = 0U;
	error = media_vkvideo_runtime_reserve(video);
	if (error != 0)
		return error;
	started = vkvideo_now_ns();
	flight = NULL;
	for (index = 0U; index < video->flights; index++) {
		if (video->flight[index].state == VKVIDEO_FREE) {
			flight = &video->flight[index];
			break;
		}
	}

	/* reserve made one free. */
	if (flight == NULL)
		return EIO;

	/* Size and align the complete access unit before writing or submitting any part of it. */
	if (job->slice_count > VKVIDEO_SLICES)
		return EINVAL;
	at = 0U;
	for (slice = 0U; slice < job->slice_count; slice++) {
		if (job->slice_sizes[slice] > VKVIDEO_SOURCE_MAX - at - 3U)
			return EINVAL;
		at += 3U + job->slice_sizes[slice];
	}

	/* Pad the bitstream range to the queried device alignment. */
	alignment = video->capabilities.minBitstreamBufferSizeAlignment;
	aligned = ((at + alignment - 1U) / alignment) * alignment;
	if (aligned > VKVIDEO_SOURCE_MAX)
		return EINVAL;
	if (aligned > flight->buffer_size) {
		video->fn.vkUnmapMemory(video->device, flight->buffer_memory);
		video->fn.vkDestroyBuffer(video->device, flight->buffer, NULL);
		video->fn.vkFreeMemory(video->device, flight->buffer_memory, NULL);
		flight->buffer = VK_NULL_HANDLE;
		flight->buffer_memory = VK_NULL_HANDLE;
		flight->buffer_map = NULL;
		error = vkvideo_bitstream(video, flight, (size_t)aligned);
		if (error != 0)
			return error;
	}

	/* Pack complete Annex-B slice NALs into the mapped source buffer. */
	at = 0U;
	for (slice = 0U; slice < job->slice_count; slice++) {
		offsets[slice] = (uint32_t)at;
		flight->buffer_map[at] = 0U;
		flight->buffer_map[at + 1U] = 0U;
		flight->buffer_map[at + 2U] = 1U;
		memcpy(flight->buffer_map + at + 3U, job->data + job->slice_offsets[slice], job->slice_sizes[slice]);
		at += 3U + job->slice_sizes[slice];
	}

	/* Zero only the required alignment padding after the final slice. */
	memset(flight->buffer_map + at, 0, (size_t)aligned - at);
	if (!flight->buffer_coherent) {
		error = vkvideo_mapped_range(video, flight->buffer_memory, 0);
		if (error != 0)
			return error;
	}

	/* The picture's output: its slot's image, or the distinct output image. */
	output_layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
	flight->slot = (uint32_t)job->setup;
	if (video->distinct) {
		output_layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
		flight->slot = video->slots;
	}

	/* The crop of the sequence, for the copy into the CPU picture. */
	flight->crop_x = job->crop_x;
	flight->crop_y = job->crop_y;
	flight->output = output;

	/* The decode's command buffer. */
	error = vkvideo_record_decode(video, flight, job, aligned, offsets);
	if (error != 0)
		return error;

	/* A picture the GPU holds holds its image from here, and its decode carries the fence. */
	if (video->gpu) {
		media_gpu_slots_hold(output, video->gpu_slots, flight->slot, flight->crop_x, flight->crop_y);
		error = vkvideo_submit_gpu(video, flight);
		if (error != 0)
			return error;
		video->stats.submit_ns += vkvideo_now_ns() - started;
		*ticket = flight->ticket;
		return 0;
	}

	/* The readback's command buffer. */
	error = vkvideo_record_readback(video, flight, output_layout);
	if (error != 0)
		return error;

	/* The decode signals its readback's semaphore. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &flight->command;
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &flight->decoded;
	result = video->fn.vkQueueSubmit(video->queue, 1U, &submit, VK_NULL_HANDLE);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkQueueSubmit video");
		video->failed = error;
		return error;
	}

	/* The picture is in flight from here; the first decode's layouts and a seek's reset are recorded. */
	video->tickets++;
	flight->ticket = video->tickets;
	flight->state = VKVIDEO_DECODING;
	video->initialized = 1;
	video->reset = 0;
	*ticket = flight->ticket;

	/* The readbacks wait behind the newest decodes: the decoder takes those while the older pictures are read back. */
	decoding = 0U;
	for (index = 0U; index < video->flights; index++) {
		if (video->flight[index].state == VKVIDEO_DECODING)
			decoding++;
	}
	while (decoding > video->lag) {
		error = vkvideo_submit_readback(video, vkvideo_oldest(video, VKVIDEO_DECODING));
		if (error != 0)
			return error;
		decoding--;
	}

	/* The submit's time, for the cost line. */
	video->stats.submit_ns += vkvideo_now_ns() - started;

	/* Succeeded: the picture is submitted; finish gives its result. */
	return 0;
}

/*
 * Completes a submitted picture and the ones before it: the cropped
 * picture is in its CPU picture.  Returns 0, EBADMSG for a failed decode
 * (no picture), or the error that ended it.
 */
int
media_vkvideo_runtime_finish(
	struct vkvideo_runtime *video,
	uint32_t ticket)
{
	struct vkvideo_flight *flight;
	struct vkvideo_result *stored;
	uint32_t index;
	int error;

	/* Until the picture's own submission has completed. */
	for (;;) {
		/* A completed picture's kept result. */
		stored = &video->results[ticket % VKVIDEO_RESULTS];
		if (stored->ticket == ticket)
			return stored->error;

		/* The picture must still be in flight. */
		flight = NULL;
		for (index = 0U; index < video->flights; index++) {
			if (video->flight[index].state != VKVIDEO_FREE && video->flight[index].ticket == ticket)
				flight = &video->flight[index];
		}

		/* A ticket of neither is unknown (a discarded picture's result is gone). */
		if (flight == NULL)
			return ECANCELED;

		/* The oldest picture completes first: the readbacks retire in order. */
		flight = vkvideo_oldest(video, VKVIDEO_READING);
		if (flight == NULL)
			flight = vkvideo_oldest(video, VKVIDEO_DECODING);
		error = vkvideo_complete(video, flight, 0);
		if (error != 0)
			return error;
	}
}

/*
 * Completes every picture in flight without copying it, as at a seek: the
 * pictures' results are gone and their CPU pictures untouched.
 */
void
media_vkvideo_runtime_discard(
	struct vkvideo_runtime *video)
{
	/* Each picture retires, in order. */
	(void)vkvideo_complete_all(video, 1);
}

/* Reports the slots whose images pictures in flight are decoded into, as a mask. */
uint32_t
media_vkvideo_runtime_busy(
	const struct vkvideo_runtime *video)
{
	uint32_t busy;
	uint32_t index;

	/* Each submission in flight holds its slot's image. */
	busy = 0U;
	for (index = 0U; index < video->flights; index++) {
		if (video->flight[index].state != VKVIDEO_FREE && video->flight[index].slot < 32U)
			busy |= 1U << video->flight[index].slot;
	}

	/* So does each picture the GPU holds until it is freed. */
	busy |= media_gpu_slots_held(video->gpu_slots);

	/* Succeeded: the mask. */
	return busy;
}

/* Reports the decode slots of the session (references and pictures in flight). */
uint32_t
media_vkvideo_runtime_slots(
	const struct vkvideo_runtime *video)
{
	/* The images the session was made with. */
	return video->slots;
}

/* Reports how many pictures may be in flight at once. */
uint32_t
media_vkvideo_runtime_flights(
	const struct vkvideo_runtime *video)
{
	/* One per submission. */
	return video->flights;
}

/*
 * Reports whether a picture whose decode reported a failure is skipped
 * instead of ending the stream (the Raspberry Pi 4's firmware decoder can
 * miss a picture).
 */
int
media_vkvideo_runtime_skips_failed(
	const struct vkvideo_runtime *video)
{
	/* The device's answer, from when it was chosen. */
	return video->skip_failed;
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
	int inflight;

	/* A null owner is valid during partial admission unwind. */
	if (video == NULL)
		return;

	/* The device's objects, then the device. */
	if (video->device != VK_NULL_HANDLE) {
		result = VK_SUCCESS;
		if (video->fn.vkDeviceWaitIdle != NULL)
			result = video->fn.vkDeviceWaitIdle(video->device);
		inflight = 0;
		for (index = 0U; index < VKVIDEO_FLIGHTS; index++) {
			if (video->flight[index].state != VKVIDEO_FREE)
				inflight = 1;
		}

		/* A picture in flight whose retirement could not be proven keeps the context. */
		if (
			inflight &&
			result != VK_SUCCESS &&
			result != VK_ERROR_DEVICE_LOST) {
			media_log("Vulkan Video retains an unretired context after failed idle (%d)", (int)result);
			return;
		}

		/* Destroy optional command and query resources after device retirement. */
		if (video->status_pool != VK_NULL_HANDLE)
			video->fn.vkDestroyQueryPool(video->device, video->status_pool, NULL);
		for (index = 0U; index < VKVIDEO_FLIGHTS; index++)
			vkvideo_flight_free(video, &video->flight[index]);
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
		for (index = 0U; index < VKVIDEO_SLOTS + 1U; index++) {
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

	/* The images' record stays with the pictures that still hold them; their exports keep the memory. */
	media_gpu_slots_unref(video->gpu_slots);
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

	/* Pictures the GPU holds share the record of the images, which the images' exports fill. */
	if (video->gpu) {
		video->gpu_slots = media_gpu_slots_create(video->slots);
		if (video->gpu_slots == NULL)
			return ENOMEM;
		video->gpu_slots->identity = (uint64_t)__atomic_add_fetch(&vkvideo_gpu_identity, 1U, __ATOMIC_RELAXED);
		video->gpu_slots->image_width = video->image_extent.width;
		video->gpu_slots->image_height = video->image_extent.height;
	}

	/* The slots' pictures. */
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

	/* Each submission's host source buffer before its staging readback buffer. */
	for (slot = 0U; slot < video->flights; slot++) {
		error = vkvideo_bitstream(video, &video->flight[slot], 1024U * 1024U);
		if (error != 0)
			return error;

		/* A picture the GPU holds is not read back. */
		if (video->gpu)
			continue;
		error = vkvideo_readback(video, &video->flight[slot]);
		if (error != 0)
			return error;
	}

	/* The pools, and each submission's commands and synchronization. */
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
	VkExternalMemoryImageCreateInfo external;
	VkImageViewCreateInfo view;
	uint32_t families[2];
	VkMemoryRequirements requirements;
	VkResult result;
	int error;

	/* The image: NV12, optimal, the decoder's output and reference picture, of the profile. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.pNext = &video->profile_list;

	/* A picture the GPU holds is exported, and its planes are viewed in their own formats. */
	if (video->gpu) {
		memset(&external, 0, sizeof(external));
		external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		external.pNext = &video->profile_list;
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		image.pNext = &external;
		image.flags = VKVIDEO_PLANE_FLAGS;
	}

	/* Its shape, usage and sharing. */
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

	/* Its memory, bound; exported when the GPU holds its pictures. */
	video->fn.vkGetImageMemoryRequirements(video->device, video->image[slot], &requirements);
	if (video->gpu) {
		error = vkvideo_gpu_export(video, slot, &requirements);
	} else {
		error = vkvideo_allocate(video, &requirements, 0, &video->image_memory[slot], NULL);
	}

	/* No memory, no picture. */
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
	struct vkvideo_flight *flight,
	size_t bytes)
{
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkResult result;
	void *map;
	int error;

	/* The buffer: whole pages, a decode's source, of the profile. */
	flight->buffer_size = ((VkDeviceSize)bytes + 4095U) & ~(VkDeviceSize)4095U;
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.pNext = &video->profile_list;
	buffer.size = flight->buffer_size;
	buffer.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = video->fn.vkCreateBuffer(video->device, &buffer, NULL, &flight->buffer);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateBuffer");
		return error;
	}

	/* Its memory, bound and mapped. */
	video->fn.vkGetBufferMemoryRequirements(video->device, flight->buffer, &requirements);
	error = vkvideo_allocate(video, &requirements, 1, &flight->buffer_memory, &flight->buffer_coherent);
	if (error != 0)
		return error;
	result = video->fn.vkBindBufferMemory(video->device, flight->buffer, flight->buffer_memory, 0U);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkBindBufferMemory");
		return error;
	}

	/* Map the newly bound compressed source allocation for host writes. */
	result = video->fn.vkMapMemory(video->device, flight->buffer_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkMapMemory");
		return error;
	}

	/* Publish the source mapping only after mapping succeeded. */
	flight->buffer_map = map;

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
	static const VkExtensionProperties h265_header = {
	    VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME,
	    VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION};
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
	if (video->codec == VKVIDEO_H265)
		info.pStdHeaderVersion = &h265_header;
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

/* Create the family-specific command pools, each submission's commands and synchronization, and the status queries. */
static int
vkvideo_commands(
	struct vkvideo_runtime *video)
{
	VkCommandPoolCreateInfo pool;
	VkResult result;
	uint32_t index;
	int error;

	/* The pool, whose buffers are reset before each picture. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = video->family;
	result = video->fn.vkCreateCommandPool(video->device, &pool, NULL, &video->pool);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateCommandPool");
		return error;
	}

	/* A separate transfer pool permits drivers with a dedicated video-only queue. */
	pool.queueFamilyIndex = video->transfer_family;
	result = video->fn.vkCreateCommandPool(video->device, &pool, NULL, &video->transfer_pool);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateCommandPool transfer");
		return error;
	}

	/* Each submission's command buffers, fence and semaphore. */
	for (index = 0U; index < video->flights; index++) {
		error = vkvideo_flight_commands(video, &video->flight[index]);
		if (error != 0)
			return error;
	}

	/* The result status queries, where the family has them. */
	error = vkvideo_status_pool(video);
	if (error != 0)
		return error;

	/* Succeeded: the commands. */
	return 0;
}

/* Create one submission's decode and transfer command buffers, its readback fence and its decode-to-transfer semaphore. */
static int
vkvideo_flight_commands(
	struct vkvideo_runtime *video,
	struct vkvideo_flight *flight)
{
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkResult result;
	int error;

	/* The decode's buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = video->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	result = video->fn.vkAllocateCommandBuffers(video->device, &command, &flight->command);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkAllocateCommandBuffers");
		return error;
	}

	/* The readback's buffer, from the transfer family's pool. */
	command.commandPool = video->transfer_pool;
	result = video->fn.vkAllocateCommandBuffers(video->device, &command, &flight->transfer_command);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkAllocateCommandBuffers transfer");
		return error;
	}

	/* The readback's fence. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = video->fn.vkCreateFence(video->device, &fence, NULL, &flight->fence);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateFence");
		return error;
	}

	/* The decode-to-readback semaphore. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	result = video->fn.vkCreateSemaphore(video->device, &semaphore, NULL, &flight->decoded);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateSemaphore decoded");
		return error;
	}

	/* Succeeded: the submission can carry a picture. */
	return 0;
}

/* Destroy one submission's synchronization and buffers after device retirement (command buffers go with their pools). */
static void
vkvideo_flight_free(
	struct vkvideo_runtime *video,
	struct vkvideo_flight *flight)
{
	/* The fence and the semaphore. */
	if (flight->fence != VK_NULL_HANDLE)
		video->fn.vkDestroyFence(video->device, flight->fence, NULL);
	if (flight->decoded != VK_NULL_HANDLE)
		video->fn.vkDestroySemaphore(video->device, flight->decoded, NULL);

	/* The readback buffer. */
	if (flight->readback_map != NULL)
		video->fn.vkUnmapMemory(video->device, flight->readback_memory);
	if (flight->readback != VK_NULL_HANDLE)
		video->fn.vkDestroyBuffer(video->device, flight->readback, NULL);
	if (flight->readback_memory != VK_NULL_HANDLE)
		video->fn.vkFreeMemory(video->device, flight->readback_memory, NULL);

	/* The bitstream buffer. */
	if (flight->buffer_map != NULL)
		video->fn.vkUnmapMemory(video->device, flight->buffer_memory);
	if (flight->buffer != VK_NULL_HANDLE)
		video->fn.vkDestroyBuffer(video->device, flight->buffer, NULL);
	if (flight->buffer_memory != VK_NULL_HANDLE)
		video->fn.vkFreeMemory(video->device, flight->buffer_memory, NULL);
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
	if (!video->status_supported)
		return 0;

	/* One query of the decode's profile for each submission. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	info.pNext = &video->profile;
	info.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
	info.queryCount = video->flights;
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
	/* The system's loader, retained for process lifetime (NULL when there is none). */
	(void)media_os_vulkan_open(&vkvideo_library);
}

/* Load only the entry points whose instance or device dispatch is already available. */
static int
vkvideo_dispatch(
	struct vkvideo_runtime *video,
	int instance)
{
	PFN_vkVoidFunction address;
	media_os_function symbol;
	unsigned index;
	int missing;

	/* The loader's function-pointer representation is transferred explicitly rather than cast through ISO C. */
	if (video->fn.vkGetInstanceProcAddr == NULL) {
		symbol = media_os_library_symbol(vkvideo_library, "vkGetInstanceProcAddr");
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
		if (address == NULL) {
			media_log("Vulkan Video: no %s", vkvideo_symbols[index].name);
			missing = 1;
		}

		/* The entry point kept (NULL when missing). */
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
	/*
	 * The required instance extension is immutable and defined by the Vulkan
	 * API, not the GPU vendor; the second is asked for only for pictures the
	 * GPU holds, whose memory is exported.
	 */
	static const char *const extensions[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities"};
	VkApplicationInfo application;
	VkInstanceCreateInfo info;
	PFN_vkVoidFunction address;
	VkResult result;
	int error;
	media_os_function symbol;

	/* Obtain the global creation operation before an instance exists. */
	if (video->fn.vkGetInstanceProcAddr == NULL) {
		symbol = media_os_library_symbol(vkvideo_library, "vkGetInstanceProcAddr");
		if (symbol == NULL) {
			media_log("Vulkan Video: no vkGetInstanceProcAddr");
			return ENODEV;
		}

		/* The entry point kept. */
		memcpy(&video->fn.vkGetInstanceProcAddr, &symbol, sizeof(symbol));
	}

	/* Resolve instance creation from the process-wide Vulkan loader. */
	address = video->fn.vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
	if (address == NULL) {
		media_log("Vulkan Video: no vkCreateInstance");
		return ENODEV;
	}

	/* Instance creation, version 1.0 with the properties-two extension. */
	memcpy(&video->fn.vkCreateInstance, &address, sizeof(address));
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "libmedia";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &application;
	info.enabledExtensionCount = 1U;
	if (video->gpu_hold != 0U)
		info.enabledExtensionCount = 2U;
	info.ppEnabledExtensionNames = extensions;
	result = video->fn.vkCreateInstance(&info, NULL, &video->instance);

	/* An instance without the exporting extension reads its pictures back. */
	if (result == VK_ERROR_EXTENSION_NOT_PRESENT && video->gpu_hold != 0U) {
		media_log("Vulkan Video: no external memory capabilities; pictures are read back");
		video->gpu_hold = 0U;
		info.enabledExtensionCount = 1U;
		result = video->fn.vkCreateInstance(&info, NULL, &video->instance);
	}

	/* No instance, no device. */
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
	VkVideoCodecOperationFlagsKHR operation;
	uint32_t count;
	unsigned index;
	int transfer;
	int decoder;

	/* Query the count first so a truncated fixed array can never be indexed past its storage. */
	count = 0U;
	video->fn.vkGetPhysicalDeviceQueueFamilyProperties2KHR(video->physical, &count, NULL);
	if (count == 0U || count > VKVIDEO_FAMILIES) {
		media_log("Vulkan Video: %u queue families", count);
		return ENODEV;
	}

	/* The chains of every family. */
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
	operation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	if (video->codec == VKVIDEO_H265)
		operation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
	for (index = 0U; index < count; index++) {
		/* Each family's flags and decode operations, for the log. */
		media_log("Vulkan Video: queue family %u flags 0x%x operations 0x%x", index, (unsigned)families[index].queueFamilyProperties.queueFlags,
		    (unsigned)properties[index].videoCodecOperations);
		if ((families[index].queueFamilyProperties.queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0U)
			transfer = (int)index;
		if ((families[index].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) == 0U)
			continue;
		if ((properties[index].videoCodecOperations & operation) == 0U)
			continue;
		decoder = (int)index;
		video->status_supported = status[index].queryResultStatusSupport;
	}

	/* Require both a decode family of the codec and a transfer-capable family. */
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
	int error;

	/* No create flag is needed. */
	error = vkvideo_formats_flags(video, usage, 0U);
	if (error != 0)
		return error;

	/* Succeeded: an NV12 format takes the usage. */
	return 0;
}

/* Find an NV12 decode format of the profile that takes every usage and create flag asked. */
static int
vkvideo_formats_flags(
	struct vkvideo_runtime *video,
	VkImageUsageFlags usage,
	VkImageCreateFlags flags)
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
		if ((formats[index].imageCreateFlags & flags) != flags)
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
	uint32_t max_level;
	int error;

	/* Each candidate begins with an independent image-model choice. */
	video->distinct = 0;

	/* The profile query provides bitstream alignment, extent, reference capacity and image-model flags. */
	video->h264_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
	video->h265_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;
	video->decode_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
	video->decode_caps.pNext = &video->h264_caps;
	if (video->codec == VKVIDEO_H265)
		video->decode_caps.pNext = &video->h265_caps;
	video->capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
	video->capabilities.pNext = &video->decode_caps;
	result = video->fn.vkGetPhysicalDeviceVideoCapabilitiesKHR(video->physical, &video->profile, &video->capabilities);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
		return error;
	}

	/* The codec's level, then the limits, for the log. */
	max_level = (uint32_t)video->h264_caps.maxLevelIdc;
	if (video->codec == VKVIDEO_H265)
		max_level = (uint32_t)video->h265_caps.maxLevelIdc;
	media_log("Vulkan Video: extent %ux%u of %ux%u..%ux%u, level %u of %u, slots %u of %u, references %u of %u, flags 0x%x",
	    video->extent.width, video->extent.height, video->capabilities.minCodedExtent.width, video->capabilities.minCodedExtent.height,
	    video->capabilities.maxCodedExtent.width, video->capabilities.maxCodedExtent.height, (unsigned)video->level,
	    (unsigned)max_level, video->slots, video->capabilities.maxDpbSlots, video->max_references,
	    video->capabilities.maxActiveReferencePictures, (unsigned)video->decode_caps.flags);

	/* The sequence must fit both coded-extent limits and the advertised profile level. */
	if (video->extent.width > video->capabilities.maxCodedExtent.width || video->extent.height > video->capabilities.maxCodedExtent.height)
		return ENOTSUP;
	if (video->extent.width < video->capabilities.minCodedExtent.width || video->extent.height < video->capabilities.minCodedExtent.height)
		return ENOTSUP;
	if (video->level > max_level)
		return ENOTSUP;
	video->flights = 1U;
	video->slots = video->max_references + 1U;
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

	/*
	 * Pictures in flight: each holds its slot until it is read back, so
	 * the slots grow by them within the device's limit.  Only a device
	 * whose decode and transfer share a family and whose output is the
	 * DPB picture takes more than one (the readback then moves the
	 * output to the transfer layout itself).
	 */
	if (!video->distinct && video->family == video->transfer_family) {
		while (video->flights < video->flight_limit && video->max_references + video->flights + 1U <= video->capabilities.maxDpbSlots)
			video->flights++;
		video->slots = video->max_references + video->flights;
	}

	/* Pictures the GPU holds, where the device can give them; their slots grow by the pictures held. */
	vkvideo_gpu_capabilities(video);

	/* The readbacks wait behind the newest decodes (all but two of the pictures in flight) and move the output's layout themselves. */
	video->lag = 0U;
	if (video->flights > 2U)
		video->lag = video->flights - 2U;
	video->late_barrier = 0;
	if (video->flights > 1U)
		video->late_barrier = 1;

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

/*
 * Sets what the device being probed takes: the Raspberry Pi 4's firmware
 * decoder, known by its vendor, takes VKVIDEO_FLIGHTS pictures in flight
 * and may miss a picture, which is then skipped; any other device takes
 * one picture in flight and a failed decode ends the stream.
 */
static void
vkvideo_firmware(
	struct vkvideo_runtime *video)
{
	VkPhysicalDeviceProperties properties;

	/* Any device but the firmware decoder. */
	video->flight_limit = 1U;
	video->skip_failed = 0;

	/* The firmware decoder, by the vendor its driver reports. */
	memset(&properties, 0, sizeof(properties));
	video->fn.vkGetPhysicalDeviceProperties(video->physical, &properties);
	if (properties.vendorID == VKVIDEO_FIRMWARE_VENDOR) {
		video->flight_limit = VKVIDEO_FLIGHTS;
		video->skip_failed = 1;
	}
}

/* Enumerate every physical device instead of assuming the first enumerated GPU can decode video. */
static int
vkvideo_device(
	struct vkvideo_runtime *video)
{
	/*
	 * Required standard extensions are process-immutable names supplied to a
	 * selected physical device; the last two export the images of pictures
	 * the GPU holds, and are enabled only for them.
	 */
	static const char *const h264_extensions[] = {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
	static const char *const h265_extensions[] = {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
	const char *const *extensions;
	unsigned exporting;
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
	PFN_vkVoidFunction address;

	/* The codec's decode extension among the required ones. */
	extensions = h264_extensions;
	if (video->codec == VKVIDEO_H265)
		extensions = h265_extensions;

	/* Bound enumeration before filling storage with driver-reported device handles. */
	count = 0U;
	result = video->fn.vkEnumeratePhysicalDevices(video->instance, &count, NULL);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkEnumeratePhysicalDevices count");
		return error;
	}

	/* Empty or excessive enumeration cannot supply this runtime's bounded device selection. */
	if (count == 0U || count > 32U) {
		media_log("Vulkan Video: %u physical devices", count);
		return ENODEV;
	}

	/* The devices. */
	result = video->fn.vkEnumeratePhysicalDevices(video->instance, &count, devices);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkEnumeratePhysicalDevices");
		return error;
	}

	/* Probe every enumerated physical device independently. */
	for (index = 0U; index < count; index++) {
		video->physical = devices[index];
		vkvideo_firmware(video);
		error = vkvideo_family(video);
		if (error != 0) {
			media_log("Vulkan Video: device %u has no decode queue family of the codec and no transfer one", index);
			continue;
		}

		/* The device's extensions. */
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

		/* Match every required extension before creating a device, and the exporting ones. */
		matched = 0U;
		exporting = 0U;
		if (result == VK_SUCCESS) {
			for (extension = 0U; extension < 6U; extension++) {
				for (candidate = 0U; candidate < extension_count; candidate++) {
					comparison = strcmp(available[candidate].extensionName, extensions[extension]);
					if (comparison != 0)
						continue;

					/* A required one, or an exporting one. */
					if (extension < 4U) {
						matched++;
					} else {
						exporting++;
					}

					/* The name is found. */
					break;
				}
			}
		}

		/* Both exporting extensions, or none. */
		video->gpu_extensions = 0;
		if (exporting == 2U)
			video->gpu_extensions = 1;

		/* Release temporary extension enumeration before deciding whether this device is admitted. */
		free(available);
		if (matched != 4U) {
			media_log("Vulkan Video: device %u has %u of the 4 video extensions", index, matched);
			continue;
		}

		/* The profile's capabilities on this device. */
		error = vkvideo_capabilities(video);
		if (error == ENOTSUP || error == ENODEV) {
			media_log("Vulkan Video: device %u refuses the profile (%d)", index, error);
			continue;
		}

		/* Another failure ends the search. */
		if (error != 0)
			return error;
		memset(&features, 0, sizeof(features));
		memset(&synchronization, 0, sizeof(synchronization));
		features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features.pNext = &synchronization;
		synchronization.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
		video->fn.vkGetPhysicalDeviceFeatures2KHR(video->physical, &features);
		if (!synchronization.synchronization2) {
			media_log("Vulkan Video: device %u lacks synchronization2", index);
			continue;
		}

		/* The decode and transfer queues of the logical device. */
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
		if (video->gpu)
			info.enabledExtensionCount = 6U;
		info.ppEnabledExtensionNames = extensions;
		result = video->fn.vkCreateDevice(video->physical, &info, NULL, &video->device);
		if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
			return ENOMEM;
		if (result != VK_SUCCESS) {
			media_log("Vulkan Video: device %u vkCreateDevice failed (%d)", index, (int)result);
			continue;
		}

		/* The device's entry points, memory and queues. */
		error = vkvideo_dispatch(video, 0);
		if (error != 0)
			return error;

		/* Pictures the GPU holds need the export of an image's memory; without it they are read back. */
		if (video->gpu) {
			address = video->fn.vkGetDeviceProcAddr(video->device, "vkGetMemoryFdKHR");
			memcpy(&video->vkGetMemoryFdKHR, &address, sizeof(address));
			if (video->vkGetMemoryFdKHR == NULL) {
				media_log("Vulkan Video: no vkGetMemoryFdKHR; pictures are read back");
				video->gpu = 0;
			}
		}

		/* The memory and the queues. */
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
	struct vkvideo_runtime *video,
	struct vkvideo_flight *flight)
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
	result = video->fn.vkCreateBuffer(video->device, &info, NULL, &flight->readback);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkCreateBuffer");
		return error;
	}

	/* Query staging-buffer memory before allocating its host-visible storage. */
	video->fn.vkGetBufferMemoryRequirements(video->device, flight->readback, &requirements);
	error = vkvideo_allocate(video, &requirements, 1, &flight->readback_memory, &flight->readback_coherent);
	if (error != 0)
		return error;
	result = video->fn.vkBindBufferMemory(video->device, flight->readback, flight->readback_memory, 0U);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkBindBufferMemory");
		return error;
	}

	/* Map the newly bound staging allocation for host reads. */
	result = video->fn.vkMapMemory(video->device, flight->readback_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkMapMemory");
		return error;
	}

	/* Publish the readback mapping only after mapping succeeded. */
	flight->readback_map = map;

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

/* Copy a submission's cropped coded planes into its independently retained CPU picture after GPU retirement. */
static void
vkvideo_copy_output(
	struct vkvideo_runtime *video,
	const struct vkvideo_flight *flight)
{
	struct media_picture *output;
	unsigned row;
	unsigned x;
	unsigned y;
	const uint8_t *luma;
	const uint8_t *chroma;

	/* Progressive 4:2:0 crop units are two luma samples in each direction (the submission kept them in samples). */
	output = flight->output;
	x = flight->crop_x;
	y = flight->crop_y;
	luma = flight->readback_map + (size_t)y * video->extent.width + x;
	chroma = flight->readback_map + (size_t)video->extent.width * video->extent.height + (size_t)(y / 2U) * video->extent.width + x;
	for (row = 0U; row < output->height; row++)
		memcpy(output->luma + (size_t)row * output->pitch, luma + (size_t)row * video->extent.width, output->width);
	for (row = 0U; row < (output->height + 1U) / 2U; row++)
		memcpy(output->chroma + (size_t)row * output->pitch, chroma + (size_t)row * video->extent.width, (output->width + 1U) & ~1U);
}

/* Reports the oldest submission in a state, or NULL. */
static struct vkvideo_flight *
vkvideo_oldest(
	struct vkvideo_runtime *video,
	enum vkvideo_state state)
{
	struct vkvideo_flight *oldest;
	uint32_t index;

	/* The smallest ticket of the state. */
	oldest = NULL;
	for (index = 0U; index < video->flights; index++) {
		if (video->flight[index].state != state)
			continue;
		if (oldest == NULL || video->flight[index].ticket < oldest->ticket)
			oldest = &video->flight[index];
	}

	/* Succeeded: the oldest, if any. */
	return oldest;
}

/* Records one picture's decode into its submission's decode command buffer. */
static int
vkvideo_record_decode(
	struct vkvideo_runtime *video,
	struct vkvideo_flight *flight,
	const struct vkvideo_job *job,
	VkDeviceSize range,
	const uint32_t *offsets)
{
	VkVideoPictureResourceInfoKHR resources[VKVIDEO_SLOTS + 1U];
	VkVideoReferenceSlotInfoKHR bound[VKVIDEO_SLOTS * 2U + 1U];
	VkVideoReferenceSlotInfoKHR references[VKVIDEO_SLOTS];
	VkVideoDecodeH264DpbSlotInfoKHR reference_info[VKVIDEO_SLOTS];
	VkVideoDecodeH265DpbSlotInfoKHR h265_reference_info[VKVIDEO_SLOTS];
	VkVideoDecodeH264DpbSlotInfoKHR setup_info;
	VkVideoDecodeH265DpbSlotInfoKHR h265_setup_info;
	VkVideoDecodeH264PictureInfoKHR h264;
	VkVideoDecodeH265PictureInfoKHR h265;
	VkVideoReferenceSlotInfoKHR setup;
	VkVideoBeginCodingInfoKHR begin;
	VkVideoCodingControlInfoKHR control;
	VkVideoDecodeInfoKHR decode;
	VkVideoEndCodingInfoKHR end;
	VkCommandBufferBeginInfo record;
	VkVideoPictureResourceInfoKHR destination;
	VkImageLayout output_layout;
	VkResult result;
	uint32_t query;
	uint32_t slot;
	uint32_t index;
	uint32_t count;
	int error;

	/* The slots the job names must be the session's. */
	if (job->setup < 0 || (uint32_t)job->setup >= video->slots || job->reference_count > VKVIDEO_SLOTS || job->deactivate_count > VKVIDEO_SLOTS)
		return EINVAL;
	for (index = 0U; index < job->reference_count; index++) {
		if (job->references[index] < 0 || (uint32_t)job->references[index] >= video->slots)
			return EINVAL;
	}
	for (index = 0U; index < job->deactivate_count; index++) {
		if (job->deactivate[index] < 0 || (uint32_t)job->deactivate[index] >= video->slots)
			return EINVAL;
	}

	/* Every slot's picture resource: its image's coded picture of the sequence. */
	memset(resources, 0, sizeof(resources));
	for (slot = 0U; slot < video->slots; slot++) {
		resources[slot].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
		resources[slot].codedExtent.width = job->width;
		resources[slot].codedExtent.height = job->height;
		resources[slot].imageViewBinding = video->view[slot];
	}

	/* The command buffer from its start. */
	memset(&record, 0, sizeof(record));
	record.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	record.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = video->fn.vkBeginCommandBuffer(flight->command, &record);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkBeginCommandBuffer");
		return error;
	}

	/* The submission's own status query starts unavailable, outside the scope. */
	query = (uint32_t)(flight - video->flight);
	if (video->status_pool != VK_NULL_HANDLE)
		video->fn.vkCmdResetQueryPool(flight->command, video->status_pool, query, 1U);

	/* Initial allocation transitions occur once; coding reset after seek preserves established layouts. */
	if (!video->initialized) {
		for (slot = 0U; slot < video->slots; slot++)
			vkvideo_barrier(video, flight->command, video->image[slot], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_PIPELINE_STAGE_2_NONE, 0U, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
		if (video->distinct)
			vkvideo_barrier(video, flight->command, video->image[video->slots], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR, VK_PIPELINE_STAGE_2_NONE, 0U, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
	}

	/* Select either the coincident current DPB image or the distinct output image. */
	output_layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
	destination = resources[job->setup];
	if (video->distinct) {
		output_layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
		destination.imageViewBinding = video->view[video->slots];
	}

	/* The scope's slots: the references, the deactivated slots, the written picture without a slot. */
	memset(bound, 0, sizeof(bound));
	count = 0U;
	for (index = 0U; index < job->reference_count; index++) {
		bound[count].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		bound[count].slotIndex = job->references[index];
		bound[count].pPictureResource = &resources[job->references[index]];
		count++;
	}

	/* Deactivate stale device slots before associating the new current picture. */
	for (index = 0U; index < job->deactivate_count; index++) {
		bound[count].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		bound[count].slotIndex = job->deactivate[index];
		bound[count].pPictureResource = NULL;
		count++;
	}

	/* Bind the current picture resource without an old reference-slot association. */
	bound[count].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	bound[count].slotIndex = -1;
	bound[count].pPictureResource = &resources[job->setup];
	count++;

	/* The scope, and the reset of the first decode. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
	begin.videoSession = video->session;
	begin.videoSessionParameters = video->parameters;
	begin.referenceSlotCount = count;
	begin.pReferenceSlots = bound;
	video->fn.vkCmdBeginVideoCodingKHR(flight->command, &begin);
	if (job->reset || video->reset) {
		memset(&control, 0, sizeof(control));
		control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
		control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
		video->fn.vkCmdControlVideoCodingKHR(flight->command, &control);
	}

	/* The references with their information, each of the codec's record. */
	memset(references, 0, sizeof(references));
	memset(reference_info, 0, sizeof(reference_info));
	memset(h265_reference_info, 0, sizeof(h265_reference_info));
	for (index = 0U; index < job->reference_count; index++) {
		references[index].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		if (video->codec == VKVIDEO_H265) {
			h265_reference_info[index].sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
			h265_reference_info[index].pStdReferenceInfo = &((const StdVideoDecodeH265ReferenceInfo *)job->reference_info)[index];
			references[index].pNext = &h265_reference_info[index];
		} else {
			reference_info[index].sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
			reference_info[index].pStdReferenceInfo = &((const StdVideoDecodeH264ReferenceInfo *)job->reference_info)[index];
			references[index].pNext = &reference_info[index];
		}
		references[index].slotIndex = job->references[index];
		references[index].pPictureResource = &resources[job->references[index]];
	}

	/* The written slot, with the picture's own information. */
	memset(&setup_info, 0, sizeof(setup_info));
	setup_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
	setup_info.pStdReferenceInfo = job->setup_info;
	memset(&h265_setup_info, 0, sizeof(h265_setup_info));
	h265_setup_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
	h265_setup_info.pStdReferenceInfo = job->setup_info;
	memset(&setup, 0, sizeof(setup));
	setup.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	setup.pNext = &setup_info;
	if (video->codec == VKVIDEO_H265)
		setup.pNext = &h265_setup_info;
	setup.slotIndex = job->setup;
	setup.pPictureResource = &resources[job->setup];

	/* The decode: the slices into the written slot's picture, with the codec's picture record. */
	memset(&h264, 0, sizeof(h264));
	h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR;
	h264.pStdPictureInfo = job->picture_info;
	h264.sliceCount = job->slice_count;
	h264.pSliceOffsets = offsets;
	memset(&h265, 0, sizeof(h265));
	h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR;
	h265.pStdPictureInfo = job->picture_info;
	h265.sliceSegmentCount = job->slice_count;
	h265.pSliceSegmentOffsets = offsets;
	memset(&decode, 0, sizeof(decode));
	decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
	decode.pNext = &h264;
	if (video->codec == VKVIDEO_H265)
		decode.pNext = &h265;
	decode.srcBuffer = flight->buffer;
	decode.srcBufferOffset = 0U;
	decode.srcBufferRange = range;
	decode.dstPictureResource = destination;
	decode.pSetupReferenceSlot = &setup;
	decode.referenceSlotCount = job->reference_count;
	decode.pReferenceSlots = references;
	if (video->status_pool != VK_NULL_HANDLE)
		video->fn.vkCmdBeginQuery(flight->command, video->status_pool, query, 0U);
	video->fn.vkCmdDecodeVideoKHR(flight->command, &decode);
	if (video->status_pool != VK_NULL_HANDLE)
		video->fn.vkCmdEndQuery(flight->command, video->status_pool, query);

	/* The scope's end. */
	memset(&end, 0, sizeof(end));
	end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
	video->fn.vkCmdEndVideoCodingKHR(flight->command, &end);

	/*
	 * The output goes to the transfer layout here, unless the readback
	 * does it: with pictures decoded ahead, the later decodes may still
	 * name it as a reference in its decode layout.
	 */
	if (!video->late_barrier && !video->gpu)
		vkvideo_barrier(video, flight->command, video->image[flight->slot], output_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_NONE, 0U);
	result = video->fn.vkEndCommandBuffer(flight->command);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkEndCommandBuffer video");
		return error;
	}

	/* Succeeded: the decode is recorded. */
	return 0;
}

/* Records a picture's readback of both NV12 planes into its submission's transfer command buffer. */
static int
vkvideo_record_readback(
	struct vkvideo_runtime *video,
	struct vkvideo_flight *flight,
	VkImageLayout output_layout)
{
	VkCommandBufferBeginInfo record;
	VkBufferImageCopy copies[2];
	VkBufferMemoryBarrier2 host_barrier;
	VkDependencyInfo host_dependency;
	VkResult result;
	int error;

	/* Begin transfer recording after the video coding scope was closed. */
	memset(&record, 0, sizeof(record));
	record.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	record.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = video->fn.vkBeginCommandBuffer(flight->transfer_command, &record);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkBeginCommandBuffer transfer");
		return error;
	}

	/* With pictures decoded ahead, the output goes to the transfer layout only now (same queue family). */
	if (video->late_barrier)
		vkvideo_barrier(video, flight->transfer_command, video->image[flight->slot], output_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

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
	video->fn.vkCmdCopyImageToBuffer(flight->transfer_command, video->image[flight->slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, flight->readback, 2U, copies);
	vkvideo_barrier(video, flight->transfer_command, video->image[flight->slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output_layout, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_NONE, 0U);

	/* The host reads the buffer after the fence. */
	memset(&host_barrier, 0, sizeof(host_barrier));
	host_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
	host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
	host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
	host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host_barrier.buffer = flight->readback;
	host_barrier.size = VK_WHOLE_SIZE;
	memset(&host_dependency, 0, sizeof(host_dependency));
	host_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	host_dependency.bufferMemoryBarrierCount = 1U;
	host_dependency.pBufferMemoryBarriers = &host_barrier;
	video->fn.vkCmdPipelineBarrier2KHR(flight->transfer_command, &host_dependency);
	result = video->fn.vkEndCommandBuffer(flight->transfer_command);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkEndCommandBuffer transfer");
		return error;
	}

	/* Succeeded: the readback is recorded. */
	return 0;
}

/* Submits a decoding picture's readback, behind its decode's semaphore, with its fence. */
static int
vkvideo_submit_readback(
	struct vkvideo_runtime *video,
	struct vkvideo_flight *flight)
{
	VkSubmitInfo submit;
	VkPipelineStageFlags wait_stage;
	VkResult result;
	int error;

	/* The readback waits for the decode. */
	wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &flight->decoded;
	submit.pWaitDstStageMask = &wait_stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &flight->transfer_command;
	result = video->fn.vkQueueSubmit(video->transfer_queue, 1U, &submit, flight->fence);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkQueueSubmit transfer");
		video->failed = error;
		return error;
	}

	/* Succeeded: the picture is read back when the fence signals. */
	flight->state = VKVIDEO_READING;
	return 0;
}

/*
 * Completes one picture in flight: its readback is submitted if it was not
 * (with one more picture's behind it, so the next readback runs while
 * this picture is copied), its fence waited for and its status read, then
 * the cropped picture is copied out unless discard.  The result is kept
 * for media_vkvideo_runtime_finish; a failure to wait ends the runtime.
 */
static int
vkvideo_complete(
	struct vkvideo_runtime *video,
	struct vkvideo_flight *flight,
	int discard)
{
	struct vkvideo_flight *next;
	VkResult result;
	uint64_t started;
	uint64_t waited;
	int32_t status;
	int outcome;
	int error;

	/* The readbacks are submitted in order: every older decoding picture's, then this one's. */
	while (flight->state == VKVIDEO_DECODING) {
		error = vkvideo_submit_readback(video, vkvideo_oldest(video, VKVIDEO_DECODING));
		if (error != 0)
			return error;
	}

	/* The next picture's readback goes now too, so it runs while this picture is copied. */
	next = vkvideo_oldest(video, VKVIDEO_DECODING);
	if (next != NULL && video->lag != 0U && !discard) {
		error = vkvideo_submit_readback(video, next);
		if (error != 0)
			return error;
	}

	/* The readback's fence. */
	started = vkvideo_now_ns();
	result = video->fn.vkWaitForFences(video->device, 1U, &flight->fence, VK_TRUE, VKVIDEO_WAIT_NS);
	waited = vkvideo_now_ns();
	video->stats.wait_ns += waited - started;
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkWaitForFences readback");
		video->failed = error;
		return error;
	}

	/*
	 * The fence and the command buffers for the submission's next picture.
	 * A picture the GPU holds recorded no readback, so its transfer
	 * command buffer has nothing to reset, and its decode command buffer
	 * is reset by the next vkBeginCommandBuffer (the pool lets a buffer be
	 * reset one by one): two kernel requests less a picture.
	 */
	result = video->fn.vkResetFences(video->device, 1U, &flight->fence);
	if (result == VK_SUCCESS && !video->gpu)
		result = video->fn.vkResetCommandBuffer(flight->command, 0U);
	if (result == VK_SUCCESS && !video->gpu)
		result = video->fn.vkResetCommandBuffer(flight->transfer_command, 0U);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkResetFences or vkResetCommandBuffer");
		video->failed = error;
		return error;
	}

	/* Whether the decode completed: its query's status, when there is one. */
	outcome = 0;
	status = VK_QUERY_RESULT_STATUS_COMPLETE_KHR;
	if (video->status_pool != VK_NULL_HANDLE) {
		result = video->fn.vkGetQueryPoolResults(video->device, video->status_pool, (uint32_t)(flight - video->flight), 1U, sizeof(status), &status, sizeof(status), VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);
		if (result != VK_SUCCESS)
			outcome = vkvideo_failed(result, "vkGetQueryPoolResults");
	}

	/* A status failure is an actual decode error; no unverified pixels are exposed (ws236). */
	if (outcome == 0 && status != VK_QUERY_RESULT_STATUS_COMPLETE_KHR)
		outcome = EBADMSG;
	if (outcome == 0 && !discard && !video->gpu && !flight->readback_coherent)
		outcome = vkvideo_mapped_range(video, flight->readback_memory, 1);

	/*
	 * The cropped linear NV12 bytes, only after retirement and host
	 * visibility, timed for the cost line; a picture the GPU holds is in
	 * its image already.
	 */
	if (outcome == 0 && !discard) {
		if (!video->gpu)
			vkvideo_copy_output(video, flight);
		video->stats.pictures++;
		started = vkvideo_now_ns();
		video->stats.copy_ns += started - waited;
		vkvideo_report(video, started);
	}

	/* The result waits for its question; a discarded picture has none. */
	if (discard)
		outcome = ECANCELED;
	vkvideo_store(video, flight->ticket, outcome);
	flight->state = VKVIDEO_FREE;
	flight->output = NULL;

	/* Succeeded: the submission is free; its picture's result is kept. */
	return 0;
}

/* Completes every picture in flight, oldest first, copying them unless discard. */
static int
vkvideo_complete_all(
	struct vkvideo_runtime *video,
	int discard)
{
	struct vkvideo_flight *flight;
	int error;

	/* The oldest, until none is left. */
	for (;;) {
		flight = vkvideo_oldest(video, VKVIDEO_READING);
		if (flight == NULL)
			flight = vkvideo_oldest(video, VKVIDEO_DECODING);
		if (flight == NULL)
			break;
		error = vkvideo_complete(video, flight, discard);
		if (error != 0)
			return error;
	}

	/* Succeeded: nothing is in flight. */
	return 0;
}

/*
 * Decides whether this context gives pictures the GPU holds: the program
 * asked for them, the device exports memory, decodes into its references
 * (the output is the DPB picture), and offers sampled NV12 images whose
 * planes may be viewed, and the slots that holds every picture can hold
 * fit the device.  The decode family need not be the transfer family:
 * nothing is read back, and the images are shared by both families when
 * they differ.
 * The slots: the references, the pictures waiting for their turn and those
 * in flight, the pictures the program holds, and the one decoded into.
 */
static void
vkvideo_gpu_capabilities(
	struct vkvideo_runtime *video)
{
	uint32_t slots;
	uint32_t limit;
	uint32_t flights;
	int error;

	/* None asked for, none given. */
	video->gpu = 0;
	if (video->gpu_hold == 0U)
		return;

	/* The device must export memory and decode into the images it keeps. */
	if (!video->gpu_extensions) {
		media_log("Vulkan Video: no external memory; pictures are read back");
		return;
	}

	/* And decode into the images it keeps. */
	if (video->distinct)
		return;

	/* Sampled NV12 images whose planes may be viewed. */
	error = vkvideo_formats_flags(video, video->reference_usage | VK_IMAGE_USAGE_SAMPLED_BIT, VKVIDEO_PLANE_FLAGS);
	if (error != 0) {
		media_log("Vulkan Video: no sampled NV12 decode images; pictures are read back");
		return;
	}

	/* Every image a picture may hold, within the device's and the decoder's limits. */
	limit = video->capabilities.maxDpbSlots;
	if (video->codec == VKVIDEO_H264 && limit > H264_DPB_SLOTS)
		limit = H264_DPB_SLOTS;
	if (limit > VKVIDEO_SLOTS)
		limit = VKVIDEO_SLOTS;
	if (limit > MEDIA_GPU_SLOTS)
		limit = MEDIA_GPU_SLOTS;
	slots = video->max_references + video->reorder + video->flights + video->gpu_hold + 1U;

	/* Fewer pictures in flight (down to two) when the images would not fit otherwise. */
	flights = video->flights;
	while (slots > limit && flights > 2U) {
		flights--;
		slots--;
	}

	/* Still too many: the pictures are read back, with every picture in flight. */
	if (slots > limit) {
		media_log("Vulkan Video: %u slots for pictures the GPU holds, more than %u; pictures are read back", slots, limit);
		return;
	}

	/* Succeeded: the images are sampled ones, and there are enough of them. */
	video->flights = flights;
	video->slots = slots;
	video->reference_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	video->gpu = 1;
	media_log("Vulkan Video: pictures the GPU holds, %u slots (references %u, reorder %u, flights %u, held %u)",
	    slots, video->max_references, video->reorder, video->flights, video->gpu_hold);
}

/*
 * Allocates a picture's image memory exportable as an opaque fd, and
 * exports it into the images' record (with its size and type, the same
 * for every image).
 */
static int
vkvideo_gpu_export(
	struct vkvideo_runtime *video,
	uint32_t slot,
	const VkMemoryRequirements *requirements)
{
	VkExportMemoryAllocateInfo export_info;
	VkMemoryAllocateInfo info;
	VkMemoryGetFdInfoKHR get;
	VkResult result;
	uint32_t index;
	int chosen;
	int fd;
	int error;

	/* A device-local type the image takes, else any it takes. */
	chosen = -1;
	for (index = 0U; index < video->memory.memoryTypeCount; index++) {
		if ((requirements->memoryTypeBits & (1U << index)) == 0U)
			continue;
		if (chosen < 0)
			chosen = (int)index;
		if ((video->memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
			chosen = (int)index;
			break;
		}
	}

	/* An image no type takes cannot be allocated. */
	if (chosen < 0)
		return ENODEV;

	/* The allocation, exportable. */
	memset(&export_info, 0, sizeof(export_info));
	export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
	export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	info.pNext = &export_info;
	info.allocationSize = requirements->size;
	info.memoryTypeIndex = (uint32_t)chosen;
	result = video->fn.vkAllocateMemory(video->device, &info, NULL, &video->image_memory[slot]);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkAllocateMemory export");
		return error;
	}

	/* Its handle, kept in the record until the record's last reference. */
	memset(&get, 0, sizeof(get));
	get.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
	get.memory = video->image_memory[slot];
	get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	fd = -1;
	result = video->vkGetMemoryFdKHR(video->device, &get, &fd);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkGetMemoryFdKHR");
		return error;
	}

	/* The record keeps it, with the memory's size and type. */
	video->gpu_slots->fd[slot] = fd;
	video->gpu_slots->memory_bytes = requirements->size;
	video->gpu_slots->memory_type = (uint32_t)chosen;

	/* Succeeded: the image's memory and its handle. */
	return 0;
}

/*
 * Submits the decode of a picture the GPU holds with its fence: the device
 * signals it once the frame is in the image, and nothing is read back.
 */
static int
vkvideo_submit_gpu(
	struct vkvideo_runtime *video,
	struct vkvideo_flight *flight)
{
	VkSubmitInfo submit;
	VkResult result;
	int error;

	/* The decode and its fence. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &flight->command;
	result = video->fn.vkQueueSubmit(video->queue, 1U, &submit, flight->fence);
	if (result != VK_SUCCESS) {
		error = vkvideo_failed(result, "vkQueueSubmit video");
		video->failed = error;
		return error;
	}

	/* The picture is in flight and completes with the fence; the first decode's layouts and a seek's reset are recorded. */
	video->tickets++;
	flight->ticket = video->tickets;
	flight->state = VKVIDEO_READING;
	video->initialized = 1;
	video->reset = 0;

	/* Succeeded: the decode is submitted. */
	return 0;
}

/*
 * Reports whether the context gives pictures the GPU holds.
 */
int
media_vkvideo_runtime_gpu(
	const struct vkvideo_runtime *video)
{
	/* A context that reads its pictures back gives none. */
	if (video == NULL || !video->gpu)
		return 0;

	/* Succeeded: its pictures hold their images. */
	return 1;
}

/* Reports the monotonic clock in nanoseconds. */
static uint64_t
vkvideo_now_ns(
	void)
{
	struct timespec now;
	int error;

	/* The clock; one that cannot be read counts as zero. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0U;

	/* Succeeded: the time. */
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/*
 * Logs the cost line when VKVIDEO_STATS_NS have passed since the window
 * opened (the first VKVIDEO_STATS_LINES windows), and opens the next: the
 * pictures copied out, and the average submit, fence wait and copy in
 * microseconds.
 */
static void
vkvideo_report(
	struct vkvideo_runtime *video,
	uint64_t now)
{
	struct vkvideo_stats *stats;
	uint32_t pictures;

	/* The first picture opens the first window. */
	stats = &video->stats;
	if (stats->window_start == 0U) {
		stats->window_start = now;
		return;
	}

	/* A window shorter than VKVIDEO_STATS_NS keeps counting. */
	if (now - stats->window_start < VKVIDEO_STATS_NS)
		return;

	/* The averages, while the runtime's lines last. */
	pictures = stats->pictures;
	if (pictures != 0U && stats->lines < VKVIDEO_STATS_LINES) {
		stats->lines++;
		media_log("Vulkan Video: %u pictures in %llu ms, %u in flight; avg_us submit %llu wait %llu copy %llu",
		    (unsigned)pictures, (unsigned long long)((now - stats->window_start) / 1000000ULL), (unsigned)video->flights,
		    (unsigned long long)(stats->submit_ns / pictures / 1000ULL),
		    (unsigned long long)(stats->wait_ns / pictures / 1000ULL),
		    (unsigned long long)(stats->copy_ns / pictures / 1000ULL));
	}

	/* The next window starts empty. */
	stats->window_start = now;
	stats->pictures = 0U;
	stats->submit_ns = 0U;
	stats->wait_ns = 0U;
	stats->copy_ns = 0U;
}

/* Keeps a completed picture's result under its ticket. */
static void
vkvideo_store(
	struct vkvideo_runtime *video,
	uint32_t ticket,
	int error)
{
	struct vkvideo_result *stored;

	/* The ring's place of the ticket. */
	stored = &video->results[ticket % VKVIDEO_RESULTS];
	stored->ticket = ticket;
	stored->error = error;
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
