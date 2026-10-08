/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Queries standard external-memory profiles while keeping renderer handles private.
 */

#include <string.h>
#include "internal.h"
#include <uapi/gpu-allocation.h>

static void external_header(struct vulkan_reader *reader, VkStructureType type, uint64_t next);
static void external_decode_memory(struct vulkan_reader *reader, VkExternalMemoryProperties *properties);
static void external_memory_profile(VkExternalMemoryProperties *properties, uint32_t native_type);
static uint32_t external_native_type(const struct vulkan_context *context);
static VkResult external_buffer_properties(struct VkPhysicalDevice_T *physical, const VkPhysicalDeviceExternalBufferInfo *info, VkExternalMemoryProperties *properties);
static VkResult external_image_properties(struct VkPhysicalDevice_T *physical, const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties *properties, VkExternalMemoryProperties *external);
static void external_id_array(struct vulkan_reader *reader, void *bytes, uint32_t count);
static VkResult external_video_image_properties(struct VkPhysicalDevice_T *physical, const VkPhysicalDeviceImageFormatInfo2 *info, const VkVideoProfileListInfoKHR *profiles, const VkPhysicalDeviceExternalImageFormatInfo *external, VkImageFormatProperties *properties);
static void external_queue_family_chain(struct VkPhysicalDevice_T *physical, uint32_t family, VkBaseOutStructure *chain);

/* Returns the same supported core feature snapshot through its standard extensible form. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures2KHR(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceFeatures2 *pFeatures)
{
	struct VkPhysicalDevice_T *physical;
	VkBaseOutStructure *next;
	VkPhysicalDeviceSynchronization2Features *synchronization;

	/* Unknown output extensions are left untouched, as in the standard chain contract. */
	physical = vulkan_physical_device(physicalDevice);
	vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);

	/* Answers the synchronization2 feature wherever the caller chained it. */
	for (next = pFeatures->pNext; next != NULL; next = next->pNext) {
		/* Only the synchronization2 feature record has a local answer. */
		if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES)
			continue;

		/* The translated commands are available exactly when the extension is named. */
		synchronization = (VkPhysicalDeviceSynchronization2Features *)next;
		synchronization->synchronization2 = VK_FALSE;
		if (physical->supported_extensions & VULKAN_DEVICE_SYNCHRONIZATION2)
			synchronization->synchronization2 = VK_TRUE;
	}

	/* Succeeded: the wrapper preserves caller-owned chain links. */
	return;
}

/* Supplies stable external-handle UUIDs without exposing the renderer's fd namespace. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2KHR(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceProperties2 *pProperties)
{
	struct VkPhysicalDevice_T *physical;
	VkBaseOutStructure *next;
	VkPhysicalDeviceIDProperties identity;
	VkPhysicalDeviceIDProperties *output;
	VkResult status;

	/* Ordinary public properties retain the implemented guest API version and limits. */
	physical = vulkan_physical_device(physicalDevice);
	pProperties->properties = physical->properties;

	/* Fills recognized identity records while preserving the application chain. */
	for (next = pProperties->pNext; next != NULL; next = next->pNext) {
		/* Only the enabled external-capability output has local interpretation. */
		if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES)
			continue;

		/* A failed native query never supplies a made-up compatible UUID. */
		memset(&identity, 0, sizeof(identity));
		status = vulkan_physical_identity(physical, &identity);
		if (status != VK_SUCCESS)
			memset(&identity, 0, sizeof(identity));

		/* Copies only result fields, preserving the application's header and chain. */
		output = (VkPhysicalDeviceIDProperties *)next;
		memcpy(output->deviceUUID, identity.deviceUUID, VK_UUID_SIZE);
		memcpy(output->driverUUID, identity.driverUUID, VK_UUID_SIZE);
		memcpy(output->deviceLUID, identity.deviceLUID, VK_LUID_SIZE);
		output->deviceNodeMask = identity.deviceNodeMask;
		output->deviceLUIDValid = identity.deviceLUIDValid;
	}

	/* Succeeded: compatible guest contexts observe the same implementation identity. */
	return;
}

