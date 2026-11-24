/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Standard Vulkan Video H.264 hardware encoding backend for libmedia.
 * Accepts CPU NV12 pictures and GPU-written LINEAR NV12 slots (copied on
 * the GPU to OPTIMAL TileY before VDEnc/MFX submission).
 */

#define VK_NO_PROTOTYPES
#include "media-os.h"
#include "media-private.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#ifdef __ZEDBSD__
#include <vulkan/vulkan_video.h>
#else
#include <vk_video/vulkan_video_codec_h264std.h>
#include <vk_video/vulkan_video_codec_h264std_encode.h>
#endif

#define VKENC_BUFFER_USAGE_VIDEO_ENCODE_DST	((VkBufferUsageFlags)0x00008000U)
#define VKENC_IMAGE_LAYOUT_VIDEO_ENCODE_SRC	((VkImageLayout)1000299001)

#define VKENC_BINDINGS		32U
#define VKENC_FAMILIES		64U
#define VKENC_DEVICES		16U
#define VKENC_EXTENSIONS	128U
#define VKENC_SLOTS		4U
#define VKENC_DPB_SLOTS		2U
#define VKENC_BITSTREAM_BYTES	(4U * 1024U * 1024U)
#define VKENC_WAIT_NS		5000000000ULL
#define VKENC_MIN_SIDE		16U
#define VKENC_DEFAULT_QP	26U
#define VKENC_KEY_QP_DELTA	3U
#define VKENC_PLANE_FLAGS \
	(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)

struct vkenc_functions {
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
	PFN_vkCreateInstance vkCreateInstance;
	PFN_vkDestroyInstance vkDestroyInstance;
	PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
	PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
	PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
	PFN_vkGetPhysicalDeviceProperties2KHR vkGetPhysicalDeviceProperties2KHR;
	PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR vkGetPhysicalDeviceQueueFamilyProperties2KHR;
	PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR vkGetPhysicalDeviceVideoCapabilitiesKHR;
	PFN_vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR;
	PFN_vkCreateDevice vkCreateDevice;
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
	PFN_vkDestroyDevice vkDestroyDevice;
	PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
	PFN_vkGetDeviceQueue vkGetDeviceQueue;
	PFN_vkCreateCommandPool vkCreateCommandPool;
	PFN_vkDestroyCommandPool vkDestroyCommandPool;
	PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
	PFN_vkResetCommandBuffer vkResetCommandBuffer;
	PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
	PFN_vkEndCommandBuffer vkEndCommandBuffer;
	PFN_vkCreateFence vkCreateFence;
	PFN_vkDestroyFence vkDestroyFence;
	PFN_vkWaitForFences vkWaitForFences;
	PFN_vkResetFences vkResetFences;
	PFN_vkCreateSemaphore vkCreateSemaphore;
	PFN_vkDestroySemaphore vkDestroySemaphore;
	PFN_vkQueueSubmit vkQueueSubmit;
	PFN_vkCreateBuffer vkCreateBuffer;
	PFN_vkDestroyBuffer vkDestroyBuffer;
	PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
	PFN_vkAllocateMemory vkAllocateMemory;
	PFN_vkFreeMemory vkFreeMemory;
	PFN_vkBindBufferMemory vkBindBufferMemory;
	PFN_vkMapMemory vkMapMemory;
	PFN_vkUnmapMemory vkUnmapMemory;
	PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges;
	PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges;
	PFN_vkCreateImage vkCreateImage;
	PFN_vkDestroyImage vkDestroyImage;
	PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
	PFN_vkGetImageSubresourceLayout vkGetImageSubresourceLayout;
	PFN_vkBindImageMemory vkBindImageMemory;
	PFN_vkCreateImageView vkCreateImageView;
	PFN_vkDestroyImageView vkDestroyImageView;
	PFN_vkCreateQueryPool vkCreateQueryPool;
	PFN_vkDestroyQueryPool vkDestroyQueryPool;
	PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
	PFN_vkCreateVideoSessionKHR vkCreateVideoSessionKHR;
	PFN_vkDestroyVideoSessionKHR vkDestroyVideoSessionKHR;
	PFN_vkGetVideoSessionMemoryRequirementsKHR vkGetVideoSessionMemoryRequirementsKHR;
	PFN_vkBindVideoSessionMemoryKHR vkBindVideoSessionMemoryKHR;
	PFN_vkCreateVideoSessionParametersKHR vkCreateVideoSessionParametersKHR;
	PFN_vkDestroyVideoSessionParametersKHR vkDestroyVideoSessionParametersKHR;
	PFN_vkGetEncodedVideoSessionParametersKHR vkGetEncodedVideoSessionParametersKHR;
	PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
	PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
	PFN_vkCmdCopyImage vkCmdCopyImage;
	PFN_vkCmdResetQueryPool vkCmdResetQueryPool;
	PFN_vkCmdBeginQuery vkCmdBeginQuery;
	PFN_vkCmdEndQuery vkCmdEndQuery;
	PFN_vkCmdBeginVideoCodingKHR vkCmdBeginVideoCodingKHR;
	PFN_vkCmdControlVideoCodingKHR vkCmdControlVideoCodingKHR;
	PFN_vkCmdEndVideoCodingKHR vkCmdEndVideoCodingKHR;
	PFN_vkCmdEncodeVideoKHR vkCmdEncodeVideoKHR;
	PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR;
};

struct vkenc_slot {
	VkImage image;
	VkDeviceMemory memory;
	struct media_encoder_slot info;
	int acquired;
};

struct vkvideo_enc {
	struct vkenc_functions fn;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkPhysicalDeviceMemoryProperties mem_props;
	uint32_t transfer_family;
	uint32_t encode_family;
	VkDevice device;
	VkQueue transfer_queue;
	VkQueue encode_queue;
	VkCommandPool transfer_pool;
	VkCommandPool encode_pool;
	VkCommandBuffer transfer_cmd;
	VkCommandBuffer encode_cmd;
	VkSemaphore transfer_done;
	VkFence encode_done;
	VkQueryPool query_pool;
	VkVideoProfileInfoKHR profile;
	VkVideoEncodeH264ProfileInfoKHR h264_profile;
	VkVideoSessionKHR session;
	VkDeviceMemory session_mem[VKENC_BINDINGS];
	uint32_t session_mem_count;
	VkVideoSessionParametersKHR parameters;
	uint8_t *ps_buf;
	size_t ps_len;
	uint32_t width;
	uint32_t height;
	uint32_t coded_w;
	uint32_t coded_h;
	uint32_t qp_i;
	uint32_t qp_p;
	uint32_t key_interval;
	unsigned input;
	unsigned profile_kind;
	uint32_t level_idc;
	VkImage src_image;
	VkDeviceMemory src_mem;
	VkImageView src_view;
	VkImage dpb_image[VKENC_DPB_SLOTS];
	VkDeviceMemory dpb_mem[VKENC_DPB_SLOTS];
	VkImageView dpb_view[VKENC_DPB_SLOTS];
	VkBuffer staging_buf;
	VkDeviceMemory staging_mem;
	uint8_t *staging_map;
	int staging_coherent;
	VkBuffer out_buf;
	VkDeviceMemory out_mem;
	uint8_t *out_map;
	int out_coherent;
	struct vkenc_slot slots[VKENC_SLOTS];
	uint32_t slot_count;
	int has_external_fd;
	uint32_t frame_num;
	uint32_t idr_pic_id;
	uint32_t frames_since_idr;
	uint32_t cur_dpb;
	uint32_t prev_dpb;
	int has_ref;
	int session_reset;
	int in_flight;
	int flight_key;
	int64_t flight_time_us;
	unsigned flight_marks;
	int flight_slot;
	int draining;
	int lost;
	uint8_t *pkt_buf;
	struct media_encoding_recovery recovery;
};

struct vkenc_probe {
	int supported;
	int has_external_fd;
	uint32_t transfer_family;
	uint32_t encode_family;
	VkVideoCapabilitiesKHR caps;
	VkVideoEncodeCapabilitiesKHR enc_caps;
	VkVideoEncodeH264CapabilitiesKHR h264_caps;
	uint8_t device_uuid[16];
	uint8_t driver_uuid[16];
};

struct vkenc_symbol {
	const char *name;
	size_t offset;
	int required;
};

static const struct vkenc_symbol vkenc_instance_symbols[] = {
	{"vkDestroyInstance", offsetof(struct vkenc_functions, vkDestroyInstance), 1},
	{"vkEnumeratePhysicalDevices", offsetof(struct vkenc_functions, vkEnumeratePhysicalDevices), 1},
	{"vkEnumerateDeviceExtensionProperties", offsetof(struct vkenc_functions, vkEnumerateDeviceExtensionProperties), 1},
	{"vkGetPhysicalDeviceProperties", offsetof(struct vkenc_functions, vkGetPhysicalDeviceProperties), 1},
	{"vkGetPhysicalDeviceProperties2KHR", offsetof(struct vkenc_functions, vkGetPhysicalDeviceProperties2KHR), 0},
	{"vkGetPhysicalDeviceMemoryProperties", offsetof(struct vkenc_functions, vkGetPhysicalDeviceMemoryProperties), 1},
	{"vkGetPhysicalDeviceQueueFamilyProperties2KHR", offsetof(struct vkenc_functions, vkGetPhysicalDeviceQueueFamilyProperties2KHR), 1},
	{"vkGetPhysicalDeviceVideoCapabilitiesKHR", offsetof(struct vkenc_functions, vkGetPhysicalDeviceVideoCapabilitiesKHR), 1},
	{"vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR", offsetof(struct vkenc_functions, vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR), 0},
	{"vkCreateDevice", offsetof(struct vkenc_functions, vkCreateDevice), 1},
	{"vkGetDeviceProcAddr", offsetof(struct vkenc_functions, vkGetDeviceProcAddr), 1}
};

