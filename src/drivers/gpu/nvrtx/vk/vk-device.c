/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's instance, physical-device, device and queue commands.
 *
 * Each command's wire is the one libvulkan sends (userland/desktop/
 * libvulkan: instance.c, device.c), and its records travel through the
 * generated codec, so the two ends cannot drift.  The device is one
 * physical device with one queue family (graphics, compute and transfer)
 * of one queue, and one memory type: the VRAM the CPU reaches through its
 * window, so it is device-local, host-visible and host-coherent.  The
 * formats are the ones the executor lays out (pitch-linear only, so both
 * tilings have the same features); the limits are values a Turing part
 * meets, not a survey of its maxima.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/compiler/ir.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-vk.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The bits of the float constants the limits are filled with. */
#define DEVICE_F32_ONE			0x3f800000U
#define DEVICE_F32_16			0x41800000U
#define DEVICE_F32_SIXTEENTH		0x3d800000U
#define DEVICE_F32_MINUS_32768		0xc7000000U
#define DEVICE_F32_32767		0x46fffe00U
#define DEVICE_F32_POINT_MAX		0x44fffe14U

/*
 * NVIDIA's PCI vendor, and the version and driver version the device
 * reports (libvulkan needs a native 1.1).
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/headers/nv_device_info.h
 * (NVIDIA_VENDOR_ID).
 */
#define DEVICE_VENDOR			0x10deU
#define DEVICE_API_VERSION		VK_MAKE_VERSION(1, 1, 0)
#define DEVICE_DRIVER_VERSION		1U

/* The timelines a queue may complete jobs on: 1 to 63 (0 is the context's own). */
#define DEVICE_TIMELINES		64U

/* The levels a 3D image has at most: its largest side halved to one. */
#define DEVICE_3D_LEVELS		12U

/* The features a format the executor renders to, samples and copies has. */
#define DEVICE_COLOUR_FEATURES	(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
				 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | \
				 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | \
				 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_DST_BIT | \
				 VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
				 VK_FORMAT_FEATURE_BLIT_DST_BIT)

/*
 * The features of an integer format: rendered to, sampled and copied, not
 * blended or filtered, nor blitted (a blit's quad samples through floats).
 */
#define DEVICE_INTEGER_FEATURES	(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
				 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

/*
 * The features of a colour format that is sampled but not rendered to (a
 * shared exponent one): sampled and filtered, copied, a blit's source (a
 * blit's quad samples it).
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/nil/nil_formats.csv (the
 * support column T, without C, of R9G9B9E5_FLOAT).
 */
#define DEVICE_SAMPLED_FEATURES	(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
				 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_DST_BIT | \
				 VK_FORMAT_FEATURE_BLIT_SRC_BIT)

/*
 * The features of a depth format, in optimal tiling: a depth attachment,
 * sampled (its depth) and filtered, copied by the copy engine.
 */
#define DEVICE_DEPTH_FEATURES	(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | \
				 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
				 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

/*
 * What the object registry holds for an instance, a physical device, a
 * device and a queue: the executor only remembers that they exist, so
 * every one of them stands for this token, which lives as long as the
 * kernel and is never freed.
 */
static int device_token;

