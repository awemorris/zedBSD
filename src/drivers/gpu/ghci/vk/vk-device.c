/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The instance, the physical device, the device and its queue.
 *
 * The node is one physical device whose answers come from the GPU's
 * capability block (vk-caps.c).  vkCreateDevice takes one of the
 * fifteen GHCI sessions of the hardware for the new VkDevice, and fails
 * with VK_ERROR_TOO_MANY_OBJECTS when they are all in use.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/*
 * The version on the wire (1.1, which libvulkan requires and shows
 * applications as 1.0) and the driver's version.
 */
#define DEVICE_API_VERSION		VK_MAKE_VERSION(1, 1, 0)
#define DEVICE_DRIVER_VERSION		1U

/* The one queue family. */
#define DEVICE_FAMILIES			1U

static int create_instance(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_instance(struct i915_wire_reader *reader);
static int enumerate(uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int properties(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int properties2(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int identity(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_writer *reply);
static void device_record(struct ghci_vk_session *session, VkPhysicalDeviceProperties *record);
static int features(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int features2(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void device_core11(struct ghci_vk_session *session, struct i915_vkc_core11 *core11);
static int memory_properties(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int families(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int format(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int image_format(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int image_format2(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int buffer_external(uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int sparse_formats(uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_device(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t device_admit(struct ghci_vk_session *session, const VkDeviceCreateInfo *info);
static int device_open(struct ghci_vk_session *session, uint64_t handle, const VkDeviceCreateInfo *info, uint32_t *result);
static int destroy_device(struct ghci_vk_session *session, struct i915_wire_reader *reader);
static int device_queue(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

/*
 * Runs an instance, physical-device, device or queue command; *handled
 * stays clear for another group's number.
 */
int
ghci_vk_device_route(
	struct ghci_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another group. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_INSTANCE:
		error = create_instance(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_INSTANCE:
		error = destroy_instance(reader);
		break;
	case GPU_OP_ENUMERATE_PHYSICAL_DEVICES:
		error = enumerate(requested, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FEATURES:
		error = features(session, requested, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FEATURES2:
		error = features2(session, requested, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES:
		error = format(session, requested, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES:
		error = image_format(session, requested, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES:
		error = properties(session, requested, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2:
		error = properties2(session, requested, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES:
		error = families(session, requested, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES:
		error = memory_properties(session, requested, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES2:
		error = image_format2(session, requested, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_EXTERNAL_BUFFER_PROPERTIES:
		error = buffer_external(requested, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_PROPERTIES:
		error = sparse_formats(requested, reader, reply);
		break;
	case GPU_OP_CREATE_DEVICE:
		error = create_device(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_DEVICE:
		error = destroy_device(session, reader);
		break;
	case GPU_OP_GET_DEVICE_QUEUE2:
		error = device_queue(session, requested, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded or run. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/* vkCreateInstance: [u64 1][create info][u64 0][u64 1][instance] -> [result][u64 1][instance]. */
static int
create_instance(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkInstanceCreateInfo info;
	uint64_t present;
	uint64_t instance;

	/* The create info when present; nothing of it is kept. */
	kern_memset(&info, 0, sizeof(info));
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkInstanceCreateInfo(reader, &session->arena, &info);
	instance = ghci_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The instance is the session's alone and keeps nothing. */
	ghci_vk_reply_created(reply, requested, (uint32_t)VK_SUCCESS, instance);

	/* Succeeded: the instance exists. */
	return 0;
}

/* vkDestroyInstance: [instance][u64 0], no reply body. */
static int
destroy_instance(
	struct i915_wire_reader *reader)
{
	/* The instance, then the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the instance is forgotten. */
	return 0;
}

/*
 * vkEnumeratePhysicalDevices: [instance][u64 1][u32 count][u64 count]
 * [ids] -> [result][u64 1][u32 count][u64 count][ids].  There is one
 * device; the library chooses its identity.
 */
static int
enumerate(
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t array_count;
	uint64_t physical;

	/* The instance, the count's presence word and the count, then the array. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	array_count = drv_i915_wire_read_u64(reader);
	physical = 0U;
	if (array_count == 1U)
		physical = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || array_count > 1U)
		return EINVAL;

	/* Success, one device, and its identity when the array holds it. */
	if (requested != 0U) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u32(reply, 1U);
		drv_i915_wire_reply_u64(reply, array_count);
		if (array_count != 0U)
			drv_i915_wire_reply_u64(reply, physical);
	}

	/* Succeeded: the device is reported. */
	return 0;
}

/* vkGetPhysicalDeviceProperties: [physical][u64 1] -> [u64 1][properties]. */
static int
properties(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceProperties *record;

	/* The physical device and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The device's record, in the scratch. */
	record = ghci_vk_arena_take(session, sizeof(*record));
	if (record == NULL)
		return ENOMEM;
	device_record(session, record);

	/* The presence word and the record. */
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		i915_vkc_enc_VkPhysicalDeviceProperties(reply, record);
	}

	/* Succeeded: the properties are reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceProperties2 in either of its forms: [physical]
 * [u64 1][PROPERTIES_2 sType][the chain's headers] -> the device's
 * identity (identity()) or its Vulkan 1.1 properties (the 1.1 fields,
 * then the properties).  Any other chain is refused.
 */
static int
properties2(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vkc_core11 core11;
	VkPhysicalDeviceProperties *record;
	int form;
	int error;

	/* The physical device, then the output chain, which says which form is asked for. */
	(void)drv_i915_wire_read_u64(reader);
	error = i915_vkc_read_properties2(reader, &form);
	if (error != 0)
		return error;

	/* The identity, which the export and import of memory name. */
	if (form == I915_VKC_PROPERTIES2_IDENTITY) {
		error = identity(session, requested, reply);
		if (error != 0)
			return error;

		/* Succeeded: the identity is reported. */
		return 0;
	}

	/* The Vulkan 1.1 properties, then the ordinary ones. */
	record = ghci_vk_arena_take(session, sizeof(*record));
	if (record == NULL)
		return ENOMEM;
	device_record(session, record);
	device_core11(session, &core11);
	if (requested != 0U) {
		i915_vkc_reply_core11_properties(reply, &core11);
		i915_vkc_enc_VkPhysicalDeviceProperties(reply, record);
	}

	/* Succeeded: the Vulkan 1.1 properties are reported. */
	return 0;
}

/*
 * The identity form of vkGetPhysicalDeviceProperties2 (the ID properties
 * alone chained, read by properties2()): [u64 1], the two headers, the
 * device and driver UUIDs, the LUID (none), the node mask, the LUID-valid
 * word and the properties.  Every session of the device reports the same
 * UUIDs.
 */
static int
identity(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_writer *reply)
{
	static const uint8_t driver_uuid[VK_UUID_SIZE] = {
		'z', 'e', 'd', 'B', 'S', 'D', '-', 'g', 'h', 'c', 'i', '-', 'v', 'k', '1', '0'
	};
	static const uint8_t luid[VK_LUID_SIZE] = { 0 };
	VkPhysicalDeviceProperties *record;
	struct ghci_device *device;
	uint8_t device_uuid[VK_UUID_SIZE];
	uint32_t ids;

	/* The ordinary properties, as the plain query gives them. */
	record = ghci_vk_arena_take(session, sizeof(*record));
	if (record == NULL)
		return ENOMEM;
	device_record(session, record);

	/* The device's UUID: the driver's name, the PCI IDs and the GHCI version. */
	device = session->device;
	kern_memset(device_uuid, 0, sizeof(device_uuid));
	kern_memcpy(device_uuid, "ghci", 4U);
	ids = ((uint32_t)device->pci_vendor << 16) | device->pci_device;
	kern_memcpy(device_uuid + 4U, &ids, 4U);
	kern_memcpy(device_uuid + 8U, &device->version, 4U);

	/* The two headers, the identity (no Windows adapter: no LUID), then the properties. */
	if (requested != 0U) {
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
		i915_vkc_enc_VkPhysicalDeviceProperties(reply, record);
	}

	/* Succeeded: the identity every session of the device reports. */
	return 0;
}

/*
 * Fills a cleared record with the device's properties: the version, the
 * PCI IDs, the kind of GPU, the node's name and the limits.
 */
static void
device_record(
	struct ghci_vk_session *session,
	VkPhysicalDeviceProperties *record)
{
	struct ghci_device *device;
	size_t length;
	int discrete;

	/* The version, the PCI IDs, and a discrete GPU when the driver hands out its local memory. */
	device = session->device;
	discrete = ghci_memory_local_bytes(device) != 0U;
	record->apiVersion = DEVICE_API_VERSION;
	record->driverVersion = DEVICE_DRIVER_VERSION;
	record->vendorID = device->pci_vendor;
	record->deviceID = device->pci_device;
	record->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
	if (discrete)
		record->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

	/* The name: the node's, "ghci:<vendor>:<device>". */
	length = kern_strlen(device->name);
	if (length >= VK_MAX_PHYSICAL_DEVICE_NAME_SIZE)
		length = VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1U;
	kern_memcpy(record->deviceName, device->name, length);

	/* The pipeline cache's UUID (the cache keeps nothing), and the limits. */
	kern_memcpy(record->pipelineCacheUUID, GHCI_VK_CACHE_UUID, VK_UUID_SIZE);
	ghci_vk_caps_limits(&device->caps, &record->limits);
}

/* vkGetPhysicalDeviceFeatures: [physical][u64 1] -> [u64 1][features]. */
static int
features(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceFeatures record;

	/* The physical device and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The presence word and the device's features. */
	ghci_vk_caps_features(&session->device->caps, &record);
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		i915_vkc_enc_VkPhysicalDeviceFeatures(reply, &record);
	}

	/* Succeeded: the features are reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceFeatures2 with the Vulkan 1.1 features chained:
 * [physical][output chain] -> [u64 1][the chain's headers][the 1.1
 * fields][features].
 */
static int
features2(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vkc_core11 core11;
	VkPhysicalDeviceFeatures record;
	int error;

	/* The physical device, then the output chain of the 1.1 feature structures. */
	(void)drv_i915_wire_read_u64(reader);
	error = i915_vkc_read_features2(reader);
	if (error != 0)
		return error;

	/* The 1.1 features, then the device's features. */
	ghci_vk_caps_features(&session->device->caps, &record);
	device_core11(session, &core11);
	if (requested != 0U) {
		i915_vkc_reply_core11_features(reply, &core11);
		i915_vkc_enc_VkPhysicalDeviceFeatures(reply, &record);
	}

	/* Succeeded: the features are reported. */
	return 0;
}

/*
 * Fills the device's Vulkan 1.1 answers: the subgroup of one invocation
 * every executor starts from (compute only); multiview, a render pass of
 * views drawn once per view into each attachment's layer of the view,
 * with gl_ViewIndex in the constant bank (vk-execute.c), instances
 * numbered as without multiview; a set layout holds 4096 descriptors
 * (vk-layout.c), of which 1024 are reported; the largest allocation is
 * the heap's.
 */
static void
device_core11(
	struct ghci_vk_session *session,
	struct i915_vkc_core11 *core11)
{
	/* What every executor starts from. */
	i915_vkc_core11_defaults(core11);

	/* Multiview. */
	core11->multiview.multiview = VK_TRUE;
	core11->multiview_limits.maxMultiviewViewCount = GHCI_VK_VIEWS_MAX;
	core11->multiview_limits.maxMultiviewInstanceIndex = (1U << 27) - 1U;

	/* The descriptors of one set and the largest allocation. */
	core11->maintenance3.maxPerSetDescriptors = 1024U;
	core11->maintenance3.maxMemoryAllocationSize = ghci_vk_caps_heap_bytes(&session->device->caps);
}

/* vkGetPhysicalDeviceMemoryProperties: [physical][u64 1][u64 32][u64 16] -> [u64 1][properties]. */
static int
memory_properties(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPhysicalDeviceMemoryProperties *record;
	uint64_t types;
	uint64_t heaps;

	/* The physical device, the presence word and the arrays' fixed extents. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	types = drv_i915_wire_read_u64(reader);
	heaps = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (types != VK_MAX_MEMORY_TYPES || heaps != VK_MAX_MEMORY_HEAPS)
		return EINVAL;

	/* The record, in the scratch. */
	record = ghci_vk_arena_take(session, sizeof(*record));
	if (record == NULL)
		return ENOMEM;
	ghci_vk_caps_memory(&session->device->caps, ghci_memory_local_bytes(session->device), record);

	/* The presence word and the record. */
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		i915_vkc_enc_VkPhysicalDeviceMemoryProperties(reply, record);
	}

	/* Succeeded: the memory is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceQueueFamilyProperties: [physical][u64 1][u32 count]
 * [u64 count] -> [u64 1][u32 count][u64 count][families].
 */
static int
families(
	struct ghci_vk_session *session,
	uint32_t requested,
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
	ghci_vk_caps_family(&session->device->caps, &family);
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u32(reply, DEVICE_FAMILIES);
		drv_i915_wire_reply_u64(reply, array_count);
		for (index = 0U; index < array_count; index++)
			i915_vkc_enc_VkQueueFamilyProperties(reply, &family);
	}

	/* Succeeded: the family is reported. */
	return 0;
}

/* vkGetPhysicalDeviceFormatProperties: [physical][u32 format][u64 1] -> [u64 1][properties]. */
static int
format(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFormatProperties record;
	struct ghci_vk *vk;
	uint32_t vk_format;

	/* The physical device, the format and the presence word. */
	(void)drv_i915_wire_read_u64(reader);
	vk_format = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The format's features on each tiling and in buffers. */
	vk = session->device->vk;
	ghci_vk_caps_format(&vk->formats, vk_format, &record);
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		i915_vkc_enc_VkFormatProperties(reply, &record);
	}

	/* Succeeded: the format is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceImageFormatProperties: [physical][u32 format]
 * [u32 type][u32 tiling][u32 usage][u32 flags][u64 1] -> [result][u64 1]
 * [properties].
 */
static int
image_format(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageFormatProperties record;
	struct ghci_vk *vk;
	uint32_t words[5];
	uint32_t result;
	unsigned index;

	/* The physical device, the format, type, tiling, usage and flags, and the presence word. */
	(void)drv_i915_wire_read_u64(reader);
	for (index = 0U; index < 5U; index++)
		words[index] = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* What such an image may be. */
	vk = session->device->vk;
	result = ghci_vk_caps_image_format(&session->device->caps, &vk->formats, words[0], words[1], words[2], words[3], words[4], &record);

	/* The result, the presence word and the limits. */
	if (requested != 0U) {
		drv_i915_wire_reply_u32(reply, result);
		drv_i915_wire_reply_u64(reply, 1U);
		i915_vkc_enc_VkImageFormatProperties(reply, &record);
	}

	/* Succeeded: the format is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceImageFormatProperties2 with an external memory
 * handle type (the chains as uapi/gpu-op.h lays them out) -> [result]
 * [u64 1], the two headers, the external memory properties (none: the
 * device shares memory only within itself) and the image's limits.
 */
static int
image_format2(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageFormatProperties record;
	struct ghci_vk *vk;
	uint32_t words[5];
	uint32_t result;
	unsigned index;

	/* The physical device and the input chain up to the image's description. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The image, then the output chain's headers. */
	for (index = 0U; index < 5U; index++)
		words[index] = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The image's limits. */
	vk = session->device->vk;
	result = ghci_vk_caps_image_format(&session->device->caps, &vk->formats, words[0], words[1], words[2], words[3], words[4], &record);

	/* The result, the two headers, no external memory feature, then the limits. */
	if (requested != 0U) {
		drv_i915_wire_reply_u32(reply, result);
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2);
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
		drv_i915_wire_reply_u64(reply, 0U);
		drv_i915_wire_reply_u32(reply, 0U);
		drv_i915_wire_reply_u32(reply, 0U);
		drv_i915_wire_reply_u32(reply, 0U);
		i915_vkc_enc_VkImageFormatProperties(reply, &record);
	}

	/* Succeeded: the format is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceExternalBufferProperties with the context's native
 * handle type (the chains as uapi/gpu-op.h lays them out) -> [u64 1],
 * the header and the external memory properties: none.
 */
static int
buffer_external(
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	/* The physical device and the two chains. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The presence word, the header and three empty words. */
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u32(reply, VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES);
		drv_i915_wire_reply_u64(reply, 0U);
		drv_i915_wire_reply_u32(reply, 0U);
		drv_i915_wire_reply_u32(reply, 0U);
		drv_i915_wire_reply_u32(reply, 0U);
	}

	/* Succeeded: no external memory is reported. */
	return 0;
}

/*
 * vkGetPhysicalDeviceSparseImageFormatProperties: [physical][u32 format]
 * [u32 type][u32 samples][u32 usage][u32 tiling][u64 1][u32 capacity]
 * [u64 capacity] -> [u64 1][u32 0][u64 0]: no sparse image format.
 */
static int
sparse_formats(
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	/* The physical device, the five words, the presence word and the capacities. */
	(void)drv_i915_wire_read_u64(reader);
	ghci_vk_skip(reader, 5U * 4U);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* None. */
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u32(reply, 0U);
		drv_i915_wire_reply_u64(reply, 0U);
	}

	/* Succeeded: the device has no sparse images. */
	return 0;
}

/*
 * vkCreateDevice: [physical][u64 1][create info][u64 0][u64 1][device] ->
 * [result][u64 1][device].  One queue of the one family, only features
 * the device offers, and a GHCI session of the device's own.
 */
static int
create_device(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDeviceCreateInfo info;
	uint64_t present;
	uint64_t handle;
	uint32_t result;
	int error;

	/* The physical device, then the create info. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkDeviceCreateInfo(reader, &session->arena, &info);
	handle = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;

	/* The queues and features asked for, then the device's GHCI session. */
	result = device_admit(session, &info);
	if (result == (uint32_t)VK_SUCCESS) {
		error = device_open(session, handle, &info, &result);
		if (error != 0)
			return error;
	}

	/* The result, and the identity when it succeeded. */
	ghci_vk_reply_created(reply, requested, result, handle);

	/* Succeeded: the device exists, or the reply says why not. */
	return 0;
}

/*
 * Reports whether a device may be made as asked: VK_SUCCESS, or
 * VK_ERROR_INITIALIZATION_FAILED for queues the device does not have, or
 * VK_ERROR_FEATURE_NOT_PRESENT for a feature it does not offer.
 */
static uint32_t
device_admit(
	struct ghci_vk_session *session,
	const VkDeviceCreateInfo *info)
{
	struct i915_vkc_core11 core11;
	uint64_t decoded;
	uint32_t index;
	int within;

	/* One queue of the one family (the array as long as its count). */
	decoded = ghci_vk_decoded_count(session, info->pQueueCreateInfos);
	if (decoded != info->queueCreateInfoCount)
		return (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
	for (index = 0U; index < info->queueCreateInfoCount; index++) {
		if (info->pQueueCreateInfos[index].queueFamilyIndex >= DEVICE_FAMILIES)
			return (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
		if (info->pQueueCreateInfos[index].queueCount != 1U)
			return (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Only the features the device offers, Vulkan 1.1's too. */
	if (info->pEnabledFeatures != NULL) {
		within = ghci_vk_caps_features_within(&session->device->caps, info->pEnabledFeatures);
		if (!within)
			return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;
	}
	device_core11(session, &core11);
	if (i915_vkc_core11_check_device(info, &core11) != 0)
		return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;

	/* Succeeded: the device may be made. */
	return (uint32_t)VK_SUCCESS;
}

/*
 * Makes the device's record and object and opens its GHCI session.  The
 * result says why a device could not be made: all fifteen sessions in
 * use (VK_ERROR_TOO_MANY_OBJECTS), no room in CP_HEAP's bound
 * (VK_ERROR_OUT_OF_DEVICE_MEMORY), no host memory for the session's
 * record (VK_ERROR_OUT_OF_HOST_MEMORY), or a lost GPU
 * (VK_ERROR_INITIALIZATION_FAILED).  Returns 0, or an
 * error that ends the frame (no host memory for the device's own record,
 * a reused identity).
 */
static int
device_open(
	struct ghci_vk_session *session,
	uint64_t handle,
	const VkDeviceCreateInfo *info,
	uint32_t *result)
{
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	struct ghci_vm *vm;
	unsigned long irq;
	int error;

	/* The record. */
	device = kern_calloc(1U, sizeof(*device));
	if (device == NULL)
		return ENOMEM;
	device->session = session;
	if (info->pEnabledFeatures != NULL && info->pEnabledFeatures->robustBufferAccess != VK_FALSE)
		device->robust = 1;

	/* The object, before the session, so that nothing is left to undo on the GPU. */
	error = ghci_vk_object_add(session, GHCI_VK_DEVICE, handle, device, &object);
	if (error != 0) {
		kern_free(device);
		return error;
	}

	/* One of the hardware's fifteen sessions; a refusal is the reply's. */
	error = ghci_vm_open(session->device, session->owner, &vm);
	if (error != 0) {
		object->device = NULL;
		ghci_vk_object_destroy(session, object);
		kern_free(device);
		*result = ghci_vk_result(error);
		return 0;
	}
	device->vm = vm;

	/* The device joins the session's, which the job ioctls and the recovery read. */
	irq = spin_lock_irqsave(&session->device->irq_lock);

	device->next = session->devices;
	session->devices = device;

	spin_unlock_irqrestore(&session->device->irq_lock, irq);

	/* Succeeded: the device runs in its GHCI session. */
	*result = (uint32_t)VK_SUCCESS;
	return 0;
}

/* vkDestroyDevice: [device][u64 0], no reply body; the device's objects and GHCI session go with it. */
static int
destroy_device(
	struct ghci_vk_session *session,
	struct i915_wire_reader *reader)
{
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t allocator;

	/* The device, then the allocator. */
	handle = drv_i915_wire_read_u64(reader);
	allocator = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0U)
		return EINVAL;

	/* The device, when the session has it. */
	object = ghci_vk_object_find(session, GHCI_VK_DEVICE, handle);
	if (object != NULL)
		ghci_vk_object_destroy(session, object);

	/* Succeeded: the device is destroyed. */
	return 0;
}

/*
 * vkGetDeviceQueue2 as libvulkan sends it: [device][u64 1][sType][u64 1]
 * [timeline sType][u64 0][u32 timeline][u32 flags][u32 family][u32 index]
 * [u64 1][queue] -> [u64 1][queue].  The queue's jobs complete on the
 * timeline (1 to 63) from now on.
 */
static int
device_queue(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *device;
	struct ghci_vk_object *queue;
	uint64_t handle;
	uint64_t identity;
	unsigned long irq;
	uint32_t record;
	uint32_t timeline;
	uint32_t family;
	uint32_t index;
	int error;

	/* The device, the two headers, the timeline, the flags, the family, the index and the queue. */
	handle = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	record = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	timeline = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	family = drv_i915_wire_read_u32(reader);
	index = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || record != GPU_OP_RECORD_QUEUE_TIMELINE)
		return EINVAL;

	/* The one queue of the one family of a device of the session, on a timeline of 1 to 63. */
	device = ghci_vk_object_find(session, GHCI_VK_DEVICE, handle);
	if (device == NULL || family >= DEVICE_FAMILIES || index != 0U)
		return EINVAL;
	if (timeline == 0U || timeline >= GHCI_VK_TIMELINES)
		return EINVAL;

	/* The queue's object, once. */
	queue = ghci_vk_object_find(session, GHCI_VK_QUEUE, identity);
	if (queue == NULL) {
		error = ghci_vk_object_add(session, GHCI_VK_QUEUE, identity, device->device, &queue);
		if (error != 0)
			return error;
	}
	queue->as.queue.timeline = timeline;
	queue->as.queue.family = family;

	/* The timeline's jobs follow the device's work. */
	irq = spin_lock_irqsave(&session->device->irq_lock);

	session->timeline_device[timeline] = device->device;

	spin_unlock_irqrestore(&session->device->irq_lock, irq);

	/* The presence word and the identity. */
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, identity);
	}

	/* Succeeded: the queue is known. */
	return 0;
}
