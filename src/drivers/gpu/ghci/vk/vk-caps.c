/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The answers of the Vulkan executor that the GPU's capabilities decide:
 * the features, the limits, the queue family, the memory types, the
 * format properties, and the list of what a Vulkan 1.0 device should have
 * and this one lacks.
 *
 * The device always calls itself Vulkan 1.0.  Where the hardware cannot
 * reach a minimum of Vulkan 1.0, the executor answers what the hardware
 * has, says so in the attach's log, and refuses the operation that would
 * need more when it is asked for.  Every answer depends on struct
 * ghci_caps (and the format table) only, through ghci_caps_has(), so a
 * host test asks each one of a profile directly.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>

#include "drivers/gpu/compiler/spirv.h"

#include "vk.h"

/*
 * The limits of the hardware's descriptors, images and compute.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 8.6,
 * the Vulkan limits; section 8.5.2, the mip levels of a texture).
 */
#define LIMIT_IMAGE_EDGE		16384U
#define LIMIT_IMAGE_EDGE_3D		2048U
#define LIMIT_IMAGE_LAYERS		2048U
#define LIMIT_IMAGE_LEVELS		15U
#define LIMIT_IMAGE_LEVELS_3D		12U
#define LIMIT_UNIFORM_RANGE		65536U
#define LIMIT_STORAGE_RANGE		0xFFFFFFFFU
#define LIMIT_PUSH_BYTES		128U
#define LIMIT_ALLOCATIONS		4096U
#define LIMIT_SAMPLERS			4000U
#define LIMIT_SETS			4U
#define LIMIT_STAGE_SLOTS		16U
#define LIMIT_STAGE_STORAGE_IMAGES	8U
#define LIMIT_SET_SLOTS			96U
#define LIMIT_ATTRIBUTES		16U
#define LIMIT_ATTRIBUTE_OFFSET		524287U
#define LIMIT_BINDING_STRIDE		4095U
#define LIMIT_WORKGROUPS		65535U

/*
 * The invocations of one workgroup the shader frontend compiles: the
 * hardware takes 256, but the shared SPIR-V frontend refuses more than
 * 128, the least Vulkan 1.0 allows.
 */
#define LIMIT_INVOCATIONS		DRV_GPU_SPIRV_MAX_GROUP_INVOCATIONS
#define LIMIT_WORKGROUP_DEPTH		64U
#define LIMIT_SHARED_BYTES		16384U
#define LIMIT_SUBPIXEL_BITS		4U
#define LIMIT_DEFAULT_CLOCK_HZ		100000000U

/*
 * The limits the driver decides: the texel buffers the compiler samples
 * as a 256-texel wide 2D texture, the descriptors of set 2's table, the
 * dynamic offsets of the constant bank, the alignments of the
 * descriptors and of the layout's rows, and Vulkan 1.0's smallest values
 * for the rest.
 */
#define LIMIT_TEXEL_ELEMENTS		(256U * 16384U)
#define LIMIT_SET_UNIFORM_BUFFERS	72U
#define LIMIT_SET_STORAGE_BUFFERS	24U
#define LIMIT_SET_STORAGE_IMAGES	24U
#define LIMIT_SET_DYNAMIC_UNIFORM	8U
#define LIMIT_SET_DYNAMIC_STORAGE	4U
#define LIMIT_INPUT_ATTACHMENTS		4U
#define LIMIT_STAGE_RESOURCES		128U
#define LIMIT_VARYING_COMPONENTS	64U
#define LIMIT_FRAMEBUFFER_LAYERS	256U
#define LIMIT_GRANULE			256U
#define LIMIT_ROW_ALIGN			64U
#define LIMIT_UNIFORM_ALIGN		64U
#define LIMIT_TEXEL_ALIGN		16U
#define LIMIT_STORAGE_ALIGN		4U
#define LIMIT_MAP_ALIGN			64U

/* The float words of the limits (IEEE 754 singles). */
#define F32_ONE				0x3f800000U
#define F32_FIFTEEN			0x41700000U
#define F32_HALF_NEGATIVE		0xbf000000U
#define F32_0_4375			0x3ee00000U
#define F32_32768_NEGATIVE		0xc7000000U
#define F32_32767			0x46fffe00U

/* The shortfalls the list can name: every one at once. */
#define SHORTFALLS_MAX			8U

