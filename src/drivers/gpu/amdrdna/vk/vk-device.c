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
 * physical device with two queue families of one queue each: the
 * universal one (graphics, compute and transfer, on the graphics queue)
 * and a compute one (on a compute queue of its own), and one memory type: the VRAM the CPU reaches through its
 * window, so it is device-local, host-visible and host-coherent.  The
 * formats are the ones the executor lays out (linear only, so both tilings
 * have the same features); the limits are values an RDNA1 part meets, not a
 * survey of its maxima.  Memory is exported and imported as the node's
 * allocations (OPAQUE_FD), whatever a buffer or an image is in it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/amdrdna/amdrdna-regs.h"
#include "drivers/gpu/amdrdna/vk/amdrdna-vk-exec.h"
#include "drivers/gpu/amdrdna/vk/amdrdna-vk-objects.h"
#include "drivers/gpu/amdrdna/vk/amdrdna-vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/*
 * The bits of the float constants the limits are filled with: 1, 10, 16,
 * 1/8, -32768, 32767 and 2048.
 *
 * Values referenced: IEEE 754-2008 binary32 (sign 31, biased exponent
 * 30:23 with a bias of 127, fraction 22:0).
 */
#define DEVICE_F32_ONE			0x3f800000U
#define DEVICE_F32_TEN			0x41200000U
#define DEVICE_F32_16			0x41800000U
#define DEVICE_F32_EIGHTH		0x3e000000U
#define DEVICE_F32_MINUS_32768		0xc7000000U
#define DEVICE_F32_32767		0x46fffe00U
#define DEVICE_F32_2048			0x45000000U

/*
 * The bits of a timestamp each queue writes (the GPU's 64-bit counter).
 *
 * Value referenced: Mesa 25.0.7 src/amd/vulkan/radv_physical_device.c
 * (the graphics and compute families' timestampValidBits 64).
 */
#define DEVICE_TIMESTAMP_BITS		64U

/* The version and driver version the device reports (libvulkan needs a native 1.1). */
#define DEVICE_API_VERSION		VK_MAKE_VERSION(1, 2, 0)
#define DEVICE_DRIVER_VERSION		1U

/* The timelines a queue may complete jobs on: 1 to 63 (0 is the context's own). */
#define DEVICE_TIMELINES		64U

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
 * The features of the depth formats (with stencil or without), in
 * optimal tiling only (the depth unit's swizzle): a depth attachment,
 * sampled (its depth filtered, or compared, by the texture unit; its
 * stencil fetched), copied, and blitted from and to (nearest, between two
 * images of the format).
 *
 * Values referenced: the Vulkan 1.3 specification (Required Format
 * Support, formats-mandatory-features-depth-stencil: D16_UNORM's
 * SAMPLED_IMAGE, BLIT_SRC and DEPTH_STENCIL_ATTACHMENT, D32_SFLOAT's
 * SAMPLED_IMAGE and BLIT_SRC; a format of SAMPLED_IMAGE has TRANSFER_SRC
 * and TRANSFER_DST) and Mesa 25.0.7 src/amd/vulkan/radv_formats.c
 * (radv_physical_device_get_format_properties: a depth/stencil format's
 * optimal features on GFX10, linear filtering of the depth).
 */
#define DEVICE_DEPTH_FEATURES	(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | \
				 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
				 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_DST_BIT | \
				 VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
				 VK_FORMAT_FEATURE_BLIT_DST_BIT)

/*
 * The features of a format of one or two bytes a texel (R8_UNORM,
 * R8G8_UNORM, also a two-plane image's planes): sampled and copied, not
 * drawn into or blitted.
 */
#define DEVICE_NARROW_FEATURES	(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
				 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

/*
 * The features of a storage format (amdrdna_vk_format_storage()): a
 * storage image's, copied; R32_UINT and R32_SINT with the atomics too.
 * The storage formats and R8G8B8A8_UNORM are a storage texel buffer's (R32
 * with the atomics).
 */
#define DEVICE_STORAGE_FEATURES	(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
				 VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

/*
 * The features of the two-plane format itself: copied from and to (its
 * planes are sampled through views in their own formats, so the format
 * has no sampling of its own).
 */
#define DEVICE_PLANAR_FEATURES	(VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

/*
 * What a two-plane image is besides in optimal tiling while the GPU's
 * video engine is up: a video decode's output and reference picture.
 */
#define DEVICE_VIDEO_FEATURES	(VK_FORMAT_FEATURE_VIDEO_DECODE_OUTPUT_BIT_KHR | VK_FORMAT_FEATURE_VIDEO_DECODE_DPB_BIT_KHR)

/*
 * What a two-plane image may be asked for: copied, its planes sampled
 * (with VK_IMAGE_CREATE_EXTENDED_USAGE_BIT), a video decode's output and
 * reference picture (in optimal tiling), and the flags of its planes'
 * views.  vk-resource.c makes the same images.
 */
#define DEVICE_PLANAR_USAGES	(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | \
				 VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR)
#define DEVICE_PLANAR_FLAGS	(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)