static const struct vkenc_symbol vkenc_device_symbols[] = {
	{"vkDestroyDevice", offsetof(struct vkenc_functions, vkDestroyDevice), 1},
	{"vkDeviceWaitIdle", offsetof(struct vkenc_functions, vkDeviceWaitIdle), 1},
	{"vkGetDeviceQueue", offsetof(struct vkenc_functions, vkGetDeviceQueue), 1},
	{"vkCreateCommandPool", offsetof(struct vkenc_functions, vkCreateCommandPool), 1},
	{"vkDestroyCommandPool", offsetof(struct vkenc_functions, vkDestroyCommandPool), 1},
	{"vkAllocateCommandBuffers", offsetof(struct vkenc_functions, vkAllocateCommandBuffers), 1},
	{"vkResetCommandBuffer", offsetof(struct vkenc_functions, vkResetCommandBuffer), 1},
	{"vkBeginCommandBuffer", offsetof(struct vkenc_functions, vkBeginCommandBuffer), 1},
	{"vkEndCommandBuffer", offsetof(struct vkenc_functions, vkEndCommandBuffer), 1},
	{"vkCreateFence", offsetof(struct vkenc_functions, vkCreateFence), 1},
	{"vkDestroyFence", offsetof(struct vkenc_functions, vkDestroyFence), 1},
	{"vkWaitForFences", offsetof(struct vkenc_functions, vkWaitForFences), 1},
	{"vkResetFences", offsetof(struct vkenc_functions, vkResetFences), 1},
	{"vkCreateSemaphore", offsetof(struct vkenc_functions, vkCreateSemaphore), 1},
	{"vkDestroySemaphore", offsetof(struct vkenc_functions, vkDestroySemaphore), 1},
	{"vkQueueSubmit", offsetof(struct vkenc_functions, vkQueueSubmit), 1},
	{"vkCreateBuffer", offsetof(struct vkenc_functions, vkCreateBuffer), 1},
	{"vkDestroyBuffer", offsetof(struct vkenc_functions, vkDestroyBuffer), 1},
	{"vkGetBufferMemoryRequirements", offsetof(struct vkenc_functions, vkGetBufferMemoryRequirements), 1},
	{"vkAllocateMemory", offsetof(struct vkenc_functions, vkAllocateMemory), 1},
	{"vkFreeMemory", offsetof(struct vkenc_functions, vkFreeMemory), 1},
	{"vkBindBufferMemory", offsetof(struct vkenc_functions, vkBindBufferMemory), 1},
	{"vkMapMemory", offsetof(struct vkenc_functions, vkMapMemory), 1},
	{"vkUnmapMemory", offsetof(struct vkenc_functions, vkUnmapMemory), 1},
	{"vkFlushMappedMemoryRanges", offsetof(struct vkenc_functions, vkFlushMappedMemoryRanges), 1},
	{"vkInvalidateMappedMemoryRanges", offsetof(struct vkenc_functions, vkInvalidateMappedMemoryRanges), 1},
	{"vkCreateImage", offsetof(struct vkenc_functions, vkCreateImage), 1},
	{"vkDestroyImage", offsetof(struct vkenc_functions, vkDestroyImage), 1},
	{"vkGetImageMemoryRequirements", offsetof(struct vkenc_functions, vkGetImageMemoryRequirements), 1},
	{"vkGetImageSubresourceLayout", offsetof(struct vkenc_functions, vkGetImageSubresourceLayout), 1},
	{"vkBindImageMemory", offsetof(struct vkenc_functions, vkBindImageMemory), 1},
	{"vkCreateImageView", offsetof(struct vkenc_functions, vkCreateImageView), 1},
	{"vkDestroyImageView", offsetof(struct vkenc_functions, vkDestroyImageView), 1},
	{"vkCreateQueryPool", offsetof(struct vkenc_functions, vkCreateQueryPool), 1},
	{"vkDestroyQueryPool", offsetof(struct vkenc_functions, vkDestroyQueryPool), 1},
	{"vkGetQueryPoolResults", offsetof(struct vkenc_functions, vkGetQueryPoolResults), 1},
	{"vkCreateVideoSessionKHR", offsetof(struct vkenc_functions, vkCreateVideoSessionKHR), 1},
	{"vkDestroyVideoSessionKHR", offsetof(struct vkenc_functions, vkDestroyVideoSessionKHR), 1},
	{"vkGetVideoSessionMemoryRequirementsKHR", offsetof(struct vkenc_functions, vkGetVideoSessionMemoryRequirementsKHR), 1},
	{"vkBindVideoSessionMemoryKHR", offsetof(struct vkenc_functions, vkBindVideoSessionMemoryKHR), 1},
	{"vkCreateVideoSessionParametersKHR", offsetof(struct vkenc_functions, vkCreateVideoSessionParametersKHR), 1},
	{"vkDestroyVideoSessionParametersKHR", offsetof(struct vkenc_functions, vkDestroyVideoSessionParametersKHR), 1},
	{"vkGetEncodedVideoSessionParametersKHR", offsetof(struct vkenc_functions, vkGetEncodedVideoSessionParametersKHR), 1},
	{"vkCmdPipelineBarrier", offsetof(struct vkenc_functions, vkCmdPipelineBarrier), 1},
	{"vkCmdCopyBufferToImage", offsetof(struct vkenc_functions, vkCmdCopyBufferToImage), 1},
	{"vkCmdCopyImage", offsetof(struct vkenc_functions, vkCmdCopyImage), 1},
	{"vkCmdResetQueryPool", offsetof(struct vkenc_functions, vkCmdResetQueryPool), 1},
	{"vkCmdBeginQuery", offsetof(struct vkenc_functions, vkCmdBeginQuery), 1},
	{"vkCmdEndQuery", offsetof(struct vkenc_functions, vkCmdEndQuery), 1},
	{"vkCmdBeginVideoCodingKHR", offsetof(struct vkenc_functions, vkCmdBeginVideoCodingKHR), 1},
	{"vkCmdControlVideoCodingKHR", offsetof(struct vkenc_functions, vkCmdControlVideoCodingKHR), 1},
	{"vkCmdEndVideoCodingKHR", offsetof(struct vkenc_functions, vkCmdEndVideoCodingKHR), 1},
	{"vkCmdEncodeVideoKHR", offsetof(struct vkenc_functions, vkCmdEncodeVideoKHR), 1},
	{"vkGetMemoryFdKHR", offsetof(struct vkenc_functions, vkGetMemoryFdKHR), 0}
};

static int vkenc_query(unsigned codec, struct media_encoder_info *infos, unsigned capacity, unsigned *count);
static int vkenc_open(const struct media_encoder_config *config, void **state);
static int vkenc_send(void *state, const struct media_encoder_picture *picture, int64_t time_us, unsigned flags);
static int vkenc_slots(void *state, struct media_encoder_slot *slots, unsigned capacity, unsigned *count);
static int vkenc_acquire(void *state, unsigned *index);
static int vkenc_send_slot(void *state, unsigned index, int64_t time_us, unsigned flags);
static int vkenc_receive(void *state, unsigned flags, struct media_encoded *packet);
static int vkenc_parameter_sets(const void *state, const uint8_t **data, size_t *size);
static void vkenc_stats(const void *state, struct media_encoder_stats *stats);
static void vkenc_close(void *state);

const struct media_encoder_ops media_vkvideo_encoder_ops = {
	"vkvideo-enc",
	MEDIA_ENCODER_HARDWARE,
	vkenc_query,
	vkenc_open,
	vkenc_send,
	vkenc_slots,
	vkenc_acquire,
	vkenc_send_slot,
	vkenc_receive,
	NULL,
	vkenc_parameter_sets,
	vkenc_stats,
	vkenc_close
};

/*
 * Loads the Vulkan loader and creates an instance with physical device queries.
 */
static int
vkenc_create_instance(
	struct vkenc_functions *fn,
	VkInstance *out_instance,
	int *out_ext_caps)
{
	struct media_os_library *lib;
	const char *exts[2];
	VkApplicationInfo app;
	VkInstanceCreateInfo info;
	VkResult result;
	size_t i;

	memset(fn, 0, sizeof(*fn));
	*out_instance = VK_NULL_HANDLE;
	*out_ext_caps = 0;

	if (media_os_vulkan_open(&lib) != 0)
		return ENODEV;
	fn->vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)media_os_library_symbol(lib, "vkGetInstanceProcAddr");
	if (fn->vkGetInstanceProcAddr == NULL)
		return ENODEV;
	fn->vkCreateInstance = (PFN_vkCreateInstance)(void *)fn->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
	if (fn->vkCreateInstance == NULL)
		return ENODEV;

	exts[0] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
	exts[1] = VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME;

	memset(&app, 0, sizeof(app));
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "libmedia";
	app.apiVersion = VK_API_VERSION_1_1;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &app;
	info.enabledExtensionCount = 2U;
	info.ppEnabledExtensionNames = exts;

	result = fn->vkCreateInstance(&info, NULL, out_instance);
	if (result == VK_SUCCESS) {
		*out_ext_caps = 1;
	} else if (result == VK_ERROR_EXTENSION_NOT_PRESENT) {
		info.enabledExtensionCount = 1U;
		result = fn->vkCreateInstance(&info, NULL, out_instance);
		if (result == VK_ERROR_EXTENSION_NOT_PRESENT) {
			info.enabledExtensionCount = 0U;
			result = fn->vkCreateInstance(&info, NULL, out_instance);
		}
	}
	if (result != VK_SUCCESS)
		return ENODEV;

	for (i = 0U; i < sizeof(vkenc_instance_symbols) / sizeof(vkenc_instance_symbols[0]); i++) {
		PFN_vkVoidFunction sym = fn->vkGetInstanceProcAddr(*out_instance, vkenc_instance_symbols[i].name);

		if (sym == NULL && vkenc_instance_symbols[i].required) {
			if (fn->vkDestroyInstance != NULL)
				fn->vkDestroyInstance(*out_instance, NULL);
			*out_instance = VK_NULL_HANDLE;
			return ENODEV;
		}
		memcpy((char *)fn + vkenc_instance_symbols[i].offset, &sym, sizeof(sym));
	}

	return 0;
}

/*
 * Loads device-level Vulkan entry points for an open VkDevice.
 */
static int
vkenc_load_device_symbols(
	struct vkenc_functions *fn,
	VkDevice device)
{
	size_t i;

	for (i = 0U; i < sizeof(vkenc_device_symbols) / sizeof(vkenc_device_symbols[0]); i++) {
		PFN_vkVoidFunction sym = fn->vkGetDeviceProcAddr(device, vkenc_device_symbols[i].name);

		if (sym == NULL && vkenc_device_symbols[i].required) {
			media_log("vkvideo-enc: missing device symbol %s", vkenc_device_symbols[i].name);
			return ENODEV;
		}
		memcpy((char *)fn + vkenc_device_symbols[i].offset, &sym, sizeof(sym));
	}

	return 0;
}

/*
 * Checks whether a physical device has all required encode extensions.
 */
