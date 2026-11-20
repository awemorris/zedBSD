/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private Vulkan discovery describes the finite Keiland graphics path, without unimplemented optional features. */
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
#include "drivers/gpu/videocore/vk/vulkan-video.h"
#endif
#include "drivers/gpu/videocore/vk/vulkan-device.h"
#include "drivers/gpu/videocore/vk/vulkan-memory.h"
#include "drivers/gpu/videocore/vk/vulkan-occlusion.h"
#include "drivers/gpu/videocore/vk/vulkan-resource.h"
#include "drivers/gpu/videocore/native-pass.h"
#include "drivers/gpu/videocore/bcm2711-private.h"
#include "drivers/gpu/videocore/render-device.h"

/* Generated device-independent Zlib record encoding remains shared read-only source. */
#include "drivers/gpu/i915/vk/vk-codec.inc"

/* Driver-owned limits bound the implemented single-sample graphics path. */
#define VULKAN_IMAGE_SIDE 4096U

/* The most bytes one image resource takes, as image creation bounds it. */
#define VULKAN_RESOURCE_LIMIT (256ULL << 20)

/* The levels a 3D image's longest side (BCM2711_VULKAN_IMAGE_DEPTH) halves through to one texel. */
#define VULKAN_VOLUME_LEVELS 9U
#define VULKAN_PUSH_BYTES 128U
#define VULKAN_DESCRIPTOR_SETS 4U
#define VULKAN_TEXTURES 8U

/*
 * The compute interface of V3D 7.1: storage buffers a stage reads (as many
 * as a set layout holds), the shader compiler's workgroup ceiling (128
 * invocations, which the common SPIR-V frontend admits at most), its
 * shared memory (16 KiB a workgroup) and the CSD's 16-bit workgroup counts.
 * A storage buffer's offset keeps the TMU's natural alignment with room to
 * spare; its range stays below the 32-bit device address space.
 */
#define VULKAN_STORAGE_BUFFERS 8U
#define VULKAN_COMPUTE_INVOCATIONS 128U
#define VULKAN_COMPUTE_SHARED 16384U
#define VULKAN_COMPUTE_GROUPS 65535U
#define VULKAN_STORAGE_RANGE (1U << 27)
#define VULKAN_STORAGE_ALIGNMENT 32U

/* The storage images a stage and a set hold on a 7.1 engine (vulkan-layout.c VULKAN_LAYOUT_IMAGES). */
#define VULKAN_STORAGE_IMAGES 8U