static int device_create_instance(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_enumerate(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_properties(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_identity(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static VkPhysicalDeviceProperties *device_describe(struct nvrtx_vk_session *session);
static void device_limits(VkPhysicalDeviceLimits *limits);
static int device_features(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static VkPhysicalDeviceFeatures *device_feature_record(struct nvrtx_vk_session *session);
static int device_features2(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_properties2(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void device_core11(struct nvrtx_vk_session *session, struct i915_vkc_core11 *core11);
static int device_memory(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_families(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void device_format_features(uint32_t format, VkFormatProperties *properties);
static uint32_t device_usage_features(uint32_t usage);
static int device_format(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_image_format(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_create(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_queue(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_destroy(struct nvrtx_vk_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int device_wait_idle(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_token_destroy(struct nvrtx_vk_session *session, void *payload);
static int device_remember(struct nvrtx_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
static void device_set_float(float *destination, uint32_t bits);

/*
 * Runs an instance, physical-device, device or queue command; *handled
 * stays clear for an opcode of another handler's.
 */
int
nvrtx_vk_device_route(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	UNUSED_PARAMETER(requested);

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_INSTANCE:
		error = device_create_instance(session, reader, reply);
		break;
	case GPU_OP_DESTROY_INSTANCE:
		error = device_destroy(session, I915_VK_OBJ_INSTANCE, reader);
		break;
	case GPU_OP_ENUMERATE_PHYSICAL_DEVICES:
		error = device_enumerate(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FEATURES:
		error = device_features(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FEATURES2:
		error = device_features2(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES:
		error = device_format(reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES:
		error = device_image_format(reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES:
		error = device_properties(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2:
		error = device_properties2(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES:
		error = device_families(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES:
		error = device_memory(session, reader, reply);
		break;
	case GPU_OP_CREATE_DEVICE:
		error = device_create(session, reader, reply);
		break;
	case GPU_OP_DESTROY_DEVICE:
		error = device_destroy(session, I915_VK_OBJ_DEVICE, reader);
		break;
	case GPU_OP_QUEUE_WAIT_IDLE:
	case GPU_OP_DEVICE_WAIT_IDLE:
		error = device_wait_idle(session, reader, reply);
		break;
	case GPU_OP_GET_DEVICE_QUEUE2:
		error = device_queue(session, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Reports why the command was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/* vkCreateInstance: [present][create info][allocator][present][id] -> [result][present][id]. */
static int
device_create_instance(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkInstanceCreateInfo info;
	uint64_t present;
	uint64_t identity;
	int error;

	/* The create info, when present; nothing of it is kept. */
	kern_memset(&info, 0, sizeof(info));
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkInstanceCreateInfo(reader, &session->arena, &info);

	/* The allocator and the output's present word, then the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Remembers the instance. */
	error = device_remember(session, I915_VK_OBJ_INSTANCE, identity);
	if (error != 0)
		return error;

	/* Replies success and the identity. */
	drv_i915_wire_reply_u32(reply, VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the instance exists. */
	return 0;
}

/*
 * vkEnumeratePhysicalDevices: [instance][present][count][array count][ids]
 * -> [result][present][count][array count][ids].  There is one physical device.
 */
static int
device_enumerate(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t array_count;
	uint64_t identity;
	uint64_t index;
	uint32_t count;

	/* The instance and the count's present word, then the counts. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	array_count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An array of more than the one device, or one without a count of one, is malformed. */
	if (array_count > 1U)
		return EINVAL;
	if (array_count != 0U && count != 1U)
		return EINVAL;

	/* Replies success and one device. */
	drv_i915_wire_reply_u32(reply, VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u64(reply, array_count);

	/* Remembers and echoes the identity the library chose for it. */
	for (index = 0U; index < array_count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;

		/* A device enumerated again is already remembered; the reply is the same. */
		(void)device_remember(session, I915_VK_OBJ_PHYSICAL_DEVICE, identity);
		drv_i915_wire_reply_u64(reply, identity);
	}

	/* Succeeded: the device is reported. */
	return 0;
}

/* vkGetPhysicalDeviceProperties: [physical][present] -> [present][properties]. */
static int
device_properties(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceProperties *properties;

	/* The physical device and the output's present word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The record. */
	properties = device_describe(session);
	if (properties == NULL)
		return ENOMEM;

	/* Replies the present word and the record. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkPhysicalDeviceProperties(reply, properties);

	/* Succeeded: the properties are reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceProperties2 in either of its forms: [physical]
 * [present][PROPERTIES_2 header][the chain's headers] -> the device's
 * identity (device_identity()) or its Vulkan 1.1 properties.
 */
static int
device_properties2(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vkc_core11 core11;
	VkPhysicalDeviceProperties *properties;
	int form;
	int error;

	/* The physical device, then the output chain, which says which form is asked for. */
	(void)drv_i915_wire_read_u64(reader);
	error = i915_vkc_read_properties2(reader, &form);
	if (error != 0) {
		reader->error = 1;
		return error;
	}

	/* The identity, which the export and import of memory name. */
	if (form == I915_VKC_PROPERTIES2_IDENTITY) {
		error = device_identity(session, reader, reply);
		if (error != 0)
			return error;

		/* Succeeded: the identity is reported. */
		return 0;
	}

	/* The Vulkan 1.1 properties, then the record. */
	properties = device_describe(session);
	if (properties == NULL)
		return ENOMEM;
	device_core11(session, &core11);
	i915_vkc_reply_core11_properties(reply, &core11);
	i915_vkc_enc_VkPhysicalDeviceProperties(reply, properties);

	/* Succeeded: the Vulkan 1.1 properties are reported. */
	return 0;
}

/*
 * The identity form of vkGetPhysicalDeviceProperties2 (only the ID
 * properties chained), which libvulkan asks before it exports or imports
 * memory by fd: the two headers, the device's and the driver's UUIDs, an
 * invalid LUID, then the properties.  Both sides of a share ask the same
 * device, so the UUIDs are fixed words of this executor.
 */
static int
device_identity(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	static const uint8_t device_uuid[VK_UUID_SIZE] = {
		'z', 'e', 'd', 'B', 'S', 'D', '-', 'n', 'v', 'r', 't', 'x', 'T', 'U', '1', 'x'
	};
	static const uint8_t driver_uuid[VK_UUID_SIZE] = {
		'z', 'e', 'd', 'B', 'S', 'D', '-', 'n', 'v', 'r', 't', 'x', '-', 'v', 'k', '1'
	};
	static const uint8_t luid[VK_LUID_SIZE] = { 0 };
	VkPhysicalDeviceProperties *properties;

	UNUSED_PARAMETER(reader);

	/* The record. */
	properties = device_describe(session);
	if (properties == NULL)
		return ENOMEM;

	/* The headers, the identifiers and the properties. */
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
	drv_i915_wire_reply_u64(reply, 0U);
	drv_i915_wire_reply_u64(reply, VK_UUID_SIZE);
	i915_vkc_reply_bytes(reply, device_uuid, VK_UUID_SIZE);
	drv_i915_wire_reply_u64(reply, VK_UUID_SIZE);
	i915_vkc_reply_bytes(reply, driver_uuid, VK_UUID_SIZE);
	drv_i915_wire_reply_u64(reply, VK_LUID_SIZE);
	i915_vkc_reply_bytes(reply, luid, VK_LUID_SIZE);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u32(reply, VK_FALSE);
	i915_vkc_enc_VkPhysicalDeviceProperties(reply, properties);

	/* Succeeded: the identity is reported. */
	return 0;
}

/*
 * The device's properties in a cleared record from the session's scratch
 * (too large for the stack): NVIDIA's, discrete, its PCI product, and the
 * limits.  NULL when the scratch is full.
 */
static VkPhysicalDeviceProperties *
device_describe(
	struct nvrtx_vk_session *session)
{
	static const char name[] = "zedBSD nvrtx (NVIDIA Turing)";
	VkPhysicalDeviceProperties *properties;

	/* A cleared record. */
	properties = nvrtx_vk_arena_take(session, sizeof(*properties));
	if (properties == NULL)
		return NULL;

	/* Names the device. */
	properties->apiVersion = DEVICE_API_VERSION;
	properties->driverVersion = DEVICE_DRIVER_VERSION;
	properties->vendorID = DEVICE_VENDOR;
	properties->deviceID = nvrtx_vk_device_id(session->owner);
	properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
	kern_memcpy(properties->deviceName, name, sizeof(name));

	/* The limits. */
	device_limits(&properties->limits);

	/* Succeeded: the record is filled. */
	return properties;
}

/* Fills the limits: values a Turing part meets that applications read. */
static void
device_limits(
	VkPhysicalDeviceLimits *limits)
{
	/* Image, buffer and allocation sizes. */
	limits->maxImageDimension1D = 16384U;
	limits->maxImageDimension2D = 16384U;
	limits->maxImageDimension3D = 2048U;
	limits->maxImageDimensionCube = 16384U;
	limits->maxImageArrayLayers = 2048U;
	limits->maxTexelBufferElements = 1U << 27;
	limits->maxUniformBufferRange = 65536U;
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
	limits->maxFragmentOutputAttachments = NVRTX_EXEC_COLOURS;
	limits->maxFragmentCombinedOutputResources = 4U;

	/* Dual source blending: colour target 0 takes a second colour (the program's location 1 registers). */
	limits->maxFragmentDualSrcAttachments = 1U;

	/*
	 * Geometry programs: the invocations of a primitive, the components a
	 * vertex brings in and takes out (the vertex stage's), the vertices
	 * one emits, and the words all of them take.
	 *
	 * Values referenced: Mesa 25.0.7
	 * src/nouveau/vulkan/nvk_physical_device.c
	 * (maxGeometryShaderInvocations 32, maxGeometryTotalOutputComponents
	 * 1024); the vertices the IR allows.
	 */
	limits->maxGeometryShaderInvocations = DRV_GPU_IR_MAX_GEOMETRY_INVOCATIONS;
	limits->maxGeometryInputComponents = 64U;
	limits->maxGeometryOutputComponents = 64U;
	limits->maxGeometryOutputVertices = DRV_GPU_IR_MAX_OUTPUT_VERTICES;
	limits->maxGeometryTotalOutputComponents = 1024U;

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
	limits->subPixelPrecisionBits = 8U;
	limits->subTexelPrecisionBits = 8U;
	limits->mipmapPrecisionBits = 8U;
	limits->maxDrawIndexedIndexValue = 0xffffffffU;
	limits->maxDrawIndirectCount = NVRTX_VK_DRAW_INDIRECT_MAX;
	device_set_float(&limits->maxSamplerLodBias, DEVICE_F32_16);
	device_set_float(&limits->maxSamplerAnisotropy, DEVICE_F32_ONE);

	/* Viewports. */
	limits->maxViewports = 1U;
	limits->maxViewportDimensions[0] = 16384U;
	limits->maxViewportDimensions[1] = 16384U;
	device_set_float(&limits->viewportBoundsRange[0], DEVICE_F32_MINUS_32768);
	device_set_float(&limits->viewportBoundsRange[1], DEVICE_F32_32767);

	/* Alignments and texel offsets. */
	limits->minMemoryMapAlignment = 4096U;
	limits->minTexelBufferOffsetAlignment = 16U;
	limits->minUniformBufferOffsetAlignment = 256U;
	limits->minStorageBufferOffsetAlignment = 64U;
	limits->minTexelOffset = -8;
	limits->maxTexelOffset = 7U;
	limits->minTexelGatherOffset = -8;
	limits->maxTexelGatherOffset = 7U;
	limits->subPixelInterpolationOffsetBits = 4U;

	/*
	 * Framebuffers: one or four samples a pixel (a colour one's sampled, its
	 * samples fetched; depth ones not), four colour
	 * attachments (a render pass draws into four targets), as many layers
	 * as an image has (a layered pass, nvk's 2048); a pass of no attachment
	 * draws nothing.
	 */
	limits->maxFramebufferWidth = 16384U;
	limits->maxFramebufferHeight = 16384U;
	limits->maxFramebufferLayers = NVRTX_VK_IMAGE_LAYERS_MAX;
	limits->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
	limits->framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
	limits->framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
	limits->framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->maxColorAttachments = NVRTX_EXEC_COLOURS;
	limits->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
	limits->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->maxSampleMaskWords = 1U;

	/*
	 * Timestamps on the graphics and compute queue, a nanosecond a tick
	 * (the report semaphore's timestamp, in nanoseconds).
	 *
	 * Values referenced: Mesa 25.0.7
	 * src/nouveau/vulkan/nvk_physical_device.c (timestampComputeAndGraphics,
	 * timestampPeriod 1.0).
	 */
	limits->timestampComputeAndGraphics = VK_TRUE;
	device_set_float(&limits->timestampPeriod, DEVICE_F32_ONE);

	/*
	 * Clipping, queue priorities, points and lines: a point's size from
	 * 1 to 2047.94 in sixteenths (the rasterizer's), lines one pixel wide
	 * (wide lines are not drawn).
	 *
	 * Values referenced: Mesa 25.0.7
	 * src/nouveau/vulkan/nvk_physical_device.c (pointSizeRange,
	 * pointSizeGranularity).
	 */
	limits->maxClipDistances = 8U;
	limits->maxCullDistances = 8U;
	limits->maxCombinedClipAndCullDistances = 8U;
	limits->discreteQueuePriorities = 2U;
	device_set_float(&limits->pointSizeRange[0], DEVICE_F32_ONE);
	device_set_float(&limits->pointSizeRange[1], DEVICE_F32_POINT_MAX);
	device_set_float(&limits->pointSizeGranularity, DEVICE_F32_SIXTEENTH);
	device_set_float(&limits->lineWidthRange[0], DEVICE_F32_ONE);
	device_set_float(&limits->lineWidthRange[1], DEVICE_F32_ONE);
	limits->strictLines = VK_FALSE;
	limits->standardSampleLocations = VK_TRUE;

	/* Copy and coherence granularities. */
	limits->optimalBufferCopyOffsetAlignment = 64U;
	limits->optimalBufferCopyRowPitchAlignment = 64U;
	limits->nonCoherentAtomSize = 64U;
}

/*
 * vkGetPhysicalDeviceFeatures: [physical][present] -> [present][features]:
 * the optional features the executor has, the rest off.  Cube map arrays
 * are sampled (their views' texture header, the hardware's cube map array
 * type); geometry programs run (of points, lines and triangles, with
 * adjacency) and may write the point size; points take their size from
 * the last program before the rasterizer (large points); each colour
 * target has its own blend (independent blend); the colour targets take
 * a logic operation (logic op); the depth may be clamped instead of
 * clipped (depth clamp), its bias clamped (depth bias clamp); polygons
 * are drawn as lines or points too (fill mode non solid); colour target
 * 0 blends with a second colour of the fragment program (dual source
 * blend).
 */
static int
device_features(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceFeatures *features;

	/* The physical device and the output's present word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The record of the device's features. */
	features = device_feature_record(session);
	if (features == NULL)
		return ENOMEM;

	/* Replies the present word and the record. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkPhysicalDeviceFeatures(reply, features);

	/* Succeeded: the features are reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceFeatures2 with the Vulkan 1.1 features chained:
 * [physical][output chain] -> [present][chain][1.1 fields]
 * [VkPhysicalDeviceFeatures].
 */
static int
device_features2(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vkc_core11 core11;
	VkPhysicalDeviceFeatures *features;
	int error;

	/* The physical device, then the output chain of the 1.1 feature structures. */
	(void)drv_i915_wire_read_u64(reader);
	error = i915_vkc_read_features2(reader);
	if (error != 0) {
		reader->error = 1;
		return error;
	}

	/* The record of the device's features. */
	features = device_feature_record(session);
	if (features == NULL)
		return ENOMEM;

	/* The 1.1 features, then the record. */
	device_core11(session, &core11);
	i915_vkc_reply_core11_features(reply, &core11);
	i915_vkc_enc_VkPhysicalDeviceFeatures(reply, features);

	/* Succeeded: the features are reported. */
	return 0;
}

/* A cleared record of the device's core features from the session's scratch; NULL when it is full. */
static VkPhysicalDeviceFeatures *
device_feature_record(
	struct nvrtx_vk_session *session)
{
	VkPhysicalDeviceFeatures *features;

	/* A cleared record: every optional feature off but those the executor has. */
	features = nvrtx_vk_arena_take(session, sizeof(*features));
	if (features == NULL)
		return NULL;
	features->imageCubeArray = VK_TRUE;
	features->geometryShader = VK_TRUE;
	features->multiDrawIndirect = VK_TRUE;
	features->drawIndirectFirstInstance = VK_TRUE;
	features->largePoints = VK_TRUE;
	features->independentBlend = VK_TRUE;
	features->dualSrcBlend = VK_TRUE;
	features->logicOp = VK_TRUE;
	features->depthClamp = VK_TRUE;
	features->depthBiasClamp = VK_TRUE;
	features->fillModeNonSolid = VK_TRUE;
	features->shaderTessellationAndGeometryPointSize = VK_TRUE;
	features->occlusionQueryPrecise = VK_TRUE;
	features->vertexPipelineStoresAndAtomics = VK_TRUE;

	/* Succeeded: the record lives until the command returns. */
	return features;
}

/*
 * Fills the device's Vulkan 1.1 answers: the subgroup of one invocation
 * every executor starts from; multiview, each view of a subpass drawn into
 * its layer with programs compiled for it (vk-execute.c: execute_views),
 * the geometry stage among them, instances numbered as without multiview;
 * a set's descriptors are bounded by the per-set limits (each binding at
 * most NVRTX_VK_ELEMENTS_MAX), which together stay below 1024; an
 * allocation may take the whole video memory heap.
 */
static void
device_core11(
	struct nvrtx_vk_session *session,
	struct i915_vkc_core11 *core11)
{
	/* What every executor starts from. */
	i915_vkc_core11_defaults(core11);

	/* Multiview, with the geometry stage. */
	core11->multiview.multiview = VK_TRUE;
	core11->multiview.multiviewGeometryShader = VK_TRUE;
	core11->multiview_limits.maxMultiviewViewCount = NVRTX_VK_VIEWS_MAX;
	core11->multiview_limits.maxMultiviewInstanceIndex = (1U << 27) - 1U;

	/* The descriptors of one set and the largest allocation. */
	core11->maintenance3.maxPerSetDescriptors = 1024U;
	core11->maintenance3.maxMemoryAllocationSize = nvrtx_vk_heap_bytes(session->owner);
}

/*
 * vkGetPhysicalDeviceMemoryProperties: [physical][present][type extent][heap extent]
 * -> [present][properties].  One type and one heap: the VRAM the CPU
 * reaches through its window.
 */
static int
device_memory(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceMemoryProperties *memory;

	/* The physical device, the present word and the two extents. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A cleared record from the scratch. */
	memory = nvrtx_vk_arena_take(session, sizeof(*memory));
	if (memory == NULL)
		return ENOMEM;

	/*
	 * The types: device-local, which the CPU's window makes host-visible
	 * and coherent (uncached); and RAM, host-visible, coherent and cached
	 * (the GPU reaches it over PCI Express with snooped accesses), for
	 * buffers the CPU reads back.  Neither's properties are a subset of
	 * the other's, so the VRAM's stays first for whoever takes the first
	 * host-visible type.
	 */
	memory->memoryTypeCount = NVRTX_VK_MEMORY_TYPES;
	memory->memoryTypes[NVRTX_VK_MEMORY_TYPE].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	memory->memoryTypes[NVRTX_VK_MEMORY_TYPE].heapIndex = 0U;
	memory->memoryTypes[NVRTX_VK_MEMORY_SYSTEM].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	    VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	memory->memoryTypes[NVRTX_VK_MEMORY_SYSTEM].heapIndex = 1U;

	/* The heaps: what the window holds, and the share of the RAM. */
	memory->memoryHeapCount = 2U;
	memory->memoryHeaps[0].size = nvrtx_vk_heap_bytes(session->owner);
	memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
	memory->memoryHeaps[1].size = NVRTX_VK_SYSTEM_HEAP_BYTES;
	memory->memoryHeaps[1].flags = 0U;

	/* Replies the present word and the record. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkPhysicalDeviceMemoryProperties(reply, memory);

	/* Succeeded: the memory is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceQueueFamilyProperties: [physical][present][count][array count]
 * -> [present][count][array count][families].  One family of one queue.
 */
static int
device_families(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkQueueFamilyProperties family;
	uint64_t array_count;
	bool video;

	/* The physical device, the present word and the count, then the array's count. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	array_count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An array of more than the one family is malformed. */
	if (array_count > 1U)
		return EINVAL;

	/* The family: graphics with compute and transfer, one queue, any granularity. */
	kern_memset(&family, 0, sizeof(family));
	family.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
	family.queueCount = 1U;

	/* The same family decodes video when the device has the video decoder. */
	video = nvrtx_vk_video_available(session->owner);
	if (video)
		family.queueFlags |= VK_QUEUE_VIDEO_DECODE_BIT_KHR;

	/*
	 * Timestamps of 64 bits (the report semaphore's).
	 *
	 * Value referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_physical_device.c
	 * (timestampValidBits 64).
	 */
	family.timestampValidBits = 64U;

	/* Any image transfer may start at any texel. */
	family.minImageTransferGranularity.width = 1U;
	family.minImageTransferGranularity.height = 1U;
	family.minImageTransferGranularity.depth = 1U;

	/* Replies the present word, the count and the family when the array holds it. */
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u64(reply, array_count);
	if (array_count == 1U)
		i915_vkc_enc_VkQueueFamilyProperties(reply, &family);

	/* Succeeded: the family is reported. */
	return 0;
}

/*
 * Reports what the executor does with a format.  A colour format has the
 * same features in both tilings (a linear image is pitch linear, an
 * optimal one may be either); a depth format is optimal only; the vertex
 * formats the compositor's quads read are a vertex buffer's; the colour
 * formats rendered to but the sRGB ones are a uniform texel buffer's.
 */
static void
device_format_features(
	uint32_t format,
	VkFormatProperties *properties)
{
	uint32_t code;
	bool colour;
	bool integer;
	bool rendered;
	bool depth;

	kern_memset(properties, 0, sizeof(*properties));

	/*
	 * The colour formats (nvrtx_exec_format_find()): rendered to, sampled
	 * and copied; blended, filtered and blitted unless integer ones; only
	 * sampled, copied and blitted from when they have no target format.
	 */
	colour = nvrtx_vk_format_colour(format);
	integer = nvrtx_exec_format_integer(format);
	rendered = nvrtx_exec_target_format(format, &code);
	if (colour) {
		properties->optimalTilingFeatures = DEVICE_COLOUR_FEATURES;
		if (integer)
			properties->optimalTilingFeatures = DEVICE_INTEGER_FEATURES;
		if (!rendered)
			properties->optimalTilingFeatures = DEVICE_SAMPLED_FEATURES;
		properties->linearTilingFeatures = properties->optimalTilingFeatures;
	}

	/* The depth formats (nvrtx_exec_depth_format_find()), of optimal tiling only. */
	depth = nvrtx_vk_format_depth(format);
	if (depth)
		properties->optimalTilingFeatures = DEVICE_DEPTH_FEATURES;

	/* The video pictures. */
	switch (format) {
	case NVRTX_VK_FORMAT_NV12:
		/*
		 * The video decoder's pictures, of optimal (block-linear) tiling
		 * only: decoded into, references, copied out, their planes sampled
		 * through R8 and R8G8 views.  The video features are reported
		 * whether or not the device has the decoder: libvulkan withholds
		 * them from the application when the capset's native record
		 * promises no video decode family, which runtime_capset promises
		 * only when the device has the decoder.
		 */
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_VIDEO_DECODE_OUTPUT_BIT_KHR |
						    VK_FORMAT_FEATURE_VIDEO_DECODE_DPB_BIT_KHR |
						    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
						    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		break;
	default:
		break;
	}

	/* The vertex formats, and the formats a uniform texel buffer's texels are fetched in. */
	if (nvrtx_exec_vertex_format(format))
		properties->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
	if (nvrtx_exec_texel_format(format))
		properties->bufferFeatures |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
}

/* Reports the format features an image usage needs. */
static uint32_t
device_usage_features(
	uint32_t usage)
{
	uint32_t required;

	required = 0U;

	/* A copy out needs the transfer source feature. */
	if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;

	/* A copy in needs the transfer destination feature. */
	if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

	/* Sampling needs the sampled image feature. */
	if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

	/* A storage image needs the storage feature, which no format has. */
	if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;

	/* Rendering colour needs the colour attachment feature. */
	if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;

	/* Decoding into the image needs the video output feature. */
	if ((usage & VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR) != 0U)
		required |= VK_FORMAT_FEATURE_VIDEO_DECODE_OUTPUT_BIT_KHR;

	/* Keeping the image as a reference needs the video DPB feature. */
	if ((usage & VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR) != 0U)
		required |= VK_FORMAT_FEATURE_VIDEO_DECODE_DPB_BIT_KHR;

	/* Rendering depth needs the depth attachment feature (the depth formats'). */
	if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;

	/* Succeeded: the features the usage needs. */
	return required;
}

/* vkGetPhysicalDeviceFormatProperties: [physical][format][present] -> [present][properties]. */
static int
device_format(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFormatProperties properties;
	uint32_t format;

	/* The physical device, the format and the present word. */
	(void)drv_i915_wire_read_u64(reader);
	format = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Replies the present word and the format's features. */
	device_format_features(format, &properties);
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkFormatProperties(reply, &properties);

	/* Succeeded: the format is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceImageFormatProperties: [physical][format][type][tiling][usage][flags][present]
 * -> [result][present][properties].  An image of one sample: of a colour
 * format 1D or 2D of layers (a cube map's of square faces) or 3D of one
 * layer, layered or 3D only in optimal tiling; of a depth format 2D of
 * levels and layers (a cube map's too); of a video format 2D of one level
 * and layer.  Four samples too for a 2D attachment (a colour one sampled or not).
 */
static int
device_image_format(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageFormatProperties image;
	VkFormatProperties properties;
	uint32_t format;
	uint32_t type;
	uint32_t tiling;
	uint32_t usage;
	uint32_t flags;
	uint32_t features;
	uint32_t required;
	bool supported;
	bool plain;
	bool depth;
	bool multisampled;

	/* The format, type, tiling, usage and flags; the physical device and present word are skipped. */
	(void)drv_i915_wire_read_u64(reader);
	format = drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	tiling = drv_i915_wire_read_u32(reader);
	usage = drv_i915_wire_read_u32(reader);
	flags = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The features of the tiling asked about, and what the usage needs. */
	device_format_features(format, &properties);
	features = properties.optimalTilingFeatures;
	if (tiling == VK_IMAGE_TILING_LINEAR)
		features = properties.linearTilingFeatures;
	required = device_usage_features(usage);
	kern_memset(&image, 0, sizeof(image));

	/*
	 * Supported: a format with the features the usage needs; a depth or
	 * video format 2D (a video format not cube compatible), a colour
	 * format of any type, a type other than 2D only in optimal tiling,
	 * cube compatible only 2D.
	 */
	plain = false;
	if (format == NVRTX_VK_FORMAT_NV12)
		plain = true;
	depth = nvrtx_vk_format_depth(format);
	if (depth && type != VK_IMAGE_TYPE_2D)
		features = 0U;
	supported = true;
	if (features == 0U || (features & required) != required) {
		supported = false;
	} else if (type != VK_IMAGE_TYPE_1D && type != VK_IMAGE_TYPE_2D && type != VK_IMAGE_TYPE_3D) {
		supported = false;
	} else if (type != VK_IMAGE_TYPE_2D && (plain || tiling != VK_IMAGE_TILING_OPTIMAL)) {
		supported = false;
	} else if ((flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0U && (type != VK_IMAGE_TYPE_2D || plain)) {
		supported = false;
	}
	if (!supported) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED);
		drv_i915_wire_reply_u64(reply, 1U);
		i915_vkc_enc_VkImageFormatProperties(reply, &image);
		return 0;
	}

	/*
	 * Every level and layer of a colour or depth image (a layer of linear
	 * tiling; one level and layer of a video image), one sample, up to
	 * 16384 a side (2048 for 3D) and 1 GiB.
	 */
	image.maxExtent.width = NVRTX_VK_IMAGE_SIDE_MAX;
	image.maxExtent.height = NVRTX_VK_IMAGE_SIDE_MAX;
	image.maxExtent.depth = 1U;
	image.maxMipLevels = NVRTX_VK_LEVELS_MAX;
	image.maxArrayLayers = NVRTX_VK_IMAGE_LAYERS_MAX;
	if (type == VK_IMAGE_TYPE_1D)
		image.maxExtent.height = 1U;
	if (type == VK_IMAGE_TYPE_3D) {
		image.maxExtent.width = NVRTX_VK_IMAGE_3D_SIDE_MAX;
		image.maxExtent.height = NVRTX_VK_IMAGE_3D_SIDE_MAX;
		image.maxExtent.depth = NVRTX_VK_IMAGE_3D_SIDE_MAX;
		image.maxMipLevels = DEVICE_3D_LEVELS;
		image.maxArrayLayers = 1U;
	}
	if (tiling == VK_IMAGE_TILING_LINEAR)
		image.maxArrayLayers = 1U;
	if (plain) {
		image.maxMipLevels = 1U;
		image.maxArrayLayers = 1U;
	}
	image.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
	image.maxResourceSize = 1ULL << 30;

	/*
	 * Four samples too for a 2D attachment of optimal tiling not stored
	 * and not cube compatible: a colour one of a format that blends (no
	 * integer one), sampled or not, a depth one not sampled.
	 */
	multisampled = false;
	if (type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) == 0U &&
	    (usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0U)
		multisampled = true;
	if (depth && (usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U)
		multisampled = false;
	if (multisampled && (features & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0U)
		image.sampleCounts |= VK_SAMPLE_COUNT_4_BIT;

	/* Replies success, the present word and the record. */
	drv_i915_wire_reply_u32(reply, VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkImageFormatProperties(reply, &image);

	/* Succeeded: the image format is reported. */
	return 0;
}

/* vkCreateDevice: [physical][present][create info][allocator][present][id] -> [result][present][id]. */
static int
device_create(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vkc_core11 core11;
	VkDeviceCreateInfo info;
	uint64_t present;
	uint64_t identity;
	int error;

	/* The physical device, then the create info when present; only its 1.1 features are checked. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkDeviceCreateInfo(reader, &session->arena, &info);

	/* The allocator and the output's present word, then the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses a Vulkan 1.1 feature the device does not report (libvulkan refuses it first). */
	device_core11(session, &core11);
	error = i915_vkc_core11_check_device(&info, &core11);
	if (error != 0)
		return error;

	/* Remembers the device. */
	error = device_remember(session, I915_VK_OBJ_DEVICE, identity);
	if (error != 0)
		return error;

	/* Replies success and the identity. */
	drv_i915_wire_reply_u32(reply, VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the device exists. */
	return 0;
}

/*
 * vkGetDeviceQueue2 as libvulkan sends it: [device][present][sType]
 * [pNext present][timeline sType][pNext][timeline][flags][family][index]
 * [present][id] -> [present][id].
 */
static int
device_queue(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t identity;
	uint32_t timeline;
	uint32_t family;

	/* Skips up to the timeline, reads it, skips the flags, reads the family, skips the index and present word, reads the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	timeline = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	family = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Only the one family exists; its queue's jobs complete on a timeline of 1 to 63. */
	if (family != 0U || timeline == 0U || timeline >= DEVICE_TIMELINES)
		return EINVAL;

	/* Remembers the queue; one asked for again is already remembered, and the reply is the same. */
	(void)device_remember(session, I915_VK_OBJ_QUEUE, identity);

	/* The session's jobs may now name the queue's timeline (the runtime reads the mask). */
	__atomic_fetch_or(&session->timelines, 1ULL << timeline, __ATOMIC_RELEASE);

	/* Replies the present word and the identity. */
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the queue is known. */
	return 0;
}

/* vkDestroyInstance and vkDestroyDevice: [handle][allocator], no reply body. */
static int
device_destroy(
	struct nvrtx_vk_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader)
{
	uint64_t identity;

	/* The identity, then the allocator. */
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Forgets it; one never remembered is no error. */
	(void)nvrtx_vk_object_remove(session, kind, identity);

	/* Succeeded: the identity is forgotten. */
	return 0;
}

/*
 * vkQueueWaitIdle and vkDeviceWaitIdle: [handle] -> [result].  The
 * session's newest run is waited for; a run that did not end in time
 * loses the device.
 */
static int
device_wait_idle(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t result;
	int error;

	/* The queue or device. */
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Waits for the newest run, and replies whether it did its work. */
	result = (uint32_t)VK_SUCCESS;
	error = nvrtx_vk_gpu_idle(session->owner);
	if (error != 0)
		result = (uint32_t)VK_ERROR_DEVICE_LOST;
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the queue is idle. */
	return 0;
}

/* The token's destructor: the token is static, nothing is freed. */
static int
device_token_destroy(
	struct nvrtx_vk_session *session,
	void *payload)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(payload);

	/* Succeeded: nothing to free. */
	return 0;
}

/* Publishes an identity that stands for the token. */
static int
device_remember(
	struct nvrtx_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct nvrtx_vk_object *object;
	int error;

	/* Publishes it. */
	error = nvrtx_vk_object_publish(session, kind, identity, &device_token, device_token_destroy, &object);
	if (error != 0)
		return error;

	/* Succeeded: the identity is remembered. */
	return 0;
}

/* Fills a float field by its bits, so this file never touches a floating-point register. */
static void
device_set_float(
	float *destination,
	uint32_t bits)
{
	/* Stores the bits unchanged. */
	kern_memcpy(destination, &bits, sizeof(bits));
}
