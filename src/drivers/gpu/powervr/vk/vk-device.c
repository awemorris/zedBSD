/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's instance, physical-device, device and queue commands.
 *
 * The device is one physical device with one queue family of one queue
 * that draws, computes and copies, and two memory types over one heap
 * of uncached system memory: device local, and host visible and
 * coherent.  The formats are the table's (image-state.c); the limits
 * are the ones the executor keeps.  A session opened with a video engine
 * (video-state.c) adds its video to the family and to NV12; memory is
 * exported and imported by the node's handle types.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/compute-program.h"
#include "drivers/gpu/powervr/render-pass.h"
#include "drivers/gpu/powervr/video-state.h"
#include "drivers/gpu/powervr/vk/vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/*
 * The bits of the float constants of the limits: 1 and 2048.
 *
 * Values referenced: IEEE 754-2008 binary32 (sign 31, biased exponent
 * 30:23 with a bias of 127, fraction 22:0).
 */
#define DEVICE_F32_ONE			0x3f800000U

/* The sample counts of attachments and sampled images: one and four. */
#define DEVICE_SAMPLE_COUNTS		(VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)
#define DEVICE_F32_2048			0x45000000U

/* The version the device reports (libvulkan needs a native 1.1) and the driver's version. */
#define DEVICE_API_VERSION		VK_MAKE_VERSION(1, 2, 0)
#define DEVICE_DRIVER_VERSION		1U

/*
 * The core's vendor and device numbers as Vulkan names them: Imagination
 * Technologies' PCI vendor id, and the BXE-4-32's BVNC packed as
 * B << 48 | V << 32 | N << 16 | C, of which the low 32 bits are taken.
 *
 * Values referenced: PCI-SIG vendor id 0x1010 (Imagination Technologies);
 * the BXE-4-32's BVNC 36.50.54.182 as its core identification registers
 * report it.
 */
#define DEVICE_VENDOR_ID		0x1010U
#define DEVICE_ID			((54U << 16) | 182U)

/* The one queue family and the timelines a queue may complete jobs on (1 to 63). */
#define DEVICE_FAMILIES			1U
#define DEVICE_TIMELINES		64U

/* The heap the memory types share. */
#define DEVICE_HEAP_BYTES		POWERVR_RENDER_MAX_BYTES

/*
 * What the registry holds for an instance, a physical device, a device
 * and a queue: the executor only remembers that they exist, so each of
 * them stands for this token, which lives as long as the kernel.
 */
static int device_token;