/* The nanoseconds of a second, for the timestamp's period. */
#define NANOSECONDS			1000000000U

/* What one required format needs on optimal tiling (a format Vulkan 1.0 requires, and the features). */
struct required_format {
	uint32_t format;
	uint32_t features;
};

/*
 * A part of the formats Vulkan 1.0 requires, each with the features it
 * requires on optimal tiling, as the check of the attach's log.
 *
 * Values referenced: the Vulkan 1.0 specification (section 31.3.3,
 * the required format support tables).
 */
static const struct required_format required_formats[] = {
	{ VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
	    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT },
	{ VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
	    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT },
	{ VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT },
	{ VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT },
	{ VK_FORMAT_D16_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT }
};

static uint32_t usage_features(uint32_t usage);

/*
 * Fills the features: those of Vulkan 1.0's required set the driver
 * provides on every GHCI GPU, and the optional ones the capability flags
 * give.
 */
void
ghci_vk_caps_features(
	const struct ghci_caps *caps,
	VkPhysicalDeviceFeatures *features)
{
	int indirect;
	int dual;
	int logic;
	int blend;
	int etc2;
	int srgb;

	/* Every feature not named below is false. */
	kern_memset(features, 0, sizeof(*features));

	/* The ones every GHCI GPU has: the compiler's bounds checks, 32-bit indices, the PSO's depth clamp. */
	features->robustBufferAccess = VK_TRUE;
	features->fullDrawIndexUint32 = VK_TRUE;
	features->depthClamp = VK_TRUE;
	features->depthBiasClamp = VK_TRUE;
	features->fillModeNonSolid = VK_TRUE;
	features->occlusionQueryPrecise = VK_TRUE;
	features->vertexPipelineStoresAndAtomics = VK_TRUE;
	features->fragmentStoresAndAtomics = VK_TRUE;

	/* Indirect draws (CAP_INDIRECT_DRAW). */
	indirect = ghci_caps_has(caps, GHCI_HAS_INDIRECT_DRAW);
	if (indirect) {
		features->multiDrawIndirect = VK_TRUE;
		features->drawIndirectFirstInstance = VK_TRUE;
	}

	/* Dual-source blending, logic operations and blending per attachment. */
	dual = ghci_caps_has(caps, GHCI_HAS_DUAL_SOURCE);
	logic = ghci_caps_has(caps, GHCI_HAS_LOGIC_OP);
	blend = ghci_caps_has(caps, GHCI_HAS_MRT_BLEND);
	if (dual)
		features->dualSrcBlend = VK_TRUE;
	if (logic)
		features->logicOp = VK_TRUE;
	if (blend)
		features->independentBlend = VK_TRUE;

	/* ETC2 and EAC, which include sRGB formats and so need the sRGB bit too. */
	etc2 = ghci_caps_has(caps, GHCI_HAS_ETC2);
	srgb = ghci_caps_has(caps, GHCI_HAS_SRGB);
	if (etc2 && srgb)
		features->textureCompressionETC2 = VK_TRUE;
}

/*
 * Fills the limits: the specification's table of Vulkan limits, the
 * capability registers (the colour attachments, the shared memory, the
 * clock of the timestamps, the samples), and the driver's own choices.
 */