/* The one handle type memory is exported and imported by: the node's allocation as an fd. */
#define DEVICE_EXTERNAL_HANDLE	VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT

/*
 * The sample counts of the attachments and of the images drawn into and
 * fetched by sample (one, or four), and the usages an image of four
 * samples may have (vk-resource.c lays it out).
 */
#define DEVICE_SAMPLE_COUNTS	(VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)
#define DEVICE_SAMPLED_USAGES	(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | \
				 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | \
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | \
				 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)

/*
 * What the object registry holds for an instance, a physical device, a
 * device and a queue: the executor only remembers that they exist, so
 * every one of them stands for this token, which lives as long as the
 * kernel and is never freed.
 */
static int device_token;

static int device_create_instance(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_enumerate(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_properties(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_identity(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static VkPhysicalDeviceProperties *device_describe(struct amdrdna_vk_session *session);
static void device_limits(VkPhysicalDeviceLimits *limits);
static int device_features(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static VkPhysicalDeviceFeatures *device_feature_record(struct amdrdna_vk_session *session);
static int device_features2(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_properties2(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void device_core11(struct amdrdna_vk_session *session, struct i915_vkc_core11 *core11);
static void device_core12(struct i915_vkc_core12 *core12);
static int device_memory(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_families(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void device_format_features(uint32_t format, bool video, VkFormatProperties *properties);
static uint32_t device_usage_features(uint32_t usage);
static int device_format(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_image_format(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_image_format_external(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_buffer_external(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void device_reply_external(struct i915_wire_writer *reply, uint32_t handle_type);
static uint32_t device_image_limits(uint32_t format, uint32_t type, uint32_t tiling, uint32_t usage, uint32_t flags, bool video, VkImageFormatProperties *image);
static int device_create(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_queue(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_destroy(struct amdrdna_vk_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int device_wait_idle(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_token_destroy(struct amdrdna_vk_session *session, void *payload);
static int device_remember(struct amdrdna_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
static void device_set_float(float *destination, uint32_t bits);

/*
 * Runs an instance, physical-device, device or queue command; *handled
 * stays clear for an opcode of another handler's.
 */
int
amdrdna_vk_device_route(
	struct amdrdna_vk_session *session,
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
		error = device_format(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES:
		error = device_image_format(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES2:
		error = device_image_format_external(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_EXTERNAL_BUFFER_PROPERTIES:
		error = device_buffer_external(reader, reply);
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
	struct amdrdna_vk_session *session,
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
	struct amdrdna_vk_session *session,
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
	struct amdrdna_vk_session *session,
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
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vkc_core11 core11;
	struct i915_vkc_core12 core12;
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

	/* The Vulkan 1.1 or 1.2 properties, then the record. */
	properties = device_describe(session);
	if (properties == NULL)
		return ENOMEM;
	device_core11(session, &core11);
	device_core12(&core12);
	if (form == I915_VKC_PROPERTIES2_CORE12)
		i915_vkc_reply_core12_properties(reply, &core12);
	else
		i915_vkc_reply_core11_properties(reply, &core11);
	i915_vkc_enc_VkPhysicalDeviceProperties(reply, properties);

	/* Succeeded: the properties are reported. */
	return 0;
}

/*
 * The identity form of vkGetPhysicalDeviceProperties2 (only the ID
 * properties chained), which libvulkan asks before it exports or imports
 * memory by fd: the two headers, the device's and the driver's UUIDs, an
 * invalid LUID, the node mask, whether the LUID is valid, and the
 * properties.  The UUIDs are fixed words of this executor.
 */
static int
device_identity(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	static const uint8_t device_uuid[VK_UUID_SIZE] = {
		'z', 'e', 'd', 'B', 'S', 'D', '-', 'a', 'm', 'd', 'r', 'd', 'n', 'a', 'N', '1'
	};
	static const uint8_t driver_uuid[VK_UUID_SIZE] = {
		'z', 'e', 'd', 'B', 'S', 'D', '-', 'a', 'm', 'd', 'r', 'd', 'n', 'a', 'v', '1'
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
 * (too large for the stack): AMD's, discrete, its PCI product, and the
 * limits.  NULL when the scratch is full.
 */
static VkPhysicalDeviceProperties *
device_describe(
	struct amdrdna_vk_session *session)
{
	static const char name[] = "zedBSD amdrdna (AMD RDNA1)";
	VkPhysicalDeviceProperties *properties;

	/* A cleared record. */
	properties = amdrdna_vk_arena_take(session, sizeof(*properties));
	if (properties == NULL)
		return NULL;

	/* Names the device: AMD's, discrete, its PCI product. */
	properties->apiVersion = DEVICE_API_VERSION;
	properties->driverVersion = DEVICE_DRIVER_VERSION;
	properties->vendorID = AMDRDNA_PCI_VENDOR;
	properties->deviceID = amdrdna_vk_device_id(session->owner);
	properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
	kern_memcpy(properties->deviceName, name, sizeof(name));

	/* The limits. */
	device_limits(&properties->limits);

	/* Succeeded: the record is filled. */
	return properties;
}

/* Fills the limits: values an RDNA1 part meets that applications read. */
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

	/*
	 * Descriptor counts per stage and per set, as a program's resource
	 * table holds them (the compiler loads the descriptors that do not fit
	 * in SGPRs at their use): 16 sampled images (with the uniform texel
	 * buffers) with their samplers, 12 uniform buffers, storage buffers (4
	 * reported), 8 storage images (with the storage texel buffers), 4
	 * input attachments (read as sampled images: XXX they share the 16);
	 * with the eight colour attachments, 48 resources a stage.  A set
	 * holds six stages'.
	 */
	limits->maxBoundDescriptorSets = 4U;
	limits->maxPerStageDescriptorSamplers = 16U;
	limits->maxPerStageDescriptorUniformBuffers = 12U;
	limits->maxPerStageDescriptorStorageBuffers = 4U;
	limits->maxPerStageDescriptorSampledImages = 16U;
	limits->maxPerStageDescriptorStorageImages = AMDRDNA_VK_STORAGE_IMAGES_MAX;
	limits->maxPerStageDescriptorInputAttachments = 4U;
	limits->maxPerStageResources = 48U;
	limits->maxDescriptorSetSamplers = 96U;
	limits->maxDescriptorSetUniformBuffers = 72U;
	limits->maxDescriptorSetUniformBuffersDynamic = 8U;
	limits->maxDescriptorSetStorageBuffers = 24U;
	limits->maxDescriptorSetStorageBuffersDynamic = 4U;
	limits->maxDescriptorSetSampledImages = 96U;
	limits->maxDescriptorSetStorageImages = 6U * AMDRDNA_VK_STORAGE_IMAGES_MAX;
	limits->maxDescriptorSetInputAttachments = 4U;

	/* Vertex input as a pipeline takes it (AMDRDNA_VK_VERTEX_ATTRIBUTES_MAX, _BINDINGS_MAX), and the stage interfaces. */
	limits->maxVertexInputAttributes = AMDRDNA_VK_VERTEX_ATTRIBUTES_MAX;
	limits->maxVertexInputBindings = AMDRDNA_VK_VERTEX_BINDINGS_MAX;
	limits->maxVertexInputAttributeOffset = 2047U;
	limits->maxVertexInputBindingStride = 2048U;
	limits->maxVertexOutputComponents = 64U;
	limits->maxFragmentInputComponents = 64U;
	limits->maxFragmentOutputAttachments = AMDRDNA_VK_COLOURS_MAX;
	limits->maxFragmentDualSrcAttachments = 1U;
	limits->maxFragmentCombinedOutputResources = AMDRDNA_VK_COLOURS_MAX + 4U + AMDRDNA_VK_STORAGE_IMAGES_MAX;

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
	limits->maxDrawIndirectCount = 1U;
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
	 * Framebuffers: one layer (a view of one layer is a target); one or
	 * four samples of colour, depth and stencil, and of a pass without an
	 * attachment (its draws raster at the pipeline's); colour and depth
	 * images of four
	 * samples fetched by sample (a depth image's depth or stencil), not
	 * an integer or a storage image.
	 */
	limits->maxFramebufferWidth = 16384U;
	limits->maxFramebufferHeight = 16384U;
	limits->maxFramebufferLayers = 1U;
	limits->framebufferColorSampleCounts = DEVICE_SAMPLE_COUNTS;
	limits->framebufferDepthSampleCounts = DEVICE_SAMPLE_COUNTS;
	limits->framebufferStencilSampleCounts = DEVICE_SAMPLE_COUNTS;
	limits->framebufferNoAttachmentsSampleCounts = DEVICE_SAMPLE_COUNTS;
	limits->maxColorAttachments = AMDRDNA_VK_COLOURS_MAX;
	limits->sampledImageColorSampleCounts = DEVICE_SAMPLE_COUNTS;
	limits->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->sampledImageDepthSampleCounts = DEVICE_SAMPLE_COUNTS;
	limits->sampledImageStencilSampleCounts = DEVICE_SAMPLE_COUNTS;
	limits->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->maxSampleMaskWords = 1U;

	/* Clipping, queue priorities, points and lines. */
	limits->maxClipDistances = 8U;
	limits->maxCullDistances = 8U;
	limits->maxCombinedClipAndCullDistances = 8U;
	limits->discreteQueuePriorities = 2U;
	device_set_float(&limits->pointSizeRange[0], DEVICE_F32_EIGHTH);
	device_set_float(&limits->pointSizeRange[1], DEVICE_F32_2048);
	device_set_float(&limits->lineWidthRange[0], DEVICE_F32_ONE);
	device_set_float(&limits->lineWidthRange[1], DEVICE_F32_ONE);
	limits->strictLines = VK_FALSE;
	limits->standardSampleLocations = VK_TRUE;

	/*
	 * Timestamps on both queues, a tick each 10 ns: the GPU's counter runs
	 * at the reference clock (100 MHz on Navi 1x).
	 *
	 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_physical_device.c
	 * (timestampComputeAndGraphics true, timestampPeriod 1000000 over
	 * clock_crystal_freq in kHz) and src/amd/common/ac_gpu_info.c
	 * (clock_crystal_freq from the kernel's gpu_counter_freq).
	 */
	limits->timestampComputeAndGraphics = VK_TRUE;
	device_set_float(&limits->timestampPeriod, DEVICE_F32_TEN);

	/* Copy and coherence granularities. */
	limits->optimalBufferCopyOffsetAlignment = 64U;
	limits->optimalBufferCopyRowPitchAlignment = 64U;
	limits->nonCoherentAtomSize = 64U;
}

/*
 * vkGetPhysicalDeviceFeatures: [physical][present] -> [present][features].
 * The optional features the executor and the compiler have: a blend of
 * its own for each colour target (CB_BLENDi_CONTROL), the dual-source
 * blend of target 0 (the second colour exported after the first),
 * arrays of uniform
 * buffers, sampled images and storage buffers indexed at run time (the
 * frontend reads every element and selects; a draw writes each element's
 * descriptor), storage buffer
 * writes and atomics in the vertex programs (legacy and primitive
 * shaders; one that writes memory is not culled, so it runs once a
 * vertex) and in the pixel programs (the helper lanes write nothing), as
 * are the storage images' and storage texel buffers' writes and atomics.
 * The storage images read and write only in the formats their shaders
 * name (no extended formats, no access without a format), one sample.
 */
static int
device_features(
	struct amdrdna_vk_session *session,
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
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vkc_core11 core11;
	struct i915_vkc_core12 core12;
	VkPhysicalDeviceFeatures *features;
	int form;
	int error;

	/* The physical device, then the output chain of the 1.1 or 1.2 feature structures. */
	(void)drv_i915_wire_read_u64(reader);
	error = i915_vkc_read_features2(reader, &form);
	if (error != 0) {
		reader->error = 1;
		return error;
	}

	/* The record of the device's features. */
	features = device_feature_record(session);
	if (features == NULL)
		return ENOMEM;

	/* The 1.1 or 1.2 features, then the record. */
	device_core11(session, &core11);
	device_core12(&core12);
	if (form == I915_VKC_FEATURES2_CORE12)
		i915_vkc_reply_core12_features(reply, &core12);
	else
		i915_vkc_reply_core11_features(reply, &core11);
	i915_vkc_enc_VkPhysicalDeviceFeatures(reply, features);

	/* Succeeded: the features are reported. */
	return 0;
}

/* A cleared record of the device's core features from the session's scratch; NULL when it is full. */
static VkPhysicalDeviceFeatures *
device_feature_record(
	struct amdrdna_vk_session *session)
{
	VkPhysicalDeviceFeatures *features;

	/* A cleared record: every optional feature off but those the device has. */
	features = amdrdna_vk_arena_take(session, sizeof(*features));
	if (features == NULL)
		return NULL;
	features->independentBlend = VK_TRUE;
	features->dualSrcBlend = VK_TRUE;
	features->shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
	features->shaderSampledImageArrayDynamicIndexing = VK_TRUE;
	features->shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
	features->vertexPipelineStoresAndAtomics = VK_TRUE;
	features->fragmentStoresAndAtomics = VK_TRUE;
	features->occlusionQueryPrecise = VK_TRUE;

	/* Succeeded: the record lives until the command returns. */
	return features;
}

/*
 * Fills the device's Vulkan 1.1 answers: the subgroup of one invocation
 * every executor starts from; multiview, each view of a subpass drawn
 * into its layer with programs compiled for it (vk-execute.c), instances
 * numbered as without multiview; a set's descriptors are bounded by the
 * per-set limits, which together stay below 1024; an allocation may take
 * the whole of the larger heap.
 */
static void
device_core11(
	struct amdrdna_vk_session *session,
	struct i915_vkc_core11 *core11)
{
	uint64_t device;
	uint64_t host;

	/* What every executor starts from. */
	i915_vkc_core11_defaults(core11);

	/* Multiview. */
	core11->multiview.multiview = VK_TRUE;
	core11->multiview_limits.maxMultiviewViewCount = AMDRDNA_VK_VIEWS_MAX;
	core11->multiview_limits.maxMultiviewInstanceIndex = (1U << 27) - 1U;

	/* The descriptors of one set and the largest allocation. */
	core11->maintenance3.maxPerSetDescriptors = 1024U;
	device = amdrdna_vk_heap_bytes(session->owner, false);
	host = amdrdna_vk_heap_bytes(session->owner, true);
	core11->maintenance3.maxMemoryAllocationSize = device > host ? device : host;
}

/* Fills the device's Vulkan 1.2 answers: those every executor starts from, under its name. */
static void
device_core12(
	struct i915_vkc_core12 *core12)
{
	/* The shaders' features, the limits and the name. */
	i915_vkc_core12_defaults(core12, "zedBSD amdrdna");

	/* The end of a subpass resolves sample 0 of the depth and stencil (vk-execute.c). */
	core12->properties.supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
	core12->properties.supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
}

/*
 * vkGetPhysicalDeviceMemoryProperties: [physical][present][type extent][heap extent]
 * -> [present][properties].  One type and one heap: the VRAM the CPU
 * reaches through its window.
 */
static int
device_memory(
	struct amdrdna_vk_session *session,
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
	memory = amdrdna_vk_arena_take(session, sizeof(*memory));
	if (memory == NULL)
		return ENOMEM;

	/*
	 * The types: device-local only (the VRAM outside the CPU's window),
	 * then device-local that the CPU's window makes host-visible and
	 * coherent (uncached); the fewer properties first, as the
	 * specification orders them.
	 */
	memory->memoryTypeCount = AMDRDNA_VK_MEMORY_TYPES;
	memory->memoryTypes[AMDRDNA_VK_MEMORY_DEVICE].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	memory->memoryTypes[AMDRDNA_VK_MEMORY_DEVICE].heapIndex = AMDRDNA_VK_MEMORY_DEVICE;
	memory->memoryTypes[AMDRDNA_VK_MEMORY_HOST].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	memory->memoryTypes[AMDRDNA_VK_MEMORY_HOST].heapIndex = AMDRDNA_VK_MEMORY_HOST;

	/* The heaps: the VRAM outside the window, and what the window holds. */
	memory->memoryHeapCount = AMDRDNA_VK_MEMORY_TYPES;
	memory->memoryHeaps[AMDRDNA_VK_MEMORY_DEVICE].size = amdrdna_vk_heap_bytes(session->owner, false);
	memory->memoryHeaps[AMDRDNA_VK_MEMORY_DEVICE].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
	memory->memoryHeaps[AMDRDNA_VK_MEMORY_HOST].size = amdrdna_vk_heap_bytes(session->owner, true);
	memory->memoryHeaps[AMDRDNA_VK_MEMORY_HOST].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

	/* Replies the present word and the record. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkPhysicalDeviceMemoryProperties(reply, memory);

	/* Succeeded: the memory is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceQueueFamilyProperties: [physical][present][count][array count]
 * -> [present][count][array count][families].  Two families of one queue
 * each: the universal family, then the compute family.
 */
static int
device_families(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkQueueFamilyProperties family;
	uint64_t array_count;
	uint64_t index;
	bool video;

	/* The physical device, the present word and the count, then the array's count. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	array_count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An array of more than the families is malformed. */
	if (array_count > AMDRDNA_VK_FAMILIES)
		return EINVAL;

	/* The universal family decodes video when the GPU's video engine is up. */
	video = amdrdna_vk_vcn_available(session->owner);

	/* Replies the present word, the count, then each family the array holds. */
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, AMDRDNA_VK_FAMILIES);
	drv_i915_wire_reply_u64(reply, array_count);
	for (index = 0U; index < array_count; index++) {
		/* Graphics with compute and transfer, or compute alone; one queue, any granularity. */
		kern_memset(&family, 0, sizeof(family));
		family.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
		if (index == AMDRDNA_VK_FAMILY_COMPUTE)
			family.queueFlags = VK_QUEUE_COMPUTE_BIT;
		if (index == AMDRDNA_VK_FAMILY_UNIVERSAL && video)
			family.queueFlags |= VK_QUEUE_VIDEO_DECODE_BIT_KHR;
		family.queueCount = 1U;
		family.timestampValidBits = DEVICE_TIMESTAMP_BITS;
		family.minImageTransferGranularity.width = 1U;
		family.minImageTransferGranularity.height = 1U;
		family.minImageTransferGranularity.depth = 1U;
		i915_vkc_enc_VkQueueFamilyProperties(reply, &family);
	}

	/* Succeeded: the families are reported. */
	return 0;
}

/*
 * Reports what the executor does with a format.  The colour images, those
 * of one or two bytes a texel and the two-plane images are pitch-linear,
 * so both tilings have the same features; the depth formats (with stencil
 * or without) are drawn into, sampled, copied and blitted, in optimal
 * tiling;
 * the storage formats are a storage image's (R8G8B8A8_UNORM's too); the
 * vertex formats the compositor's quads read are a vertex buffer's, every
 * format a buffer descriptor fetches a uniform texel buffer's, and the
 * storage formats a storage texel buffer's.
 */
static void
device_format_features(
	uint32_t format,
	bool video,
	VkFormatProperties *properties)
{
	uint32_t code;
	uint32_t components;
	uint32_t bytes;
	uint32_t layout;
	bool fetched;
	bool storage;

	kern_memset(properties, 0, sizeof(*properties));

	/* The colour formats: rendered to, blended, sampled, copied and blitted; R8G8B8A8_UNORM a storage image too. */
	switch (format) {
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_R8G8B8A8_SRGB:
		properties->optimalTilingFeatures = DEVICE_COLOUR_FEATURES;
		properties->linearTilingFeatures = DEVICE_COLOUR_FEATURES;
		break;
	case VK_FORMAT_R8G8B8A8_UNORM:
		properties->optimalTilingFeatures = DEVICE_COLOUR_FEATURES | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
		properties->linearTilingFeatures = DEVICE_COLOUR_FEATURES | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
		break;
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		/* The depth formats: in the depth unit's own layout (optimal tiling). */
		properties->optimalTilingFeatures = DEVICE_DEPTH_FEATURES;
		break;
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8G8_UNORM:
		/* One or two bytes a texel: sampled and copied. */
		properties->optimalTilingFeatures = DEVICE_NARROW_FEATURES;
		properties->linearTilingFeatures = DEVICE_NARROW_FEATURES;
		break;
	case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
		/* Two planes: copied (their views sample them), and in optimal tiling a video decode's pictures while the engine is up. */
		properties->optimalTilingFeatures = DEVICE_PLANAR_FEATURES;
		if (video)
			properties->optimalTilingFeatures |= DEVICE_VIDEO_FEATURES;
		properties->linearTilingFeatures = DEVICE_PLANAR_FEATURES;
		break;
	default:
		break;
	}

	/* The vertex formats. */
	switch (format) {
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32B32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		properties->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
		break;
	default:
		break;
	}

	/*
	 * The storage formats: a storage image's, copied, and a colour
	 * attachment (blended unless its numbers are integers, whose targets
	 * bypass the blending); R32 integers with the atomics.
	 *
	 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_formats.c
	 * (radv_is_colorbuffer_format_blendable: not for NUMBER_UINT and
	 * NUMBER_SINT; radv_physical_device_get_format_properties:
	 * COLOR_ATTACHMENT for a colour buffer format, COLOR_ATTACHMENT_BLEND
	 * when blendable).
	 */
	storage = amdrdna_vk_format_storage(format, &bytes);
	if (storage) {
		properties->optimalTilingFeatures = DEVICE_STORAGE_FEATURES | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
		if (amdrdna_vk_target_format(format, &layout) && (layout & AMDRDNA_CX_TARGET_BLEND_BYPASS) == 0U)
			properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
		properties->linearTilingFeatures = properties->optimalTilingFeatures;
	}
	if (format == VK_FORMAT_R32_UINT || format == VK_FORMAT_R32_SINT) {
		properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
		properties->linearTilingFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
	}

	/* The formats a buffer descriptor fetches are a uniform texel buffer's; the storage ones a storage texel buffer's. */
	fetched = amdrdna_vk_texel_format(format, &code, &components, &bytes);
	if (fetched)
		properties->bufferFeatures |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
	if (storage || format == VK_FORMAT_R8G8B8A8_UNORM)
		properties->bufferFeatures |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
	if (format == VK_FORMAT_R32_UINT || format == VK_FORMAT_R32_SINT)
		properties->bufferFeatures |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT;
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

	/* A storage image needs the storage feature. */
	if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;

	/* Rendering colour needs the colour attachment feature. */
	if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;

	/* Decoding into the image needs the video output feature, keeping it as a reference the video DPB feature. */
	if ((usage & VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR) != 0U)
		required |= VK_FORMAT_FEATURE_VIDEO_DECODE_OUTPUT_BIT_KHR;
	if ((usage & VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR) != 0U)
		required |= VK_FORMAT_FEATURE_VIDEO_DECODE_DPB_BIT_KHR;

	/* Rendering depth needs the depth attachment feature, which the depth format has. */
	if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U)
		required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;

	/* Succeeded: the features the usage needs. */
	return required;
}

/* vkGetPhysicalDeviceFormatProperties: [physical][format][present] -> [present][properties]. */
static int
device_format(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFormatProperties properties;
	uint32_t format;
	bool video;

	/* The physical device, the format and the present word. */
	(void)drv_i915_wire_read_u64(reader);
	format = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Replies the present word and the format's features (the video ones while the video engine is up). */
	video = amdrdna_vk_vcn_available(session->owner);
	device_format_features(format, video, &properties);
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkFormatProperties(reply, &properties);

	/* Succeeded: the format is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceImageFormatProperties: [physical][format][type][tiling][usage][flags][present]
 * -> [result][present][properties].  What device_image_limits says.
 */
static int
device_image_format(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageFormatProperties image;
	bool video;
	uint32_t format;
	uint32_t type;
	uint32_t tiling;
	uint32_t usage;
	uint32_t flags;
	uint32_t result;

	/* The format, type, tiling, usage and flags; the physical device and the present word are skipped. */
	(void)drv_i915_wire_read_u64(reader);
	format = drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	tiling = drv_i915_wire_read_u32(reader);
	usage = drv_i915_wire_read_u32(reader);
	flags = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* What such an image may be (a video decode's picture while the video engine is up). */
	video = amdrdna_vk_vcn_available(session->owner);
	result = device_image_limits(format, type, tiling, usage, flags, video, &image);

	/* Replies the result, the present word and the record. */
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkImageFormatProperties(reply, &image);

	/* Succeeded: the image format is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceImageFormatProperties2 with an external memory handle
 * type, the chains as uapi/gpu-op.h lays them out: [physical][1]
 * [IMAGE_FORMAT_INFO_2][1][EXTERNAL_IMAGE_FORMAT_INFO][0][handle type]
 * [format][type][tiling][usage][flags][1][IMAGE_FORMAT_PROPERTIES_2][1]
 * [EXTERNAL_IMAGE_FORMAT_PROPERTIES][0] -> [result][1], the two output
 * headers, the external memory properties and the image's limits.  An
 * image the executor makes is in memory exported and imported as any
 * memory is (DEVICE_EXTERNAL_HANDLE); its limits are the image's without
 * the handle type.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/powervr/vk/vk-device.c
 * (image_format_external: the same wire).
 */
static int
device_image_format_external(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageFormatProperties image;
	bool video;
	uint32_t handle_type;
	uint32_t format;
	uint32_t type;
	uint32_t tiling;
	uint32_t usage;
	uint32_t flags;
	uint32_t result;

	/* The physical device and the input chain up to the handle type. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	handle_type = drv_i915_wire_read_u32(reader);

	/* The image, then the output chain's headers. */
	format = drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	tiling = drv_i915_wire_read_u32(reader);
	usage = drv_i915_wire_read_u32(reader);
	flags = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* What such an image may be; an image the executor cannot make has no external memory either. */
	video = amdrdna_vk_vcn_available(session->owner);
	result = device_image_limits(format, type, tiling, usage, flags, video, &image);
	if (result != (uint32_t)VK_SUCCESS)
		handle_type = 0U;

	/* The result, the two headers, the external memory properties, then the limits. */
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
	drv_i915_wire_reply_u64(reply, 0U);
	device_reply_external(reply, handle_type);
	i915_vkc_enc_VkImageFormatProperties(reply, &image);

	/* Succeeded: the image format is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceExternalBufferProperties with the context's native
 * handle type, the chains as uapi/gpu-op.h lays them out: [physical][1]
 * [EXTERNAL_BUFFER_INFO][0][flags][usage][handle type][1]
 * [EXTERNAL_BUFFER_PROPERTIES][0] -> [1], the header and the external
 * memory properties: a buffer's memory is exported and imported as any
 * memory is.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/powervr/vk/vk-device.c
 * (buffer_external: the same wire).
 */
static int
device_buffer_external(
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
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES);
	drv_i915_wire_reply_u64(reply, 0U);
	device_reply_external(reply, handle_type);

	/* Succeeded: the buffer's external memory is reported. */
	return 0;
}

/*
 * Writes the external memory properties of one handle type: exported and
 * imported, from and compatible with itself only, for the node's
 * allocation handle (DEVICE_EXTERNAL_HANDLE); none for any other.
 */
static void
device_reply_external(
	struct i915_wire_writer *reply,
	uint32_t handle_type)
{
	uint32_t features;

	/* The node's handle type is exported and imported as itself; another is not shared. */
	features = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
	if (handle_type != DEVICE_EXTERNAL_HANDLE) {
		features = 0U;
		handle_type = 0U;
	}

	/* The features, the types it exports from, the types it is compatible with. */
	drv_i915_wire_reply_u32(reply, features);
	drv_i915_wire_reply_u32(reply, handle_type);
	drv_i915_wire_reply_u32(reply, handle_type);
}

/*
 * Works out what an image of a format, type, tiling, usage and flags may
 * be: VK_SUCCESS and its limits, or VK_ERROR_FORMAT_NOT_SUPPORTED and
 * zeros.  A 2D image (cube compatible or not) or a 3D one, of one sample
 * (or four: a 2D colour or depth image, optimal, not a cube, of the usages
 * an image of samples has), whose usage the format's features on the tiling cover, with levels down
 * to one pixel and layers (one level and one layer, 2D and not a cube,
 * for the depth formats); a two-plane image of one level that is copied
 * or whose planes are sampled (only with
 * VK_IMAGE_CREATE_EXTENDED_USAGE_BIT), with only its plane views' flags.
 */
static uint32_t
device_image_limits(
	uint32_t format,
	uint32_t type,
	uint32_t tiling,
	uint32_t usage,
	uint32_t flags,
	bool video,
	VkImageFormatProperties *image)
{
	VkFormatProperties properties;
	uint32_t features;
	uint32_t required;
	bool planar;
	bool depth;
	bool colour;
	bool allowed;

	/* The features of the tiling asked about, and what the usage needs. */
	device_format_features(format, video, &properties);
	features = properties.optimalTilingFeatures;
	if (tiling == VK_IMAGE_TILING_LINEAR)
		features = properties.linearTilingFeatures;
	required = device_usage_features(usage);
	kern_memset(image, 0, sizeof(*image));

	/*
	 * A two-plane image: some usage the planes have, sampling through
	 * the planes' views only, and only the views' flags.  Any other: a
	 * format with features whose usage they cover.
	 */
	planar = amdrdna_vk_format_planar(format);
	depth = amdrdna_vk_format_depth(format);
	allowed = true;
	if (features == 0U || (type != VK_IMAGE_TYPE_2D && type != VK_IMAGE_TYPE_3D)) {
		allowed = false;
	} else if (type == VK_IMAGE_TYPE_3D && (depth || planar || (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0U)) {
		allowed = false;
	} else if ((depth || planar) && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0U) {
		allowed = false;
	} else if (planar) {
		if (usage == 0U || (usage & ~(uint32_t)DEVICE_PLANAR_USAGES) != 0U)
			allowed = false;
		if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U && (flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) == 0U)
			allowed = false;
		if ((flags & ~(uint32_t)DEVICE_PLANAR_FLAGS) != 0U)
			allowed = false;
		if ((features & required & DEVICE_VIDEO_FEATURES) != (required & DEVICE_VIDEO_FEATURES))
			allowed = false;
	} else if ((features & required) != required) {
		allowed = false;
	}

	/* An image that is not made has no limits. */
	if (!allowed)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;

	/*
	 * Levels down to one pixel, up to 2048 layers of a 2D image or 2048
	 * deep a 3D image (one level and one layer for the depth formats and
	 * the two-plane format), one sample, up to 16384 a side and 1 GiB.
	 */
	image->maxExtent.width = AMDRDNA_VK_IMAGE_SIDE_MAX;
	image->maxExtent.height = AMDRDNA_VK_IMAGE_SIDE_MAX;
	image->maxExtent.depth = 1U;
	image->maxMipLevels = AMDRDNA_VK_LEVELS_MAX;
	image->maxArrayLayers = AMDRDNA_VK_LAYERS_MAX;
	if (type == VK_IMAGE_TYPE_3D) {
		image->maxExtent.width = AMDRDNA_VK_DEPTH_MAX;
		image->maxExtent.height = AMDRDNA_VK_DEPTH_MAX;
		image->maxExtent.depth = AMDRDNA_VK_DEPTH_MAX;
		image->maxArrayLayers = 1U;
	}
	if (depth || planar) {
		image->maxMipLevels = 1U;
		image->maxArrayLayers = 1U;
	}
	image->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
	image->maxResourceSize = AMDRDNA_VK_RESOURCE_MAX;

	/* Four samples too for a 2D colour or depth image drawn into, sampled or resolved, of optimal tiling, not a cube. */
	colour = amdrdna_vk_format_colour(format);
	if (type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL &&
	    (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) == 0U &&
	    (usage & ~(uint32_t)DEVICE_SAMPLED_USAGES) == 0U &&
	    (colour || depth))
		image->sampleCounts = DEVICE_SAMPLE_COUNTS;

	/* Succeeded: such an image is made. */
	return (uint32_t)VK_SUCCESS;
}

/* vkCreateDevice: [physical][present][create info][allocator][present][id] -> [result][present][id]. */
static int
device_create(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vkc_core11 core11;
	struct i915_vkc_core12 core12;
	VkDeviceCreateInfo info;
	uint64_t present;
	uint64_t identity;
	int error;

	/* The physical device, then the create info when present; only its 1.1 and 1.2 features are checked. */
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

	/* Refuses a Vulkan 1.1 or 1.2 feature the device does not report (libvulkan refuses it first). */
	device_core11(session, &core11);
	error = i915_vkc_core11_check_device(&info, &core11);
	if (error != 0)
		return error;
	device_core12(&core12);
	error = i915_vkc_core12_check_device(&info, &core12);
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
	struct amdrdna_vk_session *session,
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

	/* A family that exists; its queue's jobs complete on a timeline of 1 to 63. */
	if (family >= AMDRDNA_VK_FAMILIES || timeline == 0U || timeline >= DEVICE_TIMELINES)
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
	struct amdrdna_vk_session *session,
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
	(void)amdrdna_vk_object_remove(session, kind, identity);

	/* Succeeded: the identity is forgotten. */
	return 0;
}

/*
 * vkQueueWaitIdle and vkDeviceWaitIdle: [handle] -> [result].  The
 * session's newest runs on both queues are waited for; a run that did not
 * end in time loses the device.
 */
static int
device_wait_idle(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct amdrdna_vk_ticket last[2];
	uint32_t result;
	uint32_t queue;
	int error;

	/* The queue or device. */
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Waits for each queue's newest run. */
	result = (uint32_t)VK_SUCCESS;
	amdrdna_vk_execute_last(session, last);
	for (queue = 0U; queue < 2U; queue++) {
		error = amdrdna_vk_gpu_wait(session->owner, &last[queue]);
		if (error != 0)
			result = (uint32_t)VK_ERROR_DEVICE_LOST;
	}

	/* Replies whether the work did its work. */
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the queue is idle. */
	return 0;
}

/* The token's destructor: the token is static, nothing is freed. */
static int
device_token_destroy(
	struct amdrdna_vk_session *session,
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
	struct amdrdna_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct amdrdna_vk_object *object;
	int error;

	/* Publishes it. */
	error = amdrdna_vk_object_publish(session, kind, identity, &device_token, device_token_destroy, &object);
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