static int create_instance(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int enumerate(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int properties(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int properties2(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void identity(struct i915_wire_writer *reply, const VkPhysicalDeviceProperties *record);
static int features2(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void device_core11(struct i915_vkc_core11 *core11);
static void device_core12(struct i915_vkc_core12 *core12);
static VkPhysicalDeviceProperties *device_record(struct powervr_vk_session *session);
static void limits(VkPhysicalDeviceLimits *limit);
static int features(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void device_features(VkPhysicalDeviceFeatures *record);
static int memory_properties(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int families(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int family_video(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int format(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int image_format(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int image_format_external(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int buffer_external(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t image_limits(const struct powervr_vk_session *session, const uint32_t words[5], VkImageFormatProperties *record);
static bool usage_allowed(uint32_t usage, uint32_t features);
static bool multisample_allowed(const struct powervr_format *entry, uint32_t features, uint32_t type, uint32_t tiling, uint32_t usage, uint32_t flags);
static bool shape_allowed(const struct powervr_format *entry, uint32_t type, uint32_t tiling, uint32_t usage, uint32_t flags);
static bool planar_allowed(const struct powervr_vk_session *session, uint32_t type, uint32_t tiling, uint32_t usage, uint32_t flags);
static void reply_external(struct i915_wire_writer *reply, uint32_t handle_type);
static int create_device(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_queue(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy(struct powervr_vk_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int wait_idle(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int token_destroy(struct powervr_vk_session *session, void *payload);
static int remember(struct powervr_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
static void set_float(float *destination, uint32_t bits);

/*
 * Runs an instance, physical-device, device or queue command; *handled
 * stays clear for another handler's opcode.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/bcm2711/vk/vulkan-device.c
 * and vulkan-query.c (the wire of each command).
 */
int
powervr_vk_device_route(
	struct powervr_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	(void)requested;
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
		error = features(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FEATURES2:
		error = features2(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES:
		error = format(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES:
		error = image_format(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES2:
		error = image_format_external(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_EXTERNAL_BUFFER_PROPERTIES:
		error = buffer_external(reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES:
		error = properties(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2:
		error = properties2(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES:
		error = families(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES:
		error = family_video(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES:
		error = memory_properties(session, reader, reply);
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

/* vkCreateInstance: [present][create info][allocator][present][id] -> [result][present][id]. */
static int
create_instance(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkInstanceCreateInfo info;
	uint64_t present;
	uint64_t identity;
	int error;

	/* The create info when present; nothing of it is kept. */
	kern_memset(&info, 0, sizeof(info));
	present = drv_i915_wire_read_u64(reader);
	if (present != 0)
		i915_vkc_dec_VkInstanceCreateInfo(reader, &session->arena, &info);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Remembers the instance. */
	error = remember(session, I915_VK_OBJ_INSTANCE, identity);
	if (error != 0)
		return error;

	/* Success and the identity. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the instance exists. */
	return 0;
}

/*
 * vkEnumeratePhysicalDevices: [instance][present][count][array count][ids]
 * -> [result][present][count][array count][ids].  There is one device.
 */
static int
enumerate(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t array_count;
	uint64_t identity;
	uint64_t index;
	uint32_t count;

	/* The instance and the count's presence word, then the counts. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	array_count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (array_count > 1 || (array_count != 0 && count != 1))
		return EINVAL;

	/* Success and one device. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, 1);
	drv_i915_wire_reply_u64(reply, array_count);

	/* Remembers and echoes the identity the library chose; one enumerated again is already remembered. */
	for (index = 0; index < array_count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;
		(void)remember(session, I915_VK_OBJ_PHYSICAL_DEVICE, identity);
		drv_i915_wire_reply_u64(reply, identity);
	}

	/* Succeeded: the device is reported. */
	return 0;
}

/* vkGetPhysicalDeviceProperties: [physical][present] -> [present][properties]. */
static int
properties(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceProperties *record;

	/* The physical device and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The device's record. */
	record = device_record(session);
	if (record == NULL)
		return ENOMEM;

	/* The presence word and the record. */
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkPhysicalDeviceProperties(reply, record);

	/* Succeeded: the properties are reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceProperties2 in either of its forms: [physical]
 * [present][PROPERTIES_2 header][the chain's headers] -> the device's
 * identity (identity()) or its Vulkan 1.1 properties.  ENOTSUP for
 * another chain.
 */
static int
properties2(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceProperties *record;
	struct i915_vkc_core11 core11;
	struct i915_vkc_core12 core12;
	int form;
	int error;

	/* The physical device, then the output chain, which says which form is asked for. */
	(void)drv_i915_wire_read_u64(reader);
	error = i915_vkc_read_properties2(reader, &form);
	if (error != 0) {
		reader->error = 1;
		return error;
	}

	/* The ordinary properties, as the plain query gives them. */
	record = device_record(session);
	if (record == NULL)
		return ENOMEM;

	/* The identity, which the export and import of memory name. */
	if (form == I915_VKC_PROPERTIES2_IDENTITY) {
		identity(reply, record);

		/* Succeeded: the identity every session of the device reports. */
		return 0;
	}

	/* The Vulkan 1.1 or 1.2 properties, then the record. */
	device_core11(&core11);
	device_core12(&core12);
	if (form == I915_VKC_PROPERTIES2_CORE12)
		i915_vkc_reply_core12_properties(reply, &core12);
	else
		i915_vkc_reply_core11_properties(reply, &core11);
	i915_vkc_enc_VkPhysicalDeviceProperties(reply, record);

	/* Succeeded: the properties are reported. */
	return 0;
}

/*
 * The identity form of vkGetPhysicalDeviceProperties2 (only the ID
 * properties chained): [u64 1], the two headers, the device and driver
 * UUIDs, the LUID (none), the node mask, the LUID-valid word and the
 * properties.  The UUIDs pair an exported allocation with its importer:
 * every session of the device reports the same ones.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/videocore/vk/vulkan-query.c
 * (identity_query); include/uapi/gpu-op.h
 * (GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2).
 */
static void
identity(
	struct i915_wire_writer *reply,
	const VkPhysicalDeviceProperties *record)
{
	static const uint8_t device_uuid[VK_UUID_SIZE] = {
		'z', 'e', 'd', 'B', 'S', 'D', '-', 'p', 'o', 'w', 'e', 'r', 'v', 'r', 'b', 'x'
	};
	static const uint8_t driver_uuid[VK_UUID_SIZE] = {
		'z', 'e', 'd', 'B', 'S', 'D', '-', 'p', 'v', 'r', '-', 'v', 'k', '-', '0', '1'
	};
	static const uint8_t luid[VK_LUID_SIZE] = { 0 };

	/* The two headers, the identity (no Windows adapter: no LUID), then the properties. */
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
}

/*
 * vkGetPhysicalDeviceFeatures2 with the Vulkan 1.1 features chained:
 * [physical][output chain] -> [present][chain][1.1 fields]
 * [VkPhysicalDeviceFeatures].
 */
static int
features2(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceFeatures *record;
	struct i915_vkc_core11 core11;
	struct i915_vkc_core12 core12;
	int form;
	int error;

	/* The physical device, then the output chain of the 1.1 or 1.2 feature structures. */
	(void)drv_i915_wire_read_u64(reader);
	error = i915_vkc_read_features2(reader, &form);
	if (error != 0) {
		reader->error = 1;
		return error;
	}

	/* The record of the features the device has. */
	record = powervr_vk_arena_take(session, sizeof(*record));
	if (record == NULL)
		return ENOMEM;
	device_features(record);

	/* The 1.1 or 1.2 features, then the record. */
	device_core11(&core11);
	device_core12(&core12);
	if (form == I915_VKC_FEATURES2_CORE12)
		i915_vkc_reply_core12_features(reply, &core12);
	else
		i915_vkc_reply_core11_features(reply, &core11);
	i915_vkc_enc_VkPhysicalDeviceFeatures(reply, record);

	/* Succeeded: the features are reported. */
	return 0;
}

/*
 * Fills the device's Vulkan 1.1 answers: the subgroup of one invocation
 * every executor starts from, which the compiler lowers too; multiview, each view of a subpass rendered by jobs
 * of its own at the attachments' layers of the view, the programs reading
 * the view as a shared register word (no geometry or tessellation stage);
 * instances numbered as without multiview; a set's descriptors bounded by
 * the per-set limits, which together stay below 1024.  An allocation is
 * one physically contiguous run of at most POWERVR_RENDER_MAX_BYTES (the
 * whole heap), the largest allocation.
 */
static void
device_core11(
	struct i915_vkc_core11 *core11)
{
	/* What every executor starts from. */
	i915_vkc_core11_defaults(core11);

	/* Multiview, the views rendered one after another. */
	core11->multiview.multiview = VK_TRUE;
	core11->multiview_limits.maxMultiviewViewCount = POWERVR_PASS_VIEWS;
	core11->multiview_limits.maxMultiviewInstanceIndex = (1U << 27) - 1U;

	/* The descriptors of one set and the largest allocation. */
	core11->maintenance3.maxPerSetDescriptors = 1024U;
	core11->maintenance3.maxMemoryAllocationSize = POWERVR_RENDER_MAX_BYTES;
}

/* Fills the device's Vulkan 1.2 answers: those every executor starts from, under its name. */
static void
device_core12(
	struct i915_vkc_core12 *core12)
{
	/* The shaders' features, the limits and the name. */
	i915_vkc_core12_defaults(core12, "zedBSD powervr");

	/* A subpass's end resolves sample 0 of the depth and stencil (the transfer after the render, vk-transfer.c). */
	core12->properties.supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
	core12->properties.supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
}

/* Fills a cleared record from the scratch (too large for the stack) with the device's properties; NULL when none is left. */
static VkPhysicalDeviceProperties *
device_record(
	struct powervr_vk_session *session)
{
	static const char name[] = "zedBSD powervr (PowerVR BXE-4-32)";
	VkPhysicalDeviceProperties *record;

	/* The cleared record. */
	record = powervr_vk_arena_take(session, sizeof(*record));
	if (record == NULL)
		return NULL;

	/* The device: an integrated GPU, its names and its limits. */
	record->apiVersion = DEVICE_API_VERSION;
	record->driverVersion = DEVICE_DRIVER_VERSION;
	record->vendorID = DEVICE_VENDOR_ID;
	record->deviceID = DEVICE_ID;
	record->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
	kern_memcpy(record->deviceName, name, sizeof(name));
	limits(&record->limits);

	/* Succeeded: the record. */
	return record;
}

/* Fills the limits the compute path keeps; the rest stay at the smallest. */
static void
limits(
	VkPhysicalDeviceLimits *limit)
{
	/* Allocations, buffers and descriptors. */
	limit->maxImageDimension1D = 4096;
	limit->maxImageDimension2D = POWERVR_VK_IMAGE_EDGE;
	limit->maxImageDimension3D = POWERVR_VK_IMAGE_VOLUME_EDGE;
	limit->maxImageDimensionCube = 4096;
	limit->maxImageArrayLayers = POWERVR_VK_IMAGE_LAYERS;
	limit->maxTexelBufferElements = 65536;
	limit->maxUniformBufferRange = 16384;
	limit->maxStorageBufferRange = 1U << 27;
	limit->maxPushConstantsSize = 128;
	limit->maxMemoryAllocationCount = 256;
	limit->maxSamplerAllocationCount = 4000;
	limit->bufferImageGranularity = 1;
	limit->maxBoundDescriptorSets = POWERVR_VK_SETS_MAX;
	limit->maxPerStageDescriptorSamplers = 16;
	limit->maxPerStageDescriptorUniformBuffers = 12;
	limit->maxPerStageDescriptorStorageBuffers = 4;
	limit->maxPerStageDescriptorSampledImages = 16;
	limit->maxPerStageDescriptorStorageImages = 4;
	limit->maxPerStageDescriptorInputAttachments = 4;
	limit->maxPerStageResources = 128;
	limit->maxDescriptorSetSamplers = 96;
	limit->maxDescriptorSetUniformBuffers = 72;
	limit->maxDescriptorSetUniformBuffersDynamic = 8;
	limit->maxDescriptorSetStorageBuffers = 24;
	limit->maxDescriptorSetStorageBuffersDynamic = 4;
	limit->maxDescriptorSetSampledImages = 96;
	limit->maxDescriptorSetStorageImages = 24;
	limit->maxDescriptorSetInputAttachments = 4;

	/* Vertex input and the stage interfaces, the specification's smallest. */
	limit->maxVertexInputAttributes = 16;
	limit->maxVertexInputBindings = 16;
	limit->maxVertexInputAttributeOffset = 2047;
	limit->maxVertexInputBindingStride = 2048;
	limit->maxVertexOutputComponents = 64;
	limit->maxFragmentInputComponents = 64;
	limit->maxFragmentOutputAttachments = 4;
	limit->maxFragmentCombinedOutputResources = 4;

	/* Compute: what the executor and the compiler run (compute-program.h). */
	limit->maxComputeSharedMemorySize = POWERVR_VK_COMPUTE_SHARED_BYTES;
	limit->maxComputeWorkGroupCount[0] = POWERVR_VK_COMPUTE_GROUPS_X;
	limit->maxComputeWorkGroupCount[1] = POWERVR_VK_COMPUTE_GROUPS_Y;
	limit->maxComputeWorkGroupCount[2] = POWERVR_VK_COMPUTE_GROUPS_Z;
	limit->maxComputeWorkGroupInvocations = POWERVR_VK_COMPUTE_INVOCATIONS;
	limit->maxComputeWorkGroupSize[0] = POWERVR_VK_COMPUTE_SIZE_X;
	limit->maxComputeWorkGroupSize[1] = POWERVR_VK_COMPUTE_SIZE_Y;
	limit->maxComputeWorkGroupSize[2] = POWERVR_VK_COMPUTE_SIZE_Z;

	/* Precision, draws, sampling and viewports. */
	limit->subPixelPrecisionBits = 4;
	limit->subTexelPrecisionBits = 4;
	limit->mipmapPrecisionBits = 4;
	limit->maxDrawIndexedIndexValue = 0xffffffU;
	limit->maxDrawIndirectCount = 1;
	set_float(&limit->maxSamplerLodBias, DEVICE_F32_ONE);
	set_float(&limit->maxSamplerAnisotropy, DEVICE_F32_ONE);
	limit->maxViewports = 1;
	limit->maxViewportDimensions[0] = 4096;
	limit->maxViewportDimensions[1] = 4096;
	set_float(&limit->viewportBoundsRange[0], DEVICE_F32_2048 | 0x80000000U);
	set_float(&limit->viewportBoundsRange[1], DEVICE_F32_2048);

	/* Alignments. */
	limit->minMemoryMapAlignment = 4096;
	limit->minTexelBufferOffsetAlignment = 16;
	limit->minUniformBufferOffsetAlignment = POWERVR_VK_RESOURCE_ALIGN;
	limit->minStorageBufferOffsetAlignment = 4;
	limit->minTexelOffset = -8;
	limit->maxTexelOffset = 7;
	limit->minTexelGatherOffset = -8;
	limit->maxTexelGatherOffset = 7;
	limit->subPixelInterpolationOffsetBits = 4;

	/*
	 * Framebuffers and samples: one or four samples of attachments and of
	 * sampled images (the BXE-4-32's most is four, one ISP sample a
	 * pixel), storage images one.
	 *
	 * Values referenced: Mesa 26.2.6 src/imagination/vulkan/
	 * pvr_physical_device.c (the framebuffer and sampled image sample
	 * counts 1 and 4, storage images 1) and
	 * src/imagination/common/device_info/bxe-4-32.h (max_multisample 4).
	 */
	limit->maxFramebufferWidth = 4096;
	limit->maxFramebufferHeight = 4096;
	limit->maxFramebufferLayers = 256;
	limit->framebufferColorSampleCounts = DEVICE_SAMPLE_COUNTS;
	limit->framebufferDepthSampleCounts = DEVICE_SAMPLE_COUNTS;
	limit->framebufferStencilSampleCounts = DEVICE_SAMPLE_COUNTS;
	limit->framebufferNoAttachmentsSampleCounts = DEVICE_SAMPLE_COUNTS;
	limit->maxColorAttachments = 4;
	limit->sampledImageColorSampleCounts = DEVICE_SAMPLE_COUNTS;
	limit->sampledImageIntegerSampleCounts = DEVICE_SAMPLE_COUNTS;
	limit->sampledImageDepthSampleCounts = DEVICE_SAMPLE_COUNTS;
	limit->sampledImageStencilSampleCounts = DEVICE_SAMPLE_COUNTS;
	limit->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limit->maxSampleMaskWords = 1;

	/* Queue priorities, points, lines and copies. */
	limit->discreteQueuePriorities = 2;
	set_float(&limit->pointSizeRange[0], DEVICE_F32_ONE);
	set_float(&limit->pointSizeRange[1], DEVICE_F32_ONE);
	set_float(&limit->lineWidthRange[0], DEVICE_F32_ONE);
	set_float(&limit->lineWidthRange[1], DEVICE_F32_ONE);
	limit->standardSampleLocations = VK_TRUE;
	limit->optimalBufferCopyOffsetAlignment = 64;
	limit->optimalBufferCopyRowPitchAlignment = 64;
	limit->nonCoherentAtomSize = 64;
}

/* vkGetPhysicalDeviceFeatures: [physical][present] -> [present][features], those device_features names. */
static int
features(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceFeatures *record;

	/* The physical device and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The record of the features the device has. */
	record = powervr_vk_arena_take(session, sizeof(*record));
	if (record == NULL)
		return ENOMEM;
	device_features(record);

	/* The presence word and the record. */
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkPhysicalDeviceFeatures(reply, record);

	/* Succeeded: the features are reported. */
	return 0;
}

/*
 * Fills the optional features the device has; every other one is off.
 *
 * Stores and atomics from the vertex stage: the vertex shader runs on the
 * same unified shader cores as a compute kernel, and the compiler emits
 * its buffer stores and atomics with the same instructions in any stage.
 *
 * Independent blend: the blend and write mask of each colour attachment
 * are their own in the fragment key (render-draw.c), and the compiler
 * blends and masks each output with its attachment's (the blend is done
 * in the fragment shader, as on Mesa's driver, which reports the feature:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_physical_device.c,
 * independentBlend).
 */
static void
device_features(
	VkPhysicalDeviceFeatures *record)
{
	/* Everything off, then what the device has. */
	kern_memset(record, 0, sizeof(*record));
	record->vertexPipelineStoresAndAtomics = VK_TRUE;
	record->independentBlend = VK_TRUE;
}

/*
 * vkGetPhysicalDeviceMemoryProperties: [physical][present][type extent]
 * [heap extent] -> [present][properties].  Two types over one heap.
 */
static int
memory_properties(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceMemoryProperties *record;

	/* The physical device, the presence word and the two extents. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A cleared record from the scratch. */
	record = powervr_vk_arena_take(session, sizeof(*record));
	if (record == NULL)
		return ENOMEM;

	/*
	 * Device local, then host visible and coherent (uncached): the fewer
	 * properties first, as the specification orders them.  Host cached is
	 * not offered: the user library never asks the driver to flush or
	 * invalidate a mapping, so a cached view would go stale.
	 */
	record->memoryTypeCount = POWERVR_VK_MEMORY_TYPES;
	record->memoryTypes[POWERVR_VK_MEMORY_DEVICE].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	record->memoryTypes[POWERVR_VK_MEMORY_DEVICE].heapIndex = 0;
	record->memoryTypes[POWERVR_VK_MEMORY_HOST].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	record->memoryTypes[POWERVR_VK_MEMORY_HOST].heapIndex = 0;

	/* The one heap of system memory the core reaches. */
	record->memoryHeapCount = 1;
	record->memoryHeaps[0].size = DEVICE_HEAP_BYTES;
	record->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

	/* The presence word and the record. */
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkPhysicalDeviceMemoryProperties(reply, record);

	/* Succeeded: the memory is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceQueueFamilyProperties: [physical][present][count]
 * [array count] -> [present][count][array count][families].  One family
 * of one queue that draws, computes and copies: the executor runs render
 * passes (vk-queue.c run_job), dispatches and transfers on its one queue;
 * with the session's video engine it decodes (and encodes) on it too.
 */
static int
families(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkQueueFamilyProperties family;
	uint64_t array_count;
	uint64_t index;

	/* The physical device, the presence word and the count, then the array's count. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	array_count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || array_count > DEVICE_FAMILIES)
		return EINVAL;

	/* The presence word, the count, then the family when the array holds it. */
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, DEVICE_FAMILIES);
	drv_i915_wire_reply_u64(reply, array_count);
	for (index = 0; index < array_count; index++) {
		kern_memset(&family, 0, sizeof(family));
		family.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
		family.queueFlags |= powervr_video_queue_flags(session->owner->video_operations);
		family.queueCount = 1;
		family.minImageTransferGranularity.width = 1;
		family.minImageTransferGranularity.height = 1;
		family.minImageTransferGranularity.depth = 1;
		i915_vkc_enc_VkQueueFamilyProperties(reply, &family);
	}

	/* Succeeded: the family is reported. */
	return 0;
}

/*
 * The codec operations of each queue family (zedBSD's own op):
 * [physical][u32 family count] -> [u64 1][u32 count][count x u32
 * operations].  The one family's are the session's video engine's
 * (none without one).
 *
 * Procedure referenced: zedBSD src/drivers/gpu/nvrtx/vk/vk-video.c
 * (video_family_properties: the same wire).
 */
static int
family_video(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t count;
	uint32_t codecs;

	/* The physical device and the family count, which is the one family. */
	(void)drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || count != DEVICE_FAMILIES)
		return EINVAL;

	/* The presence word, the count and the family's codec operations. */
	codecs = powervr_video_codec_operations(session->owner->video_operations);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, DEVICE_FAMILIES);
	drv_i915_wire_reply_u32(reply, codecs);

	/* Succeeded: the family's codec operations are reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceFormatProperties: [physical][format][present] ->
 * [present][properties]; the features of the format table, none for a
 * format outside it.  NV12 adds a decode's output and reference with the
 * session's video decoder.
 */
static int
format(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFormatProperties record;
	const struct powervr_format *entry;
	uint32_t vk_format;

	/* The physical device, the format and the presence word. */
	(void)drv_i915_wire_read_u64(reader);
	vk_format = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The format's features on each tiling and on buffers. */
	kern_memset(&record, 0, sizeof(record));
	entry = powervr_format_find(vk_format);
	if (entry != NULL) {
		record.optimalTilingFeatures = entry->optimal_features | powervr_format_storage_features(entry);
		record.linearTilingFeatures = powervr_format_linear_features(entry) | powervr_format_storage_features(entry);
		record.bufferFeatures = entry->buffer_features;
	}
	if (entry != NULL && (entry->kind & POWERVR_FORMAT_PLANAR) != 0) {
		record.optimalTilingFeatures |= powervr_video_format_features(session->owner->video_operations);
		record.linearTilingFeatures |= powervr_video_format_features(session->owner->video_operations);
	}
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkFormatProperties(reply, &record);

	/* Succeeded: the format is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceImageFormatProperties: [physical][format][type]
 * [tiling][usage][flags][present] -> [result][present][properties].
 */
static int
image_format(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageFormatProperties record;
	uint32_t words[5];
	uint32_t result;
	uint32_t index;

	/* The physical device, the format, type, tiling, usage and flags, and the presence word. */
	(void)drv_i915_wire_read_u64(reader);
	for (index = 0; index < 5U; index++)
		words[index] = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* What such an image may be. */
	result = image_limits(session, words, &record);

	/* The result, the presence word and the limits. */
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkImageFormatProperties(reply, &record);

	/* Succeeded: the format is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceImageFormatProperties2 with an external memory
 * handle type (the chains as uapi/gpu-op.h lays them out) -> [result]
 * [u64 1], the two headers, the external memory properties and the
 * image's limits.  An image of a handle type the node shares by is
 * exported and imported as its memory is (vk-memory.c); its limits are
 * those of the image without the handle type.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/ghci/vk/vk-device.c
 * (image_format2: the same wire).
 */
static int
image_format_external(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageFormatProperties record;
	uint32_t words[5];
	uint32_t handle_type;
	uint32_t result;
	uint32_t index;

	/* The physical device and the input chain up to the handle type. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	handle_type = drv_i915_wire_read_u32(reader);

	/* The image, then the output chain's headers. */
	for (index = 0; index < 5U; index++)
		words[index] = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* What such an image may be; an image the node cannot make has no external memory either. */
	result = image_limits(session, words, &record);
	if (result != (uint32_t)VK_SUCCESS)
		handle_type = 0;

	/* The result, the two headers, the external memory properties, then the limits. */
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
	drv_i915_wire_reply_u64(reply, 0);
	reply_external(reply, handle_type);
	i915_vkc_enc_VkImageFormatProperties(reply, &record);

	/* Succeeded: the format is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceExternalBufferProperties with the context's native
 * handle type (the chains as uapi/gpu-op.h lays them out) -> [u64 1],
 * the header and the external memory properties: a buffer's memory is
 * exported and imported as any memory is, by the node's handle types.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/ghci/vk/vk-device.c
 * (buffer_external: the same wire).
 */
static int
buffer_external(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t handle_type;

	/* The physical device, the input chain with the handle type, and the output chain. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	handle_type = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The presence word, the header and the properties. */
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES);
	drv_i915_wire_reply_u64(reply, 0);
	reply_external(reply, handle_type);

	/* Succeeded: the buffer's external memory is reported. */
	return 0;
}

/*
 * Writes the external memory properties of one handle type: exported
 * and imported, compatible only with itself, for one type the node
 * shares by (POWERVR_VK_EXTERNAL_HANDLES); none for any other (or 0).
 */
static void
reply_external(
	struct i915_wire_writer *reply,
	uint32_t handle_type)
{
	uint32_t features;

	/* One known handle type, exported and imported as itself. */
	features = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
	if (handle_type == 0 ||
	    (handle_type & (handle_type - 1U)) != 0 ||
	    (handle_type & ~(uint32_t)POWERVR_VK_EXTERNAL_HANDLES) != 0) {
		features = 0;
		handle_type = 0;
	}

	/* The features, the types it exports from, the types it is compatible with. */
	drv_i915_wire_reply_u32(reply, features);
	drv_i915_wire_reply_u32(reply, handle_type);
	drv_i915_wire_reply_u32(reply, handle_type);
}

/*
 * Works out what an image of a format, type, tiling, usage and flags
 * (words in that order) may be: VK_SUCCESS and its limits, or
 * VK_ERROR_FORMAT_NOT_SUPPORTED and zeros.  A two- or three-dimensional
 * image of one sample as shape_allowed says, whose every usage the
 * format's features on the tiling allow; a multi-planar image as
 * planar_allowed says.  A three-dimensional image's edges are the
 * volume's, of one layer and the levels down to one texel.
 */
static uint32_t
image_limits(
	const struct powervr_vk_session *session,
	const uint32_t words[5],
	VkImageFormatProperties *record)
{
	const struct powervr_format *entry;
	uint32_t features;
	uint32_t result;
	bool planar;
	bool allowed;

	/* The format's features on the tiling. */
	features = 0;
	entry = powervr_format_find(words[0]);
	if (entry != NULL && words[2] == VK_IMAGE_TILING_OPTIMAL)
		features = entry->optimal_features;
	else if (entry != NULL && words[2] == VK_IMAGE_TILING_LINEAR)
		features = powervr_format_linear_features(entry);

	/*
	 * A storage image is linear in either tiling, and takes the format's
	 * optimal usages besides (vk-image.c storage_check).
	 */
	if (entry != NULL && (words[3] & VK_IMAGE_USAGE_STORAGE_BIT) != 0 && powervr_format_storage_features(entry) != 0)
		features = entry->optimal_features | powervr_format_storage_features(entry);

	/* A multi-planar image as its planes allow, any other plain one whose usages its features allow. */
	planar = false;
	if (entry != NULL && (entry->kind & POWERVR_FORMAT_PLANAR) != 0)
		planar = true;
	if (planar) {
		allowed = planar_allowed(session, words[1], words[2], words[3], words[4]);
	} else {
		allowed = usage_allowed(words[3], features);
		if (!shape_allowed(entry, words[1], words[2], words[3], words[4]))
			allowed = false;
	}
	result = (uint32_t)VK_SUCCESS;
	if (!allowed)
		result = (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The limits of such an image: a linear or multi-planar one is one level and one layer. */
	kern_memset(record, 0, sizeof(*record));
	if (result == (uint32_t)VK_SUCCESS) {
		record->maxExtent.width = POWERVR_VK_IMAGE_EDGE;
		record->maxExtent.height = POWERVR_VK_IMAGE_EDGE;
		record->maxExtent.depth = 1;
		record->maxMipLevels = 13;
		record->maxArrayLayers = POWERVR_VK_IMAGE_LAYERS;
		if (words[2] == VK_IMAGE_TILING_LINEAR || planar || (words[3] & VK_IMAGE_USAGE_STORAGE_BIT) != 0) {
			record->maxMipLevels = 1;
			record->maxArrayLayers = 1;
		}
		if (words[1] == VK_IMAGE_TYPE_3D) {
			record->maxExtent.width = POWERVR_VK_IMAGE_VOLUME_EDGE;
			record->maxExtent.height = POWERVR_VK_IMAGE_VOLUME_EDGE;
			record->maxExtent.depth = POWERVR_VK_IMAGE_VOLUME_EDGE;
			record->maxMipLevels = 9;
			record->maxArrayLayers = 1;
		}
		record->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
		if (multisample_allowed(entry, features, words[1], words[2], words[3], words[4]))
			record->sampleCounts = DEVICE_SAMPLE_COUNTS;
		record->maxResourceSize = POWERVR_RENDER_MAX_BYTES;
	}

	/* Succeeded: the image's result. */
	return result;
}

/*
 * Says whether an image may have four samples: a twiddled flat image,
 * not cube compatible, of a plain format an attachment draws, not a
 * storage image (vk-image.c multisample_check makes such an image).
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/pvr_formats.c
 * (pvr_get_image_format_properties: sample counts above one only for an
 * optimal 2D image of a colour or depth attachment format, not cube
 * compatible).
 */
static bool
multisample_allowed(
	const struct powervr_format *entry,
	uint32_t features,
	uint32_t type,
	uint32_t tiling,
	uint32_t usage,
	uint32_t flags)
{
	/* A plain format an attachment draws, a flat twiddled image, no cube, no storage. */
	if (entry == NULL || (entry->kind & POWERVR_FORMAT_PLANAR) != 0)
		return false;
	if ((features & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) == 0)
		return false;
	if (type != VK_IMAGE_TYPE_2D || tiling != VK_IMAGE_TILING_OPTIMAL)
		return false;
	return (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) == 0 && (usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0;
}

/* Says whether every usage of an image needs only features the format has; none at all is refused. */
static bool
usage_allowed(
	uint32_t usage,
	uint32_t features)
{
	uint32_t known;

	/* Some usage, all of them known, and a format with features. */
	known = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
	if (usage == 0 || (usage & ~known) != 0 || features == 0)
		return false;

	/* Each usage against the feature it needs. */
	if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 && (features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0) {
		/* A copy from the image. */
		return false;
	} else if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 && (features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0) {
		/* A copy or clear into the image. */
		return false;
	} else if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 && (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
		/* A shader's sampling. */
		return false;
	} else if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 && (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
		/* A render pass's colour. */
		return false;
	} else if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0 && (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
		/* A render pass's depth. */
		return false;
	} else if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0 && (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0) {
		/* A shader's loads and stores. */
		return false;
	} else if ((usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) != 0 &&
		   ((features & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) == 0 ||
		    (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)) {
		/* A subpass's input, which the shader reads as a sampled image. */
		return false;
	}

	/* Succeeded: every usage is allowed. */
	return true;
}

/*
 * Says whether an image of one format of a type, tiling, usage and flags
 * has a shape vk-image.c makes: a two-dimensional one with no flag but
 * cube compatibility, or a twiddled three-dimensional one with no flag,
 * of a colour format and neither attachment usage.
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/pvr_formats.c
 * (pvr_get_image_format_properties: no 3D depth or stencil image).
 */
static bool
shape_allowed(
	const struct powervr_format *entry,
	uint32_t type,
	uint32_t tiling,
	uint32_t usage,
	uint32_t flags)
{
	/*
	 * A storage image: two-dimensional and linear, so no cube (and one
	 * level and layer, image_limits), its format mutable or not.
	 */
	if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0)
		return type == VK_IMAGE_TYPE_2D && (flags & ~(uint32_t)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == 0;

	/* A two-dimensional image, which cube views may read. */
	if (type == VK_IMAGE_TYPE_2D)
		return (flags & ~(uint32_t)VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) == 0;

	/* A twiddled volume of colour texels, not drawn into. */
	if (type == VK_IMAGE_TYPE_3D) {
		if (flags != 0 || tiling != VK_IMAGE_TILING_OPTIMAL)
			return false;
		if (entry == NULL || (entry->kind & POWERVR_FORMAT_DEPTH) != 0)
			return false;
		if ((usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0)
			return false;
		return true;
	}

	/* No other type. */
	return false;
}

/*
 * Says whether a multi-planar (NV12) image of a type, tiling, usage and
 * flags can be made (vk-image.c makes the same ones): a flat image in
 * either tiling, with only its planes' views' flags, copied out and its
 * planes sampled (the sampling only through the extended usage flag, as
 * the format itself is not sampled), and with the session's video
 * decoder decoded into and referenced.
 */
static bool
planar_allowed(
	const struct powervr_vk_session *session,
	uint32_t type,
	uint32_t tiling,
	uint32_t usage,
	uint32_t flags)
{
	uint32_t usages;

	/* A flat image in a tiling the planes lie in, with only the plane views' flags. */
	if (type != VK_IMAGE_TYPE_2D)
		return false;
	if (tiling != VK_IMAGE_TILING_OPTIMAL && tiling != VK_IMAGE_TILING_LINEAR)
		return false;
	if ((flags & ~(uint32_t)POWERVR_VK_PLANAR_FLAGS) != 0)
		return false;

	/* Some usage, all of them ones the planes or the session's decoder have. */
	usages = POWERVR_VK_PLANAR_USAGES | powervr_video_image_usages(session->owner->video_operations);
	if (usage == 0 || (usage & ~usages) != 0)
		return false;

	/* Sampling only through the planes' views. */
	if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 && (flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) == 0)
		return false;

	/* Succeeded: such an image is made. */
	return true;
}

/*
 * vkCreateDevice: [physical][present][create info][allocator][present]
 * [id] -> [result][present][id].  Queues of the one family only, and only
 * optional features the device has (device_features).
 */
static int
create_device(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDeviceCreateInfo info;
	VkPhysicalDeviceFeatures offered;
	struct i915_vkc_core11 core11;
	struct i915_vkc_core12 core12;
	const VkBool32 *wanted;
	const VkBool32 *has;
	uint64_t present;
	uint64_t identity;
	uint32_t index;
	int error;

	/* The physical device, then the create info when present. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0)
		i915_vkc_dec_VkDeviceCreateInfo(reader, &session->arena, &info);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Queues of the one family, one queue each. */
	for (index = 0; index < info.queueCreateInfoCount && info.pQueueCreateInfos != NULL; index++) {
		if (info.pQueueCreateInfos[index].queueFamilyIndex >= DEVICE_FAMILIES || info.pQueueCreateInfos[index].queueCount != 1) {
			drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
			return 0;
		}
	}

	/* No Vulkan 1.1 or 1.2 feature the device does not report (libvulkan refuses it first). */
	device_core11(&core11);
	device_core12(&core12);
	if (i915_vkc_core11_check_device(&info, &core11) != 0 || i915_vkc_core12_check_device(&info, &core12) != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT);
		return 0;
	}

	/* No optional feature the device does not have (the record is VkBool32 words only). */
	if (info.pEnabledFeatures != NULL) {
		device_features(&offered);
		wanted = (const VkBool32 *)info.pEnabledFeatures;
		has = (const VkBool32 *)&offered;
		for (index = 0; index < sizeof(offered) / sizeof(VkBool32); index++) {
			if (wanted[index] != VK_FALSE && has[index] == VK_FALSE) {
				drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT);
				return 0;
			}
		}
	}

	/* Remembers the device. */
	error = remember(session, I915_VK_OBJ_DEVICE, identity);
	if (error != 0)
		return error;

	/* Success and the identity. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1);
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
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t identity;
	uint32_t timeline;
	uint32_t family;
	uint32_t index;

	/* Up to the timeline, the timeline, the flags, the family, the index, the presence word, the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	timeline = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	family = drv_i915_wire_read_u32(reader);
	index = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The one queue of the one family, completing jobs on a timeline of 1 to 63. */
	if (family >= DEVICE_FAMILIES || index != 0 || timeline == 0 || timeline >= DEVICE_TIMELINES)
		return EINVAL;

	/* Remembers the queue; one asked for again is already remembered. */
	(void)remember(session, I915_VK_OBJ_QUEUE, identity);

	/* The session's jobs may name the queue's timeline from now on. */
	__atomic_fetch_or(&session->timelines, 1ULL << timeline, __ATOMIC_RELEASE);

	/* The presence word and the identity. */
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the queue is known. */
	return 0;
}

/* vkDestroyInstance and vkDestroyDevice: [handle][allocator], no reply body. */
static int
destroy(
	struct powervr_vk_session *session,
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
	(void)powervr_vk_object_remove(session, kind, identity);

	/* Succeeded: the identity is forgotten. */
	return 0;
}

/*
 * vkQueueWaitIdle and vkDeviceWaitIdle: [handle] -> [result].  A
 * submission's work ends before its frame's later commands run, so the
 * queue is idle here.
 */
static int
wait_idle(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	/* The queue or the device. */
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Success. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);

	/* Succeeded: the queue is idle. */
	return 0;
}

/* The token's destructor: the token is static, nothing is freed. */
static int
token_destroy(
	struct powervr_vk_session *session,
	void *payload)
{
	/* Nothing to free. */
	(void)session;
	(void)payload;

	/* Succeeded: the token stays. */
	return 0;
}

/* Publishes an identity that stands for the token. */
static int
remember(
	struct powervr_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	int error;

	/* Publishes it. */
	error = powervr_vk_object_publish(session, kind, identity, &device_token, token_destroy);
	if (error != 0)
		return error;

	/* Succeeded: the identity is remembered. */
	return 0;
}

/* Fills a float field by its bits, so this file never touches a floating-point register. */
static void
set_float(
	float *destination,
	uint32_t bits)
{
	/* Stores the bits unchanged. */
	kern_memcpy(destination, &bits, sizeof(bits));
}