void
ghci_vk_caps_limits(
	const struct ghci_caps *caps,
	VkPhysicalDeviceLimits *limits)
{
	uint32_t attachments;
	uint32_t shared;
	uint32_t samples;
	uint32_t clock;
	int dual;
	int indirect;

	/* What the capability registers decide. */
	kern_memset(limits, 0, sizeof(*limits));
	attachments = caps->max_mrt;
	if (attachments == 0U)
		attachments = 1U;
	shared = caps->lds_bytes;
	if (shared > LIMIT_SHARED_BYTES)
		shared = LIMIT_SHARED_BYTES;
	samples = ghci_vk_caps_sample_counts(caps);
	clock = caps->gclk_hz;
	if (clock == 0U)
		clock = LIMIT_DEFAULT_CLOCK_HZ;
	dual = ghci_caps_has(caps, GHCI_HAS_DUAL_SOURCE);
	indirect = ghci_caps_has(caps, GHCI_HAS_INDIRECT_DRAW);

	/* Images, buffers and allocations. */
	limits->maxImageDimension1D = LIMIT_IMAGE_EDGE;
	limits->maxImageDimension2D = LIMIT_IMAGE_EDGE;
	limits->maxImageDimension3D = LIMIT_IMAGE_EDGE_3D;
	limits->maxImageDimensionCube = LIMIT_IMAGE_EDGE;
	limits->maxImageArrayLayers = LIMIT_IMAGE_LAYERS;
	limits->maxTexelBufferElements = LIMIT_TEXEL_ELEMENTS;
	limits->maxUniformBufferRange = LIMIT_UNIFORM_RANGE;
	limits->maxStorageBufferRange = LIMIT_STORAGE_RANGE;
	limits->maxPushConstantsSize = LIMIT_PUSH_BYTES;
	limits->maxMemoryAllocationCount = LIMIT_ALLOCATIONS;
	limits->maxSamplerAllocationCount = LIMIT_SAMPLERS;
	limits->bufferImageGranularity = LIMIT_GRANULE;
	limits->sparseAddressSpaceSize = 0U;

	/* Descriptors: the slots and set 2's table. */
	limits->maxBoundDescriptorSets = LIMIT_SETS;
	limits->maxPerStageDescriptorSamplers = LIMIT_STAGE_SLOTS;
	limits->maxPerStageDescriptorUniformBuffers = LIMIT_STAGE_SLOTS;
	limits->maxPerStageDescriptorStorageBuffers = LIMIT_STAGE_SLOTS;
	limits->maxPerStageDescriptorSampledImages = LIMIT_STAGE_SLOTS;
	limits->maxPerStageDescriptorStorageImages = LIMIT_STAGE_STORAGE_IMAGES;
	limits->maxPerStageDescriptorInputAttachments = LIMIT_INPUT_ATTACHMENTS;
	limits->maxPerStageResources = LIMIT_STAGE_RESOURCES;
	limits->maxDescriptorSetSamplers = LIMIT_SET_SLOTS;
	limits->maxDescriptorSetUniformBuffers = LIMIT_SET_UNIFORM_BUFFERS;
	limits->maxDescriptorSetUniformBuffersDynamic = LIMIT_SET_DYNAMIC_UNIFORM;
	limits->maxDescriptorSetStorageBuffers = LIMIT_SET_STORAGE_BUFFERS;
	limits->maxDescriptorSetStorageBuffersDynamic = LIMIT_SET_DYNAMIC_STORAGE;
	limits->maxDescriptorSetSampledImages = LIMIT_SET_SLOTS;
	limits->maxDescriptorSetStorageImages = LIMIT_SET_STORAGE_IMAGES;
	limits->maxDescriptorSetInputAttachments = LIMIT_INPUT_ATTACHMENTS;

	/* Vertex input, the varyings and the fragment outputs; no tessellation, no geometry. */
	limits->maxVertexInputAttributes = LIMIT_ATTRIBUTES;
	limits->maxVertexInputBindings = LIMIT_ATTRIBUTES;
	limits->maxVertexInputAttributeOffset = LIMIT_ATTRIBUTE_OFFSET;
	limits->maxVertexInputBindingStride = LIMIT_BINDING_STRIDE;
	limits->maxVertexOutputComponents = LIMIT_VARYING_COMPONENTS;
	limits->maxFragmentInputComponents = LIMIT_VARYING_COMPONENTS;
	limits->maxFragmentOutputAttachments = attachments;
	limits->maxFragmentDualSrcAttachments = 0U;
	if (dual)
		limits->maxFragmentDualSrcAttachments = 1U;
	limits->maxFragmentCombinedOutputResources = attachments + LIMIT_STAGE_SLOTS + LIMIT_STAGE_STORAGE_IMAGES;

	/* Compute: the shared memory of EXT_CAP_FEATURE, and a workgroup of the frontend's four warps. */
	limits->maxComputeSharedMemorySize = shared;
	limits->maxComputeWorkGroupCount[0] = LIMIT_WORKGROUPS;
	limits->maxComputeWorkGroupCount[1] = LIMIT_WORKGROUPS;
	limits->maxComputeWorkGroupCount[2] = LIMIT_WORKGROUPS;
	limits->maxComputeWorkGroupInvocations = LIMIT_INVOCATIONS;
	limits->maxComputeWorkGroupSize[0] = LIMIT_INVOCATIONS;
	limits->maxComputeWorkGroupSize[1] = LIMIT_INVOCATIONS;
	limits->maxComputeWorkGroupSize[2] = LIMIT_WORKGROUP_DEPTH;

	/* Precision, draws and sampling. */
	limits->subPixelPrecisionBits = LIMIT_SUBPIXEL_BITS;
	limits->subTexelPrecisionBits = LIMIT_SUBPIXEL_BITS;
	limits->mipmapPrecisionBits = LIMIT_SUBPIXEL_BITS;
	limits->maxDrawIndexedIndexValue = 0xFFFFFFFFU;
	limits->maxDrawIndirectCount = 1U;
	if (indirect)
		limits->maxDrawIndirectCount = 0xFFFFFFFFU;
	ghci_vk_set_float(&limits->maxSamplerLodBias, F32_FIFTEEN);
	ghci_vk_set_float(&limits->maxSamplerAnisotropy, F32_ONE);

	/* One viewport of the largest framebuffer. */
	limits->maxViewports = 1U;
	limits->maxViewportDimensions[0] = LIMIT_IMAGE_EDGE;
	limits->maxViewportDimensions[1] = LIMIT_IMAGE_EDGE;
	ghci_vk_set_float(&limits->viewportBoundsRange[0], F32_32768_NEGATIVE);
	ghci_vk_set_float(&limits->viewportBoundsRange[1], F32_32767);
	limits->viewportSubPixelBits = 0U;

	/* Alignments: the mapping, texel buffers, uniform buffers and storage buffers. */
	limits->minMemoryMapAlignment = LIMIT_MAP_ALIGN;
	limits->minTexelBufferOffsetAlignment = LIMIT_TEXEL_ALIGN;
	limits->minUniformBufferOffsetAlignment = LIMIT_UNIFORM_ALIGN;
	limits->minStorageBufferOffsetAlignment = LIMIT_STORAGE_ALIGN;

	/* Texel offsets, gathers and interpolation offsets. */
	limits->minTexelOffset = -8;
	limits->maxTexelOffset = 7U;
	limits->minTexelGatherOffset = -8;
	limits->maxTexelGatherOffset = 7U;
	ghci_vk_set_float(&limits->minInterpolationOffset, F32_HALF_NEGATIVE);
	ghci_vk_set_float(&limits->maxInterpolationOffset, F32_0_4375);
	limits->subPixelInterpolationOffsetBits = LIMIT_SUBPIXEL_BITS;

	/* Framebuffers and their samples. */
	limits->maxFramebufferWidth = LIMIT_IMAGE_EDGE;
	limits->maxFramebufferHeight = LIMIT_IMAGE_EDGE;
	limits->maxFramebufferLayers = LIMIT_FRAMEBUFFER_LAYERS;
	limits->framebufferColorSampleCounts = samples;
	limits->framebufferDepthSampleCounts = samples;
	limits->framebufferStencilSampleCounts = samples;
	limits->framebufferNoAttachmentsSampleCounts = samples;
	limits->maxColorAttachments = attachments;
	limits->sampledImageColorSampleCounts = samples;
	limits->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->sampledImageDepthSampleCounts = samples;
	limits->sampledImageStencilSampleCounts = samples;
	limits->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->maxSampleMaskWords = 1U;

	/* Timestamps on the one queue, at the core clock; no clip or cull distances. */
	limits->timestampComputeAndGraphics = VK_TRUE;
	ghci_vk_set_float(&limits->timestampPeriod, ghci_vk_float_ratio(NANOSECONDS, clock));
	limits->maxClipDistances = 0U;
	limits->maxCullDistances = 0U;
	limits->maxCombinedClipAndCullDistances = 0U;

	/* Queue priorities, points and lines of width 1, the standard sample places. */
	limits->discreteQueuePriorities = 2U;
	ghci_vk_set_float(&limits->pointSizeRange[0], F32_ONE);
	ghci_vk_set_float(&limits->pointSizeRange[1], F32_ONE);
	ghci_vk_set_float(&limits->lineWidthRange[0], F32_ONE);
	ghci_vk_set_float(&limits->lineWidthRange[1], F32_ONE);
	limits->strictLines = VK_FALSE;
	limits->standardSampleLocations = VK_TRUE;

	/* Copies: rows of 64 bytes. */
	limits->optimalBufferCopyOffsetAlignment = LIMIT_ROW_ALIGN;
	limits->optimalBufferCopyRowPitchAlignment = LIMIT_ROW_ALIGN;
	limits->nonCoherentAtomSize = LIMIT_ROW_ALIGN;
}

