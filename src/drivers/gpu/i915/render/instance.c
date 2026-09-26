/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The instance, physical-device, device and queue commands (see
 * instance.h).
 *
 * Minimal connection, happy path only.  The wire of every command here was
 * read from the library that sends it (userland/base/libvulkan: instance.c,
 * device.c, objects.c), and the records travel through the generated codec,
 * so the two ends cannot drift.
 *
 * XXX: the limits and the format table are what the connectivity check
 * needs, not a survey of the hardware; each is marked where it is filled.
 */

#include "instance.h"
#include "codec.h"
#include "object.h"
#include <kern/kcrt.h>

#include "../i915.h"

#include <uapi/errno.h>
#include <stdint.h>

#include "vulkan-codec.inc"

/* The bits of the float constants the limits are filled with. */
#define I915_F32_ONE		0x3f800000U	/* 1.0f */
#define I915_F32_16		0x41800000U	/* 16.0f */
#define I915_F32_EIGHTH		0x3e000000U	/* 0.125f */
#define I915_F32_MINUS_32768	0xc7000000U	/* -32768.0f */
#define I915_F32_32767		0x46fffe00U	/* 32767.0f */
#define I915_F32_2048		0x45000000U	/* 2048.0f */

/*
 * What the object table records for an instance, a physical device, a
 * device and a queue.
 *
 * The identities are the library's; the executor only remembers that they
 * were created, so every one of them points at this token.  It is static
 * and never freed.
 */
static int i915_instance_token;