static int fixed_query(struct bcm2711_vulkan_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int queue_properties(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int format_query(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int image_query(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void identity_query(struct i915_wire_writer *reply, const VkPhysicalDeviceProperties *properties);
static int properties2_query(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int features2_query(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void device_properties(const struct bcm2711_vulkan_session *session, VkPhysicalDeviceProperties *properties);
static void format_properties(const struct bcm2711_vulkan_session *session, VkFormat format, VkFormatProperties *properties);
static void set_float(float *destination, uint32_t bits);
static bool compute_engine(const struct bcm2711_vulkan_session *session);

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
	if (opcode != GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2 &&
	    opcode != GPU_OP_GET_PHYSICAL_DEVICE_FEATURES2 &&
	    (opcode < GPU_OP_GET_PHYSICAL_DEVICE_FEATURES ||
	     opcode > GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES))
		return 0;
	*handled = 1;
	if (requested != 1)
		return EINVAL;

	/* The device's identity, which an exported allocation carries to its importer, or its Vulkan 1.1 properties. */
	if (opcode == GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2) {
		error = properties2_query(session, reader, reply);
		if (error != 0)
			return error;

		/* Succeeded: the chain's structures and the properties are in the reply. */
		return 0;
	}

	/* The Vulkan 1.1 features. */
	if (opcode == GPU_OP_GET_PHYSICAL_DEVICE_FEATURES2) {
		error = features2_query(session, reader, reply);
		if (error != 0)
			return error;

		/* Succeeded: the chain's structures and the features are in the reply. */
		return 0;
	}

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

	/* Feature structures begin zeroed, and only the implemented optional features are claimed. */
	if (opcode == GPU_OP_GET_PHYSICAL_DEVICE_FEATURES) {
		features = i915_vkc_array(reader, &session->arena, 1, sizeof(*features));
		if (features == NULL)
			return ENOMEM;
		bcm2711_vulkan_device_features(session, features);
		drv_i915_wire_reply_u64(reply, 1);
		i915_vkc_enc_VkPhysicalDeviceFeatures(reply, features);
		return 0;
	}

	/* The private protocol requires native version 1.1; limits describe only this driver's finite graphics subset. */
	if (opcode == GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES) {
		properties = i915_vkc_array(reader, &session->arena, 1, sizeof(*properties));
		if (properties == NULL)
			return ENOMEM;
		device_properties(session, properties);
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
	memory->memoryHeaps[0].size = BCM2711_VULKAN_MEMORY_BUDGET;
	memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkPhysicalDeviceMemoryProperties(reply, memory);

	/* Succeeded: the driver budget and coherent memory policy share stable type and heap index zero. */
	return 0;
}

/*
 * vkGetPhysicalDeviceProperties2 in either of its forms: [physical]
 * [present][PROPERTIES_2 header][the chain's headers] -> the device's
 * identity (identity_query()) or its Vulkan 1.1 properties.
 */
static int
properties2_query(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *physical;
	VkPhysicalDeviceProperties *properties;
	struct i915_vkc_core11 core11;
	struct i915_vkc_core12 core12;
	uint64_t identity;
	int form;
	int error;

	/* The physical device, then the output chain, which says which form is asked for. */
	identity = drv_i915_wire_read_u64(reader);
	error = i915_vkc_read_properties2(reader, &form);
	if (error != 0) {
		reader->error = 1;
		return error;
	}
	physical = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PHYSICAL_DEVICE, identity);
	if (physical == NULL)
		return EINVAL;

	/* The ordinary properties, as the plain query gives them. */
	properties = i915_vkc_array(reader, &session->arena, 1, sizeof(*properties));
	if (properties == NULL)
		return ENOMEM;
	device_properties(session, properties);

	/* The identity, which the export and import of memory name. */
	if (form == I915_VKC_PROPERTIES2_IDENTITY) {
		identity_query(reply, properties);

		/* Succeeded: the identity every context of this device reports. */
		return 0;
	}

	/* The Vulkan 1.1 or 1.2 properties, then the record. */
	bcm2711_vulkan_device_core11(session, &core11);
	bcm2711_vulkan_device_core12(session, &core12);
	if (form == I915_VKC_PROPERTIES2_CORE12)
		i915_vkc_reply_core12_properties(reply, &core12);
	else
		i915_vkc_reply_core11_properties(reply, &core11);
	i915_vkc_enc_VkPhysicalDeviceProperties(reply, properties);

	/* Succeeded: the properties are reported. */
	return 0;
}

/*
 * The identity form of vkGetPhysicalDeviceProperties2 (only
 * VkPhysicalDeviceIDProperties chained): libvulkan stamps the identity into
 * every allocation it exports as an opaque fd, and an importer checks it
 * against its own.  The identities are fixed: the device's names this one
 * V3D, the driver's this driver.
 */
static void
identity_query(
	struct i915_wire_writer *reply,
	const VkPhysicalDeviceProperties *properties)
{
	static const uint8_t device_uuid[VK_UUID_SIZE] = {
		'z', 'e', 'd', 'B', 'S', 'D', '-', 'b', 'c', 'm', '2', '7', '1', '1', 'v', '3'
	};
	static const uint8_t driver_uuid[VK_UUID_SIZE] = {
		'z', 'e', 'd', 'B', 'S', 'D', '-', 'v', '3', 'd', '-', 'v', 'k', '-', '0', '1'
	};
	static const uint8_t luid[VK_LUID_SIZE] = {0};

	/* The chain's two headers, the identity (no Windows adapter: no LUID), then the properties. */
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
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u32(reply, VK_FALSE);
	i915_vkc_enc_VkPhysicalDeviceProperties(reply, properties);

	/* Succeeded: the identity is in the reply. */
	return;
}

/*
 * vkGetPhysicalDeviceFeatures2 with the Vulkan 1.1 features chained:
 * [physical][output chain] -> [present][chain][1.1 fields]
 * [VkPhysicalDeviceFeatures].
 */
static int
features2_query(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *physical;
	VkPhysicalDeviceFeatures *features;
	struct i915_vkc_core11 core11;
	struct i915_vkc_core12 core12;
	uint64_t identity;
	int form;
	int error;

	/* The physical device, then the output chain of the 1.1 or 1.2 feature structures. */
	identity = drv_i915_wire_read_u64(reader);
	error = i915_vkc_read_features2(reader, &form);
	if (error != 0) {
		reader->error = 1;
		return error;
	}
	physical = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PHYSICAL_DEVICE, identity);
	if (physical == NULL)
		return EINVAL;

	/* The record of the core features, as the plain query gives it. */
	features = i915_vkc_array(reader, &session->arena, 1, sizeof(*features));
	if (features == NULL)
		return ENOMEM;
	bcm2711_vulkan_device_features(session, features);

	/* The 1.1 or 1.2 features, then the record. */
	bcm2711_vulkan_device_core11(session, &core11);
	bcm2711_vulkan_device_core12(session, &core12);
	if (form == I915_VKC_FEATURES2_CORE12)
		i915_vkc_reply_core12_features(reply, &core12);
	else
		i915_vkc_reply_core11_features(reply, &core11);
	i915_vkc_enc_VkPhysicalDeviceFeatures(reply, features);

	/* Succeeded: the features are reported. */
	return 0;
}

/*
 * Fills the device's Vulkan 1.1 answers.
 *
 * The subgroup is one invocation, in the vertex, fragment and compute
 * programs of a 7.1 and the vertex and fragment programs of a 4.2 (which
 * has no compute queue).  Multiview renders
 * each view of a subpass into its layer as a native pass of its own, with
 * the view a uniform of the programs (no geometry or tessellation stage);
 * instances are numbered as without multiview.  A set's descriptors are
 * bounded by the per-set limits, which together stay below 1024, and one
 * allocation may take the whole heap.
 */
void
bcm2711_vulkan_device_core11(
	const struct bcm2711_vulkan_session *session,
	struct i915_vkc_core11 *core11)
{
	bool compute;

	/* What every executor starts from. */
	i915_vkc_core11_defaults(core11);

	/* A 4.2 runs no compute program. */
	compute = compute_engine(session);
	if (!compute)
		core11->subgroup.supportedStages &= ~VK_SHADER_STAGE_COMPUTE_BIT;

	/* Multiview, the views drawn one after another. */
	core11->multiview.multiview = VK_TRUE;
	core11->multiview_limits.maxMultiviewViewCount = BCM2711_VULKAN_VIEWS;
	core11->multiview_limits.maxMultiviewInstanceIndex = (1U << 27) - 1U;

	/* The descriptors of one set and the largest allocation. */
	core11->maintenance3.maxPerSetDescriptors = 1024U;
	core11->maintenance3.maxMemoryAllocationSize = BCM2711_VULKAN_MEMORY_BUDGET;
}

/* Fills the device's Vulkan 1.2 answers: those every executor starts from, under its name. */
void
bcm2711_vulkan_device_core12(
	const struct bcm2711_vulkan_session *session,
	struct i915_vkc_core12 *core12)
{
	/* Every engine alike: the shaders' features, the limits and the name. */
	(void)session;
	i915_vkc_core12_defaults(core12, "zedBSD v3d");
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
	bool compute;

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

	/* The one serialized V3D runner supports graphics and transfers, with no timestamp query support. */
	kern_memset(&properties, 0, sizeof(properties));
	properties.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;

	/* A 7.1 engine also runs compute dispatches, each as a CSD job; the 4.2 compiler has no compute programs. */
	compute = compute_engine(session);
	if (compute)
		properties.queueFlags |= VK_QUEUE_COMPUTE_BIT;
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	/* A session promised video decode also decodes on the one family. */
	if (bcm2711_vulkan_video_session(session))
		properties.queueFlags |= BCM2711_VIDEO_QUEUE_DECODE;
#endif
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
	format_properties(session, format, &properties);
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
	bool layered;
	bool depth_layered;
	uint32_t allowed;
	bool volume;
	VkResult status;
	/* The format, type and usage last reported as refused (a report only; a race repeats one line at most). */
	static uint32_t refused[3];

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

	/* 2D colour sampling/rendering and transfers, and depth/stencil attachments, belong to the native graphics path. */
	kern_memset(&properties, 0, sizeof(properties));
	format_properties(session, format, &formats);
	features = formats.optimalTilingFeatures;
	if (tiling == VK_IMAGE_TILING_LINEAR)
		features = formats.linearTilingFeatures;
	known_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		      VK_IMAGE_USAGE_STORAGE_BIT;

	/* A depth/stencil format is an attachment, which may declare transfer usage as image creation accepts; a colour format is never one. */
	if ((features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
		known_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
			      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	}
	/*
	 * An optimally tiled colour image has array layers and may be viewed
	 * as a cube; an optimally tiled depth/stencil image has array layers
	 * alone (a 2D array, one level); no other image has either.
	 */
	layered = false;
	if (tiling == VK_IMAGE_TILING_OPTIMAL && bcm2711_vulkan_colour_format(format) != NULL)
		layered = true;
	depth_layered = false;
	if (tiling == VK_IMAGE_TILING_OPTIMAL && bcm2711_vulkan_depth_format(format))
		depth_layered = true;
	status = VK_SUCCESS;

	/*
	 * A colour image's usage needs the feature that serves it; a
	 * depth/stencil image keeps the transfer usage image creation accepts.
	 */
	if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 && (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0)
		status = VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 && (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)
		status = VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0 && (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0)
		status = VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
		if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 && (features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0)
			status = VK_ERROR_FORMAT_NOT_SUPPORTED;
		if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 && (features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0)
			status = VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	/*
	 * A 3D image (on a 7.1 engine, whose compiler reads 3D coordinates) is
	 * an optimally tiled colour image, sampled, copied and stored into but
	 * not drawn into, with no create flag but the mutable format of a
	 * storage image (checked below).
	 */
	volume = false;
	if (type == VK_IMAGE_TYPE_3D) {
		volume = true;
		if (!layered ||
		    !compute_engine(session) ||
		    (flags & ~(uint32_t)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0 ||
		    (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0)
			status = VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	if (features == 0 ||
	    (type != VK_IMAGE_TYPE_2D && !volume) ||
	    (tiling != VK_IMAGE_TILING_LINEAR &&
	     tiling != VK_IMAGE_TILING_OPTIMAL) ||
	    usage == 0 ||
	    (usage & ~known_usage) != 0)
		status = VK_ERROR_FORMAT_NOT_SUPPORTED;
	/* A layered image may be cube compatible; a 7.1 engine's storage image of a storage format mutable, viewed as another of its size. */
	allowed = 0;
	if (layered)
		allowed = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	if (layered && compute_engine(session) && (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0 && bcm2711_vulkan_storage_texel(format) != 0)
		allowed |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	if ((flags & ~allowed) != 0)
		status = VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* A known format the device does not make in this form is reported, once a run of the same question, as a client then goes without the image. */
	if (status != VK_SUCCESS && features != 0 && (format != refused[0] || type != refused[1] || usage != refused[2])) {
		refused[0] = format;
		refused[1] = type;
		refused[2] = usage;
		kern_logf("v3d: vk: no image of format %u type %u tiling %u usage %#x flags %#x\n",
			  (unsigned)format, (unsigned)type, (unsigned)tiling, (unsigned)usage, (unsigned)flags);
	}

	/* Successful resources stay within the same layer/single-sample/dimension limits as native image creation. */
	if (status == VK_SUCCESS) {
		properties.maxExtent.width = VULKAN_IMAGE_SIDE;
		properties.maxExtent.height = VULKAN_IMAGE_SIDE;
		properties.maxExtent.depth = 1;
		properties.maxMipLevels = 1;
		properties.maxArrayLayers = 1;
		properties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
		properties.maxResourceSize = (uint64_t)VULKAN_IMAGE_SIDE * VULKAN_IMAGE_SIDE * 4;

		/*
		 * An optimally tiled RGBA8 or BGRA8 image has its whole mip chain,
		 * which at most doubles its bytes, and its array layers, all of
		 * them within one resource's bytes.
		 */
		if (layered) {
			properties.maxMipLevels = BCM2711_VULKAN_IMAGE_LEVELS;
			properties.maxArrayLayers = BCM2711_VULKAN_IMAGE_LAYERS;
			properties.maxResourceSize = VULKAN_RESOURCE_LIMIT;
		}

		/* An optimally tiled depth/stencil image has its array layers, within one resource's bytes, and its one level. */
		if (depth_layered && type == VK_IMAGE_TYPE_2D) {
			properties.maxArrayLayers = BCM2711_VULKAN_IMAGE_LAYERS;
			properties.maxResourceSize = VULKAN_RESOURCE_LIMIT;
		}

		/* A 3D image has one layer of slices, no side of them longer than the most slices, and the levels its sides halve through. */
		if (volume) {
			properties.maxExtent.width = BCM2711_VULKAN_IMAGE_DEPTH;
			properties.maxExtent.height = BCM2711_VULKAN_IMAGE_DEPTH;
			properties.maxExtent.depth = BCM2711_VULKAN_IMAGE_DEPTH;
			properties.maxMipLevels = VULKAN_VOLUME_LEVELS;
			properties.maxArrayLayers = 1;
		}

		/*
		 * A 7.1 engine's 2D attachment of an optimally tiled colour format
		 * it draws into, or of a depth/stencil format, has four samples
		 * too, sampled a sample at a time or not (one level and one layer,
		 * which image creation asks of a multisampled image); a storage
		 * image has one.
		 */
		if (compute_engine(session) &&
		    type == VK_IMAGE_TYPE_2D &&
		    tiling == VK_IMAGE_TILING_OPTIMAL &&
		    flags == 0 &&
		    (usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0 &&
		    (features & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0)
			properties.sampleCounts |= VK_SAMPLE_COUNT_4_BIT;
	}

	/* Failure returns a complete zero output record instead of leaving uninitialized client storage. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkImageFormatProperties(reply, &properties);

	/* Succeeded: the response records either exact native support or an explicit unsupported-format result. */
	return 0;
}

/*
 * Fills independent bounded graphics limits that the final native runtime must enforce before capability publication.
 *
 * The name and the device number follow the engine's identified version:
 * V3D 4.2 on the Raspberry Pi 4, 7.1 on the Pi 5.
 */
static void
device_properties(
	const struct bcm2711_vulkan_session *session,
	VkPhysicalDeviceProperties *properties)
{
	static const char name_42[] = "zedBSD V3D 4.2 (Keiland graphics)";
	static const char name_71[] = "zedBSD V3D 7.1 (Keiland graphics)";
	const struct bcm2711_v3d *engine;
	VkPhysicalDeviceLimits *limits;
	uint32_t layout;
	bool compute;

	/* The private 1.2 version is a transport requirement; optional features and unimplemented stages remain absent. */
	properties->apiVersion = VK_MAKE_VERSION(1, 2, 0);
	properties->driverVersion = 1;
	properties->vendorID = 0x14e4;
	properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

	/* The hub's identification says 4.2 or 7.1 (technology version 7 in its low four bits). */
	engine = session->render->device->space.native;
	layout = engine->hardware.hub_ident[1];
	if ((layout & 15U) == 7U) {
		properties->deviceID = 0x0701;
		kern_memcpy(properties->deviceName, name_71, sizeof(name_71));
	} else {
		properties->deviceID = 0x0402;
		kern_memcpy(properties->deviceName, name_42, sizeof(name_42));
	}
	limits = &properties->limits;

	/* 2D images with their levels and layers, bounded memory and immutable kernel-owned shader programs use explicit finite budgets. */
	limits->maxImageDimension2D = VULKAN_IMAGE_SIDE;
	limits->maxImageDimensionCube = VULKAN_IMAGE_SIDE;
	limits->maxImageArrayLayers = BCM2711_VULKAN_IMAGE_LAYERS;
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
	limits->maxDescriptorSetUniformBuffersDynamic = 4;

	/* Scalar native vertex/VPM interfaces support bounded float attributes; a 7.1 engine draws into four colour targets, a 4.2 one into one. */
	limits->maxVertexInputAttributes = 16;
	limits->maxVertexInputBindings = 16;
	limits->maxVertexInputAttributeOffset = 2047;
	limits->maxVertexInputBindingStride = 2048;
	limits->maxVertexOutputComponents = 32;
	limits->maxFragmentInputComponents = 32;
	limits->maxFragmentOutputAttachments = 1;
	limits->maxFragmentCombinedOutputResources = 1;
	if ((layout & 15U) == 7U) {
		limits->maxFragmentOutputAttachments = BCM2711_NATIVE_PASS_TARGETS;
		limits->maxFragmentCombinedOutputResources = BCM2711_NATIVE_PASS_TARGETS;
	}
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
	if ((layout & 15U) == 7U)
		limits->maxColorAttachments = BCM2711_NATIVE_PASS_TARGETS;
	limits->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;

	/* A 7.1 engine's tile buffer draws with four samples a pixel too. */
	if ((layout & 15U) == 7U) {
		limits->framebufferColorSampleCounts |= VK_SAMPLE_COUNT_4_BIT;
		limits->framebufferDepthSampleCounts |= VK_SAMPLE_COUNT_4_BIT;
		limits->framebufferStencilSampleCounts |= VK_SAMPLE_COUNT_4_BIT;
	}

	/*
	 * A framebuffer with nothing attached has one sample on a 4.2 engine,
	 * whose passes name at least one attachment, and the one bit is the
	 * least Vulkan allows; a 7.1 engine's pass without attachments takes
	 * the samples of its pipelines, one or four.
	 */
	limits->framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	if ((layout & 15U) == 7U)
		limits->framebufferNoAttachmentsSampleCounts |= VK_SAMPLE_COUNT_4_BIT;

	/*
	 * Images are sampled with one sample, of colour, integer, depth and
	 * stencil formats alike; a 7.1 engine's four-sample images are
	 * fetched a sample at a time too (texelFetch of a sampler2DMS, the
	 * sample's texel of the 2x2 block a pixel's samples make), as Mesa
	 * 26.2.6 vulkan/v3dv_device.c reports.
	 */
	limits->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	if ((layout & 15U) == 7U) {
		limits->sampledImageColorSampleCounts |= VK_SAMPLE_COUNT_4_BIT;
		limits->sampledImageIntegerSampleCounts |= VK_SAMPLE_COUNT_4_BIT;
		limits->sampledImageDepthSampleCounts |= VK_SAMPLE_COUNT_4_BIT;
		limits->sampledImageStencilSampleCounts |= VK_SAMPLE_COUNT_4_BIT;
	}

	/* Storage images have one sample, the least Vulkan allows (Mesa's v3dv reports the same). */
	limits->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits->maxSampleMaskWords = 1;

	/* The four samples are not at Vulkan's standard locations (Mesa's v3dv reports the same). */
	limits->standardSampleLocations = VK_FALSE;

	/*
	 * Samplers bias the level of detail by up to 14 (the Mesa driver's
	 * value; the field holds 127) and have no anisotropy.  Points are 1 to 512
	 * pixels and lines 1 to 32 wide, in steps of two of the rasterizer's
	 * sub-pixel units: 1/64 on 7.1, 1/256 on 4.2 (Mesa 26.2.6
	 * src/broadcom/common/v3d_limits.h V3D_MAX_POINT_SIZE and
	 * V3D_MAX_LINE_WIDTH, and vulkan/v3dv_device.c's granularity).
	 */
	set_float(&limits->maxSamplerLodBias, 0x41600000U);
	set_float(&limits->maxSamplerAnisotropy, 0x3f800000U);
	set_float(&limits->pointSizeRange[0], 0x3f800000U);
	set_float(&limits->pointSizeRange[1], 0x44000000U);
	set_float(&limits->lineWidthRange[0], 0x3f800000U);
	set_float(&limits->lineWidthRange[1], 0x42000000U);
	if ((layout & 15U) == 7U) {
		set_float(&limits->pointSizeGranularity, 0x3d000000U);
		set_float(&limits->lineWidthGranularity, 0x3d000000U);
	} else {
		set_float(&limits->pointSizeGranularity, 0x3c000000U);
		set_float(&limits->lineWidthGranularity, 0x3c000000U);
	}
	limits->minMemoryMapAlignment = 4096;
	limits->minUniformBufferOffsetAlignment = 4;
	limits->optimalBufferCopyOffsetAlignment = 4;
	limits->optimalBufferCopyRowPitchAlignment = 4;
	limits->nonCoherentAtomSize = 64;

	/* The compute and storage buffer limits of a 7.1 engine; a 4.2 engine reports none, having no compute queue. */
	compute = compute_engine(session);
	if (compute) {
		limits->maxPerStageDescriptorStorageBuffers = VULKAN_STORAGE_BUFFERS;
		limits->maxDescriptorSetStorageBuffers = VULKAN_STORAGE_BUFFERS;
		limits->maxPerStageDescriptorStorageImages = VULKAN_STORAGE_IMAGES;
		limits->maxDescriptorSetStorageImages = VULKAN_STORAGE_IMAGES;
		limits->maxStorageBufferRange = VULKAN_STORAGE_RANGE;
		limits->minStorageBufferOffsetAlignment = VULKAN_STORAGE_ALIGNMENT;
		limits->maxComputeSharedMemorySize = VULKAN_COMPUTE_SHARED;
		limits->maxComputeWorkGroupCount[0] = VULKAN_COMPUTE_GROUPS;
		limits->maxComputeWorkGroupCount[1] = VULKAN_COMPUTE_GROUPS;
		limits->maxComputeWorkGroupCount[2] = VULKAN_COMPUTE_GROUPS;
		limits->maxComputeWorkGroupInvocations = VULKAN_COMPUTE_INVOCATIONS;
		limits->maxComputeWorkGroupSize[0] = VULKAN_COMPUTE_INVOCATIONS;
		limits->maxComputeWorkGroupSize[1] = VULKAN_COMPUTE_INVOCATIONS;
		limits->maxComputeWorkGroupSize[2] = 64;

		/* A 7.1 engine samples 3D images, no side of them longer than the most slices. */
		limits->maxImageDimension3D = BCM2711_VULKAN_IMAGE_DEPTH;
	}

	/* Succeeded: every reported optional/unsupported field remains zero in the command-local record. */
	return;
}

/*
 * Fills the optional features the device implements, the rest zero: the
 * depth bias clamp, lines wider than a pixel and points larger than one
 * (the bin's Depth Offset limit, Line Width and the shaded point size),
 * exact occlusion counts,
 * and on a 7.1 engine the vertex stage's storage buffer stores and atomics
 * (the general memory TMU of the compute lowering, run by the coordinate
 * and the vertex program) and independent blending of its four targets.
 */
void
bcm2711_vulkan_device_features(
	const struct bcm2711_vulkan_session *session,
	VkPhysicalDeviceFeatures *features)
{
	/* The features of a 4.2 and a 7.1 engine. */
	kern_memset(features, 0, sizeof(*features));
	features->depthBiasClamp = VK_TRUE;
	features->wideLines = VK_TRUE;
	features->largePoints = VK_TRUE;

	/* An occlusion query counts every sample that passes (the tile buffer's counter is exact), as Mesa's v3dv reports. */
	features->occlusionQueryPrecise = VK_TRUE;

	/*
	 * A 7.1 engine's vertex programs store into storage buffers, and its
	 * colour targets each blend and mask as their own attachment says (a
	 * Blend Cfg and four Color Write Masks bits a target).
	 */
	if (compute_engine(session)) {
		features->vertexPipelineStoresAndAtomics = VK_TRUE;
		features->independentBlend = VK_TRUE;
	}

	/* Succeeded: every other feature stays absent. */
	return;
}

/*
 * Defines the colour formats' sampling, attachment, blit and transfer
 * features, the tile buffer's depth/stencil formats and the vertex inputs
 * consumed by VPM.
 *
 * RGBA8 and BGRA8 are colour attachments, blit sources and destinations on
 * both engines.  On a 7.1 engine every colour format with an output format
 * is a colour attachment (blended when the tile buffer blends it), and,
 * unless of integer channels, a blit destination; every sampled colour
 * format not of integer channels is a blit source.  Every colour format is
 * sampled and copied as its texels' bytes.
 */
static void
format_properties(
	const struct bcm2711_vulkan_session *session,
	VkFormat format,
	VkFormatProperties *properties)
{
	const struct bcm2711_vulkan_colour_format *colour;
	struct bcm2711_vulkan_colour_target target;
	bool integer;
	int error;

	/* Unknown and compressed images stay unsupported until their complete native paths exist. */
	kern_memset(properties, 0, sizeof(*properties));
	if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM) {
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
						    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
						    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
						    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
		properties->linearTilingFeatures = properties->optimalTilingFeatures;
	} else {
		/* Another colour format is sampled, filtered linearly when it may be, and copied as its texels' bytes. */
		colour = bcm2711_vulkan_colour_format(format);
		if (colour != NULL) {
			properties->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
							    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
			if (colour->filter != 0)
				properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

			/* A 7.1 engine draws into it and blits from and into it as its channels allow. */
			error = bcm2711_vulkan_colour_target(format, &target);
			integer = false;
			if (error == 0)
				integer = target.integer;
			if (compute_engine(session) && error == 0) {
				properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
				if (target.blend)
					properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
				if (!integer)
					properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_BLIT_DST_BIT;
			}
			if (compute_engine(session) && !integer)
				properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_BLIT_SRC_BIT;
			properties->linearTilingFeatures = properties->optimalTilingFeatures;
		}
	}

	/*
	 * A 7.1 engine's compute programs read and write an optimally tiled
	 * image of a storage format (RGBA8 among them), and the 32-bit integer
	 * ones atomically too (the general memory TMU's operations on a word).
	 */
	if (compute_engine(session) && bcm2711_vulkan_storage_texel(format) != 0) {
		properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
		if (format == VK_FORMAT_R32_UINT || format == VK_FORMAT_R32_SINT)
			properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
	}

	/*
	 * Depth/stencil attachments live only in the tile buffer and their own
	 * raster image: a combined D24S8 and a depth-only X8_D24 of the tile
	 * buffer's 24-bit depth, a depth-only D16 of its 16-bit depth and a
	 * depth-only D32_SFLOAT of its 32-bit float depth, optimally tiled.
	 * Their depth is sampled (DEPTH24_X8, DEPTH_COMP16 and DEPTH_COMP32F
	 * textures), compared or not, without linear filtering, as Mesa's v3dv
	 * offers them.
	 * Values referenced: Mesa 25.0.7 src/broadcom/vulkan/v3dvx_formats.c
	 * (the depth formats' texture types and their filtering).
	 */
	if (format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_X8_D24_UNORM_PACK32 || format == VK_FORMAT_D16_UNORM ||
	    format == VK_FORMAT_D32_SFLOAT)
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

	/*
	 * A depth/stencil image is copied to and from buffers (a D16 or
	 * D32_SFLOAT image's depth as its texels' bytes, a D24 image's depth
	 * and stencil moved to and from their bits of its words) and to and
	 * from images of its format.
	 */
	if (bcm2711_vulkan_depth_format(format))
		properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	/* NV12 pictures (whose planes are viewed as R8 luma and RG8 chroma, colour formats of the table) are decoded into and read back by the CPU copy. */
	if (format == (VkFormat)BCM2711_VIDEO_FORMAT_NV12) {
		properties->optimalTilingFeatures = BCM2711_VIDEO_FORMAT_FEATURES | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
		properties->linearTilingFeatures = properties->optimalTilingFeatures;
	}
#endif

	/* The scalar frontend consumes bounded 32-bit float and integer attributes rather than foreign GPU vertex format encodings. */
	if (bcm2711_vulkan_attribute_components(format) != 0)
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

/* Tells whether the session's engine is a V3D 7.1, the version whose compiler builds compute programs. */
static bool
compute_engine(
	const struct bcm2711_vulkan_session *session)
{
	const struct bcm2711_v3d *engine;
	uint32_t layout;

	/* The hub's identification names the technology version in its low four bits. */
	engine = session->render->device->space.native;
	layout = engine->hardware.hub_ident[1];
	if ((layout & 15U) != 7U)
		return false;

	/* Succeeded: the engine is a 7.1. */
	return true;
}

/* The occlusion query pools. */
#include "vulkan-occlusion.inc"
