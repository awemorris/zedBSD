/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private Vulkan discovery describes the finite Keiland graphics path, without unimplemented optional features. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-device.h"

/* Generated device-independent Zlib record encoding remains shared read-only source. */
#include "drivers/gpu/i915/render/vulkan-codec.inc"

/* Driver-owned limits bound the implemented single-sample, one-layer graphics path. */
#define VULKAN_IMAGE_SIDE 4096U
#define VULKAN_MEMORY_BYTES (256U << 20)
#define VULKAN_PUSH_BYTES 128U
#define VULKAN_DESCRIPTOR_SETS 4U
#define VULKAN_TEXTURES 8U

static int fixed_query(struct bcm2711_vulkan_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int queue_properties(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int format_query(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int image_query(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void device_properties(VkPhysicalDeviceProperties *properties);
static void format_properties(VkFormat format, VkFormatProperties *properties);
static void set_float(float *destination, uint32_t bits);

/*
 * Answers typed physical queries using the same immutable capability limits as native graphics resources.
 */
int
bcm2711_vulkan_query_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Other typed routers retain ownership of opcodes outside physical-device discovery. */
	*handled = 0;
	if (opcode < GPU_OP_GET_PHYSICAL_DEVICE_FEATURES || opcode > GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES)
		return 0;
	*handled = 1;
	if (requested != 1)
		return EINVAL;

	/* Each physical query has its own exact client framing and output structure. */
	switch (opcode) {
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES:
		error = queue_properties(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES:
		error = format_query(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES:
		error = image_query(session, reader, reply);
		break;
	default:
		error = fixed_query(session, opcode, reader, reply);
		break;
	}

	/* Malformed input never produces a successful incomplete physical-device snapshot. */
	if (error != 0)
		return error;

	/* Succeeded: one complete native physical output is ready for the client's actual codec. */
	return 0;
}

/* Returns complete property, feature or memory records behind a live physical-device identity. */
static int
fixed_query(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *physical;
	VkPhysicalDeviceProperties *properties;
	VkPhysicalDeviceFeatures *features;
	VkPhysicalDeviceMemoryProperties *memory;
	uint64_t identity;
	uint64_t present;
	uint64_t types;
	uint64_t heaps;

	/* Queries cannot silently borrow another session's absent physical identity. */
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	physical = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PHYSICAL_DEVICE, identity);
	if (physical == NULL)
		return EINVAL;

	/* Feature structures begin zeroed, and no unsupported optional feature is claimed. */
	if (opcode == GPU_OP_GET_PHYSICAL_DEVICE_FEATURES) {
		features = i915_vkc_array(reader, &session->arena, 1, sizeof(*features));
		if (features == NULL)
			return ENOMEM;
		drv_i915_wire_reply_u64(reply, 1);
		i915_vkc_enc_VkPhysicalDeviceFeatures(reply, features);
		return 0;
	}

	/* The private protocol requires native version 1.1; limits describe only this driver's finite graphics subset. */
	if (opcode == GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES) {
		properties = i915_vkc_array(reader, &session->arena, 1, sizeof(*properties));
		if (properties == NULL)
			return ENOMEM;
		device_properties(properties);
		drv_i915_wire_reply_u64(reply, 1);
		i915_vkc_enc_VkPhysicalDeviceProperties(reply, properties);
		return 0;
	}

	/* The fixed protocol arrays retain their maximum ABI extents even when only one native type/heap is used. */
	types = drv_i915_wire_read_u64(reader);
	heaps = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    types != VK_MAX_MEMORY_TYPES ||
	    heaps != VK_MAX_MEMORY_HEAPS)
		return EINVAL;
	memory = i915_vkc_array(reader, &session->arena, 1, sizeof(*memory));
	if (memory == NULL)
		return ENOMEM;

	/* Native allocations use Normal non-cacheable kernel/user RAM aliases, never a cached nonsnooping mapping. */
	memory->memoryTypeCount = 1;
	memory->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	memory->memoryTypes[0].heapIndex = 0;
	memory->memoryHeapCount = 1;
	memory->memoryHeaps[0].size = VULKAN_MEMORY_BYTES;
	memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkPhysicalDeviceMemoryProperties(reply, memory);

	/* Succeeded: the driver budget and coherent memory policy share stable type and heap index zero. */
	return 0;
}

/* Enumerates one graphics/transfer family while preserving count-only and actual output-array framing. */
static int
queue_properties(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *physical;
	VkQueueFamilyProperties properties;
	uint64_t identity;
	uint64_t present;
	uint64_t array;
	uint32_t capacity;

	/* A count-only query carries no records and an array request can contain at most the sole native family. */
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	capacity = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    present != 1 ||
	    array > 1 ||
	    (array != 0 &&
	     capacity != 1))
		return EINVAL;
	physical = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PHYSICAL_DEVICE, identity);
	if (physical == NULL)
		return EINVAL;

	/* The one serialized V3D runner supports graphics and transfers, with no public compute or timestamp query support. */
	kern_memset(&properties, 0, sizeof(properties));
	properties.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
	properties.queueCount = 1;
	properties.minImageTransferGranularity.width = 1;
	properties.minImageTransferGranularity.height = 1;
	properties.minImageTransferGranularity.depth = 1;
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, 1);
	drv_i915_wire_reply_u64(reply, array);
	if (array != 0)
		i915_vkc_enc_VkQueueFamilyProperties(reply, &properties);

	/* Succeeded: the reported family/index matches logical-device queue validation. */
	return 0;
}