/* Preserves the native format feature snapshot in the properties2 ABI. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties2KHR(
	VkPhysicalDevice physicalDevice,
	VkFormat format,
	VkFormatProperties2 *pFormatProperties)
{
	/* The core query already applies all guest feature filtering. */
	vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &pFormatProperties->formatProperties);

	/* Succeeded: the caller owns the unchanged output chain. */
	return;
}

/* Queries external optimal or linear images using the renderer's actual format rules. */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties2KHR(
	VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
	VkImageFormatProperties2 *pImageFormatProperties)
{
	struct VkPhysicalDevice_T *physical;
	const VkBaseInStructure *input;
	VkBaseOutStructure *output;
	const VkPhysicalDeviceExternalImageFormatInfo *external;
	const VkVideoProfileListInfoKHR *profiles;
	VkExternalImageFormatProperties *properties;
	VkExternalMemoryProperties memory;
	VkResult status;

	/* Finds the requested external type and video profiles without depending on pNext order. */
	physical = vulkan_physical_device(physicalDevice);
	external = NULL;
	profiles = NULL;
	for (input = pImageFormatInfo->pNext; input != NULL; input = input->pNext) {
		/* Names the external memory type the image would be shared with. */
		if (input->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO)
			external = (const VkPhysicalDeviceExternalImageFormatInfo *)input;

		/* Names the video profiles a decode picture would be used with. */
		if (input->sType == VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR)
			profiles = (const VkVideoProfileListInfoKHR *)input;
	}

	/* A missing or zero external type has exactly the ordinary core query semantics. */
	memset(&memory, 0, sizeof(memory));
	memset(&pImageFormatProperties->imageFormatProperties, 0, sizeof(pImageFormatProperties->imageFormatProperties));
	if (profiles != NULL) {
		/* A video picture is checked here, then its limits come from the renderer. */
		status = external_video_image_properties(physical, pImageFormatInfo, profiles, external, &pImageFormatProperties->imageFormatProperties);
	} else if (external == NULL || external->handleType == 0) {
		status = vkGetPhysicalDeviceImageFormatProperties(
			physicalDevice,
			pImageFormatInfo->format,
			pImageFormatInfo->type,
			pImageFormatInfo->tiling,
			pImageFormatInfo->usage,
			pImageFormatInfo->flags,
			&pImageFormatProperties->imageFormatProperties);
	} else if (external->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
	           !(physical->object.context->capabilities & GPU_CAP_ALLOCATION_SHARE)) {
		/* The guest never promises Linux dma-buf or an unavailable kernel share primitive. */
		status = VK_ERROR_FORMAT_NOT_SUPPORTED;
	} else {
		/* Native allocation restrictions remain authoritative after handle-type translation. */
		status = external_image_properties(physical, pImageFormatInfo, &pImageFormatProperties->imageFormatProperties, &memory);
	}

	/* Writes only the recognized external output while preserving every header. */
	for (output = pImageFormatProperties->pNext; output != NULL; output = output->pNext) {
		if (output->sType != VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES)
			continue;

		/* The caller retains the header while this adapter supplies its payload. */
		properties = (VkExternalImageFormatProperties *)output;
		properties->externalMemoryProperties = memory;
	}

	/* Returns a native refusal without publishing a supported profile. */
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the profile describes an actually supported native allocation. */
	return VK_SUCCESS;
}

/* Returns cached queue families without replacing caller-owned extensible headers. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties2KHR(
	VkPhysicalDevice physicalDevice,
	uint32_t *pQueueFamilyPropertyCount,
	VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
	struct VkPhysicalDevice_T *physical;
	uint32_t count;
	uint32_t index;

	/* A null array is the standard count query. */
	physical = vulkan_physical_device(physicalDevice);
	if (pQueueFamilyProperties == NULL) {
		*pQueueFamilyPropertyCount = physical->queue_family_count;
		return;
	}

	/* Copies the caller's bounded prefix, retaining each pNext chain. */
	count = *pQueueFamilyPropertyCount;
	if (count > physical->queue_family_count)
		count = physical->queue_family_count;

	/* Initializes only the number of output entries the caller can hold. */
	for (index = 0; index < count; index++) {
		/* Copies the core record, then answers the video records the caller chained. */
		pQueueFamilyProperties[index].queueFamilyProperties = physical->queue_families[index];
		external_queue_family_chain(physical, index, pQueueFamilyProperties[index].pNext);
	}

	/* The returned count identifies every initialized output entry. */
	*pQueueFamilyPropertyCount = count;

	/* Succeeded: the returned count is the number of initialized array entries. */
	return;
}

