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
 * read from the library that sends it (userland/desktop/libvulkan: instance.c,
 * device.c, objects.c), and the records travel through the generated codec,
 * so the two ends cannot drift.
 *
 * XXX: the limits and the format table are what the connectivity check
 * needs, not a survey of the hardware; each is marked where it is filled.
 */

#include "instance.h"
#include "codec.h"
#include "object.h"
#include "state.h"
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

/* The sample counts an attachment may have: one, two or four (image.c). */
#define I915_INSTANCE_ATTACHMENT_SAMPLES \
	(VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT)

/*
 * What the object table records for an instance, a physical device, a
 * device and a queue.
 *
 * The identities are the library's; the executor only remembers that they
 * were created, so every one of them points at this token.  It is static
 * and never freed.
 */
static int i915_instance_token;

/*
 * What the object table records for a queue of the video decode family
 * (family 1), so that a submission can tell its queue's family.  Like the
 * token above it is static and never freed.
 */
static int i915_instance_video_queue_token;

static int i915_instance_create(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_enumerate_physical_devices(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_properties(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void i915_instance_limits(VkPhysicalDeviceLimits *limits);
static int i915_instance_features(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_memory_properties(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_queue_families(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void i915_instance_format_features(uint32_t format, int video, VkFormatProperties *properties);
static uint32_t i915_instance_usage_features(uint32_t usage);
static int i915_instance_format_properties(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_instance_image_format_properties(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void i915_instance_video_image_properties(uint32_t type, uint32_t tiling, uint32_t usage, uint32_t features, struct i915_wire_writer *reply);
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
	case GPU_OP_CREATE_INSTANCE:
		/* vkCreateInstance */
		error = i915_instance_create(session, reader, reply);
		break;
	case GPU_OP_DESTROY_INSTANCE:
		/* vkDestroyInstance */
		error = i915_instance_destroy(session, I915_VK_OBJ_INSTANCE, reader);
		break;
	case GPU_OP_ENUMERATE_PHYSICAL_DEVICES:
		/* vkEnumeratePhysicalDevices */
		error = i915_instance_enumerate_physical_devices(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FEATURES:
		/* vkGetPhysicalDeviceFeatures */
		error = i915_instance_features(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES:
		/* vkGetPhysicalDeviceFormatProperties */
		error = i915_instance_format_properties(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES:
		/* vkGetPhysicalDeviceImageFormatProperties */
		error = i915_instance_image_format_properties(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES:
		/* vkGetPhysicalDeviceProperties */
		error = i915_instance_properties(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES:
		/* vkGetPhysicalDeviceQueueFamilyProperties */
		error = i915_instance_queue_families(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES:
		/* vkGetPhysicalDeviceMemoryProperties */
		error = i915_instance_memory_properties(session, reader, reply);
		break;
	case GPU_OP_CREATE_DEVICE:
		/* vkCreateDevice */
		error = i915_instance_create_device(session, reader, reply);
		break;
	case GPU_OP_DESTROY_DEVICE:
		/* vkDestroyDevice */
		error = i915_instance_destroy(session, I915_VK_OBJ_DEVICE, reader);
		break;
	case GPU_OP_QUEUE_WAIT_IDLE:
	case GPU_OP_DEVICE_WAIT_IDLE:
		/* vkQueueWaitIdle and vkDeviceWaitIdle */
		error = i915_instance_wait_idle(reader, reply);
		break;
	case GPU_OP_GET_DEVICE_QUEUE2:
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

/*
 * Reports the queue family of a queue: 1 for a queue of the video decode
 * family, 0 for any other identity, a queue of the graphics family or one
 * the executor does not know (a submission then runs as before).
 */
uint32_t
drv_i915_render_queue_family(
	struct i915_render_session *session,
	uint64_t identity)
{
	void *token;

	/* The token the queue was recorded with tells its family. */
	token = drv_i915_object_lookup(session, I915_VK_OBJ_QUEUE, identity);
	if (token == &i915_instance_video_queue_token)
		return 1U;

	/* Every other queue is of the graphics family. */
	return 0U;
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

	/*
	 * The geometry stage (ws075-p007b b4, design §5.5): Vulkan's least
	 * invocations and total output (the compiler refuses Invocations past
	 * 1 and logs it), 16 vec4 in and out as the vertex stage, 256 vertices.
	 */
	limits->maxGeometryShaderInvocations = 32U;
	limits->maxGeometryInputComponents = 64U;
	limits->maxGeometryOutputComponents = 64U;
	limits->maxGeometryOutputVertices = 256U;
	limits->maxGeometryTotalOutputComponents = 1024U;
	limits->maxFragmentInputComponents = 64U;
	limits->maxFragmentOutputAttachments = 4U;
	limits->maxFragmentCombinedOutputResources = 4U;

	/* Compute. */
	limits->maxComputeSharedMemorySize = 16384U;
	limits->maxComputeWorkGroupCount[0] = I915_GFX_MAX_GROUP_COUNT;
	limits->maxComputeWorkGroupCount[1] = I915_GFX_MAX_GROUP_COUNT;
	limits->maxComputeWorkGroupCount[2] = I915_GFX_MAX_GROUP_COUNT;
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

	/*
	 * Framebuffers and sample counts: attachments of one, two or four
	 * samples (image.c); a sampled or storage image has one.
	 */
	limits->maxFramebufferWidth = 16384U;
	limits->maxFramebufferHeight = 16384U;
	limits->maxFramebufferLayers = 2048U;
	limits->framebufferColorSampleCounts = I915_INSTANCE_ATTACHMENT_SAMPLES;
	limits->framebufferDepthSampleCounts = I915_INSTANCE_ATTACHMENT_SAMPLES;
	limits->framebufferStencilSampleCounts = I915_INSTANCE_ATTACHMENT_SAMPLES;
	limits->framebufferNoAttachmentsSampleCounts = I915_INSTANCE_ATTACHMENT_SAMPLES;
	limits->maxColorAttachments = 4U;
	limits->sampledImageColorSampleCounts = I915_INSTANCE_ATTACHMENT_SAMPLES;
	limits->sampledImageIntegerSampleCounts = I915_INSTANCE_ATTACHMENT_SAMPLES;
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

/*
 * vkGetPhysicalDeviceFeatures: [physical][present] -> [present][VkPhysicalDeviceFeatures].
 * The optional features claimed are vertexPipelineStoresAndAtomics (a
 * vertex shader stores to and loads from storage buffers, ws075-p006),
 * logicOp (the blend's logic operation, ws031-p032), shaderInt16 (16-bit
 * integers carried in 32-bit values, ws031-p039) and geometryShader (the
 * geometry stage, ws075-p007; its point size is not, so
 * shaderTessellationAndGeometryPointSize stays off).  XXX: the vertex
 * stage's atomics are not compiled.
 */
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

	/* Takes a zeroed record from the arena: every feature is off but the vertex stage's stores, the logic operation, Int16 and the geometry stage. */
	features = i915_vkc_array(reader, &session->arena, 1U, sizeof(*features));
	if (features == NULL)
		return ENOMEM;
	features->vertexPipelineStoresAndAtomics = VK_TRUE;
	features->logicOp = VK_TRUE;
	features->shaderInt16 = VK_TRUE;
	features->geometryShader = VK_TRUE;

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
	VkQueueFamilyProperties video;
	uint64_t array_count;
	uint32_t count;

	/* Skips the physical device, the present word and the count, and reads the array count. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	array_count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A device that offers video decode has its video family after the graphics one. */
	count = 1U;
	if (session->vk->video)
		count = 2U;

	/* An array longer than the families is malformed. */
	if (array_count > count)
		return EINVAL;

	/* Describes one family of one queue: graphics, with the compute and transfer it implies, on RCS0. */
	kern_memset(&family, 0, sizeof(family));
	family.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
	family.queueCount = 1U;
	family.minImageTransferGranularity.width = 1U;
	family.minImageTransferGranularity.height = 1U;
	family.minImageTransferGranularity.depth = 1U;

	/*
	 * Describes the video family: one queue of video decode only, on VCS0,
	 * with no timestamps and no image transfer.
	 */
	kern_memset(&video, 0, sizeof(video));
	video.queueFlags = VK_QUEUE_VIDEO_DECODE_BIT_KHR;
	video.queueCount = 1U;

	/* Replies the present word, the count and as many families as the array holds. */
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, count);
	drv_i915_wire_reply_u64(reply, array_count);
	if (array_count >= 1U)
		i915_vkc_enc_VkQueueFamilyProperties(reply, &family);
	if (array_count >= 2U)
		i915_vkc_enc_VkQueueFamilyProperties(reply, &video);

	/* Succeeded: the queue family is reported. */
	return 0;
}

/*
 * Reports what the executor does with a format: render to it, sample it,
 * copy it; on a device with video decode, decode into NV12 (design D23).
 */
static void
i915_instance_format_features(
	uint32_t format,
	int video,
	VkFormatProperties *properties)
{
	uint32_t surface_format;
	uint32_t texel_bytes;
	int supported;
	int error;

	/* A format not listed below has no feature. */
	kern_memset(properties, 0, sizeof(*properties));

	/*
	 * XXX: the formats the executor lays out (image.c); nothing else is
	 * claimed.  Every colour image is linear, so both tilings have the same
	 * features.
	 */
	switch (format) {
	case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
		/* Decode pictures in Y tiles, copied to linear buffers by the transfer path. */
		if (video) {
			properties->optimalTilingFeatures = VK_FORMAT_FEATURE_VIDEO_DECODE_OUTPUT_BIT_KHR |
				VK_FORMAT_FEATURE_VIDEO_DECODE_DPB_BIT_KHR |
				VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
		}
		break;
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		/*
		 * Colour targets that blend, sampled images read nearest or linear
		 * (between texels and between mip levels), and GPU rectangle copies
		 * and blits, a linear blit included.
		 */
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
			VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
			VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
			VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
			VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
			VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
			VK_FORMAT_FEATURE_BLIT_SRC_BIT |
			VK_FORMAT_FEATURE_BLIT_DST_BIT;
		properties->linearTilingFeatures = properties->optimalTilingFeatures;
		break;
	case VK_FORMAT_R8G8B8A8_UINT:
	case VK_FORMAT_R8G8B8A8_SINT:
	case VK_FORMAT_R32_UINT:
	case VK_FORMAT_R32_SINT:
		/* Integer colour targets and sampled images, read nearest, copied bit for bit (no blit, which filters). */
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
			VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
			VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
			VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
		properties->linearTilingFeatures = properties->optimalTilingFeatures;
		break;
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		/*
		 * A float colour target that blends, and a sampled image read
		 * nearest (as OpenGL ES reads it: not filterable), copied and
		 * blitted nearest.
		 */
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
			VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
			VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
			VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
			VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
			VK_FORMAT_FEATURE_BLIT_SRC_BIT |
			VK_FORMAT_FEATURE_BLIT_DST_BIT;
		properties->linearTilingFeatures = properties->optimalTilingFeatures;
		break;
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
	case VK_FORMAT_S8_UINT:
		/*
		 * A depth and stencil, or a stencil, target whose planes are copied
		 * to and from buffers; the depth plane may be sampled (Y-tiled,
		 * optimal only).
		 */
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
			VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
			VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
		if (format == VK_FORMAT_D32_SFLOAT_S8_UINT)
			properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		break;
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D16_UNORM:
		/*
		 * A depth target, which may also be sampled (and compared against)
		 * and copied to and from buffers; Y-tiled, so optimal only.
		 */
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
			VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
			VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
			VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
		break;
	default:
		break;
	}

	/*
	 * A format the vertex fetcher reads may be a vertex buffer's.  Without
	 * the feature a client copies the vertices out of its own bytes, which
	 * miss what the device wrote into the buffer (transform feedback).
	 */
	supported = drv_i915_gfx_vertex_format_supported(format);
	if (supported != 0)
		properties->bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;

	/* A format the sampler reads from a buffer may be a uniform texel buffer's (a GL buffer texture). */
	error = drv_i915_gfx_texel_buffer_format(format, &surface_format, &texel_bytes);
	if (error == 0)
		properties->bufferFeatures |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
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
	struct i915_render_session *session,
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
	i915_instance_format_features(format, session->vk->video, &properties);
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
	struct i915_render_session *session,
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
	int depth;

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
	i915_instance_format_features(format, session->vk->video, &properties);
	kern_memset(&image, 0, sizeof(image));
	features = properties.optimalTilingFeatures;
	if (tiling == VK_IMAGE_TILING_LINEAR)
		features = properties.linearTilingFeatures;

	/* An NV12 picture of the video decoder has its own limits. */
	if (format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
		i915_instance_video_image_properties(type, tiling, usage, features, reply);
		return 0;
	}

	/* Finds the features the usage needs; a usage the executor never implements needs a feature no format has. */
	required = i915_instance_usage_features(usage);

	/*
	 * A format without features in that tiling, an image that is not 1D, 2D
	 * or 3D (a depth image 2D) and a usage the features do not cover reply
	 * VK_ERROR_FORMAT_NOT_SUPPORTED.  XXX: the create flags are not
	 * consulted; a cube compatible image is a 2D one.
	 */
	depth = 0;
	if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM ||
	    format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_S8_UINT)
		depth = 1;
	if (features == 0U ||
	    (type != VK_IMAGE_TYPE_1D && type != VK_IMAGE_TYPE_2D && type != VK_IMAGE_TYPE_3D) ||
	    (depth != 0 && type != VK_IMAGE_TYPE_2D) ||
	    (features & required) != required) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED);
		drv_i915_wire_reply_u64(reply, 1U);
		i915_vkc_enc_VkImageFormatProperties(reply, &image);
		return 0;
	}

	/*
	 * Describes an image of one sample (image.c): a 1D image is one texel
	 * high, a 3D one up to 2048 deep with one layer, the others up to 2048
	 * layers; a colour image has its levels down to one texel (15 for
	 * 16384), a depth image one level and one layer.
	 */
	image.maxExtent.width = 16384U;
	image.maxExtent.height = 16384U;
	if (type == VK_IMAGE_TYPE_1D)
		image.maxExtent.height = 1U;
	image.maxExtent.depth = 1U;
	if (type == VK_IMAGE_TYPE_3D)
		image.maxExtent.depth = 2048U;
	image.maxMipLevels = 15U;
	image.maxArrayLayers = 2048U;
	if (type == VK_IMAGE_TYPE_3D)
		image.maxArrayLayers = 1U;
	if (depth != 0)
		image.maxMipLevels = 1U;

	/*
	 * One sample; two or four for an optimal 2D image that is rendered to
	 * and not stored: a colour image may also be sampled (texelFetch of a
	 * sampler2DMS reads its samples as layers, ws075-p006), a depth or
	 * stencil one not (its samples are interleaved); a resource of up to
	 * 1 GiB.
	 */
	image.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
	if (type == VK_IMAGE_TYPE_2D &&
	    tiling == VK_IMAGE_TILING_OPTIMAL &&
	    (usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0U) {
		if ((features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0U) {
			/* A colour target, sampled or not. */
			image.sampleCounts = I915_INSTANCE_ATTACHMENT_SAMPLES;
		} else if ((features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U &&
			   (usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0U) {
			/* A depth or stencil target that is not sampled. */
			image.sampleCounts = I915_INSTANCE_ATTACHMENT_SAMPLES;
		}
	}
	image.maxResourceSize = 1ULL << 30;

	/* Replies VK_SUCCESS, the present word and the record. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkImageFormatProperties(reply, &image);

	/* Succeeded: the image format's properties are reported. */
	return 0;
}

/*
 * Replies the properties of an NV12 image: a 2D picture in Y tiles used only
 * as decode output, reference pictures and transfer source, of one level, one
 * layer and one sample, at most 4096 a side (design §3.4).
 */
static void
i915_instance_video_image_properties(
	uint32_t type,
	uint32_t tiling,
	uint32_t usage,
	uint32_t features,
	struct i915_wire_writer *reply)
{
	VkImageFormatProperties image;
	uint32_t video_usage;

	/* Refuses a device without video decode, another type or tiling, and another usage. */
	kern_memset(&image, 0, sizeof(image));
	video_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (features == 0U ||
	    type != VK_IMAGE_TYPE_2D ||
	    tiling != VK_IMAGE_TILING_OPTIMAL ||
	    usage == 0U ||
	    (usage & ~video_usage) != 0U) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED);
		drv_i915_wire_reply_u64(reply, 1U);
		i915_vkc_enc_VkImageFormatProperties(reply, &image);
		return;
	}

	/* One picture of one sample, at most 4096 a side, within 1 GiB. */
	image.maxExtent.width = 4096U;
	image.maxExtent.height = 4096U;
	image.maxExtent.depth = 1U;
	image.maxMipLevels = 1U;
	image.maxArrayLayers = 1U;
	image.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
	image.maxResourceSize = 1ULL << 30;

	/* Replies VK_SUCCESS, the present word and the record. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkImageFormatProperties(reply, &image);
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
	uint32_t family;
	void *token;

	/*
	 * Skips the device, the queue info and its chained timeline record up
	 * to the family, reads the family, skips the queue index and the
	 * present word, and reads the identity.  XXX: every timeline is served
	 * on RCS0 (the submissions run to their end in order); the timeline
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
	family = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The family is the graphics family, or the video family of a device that offers it. */
	token = &i915_instance_token;
	if (family == 1U && session->vk->video)
		token = &i915_instance_video_queue_token;
	else if (family != 0U)
		return EINVAL;

	/* Remembers the queue and its family; a failure to remember it is not reported, and the reply is the same. */
	(void)drv_i915_object_insert(session, I915_VK_OBJ_QUEUE, identity, token);

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