/* Returns the finite native format table without inferring support from another GPU's implementation. */
static int
format_query(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *physical;
	VkFormatProperties properties;
	uint64_t identity;
	uint64_t present;
	uint32_t format;

	/* Format queries name a live physical object and a required complete output structure. */
	identity = drv_i915_wire_read_u64(reader);
	format = drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	physical = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PHYSICAL_DEVICE, identity);
	if (physical == NULL)
		return EINVAL;

	/* Unknown formats return a complete zero capability record as Vulkan specifies. */
	format_properties(format, &properties);
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkFormatProperties(reply, &properties);

	/* Succeeded: this format has only the native paths listed in the independent driver table. */
	return 0;
}

/* Validates a whole image-format request rather than accepting unknown usage, flags or dimensions. */
static int
image_query(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *physical;
	VkImageFormatProperties properties;
	VkFormatProperties formats;
	uint64_t identity;
	uint64_t present;
	uint32_t format;
	uint32_t type;
	uint32_t tiling;
	uint32_t usage;
	uint32_t flags;
	uint32_t known_usage;
	uint32_t features;
	VkResult status;

	/* The protocol sends all core image constraints before its required output pointer. */
	identity = drv_i915_wire_read_u64(reader);
	format = drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	tiling = drv_i915_wire_read_u32(reader);
	usage = drv_i915_wire_read_u32(reader);
	flags = drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	physical = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PHYSICAL_DEVICE, identity);
	if (physical == NULL)
		return EINVAL;

	/* Only single-layer 2D colour sampling/rendering and transfers belong to the native graphics path. */
	kern_memset(&properties, 0, sizeof(properties));
	format_properties(format, &formats);
	features = formats.optimalTilingFeatures;
	if (tiling == VK_IMAGE_TILING_LINEAR)
		features = formats.linearTilingFeatures;
	known_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	status = VK_SUCCESS;
	if (features == 0 ||
	    type != VK_IMAGE_TYPE_2D ||
	    (tiling != VK_IMAGE_TILING_LINEAR &&
	     tiling != VK_IMAGE_TILING_OPTIMAL) ||
	    flags != 0 ||
	    usage == 0 ||
	    (usage & ~known_usage) != 0)
		status = VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Successful resources stay within the same one-layer/single-sample/dimension limits as native image creation. */
	if (status == VK_SUCCESS) {
		properties.maxExtent.width = VULKAN_IMAGE_SIDE;
		properties.maxExtent.height = VULKAN_IMAGE_SIDE;
		properties.maxExtent.depth = 1;
		properties.maxMipLevels = 1;
		properties.maxArrayLayers = 1;
		properties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
		properties.maxResourceSize = (uint64_t)VULKAN_IMAGE_SIDE * VULKAN_IMAGE_SIDE * 4;
	}

	/* Failure returns a complete zero output record instead of leaving uninitialized client storage. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkImageFormatProperties(reply, &properties);

	/* Succeeded: the response records either exact native support or an explicit unsupported-format result. */
	return 0;
}