/* Preserves the filtered guest memory types through the properties2 ABI. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties2KHR(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
	/* Native memory indices and mapped-access guarantees already live in the core snapshot. */
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &pMemoryProperties->memoryProperties);

	/* Succeeded: no unimplemented native memory type is exposed. */
	return;
}

/* Adapts the complete core sparse-format query without imposing a fixed array limit. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceSparseImageFormatProperties2KHR(
	VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
	uint32_t *pPropertyCount,
	VkSparseImageFormatProperties2 *pProperties)
{
	struct VkPhysicalDevice_T *physical;
	VkSparseImageFormatProperties *properties;
	uint32_t count;
	uint32_t index;
	size_t bytes;

	/* A count query needs no private storage. */
	if (pProperties == NULL) {
		vkGetPhysicalDeviceSparseImageFormatProperties(
			physicalDevice,
			pFormatInfo->format,
			pFormatInfo->type,
			pFormatInfo->samples,
			pFormatInfo->usage,
			pFormatInfo->tiling,
			pPropertyCount,
			NULL);
		return;
	}

	/* Bounds the temporary to the actual native count and caller capacity. */
	physical = vulkan_physical_device(physicalDevice);
	count = 0;
	vkGetPhysicalDeviceSparseImageFormatProperties(
		physicalDevice,
		pFormatInfo->format,
		pFormatInfo->type,
		pFormatInfo->samples,
		pFormatInfo->usage,
		pFormatInfo->tiling,
		&count,
		NULL);
	if (count > *pPropertyCount)
		count = *pPropertyCount;

	/* No array storage is needed for an empty supported profile. */
	if (count == 0) {
		*pPropertyCount = 0;
		return;
	}

	/* Uses the instance allocation policy for this transient query output. */
	bytes = (size_t)count * sizeof(*properties);
	if (bytes / sizeof(*properties) != count) {
		*pPropertyCount = 0;
		return;
	}

	/* Reserve the entire bounded temporary before issuing the native query. */
	properties = vulkan_allocate(
		&physical->object.allocator,
		bytes,
		__alignof__(VkSparseImageFormatProperties),
		VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (properties == NULL) {
		*pPropertyCount = 0;
		return;
	}

	/* Copies values before releasing the independent core-shaped array. */
	vkGetPhysicalDeviceSparseImageFormatProperties(
		physicalDevice,
		pFormatInfo->format,
		pFormatInfo->type,
		pFormatInfo->samples,
		pFormatInfo->usage,
		pFormatInfo->tiling,
		&count,
		properties);

	/* Preserves each caller header while copying the complete core result. */
	for (index = 0; index < count; index++)
		pProperties[index].properties = properties[index];

	/* Release temporary storage after all public values have been copied. */
	*pPropertyCount = count;
	vulkan_free(&physical->object.allocator, properties);

	/* Succeeded: caller-owned sparse result headers remain untouched. */
	return;
}

/* Reports import/export support for a particular external buffer usage. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceExternalBufferPropertiesKHR(
	VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
	VkExternalBufferProperties *pExternalBufferProperties)
{
	struct VkPhysicalDevice_T *physical;
	VkExternalMemoryProperties properties;
	VkResult status;

	/* Unknown fd types and unavailable kernel allocation sharing have an empty profile. */
	physical = vulkan_physical_device(physicalDevice);
	memset(&properties, 0, sizeof(properties));
	if (pExternalBufferInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT && (physical->object.context->capabilities & GPU_CAP_ALLOCATION_SHARE)) {
		status = external_buffer_properties(physical, pExternalBufferInfo, &properties);
		if (status != VK_SUCCESS)
			memset(&properties, 0, sizeof(properties));
	}

	/* The public result mentions only guest OPAQUE_FD, never the renderer's dma-buf. */
	pExternalBufferProperties->externalMemoryProperties = properties;

	/* Succeeded: unsupported profiles remain explicitly unavailable. */
	return;
}