static int i915_instance_create(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_enumerate_physical_devices(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_properties(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void i915_instance_limits(VkPhysicalDeviceLimits *limits);
static int i915_instance_features(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_memory_properties(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_queue_families(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void i915_instance_format_features(uint32_t format, VkFormatProperties *properties);
static uint32_t i915_instance_usage_features(uint32_t usage);
static int i915_instance_format_properties(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_image_format_properties(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_create_device(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_get_device_queue2(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_destroy(struct i915_render_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int i915_instance_wait_idle(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void i915_instance_set_float(float *destination, uint32_t bits);

/*
 * Executes an instance, physical-device, device or queue command.
 *
 * handled is cleared for an opcode this part does not own.
 */
int
drv_i915_render_instance_dispatch(
	struct i915_render_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or reports the opcode as someone else's. */
	*handled = 1;
	switch (opcode) {
	case 0U:
		/* vkCreateInstance */
		error = i915_instance_create(session, reader, reply);
		break;
	case 1U:
		/* vkDestroyInstance */
		error = i915_instance_destroy(session, I915_VK_OBJ_INSTANCE, reader);
		break;
	case 2U:
		/* vkEnumeratePhysicalDevices */
		error = i915_instance_enumerate_physical_devices(session, reader, reply);
		break;
	case 3U:
		/* vkGetPhysicalDeviceFeatures */
		error = i915_instance_features(session, reader, reply);
		break;
	case 4U:
		/* vkGetPhysicalDeviceFormatProperties */
		error = i915_instance_format_properties(reader, reply);
		break;
	case 5U:
		/* vkGetPhysicalDeviceImageFormatProperties */
		error = i915_instance_image_format_properties(reader, reply);
		break;
	case 6U:
		/* vkGetPhysicalDeviceProperties */
		error = i915_instance_properties(session, reader, reply);
		break;
	case 7U:
		/* vkGetPhysicalDeviceQueueFamilyProperties */
		error = i915_instance_queue_families(session, reader, reply);
		break;
	case 8U:
		/* vkGetPhysicalDeviceMemoryProperties */
		error = i915_instance_memory_properties(session, reader, reply);
		break;
	case 11U:
		/* vkCreateDevice */
		error = i915_instance_create_device(session, reader, reply);
		break;
	case 12U:
		/* vkDestroyDevice */
		error = i915_instance_destroy(session, I915_VK_OBJ_DEVICE, reader);
		break;
	case 19U:
	case 20U:
		/* vkQueueWaitIdle and vkDeviceWaitIdle */
		error = i915_instance_wait_idle(reader, reply);
		break;
	case 155U:
		/* vkGetDeviceQueue2 */
		error = i915_instance_get_device_queue2(session, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Reports why the command was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the command was executed. */
	return 0;
}

/* vkCreateInstance: [pCreateInfo][allocator = 0][pInstance: id] -> [result][present][id]. */
static int
i915_instance_create(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkInstanceCreateInfo info;
	uint64_t present;
	uint64_t identity;
	int error;

	/* Decodes the create info when it is present; nothing of it is kept. */
	kern_memset(&info, 0, sizeof(info));
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkInstanceCreateInfo(reader, &session->arena, &info);

	/* Skips pAllocator and the pInstance present word, and reads the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Remembers that the instance exists. */
	error = drv_i915_object_insert(session, I915_VK_OBJ_INSTANCE, identity, &i915_instance_token);
	if (error != 0)
		return error;

	/* Replies VK_SUCCESS and the identity. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the instance is known. */
	return 0;
}

/*
 * vkEnumeratePhysicalDevices: [instance][pCount present][count][array count][ids]
 * -> [result][present][count][array count][ids].
 */
static int
i915_instance_enumerate_physical_devices(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t array_count;
	uint64_t identity;
	uint64_t index;
	uint32_t count;

	/* Skips the instance and the pPhysicalDeviceCount present word, and reads the counts. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	array_count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* There is one physical device, so an array longer than one is malformed. */
	if (array_count > 1U)
		return EINVAL;

	/* An array must come with a count of one. */
	if (array_count != 0U && count != 1U)
		return EINVAL;

	/* Replies VK_SUCCESS and a count of one physical device. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u64(reply, array_count);

	/* Remembers and echoes the identity the library chose for the physical device. */
	for (index = 0U; index < array_count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;

		/* A failure to remember the identity is not reported; the reply is the same. */
		(void)drv_i915_object_insert(session, I915_VK_OBJ_PHYSICAL_DEVICE, identity, &i915_instance_token);
		drv_i915_wire_reply_u64(reply, identity);
	}

	/* Succeeded: the one physical device is reported. */
	return 0;
}

/* vkGetPhysicalDeviceProperties: [physical][present] -> [present][VkPhysicalDeviceProperties]. */
static int
i915_instance_properties(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	static const char name[] = "zedBSD i915 (Gen12 Xe)";
	VkPhysicalDeviceProperties *properties;

	/* Skips the physical device and the present word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Takes the zeroed record from the arena; it is too large for the stack. */
	properties = i915_vkc_array(reader, &session->arena, 1U, sizeof(*properties));
	if (properties == NULL)
		return ENOMEM;

	/* Names the device; the library requires a native Vulkan 1.1. */
	properties->apiVersion = VK_MAKE_VERSION(1, 1, 0);
	properties->driverVersion = 1U;
	properties->vendorID = 0x8086U;
	properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
	kern_memcpy(properties->deviceName, name, sizeof(name));

	/* Reports the PCI product, or zero for an executor without a device. */
	if (session->vk->i915 != NULL) {
		properties->deviceID = session->vk->i915->product;
	} else {
		properties->deviceID = 0U;
	}

	/* Fills the limits. */
	i915_instance_limits(&properties->limits);

	/* Replies the present word and the record. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkPhysicalDeviceProperties(reply, properties);

	/* Succeeded: the properties are reported. */
	return 0;
}

/*
 * Fills the limits.  XXX: the values a Gen12 part comfortably meets and the
 * standard application reads, not a transcription of the hardware's maxima.
 */
static void
i915_instance_limits(
	VkPhysicalDeviceLimits *limits)
{
	/* Image, buffer and allocation sizes. */
	limits->maxImageDimension1D = 16384U;
	limits->maxImageDimension2D = 16384U;
	limits->maxImageDimension3D = 2048U;
	limits->maxImageDimensionCube = 16384U;
	limits->maxImageArrayLayers = 2048U;
	limits->maxTexelBufferElements = 1U << 27;
	limits->maxUniformBufferRange = 1U << 27;
	limits->maxStorageBufferRange = 1U << 30;
	limits->maxPushConstantsSize = 128U;
	limits->maxMemoryAllocationCount = 4096U;
	limits->maxSamplerAllocationCount = 4000U;
	limits->bufferImageGranularity = 1U;

	/* Descriptor counts per stage and per set. */
	limits->maxBoundDescriptorSets = 4U;
	limits->maxPerStageDescriptorSamplers = 16U;
	limits->maxPerStageDescriptorUniformBuffers = 12U;
	limits->maxPerStageDescriptorStorageBuffers = 4U;
	limits->maxPerStageDescriptorSampledImages = 16U;
	limits->maxPerStageDescriptorStorageImages = 4U;
	limits->maxPerStageDescriptorInputAttachments = 4U;
	limits->maxPerStageResources = 128U;
	limits->maxDescriptorSetSamplers = 96U;
	limits->maxDescriptorSetUniformBuffers = 72U;
	limits->maxDescriptorSetUniformBuffersDynamic = 8U;
	limits->maxDescriptorSetStorageBuffers = 24U;
	limits->maxDescriptorSetStorageBuffersDynamic = 4U;
	limits->maxDescriptorSetSampledImages = 96U;
	limits->maxDescriptorSetStorageImages = 24U;
	limits->maxDescriptorSetInputAttachments = 4U;

	/* Vertex input and the stage interfaces. */
	limits->maxVertexInputAttributes = 16U;
	limits->maxVertexInputBindings = 16U;
	limits->maxVertexInputAttributeOffset = 2047U;
	limits->maxVertexInputBindingStride = 2048U;
	limits->maxVertexOutputComponents = 64U;
	limits->maxFragmentInputComponents = 64U;
	limits->maxFragmentOutputAttachments = 4U;
	limits->maxFragmentCombinedOutputResources = 4U;

	/* Compute. */
	limits->maxComputeSharedMemorySize = 16384U;
	limits->maxComputeWorkGroupCount[0] = 65535U;
	limits->maxComputeWorkGroupCount[1] = 65535U;
	limits->maxComputeWorkGroupCount[2] = 65535U;
	limits->maxComputeWorkGroupInvocations = 128U;
	limits->maxComputeWorkGroupSize[0] = 128U;
	limits->maxComputeWorkGroupSize[1] = 128U;
	limits->maxComputeWorkGroupSize[2] = 64U;

	/* Precision, draws and sampling. */
	limits->subPixelPrecisionBits = 4U;
	limits->subTexelPrecisionBits = 4U;
	limits->mipmapPrecisionBits = 4U;
	limits->maxDrawIndexedIndexValue = 0xffffffffU;
	limits->maxDrawIndirectCount = 1U;
	i915_instance_set_float(&limits->maxSamplerLodBias, I915_F32_16);
	i915_instance_set_float(&limits->maxSamplerAnisotropy, I915_F32_ONE);

	/* Viewports. */
	limits->maxViewports = 1U;
	limits->maxViewportDimensions[0] = 16384U;
	limits->maxViewportDimensions[1] = 16384U;
	i915_instance_set_float(&limits->viewportBoundsRange[0], I915_F32_MINUS_32768);
	i915_instance_set_float(&limits->viewportBoundsRange[1], I915_F32_32767);

	/* Alignments and texel offsets. */
	limits->minMemoryMapAlignment = 4096U;
	limits->minTexelBufferOffsetAlignment = 16U;
	limits->minUniformBufferOffsetAlignment = 64U;
	limits->minStorageBufferOffsetAlignment = 64U;
	limits->minTexelOffset = -8;
	limits->maxTexelOffset = 7U;
	limits->minTexelGatherOffset = -8;
	limits->maxTexelGatherOffset = 7U;
	limits->subPixelInterpolationOffsetBits = 4U;

	/* Framebuffers and sample counts: one sample everywhere. */
	limits->maxFramebufferWidth = 16384U;
	limits->maxFramebufferHeight = 16384U;
	limits->maxFramebufferLayers = 1U;
	limits->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->maxColorAttachments = 4U;
	limits->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->maxSampleMaskWords = 1U;

	/* Clipping, queue priorities, points and lines. */
	limits->maxClipDistances = 8U;
	limits->maxCullDistances = 8U;
	limits->maxCombinedClipAndCullDistances = 8U;
	limits->discreteQueuePriorities = 2U;
	i915_instance_set_float(&limits->pointSizeRange[0], I915_F32_EIGHTH);
	i915_instance_set_float(&limits->pointSizeRange[1], I915_F32_2048);
	i915_instance_set_float(&limits->lineWidthRange[0], I915_F32_ONE);
	i915_instance_set_float(&limits->lineWidthRange[1], I915_F32_ONE);
	limits->strictLines = VK_FALSE;
	limits->standardSampleLocations = VK_TRUE;

	/* Copy and coherence granularities. */
	limits->optimalBufferCopyOffsetAlignment = 64U;
	limits->optimalBufferCopyRowPitchAlignment = 64U;
	limits->nonCoherentAtomSize = 64U;
}

/* vkGetPhysicalDeviceFeatures: [physical][present] -> [present][VkPhysicalDeviceFeatures]; no optional feature is claimed. */
static int
i915_instance_features(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceFeatures *features;

	/* Skips the physical device and the present word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Takes a zeroed record from the arena: every feature is off. */
	features = i915_vkc_array(reader, &session->arena, 1U, sizeof(*features));
	if (features == NULL)
		return ENOMEM;

	/* Replies the present word and the record. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkPhysicalDeviceFeatures(reply, features);

	/* Succeeded: the features are reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceMemoryProperties: [physical][present][type extent][heap extent]
 * -> [present][VkPhysicalDeviceMemoryProperties].
 */
static int
i915_instance_memory_properties(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceMemoryProperties *memory;

	/* Skips the physical device, the present word and the two extents. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Takes the zeroed record from the arena. */
	memory = i915_vkc_array(reader, &session->arena, 1U, sizeof(*memory));
	if (memory == NULL)
		return ENOMEM;

	/* Describes one type: the GPU shares the system's memory, so it is device-local and host-coherent. */
	memory->memoryTypeCount = 1U;
	memory->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
		VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	memory->memoryTypes[0].heapIndex = 0U;

	/* Describes one heap.  XXX: its size is a budget, not a measurement. */
	memory->memoryHeapCount = 1U;
	memory->memoryHeaps[0].size = 1024ULL * 1024ULL * 1024ULL;
	memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

	/* Replies the present word and the record. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkPhysicalDeviceMemoryProperties(reply, memory);

	/* Succeeded: the memory properties are reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceQueueFamilyProperties: [physical][present][count][array count]
 * -> [present][count][array count][families].
 */
static int
i915_instance_queue_families(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkQueueFamilyProperties family;
	uint64_t array_count;

	UNUSED_PARAMETER(session);

	/* Skips the physical device, the present word and the count, and reads the array count. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	array_count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* There is one family, so an array longer than one is malformed. */
	if (array_count > 1U)
		return EINVAL;

	/* Describes one family of one queue: graphics, with the compute and transfer it implies, on RCS0. */
	kern_memset(&family, 0, sizeof(family));
	family.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
	family.queueCount = 1U;
	family.minImageTransferGranularity.width = 1U;
	family.minImageTransferGranularity.height = 1U;
	family.minImageTransferGranularity.depth = 1U;

	/* Replies the present word, the count and the array. */
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u64(reply, array_count);
	if (array_count != 0U)
		i915_vkc_enc_VkQueueFamilyProperties(reply, &family);

	/* Succeeded: the queue family is reported. */
	return 0;
}

/* Reports what the executor does with a format: render to it, sample it, copy it. */
static void
i915_instance_format_features(
	uint32_t format,
	VkFormatProperties *properties)
{
	/* A format not listed below has no feature. */
	kern_memset(properties, 0, sizeof(*properties));

	/*
	 * XXX: the three formats the executor lays out; nothing else is claimed.
	 * Every image is linear, so both tilings have the same features.
	 */
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_UNORM:
		/*
		 * Colour targets, sampled images read nearest or linear (between
		 * texels and between mip levels), and GPU rectangle copies and
		 * blits, a linear blit included.
		 */
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
			VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
			VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
			VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
			VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
			VK_FORMAT_FEATURE_BLIT_SRC_BIT |
			VK_FORMAT_FEATURE_BLIT_DST_BIT;
		properties->linearTilingFeatures = properties->optimalTilingFeatures;
		break;
	case VK_FORMAT_D32_SFLOAT:
		/* A depth target only. */
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
		break;
	default:
		break;
	}
}

/*
 * Reports the format features an image usage needs: sampling, rendering and
 * copying each need their own.  A storage image needs the storage feature,
 * which no format claims.  XXX: an input or transient attachment is not
 * mapped to a feature.
 */
static uint32_t
i915_instance_usage_features(
	uint32_t usage)
{
	uint32_t required;

	/* A copy out of the image needs the transfer source feature. */
	required = 0U;
	if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;

	/* A copy into the image needs the transfer destination feature. */
	if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

	/* Sampling needs the sampled image feature. */
	if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

	/* A storage image needs the storage feature. */
	if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;

	/* Rendering colour into the image needs the colour attachment feature. */
	if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;

	/* Rendering depth into the image needs the depth attachment feature. */
	if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;

	/* Succeeded: the features the usage needs. */
	return required;
}

/* vkGetPhysicalDeviceFormatProperties: [physical][format][present] -> [present][VkFormatProperties]. */
static int
i915_instance_format_properties(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFormatProperties properties;
	uint32_t format;

	/* Skips the physical device and the present word, and reads the format. */
	(void)drv_i915_wire_read_u64(reader);
	format = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Looks the format up and replies the present word and the record. */
	i915_instance_format_features(format, &properties);
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkFormatProperties(reply, &properties);

	/* Succeeded: the format's features are reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceImageFormatProperties: [physical][format][type][tiling][usage][flags][present]
 * -> [result][present][VkImageFormatProperties].
 */
static int
i915_instance_image_format_properties(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageFormatProperties image;
	VkFormatProperties properties;
	uint32_t format;
	uint32_t type;
	uint32_t tiling;
	uint32_t usage;
	uint32_t features;
	uint32_t required;

	/* Reads the format, the type, the tiling and the usage, and skips the create flags. */
	(void)drv_i915_wire_read_u64(reader);
	format = drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	tiling = drv_i915_wire_read_u32(reader);
	usage = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Looks the format up and takes the features of the tiling asked about. */
	i915_instance_format_features(format, &properties);
	kern_memset(&image, 0, sizeof(image));
	features = properties.optimalTilingFeatures;
	if (tiling == VK_IMAGE_TILING_LINEAR)
		features = properties.linearTilingFeatures;

	/* Finds the features the usage needs; a usage the executor never implements needs a feature no format has. */
	required = i915_instance_usage_features(usage);

	/*
	 * A format without features in that tiling, an image that is not 2D and
	 * a usage the features do not cover reply VK_ERROR_FORMAT_NOT_SUPPORTED.
	 * XXX: the create flags are not consulted.
	 */
	if (features == 0U ||
	    type != VK_IMAGE_TYPE_2D ||
	    (features & required) != required) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED);
		drv_i915_wire_reply_u64(reply, 1U);
		i915_vkc_enc_VkImageFormatProperties(reply, &image);
		return 0;
	}

	/*
	 * Describes a 2D image of one layer and one sample: a colour image has
	 * its levels down to one texel (15 for 16384), a depth image one level.
	 */
	image.maxExtent.width = 16384U;
	image.maxExtent.height = 16384U;
	image.maxExtent.depth = 1U;
	image.maxMipLevels = 15U;
	if (format == VK_FORMAT_D32_SFLOAT)
		image.maxMipLevels = 1U;
	image.maxArrayLayers = 1U;
	image.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
	image.maxResourceSize = 1ULL << 30;

	/* Replies VK_SUCCESS, the present word and the record. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkImageFormatProperties(reply, &image);

	/* Succeeded: the image format's properties are reported. */
	return 0;
}

/* vkCreateDevice: [physical][present][VkDeviceCreateInfo][allocator][present][id] -> [result][present][id]. */
static int
i915_instance_create_device(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDeviceCreateInfo info;
	uint64_t present;
	uint64_t identity;
	int error;

	/* Skips the physical device and decodes the create info when it is present; nothing of it is kept. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkDeviceCreateInfo(reader, &session->arena, &info);

	/* Skips pAllocator and the pDevice present word, and reads the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Remembers that the device exists. */
	error = drv_i915_object_insert(session, I915_VK_OBJ_DEVICE, identity, &i915_instance_token);
	if (error != 0)
		return error;

	/* Replies VK_SUCCESS and the identity. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the device is known. */
	return 0;
}

/*
 * vkGetDeviceQueue2 as device.c sends it: [device][present][sType DEVICE_QUEUE_INFO_2]
 * [pNext present][sType of the timeline record][pNext = 0][timeline index][flags][family]
 * [index][present][id] -> [present][id].  The command has no result.
 */
static int
i915_instance_get_device_queue2(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t identity;

	/*
	 * Skips the device, the queue info and its chained timeline record, and
	 * reads the identity.  XXX: there is one timeline, on RCS0; the timeline
	 * index is not checked.
	 */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Remembers the queue; a failure to remember it is not reported, and the reply is the same. */
	(void)drv_i915_object_insert(session, I915_VK_OBJ_QUEUE, identity, &i915_instance_token);

	/* Replies the present word and the identity. */
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the queue is known. */
	return 0;
}

/* vkDestroyInstance and vkDestroyDevice: [handle][allocator], no reply body. */
static int
i915_instance_destroy(
	struct i915_render_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader)
{
	uint64_t identity;

	/* Reads the identity and skips pAllocator. */
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Forgets the object; the token it named is not freed. */
	drv_i915_object_remove(session, kind, identity);

	/* Succeeded: the identity is forgotten. */
	return 0;
}

/* vkQueueWaitIdle and vkDeviceWaitIdle: [handle] -> [result]. */
static int
i915_instance_wait_idle(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	/* Skips the queue or device. */
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/*
	 * Replies VK_SUCCESS.  XXX: happy path -- the request worker runs every
	 * request to its end before it takes the next, and this command travels
	 * the same ordered stream, so by the time it is decoded the queue is
	 * idle.  A real wait belongs here once submission is asynchronous.
	 */
	drv_i915_wire_reply_u32(reply, 0U);

	/* Succeeded: the queue is idle. */
	return 0;
}

/* Fills a float field by its bits: this translation unit never touches an FP register. */
static void
i915_instance_set_float(
	float *destination,
	uint32_t bits)
{
	/* Stores the bits unchanged. */
	kern_memcpy(destination, &bits, sizeof(bits));
}