/* Fills independent bounded graphics limits that the final native runtime must enforce before capability publication. */
static void
device_properties(
	VkPhysicalDeviceProperties *properties)
{
	static const char name[] = "zedBSD V3D 4.2 (Keiland graphics)";
	VkPhysicalDeviceLimits *limits;

	/* The private 1.1 version is a transport requirement; optional features and unimplemented stages remain absent. */
	properties->apiVersion = VK_MAKE_VERSION(1, 1, 0);
	properties->driverVersion = 1;
	properties->vendorID = 0x14e4;
	properties->deviceID = 0x0402;
	properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
	kern_memcpy(properties->deviceName, name, sizeof(name));
	limits = &properties->limits;

	/* One-level 2D images, bounded memory and immutable kernel-owned shader programs use explicit finite budgets. */
	limits->maxImageDimension2D = VULKAN_IMAGE_SIDE;
	limits->maxImageArrayLayers = 1;
	limits->maxUniformBufferRange = 65536;
	limits->maxPushConstantsSize = VULKAN_PUSH_BYTES;
	limits->maxMemoryAllocationCount = 4096;
	limits->maxSamplerAllocationCount = 4096;
	limits->bufferImageGranularity = 4096;
	limits->maxBoundDescriptorSets = VULKAN_DESCRIPTOR_SETS;
	limits->maxPerStageDescriptorSamplers = VULKAN_TEXTURES;
	limits->maxPerStageDescriptorSampledImages = VULKAN_TEXTURES;
	limits->maxPerStageDescriptorUniformBuffers = 4;
	limits->maxPerStageResources = 20;
	limits->maxDescriptorSetSamplers = VULKAN_TEXTURES;
	limits->maxDescriptorSetSampledImages = VULKAN_TEXTURES;
	limits->maxDescriptorSetUniformBuffers = 4;

	/* Scalar native vertex/VPM interfaces support bounded float attributes and one fragment colour target. */
	limits->maxVertexInputAttributes = 16;
	limits->maxVertexInputBindings = 16;
	limits->maxVertexInputAttributeOffset = 2047;
	limits->maxVertexInputBindingStride = 2048;
	limits->maxVertexOutputComponents = 32;
	limits->maxFragmentInputComponents = 32;
	limits->maxFragmentOutputAttachments = 1;
	limits->maxFragmentCombinedOutputResources = 1;
	limits->maxDrawIndexedIndexValue = 0xffffffffU;
	limits->maxDrawIndirectCount = 0;
	limits->maxViewports = 1;
	limits->maxViewportDimensions[0] = VULKAN_IMAGE_SIDE;
	limits->maxViewportDimensions[1] = VULKAN_IMAGE_SIDE;
	set_float(&limits->viewportBoundsRange[0], 0xc5800000U);
	set_float(&limits->viewportBoundsRange[1], 0x45800000U);
	limits->viewportSubPixelBits = 8;
	limits->subPixelPrecisionBits = 8;
	limits->subTexelPrecisionBits = 4;
	limits->maxFramebufferWidth = VULKAN_IMAGE_SIDE;
	limits->maxFramebufferHeight = VULKAN_IMAGE_SIDE;
	limits->maxFramebufferLayers = 1;
	limits->maxColorAttachments = 1;
	limits->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->maxSampleMaskWords = 1;
	limits->standardSampleLocations = VK_TRUE;

	/* Samplers and rasterization support fixed LOD/line width, with no anisotropy or wide primitive feature. */
	set_float(&limits->maxSamplerLodBias, 0);
	set_float(&limits->maxSamplerAnisotropy, 0x3f800000U);
	set_float(&limits->lineWidthRange[0], 0x3f800000U);
	set_float(&limits->lineWidthRange[1], 0x3f800000U);
	set_float(&limits->lineWidthGranularity, 0x3f800000U);
	limits->minMemoryMapAlignment = 4096;
	limits->minUniformBufferOffsetAlignment = 4;
	limits->optimalBufferCopyOffsetAlignment = 4;
	limits->optimalBufferCopyRowPitchAlignment = 4;
	limits->nonCoherentAtomSize = 64;

	/* Succeeded: every reported optional/unsupported field remains zero in the command-local record. */
	return;
}

/* Defines only colour formats used by the native compositor and float vertex inputs consumed by VPM. */
static void
format_properties(
	VkFormat format,
	VkFormatProperties *properties)
{
	/* Unknown, depth, compressed, integer and sRGB images stay unsupported until their complete native paths exist. */
	kern_memset(properties, 0, sizeof(*properties));
	if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM) {
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
						    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
						    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
						    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
		properties->linearTilingFeatures = properties->optimalTilingFeatures;
	}

	/* The scalar frontend consumes bounded 32-bit float attributes rather than foreign GPU vertex format encodings. */
	if (format == VK_FORMAT_R32_SFLOAT ||
	    format == VK_FORMAT_R32G32_SFLOAT ||
	    format == VK_FORMAT_R32G32B32_SFLOAT ||
	    format == VK_FORMAT_R32G32B32A32_SFLOAT)
		properties->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;

	/* Succeeded: this finite native table has no inferred optional format capabilities. */
	return;
}

/* Copies IEEE-754 bits without executing a kernel floating-point instruction. */
static void
set_float(
	float *destination,
	uint32_t bits)
{
	/* Native builds reserve floating-point register state for userspace and shader execution only. */
	kern_memcpy(destination, &bits, sizeof(bits));

	/* Succeeded: the wire codec can encode the exact declared limit bits. */
	return;
}