/* Resolves a stable guest driver/device identity from the native capability query. */
VkResult
vulkan_physical_identity(
	struct VkPhysicalDevice_T *physical,
	VkPhysicalDeviceIDProperties *identity)
{
	static const uint8_t driver_namespace[VK_UUID_SIZE] = {
		0x7a, 0x65, 0x64, 0x42, 0x53, 0x44, 0x2d, 0x56,
		0x65, 0x6e, 0x75, 0x73, 0x2d, 0x30, 0x30, 0x31
	};
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkPhysicalDeviceProperties properties;
	VkResult status;
	VkBool32 present;
	uint32_t index;

	/* Only the output chain shape is serialized in this partial native query. */
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u64(&writer, 1U);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
	vulkan_write_u64(&writer, 1U);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
	vulkan_write_u64(&writer, 0U);
	status = vulkan_command_execute(physical->object.context, &writer, 4096U, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);

	/* Validates every framing field before trusting UUID bytes from this renderer. */
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (!present)
			reader.error = VK_ERROR_DEVICE_LOST;

		/* Validates the fixed output chain before consuming any property payload. */
		external_header(&reader, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, 1U);
		external_header(&reader, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, 0U);
		external_id_array(&reader, identity->deviceUUID, VK_UUID_SIZE);
		external_id_array(&reader, identity->driverUUID, VK_UUID_SIZE);
		external_id_array(&reader, identity->deviceLUID, VK_LUID_SIZE);
		identity->deviceNodeMask = vulkan_read_u32(&reader);
		identity->deviceLUIDValid = vulkan_read_u32(&reader);
		vulkan_decode_VkPhysicalDeviceProperties(&reader, &properties);
	}

	/* Retires the complete reply and keeps malformed framing terminal. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Distinguishes this guest implementation's payload from host Vulkan OPAQUE_FD. */
	for (index = 0; index < VK_UUID_SIZE; index++)
		identity->driverUUID[index] ^= driver_namespace[index];

	/* The guest implementation has no matching Windows adapter identity. */
	memset(identity->deviceLUID, 0, VK_LUID_SIZE);
	identity->deviceNodeMask = 0U;
	identity->deviceLUIDValid = VK_FALSE;

	/* Succeeded: independent guest contexts can verify the same stable compatibility pair. */
	return VK_SUCCESS;
}

/* Checks the exact returned structure header without accepting an unknown payload shape. */
static void
external_header(
	struct vulkan_reader *reader,
	VkStructureType type,
	uint64_t next)
{
	uint32_t actual_type;
	uint64_t actual_next;

	/* The independent decoder consumes the protocol's scalar header fields in order. */
	actual_type = vulkan_read_u32(reader);
	actual_next = vulkan_read_u64(reader);
	if (actual_type != (uint32_t)type || actual_next != next)
		reader->error = VK_ERROR_DEVICE_LOST;

	/* Succeeded: the reader retains any mismatch as a terminal framing error. */
	return;
}

/* Reads the three fixed registry fields that describe external allocation compatibility. */
static void
external_decode_memory(
	struct vulkan_reader *reader,
	VkExternalMemoryProperties *properties)
{
	/* No native structure padding is part of the wire representation. */
	properties->externalMemoryFeatures = vulkan_read_u32(reader);
	properties->exportFromImportedHandleTypes = vulkan_read_u32(reader);
	properties->compatibleHandleTypes = vulkan_read_u32(reader);

	/* Succeeded: translation can apply the guest handle-type contract. */
	return;
}