/*
 * Fills the memory types and heaps.  A unified GPU (local_bytes 0, the
 * local memory the driver hands out) has one heap and one type that is
 * device local, host visible and coherent.  A discrete one has two heaps:
 * local memory, device local, with a type that is device local alone;
 * and host memory, with a type that is host visible and coherent, for
 * everything the processor writes or reads.
 */
void
ghci_vk_caps_memory(
	const struct ghci_caps *caps,
	uint64_t local_bytes,
	VkPhysicalDeviceMemoryProperties *memory)
{
	(void)caps;

	/* Two heaps on a discrete GPU: local memory first, then host memory. */
	kern_memset(memory, 0, sizeof(*memory));
	if (local_bytes != 0U) {
		memory->memoryTypeCount = 2U;
		memory->memoryTypes[GHCI_VK_TYPE_LOCAL].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		memory->memoryTypes[GHCI_VK_TYPE_LOCAL].heapIndex = 0U;
		memory->memoryTypes[GHCI_VK_TYPE_HOST].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		memory->memoryTypes[GHCI_VK_TYPE_HOST].heapIndex = 1U;
		memory->memoryHeapCount = 2U;
		memory->memoryHeaps[0].size = local_bytes;
		memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
		memory->memoryHeaps[1].size = GHCI_VK_HEAP_SIZE;
		memory->memoryHeaps[1].flags = 0U;
		return;
	}

	/* One type in one heap. */
	memory->memoryTypeCount = 1U;
	memory->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	memory->memoryTypes[0].heapIndex = 0U;
	memory->memoryHeapCount = 1U;
	memory->memoryHeaps[0].size = GHCI_VK_HEAP_SIZE;
	memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

/* Fills the one queue family: one queue that draws, computes and copies, with 64-bit timestamps. */
void
ghci_vk_caps_family(
	const struct ghci_caps *caps,
	VkQueueFamilyProperties *family)
{
	(void)caps;

	/* The ring is the queue. */
	kern_memset(family, 0, sizeof(*family));
	family->queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
	family->queueCount = 1U;
	family->timestampValidBits = 64U;
	family->minImageTransferGranularity.width = 1U;
	family->minImageTransferGranularity.height = 1U;
	family->minImageTransferGranularity.depth = 1U;
}

/*
 * Reports the sample counts of attachments and sampled images: 1, and 4
 * with CAP_MSAA (2 has no standard sample positions in the
 * specification, so it is not offered).
 */
uint32_t
ghci_vk_caps_sample_counts(
	const struct ghci_caps *caps)
{
	int msaa;

	/* Four samples with multisampling. */
	msaa = ghci_caps_has(caps, GHCI_HAS_MSAA);
	if (msaa)
		return VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;

	/* Succeeded: one sample. */
	return VK_SAMPLE_COUNT_1_BIT;
}

/* Fills a format's properties: its features on each tiling and in buffers (none for a format outside Vulkan 1.0). */
void
ghci_vk_caps_format(
	const struct ghci_vk_formats *formats,
	uint32_t format,
	VkFormatProperties *properties)
{
	/* No feature for a format the table does not reach. */
	kern_memset(properties, 0, sizeof(*properties));
	if (format >= GHCI_VK_FORMATS)
		return;

	/* The three sets of features. */
	properties->optimalTilingFeatures = formats->optimal[format];
	properties->linearTilingFeatures = formats->linear[format];
	properties->bufferFeatures = formats->buffer[format];
}

/*
 * Fills the limits of an image of a format, type, tiling, usage and
 * flags, and reports VK_SUCCESS or VK_ERROR_FORMAT_NOT_SUPPORTED.
 */
uint32_t
ghci_vk_caps_image_format(
	const struct ghci_caps *caps,
	const struct ghci_vk_formats *formats,
	uint32_t format,
	uint32_t type,
	uint32_t tiling,
	uint32_t usage,
	uint32_t flags,
	VkImageFormatProperties *properties)
{
	uint32_t features;
	uint32_t needed;
	uint32_t known;

	/* The format's features on the tiling. */
	kern_memset(properties, 0, sizeof(*properties));
	if (format >= GHCI_VK_FORMATS)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	features = formats->optimal[format];
	if (tiling == VK_IMAGE_TILING_LINEAR)
		features = formats->linear[format];
	else if (tiling != VK_IMAGE_TILING_OPTIMAL)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Some usage of Vulkan 1.0, every one with the feature it needs. */
	needed = usage_features(usage);
	if (features == 0U || needed == 0xFFFFFFFFU)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((features & needed) != needed)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) != 0U &&
	    (features & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) == 0U)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The flags of Vulkan 1.0 the device takes (no sparse); a cube is a 2D image. */
	known = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	if ((flags & ~known) != 0U)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0U && type != VK_IMAGE_TYPE_2D)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The extent, the levels and the layers of each type. */
	properties->maxExtent.width = LIMIT_IMAGE_EDGE;
	properties->maxExtent.height = 1U;
	properties->maxExtent.depth = 1U;
	properties->maxMipLevels = LIMIT_IMAGE_LEVELS;
	properties->maxArrayLayers = LIMIT_IMAGE_LAYERS;
	if (type == VK_IMAGE_TYPE_2D) {
		properties->maxExtent.height = LIMIT_IMAGE_EDGE;
	} else if (type == VK_IMAGE_TYPE_3D) {
		properties->maxExtent.width = LIMIT_IMAGE_EDGE_3D;
		properties->maxExtent.height = LIMIT_IMAGE_EDGE_3D;
		properties->maxExtent.depth = LIMIT_IMAGE_EDGE_3D;
		properties->maxMipLevels = LIMIT_IMAGE_LEVELS_3D;
		properties->maxArrayLayers = 1U;
	} else if (type != VK_IMAGE_TYPE_1D) {
		kern_memset(properties, 0, sizeof(*properties));
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* One sample, and the device's counts for a plain 2D image that may be an attachment. */
	properties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
	if (type == VK_IMAGE_TYPE_2D &&
	    (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) == 0U &&
	    (features & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0U)
		properties->sampleCounts = ghci_vk_caps_sample_counts(caps);

	/* The largest image is the largest allocation. */
	properties->maxResourceSize = GHCI_VK_HEAP_SIZE;

	/* Succeeded: such an image may be made. */
	return (uint32_t)VK_SUCCESS;
}

/*
 * Reports whether every feature asked for at vkCreateDevice is one the
 * device offers: 1 when it is.  VkPhysicalDeviceFeatures is an array of
 * VkBool32.
 */
int
ghci_vk_caps_features_within(
	const struct ghci_caps *caps,
	const VkPhysicalDeviceFeatures *asked)
{
	VkPhysicalDeviceFeatures offered;
	const VkBool32 *asked_words;
	const VkBool32 *offered_words;
	size_t count;
	size_t index;

	/* What the device offers, word by word against what was asked. */
	ghci_vk_caps_features(caps, &offered);
	asked_words = (const VkBool32 *)(const void *)asked;
	offered_words = (const VkBool32 *)(const void *)&offered;
	count = sizeof(*asked) / sizeof(VkBool32);

	/* A feature asked for that the device does not offer. */
	for (index = 0U; index < count; index++) {
		if (asked_words[index] != VK_FALSE && offered_words[index] == VK_FALSE)
			return 0;
	}

	/* Succeeded: every feature asked for is offered. */
	return 1;
}

/*
 * Lists, for the attach's log, what Vulkan 1.0 requires and this device
 * lacks; the executor refuses the operations that would need it when
 * they are asked for.  Returns the number of names written to names (at
 * most capacity).
 */
unsigned
ghci_vk_caps_shortfalls(
	const struct ghci_caps *caps,
	const struct ghci_vk_formats *formats,
	const char **names,
	unsigned capacity)
{
	const char *found[SHORTFALLS_MAX];
	unsigned count;
	unsigned index;
	size_t row;
	uint32_t features;
	int indirect;
	int msaa;
	int clip;
	int bilinear;

	/* The texture and sampler slots are shared by a pipeline's stages: 16, not Vulkan's 96. */
	count = 0U;
	found[count] = "sampled images and samplers: 16 per pipeline";
	count++;

	/* Indirect draws, colour attachments, shared memory and multisampling below Vulkan 1.0's minimum. */
	indirect = ghci_caps_has(caps, GHCI_HAS_INDIRECT_DRAW);
	msaa = ghci_caps_has(caps, GHCI_HAS_MSAA);
	if (!indirect) {
		found[count] = "indirect draws (CAP_INDIRECT_DRAW)";
		count++;
	}
	if (caps->max_mrt < 4U) {
		found[count] = "maxColorAttachments below 4";
		count++;
	}
	if (caps->lds_bytes < LIMIT_SHARED_BYTES) {
		found[count] = "maxComputeSharedMemorySize below 16384";
		count++;
	}
	if (!msaa) {
		found[count] = "4x multisampling (CAP_MSAA)";
		count++;
	}

	/*
	 * Without the polygon clipper the rasterizer has a guard band
	 * alone: a primitive that crosses the near plane is rejected (the
	 * driver has no clipping of its own), where Vulkan clips it.
	 * Without the bilinear filter no format filters, where Vulkan
	 * requires the linear filter of some.
	 *
	 * Values referenced: amaterrace-gpu ad23fe0 docs/ghci.md (section
	 * 3.2.2, EXT_CAP_FEATURE [30] CAP_CLIP_POLY and EXT_CAP_FMT0 [16]
	 * CAP_TU_BILINEAR; section 12.10, the hardware polygon clipping).
	 */
	clip = ghci_caps_has(caps, GHCI_HAS_CLIP_POLY);
	bilinear = ghci_caps_has(caps, GHCI_HAS_TU_BILINEAR);
	if (!clip) {
		found[count] = "clipping of primitives across the near plane (CAP_CLIP_POLY)";
		count++;
	}
	if (!bilinear) {
		found[count] = "linear texture filtering (CAP_TU_BILINEAR)";
		count++;
	}

	/* A required format without a required feature. */
	for (row = 0U; row < sizeof(required_formats) / sizeof(required_formats[0]); row++) {
		features = formats->optimal[required_formats[row].format];
		if ((features & required_formats[row].features) != required_formats[row].features) {
			found[count] = "a format feature Vulkan 1.0 requires";
			count++;
			break;
		}
	}

	/* The caller's room. */
	for (index = 0U; index < count && index < capacity; index++)
		names[index] = found[index];

	/* Succeeded: the number written. */
	return index;
}

/*
 * Reports whether the executor gives the blitter its transfers: only a
 * blitter that scales.  The specification does not say what alignment a
 * blitter without scaling needs of a copy's addresses, so on one the
 * copies, blits, fills and resolves are the driver's programs, as on a
 * device without the blitter.
 *
 * Values referenced: amaterrace-gpu ad23fe0 docs/ghci.md (section 3.2.1,
 * DEVICE_CAPS [29] CAP_BLT; section 3.2.2, EXT_CAP_FEATURE [31]
 * CAP_BLT_SCALE, without it 1:1 copies and fills only and no unaligned
 * byte window).
 */
int
ghci_vk_caps_blitter(
	const struct ghci_caps *caps)
{
	int scale;

	/* A blitter with scaling; CAP_BLT_SCALE reads 0 without CAP_BLT. */
	scale = ghci_caps_has(caps, GHCI_HAS_BLT_SCALE);
	if (!scale)
		return 0;

	/* Succeeded: the blitter takes the transfers. */
	return 1;
}

/*
 * Reports the optimal-tiling features an image's usage needs, or
 * 0xFFFFFFFF for no usage or a usage outside Vulkan 1.0.  The input
 * attachment is checked by the caller (a colour or a depth format).
 */
static uint32_t
usage_features(
	uint32_t usage)
{
	uint32_t known;
	uint32_t needed;

	/* Some usage, all of them Vulkan 1.0's. */
	known = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	    VK_IMAGE_USAGE_SAMPLED_BIT |
	    VK_IMAGE_USAGE_STORAGE_BIT |
	    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
	    VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
	    VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
	if (usage == 0U || (usage & ~known) != 0U)
		return 0xFFFFFFFFU;

	/* Each usage's feature. */
	needed = 0U;
	if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U)
		needed |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0U)
		needed |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
	if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0U)
		needed |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
	if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U)
		needed |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;

	/* Succeeded: the features. */
	return needed;
}