static int
vkenc_has_extensions(
	const struct vkenc_functions *fn,
	VkPhysicalDevice physical,
	int *out_external_fd)
{
	VkExtensionProperties exts[VKENC_EXTENSIONS];
	uint32_t count;
	uint32_t i;
	int has_vq = 0;
	int has_ve = 0;
	int has_h264 = 0;
	int has_ext_mem = 0;
	int has_ext_fd = 0;

	count = VKENC_EXTENSIONS;
	if (fn->vkEnumerateDeviceExtensionProperties(physical, NULL, &count, exts) < 0)
		return 0;
	for (i = 0U; i < count; i++) {
		if (strcmp(exts[i].extensionName, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) == 0)
			has_vq = 1;
		else if (strcmp(exts[i].extensionName, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME) == 0)
			has_ve = 1;
		else if (strcmp(exts[i].extensionName, VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME) == 0)
			has_h264 = 1;
		else if (strcmp(exts[i].extensionName, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0)
			has_ext_mem = 1;
		else if (strcmp(exts[i].extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0)
			has_ext_fd = 1;
	}
	*out_external_fd = (has_ext_mem && has_ext_fd) ? 1 : 0;
	return has_vq && has_ve && has_h264;
}

/*
 * Probes a physical device for H.264 video encode capabilities and queue families.
 */
static void
vkenc_probe_device(
	const struct vkenc_functions *fn,
	VkPhysicalDevice physical,
	struct vkenc_probe *out)
{
	VkQueueFamilyProperties2 families[VKENC_FAMILIES];
	VkQueueFamilyVideoPropertiesKHR vprops[VKENC_FAMILIES];
	VkVideoProfileInfoKHR prof;
	VkVideoEncodeH264ProfileInfoKHR h264_prof;
	VkPhysicalDeviceProperties2 props2;
	VkPhysicalDeviceIDProperties id_props;
	uint32_t count;
	uint32_t i;
	int transfer_fam;
	int encode_fam;

	memset(out, 0, sizeof(*out));
	if (!vkenc_has_extensions(fn, physical, &out->has_external_fd))
		return;

	count = 0U;
	fn->vkGetPhysicalDeviceQueueFamilyProperties2KHR(physical, &count, NULL);
	if (count == 0U || count > VKENC_FAMILIES)
		return;

	memset(families, 0, sizeof(families));
	memset(vprops, 0, sizeof(vprops));
	for (i = 0U; i < count; i++) {
		families[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
		families[i].pNext = &vprops[i];
		vprops[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
	}
	fn->vkGetPhysicalDeviceQueueFamilyProperties2KHR(physical, &count, families);

	transfer_fam = -1;
	encode_fam = -1;
	for (i = 0U; i < count; i++) {
		VkQueueFlags qf = families[i].queueFamilyProperties.queueFlags;

		if (transfer_fam < 0 && (qf & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) != 0U)
			transfer_fam = (int)i;
		if (encode_fam < 0 &&
		    (qf & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) != 0U &&
		    (vprops[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) != 0U)
			encode_fam = (int)i;
	}
	if (transfer_fam < 0 || encode_fam < 0)
		return;

	memset(&h264_prof, 0, sizeof(h264_prof));
	h264_prof.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR;
	h264_prof.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;

	memset(&prof, 0, sizeof(prof));
	prof.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	prof.pNext = &h264_prof;
	prof.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
	prof.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	prof.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	prof.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

	out->h264_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR;
	out->enc_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;
	out->enc_caps.pNext = &out->h264_caps;
	out->caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
	out->caps.pNext = &out->enc_caps;

	if (fn->vkGetPhysicalDeviceVideoCapabilitiesKHR(physical, &prof, &out->caps) != VK_SUCCESS)
		return;

	if (fn->vkGetPhysicalDeviceProperties2KHR != NULL) {
		memset(&id_props, 0, sizeof(id_props));
		id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
		memset(&props2, 0, sizeof(props2));
		props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		props2.pNext = &id_props;
		fn->vkGetPhysicalDeviceProperties2KHR(physical, &props2);
		memcpy(out->device_uuid, id_props.deviceUUID, sizeof(out->device_uuid));
		memcpy(out->driver_uuid, id_props.driverUUID, sizeof(out->driver_uuid));
	}

	out->transfer_family = (uint32_t)transfer_fam;
	out->encode_family = (uint32_t)encode_fam;
	out->supported = 1;
}

/*
 * Lists H.264 hardware encoder devices exposed through Vulkan Video Encode.
 */
static int
vkenc_query(
	unsigned codec,
	struct media_encoder_info *infos,
	unsigned capacity,
	unsigned *count)
{
	struct vkenc_functions fn;
	VkInstance instance;
	VkPhysicalDevice devs[VKENC_DEVICES];
	uint32_t dev_count;
	uint32_t i;
	int ext_caps;
	unsigned found;

	*count = 0U;
	if (codec != MEDIA_CODEC_H264)
		return 0;

	if (vkenc_create_instance(&fn, &instance, &ext_caps) != 0)
		return 0;

	dev_count = VKENC_DEVICES;
	if (fn.vkEnumeratePhysicalDevices(instance, &dev_count, devs) < 0)
		dev_count = 0U;

	found = 0U;
	for (i = 0U; i < dev_count; i++) {
		struct vkenc_probe probe;

		vkenc_probe_device(&fn, devs[i], &probe);
		if (!probe.supported)
			continue;
		if (infos != NULL && found < capacity) {
			struct media_encoder_info *dst = &infos[found];
			uint32_t step;

			memset(dst, 0, sizeof(*dst));
			snprintf(dst->backend, sizeof(dst->backend), "vkvideo-enc");
			dst->kind = MEDIA_ENCODER_HARDWARE;
			dst->codec = MEDIA_CODEC_H264;
			dst->profiles =
				(1U << MEDIA_ENCODER_PROFILE_H264_CONSTRAINED_BASELINE) |
				(1U << MEDIA_ENCODER_PROFILE_H264_MAIN) |
				(1U << MEDIA_ENCODER_PROFILE_H264_HIGH);
			dst->min_width = (probe.caps.minCodedExtent.width >= VKENC_MIN_SIDE) ?
				probe.caps.minCodedExtent.width : VKENC_MIN_SIDE;
			dst->min_height = (probe.caps.minCodedExtent.height >= VKENC_MIN_SIDE) ?
				probe.caps.minCodedExtent.height : VKENC_MIN_SIDE;
			dst->max_width = probe.caps.maxCodedExtent.width;
			dst->max_height = probe.caps.maxCodedExtent.height;
			step = probe.caps.pictureAccessGranularity.width;
			dst->step = (step >= 2U) ? step : 2U;
			dst->rate_controls =
				(1U << MEDIA_ENCODER_RATE_DEFAULT) |
				(1U << MEDIA_ENCODER_RATE_CQP);
			dst->min_qp = (probe.h264_caps.minQp >= 0) ? (unsigned)probe.h264_caps.minQp : 0U;
			dst->max_qp = (probe.h264_caps.maxQp > 0) ? (unsigned)probe.h264_caps.maxQp : 51U;
			dst->max_b_frames = 0U;
			dst->max_references = (probe.h264_caps.maxPPictureL0ReferenceCount > 0U) ?
				probe.h264_caps.maxPPictureL0ReferenceCount : 1U;
			dst->quality_levels = (probe.enc_caps.maxQualityLevels > 0U) ?
				probe.enc_caps.maxQualityLevels : 1U;
			dst->inputs = MEDIA_ENCODER_INPUT_NV12;
			if (ext_caps && probe.has_external_fd)
				dst->inputs |= MEDIA_ENCODER_INPUT_GPU_NV12;
			memcpy(dst->device_uuid, probe.device_uuid, sizeof(dst->device_uuid));
			memcpy(dst->driver_uuid, probe.driver_uuid, sizeof(dst->driver_uuid));
		}
		found++;
	}

	fn.vkDestroyInstance(instance, NULL);
	*count = found;
	return 0;
}

/*
 * Selects a memory type index matching required bits and property flags.
 */
static int
vkenc_find_memory_type(
	const VkPhysicalDeviceMemoryProperties *props,
	uint32_t type_bits,
	VkMemoryPropertyFlags wanted,
	uint32_t *out_index,
	int *out_coherent)
{
	uint32_t i;

	for (i = 0U; i < props->memoryTypeCount; i++) {
		if ((type_bits & (1U << i)) == 0U)
			continue;
		if ((props->memoryTypes[i].propertyFlags & wanted) == wanted) {
			*out_index = i;
			if (out_coherent != NULL)
				*out_coherent = (props->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U ? 1 : 0;
			return 0;
		}
	}
	return ENOMEM;
}

/*
 * Allocates and binds device memory for a VkBuffer, mapping it if requested.
 */
static int
vkenc_create_buffer(
	struct vkvideo_enc *enc,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags props,
	VkBuffer *out_buf,
	VkDeviceMemory *out_mem,
	uint8_t **out_map,
	int *out_coherent)
{
	VkBufferCreateInfo binfo;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo ainfo;
	uint32_t type_idx;
	void *mapped;
	VkResult res;

	memset(&binfo, 0, sizeof(binfo));
	binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	binfo.size = size;
	binfo.usage = usage;
	binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	res = enc->fn.vkCreateBuffer(enc->device, &binfo, NULL, out_buf);
	if (res != VK_SUCCESS)
		return ENOMEM;

	enc->fn.vkGetBufferMemoryRequirements(enc->device, *out_buf, &req);
	if (vkenc_find_memory_type(&enc->mem_props, req.memoryTypeBits, props, &type_idx, out_coherent) != 0)
		return ENOMEM;

	memset(&ainfo, 0, sizeof(ainfo));
	ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ainfo.allocationSize = req.size;
	ainfo.memoryTypeIndex = type_idx;
	res = enc->fn.vkAllocateMemory(enc->device, &ainfo, NULL, out_mem);
	if (res != VK_SUCCESS)
		return ENOMEM;

	res = enc->fn.vkBindBufferMemory(enc->device, *out_buf, *out_mem, 0);
	if (res != VK_SUCCESS)
		return EIO;

	if (out_map != NULL) {
		res = enc->fn.vkMapMemory(enc->device, *out_mem, 0, size, 0, &mapped);
		if (res != VK_SUCCESS)
			return EIO;
		*out_map = (uint8_t *)mapped;
	}
	return 0;
}

/*
 * Allocates an NV12 image and optional view (OPTIMAL for video, or LINEAR exportable).
 */
static int
vkenc_create_nv12_image(
	struct vkvideo_enc *enc,
	uint32_t width,
	uint32_t height,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	int export_fd,
	VkImage *out_img,
	VkDeviceMemory *out_mem,
	VkImageView *out_view,
	uint64_t *out_bytes,
	uint32_t *out_mem_type)
{
	VkImageCreateInfo iinfo;
	VkExternalMemoryImageCreateInfo ext_info;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo ainfo;
	VkExportMemoryAllocateInfo export_info;
	VkImageViewCreateInfo vinfo;
	uint32_t type_idx;
	VkMemoryPropertyFlags prop;
	VkResult res;

	memset(&ext_info, 0, sizeof(ext_info));
	ext_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	ext_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

	memset(&iinfo, 0, sizeof(iinfo));
	iinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	iinfo.pNext = export_fd ? &ext_info : NULL;
	iinfo.flags = export_fd ? VKENC_PLANE_FLAGS : 0U;
	iinfo.imageType = VK_IMAGE_TYPE_2D;
	iinfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	iinfo.extent.width = width;
	iinfo.extent.height = height;
	iinfo.extent.depth = 1U;
	iinfo.mipLevels = 1U;
	iinfo.arrayLayers = 1U;
	iinfo.samples = VK_SAMPLE_COUNT_1_BIT;
	iinfo.tiling = tiling;
	iinfo.usage = usage;
	iinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	iinfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	res = enc->fn.vkCreateImage(enc->device, &iinfo, NULL, out_img);
	if (res != VK_SUCCESS)
		return ENOMEM;

	enc->fn.vkGetImageMemoryRequirements(enc->device, *out_img, &req);
	prop = (tiling == VK_IMAGE_TILING_LINEAR) ?
		(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) :
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (vkenc_find_memory_type(&enc->mem_props, req.memoryTypeBits, prop, &type_idx, NULL) != 0) {
		if (vkenc_find_memory_type(&enc->mem_props, req.memoryTypeBits, 0U, &type_idx, NULL) != 0)
			return ENOMEM;
	}

	memset(&export_info, 0, sizeof(export_info));
	export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
	export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

	memset(&ainfo, 0, sizeof(ainfo));
	ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ainfo.pNext = export_fd ? &export_info : NULL;
	ainfo.allocationSize = req.size;
	ainfo.memoryTypeIndex = type_idx;
	res = enc->fn.vkAllocateMemory(enc->device, &ainfo, NULL, out_mem);
	if (res != VK_SUCCESS)
		return ENOMEM;

	res = enc->fn.vkBindImageMemory(enc->device, *out_img, *out_mem, 0);
	if (res != VK_SUCCESS)
		return EIO;

	if (out_bytes != NULL)
		*out_bytes = req.size;
	if (out_mem_type != NULL)
		*out_mem_type = type_idx;

	if (out_view != NULL) {
		memset(&vinfo, 0, sizeof(vinfo));
		vinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vinfo.image = *out_img;
		vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vinfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
		vinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vinfo.subresourceRange.levelCount = 1U;
		vinfo.subresourceRange.layerCount = 1U;
		res = enc->fn.vkCreateImageView(enc->device, &vinfo, NULL, out_view);
		if (res != VK_SUCCESS)
			return ENOMEM;
	}
	return 0;
}

/*
 * Selects the closest H.264 level_idc for the configured coded dimensions.
 */
static StdVideoH264LevelIdc
vkenc_pick_level(
	uint32_t coded_w,
	uint32_t coded_h,
	unsigned requested)
{
	uint32_t mbs;

	if (requested != 0U) {
		if (requested <= 30U)
			return STD_VIDEO_H264_LEVEL_IDC_3_0;
		if (requested <= 31U)
			return STD_VIDEO_H264_LEVEL_IDC_3_1;
		if (requested <= 40U)
			return STD_VIDEO_H264_LEVEL_IDC_4_0;
		if (requested <= 41U)
			return STD_VIDEO_H264_LEVEL_IDC_4_1;
		return STD_VIDEO_H264_LEVEL_IDC_5_1;
	}
	mbs = (coded_w / 16U) * (coded_h / 16U);
	if (mbs <= 1620U)
		return STD_VIDEO_H264_LEVEL_IDC_3_0;
	if (mbs <= 3600U)
		return STD_VIDEO_H264_LEVEL_IDC_3_1;
	if (mbs <= 8192U)
		return STD_VIDEO_H264_LEVEL_IDC_4_0;
	return STD_VIDEO_H264_LEVEL_IDC_5_1;
}

/*
 * Creates the video session, binds its memory, and builds encoded SPS+PPS bytes.
 */
static int
vkenc_setup_session(
	struct vkvideo_enc *enc,
	const struct media_encoder_config *config)
{
	VkVideoEncodeH264SessionCreateInfoKHR h264_sess;
	VkExtensionProperties std_header;
	VkVideoSessionCreateInfoKHR sess_info;
	VkVideoSessionMemoryRequirementsKHR reqs[VKENC_BINDINGS];
	VkBindVideoSessionMemoryInfoKHR binds[VKENC_BINDINGS];
	StdVideoH264SequenceParameterSet sps;
	StdVideoH264PictureParameterSet pps;
	VkVideoEncodeH264SessionParametersAddInfoKHR add_info;
	VkVideoEncodeH264SessionParametersCreateInfoKHR h264_params;
	VkVideoSessionParametersCreateInfoKHR params_info;
	VkVideoEncodeH264SessionParametersGetInfoKHR h264_get;
	VkVideoEncodeSessionParametersGetInfoKHR get_info;
	uint32_t req_count;
	uint32_t i;
	size_t ps_size;
	VkResult res;

	memset(&h264_sess, 0, sizeof(h264_sess));
	h264_sess.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_CREATE_INFO_KHR;
	h264_sess.useMaxLevelIdc = VK_FALSE;
	h264_sess.maxLevelIdc = vkenc_pick_level(enc->coded_w, enc->coded_h, config->level);

	memset(&std_header, 0, sizeof(std_header));
	snprintf(std_header.extensionName, sizeof(std_header.extensionName),
	    "%s", VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME);
	std_header.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_SPEC_VERSION;

	memset(&sess_info, 0, sizeof(sess_info));
	sess_info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
	sess_info.pNext = &h264_sess;
	sess_info.queueFamilyIndex = enc->encode_family;
	sess_info.pVideoProfile = &enc->profile;
	sess_info.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	sess_info.maxCodedExtent.width = enc->coded_w;
	sess_info.maxCodedExtent.height = enc->coded_h;
	sess_info.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	sess_info.maxDpbSlots = VKENC_DPB_SLOTS;
	sess_info.maxActiveReferencePictures = 1U;
	sess_info.pStdHeaderVersion = &std_header;

	res = enc->fn.vkCreateVideoSessionKHR(enc->device, &sess_info, NULL, &enc->session);
	if (res != VK_SUCCESS) {
		media_log("vkvideo-enc: vkCreateVideoSessionKHR failed (%d)", (int)res);
		return EIO;
	}

	req_count = VKENC_BINDINGS;
	memset(reqs, 0, sizeof(reqs));
	for (i = 0U; i < VKENC_BINDINGS; i++)
		reqs[i].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
	res = enc->fn.vkGetVideoSessionMemoryRequirementsKHR(enc->device, enc->session, &req_count, reqs);
	if (res != VK_SUCCESS || req_count == 0U) {
		media_log("vkvideo-enc: vkGetVideoSessionMemoryRequirementsKHR failed (%d, %u)", (int)res, req_count);
		return EIO;
	}

	memset(binds, 0, sizeof(binds));
	for (i = 0U; i < req_count; i++) {
		VkMemoryAllocateInfo ainfo;
		uint32_t type_idx;

		if (vkenc_find_memory_type(&enc->mem_props, reqs[i].memoryRequirements.memoryTypeBits,
		    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_idx, NULL) != 0) {
			if (vkenc_find_memory_type(&enc->mem_props, reqs[i].memoryRequirements.memoryTypeBits,
			    0U, &type_idx, NULL) != 0)
				return ENOMEM;
		}
		memset(&ainfo, 0, sizeof(ainfo));
		ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		ainfo.allocationSize = reqs[i].memoryRequirements.size;
		ainfo.memoryTypeIndex = type_idx;
		res = enc->fn.vkAllocateMemory(enc->device, &ainfo, NULL, &enc->session_mem[i]);
		if (res != VK_SUCCESS)
			return ENOMEM;
		enc->session_mem_count++;
		binds[i].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
		binds[i].memoryBindIndex = reqs[i].memoryBindIndex;
		binds[i].memory = enc->session_mem[i];
		binds[i].memoryOffset = 0;
		binds[i].memorySize = reqs[i].memoryRequirements.size;
	}
	res = enc->fn.vkBindVideoSessionMemoryKHR(enc->device, enc->session, req_count, binds);
	if (res != VK_SUCCESS) {
		media_log("vkvideo-enc: vkBindVideoSessionMemoryKHR failed (%d)", (int)res);
		return EIO;
	}

	memset(&sps, 0, sizeof(sps));
	sps.profile_idc = enc->h264_profile.stdProfileIdc;
	sps.level_idc = h264_sess.maxLevelIdc;
	sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
	sps.seq_parameter_set_id = 0U;
	sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2;
	sps.max_num_ref_frames = 1U;
	sps.log2_max_frame_num_minus4 = 0U;
	sps.pic_width_in_mbs_minus1 = (enc->coded_w / 16U) - 1U;
	sps.pic_height_in_map_units_minus1 = (enc->coded_h / 16U) - 1U;
	sps.flags.frame_mbs_only_flag = 1U;
	sps.flags.direct_8x8_inference_flag = 1U;
	if (enc->h264_profile.stdProfileIdc == STD_VIDEO_H264_PROFILE_IDC_BASELINE)
		sps.flags.constraint_set1_flag = 1U;
	if (enc->coded_w != enc->width || enc->coded_h != enc->height) {
		sps.flags.frame_cropping_flag = 1U;
		sps.frame_crop_right_offset = (enc->coded_w - enc->width) / 2U;
		sps.frame_crop_bottom_offset = (enc->coded_h - enc->height) / 2U;
	}

	memset(&pps, 0, sizeof(pps));
	pps.seq_parameter_set_id = 0U;
	pps.pic_parameter_set_id = 0U;
	pps.flags.entropy_coding_mode_flag =
		(enc->h264_profile.stdProfileIdc != STD_VIDEO_H264_PROFILE_IDC_BASELINE) ? 1U : 0U;
	pps.flags.deblocking_filter_control_present_flag = 1U;

	memset(&add_info, 0, sizeof(add_info));
	add_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
	add_info.stdSPSCount = 1U;
	add_info.pStdSPSs = &sps;
	add_info.stdPPSCount = 1U;
	add_info.pStdPPSs = &pps;

	memset(&h264_params, 0, sizeof(h264_params));
	h264_params.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
	h264_params.maxStdSPSCount = 1U;
	h264_params.maxStdPPSCount = 1U;
	h264_params.pParametersAddInfo = &add_info;

	memset(&params_info, 0, sizeof(params_info));
	params_info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
	params_info.pNext = &h264_params;
	params_info.videoSession = enc->session;

	res = enc->fn.vkCreateVideoSessionParametersKHR(enc->device, &params_info, NULL, &enc->parameters);
	if (res != VK_SUCCESS) {
		media_log("vkvideo-enc: vkCreateVideoSessionParametersKHR failed (%d)", (int)res);
		return EIO;
	}

	memset(&h264_get, 0, sizeof(h264_get));
	h264_get.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR;
	h264_get.writeStdSPS = VK_TRUE;
	h264_get.writeStdPPS = VK_TRUE;

	memset(&get_info, 0, sizeof(get_info));
	get_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR;
	get_info.pNext = &h264_get;
	get_info.videoSessionParameters = enc->parameters;

	ps_size = 0U;
	res = enc->fn.vkGetEncodedVideoSessionParametersKHR(enc->device, &get_info, NULL, &ps_size, NULL);
	if (res != VK_SUCCESS || ps_size == 0U) {
		media_log("vkvideo-enc: vkGetEncodedVideoSessionParametersKHR(size) failed (%d, %zu)", (int)res, ps_size);
		return EIO;
	}

	enc->ps_buf = (uint8_t *)malloc(ps_size);
	if (enc->ps_buf == NULL)
		return ENOMEM;
	res = enc->fn.vkGetEncodedVideoSessionParametersKHR(enc->device, &get_info, NULL, &ps_size, enc->ps_buf);
	if (res != VK_SUCCESS)
		return EIO;
	enc->ps_len = ps_size;
	return 0;
}

/*
 * Opens a Vulkan Video H.264 hardware encoder for the resolved configuration.
 */
static int
vkenc_open(
	const struct media_encoder_config *config,
	void **state)
{
	struct vkvideo_enc *enc;
	VkPhysicalDevice devs[VKENC_DEVICES];
	uint32_t dev_count;
	uint32_t i;
	int ext_caps;
	int match_uuid;
	uint8_t zero_uuid[16];
	struct vkenc_probe probe;
	VkPhysicalDevice chosen;
	VkDeviceQueueCreateInfo qinfos[2];
	uint32_t qcount;
	float prio = 1.0f;
	const char *dev_exts[6];
	uint32_t dev_ext_count;
	VkDeviceCreateInfo dinfo;
	VkCommandPoolCreateInfo pinfo;
	VkCommandBufferAllocateInfo cinfo;
	VkSemaphoreCreateInfo sem_info;
	VkFenceCreateInfo f_info;
	VkQueryPoolVideoEncodeFeedbackCreateInfoKHR fb_q;
	VkQueryPoolCreateInfo qpool_info;
	VkResult res;
	int error;

	*state = NULL;
	if (config->codec != MEDIA_CODEC_H264)
		return MEDIA_PROBLEM_CONFIG;
	if (config->rate_control != MEDIA_ENCODER_RATE_DEFAULT &&
	    config->rate_control != MEDIA_ENCODER_RATE_CQP)
		return MEDIA_PROBLEM_CONFIG;
	if (config->input != MEDIA_ENCODER_INPUT_NV12 &&
	    config->input != MEDIA_ENCODER_INPUT_GPU_NV12)
		return MEDIA_PROBLEM_CONFIG;
	if (config->b_frames != 0U)
		return MEDIA_PROBLEM_CONFIG;

	enc = (struct vkvideo_enc *)calloc(1U, sizeof(*enc));
	if (enc == NULL)
		return ENOMEM;
	enc->flight_slot = -1;

	if (vkenc_create_instance(&enc->fn, &enc->instance, &ext_caps) != 0) {
		vkenc_close(enc);
		return MEDIA_PROBLEM_DEVICE;
	}

	memset(zero_uuid, 0, sizeof(zero_uuid));
	match_uuid = (memcmp(config->device_uuid, zero_uuid, 16U) != 0 ||
	    memcmp(config->driver_uuid, zero_uuid, 16U) != 0);

	dev_count = VKENC_DEVICES;
	if (enc->fn.vkEnumeratePhysicalDevices(enc->instance, &dev_count, devs) < 0)
		dev_count = 0U;

	chosen = VK_NULL_HANDLE;
	memset(&probe, 0, sizeof(probe));
	for (i = 0U; i < dev_count; i++) {
		vkenc_probe_device(&enc->fn, devs[i], &probe);
		if (!probe.supported)
			continue;
		if (match_uuid) {
			if (memcmp(config->device_uuid, probe.device_uuid, 16U) != 0 ||
			    memcmp(config->driver_uuid, probe.driver_uuid, 16U) != 0)
				continue;
		}
		if (config->input == MEDIA_ENCODER_INPUT_GPU_NV12 && (!ext_caps || !probe.has_external_fd))
			continue;
		chosen = devs[i];
		break;
	}
	if (chosen == VK_NULL_HANDLE) {
		media_log("vkvideo-enc: no matching physical device");
		vkenc_close(enc);
		return MEDIA_PROBLEM_DEVICE;
	}

	enc->physical = chosen;
	enc->transfer_family = probe.transfer_family;
	enc->encode_family = probe.encode_family;
	enc->has_external_fd = probe.has_external_fd;
	enc->fn.vkGetPhysicalDeviceMemoryProperties(chosen, &enc->mem_props);

	enc->width = config->width;
	enc->height = config->height;
	enc->coded_w = (config->width + 15U) & ~15U;
	enc->coded_h = (config->height + 15U) & ~15U;
	if (enc->coded_w < probe.caps.minCodedExtent.width ||
	    enc->coded_h < probe.caps.minCodedExtent.height ||
	    enc->coded_w > probe.caps.maxCodedExtent.width ||
	    enc->coded_h > probe.caps.maxCodedExtent.height) {
		vkenc_close(enc);
		return MEDIA_PROBLEM_CONFIG;
	}

	enc->input = config->input;
	enc->qp_p = (config->rate_control == MEDIA_ENCODER_RATE_CQP) ? config->qp : VKENC_DEFAULT_QP;
	enc->qp_i = (enc->qp_p >= VKENC_KEY_QP_DELTA) ? (enc->qp_p - VKENC_KEY_QP_DELTA) : 0U;
	enc->key_interval = config->key_interval;

	enc->h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR;
	if (config->profile == MEDIA_ENCODER_PROFILE_H264_CONSTRAINED_BASELINE)
		enc->h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
	else if (config->profile == MEDIA_ENCODER_PROFILE_H264_HIGH)
		enc->h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
	else
		enc->h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;

	enc->profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	enc->profile.pNext = &enc->h264_profile;
	enc->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
	enc->profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	enc->profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	enc->profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

	memset(qinfos, 0, sizeof(qinfos));
	qinfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qinfos[0].queueFamilyIndex = enc->transfer_family;
	qinfos[0].queueCount = 1U;
	qinfos[0].pQueuePriorities = &prio;
	qcount = 1U;
	if (enc->encode_family != enc->transfer_family) {
		qinfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qinfos[1].queueFamilyIndex = enc->encode_family;
		qinfos[1].queueCount = 1U;
		qinfos[1].pQueuePriorities = &prio;
		qcount = 2U;
	}

	dev_exts[0] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
	dev_exts[1] = VK_KHR_VIDEO_QUEUE_EXTENSION_NAME;
	dev_exts[2] = VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME;
	dev_exts[3] = VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME;
	dev_ext_count = 4U;
	if (enc->input == MEDIA_ENCODER_INPUT_GPU_NV12) {
		dev_exts[dev_ext_count++] = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
		dev_exts[dev_ext_count++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
	}

	memset(&dinfo, 0, sizeof(dinfo));
	dinfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dinfo.queueCreateInfoCount = qcount;
	dinfo.pQueueCreateInfos = qinfos;
	dinfo.enabledExtensionCount = dev_ext_count;
	dinfo.ppEnabledExtensionNames = dev_exts;
	res = enc->fn.vkCreateDevice(chosen, &dinfo, NULL, &enc->device);
	if (res != VK_SUCCESS) {
		media_log("vkvideo-enc: vkCreateDevice failed (%d)", (int)res);
		vkenc_close(enc);
		return MEDIA_PROBLEM_DEVICE;
	}

	if (vkenc_load_device_symbols(&enc->fn, enc->device) != 0) {
		vkenc_close(enc);
		return MEDIA_PROBLEM_DEVICE;
	}

	enc->fn.vkGetDeviceQueue(enc->device, enc->transfer_family, 0U, &enc->transfer_queue);
	enc->fn.vkGetDeviceQueue(enc->device, enc->encode_family, 0U, &enc->encode_queue);

	memset(&pinfo, 0, sizeof(pinfo));
	pinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pinfo.queueFamilyIndex = enc->transfer_family;
	if (enc->fn.vkCreateCommandPool(enc->device, &pinfo, NULL, &enc->transfer_pool) != VK_SUCCESS) {
		vkenc_close(enc);
		return ENOMEM;
	}
	pinfo.queueFamilyIndex = enc->encode_family;
	if (enc->fn.vkCreateCommandPool(enc->device, &pinfo, NULL, &enc->encode_pool) != VK_SUCCESS) {
		vkenc_close(enc);
		return ENOMEM;
	}

	memset(&cinfo, 0, sizeof(cinfo));
	cinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cinfo.commandBufferCount = 1U;
	cinfo.commandPool = enc->transfer_pool;
	if (enc->fn.vkAllocateCommandBuffers(enc->device, &cinfo, &enc->transfer_cmd) != VK_SUCCESS) {
		vkenc_close(enc);
		return ENOMEM;
	}
	cinfo.commandPool = enc->encode_pool;
	if (enc->fn.vkAllocateCommandBuffers(enc->device, &cinfo, &enc->encode_cmd) != VK_SUCCESS) {
		vkenc_close(enc);
		return ENOMEM;
	}

	memset(&sem_info, 0, sizeof(sem_info));
	sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	if (enc->fn.vkCreateSemaphore(enc->device, &sem_info, NULL, &enc->transfer_done) != VK_SUCCESS) {
		vkenc_close(enc);
		return ENOMEM;
	}

	memset(&f_info, 0, sizeof(f_info));
	f_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (enc->fn.vkCreateFence(enc->device, &f_info, NULL, &enc->encode_done) != VK_SUCCESS) {
		vkenc_close(enc);
		return ENOMEM;
	}

	memset(&fb_q, 0, sizeof(fb_q));
	fb_q.sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR;
	fb_q.pNext = &enc->profile;
	fb_q.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;

	memset(&qpool_info, 0, sizeof(qpool_info));
	qpool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	qpool_info.pNext = &fb_q;
	qpool_info.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
	qpool_info.queryCount = 1U;
	if (enc->fn.vkCreateQueryPool(enc->device, &qpool_info, NULL, &enc->query_pool) != VK_SUCCESS) {
		media_log("vkvideo-enc: vkCreateQueryPool failed");
		vkenc_close(enc);
		return EIO;
	}

	error = vkenc_setup_session(enc, config);
	if (error != 0) {
		vkenc_close(enc);
		return error;
	}

	error = vkenc_create_nv12_image(enc, enc->coded_w, enc->coded_h,
	    VK_IMAGE_TILING_OPTIMAL,
	    VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	    0, &enc->src_image, &enc->src_mem, &enc->src_view, NULL, NULL);
	if (error != 0) {
		vkenc_close(enc);
		return error;
	}

	for (i = 0U; i < VKENC_DPB_SLOTS; i++) {
		error = vkenc_create_nv12_image(enc, enc->coded_w, enc->coded_h,
		    VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
		    0, &enc->dpb_image[i], &enc->dpb_mem[i], &enc->dpb_view[i], NULL, NULL);
		if (error != 0) {
			vkenc_close(enc);
			return error;
		}
	}

	error = vkenc_create_buffer(enc, VKENC_BITSTREAM_BYTES,
	    VKENC_BUFFER_USAGE_VIDEO_ENCODE_DST,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	    &enc->out_buf, &enc->out_mem, &enc->out_map, &enc->out_coherent);
	if (error != 0) {
		vkenc_close(enc);
		return error;
	}

	enc->pkt_buf = (uint8_t *)malloc(VKENC_BITSTREAM_BYTES);
	if (enc->pkt_buf == NULL) {
		vkenc_close(enc);
		return ENOMEM;
	}

	if (enc->input == MEDIA_ENCODER_INPUT_NV12) {
		VkDeviceSize st_bytes = (VkDeviceSize)enc->coded_w * (VkDeviceSize)enc->coded_h * 3U / 2U;

		error = vkenc_create_buffer(enc, st_bytes,
		    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		    &enc->staging_buf, &enc->staging_mem, &enc->staging_map, &enc->staging_coherent);
		if (error != 0) {
			vkenc_close(enc);
			return error;
		}
	} else {
		if (enc->fn.vkGetMemoryFdKHR == NULL) {
			vkenc_close(enc);
			return MEDIA_PROBLEM_DEVICE;
		}
		for (i = 0U; i < VKENC_SLOTS; i++) {
			struct vkenc_slot *s = &enc->slots[i];
			VkMemoryGetFdInfoKHR fd_info;
			VkImageSubresource sub;
			VkSubresourceLayout lay0;
			VkSubresourceLayout lay1;
			uint64_t mem_bytes;
			uint32_t mem_type;
			int fd;

			error = vkenc_create_nv12_image(enc, enc->coded_w, enc->coded_h,
			    VK_IMAGE_TILING_LINEAR,
			    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
			    1, &s->image, &s->memory, NULL, &mem_bytes, &mem_type);
			if (error != 0) {
				vkenc_close(enc);
				return error;
			}
			memset(&fd_info, 0, sizeof(fd_info));
			fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
			fd_info.memory = s->memory;
			fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
			fd = -1;
			if (enc->fn.vkGetMemoryFdKHR(enc->device, &fd_info, &fd) != VK_SUCCESS || fd < 0) {
				vkenc_close(enc);
				return EIO;
			}
			memset(&sub, 0, sizeof(sub));
			sub.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
			enc->fn.vkGetImageSubresourceLayout(enc->device, s->image, &sub, &lay0);
			sub.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
			enc->fn.vkGetImageSubresourceLayout(enc->device, s->image, &sub, &lay1);

			s->info.index = i;
			s->info.fd = fd;
			s->info.memory_bytes = mem_bytes;
			s->info.memory_type = mem_type;
			s->info.image_width = enc->coded_w;
			s->info.image_height = enc->coded_h;
			s->info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
			s->info.plane_offset[0] = lay0.offset;
			s->info.plane_offset[1] = lay1.offset;
			s->info.plane_pitch[0] = lay0.rowPitch;
			s->info.plane_pitch[1] = lay1.rowPitch;
			memcpy(s->info.device_uuid, probe.device_uuid, sizeof(s->info.device_uuid));
			memcpy(s->info.driver_uuid, probe.driver_uuid, sizeof(s->info.driver_uuid));
			enc->slot_count++;
		}
	}

	*state = enc;
	return 0;
}

/*
 * Copies CPU NV12 planes into the staging buffer with edge-replication padding.
 */
static void
vkenc_copy_cpu_nv12(
	struct vkvideo_enc *enc,
	const struct media_encoder_picture *picture)
{
	uint8_t *dst_y = enc->staging_map;
	uint8_t *dst_uv = enc->staging_map + ((size_t)enc->coded_w * enc->coded_h);
	const uint8_t *src_y = picture->plane[0];
	const uint8_t *src_uv = picture->plane[1];
	uint32_t y;

	for (y = 0U; y < enc->coded_h; y++) {
		uint32_t sy = (y < enc->height) ? y : (enc->height - 1U);
		const uint8_t *row = src_y + ((size_t)sy * picture->stride[0]);
		uint8_t *out = dst_y + ((size_t)y * enc->coded_w);

		memcpy(out, row, enc->width);
		if (enc->coded_w > enc->width)
			memset(out + enc->width, row[enc->width - 1U], enc->coded_w - enc->width);
	}
	for (y = 0U; y < enc->coded_h / 2U; y++) {
		uint32_t sy = (y < enc->height / 2U) ? y : ((enc->height / 2U) - 1U);
		const uint8_t *row = src_uv + ((size_t)sy * picture->stride[1]);
		uint8_t *out = dst_uv + ((size_t)y * enc->coded_w);
		uint32_t x;

		memcpy(out, row, enc->width);
		for (x = enc->width; x + 1U < enc->coded_w; x += 2U) {
			out[x] = row[enc->width - 2U];
			out[x + 1U] = row[enc->width - 1U];
		}
	}
	if (!enc->staging_coherent) {
		VkMappedMemoryRange range;

		memset(&range, 0, sizeof(range));
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = enc->staging_mem;
		range.size = (VkDeviceSize)enc->coded_w * enc->coded_h * 3U / 2U;
		enc->fn.vkFlushMappedMemoryRanges(enc->device, 1U, &range);
	}
}

/*
 * Records and submits one picture transfer + VDEnc/MFX encode.
 */
static int
vkenc_submit_frame(
	struct vkvideo_enc *enc,
	const struct media_encoder_picture *picture,
	int slot_index,
	int64_t time_us,
	unsigned flags)
{
	VkCommandBufferBeginInfo binfo;
	VkImageMemoryBarrier bar[2];
	VkSubmitInfo sub;
	VkPipelineStageFlags wait_stage;
	VkVideoReferenceSlotInfoKHR coding_slots[2];
	VkVideoPictureResourceInfoKHR dpb_res[2];
	VkVideoBeginCodingInfoKHR begin_info;
	VkVideoEndCodingInfoKHR end_info;
	VkVideoPictureResourceInfoKHR src_res;
	StdVideoEncodeH264PictureInfo std_pic;
	StdVideoEncodeH264SliceHeader std_slice;
	StdVideoEncodeH264ReferenceInfo std_setup_ref;
	StdVideoEncodeH264ReferenceInfo std_l0_ref;
	VkVideoEncodeH264DpbSlotInfoKHR setup_dpb_info;
	VkVideoEncodeH264DpbSlotInfoKHR l0_dpb_info;
	VkVideoEncodeH264NaluSliceInfoKHR slice_info;
	VkVideoEncodeH264PictureInfoKHR h264_pic;
	VkVideoEncodeInfoKHR enc_info;
	unsigned marks;
	int is_idr;
	VkResult res;

	marks = ((flags & MEDIA_ENCODE_FORCE_KEY) != 0U ||
	    enc->frame_num == 0U ||
	    enc->frames_since_idr >= enc->key_interval) ?
		MEDIA_ENCODING_PICTURE_KEY : 0U;
	marks = media_encoding_recovery_sent(&enc->recovery, marks);
	is_idr = ((marks & MEDIA_ENCODING_PICTURE_KEY) != 0U || !enc->has_ref) ? 1 : 0;

	if (is_idr) {
		enc->frame_num = 0U;
		enc->idr_pic_id = (enc->idr_pic_id + 1U) & 15U;
	}

	enc->fn.vkResetFences(enc->device, 1U, &enc->encode_done);
	enc->fn.vkResetCommandBuffer(enc->transfer_cmd, 0U);
	enc->fn.vkResetCommandBuffer(enc->encode_cmd, 0U);

	memset(&binfo, 0, sizeof(binfo));
	binfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	binfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (enc->fn.vkBeginCommandBuffer(enc->transfer_cmd, &binfo) != VK_SUCCESS)
		return EIO;

	if (picture != NULL) {
		VkBufferImageCopy regions[2];

		vkenc_copy_cpu_nv12(enc, picture);
		memset(&bar[0], 0, sizeof(bar[0]));
		bar[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		bar[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		bar[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bar[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		bar[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bar[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bar[0].image = enc->src_image;
		bar[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		bar[0].subresourceRange.levelCount = 1U;
		bar[0].subresourceRange.layerCount = 1U;
		enc->fn.vkCmdPipelineBarrier(enc->transfer_cmd,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    0U, 0U, NULL, 0U, NULL, 1U, &bar[0]);

		memset(regions, 0, sizeof(regions));
		regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
		regions[0].imageSubresource.layerCount = 1U;
		regions[0].imageExtent.width = enc->coded_w;
		regions[0].imageExtent.height = enc->coded_h;
		regions[0].imageExtent.depth = 1U;
		regions[1].bufferOffset = (VkDeviceSize)enc->coded_w * enc->coded_h;
		regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
		regions[1].imageSubresource.layerCount = 1U;
		regions[1].imageExtent.width = enc->coded_w / 2U;
		regions[1].imageExtent.height = enc->coded_h / 2U;
		regions[1].imageExtent.depth = 1U;
		enc->fn.vkCmdCopyBufferToImage(enc->transfer_cmd, enc->staging_buf,
		    enc->src_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2U, regions);
	} else {
		VkImageCopy regions[2];
		VkImage slot_img = enc->slots[slot_index].image;

		memset(bar, 0, sizeof(bar));
		bar[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		bar[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		bar[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		bar[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		bar[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		bar[0].dstQueueFamilyIndex = enc->transfer_family;
		bar[0].image = slot_img;
		bar[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		bar[0].subresourceRange.levelCount = 1U;
		bar[0].subresourceRange.layerCount = 1U;

		bar[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		bar[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		bar[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bar[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		bar[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bar[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bar[1].image = enc->src_image;
		bar[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		bar[1].subresourceRange.levelCount = 1U;
		bar[1].subresourceRange.layerCount = 1U;
		enc->fn.vkCmdPipelineBarrier(enc->transfer_cmd,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    0U, 0U, NULL, 0U, NULL, 2U, bar);

		memset(regions, 0, sizeof(regions));
		regions[0].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
		regions[0].srcSubresource.layerCount = 1U;
		regions[0].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
		regions[0].dstSubresource.layerCount = 1U;
		regions[0].extent.width = enc->coded_w;
		regions[0].extent.height = enc->coded_h;
		regions[0].extent.depth = 1U;
		regions[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
		regions[1].srcSubresource.layerCount = 1U;
		regions[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
		regions[1].dstSubresource.layerCount = 1U;
		regions[1].extent.width = enc->coded_w / 2U;
		regions[1].extent.height = enc->coded_h / 2U;
		regions[1].extent.depth = 1U;
		enc->fn.vkCmdCopyImage(enc->transfer_cmd,
		    slot_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    enc->src_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2U, regions);

		memset(&bar[0], 0, sizeof(bar[0]));
		bar[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		bar[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		bar[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		bar[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		bar[0].srcQueueFamilyIndex = enc->transfer_family;
		bar[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		bar[0].image = slot_img;
		bar[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		bar[0].subresourceRange.levelCount = 1U;
		bar[0].subresourceRange.layerCount = 1U;
		enc->fn.vkCmdPipelineBarrier(enc->transfer_cmd,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		    0U, 0U, NULL, 0U, NULL, 1U, &bar[0]);
	}

	memset(&bar[0], 0, sizeof(bar[0]));
	bar[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	bar[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bar[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	bar[0].newLayout = VKENC_IMAGE_LAYOUT_VIDEO_ENCODE_SRC;
	bar[0].srcQueueFamilyIndex = enc->transfer_family;
	bar[0].dstQueueFamilyIndex = enc->encode_family;
	bar[0].image = enc->src_image;
	bar[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	bar[0].subresourceRange.levelCount = 1U;
	bar[0].subresourceRange.layerCount = 1U;
	enc->fn.vkCmdPipelineBarrier(enc->transfer_cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
	    0U, 0U, NULL, 0U, NULL, 1U, &bar[0]);

	res = enc->fn.vkEndCommandBuffer(enc->transfer_cmd);
	if (res != VK_SUCCESS) {
		media_log("vkvideo-enc: transfer vkEndCommandBuffer failed (%d)", (int)res);
		return EIO;
	}

	memset(&sub, 0, sizeof(sub));
	sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	sub.commandBufferCount = 1U;
	sub.pCommandBuffers = &enc->transfer_cmd;
	sub.signalSemaphoreCount = 1U;
	sub.pSignalSemaphores = &enc->transfer_done;
	res = enc->fn.vkQueueSubmit(enc->transfer_queue, 1U, &sub, VK_NULL_HANDLE);
	if (res != VK_SUCCESS) {
		media_log("vkvideo-enc: transfer vkQueueSubmit failed (%d)", (int)res);
		return EIO;
	}

	if (enc->fn.vkBeginCommandBuffer(enc->encode_cmd, &binfo) != VK_SUCCESS)
		return EIO;

	enc->fn.vkCmdResetQueryPool(enc->encode_cmd, enc->query_pool, 0U, 1U);

	memset(dpb_res, 0, sizeof(dpb_res));
	dpb_res[0].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
	dpb_res[0].codedExtent.width = enc->coded_w;
	dpb_res[0].codedExtent.height = enc->coded_h;
	dpb_res[0].imageViewBinding = enc->dpb_view[enc->cur_dpb];

	dpb_res[1].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
	dpb_res[1].codedExtent.width = enc->coded_w;
	dpb_res[1].codedExtent.height = enc->coded_h;
	dpb_res[1].imageViewBinding = enc->dpb_view[enc->prev_dpb];

	memset(&std_setup_ref, 0, sizeof(std_setup_ref));
	std_setup_ref.FrameNum = enc->frame_num;
	std_setup_ref.PicOrderCnt = (int32_t)(enc->frame_num * 2U);

	memset(&setup_dpb_info, 0, sizeof(setup_dpb_info));
	setup_dpb_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
	setup_dpb_info.pStdReferenceInfo = &std_setup_ref;

	memset(&std_l0_ref, 0, sizeof(std_l0_ref));
	std_l0_ref.FrameNum = (enc->frame_num > 0U) ? (enc->frame_num - 1U) : 0U;
	std_l0_ref.PicOrderCnt = (int32_t)(std_l0_ref.FrameNum * 2U);

	memset(&l0_dpb_info, 0, sizeof(l0_dpb_info));
	l0_dpb_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
	l0_dpb_info.pStdReferenceInfo = &std_l0_ref;

	memset(coding_slots, 0, sizeof(coding_slots));
	coding_slots[0].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	coding_slots[0].pNext = &setup_dpb_info;
	coding_slots[0].slotIndex = (int32_t)enc->cur_dpb;
	coding_slots[0].pPictureResource = &dpb_res[0];

	coding_slots[1].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	coding_slots[1].pNext = &l0_dpb_info;
	coding_slots[1].slotIndex = (int32_t)enc->prev_dpb;
	coding_slots[1].pPictureResource = &dpb_res[1];

	memset(&begin_info, 0, sizeof(begin_info));
	begin_info.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
	begin_info.videoSession = enc->session;
	begin_info.videoSessionParameters = enc->parameters;
	begin_info.referenceSlotCount = is_idr ? 1U : 2U;
	begin_info.pReferenceSlots = coding_slots;
	enc->fn.vkCmdBeginVideoCodingKHR(enc->encode_cmd, &begin_info);
	if (!enc->session_reset) {
		VkVideoCodingControlInfoKHR ctrl_info;

		memset(&ctrl_info, 0, sizeof(ctrl_info));
		ctrl_info.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
		ctrl_info.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
		enc->fn.vkCmdControlVideoCodingKHR(enc->encode_cmd, &ctrl_info);
		enc->session_reset = 1;
	}

	enc->fn.vkCmdBeginQuery(enc->encode_cmd, enc->query_pool, 0U, 0U);

	memset(&src_res, 0, sizeof(src_res));
	src_res.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
	src_res.codedExtent.width = enc->coded_w;
	src_res.codedExtent.height = enc->coded_h;
	src_res.imageViewBinding = enc->src_view;

	memset(&std_pic, 0, sizeof(std_pic));
	std_pic.primary_pic_type = is_idr ?
		STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
	std_pic.frame_num = enc->frame_num;
	std_pic.idr_pic_id = (uint16_t)enc->idr_pic_id;
	std_pic.PicOrderCnt = (int32_t)(enc->frame_num * 2U);
	std_pic.flags.IdrPicFlag = is_idr ? 1U : 0U;
	std_pic.flags.is_reference = 1U;

	memset(&std_slice, 0, sizeof(std_slice));
	std_slice.slice_type = is_idr ?
		STD_VIDEO_H264_SLICE_TYPE_I : STD_VIDEO_H264_SLICE_TYPE_P;

	memset(&slice_info, 0, sizeof(slice_info));
	slice_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR;
	slice_info.constantQp = (int32_t)(is_idr ? enc->qp_i : enc->qp_p);
	slice_info.pStdSliceHeader = &std_slice;

	memset(&h264_pic, 0, sizeof(h264_pic));
	h264_pic.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR;
	h264_pic.naluSliceEntryCount = 1U;
	h264_pic.pNaluSliceEntries = &slice_info;
	h264_pic.pStdPictureInfo = &std_pic;

	memset(&enc_info, 0, sizeof(enc_info));
	enc_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;
	enc_info.pNext = &h264_pic;
	enc_info.dstBuffer = enc->out_buf;
	enc_info.dstBufferOffset = 0;
	enc_info.dstBufferRange = VKENC_BITSTREAM_BYTES;
	enc_info.srcPictureResource = src_res;
	enc_info.pSetupReferenceSlot = &coding_slots[0];
	enc_info.referenceSlotCount = is_idr ? 0U : 1U;
	enc_info.pReferenceSlots = is_idr ? NULL : &coding_slots[1];
	enc->fn.vkCmdEncodeVideoKHR(enc->encode_cmd, &enc_info);

	enc->fn.vkCmdEndQuery(enc->encode_cmd, enc->query_pool, 0U);

	memset(&end_info, 0, sizeof(end_info));
	end_info.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
	enc->fn.vkCmdEndVideoCodingKHR(enc->encode_cmd, &end_info);

	res = enc->fn.vkEndCommandBuffer(enc->encode_cmd);
	if (res != VK_SUCCESS) {
		media_log("vkvideo-enc: encode vkEndCommandBuffer failed (%d)", (int)res);
		return EIO;
	}

	wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	memset(&sub, 0, sizeof(sub));
	sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	sub.waitSemaphoreCount = 1U;
	sub.pWaitSemaphores = &enc->transfer_done;
	sub.pWaitDstStageMask = &wait_stage;
	sub.commandBufferCount = 1U;
	sub.pCommandBuffers = &enc->encode_cmd;
	res = enc->fn.vkQueueSubmit(enc->encode_queue, 1U, &sub, enc->encode_done);
	if (res != VK_SUCCESS) {
		media_log("vkvideo-enc: encode vkQueueSubmit failed (%d)", (int)res);
		return EIO;
	}

	enc->in_flight = 1;
	enc->flight_key = is_idr;
	enc->flight_time_us = time_us;
	enc->flight_marks = marks;
	enc->flight_slot = slot_index;
	enc->prev_dpb = enc->cur_dpb;
	enc->cur_dpb ^= 1U;
	enc->has_ref = 1;
	enc->frame_num = (enc->frame_num + 1U) & 15U;
	enc->frames_since_idr = is_idr ? 1U : (enc->frames_since_idr + 1U);
	return 0;
}

/*
 * Sends a CPU NV12 picture or drain request.
 */
static int
vkenc_send(
	void *state,
	const struct media_encoder_picture *picture,
	int64_t time_us,
	unsigned flags)
{
	struct vkvideo_enc *enc = state;

	if (enc->lost)
		return MEDIA_PROBLEM_LOST;
	if (picture == NULL) {
		enc->draining = 1;
		return 0;
	}
	if (enc->input != MEDIA_ENCODER_INPUT_NV12)
		return EINVAL;
	if (enc->in_flight)
		return EAGAIN;
	return vkenc_submit_frame(enc, picture, -1, time_us, flags);
}

/*
 * Describes the LINEAR NV12 GPU input slots.
 */
static int
vkenc_slots(
	void *state,
	struct media_encoder_slot *slots,
	unsigned capacity,
	unsigned *count)
{
	struct vkvideo_enc *enc = state;
	uint32_t i;

	if (enc->lost)
		return MEDIA_PROBLEM_LOST;
	if (enc->input != MEDIA_ENCODER_INPUT_GPU_NV12)
		return EINVAL;
	*count = enc->slot_count;
	for (i = 0U; i < enc->slot_count && i < capacity; i++)
		slots[i] = enc->slots[i].info;
	return 0;
}

/*
 * Acquires a free GPU input slot.
 */
static int
vkenc_acquire(
	void *state,
	unsigned *index)
{
	struct vkvideo_enc *enc = state;
	uint32_t i;

	if (enc->lost)
		return MEDIA_PROBLEM_LOST;
	if (enc->input != MEDIA_ENCODER_INPUT_GPU_NV12)
		return EINVAL;
	for (i = 0U; i < enc->slot_count; i++) {
		if (!enc->slots[i].acquired) {
			enc->slots[i].acquired = 1;
			*index = i;
			return 0;
		}
	}
	return EAGAIN;
}

/*
 * Encodes or discards an acquired GPU input slot.
 */
static int
vkenc_send_slot(
	void *state,
	unsigned index,
	int64_t time_us,
	unsigned flags)
{
	struct vkvideo_enc *enc = state;

	if (enc->lost)
		return MEDIA_PROBLEM_LOST;
	if (enc->input != MEDIA_ENCODER_INPUT_GPU_NV12 || index >= enc->slot_count ||
	    !enc->slots[index].acquired)
		return EINVAL;
	if ((flags & MEDIA_ENCODE_DISCARD) != 0U) {
		enc->slots[index].acquired = 0;
		return 0;
	}
	if (enc->in_flight)
		return EAGAIN;
	return vkenc_submit_frame(enc, NULL, (int)index, time_us, flags);
}

/*
 * Waits for and returns the encoded Annex-B slice packet for the picture in flight.
 */
static int
vkenc_receive(
	void *state,
	unsigned flags,
	struct media_encoded *packet)
{
	struct vkvideo_enc *enc = state;
	uint64_t timeout;
	uint32_t qdata[2];
	uint32_t bytes_written;
	unsigned outcome;
	VkResult res;

	if (enc->lost)
		return MEDIA_PROBLEM_LOST;
	if (!enc->in_flight)
		return enc->draining ? MEDIA_ENCODER_DRAINED : EAGAIN;

	timeout = ((flags & MEDIA_RECEIVE_NOWAIT) != 0U) ? 0ULL : VKENC_WAIT_NS;
	res = enc->fn.vkWaitForFences(enc->device, 1U, &enc->encode_done, VK_TRUE, timeout);
	if (res == VK_TIMEOUT)
		return EAGAIN;

	if (enc->flight_slot >= 0) {
		enc->slots[enc->flight_slot].acquired = 0;
		enc->flight_slot = -1;
	}
	enc->in_flight = 0;

	memset(qdata, 0, sizeof(qdata));
	if (res == VK_SUCCESS) {
		res = enc->fn.vkGetQueryPoolResults(enc->device, enc->query_pool, 0U, 1U,
		    sizeof(qdata), qdata, sizeof(qdata), VK_QUERY_RESULT_WAIT_BIT);
	}
	bytes_written = qdata[0];
	outcome = media_encoding_recovery_done(&enc->recovery, enc->flight_marks,
	    (res != VK_SUCCESS || bytes_written == 0U || bytes_written > VKENC_BITSTREAM_BYTES));
	if (outcome == MEDIA_ENCODING_LOST) {
		enc->lost = 1;
		return MEDIA_PROBLEM_LOST;
	}
	if (outcome == MEDIA_ENCODING_NO_PACKET) {
		enc->has_ref = 0;
		return enc->draining ? MEDIA_ENCODER_DRAINED : EAGAIN;
	}

	if (!enc->out_coherent) {
		VkMappedMemoryRange range;

		memset(&range, 0, sizeof(range));
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = enc->out_mem;
		range.size = VK_WHOLE_SIZE;
		enc->fn.vkInvalidateMappedMemoryRanges(enc->device, 1U, &range);
	}

	memcpy(enc->pkt_buf, enc->out_map, bytes_written);
	memset(packet, 0, sizeof(*packet));
	packet->data = enc->pkt_buf;
	packet->size = bytes_written;
	packet->time_us = enc->flight_time_us;
	packet->decode_time_us = enc->flight_time_us;
	packet->flags = enc->flight_key ? MEDIA_ENCODED_KEY : 0U;
	return 0;
}

/*
 * Returns the Annex-B SPS and PPS bytes generated by the device.
 */
static int
vkenc_parameter_sets(
	const void *state,
	const uint8_t **data,
	size_t *size)
{
	const struct vkvideo_enc *enc = state;

	*data = enc->ps_buf;
	*size = enc->ps_len;
	return 0;
}

/*
 * Copies the failure and recovery statistics.
 */
static void
vkenc_stats(
	const void *state,
	struct media_encoder_stats *stats)
{
	const struct vkvideo_enc *enc = state;

	stats->failed = enc->recovery.failed;
	stats->dropped = enc->recovery.dropped;
	stats->recovered = enc->recovery.recovered;
}

/*
 * Waits for idle and releases all Vulkan resources owned by the encoder.
 */
static void
vkenc_close(void *state)
{
	struct vkvideo_enc *enc = state;
	uint32_t i;

	if (enc == NULL)
		return;
	if (enc->device != VK_NULL_HANDLE) {
		if (enc->fn.vkDeviceWaitIdle != NULL)
			enc->fn.vkDeviceWaitIdle(enc->device);
		for (i = 0U; i < enc->slot_count; i++) {
			if (enc->slots[i].info.fd >= 0)
				close(enc->slots[i].info.fd);
			if (enc->slots[i].image != VK_NULL_HANDLE)
				enc->fn.vkDestroyImage(enc->device, enc->slots[i].image, NULL);
			if (enc->slots[i].memory != VK_NULL_HANDLE)
				enc->fn.vkFreeMemory(enc->device, enc->slots[i].memory, NULL);
		}
		if (enc->staging_map != NULL)
			enc->fn.vkUnmapMemory(enc->device, enc->staging_mem);
		if (enc->staging_buf != VK_NULL_HANDLE)
			enc->fn.vkDestroyBuffer(enc->device, enc->staging_buf, NULL);
		if (enc->staging_mem != VK_NULL_HANDLE)
			enc->fn.vkFreeMemory(enc->device, enc->staging_mem, NULL);
		if (enc->out_map != NULL)
			enc->fn.vkUnmapMemory(enc->device, enc->out_mem);
		if (enc->out_buf != VK_NULL_HANDLE)
			enc->fn.vkDestroyBuffer(enc->device, enc->out_buf, NULL);
		if (enc->out_mem != VK_NULL_HANDLE)
			enc->fn.vkFreeMemory(enc->device, enc->out_mem, NULL);
		for (i = 0U; i < VKENC_DPB_SLOTS; i++) {
			if (enc->dpb_view[i] != VK_NULL_HANDLE)
				enc->fn.vkDestroyImageView(enc->device, enc->dpb_view[i], NULL);
			if (enc->dpb_image[i] != VK_NULL_HANDLE)
				enc->fn.vkDestroyImage(enc->device, enc->dpb_image[i], NULL);
			if (enc->dpb_mem[i] != VK_NULL_HANDLE)
				enc->fn.vkFreeMemory(enc->device, enc->dpb_mem[i], NULL);
		}
		if (enc->src_view != VK_NULL_HANDLE)
			enc->fn.vkDestroyImageView(enc->device, enc->src_view, NULL);
		if (enc->src_image != VK_NULL_HANDLE)
			enc->fn.vkDestroyImage(enc->device, enc->src_image, NULL);
		if (enc->src_mem != VK_NULL_HANDLE)
			enc->fn.vkFreeMemory(enc->device, enc->src_mem, NULL);
		if (enc->parameters != VK_NULL_HANDLE)
			enc->fn.vkDestroyVideoSessionParametersKHR(enc->device, enc->parameters, NULL);
		if (enc->session != VK_NULL_HANDLE)
			enc->fn.vkDestroyVideoSessionKHR(enc->device, enc->session, NULL);
		for (i = 0U; i < enc->session_mem_count; i++) {
			if (enc->session_mem[i] != VK_NULL_HANDLE)
				enc->fn.vkFreeMemory(enc->device, enc->session_mem[i], NULL);
		}
		if (enc->query_pool != VK_NULL_HANDLE)
			enc->fn.vkDestroyQueryPool(enc->device, enc->query_pool, NULL);
		if (enc->encode_done != VK_NULL_HANDLE)
			enc->fn.vkDestroyFence(enc->device, enc->encode_done, NULL);
		if (enc->transfer_done != VK_NULL_HANDLE)
			enc->fn.vkDestroySemaphore(enc->device, enc->transfer_done, NULL);
		if (enc->encode_pool != VK_NULL_HANDLE)
			enc->fn.vkDestroyCommandPool(enc->device, enc->encode_pool, NULL);
		if (enc->transfer_pool != VK_NULL_HANDLE)
			enc->fn.vkDestroyCommandPool(enc->device, enc->transfer_pool, NULL);
		enc->fn.vkDestroyDevice(enc->device, NULL);
	}
	if (enc->instance != VK_NULL_HANDLE && enc->fn.vkDestroyInstance != NULL)
		enc->fn.vkDestroyInstance(enc->instance, NULL);
	free(enc->ps_buf);
	free(enc->pkt_buf);
	free(enc);
}