/* Maps only supported nondedicated native profiles to the guest allocation primitive. */
static void
external_memory_profile(
	VkExternalMemoryProperties *properties,
	uint32_t native_type)
{
	/* Dedicated-only resources require a separate implemented allocation contract. */
	if ((properties->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ||
	    !(properties->compatibleHandleTypes & native_type)) {
		memset(properties, 0, sizeof(*properties));
		return;
	}

	/* A guest export retains the same allocation; it does not produce a Linux dma-buf. */
	properties->externalMemoryFeatures &= VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
	if (properties->exportFromImportedHandleTypes & native_type)
		properties->exportFromImportedHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	else
		properties->exportFromImportedHandleTypes = 0U;

	/* Only the guest reference-bearing handle type is interoperable with this payload. */
	properties->compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

	/* Succeeded: the profile contains only implemented guest handle types. */
	return;
}

/* Executes the renderer buffer capability query with a private native handle type. */
static VkResult
external_buffer_properties(
	struct VkPhysicalDevice_T *physical,
	const VkPhysicalDeviceExternalBufferInfo *info,
	VkExternalMemoryProperties *properties)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;
	VkBool32 present;
	uint32_t native_type;

	/* Encodes the requested flags and usage exactly, translating only the handle type. */
	native_type = external_native_type(physical->object.context);
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_EXTERNAL_BUFFER_PROPERTIES);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u64(&writer, 1U);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO);
	vulkan_write_u64(&writer, 0U);
	vulkan_write_u32(&writer, info->flags);
	vulkan_write_u32(&writer, info->usage);
	vulkan_write_u32(&writer, native_type);
	vulkan_write_u64(&writer, 1U);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES);
	vulkan_write_u64(&writer, 0U);
	status = vulkan_command_execute(physical->object.context, &writer, 128U, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);

	/* A void native query still requires a complete typed reply before publication. */
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (!present)
			reader.error = VK_ERROR_DEVICE_LOST;

		/* Validates the fixed output chain before consuming any property payload. */
		external_header(&reader, VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES, 0U);
		external_decode_memory(&reader, properties);
	}

	/* Retires the complete reply and keeps malformed framing terminal. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Dedicated-only or unsupported native storage is not silently generalized. */
	external_memory_profile(properties, native_type);

	/* Succeeded: the result reflects this exact native buffer usage profile. */
	return VK_SUCCESS;
}

/* Executes an external image-format query including both extensible chain shapes. */
static VkResult
external_image_properties(
	struct VkPhysicalDevice_T *physical,
	const VkPhysicalDeviceImageFormatInfo2 *info,
	VkImageFormatProperties *properties,
	VkExternalMemoryProperties *external)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;
	VkBool32 present;
	uint32_t native_type;

	/* Input geometry and usage remain application-selected native Vulkan parameters. */
	native_type = external_native_type(physical->object.context);
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES2);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u64(&writer, 1U);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2);
	vulkan_write_u64(&writer, 1U);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
	vulkan_write_u64(&writer, 0U);
	vulkan_write_u32(&writer, native_type);
	vulkan_write_u32(&writer, info->format);
	vulkan_write_u32(&writer, info->type);
	vulkan_write_u32(&writer, info->tiling);
	vulkan_write_u32(&writer, info->usage);
	vulkan_write_u32(&writer, info->flags);
	vulkan_write_u64(&writer, 1U);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2);
	vulkan_write_u64(&writer, 1U);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
	vulkan_write_u64(&writer, 0U);
	status = vulkan_command_execute(physical->object.context, &writer, 256U, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);

	/* Native failure replies retain the same output structure framing. */
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (!present)
			reader.error = VK_ERROR_DEVICE_LOST;

		/* Validates the fixed output chain before consuming any property payload. */
		external_header(&reader, VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, 1U);
		external_header(&reader, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES, 0U);
		external_decode_memory(&reader, external);
		vulkan_decode_VkImageFormatProperties(&reader, properties);
	}

	/* Retires the complete reply and keeps malformed framing terminal. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS) {
		memset(external, 0, sizeof(*external));
		memset(properties, 0, sizeof(*properties));
		return status;
	}

	/* Rejects native dedicated-only profiles until that allocation extension is implemented. */
	external_memory_profile(external, native_type);
	if (external->externalMemoryFeatures == 0) {
		memset(properties, 0, sizeof(*properties));
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* Succeeded: optimal images retain their native geometry and allocation rules. */
	return VK_SUCCESS;
}

