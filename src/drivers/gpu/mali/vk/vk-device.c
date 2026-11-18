/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's instance, its one physical device (the Mali-G310) with
 * its properties, features, memory, queue family and formats, the
 * logical device and its queue.
 *
 * Instances, devices and queues hold nothing of their own: their
 * identities are only remembered, so a later command naming them is
 * known.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/mali/vk/vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The queue families (one: graphics, compute and transfer) and the timelines a queue may bind. */
#define DEVICE_FAMILIES		1U
#define DEVICE_TIMELINES	64U

/*
 * The vendor and device numbers the device reports: Arm's PCI vendor
 * number and the GPU's product word (arch 10, product 4).
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/panvk_physical_device.c (the ARM
 * vendor id 0x13b5).
 */
#define DEVICE_VENDOR		0x13b5U
#define DEVICE_ID		0xa4000000U

/* A token every remembered identity stands for. */
static int remembered_token;

static int create_instance(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int enumerate(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void fill_properties(VkPhysicalDeviceProperties *record);
static int properties(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int identity(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void supported_features(VkPhysicalDeviceFeatures *record);
static int features(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int memory_properties(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int families(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int format(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int image_format(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_device(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_queue(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy(struct mali_vk_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int wait_idle(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int token_destroy(struct mali_vk_session *session, void *payload);
static int remember(struct mali_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
static void set_float(float *destination, uint32_t bits);

/* Runs the instance, physical device, device and queue commands. */
int
mali_vk_device_route(
	struct mali_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_INSTANCE:
		error = create_instance(session, reader, reply);
		break;
	case GPU_OP_DESTROY_INSTANCE:
		error = destroy(session, I915_VK_OBJ_INSTANCE, reader);
		break;
	case GPU_OP_ENUMERATE_PHYSICAL_DEVICES:
		error = enumerate(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FEATURES:
		error = features(reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES:
		error = format(reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES:
		error = image_format(reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES:
		error = properties(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2:
		error = identity(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES:
		error = families(reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES:
		error = memory_properties(reader, reply);
		break;
	case GPU_OP_CREATE_DEVICE:
		error = create_device(session, reader, reply);
		break;
	case GPU_OP_DESTROY_DEVICE:
		error = destroy(session, I915_VK_OBJ_DEVICE, reader);
		break;
	case GPU_OP_QUEUE_WAIT_IDLE:
	case GPU_OP_DEVICE_WAIT_IDLE:
		error = wait_idle(reader, reply);
		break;
	case GPU_OP_GET_DEVICE_QUEUE2:
		error = device_queue(session, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/* vkCreateInstance: [present][info][allocator][present][identity] -> [result][present][identity]. */
static int
create_instance(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkInstanceCreateInfo info;
	uint64_t present;
	uint64_t created;
	int error;

	/* The info, read to its end and not otherwise used. */
	kern_memset(&info, 0, sizeof(info));
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkInstanceCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The identity is remembered. */
	error = remember(session, I915_VK_OBJ_INSTANCE, created);
	if (error != 0)
		return error;

	/* Succeeded: the reply. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, created);
	return 0;
}

/* vkEnumeratePhysicalDevices: one device; its identity is echoed and remembered. */
static int
enumerate(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t array_count;
	uint64_t device;
	uint64_t index;
	uint32_t count;

	/* The instance, the count's presence, the count and the array's count. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	array_count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (array_count > 1U || (array_count != 0U && count != 1U))
		return EINVAL;

	/* One device, and the identities given for it. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, 1);
	drv_i915_wire_reply_u64(reply, array_count);
	for (index = 0; index < array_count; index++) {
		device = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;
		(void)remember(session, I915_VK_OBJ_PHYSICAL_DEVICE, device);
		drv_i915_wire_reply_u64(reply, device);
	}

	/* Succeeded: the device is named. */
	return 0;
}

/*
 * Fills the device's properties: Vulkan 1.1, the GPU's names, and the
 * limits of what the executor takes.
 */
static void
fill_properties(
	VkPhysicalDeviceProperties *record)
{
	VkPhysicalDeviceLimits *limit;
	static const char name[] = "Mali-G310 (zedBSD)";

	/* The identity. */
	kern_memset(record, 0, sizeof(*record));
	record->apiVersion = (1U << 22) | (1U << 12);
	record->driverVersion = 1U;
	record->vendorID = DEVICE_VENDOR;
	record->deviceID = DEVICE_ID;
	record->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
	kern_memcpy(record->deviceName, name, sizeof(name));

	/* The image, buffer and binding limits. */
	limit = &record->limits;
	limit->maxImageDimension1D = MALI_VK_IMAGE_EDGE;
	limit->maxImageDimension2D = MALI_VK_IMAGE_EDGE;
	limit->maxImageDimension3D = 1;
	limit->maxImageDimensionCube = MALI_VK_IMAGE_EDGE;
	limit->maxImageArrayLayers = MALI_VK_IMAGE_LAYERS;
	limit->maxTexelBufferElements = 65536;
	limit->maxUniformBufferRange = 65536;
	limit->maxStorageBufferRange = 1U << 27;
	limit->maxPushConstantsSize = MALI_VK_PUSH_BYTES;
	limit->maxMemoryAllocationCount = MALI_RENDER_RESOURCES;
	limit->maxSamplerAllocationCount = 4000;
	limit->bufferImageGranularity = MALI_VK_IMAGE_ALIGN;
	limit->sparseAddressSpaceSize = 0;
	limit->maxBoundDescriptorSets = MALI_VK_SETS_MAX;
	limit->maxPerStageDescriptorSamplers = MALI_SHADER_TEXTURES;
	limit->maxPerStageDescriptorUniformBuffers = MALI_SHADER_BUFFERS;
	limit->maxPerStageDescriptorStorageBuffers = MALI_SHADER_BUFFERS;
	limit->maxPerStageDescriptorSampledImages = MALI_SHADER_TEXTURES;
	limit->maxPerStageDescriptorStorageImages = MALI_SHADER_IMAGES;
	limit->maxPerStageDescriptorInputAttachments = 0;
	limit->maxPerStageResources = 2U * MALI_SHADER_BUFFERS + MALI_SHADER_TEXTURES + MALI_SHADER_IMAGES;
	limit->maxDescriptorSetSamplers = MALI_SHADER_TEXTURES;
	limit->maxDescriptorSetUniformBuffers = MALI_SHADER_BUFFERS;
	limit->maxDescriptorSetUniformBuffersDynamic = 0;
	limit->maxDescriptorSetStorageBuffers = MALI_SHADER_BUFFERS;
	limit->maxDescriptorSetStorageBuffersDynamic = 0;
	limit->maxDescriptorSetSampledImages = MALI_SHADER_TEXTURES;
	limit->maxDescriptorSetStorageImages = MALI_SHADER_IMAGES;
	limit->maxDescriptorSetInputAttachments = 0;

	/* The vertex, fragment and compute limits. */
	limit->maxVertexInputAttributes = MALI_VK_VERTEX_ATTRIBUTES_MAX;
	limit->maxVertexInputBindings = MALI_VK_VERTEX_BINDINGS_MAX;
	limit->maxVertexInputAttributeOffset = 2047;
	limit->maxVertexInputBindingStride = 2048;
	limit->maxVertexOutputComponents = 4U * MALI_SHADER_LOCATIONS;
	limit->maxFragmentInputComponents = 4U * MALI_SHADER_LOCATIONS;
	limit->maxFragmentOutputAttachments = MALI_VK_ATTACHMENTS_MAX;
	limit->maxFragmentCombinedOutputResources = MALI_VK_ATTACHMENTS_MAX;
	limit->maxComputeSharedMemorySize = 16384;
	limit->maxComputeWorkGroupCount[0] = 65535;
	limit->maxComputeWorkGroupCount[1] = 65535;
	limit->maxComputeWorkGroupCount[2] = 65535;
	limit->maxComputeWorkGroupInvocations = 256;
	limit->maxComputeWorkGroupSize[0] = 256;
	limit->maxComputeWorkGroupSize[1] = 256;
	limit->maxComputeWorkGroupSize[2] = 64;
	limit->subPixelPrecisionBits = 8;
	limit->subTexelPrecisionBits = 8;
	limit->mipmapPrecisionBits = 8;
	limit->maxDrawIndexedIndexValue = 0xffffffffU;
	limit->maxDrawIndirectCount = 1;
	set_float(&limit->maxSamplerLodBias, 0x41000000U);
	set_float(&limit->maxSamplerAnisotropy, 0x3f800000U);

	/* The viewport and framebuffer limits. */
	limit->maxViewports = 1;
	limit->maxViewportDimensions[0] = MALI_VK_IMAGE_EDGE;
	limit->maxViewportDimensions[1] = MALI_VK_IMAGE_EDGE;
	set_float(&limit->viewportBoundsRange[0], 0xc6800000U);
	set_float(&limit->viewportBoundsRange[1], 0x467fff00U);
	limit->minMemoryMapAlignment = 4096;
	limit->minTexelBufferOffsetAlignment = 64;
	limit->minUniformBufferOffsetAlignment = 64;
	limit->minStorageBufferOffsetAlignment = 64;
	limit->maxFramebufferWidth = MALI_VK_IMAGE_EDGE;
	limit->maxFramebufferHeight = MALI_VK_IMAGE_EDGE;
	limit->maxFramebufferLayers = 1;
	limit->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limit->framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limit->framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limit->framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limit->maxColorAttachments = MALI_VK_ATTACHMENTS_MAX;
	limit->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limit->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limit->sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limit->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limit->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limit->maxSampleMaskWords = 1;
	limit->timestampComputeAndGraphics = VK_FALSE;
	set_float(&limit->timestampPeriod, 0x3f800000U);
	limit->maxClipDistances = 0;
	limit->maxCullDistances = 0;
	limit->discreteQueuePriorities = 2;
	set_float(&limit->pointSizeRange[0], 0x3f800000U);
	set_float(&limit->pointSizeRange[1], 0x42800000U);
	set_float(&limit->lineWidthRange[0], 0x3f800000U);
	set_float(&limit->lineWidthRange[1], 0x3f800000U);
	set_float(&limit->pointSizeGranularity, 0x3d800000U);
	set_float(&limit->lineWidthGranularity, 0x00000000U);
	limit->strictLines = VK_FALSE;
	limit->standardSampleLocations = VK_TRUE;
	limit->optimalBufferCopyOffsetAlignment = 64;
	limit->optimalBufferCopyRowPitchAlignment = MALI_VK_ROW_ALIGN;
	limit->nonCoherentAtomSize = 64;
}

/* vkGetPhysicalDeviceProperties: [device][present] -> [present][properties]. */
static int
properties(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceProperties *record;

	/* The device and the output's presence. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The record in the scratch. */
	record = mali_vk_arena_take(session, sizeof(*record));
	if (record == NULL)
		return ENOMEM;
	fill_properties(record);

	/* Succeeded: the reply. */
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkPhysicalDeviceProperties(reply, record);
	return 0;
}

/*
 * vkGetPhysicalDeviceProperties2 with only the ID properties chained:
 * the device's and the driver's UUIDs, an invalid LUID, and the
 * properties.
 */
static int
identity(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	static const uint8_t device_uuid[VK_UUID_SIZE] = {
		'z', 'e', 'd', 'B', 'S', 'D', '-', 'm', 'a', 'l', 'i', 'G', '3', '1', '0', 'v'
	};
	static const uint8_t driver_uuid[VK_UUID_SIZE] = {
		'z', 'e', 'd', 'B', 'S', 'D', '-', 'm', 'a', 'l', 'i', '-', 'v', 'k', '0', '1'
	};
	static const uint8_t luid[VK_LUID_SIZE] = { 0 };
	VkPhysicalDeviceProperties *record;
	uint64_t present;
	uint64_t chained;
	uint64_t last;
	uint32_t outer;
	uint32_t inner;

	/* The device, then the two headers. */
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	outer = drv_i915_wire_read_u32(reader);
	chained = drv_i915_wire_read_u64(reader);
	inner = drv_i915_wire_read_u32(reader);
	last = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	if (outer != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 || chained != 1U || inner != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES || last != 0U)
		return ENOTSUP;

	/* The properties in the scratch. */
	record = mali_vk_arena_take(session, sizeof(*record));
	if (record == NULL)
		return ENOMEM;
	fill_properties(record);

	/* The headers, the identifiers and the properties. */
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
	drv_i915_wire_reply_u64(reply, 0);
	drv_i915_wire_reply_u64(reply, VK_UUID_SIZE);
	i915_vkc_reply_bytes(reply, device_uuid, VK_UUID_SIZE);
	drv_i915_wire_reply_u64(reply, VK_UUID_SIZE);
	i915_vkc_reply_bytes(reply, driver_uuid, VK_UUID_SIZE);
	drv_i915_wire_reply_u64(reply, VK_LUID_SIZE);
	i915_vkc_reply_bytes(reply, luid, VK_LUID_SIZE);
	drv_i915_wire_reply_u32(reply, 0);
	drv_i915_wire_reply_u32(reply, VK_FALSE);
	i915_vkc_enc_VkPhysicalDeviceProperties(reply, record);

	/* Succeeded: the reply is written. */
	return 0;
}

/*
 * Lists the optional features the executor has: cube array images
 * (the compiler samples them and the views take them).
 */
static void
supported_features(
	VkPhysicalDeviceFeatures *record)
{
	/* Every other optional feature off. */
	kern_memset(record, 0, sizeof(*record));
	record->imageCubeArray = VK_TRUE;
}

/* vkGetPhysicalDeviceFeatures: [device][present] -> [present][features]: the optional ones supported_features() lists. */
static int
features(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceFeatures record;

	/* The device and the output's presence. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The optional features the executor has. */
	supported_features(&record);

	/* Succeeded: the reply. */
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkPhysicalDeviceFeatures(reply, &record);
	return 0;
}

/*
 * vkGetPhysicalDeviceMemoryProperties: one heap of system memory; type 0
 * device local, type 1 also host visible and coherent (every view of the
 * memory is uncached).
 */
static int
memory_properties(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceMemoryProperties record;

	/* The device, the presence and the two extents. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The two types of the one heap. */
	kern_memset(&record, 0, sizeof(record));
	record.memoryTypeCount = MALI_VK_MEMORY_TYPES;
	record.memoryTypes[MALI_VK_MEMORY_DEVICE].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	record.memoryTypes[MALI_VK_MEMORY_DEVICE].heapIndex = 0;
	record.memoryTypes[MALI_VK_MEMORY_HOST].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	record.memoryTypes[MALI_VK_MEMORY_HOST].heapIndex = 0;
	record.memoryHeapCount = 1;
	record.memoryHeaps[0].size = 1ULL << 30;
	record.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

	/* Succeeded: the reply. */
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkPhysicalDeviceMemoryProperties(reply, &record);
	return 0;
}

/* vkGetPhysicalDeviceQueueFamilyProperties: one family of one queue for graphics, compute and transfer. */
static int
families(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkQueueFamilyProperties family;
	uint64_t array_count;
	uint64_t index;

	/* The device, the presence, the count and the array's count. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	array_count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || array_count > DEVICE_FAMILIES)
		return EINVAL;

	/* The family. */
	kern_memset(&family, 0, sizeof(family));
	family.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
	family.queueCount = 1;
	family.timestampValidBits = 0;
	family.minImageTransferGranularity.width = 1;
	family.minImageTransferGranularity.height = 1;
	family.minImageTransferGranularity.depth = 1;

	/* The count, then the family as often as asked. */
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, DEVICE_FAMILIES);
	drv_i915_wire_reply_u64(reply, array_count);
	for (index = 0; index < array_count; index++)
		i915_vkc_enc_VkQueueFamilyProperties(reply, &family);

	/* Succeeded: the families are listed. */
	return 0;
}

/* vkGetPhysicalDeviceFormatProperties: the same features linear and optimal (every image is linear). */
static int
format(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFormatProperties record;
	const struct mali_vk_format *entry;
	uint32_t vk_format;

	/* The device, the format and the presence. */
	(void)drv_i915_wire_read_u64(reader);
	vk_format = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The features of a format the executor takes. */
	kern_memset(&record, 0, sizeof(record));
	entry = mali_vk_format_find(vk_format);
	record.linearTilingFeatures = mali_vk_format_features(entry, true);
	record.optimalTilingFeatures = record.linearTilingFeatures;
	record.bufferFeatures = mali_vk_format_features(entry, false);

	/* Succeeded: the reply. */
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkFormatProperties(reply, &record);
	return 0;
}

/*
 * vkGetPhysicalDeviceImageFormatProperties: 2D images of one sample of
 * a taken format, of levels down to one texel and of layers (cube
 * compatible ones too).
 */
static int
image_format(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageFormatProperties record;
	const struct mali_vk_format *entry;
	uint32_t vk_format;
	uint32_t type;
	uint32_t usage;
	uint32_t flags;
	uint32_t supported;
	uint32_t result;

	/* The device, the format, the type, the tiling, the usage, the flags and the presence. */
	(void)drv_i915_wire_read_u64(reader);
	vk_format = drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	usage = drv_i915_wire_read_u32(reader);
	flags = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The usages the format's features allow. */
	entry = mali_vk_format_find(vk_format);
	supported = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (entry != NULL && entry->sample)
		supported |= VK_IMAGE_USAGE_SAMPLED_BIT;
	if (entry != NULL && entry->render)
		supported |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (entry != NULL && entry->storage)
		supported |= VK_IMAGE_USAGE_STORAGE_BIT;
	if (entry != NULL && mali_surface_zs_find(vk_format) != NULL)
		supported |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	/* Refuses another type, another flag, an unknown format or a usage it does not allow. */
	result = (uint32_t)VK_SUCCESS;
	if (entry == NULL || type != VK_IMAGE_TYPE_2D || usage == 0U || (usage & ~supported) != 0U)
		result = (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((flags & ~(uint32_t)(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) != 0U)
		result = (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The limits. */
	kern_memset(&record, 0, sizeof(record));
	if (result == (uint32_t)VK_SUCCESS) {
		record.maxExtent.width = MALI_VK_IMAGE_EDGE;
		record.maxExtent.height = MALI_VK_IMAGE_EDGE;
		record.maxExtent.depth = 1;
		record.maxMipLevels = mali_surface_level_count(MALI_VK_IMAGE_EDGE, MALI_VK_IMAGE_EDGE);
		record.maxArrayLayers = MALI_VK_IMAGE_LAYERS;
		record.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
		record.maxResourceSize = MALI_RENDER_MAX_BYTES;
	}

	/* Succeeded: the reply. */
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkImageFormatProperties(reply, &record);
	return 0;
}

/* vkCreateDevice: one queue of the family, no optional feature. */
static int
create_device(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDeviceCreateInfo info;
	VkPhysicalDeviceFeatures none;
	const VkBool32 *asked;
	const VkBool32 *offered;
	uint64_t present;
	uint64_t created;
	uint32_t index;
	int different;
	int error;

	/* The physical device, then the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkDeviceCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* One queue of the one family. */
	for (index = 0; index < info.queueCreateInfoCount && info.pQueueCreateInfos != NULL; index++) {
		if (info.pQueueCreateInfos[index].queueFamilyIndex >= DEVICE_FAMILIES || info.pQueueCreateInfos[index].queueCount != 1U) {
			drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
			return 0;
		}
	}

	/* Only optional features the executor has (every member of the record is a VkBool32). */
	if (info.pEnabledFeatures != NULL) {
		supported_features(&none);
		asked = (const VkBool32 *)(const void *)info.pEnabledFeatures;
		offered = (const VkBool32 *)(const void *)&none;
		different = 0;
		for (index = 0; index < sizeof(none) / sizeof(VkBool32); index++) {
			if (asked[index] != VK_FALSE && offered[index] == VK_FALSE)
				different = 1;
		}
		if (different != 0) {
			drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT);
			return 0;
		}
	}

	/* The identity is remembered. */
	error = remember(session, I915_VK_OBJ_DEVICE, created);
	if (error != 0)
		return error;

	/* Succeeded: the reply. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, created);
	return 0;
}

/*
 * vkGetDeviceQueue2 with the queue's timeline record: the queue is
 * remembered and its timeline marked the session's.
 */
static int
device_queue(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t queue;
	uint32_t timeline;
	uint32_t family;
	uint32_t index;

	/* The device, the info's presence, its type, the chain's presence, the record's type and chain, the timeline. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	timeline = drv_i915_wire_read_u32(reader);

	/* The flags, the family, the index, the output's presence and the queue. */
	(void)drv_i915_wire_read_u32(reader);
	family = drv_i915_wire_read_u32(reader);
	index = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	queue = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (family >= DEVICE_FAMILIES || index != 0U || timeline == 0U || timeline >= DEVICE_TIMELINES)
		return EINVAL;

	/* The queue and its timeline. */
	(void)remember(session, I915_VK_OBJ_QUEUE, queue);
	__atomic_fetch_or(&session->timelines, 1ULL << timeline, __ATOMIC_RELEASE);

	/* Succeeded: the reply. */
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, queue);
	return 0;
}

/* vkDestroyInstance, vkDestroyDevice: [identity][allocator]. */
static int
destroy(
	struct mali_vk_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader)
{
	uint64_t gone;

	/* The identity and the allocator. */
	gone = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Forgotten. */
	(void)mali_vk_object_remove(session, kind, gone);
	return 0;
}

/* vkQueueWaitIdle, vkDeviceWaitIdle: every submission has ended when it returned. */
static int
wait_idle(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	/* The queue or the device. */
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: idle already. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	return 0;
}

/* Destroys a remembered identity's token: nothing to free. */
static int
token_destroy(
	struct mali_vk_session *session,
	void *payload)
{
	(void)session;
	(void)payload;
	return 0;
}

/* Remembers an identity of a kind; one remembered already is kept. */
static int
remember(
	struct mali_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity_word)
{
	int error;

	/* The token stands for the identity. */
	error = mali_vk_object_publish(session, kind, identity_word, &remembered_token, token_destroy);
	if (error != 0 && error != EEXIST)
		return error;

	/* Succeeded: the identity is known. */
	return 0;
}

/* Stores 32 IEEE bits as a float field, without floating-point instructions. */
static void
set_float(
	float *destination,
	uint32_t bits)
{
	kern_memcpy(destination, &bits, sizeof(bits));
}