/* Verifies fixed-size UUID array framing before copying any identity bytes. */
static void
external_id_array(
	struct vulkan_reader *reader,
	void *bytes,
	uint32_t count)
{
	uint64_t actual;

	/* A different wire array length is a malformed reply, not another UUID version. */
	actual = vulkan_read_u64(reader);
	if (actual != count) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Copies exactly the registry-sized identity into the caller-owned output. */
	vulkan_read_bytes(reader, bytes, count);

	/* Succeeded: the exact fixed-width registry identity has been decoded. */
	return;
}

/* Selects native opaque sharing only after both renderer endpoints advertise that contract. */
static uint32_t
external_native_type(
	const struct vulkan_context *context)
{
	/* The vendor capability permits native OPAQUE resources across independent contexts. */
	if (context->external_memory_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
		return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

	/* Succeeded: stock renderers retain their supported DMA-backed subset. */
	return VULKAN_EXTERNAL_MEMORY_DMABUF;
}

/* Checks a video decode picture locally and asks the renderer for its limits. */
static VkResult
external_video_image_properties(
	struct VkPhysicalDevice_T *physical,
	const VkPhysicalDeviceImageFormatInfo2 *info,
	const VkVideoProfileListInfoKHR *profiles,
	const VkPhysicalDeviceExternalImageFormatInfo *external,
	VkImageFormatProperties *properties)
{
	VkImageUsageFlags video_usage;
	VkResult status;

	/* Every listed profile must be the H.264 decode profile this library implements. */
	status = vulkan_video_profile_list_check(physical, profiles);
	if (status != VK_SUCCESS)
		return status;

	/*
	 * A decode picture is only ever a decode output or a reference picture
	 * here; sampling or copying it is not offered.
	 */
	video_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
	if ((info->usage & ~video_usage) != 0)
		return VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR;

	/* The decode picture is a single-layer two-plane 4:2:0 image in the decoder's tiling. */
	if (info->format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	if (info->type != VK_IMAGE_TYPE_2D)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	if (info->tiling != VK_IMAGE_TILING_OPTIMAL)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* A decode picture is never shared with another process. */
	if (external != NULL && external->handleType != 0)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Asks the renderer, which knows the decoder's largest picture, for the limits. */
	status = vkGetPhysicalDeviceImageFormatProperties(
		physical,
		info->format,
		info->type,
		info->tiling,
		info->usage,
		info->flags,
		properties);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the picture can be created with the returned limits. */
	return VK_SUCCESS;
}

/* Fills the video records of one queue family's extensible output chain. */
static void
external_queue_family_chain(
	struct VkPhysicalDevice_T *physical,
	uint32_t family,
	VkBaseOutStructure *chain)
{
	VkBaseOutStructure *next;
	VkQueueFamilyVideoPropertiesKHR *video;
	VkQueueFamilyQueryResultStatusPropertiesKHR *status;

	/* Visits every record the caller chained, in any order. */
	for (next = chain; next != NULL; next = next->pNext) {
		/* The codec operations of the family, asked of the renderer once. */
		if (next->sType == VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR) {
			video = (VkQueueFamilyVideoPropertiesKHR *)next;
			video->videoCodecOperations = physical->queue_video_operations[family];
		}

		/* A family that decodes video reports its operations' result status (ws083-p008); the others do not. */
		if (next->sType == VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR) {
			status = (VkQueueFamilyQueryResultStatusPropertiesKHR *)next;
			status->queryResultStatusSupport = VK_FALSE;
			if (physical->queue_video_operations[family] != 0)
				status->queryResultStatusSupport = VK_TRUE;
		}
	}

	/* Succeeded: the caller's chain holds the family's video answers. */
	return;
}
