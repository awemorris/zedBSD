/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Maintained by tools/maintain-codec.noct from declarations and explicit protocol rules.
 */

#include "internal.h"

/*
 * Encodes VkExtent2D independently of native structure padding.
 */
void
vulkan_encode_VkExtent2D(
	struct vulkan_writer *writer,
	const VkExtent2D *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->width);
	vulkan_write_u32(writer, record->height);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkExtent2D independently of native structure padding.
 */
void
vulkan_decode_VkExtent2D(
	struct vulkan_reader *reader,
	VkExtent2D *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->width = (uint32_t)vulkan_read_u32(reader);
	record->height = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkExtent3D independently of native structure padding.
 */
void
vulkan_encode_VkExtent3D(
	struct vulkan_writer *writer,
	const VkExtent3D *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->width);
	vulkan_write_u32(writer, record->height);
	vulkan_write_u32(writer, record->depth);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkExtent3D independently of native structure padding.
 */
void
vulkan_decode_VkExtent3D(
	struct vulkan_reader *reader,
	VkExtent3D *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->width = (uint32_t)vulkan_read_u32(reader);
	record->height = (uint32_t)vulkan_read_u32(reader);
	record->depth = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkOffset2D independently of native structure padding.
 */
void
vulkan_encode_VkOffset2D(
	struct vulkan_writer *writer,
	const VkOffset2D *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->x);
	vulkan_write_u32(writer, record->y);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkOffset2D independently of native structure padding.
 */
void
vulkan_decode_VkOffset2D(
	struct vulkan_reader *reader,
	VkOffset2D *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->x = (int32_t)vulkan_read_u32(reader);
	record->y = (int32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkOffset3D independently of native structure padding.
 */
void
vulkan_encode_VkOffset3D(
	struct vulkan_writer *writer,
	const VkOffset3D *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->x);
	vulkan_write_u32(writer, record->y);
	vulkan_write_u32(writer, record->z);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkOffset3D independently of native structure padding.
 */
void
vulkan_decode_VkOffset3D(
	struct vulkan_reader *reader,
	VkOffset3D *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->x = (int32_t)vulkan_read_u32(reader);
	record->y = (int32_t)vulkan_read_u32(reader);
	record->z = (int32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkRect2D independently of native structure padding.
 */
void
vulkan_encode_VkRect2D(
	struct vulkan_writer *writer,
	const VkRect2D *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_VkOffset2D(writer, &record->offset);
	vulkan_encode_VkExtent2D(writer, &record->extent);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkRect2D independently of native structure padding.
 */
void
vulkan_decode_VkRect2D(
	struct vulkan_reader *reader,
	VkRect2D *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_decode_VkOffset2D(reader, &record->offset);
	vulkan_decode_VkExtent2D(reader, &record->extent);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkBufferMemoryBarrier independently of native structure padding.
 */
void
vulkan_encode_VkBufferMemoryBarrier(
	struct vulkan_writer *writer,
	const VkBufferMemoryBarrier *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->srcAccessMask);
	vulkan_write_u32(writer, record->dstAccessMask);
	vulkan_write_u32(writer, record->srcQueueFamilyIndex);
	vulkan_write_u32(writer, record->dstQueueFamilyIndex);
	vulkan_encode_handle(writer, (uint64_t)record->buffer);
	vulkan_write_u64(writer, record->offset);
	vulkan_write_u64(writer, record->size);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkDispatchIndirectCommand independently of native structure padding.
 */
void
vulkan_encode_VkDispatchIndirectCommand(
	struct vulkan_writer *writer,
	const VkDispatchIndirectCommand *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->x);
	vulkan_write_u32(writer, record->y);
	vulkan_write_u32(writer, record->z);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkDispatchIndirectCommand independently of native structure padding.
 */
void
vulkan_decode_VkDispatchIndirectCommand(
	struct vulkan_reader *reader,
	VkDispatchIndirectCommand *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->x = (uint32_t)vulkan_read_u32(reader);
	record->y = (uint32_t)vulkan_read_u32(reader);
	record->z = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkDrawIndexedIndirectCommand independently of native structure padding.
 */
void
vulkan_encode_VkDrawIndexedIndirectCommand(
	struct vulkan_writer *writer,
	const VkDrawIndexedIndirectCommand *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->indexCount);
	vulkan_write_u32(writer, record->instanceCount);
	vulkan_write_u32(writer, record->firstIndex);
	vulkan_write_u32(writer, record->vertexOffset);
	vulkan_write_u32(writer, record->firstInstance);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkDrawIndexedIndirectCommand independently of native structure padding.
 */
void
vulkan_decode_VkDrawIndexedIndirectCommand(
	struct vulkan_reader *reader,
	VkDrawIndexedIndirectCommand *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->indexCount = (uint32_t)vulkan_read_u32(reader);
	record->instanceCount = (uint32_t)vulkan_read_u32(reader);
	record->firstIndex = (uint32_t)vulkan_read_u32(reader);
	record->vertexOffset = (int32_t)vulkan_read_u32(reader);
	record->firstInstance = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkDrawIndirectCommand independently of native structure padding.
 */
void
vulkan_encode_VkDrawIndirectCommand(
	struct vulkan_writer *writer,
	const VkDrawIndirectCommand *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->vertexCount);
	vulkan_write_u32(writer, record->instanceCount);
	vulkan_write_u32(writer, record->firstVertex);
	vulkan_write_u32(writer, record->firstInstance);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkDrawIndirectCommand independently of native structure padding.
 */
void
vulkan_decode_VkDrawIndirectCommand(
	struct vulkan_reader *reader,
	VkDrawIndirectCommand *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->vertexCount = (uint32_t)vulkan_read_u32(reader);
	record->instanceCount = (uint32_t)vulkan_read_u32(reader);
	record->firstVertex = (uint32_t)vulkan_read_u32(reader);
	record->firstInstance = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkImageSubresourceRange independently of native structure padding.
 */
void
vulkan_encode_VkImageSubresourceRange(
	struct vulkan_writer *writer,
	const VkImageSubresourceRange *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->aspectMask);
	vulkan_write_u32(writer, record->baseMipLevel);
	vulkan_write_u32(writer, record->levelCount);
	vulkan_write_u32(writer, record->baseArrayLayer);
	vulkan_write_u32(writer, record->layerCount);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkImageSubresourceRange independently of native structure padding.
 */
void
vulkan_decode_VkImageSubresourceRange(
	struct vulkan_reader *reader,
	VkImageSubresourceRange *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->aspectMask = (VkImageAspectFlags)vulkan_read_u32(reader);
	record->baseMipLevel = (uint32_t)vulkan_read_u32(reader);
	record->levelCount = (uint32_t)vulkan_read_u32(reader);
	record->baseArrayLayer = (uint32_t)vulkan_read_u32(reader);
	record->layerCount = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkImageMemoryBarrier independently of native structure padding.
 */
void
vulkan_encode_VkImageMemoryBarrier(
	struct vulkan_writer *writer,
	const VkImageMemoryBarrier *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->srcAccessMask);
	vulkan_write_u32(writer, record->dstAccessMask);
	vulkan_write_u32(writer, vulkan_wire_image_layout(record->oldLayout));
	vulkan_write_u32(writer, vulkan_wire_image_layout(record->newLayout));
	vulkan_write_u32(writer, record->srcQueueFamilyIndex);
	vulkan_write_u32(writer, record->dstQueueFamilyIndex);
	vulkan_encode_handle(writer, (uint64_t)record->image);
	vulkan_encode_VkImageSubresourceRange(writer, &record->subresourceRange);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkMemoryBarrier independently of native structure padding.
 */
void
vulkan_encode_VkMemoryBarrier(
	struct vulkan_writer *writer,
	const VkMemoryBarrier *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->srcAccessMask);
	vulkan_write_u32(writer, record->dstAccessMask);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineCacheHeaderVersionOne independently of native structure padding.
 */
void
vulkan_encode_VkPipelineCacheHeaderVersionOne(
	struct vulkan_writer *writer,
	const VkPipelineCacheHeaderVersionOne *record)
{
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->headerSize);
	vulkan_write_u32(writer, record->headerVersion);
	vulkan_write_u32(writer, record->vendorID);
	vulkan_write_u32(writer, record->deviceID);

	/* Includes the required extent marker even though pipelineCacheUUID has a fixed API length. */
	count = VK_UUID_SIZE;
	vulkan_write_u64(writer, count);

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_write_bytes(writer, record->pipelineCacheUUID, VK_UUID_SIZE);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkPipelineCacheHeaderVersionOne independently of native structure padding.
 */
void
vulkan_decode_VkPipelineCacheHeaderVersionOne(
	struct vulkan_reader *reader,
	VkPipelineCacheHeaderVersionOne *record)
{
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->headerSize = (uint32_t)vulkan_read_u32(reader);
	record->headerVersion = (VkPipelineCacheHeaderVersion)vulkan_read_u32(reader);
	record->vendorID = (uint32_t)vulkan_read_u32(reader);
	record->deviceID = (uint32_t)vulkan_read_u32(reader);

	/* Requires the exact declared extent of pipelineCacheUUID before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != VK_UUID_SIZE) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_read_bytes(reader, record->pipelineCacheUUID, VK_UUID_SIZE);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkApplicationInfo independently of native structure padding.
 */
void
vulkan_encode_VkApplicationInfo(
	struct vulkan_writer *writer,
	const VkApplicationInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Serializes pApplicationName with its terminator and protocol byte padding. */
	vulkan_write_string(writer, record->pApplicationName);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->applicationVersion);

	/* Serializes pEngineName with its terminator and protocol byte padding. */
	vulkan_write_string(writer, record->pEngineName);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->engineVersion);
	vulkan_write_u32(writer, record->apiVersion);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkFormatProperties independently of native structure padding.
 */
void
vulkan_encode_VkFormatProperties(
	struct vulkan_writer *writer,
	const VkFormatProperties *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->linearTilingFeatures);
	vulkan_write_u32(writer, record->optimalTilingFeatures);
	vulkan_write_u32(writer, record->bufferFeatures);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkFormatProperties independently of native structure padding.
 */
void
vulkan_decode_VkFormatProperties(
	struct vulkan_reader *reader,
	VkFormatProperties *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->linearTilingFeatures = (VkFormatFeatureFlags)vulkan_read_u32(reader);
	record->optimalTilingFeatures = (VkFormatFeatureFlags)vulkan_read_u32(reader);
	record->bufferFeatures = (VkFormatFeatureFlags)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkImageFormatProperties independently of native structure padding.
 */
void
vulkan_encode_VkImageFormatProperties(
	struct vulkan_writer *writer,
	const VkImageFormatProperties *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_VkExtent3D(writer, &record->maxExtent);
	vulkan_write_u32(writer, record->maxMipLevels);
	vulkan_write_u32(writer, record->maxArrayLayers);
	vulkan_write_u32(writer, record->sampleCounts);
	vulkan_write_u64(writer, record->maxResourceSize);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkImageFormatProperties independently of native structure padding.
 */
void
vulkan_decode_VkImageFormatProperties(
	struct vulkan_reader *reader,
	VkImageFormatProperties *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_decode_VkExtent3D(reader, &record->maxExtent);
	record->maxMipLevels = (uint32_t)vulkan_read_u32(reader);
	record->maxArrayLayers = (uint32_t)vulkan_read_u32(reader);
	record->sampleCounts = (VkSampleCountFlags)vulkan_read_u32(reader);
	record->maxResourceSize = (VkDeviceSize)vulkan_read_u64(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkInstanceCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkInstanceCreateInfo(
	struct vulkan_writer *writer,
	const VkInstanceCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);

	/* Keeps absent pApplicationInfo distinct from its count-selected payload. */
	count = 0;
	if (record->pApplicationInfo != NULL)
		count = 1;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Traverses the optional record only when its presence marker was emitted. */
	if (count != 0)
		vulkan_encode_VkApplicationInfo(writer, record->pApplicationInfo);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->enabledLayerCount);

	/* Keeps absent ppEnabledLayerNames distinct from its count-selected payload. */
	count = 0;
	if (record->ppEnabledLayerNames != NULL)
		count = record->enabledLayerCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_string(writer, record->ppEnabledLayerNames[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->enabledExtensionCount);

	/* Keeps absent ppEnabledExtensionNames distinct from its count-selected payload. */
	count = 0;
	if (record->ppEnabledExtensionNames != NULL)
		count = record->enabledExtensionCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_string(writer, record->ppEnabledExtensionNames[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkMemoryHeap independently of native structure padding.
 */
void
vulkan_encode_VkMemoryHeap(
	struct vulkan_writer *writer,
	const VkMemoryHeap *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u64(writer, record->size);
	vulkan_write_u32(writer, record->flags);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkMemoryHeap independently of native structure padding.
 */
void
vulkan_decode_VkMemoryHeap(
	struct vulkan_reader *reader,
	VkMemoryHeap *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->size = (VkDeviceSize)vulkan_read_u64(reader);
	record->flags = (VkMemoryHeapFlags)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkMemoryType independently of native structure padding.
 */
void
vulkan_encode_VkMemoryType(
	struct vulkan_writer *writer,
	const VkMemoryType *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->propertyFlags);
	vulkan_write_u32(writer, record->heapIndex);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkMemoryType independently of native structure padding.
 */
void
vulkan_decode_VkMemoryType(
	struct vulkan_reader *reader,
	VkMemoryType *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->propertyFlags = (VkMemoryPropertyFlags)vulkan_read_u32(reader);
	record->heapIndex = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPhysicalDeviceFeatures independently of native structure padding.
 */
void
vulkan_encode_VkPhysicalDeviceFeatures(
	struct vulkan_writer *writer,
	const VkPhysicalDeviceFeatures *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->robustBufferAccess);
	vulkan_write_u32(writer, record->fullDrawIndexUint32);
	vulkan_write_u32(writer, record->imageCubeArray);
	vulkan_write_u32(writer, record->independentBlend);
	vulkan_write_u32(writer, record->geometryShader);
	vulkan_write_u32(writer, record->tessellationShader);
	vulkan_write_u32(writer, record->sampleRateShading);
	vulkan_write_u32(writer, record->dualSrcBlend);
	vulkan_write_u32(writer, record->logicOp);
	vulkan_write_u32(writer, record->multiDrawIndirect);
	vulkan_write_u32(writer, record->drawIndirectFirstInstance);
	vulkan_write_u32(writer, record->depthClamp);
	vulkan_write_u32(writer, record->depthBiasClamp);
	vulkan_write_u32(writer, record->fillModeNonSolid);
	vulkan_write_u32(writer, record->depthBounds);
	vulkan_write_u32(writer, record->wideLines);
	vulkan_write_u32(writer, record->largePoints);
	vulkan_write_u32(writer, record->alphaToOne);
	vulkan_write_u32(writer, record->multiViewport);
	vulkan_write_u32(writer, record->samplerAnisotropy);
	vulkan_write_u32(writer, record->textureCompressionETC2);
	vulkan_write_u32(writer, record->textureCompressionASTC_LDR);
	vulkan_write_u32(writer, record->textureCompressionBC);
	vulkan_write_u32(writer, record->occlusionQueryPrecise);
	vulkan_write_u32(writer, record->pipelineStatisticsQuery);
	vulkan_write_u32(writer, record->vertexPipelineStoresAndAtomics);
	vulkan_write_u32(writer, record->fragmentStoresAndAtomics);
	vulkan_write_u32(writer, record->shaderTessellationAndGeometryPointSize);
	vulkan_write_u32(writer, record->shaderImageGatherExtended);
	vulkan_write_u32(writer, record->shaderStorageImageExtendedFormats);
	vulkan_write_u32(writer, record->shaderStorageImageMultisample);
	vulkan_write_u32(writer, record->shaderStorageImageReadWithoutFormat);
	vulkan_write_u32(writer, record->shaderStorageImageWriteWithoutFormat);
	vulkan_write_u32(writer, record->shaderUniformBufferArrayDynamicIndexing);
	vulkan_write_u32(writer, record->shaderSampledImageArrayDynamicIndexing);
	vulkan_write_u32(writer, record->shaderStorageBufferArrayDynamicIndexing);
	vulkan_write_u32(writer, record->shaderStorageImageArrayDynamicIndexing);
	vulkan_write_u32(writer, record->shaderClipDistance);
	vulkan_write_u32(writer, record->shaderCullDistance);
	vulkan_write_u32(writer, record->shaderFloat64);
	vulkan_write_u32(writer, record->shaderInt64);
	vulkan_write_u32(writer, record->shaderInt16);
	vulkan_write_u32(writer, record->shaderResourceResidency);
	vulkan_write_u32(writer, record->shaderResourceMinLod);
	vulkan_write_u32(writer, record->sparseBinding);
	vulkan_write_u32(writer, record->sparseResidencyBuffer);
	vulkan_write_u32(writer, record->sparseResidencyImage2D);
	vulkan_write_u32(writer, record->sparseResidencyImage3D);
	vulkan_write_u32(writer, record->sparseResidency2Samples);
	vulkan_write_u32(writer, record->sparseResidency4Samples);
	vulkan_write_u32(writer, record->sparseResidency8Samples);
	vulkan_write_u32(writer, record->sparseResidency16Samples);
	vulkan_write_u32(writer, record->sparseResidencyAliased);
	vulkan_write_u32(writer, record->variableMultisampleRate);
	vulkan_write_u32(writer, record->inheritedQueries);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkPhysicalDeviceFeatures independently of native structure padding.
 */
void
vulkan_decode_VkPhysicalDeviceFeatures(
	struct vulkan_reader *reader,
	VkPhysicalDeviceFeatures *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->robustBufferAccess = (VkBool32)vulkan_read_u32(reader);
	record->fullDrawIndexUint32 = (VkBool32)vulkan_read_u32(reader);
	record->imageCubeArray = (VkBool32)vulkan_read_u32(reader);
	record->independentBlend = (VkBool32)vulkan_read_u32(reader);
	record->geometryShader = (VkBool32)vulkan_read_u32(reader);
	record->tessellationShader = (VkBool32)vulkan_read_u32(reader);
	record->sampleRateShading = (VkBool32)vulkan_read_u32(reader);
	record->dualSrcBlend = (VkBool32)vulkan_read_u32(reader);
	record->logicOp = (VkBool32)vulkan_read_u32(reader);
	record->multiDrawIndirect = (VkBool32)vulkan_read_u32(reader);
	record->drawIndirectFirstInstance = (VkBool32)vulkan_read_u32(reader);
	record->depthClamp = (VkBool32)vulkan_read_u32(reader);
	record->depthBiasClamp = (VkBool32)vulkan_read_u32(reader);
	record->fillModeNonSolid = (VkBool32)vulkan_read_u32(reader);
	record->depthBounds = (VkBool32)vulkan_read_u32(reader);
	record->wideLines = (VkBool32)vulkan_read_u32(reader);
	record->largePoints = (VkBool32)vulkan_read_u32(reader);
	record->alphaToOne = (VkBool32)vulkan_read_u32(reader);
	record->multiViewport = (VkBool32)vulkan_read_u32(reader);
	record->samplerAnisotropy = (VkBool32)vulkan_read_u32(reader);
	record->textureCompressionETC2 = (VkBool32)vulkan_read_u32(reader);
	record->textureCompressionASTC_LDR = (VkBool32)vulkan_read_u32(reader);
	record->textureCompressionBC = (VkBool32)vulkan_read_u32(reader);
	record->occlusionQueryPrecise = (VkBool32)vulkan_read_u32(reader);
	record->pipelineStatisticsQuery = (VkBool32)vulkan_read_u32(reader);
	record->vertexPipelineStoresAndAtomics = (VkBool32)vulkan_read_u32(reader);
	record->fragmentStoresAndAtomics = (VkBool32)vulkan_read_u32(reader);
	record->shaderTessellationAndGeometryPointSize = (VkBool32)vulkan_read_u32(reader);
	record->shaderImageGatherExtended = (VkBool32)vulkan_read_u32(reader);
	record->shaderStorageImageExtendedFormats = (VkBool32)vulkan_read_u32(reader);
	record->shaderStorageImageMultisample = (VkBool32)vulkan_read_u32(reader);
	record->shaderStorageImageReadWithoutFormat = (VkBool32)vulkan_read_u32(reader);
	record->shaderStorageImageWriteWithoutFormat = (VkBool32)vulkan_read_u32(reader);
	record->shaderUniformBufferArrayDynamicIndexing = (VkBool32)vulkan_read_u32(reader);
	record->shaderSampledImageArrayDynamicIndexing = (VkBool32)vulkan_read_u32(reader);
	record->shaderStorageBufferArrayDynamicIndexing = (VkBool32)vulkan_read_u32(reader);
	record->shaderStorageImageArrayDynamicIndexing = (VkBool32)vulkan_read_u32(reader);
	record->shaderClipDistance = (VkBool32)vulkan_read_u32(reader);
	record->shaderCullDistance = (VkBool32)vulkan_read_u32(reader);
	record->shaderFloat64 = (VkBool32)vulkan_read_u32(reader);
	record->shaderInt64 = (VkBool32)vulkan_read_u32(reader);
	record->shaderInt16 = (VkBool32)vulkan_read_u32(reader);
	record->shaderResourceResidency = (VkBool32)vulkan_read_u32(reader);
	record->shaderResourceMinLod = (VkBool32)vulkan_read_u32(reader);
	record->sparseBinding = (VkBool32)vulkan_read_u32(reader);
	record->sparseResidencyBuffer = (VkBool32)vulkan_read_u32(reader);
	record->sparseResidencyImage2D = (VkBool32)vulkan_read_u32(reader);
	record->sparseResidencyImage3D = (VkBool32)vulkan_read_u32(reader);
	record->sparseResidency2Samples = (VkBool32)vulkan_read_u32(reader);
	record->sparseResidency4Samples = (VkBool32)vulkan_read_u32(reader);
	record->sparseResidency8Samples = (VkBool32)vulkan_read_u32(reader);
	record->sparseResidency16Samples = (VkBool32)vulkan_read_u32(reader);
	record->sparseResidencyAliased = (VkBool32)vulkan_read_u32(reader);
	record->variableMultisampleRate = (VkBool32)vulkan_read_u32(reader);
	record->inheritedQueries = (VkBool32)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPhysicalDeviceLimits independently of native structure padding.
 */
void
vulkan_encode_VkPhysicalDeviceLimits(
	struct vulkan_writer *writer,
	const VkPhysicalDeviceLimits *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->maxImageDimension1D);
	vulkan_write_u32(writer, record->maxImageDimension2D);
	vulkan_write_u32(writer, record->maxImageDimension3D);
	vulkan_write_u32(writer, record->maxImageDimensionCube);
	vulkan_write_u32(writer, record->maxImageArrayLayers);
	vulkan_write_u32(writer, record->maxTexelBufferElements);
	vulkan_write_u32(writer, record->maxUniformBufferRange);
	vulkan_write_u32(writer, record->maxStorageBufferRange);
	vulkan_write_u32(writer, record->maxPushConstantsSize);
	vulkan_write_u32(writer, record->maxMemoryAllocationCount);
	vulkan_write_u32(writer, record->maxSamplerAllocationCount);
	vulkan_write_u64(writer, record->bufferImageGranularity);
	vulkan_write_u64(writer, record->sparseAddressSpaceSize);
	vulkan_write_u32(writer, record->maxBoundDescriptorSets);
	vulkan_write_u32(writer, record->maxPerStageDescriptorSamplers);
	vulkan_write_u32(writer, record->maxPerStageDescriptorUniformBuffers);
	vulkan_write_u32(writer, record->maxPerStageDescriptorStorageBuffers);
	vulkan_write_u32(writer, record->maxPerStageDescriptorSampledImages);
	vulkan_write_u32(writer, record->maxPerStageDescriptorStorageImages);
	vulkan_write_u32(writer, record->maxPerStageDescriptorInputAttachments);
	vulkan_write_u32(writer, record->maxPerStageResources);
	vulkan_write_u32(writer, record->maxDescriptorSetSamplers);
	vulkan_write_u32(writer, record->maxDescriptorSetUniformBuffers);
	vulkan_write_u32(writer, record->maxDescriptorSetUniformBuffersDynamic);
	vulkan_write_u32(writer, record->maxDescriptorSetStorageBuffers);
	vulkan_write_u32(writer, record->maxDescriptorSetStorageBuffersDynamic);
	vulkan_write_u32(writer, record->maxDescriptorSetSampledImages);
	vulkan_write_u32(writer, record->maxDescriptorSetStorageImages);
	vulkan_write_u32(writer, record->maxDescriptorSetInputAttachments);
	vulkan_write_u32(writer, record->maxVertexInputAttributes);
	vulkan_write_u32(writer, record->maxVertexInputBindings);
	vulkan_write_u32(writer, record->maxVertexInputAttributeOffset);
	vulkan_write_u32(writer, record->maxVertexInputBindingStride);
	vulkan_write_u32(writer, record->maxVertexOutputComponents);
	vulkan_write_u32(writer, record->maxTessellationGenerationLevel);
	vulkan_write_u32(writer, record->maxTessellationPatchSize);
	vulkan_write_u32(writer, record->maxTessellationControlPerVertexInputComponents);
	vulkan_write_u32(writer, record->maxTessellationControlPerVertexOutputComponents);
	vulkan_write_u32(writer, record->maxTessellationControlPerPatchOutputComponents);
	vulkan_write_u32(writer, record->maxTessellationControlTotalOutputComponents);
	vulkan_write_u32(writer, record->maxTessellationEvaluationInputComponents);
	vulkan_write_u32(writer, record->maxTessellationEvaluationOutputComponents);
	vulkan_write_u32(writer, record->maxGeometryShaderInvocations);
	vulkan_write_u32(writer, record->maxGeometryInputComponents);
	vulkan_write_u32(writer, record->maxGeometryOutputComponents);
	vulkan_write_u32(writer, record->maxGeometryOutputVertices);
	vulkan_write_u32(writer, record->maxGeometryTotalOutputComponents);
	vulkan_write_u32(writer, record->maxFragmentInputComponents);
	vulkan_write_u32(writer, record->maxFragmentOutputAttachments);
	vulkan_write_u32(writer, record->maxFragmentDualSrcAttachments);
	vulkan_write_u32(writer, record->maxFragmentCombinedOutputResources);
	vulkan_write_u32(writer, record->maxComputeSharedMemorySize);

	/* Includes the required extent marker even though maxComputeWorkGroupCount has a fixed API length. */
	count = 3;
	vulkan_write_u64(writer, count);

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_write_u32(writer, record->maxComputeWorkGroupCount[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->maxComputeWorkGroupInvocations);

	/* Includes the required extent marker even though maxComputeWorkGroupSize has a fixed API length. */
	count = 3;
	vulkan_write_u64(writer, count);

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_write_u32(writer, record->maxComputeWorkGroupSize[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->subPixelPrecisionBits);
	vulkan_write_u32(writer, record->subTexelPrecisionBits);
	vulkan_write_u32(writer, record->mipmapPrecisionBits);
	vulkan_write_u32(writer, record->maxDrawIndexedIndexValue);
	vulkan_write_u32(writer, record->maxDrawIndirectCount);
	vulkan_write_float(writer, record->maxSamplerLodBias);
	vulkan_write_float(writer, record->maxSamplerAnisotropy);
	vulkan_write_u32(writer, record->maxViewports);

	/* Includes the required extent marker even though maxViewportDimensions has a fixed API length. */
	count = 2;
	vulkan_write_u64(writer, count);

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_write_u32(writer, record->maxViewportDimensions[index]);
	}

	/* Includes the required extent marker even though viewportBoundsRange has a fixed API length. */
	count = 2;
	vulkan_write_u64(writer, count);

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_write_float(writer, record->viewportBoundsRange[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->viewportSubPixelBits);
	vulkan_write_u64(writer, record->minMemoryMapAlignment);
	vulkan_write_u64(writer, record->minTexelBufferOffsetAlignment);
	vulkan_write_u64(writer, record->minUniformBufferOffsetAlignment);
	vulkan_write_u64(writer, record->minStorageBufferOffsetAlignment);
	vulkan_write_u32(writer, record->minTexelOffset);
	vulkan_write_u32(writer, record->maxTexelOffset);
	vulkan_write_u32(writer, record->minTexelGatherOffset);
	vulkan_write_u32(writer, record->maxTexelGatherOffset);
	vulkan_write_float(writer, record->minInterpolationOffset);
	vulkan_write_float(writer, record->maxInterpolationOffset);
	vulkan_write_u32(writer, record->subPixelInterpolationOffsetBits);
	vulkan_write_u32(writer, record->maxFramebufferWidth);
	vulkan_write_u32(writer, record->maxFramebufferHeight);
	vulkan_write_u32(writer, record->maxFramebufferLayers);
	vulkan_write_u32(writer, record->framebufferColorSampleCounts);
	vulkan_write_u32(writer, record->framebufferDepthSampleCounts);
	vulkan_write_u32(writer, record->framebufferStencilSampleCounts);
	vulkan_write_u32(writer, record->framebufferNoAttachmentsSampleCounts);
	vulkan_write_u32(writer, record->maxColorAttachments);
	vulkan_write_u32(writer, record->sampledImageColorSampleCounts);
	vulkan_write_u32(writer, record->sampledImageIntegerSampleCounts);
	vulkan_write_u32(writer, record->sampledImageDepthSampleCounts);
	vulkan_write_u32(writer, record->sampledImageStencilSampleCounts);
	vulkan_write_u32(writer, record->storageImageSampleCounts);
	vulkan_write_u32(writer, record->maxSampleMaskWords);
	vulkan_write_u32(writer, record->timestampComputeAndGraphics);
	vulkan_write_float(writer, record->timestampPeriod);
	vulkan_write_u32(writer, record->maxClipDistances);
	vulkan_write_u32(writer, record->maxCullDistances);
	vulkan_write_u32(writer, record->maxCombinedClipAndCullDistances);
	vulkan_write_u32(writer, record->discreteQueuePriorities);

	/* Includes the required extent marker even though pointSizeRange has a fixed API length. */
	count = 2;
	vulkan_write_u64(writer, count);

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_write_float(writer, record->pointSizeRange[index]);
	}

	/* Includes the required extent marker even though lineWidthRange has a fixed API length. */
	count = 2;
	vulkan_write_u64(writer, count);

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_write_float(writer, record->lineWidthRange[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_float(writer, record->pointSizeGranularity);
	vulkan_write_float(writer, record->lineWidthGranularity);
	vulkan_write_u32(writer, record->strictLines);
	vulkan_write_u32(writer, record->standardSampleLocations);
	vulkan_write_u64(writer, record->optimalBufferCopyOffsetAlignment);
	vulkan_write_u64(writer, record->optimalBufferCopyRowPitchAlignment);
	vulkan_write_u64(writer, record->nonCoherentAtomSize);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkPhysicalDeviceLimits independently of native structure padding.
 */
void
vulkan_decode_VkPhysicalDeviceLimits(
	struct vulkan_reader *reader,
	VkPhysicalDeviceLimits *record)
{
	size_t index;
	uint64_t count;
	uint64_t wide;

	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->maxImageDimension1D = (uint32_t)vulkan_read_u32(reader);
	record->maxImageDimension2D = (uint32_t)vulkan_read_u32(reader);
	record->maxImageDimension3D = (uint32_t)vulkan_read_u32(reader);
	record->maxImageDimensionCube = (uint32_t)vulkan_read_u32(reader);
	record->maxImageArrayLayers = (uint32_t)vulkan_read_u32(reader);
	record->maxTexelBufferElements = (uint32_t)vulkan_read_u32(reader);
	record->maxUniformBufferRange = (uint32_t)vulkan_read_u32(reader);
	record->maxStorageBufferRange = (uint32_t)vulkan_read_u32(reader);
	record->maxPushConstantsSize = (uint32_t)vulkan_read_u32(reader);
	record->maxMemoryAllocationCount = (uint32_t)vulkan_read_u32(reader);
	record->maxSamplerAllocationCount = (uint32_t)vulkan_read_u32(reader);
	record->bufferImageGranularity = (VkDeviceSize)vulkan_read_u64(reader);
	record->sparseAddressSpaceSize = (VkDeviceSize)vulkan_read_u64(reader);
	record->maxBoundDescriptorSets = (uint32_t)vulkan_read_u32(reader);
	record->maxPerStageDescriptorSamplers = (uint32_t)vulkan_read_u32(reader);
	record->maxPerStageDescriptorUniformBuffers = (uint32_t)vulkan_read_u32(reader);
	record->maxPerStageDescriptorStorageBuffers = (uint32_t)vulkan_read_u32(reader);
	record->maxPerStageDescriptorSampledImages = (uint32_t)vulkan_read_u32(reader);
	record->maxPerStageDescriptorStorageImages = (uint32_t)vulkan_read_u32(reader);
	record->maxPerStageDescriptorInputAttachments = (uint32_t)vulkan_read_u32(reader);
	record->maxPerStageResources = (uint32_t)vulkan_read_u32(reader);
	record->maxDescriptorSetSamplers = (uint32_t)vulkan_read_u32(reader);
	record->maxDescriptorSetUniformBuffers = (uint32_t)vulkan_read_u32(reader);
	record->maxDescriptorSetUniformBuffersDynamic = (uint32_t)vulkan_read_u32(reader);
	record->maxDescriptorSetStorageBuffers = (uint32_t)vulkan_read_u32(reader);
	record->maxDescriptorSetStorageBuffersDynamic = (uint32_t)vulkan_read_u32(reader);
	record->maxDescriptorSetSampledImages = (uint32_t)vulkan_read_u32(reader);
	record->maxDescriptorSetStorageImages = (uint32_t)vulkan_read_u32(reader);
	record->maxDescriptorSetInputAttachments = (uint32_t)vulkan_read_u32(reader);
	record->maxVertexInputAttributes = (uint32_t)vulkan_read_u32(reader);
	record->maxVertexInputBindings = (uint32_t)vulkan_read_u32(reader);
	record->maxVertexInputAttributeOffset = (uint32_t)vulkan_read_u32(reader);
	record->maxVertexInputBindingStride = (uint32_t)vulkan_read_u32(reader);
	record->maxVertexOutputComponents = (uint32_t)vulkan_read_u32(reader);
	record->maxTessellationGenerationLevel = (uint32_t)vulkan_read_u32(reader);
	record->maxTessellationPatchSize = (uint32_t)vulkan_read_u32(reader);
	record->maxTessellationControlPerVertexInputComponents = (uint32_t)vulkan_read_u32(reader);
	record->maxTessellationControlPerVertexOutputComponents = (uint32_t)vulkan_read_u32(reader);
	record->maxTessellationControlPerPatchOutputComponents = (uint32_t)vulkan_read_u32(reader);
	record->maxTessellationControlTotalOutputComponents = (uint32_t)vulkan_read_u32(reader);
	record->maxTessellationEvaluationInputComponents = (uint32_t)vulkan_read_u32(reader);
	record->maxTessellationEvaluationOutputComponents = (uint32_t)vulkan_read_u32(reader);
	record->maxGeometryShaderInvocations = (uint32_t)vulkan_read_u32(reader);
	record->maxGeometryInputComponents = (uint32_t)vulkan_read_u32(reader);
	record->maxGeometryOutputComponents = (uint32_t)vulkan_read_u32(reader);
	record->maxGeometryOutputVertices = (uint32_t)vulkan_read_u32(reader);
	record->maxGeometryTotalOutputComponents = (uint32_t)vulkan_read_u32(reader);
	record->maxFragmentInputComponents = (uint32_t)vulkan_read_u32(reader);
	record->maxFragmentOutputAttachments = (uint32_t)vulkan_read_u32(reader);
	record->maxFragmentDualSrcAttachments = (uint32_t)vulkan_read_u32(reader);
	record->maxFragmentCombinedOutputResources = (uint32_t)vulkan_read_u32(reader);
	record->maxComputeSharedMemorySize = (uint32_t)vulkan_read_u32(reader);

	/* Requires the exact declared extent of maxComputeWorkGroupCount before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != 3) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && reader->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		record->maxComputeWorkGroupCount[index] = (uint32_t)vulkan_read_u32(reader);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->maxComputeWorkGroupInvocations = (uint32_t)vulkan_read_u32(reader);

	/* Requires the exact declared extent of maxComputeWorkGroupSize before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != 3) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && reader->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		record->maxComputeWorkGroupSize[index] = (uint32_t)vulkan_read_u32(reader);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->subPixelPrecisionBits = (uint32_t)vulkan_read_u32(reader);
	record->subTexelPrecisionBits = (uint32_t)vulkan_read_u32(reader);
	record->mipmapPrecisionBits = (uint32_t)vulkan_read_u32(reader);
	record->maxDrawIndexedIndexValue = (uint32_t)vulkan_read_u32(reader);
	record->maxDrawIndirectCount = (uint32_t)vulkan_read_u32(reader);
	record->maxSamplerLodBias = (float)vulkan_read_float(reader);
	record->maxSamplerAnisotropy = (float)vulkan_read_float(reader);
	record->maxViewports = (uint32_t)vulkan_read_u32(reader);

	/* Requires the exact declared extent of maxViewportDimensions before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != 2) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && reader->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		record->maxViewportDimensions[index] = (uint32_t)vulkan_read_u32(reader);
	}

	/* Requires the exact declared extent of viewportBoundsRange before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != 2) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && reader->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		record->viewportBoundsRange[index] = (float)vulkan_read_float(reader);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->viewportSubPixelBits = (uint32_t)vulkan_read_u32(reader);

	/* Rejects a renderer extent outside this application address width. */
	wide = vulkan_read_u64(reader);
	if (wide > SIZE_MAX) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Publishes the extent after proving that this ABI can represent it. */
	record->minMemoryMapAlignment = (size_t)wide;
	record->minTexelBufferOffsetAlignment = (VkDeviceSize)vulkan_read_u64(reader);
	record->minUniformBufferOffsetAlignment = (VkDeviceSize)vulkan_read_u64(reader);
	record->minStorageBufferOffsetAlignment = (VkDeviceSize)vulkan_read_u64(reader);
	record->minTexelOffset = (int32_t)vulkan_read_u32(reader);
	record->maxTexelOffset = (uint32_t)vulkan_read_u32(reader);
	record->minTexelGatherOffset = (int32_t)vulkan_read_u32(reader);
	record->maxTexelGatherOffset = (uint32_t)vulkan_read_u32(reader);
	record->minInterpolationOffset = (float)vulkan_read_float(reader);
	record->maxInterpolationOffset = (float)vulkan_read_float(reader);
	record->subPixelInterpolationOffsetBits = (uint32_t)vulkan_read_u32(reader);
	record->maxFramebufferWidth = (uint32_t)vulkan_read_u32(reader);
	record->maxFramebufferHeight = (uint32_t)vulkan_read_u32(reader);
	record->maxFramebufferLayers = (uint32_t)vulkan_read_u32(reader);
	record->framebufferColorSampleCounts = (VkSampleCountFlags)vulkan_read_u32(reader);
	record->framebufferDepthSampleCounts = (VkSampleCountFlags)vulkan_read_u32(reader);
	record->framebufferStencilSampleCounts = (VkSampleCountFlags)vulkan_read_u32(reader);
	record->framebufferNoAttachmentsSampleCounts = (VkSampleCountFlags)vulkan_read_u32(reader);
	record->maxColorAttachments = (uint32_t)vulkan_read_u32(reader);
	record->sampledImageColorSampleCounts = (VkSampleCountFlags)vulkan_read_u32(reader);
	record->sampledImageIntegerSampleCounts = (VkSampleCountFlags)vulkan_read_u32(reader);
	record->sampledImageDepthSampleCounts = (VkSampleCountFlags)vulkan_read_u32(reader);
	record->sampledImageStencilSampleCounts = (VkSampleCountFlags)vulkan_read_u32(reader);
	record->storageImageSampleCounts = (VkSampleCountFlags)vulkan_read_u32(reader);
	record->maxSampleMaskWords = (uint32_t)vulkan_read_u32(reader);
	record->timestampComputeAndGraphics = (VkBool32)vulkan_read_u32(reader);
	record->timestampPeriod = (float)vulkan_read_float(reader);
	record->maxClipDistances = (uint32_t)vulkan_read_u32(reader);
	record->maxCullDistances = (uint32_t)vulkan_read_u32(reader);
	record->maxCombinedClipAndCullDistances = (uint32_t)vulkan_read_u32(reader);
	record->discreteQueuePriorities = (uint32_t)vulkan_read_u32(reader);

	/* Requires the exact declared extent of pointSizeRange before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != 2) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && reader->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		record->pointSizeRange[index] = (float)vulkan_read_float(reader);
	}

	/* Requires the exact declared extent of lineWidthRange before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != 2) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && reader->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		record->lineWidthRange[index] = (float)vulkan_read_float(reader);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->pointSizeGranularity = (float)vulkan_read_float(reader);
	record->lineWidthGranularity = (float)vulkan_read_float(reader);
	record->strictLines = (VkBool32)vulkan_read_u32(reader);
	record->standardSampleLocations = (VkBool32)vulkan_read_u32(reader);
	record->optimalBufferCopyOffsetAlignment = (VkDeviceSize)vulkan_read_u64(reader);
	record->optimalBufferCopyRowPitchAlignment = (VkDeviceSize)vulkan_read_u64(reader);
	record->nonCoherentAtomSize = (VkDeviceSize)vulkan_read_u64(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPhysicalDeviceMemoryProperties independently of native structure padding.
 */
void
vulkan_encode_VkPhysicalDeviceMemoryProperties(
	struct vulkan_writer *writer,
	const VkPhysicalDeviceMemoryProperties *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->memoryTypeCount);

	/* Includes the required extent marker even though memoryTypes has a fixed API length. */
	count = VK_MAX_MEMORY_TYPES;
	vulkan_write_u64(writer, count);

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_encode_VkMemoryType(writer, &record->memoryTypes[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->memoryHeapCount);

	/* Includes the required extent marker even though memoryHeaps has a fixed API length. */
	count = VK_MAX_MEMORY_HEAPS;
	vulkan_write_u64(writer, count);

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_encode_VkMemoryHeap(writer, &record->memoryHeaps[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkPhysicalDeviceMemoryProperties independently of native structure padding.
 */
void
vulkan_decode_VkPhysicalDeviceMemoryProperties(
	struct vulkan_reader *reader,
	VkPhysicalDeviceMemoryProperties *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->memoryTypeCount = (uint32_t)vulkan_read_u32(reader);

	/* Requires the exact declared extent of memoryTypes before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != VK_MAX_MEMORY_TYPES) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && reader->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_decode_VkMemoryType(reader, &record->memoryTypes[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->memoryHeapCount = (uint32_t)vulkan_read_u32(reader);

	/* Requires the exact declared extent of memoryHeaps before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != VK_MAX_MEMORY_HEAPS) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && reader->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_decode_VkMemoryHeap(reader, &record->memoryHeaps[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPhysicalDeviceSparseProperties independently of native structure padding.
 */
void
vulkan_encode_VkPhysicalDeviceSparseProperties(
	struct vulkan_writer *writer,
	const VkPhysicalDeviceSparseProperties *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->residencyStandard2DBlockShape);
	vulkan_write_u32(writer, record->residencyStandard2DMultisampleBlockShape);
	vulkan_write_u32(writer, record->residencyStandard3DBlockShape);
	vulkan_write_u32(writer, record->residencyAlignedMipSize);
	vulkan_write_u32(writer, record->residencyNonResidentStrict);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkPhysicalDeviceSparseProperties independently of native structure padding.
 */
void
vulkan_decode_VkPhysicalDeviceSparseProperties(
	struct vulkan_reader *reader,
	VkPhysicalDeviceSparseProperties *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->residencyStandard2DBlockShape = (VkBool32)vulkan_read_u32(reader);
	record->residencyStandard2DMultisampleBlockShape = (VkBool32)vulkan_read_u32(reader);
	record->residencyStandard3DBlockShape = (VkBool32)vulkan_read_u32(reader);
	record->residencyAlignedMipSize = (VkBool32)vulkan_read_u32(reader);
	record->residencyNonResidentStrict = (VkBool32)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPhysicalDeviceProperties independently of native structure padding.
 */
void
vulkan_encode_VkPhysicalDeviceProperties(
	struct vulkan_writer *writer,
	const VkPhysicalDeviceProperties *record)
{
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->apiVersion);
	vulkan_write_u32(writer, record->driverVersion);
	vulkan_write_u32(writer, record->vendorID);
	vulkan_write_u32(writer, record->deviceID);
	vulkan_write_u32(writer, record->deviceType);

	/* Includes the required extent marker even though deviceName has a fixed API length. */
	count = VK_MAX_PHYSICAL_DEVICE_NAME_SIZE;
	vulkan_write_u64(writer, count);

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_write_bytes(writer, record->deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);

	/* Includes the required extent marker even though pipelineCacheUUID has a fixed API length. */
	count = VK_UUID_SIZE;
	vulkan_write_u64(writer, count);

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_write_bytes(writer, record->pipelineCacheUUID, VK_UUID_SIZE);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_VkPhysicalDeviceLimits(writer, &record->limits);
	vulkan_encode_VkPhysicalDeviceSparseProperties(writer, &record->sparseProperties);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkPhysicalDeviceProperties independently of native structure padding.
 */
void
vulkan_decode_VkPhysicalDeviceProperties(
	struct vulkan_reader *reader,
	VkPhysicalDeviceProperties *record)
{
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->apiVersion = (uint32_t)vulkan_read_u32(reader);
	record->driverVersion = (uint32_t)vulkan_read_u32(reader);
	record->vendorID = (uint32_t)vulkan_read_u32(reader);
	record->deviceID = (uint32_t)vulkan_read_u32(reader);
	record->deviceType = (VkPhysicalDeviceType)vulkan_read_u32(reader);

	/* Requires the exact declared extent of deviceName before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != VK_MAX_PHYSICAL_DEVICE_NAME_SIZE) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_read_bytes(reader, record->deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);

	/* Requires the exact declared extent of pipelineCacheUUID before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != VK_UUID_SIZE) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_read_bytes(reader, record->pipelineCacheUUID, VK_UUID_SIZE);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_decode_VkPhysicalDeviceLimits(reader, &record->limits);
	vulkan_decode_VkPhysicalDeviceSparseProperties(reader, &record->sparseProperties);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkQueueFamilyProperties independently of native structure padding.
 */
void
vulkan_encode_VkQueueFamilyProperties(
	struct vulkan_writer *writer,
	const VkQueueFamilyProperties *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->queueFlags);
	vulkan_write_u32(writer, record->queueCount);
	vulkan_write_u32(writer, record->timestampValidBits);
	vulkan_encode_VkExtent3D(writer, &record->minImageTransferGranularity);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkQueueFamilyProperties independently of native structure padding.
 */
void
vulkan_decode_VkQueueFamilyProperties(
	struct vulkan_reader *reader,
	VkQueueFamilyProperties *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->queueFlags = (VkQueueFlags)vulkan_read_u32(reader);
	record->queueCount = (uint32_t)vulkan_read_u32(reader);
	record->timestampValidBits = (uint32_t)vulkan_read_u32(reader);
	vulkan_decode_VkExtent3D(reader, &record->minImageTransferGranularity);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkDeviceQueueCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkDeviceQueueCreateInfo(
	struct vulkan_writer *writer,
	const VkDeviceQueueCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->queueFamilyIndex);
	vulkan_write_u32(writer, record->queueCount);

	/* Keeps absent pQueuePriorities distinct from its count-selected payload. */
	count = 0;
	if (record->pQueuePriorities != NULL)
		count = record->queueCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_float(writer, record->pQueuePriorities[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkDeviceCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkDeviceCreateInfo(
	struct vulkan_writer *writer,
	const VkDeviceCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->queueCreateInfoCount);

	/* Keeps absent pQueueCreateInfos distinct from its count-selected payload. */
	count = 0;
	if (record->pQueueCreateInfos != NULL)
		count = record->queueCreateInfoCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkDeviceQueueCreateInfo(writer, &record->pQueueCreateInfos[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->enabledLayerCount);

	/* Keeps absent ppEnabledLayerNames distinct from its count-selected payload. */
	count = 0;
	if (record->ppEnabledLayerNames != NULL)
		count = record->enabledLayerCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_string(writer, record->ppEnabledLayerNames[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->enabledExtensionCount);

	/* Keeps absent ppEnabledExtensionNames distinct from its count-selected payload. */
	count = 0;
	if (record->ppEnabledExtensionNames != NULL)
		count = record->enabledExtensionCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_string(writer, record->ppEnabledExtensionNames[index]);
	}

	/* Keeps absent pEnabledFeatures distinct from its count-selected payload. */
	count = 0;
	if (record->pEnabledFeatures != NULL)
		count = 1;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Traverses the optional record only when its presence marker was emitted. */
	if (count != 0)
		vulkan_encode_VkPhysicalDeviceFeatures(writer, record->pEnabledFeatures);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkExtensionProperties independently of native structure padding.
 */
void
vulkan_encode_VkExtensionProperties(
	struct vulkan_writer *writer,
	const VkExtensionProperties *record)
{
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Includes the required extent marker even though extensionName has a fixed API length. */
	count = VK_MAX_EXTENSION_NAME_SIZE;
	vulkan_write_u64(writer, count);

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_write_bytes(writer, record->extensionName, VK_MAX_EXTENSION_NAME_SIZE);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->specVersion);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkExtensionProperties independently of native structure padding.
 */
void
vulkan_decode_VkExtensionProperties(
	struct vulkan_reader *reader,
	VkExtensionProperties *record)
{
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Requires the exact declared extent of extensionName before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != VK_MAX_EXTENSION_NAME_SIZE) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_read_bytes(reader, record->extensionName, VK_MAX_EXTENSION_NAME_SIZE);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->specVersion = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkLayerProperties independently of native structure padding.
 */
void
vulkan_encode_VkLayerProperties(
	struct vulkan_writer *writer,
	const VkLayerProperties *record)
{
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Includes the required extent marker even though layerName has a fixed API length. */
	count = VK_MAX_EXTENSION_NAME_SIZE;
	vulkan_write_u64(writer, count);

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_write_bytes(writer, record->layerName, VK_MAX_EXTENSION_NAME_SIZE);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->specVersion);
	vulkan_write_u32(writer, record->implementationVersion);

	/* Includes the required extent marker even though description has a fixed API length. */
	count = VK_MAX_DESCRIPTION_SIZE;
	vulkan_write_u64(writer, count);

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_write_bytes(writer, record->description, VK_MAX_DESCRIPTION_SIZE);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkLayerProperties independently of native structure padding.
 */
void
vulkan_decode_VkLayerProperties(
	struct vulkan_reader *reader,
	VkLayerProperties *record)
{
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Requires the exact declared extent of layerName before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != VK_MAX_EXTENSION_NAME_SIZE) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_read_bytes(reader, record->layerName, VK_MAX_EXTENSION_NAME_SIZE);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->specVersion = (uint32_t)vulkan_read_u32(reader);
	record->implementationVersion = (uint32_t)vulkan_read_u32(reader);

	/* Requires the exact declared extent of description before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != VK_MAX_DESCRIPTION_SIZE) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Copies the byte array with wire padding independent of its enclosing record. */
	vulkan_read_bytes(reader, record->description, VK_MAX_DESCRIPTION_SIZE);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSubmitInfo independently of native structure padding.
 */
void
vulkan_encode_VkSubmitInfo(
	struct vulkan_writer *writer,
	const VkSubmitInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->waitSemaphoreCount);

	/* Keeps absent pWaitSemaphores distinct from its count-selected payload. */
	count = 0;
	if (record->pWaitSemaphores != NULL)
		count = record->waitSemaphoreCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_handle(writer, (uint64_t)record->pWaitSemaphores[index]);
	}

	/* Keeps absent pWaitDstStageMask distinct from its count-selected payload. */
	count = 0;
	if (record->pWaitDstStageMask != NULL)
		count = record->waitSemaphoreCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_u32(writer, record->pWaitDstStageMask[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->commandBufferCount);

	/* Keeps absent pCommandBuffers distinct from its count-selected payload. */
	count = 0;
	if (record->pCommandBuffers != NULL)
		count = record->commandBufferCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_handle(writer, (uint64_t)(uintptr_t)record->pCommandBuffers[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->signalSemaphoreCount);

	/* Keeps absent pSignalSemaphores distinct from its count-selected payload. */
	count = 0;
	if (record->pSignalSemaphores != NULL)
		count = record->signalSemaphoreCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_handle(writer, (uint64_t)record->pSignalSemaphores[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkMappedMemoryRange independently of native structure padding.
 */
void
vulkan_encode_VkMappedMemoryRange(
	struct vulkan_writer *writer,
	const VkMappedMemoryRange *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_handle(writer, (uint64_t)record->memory);
	vulkan_write_u64(writer, record->offset);
	vulkan_write_u64(writer, record->size);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkMemoryAllocateInfo independently of native structure padding.
 */
void
vulkan_encode_VkMemoryAllocateInfo(
	struct vulkan_writer *writer,
	const VkMemoryAllocateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u64(writer, record->allocationSize);
	vulkan_write_u32(writer, record->memoryTypeIndex);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkMemoryRequirements independently of native structure padding.
 */
void
vulkan_encode_VkMemoryRequirements(
	struct vulkan_writer *writer,
	const VkMemoryRequirements *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u64(writer, record->size);
	vulkan_write_u64(writer, record->alignment);
	vulkan_write_u32(writer, record->memoryTypeBits);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkMemoryRequirements independently of native structure padding.
 */
void
vulkan_decode_VkMemoryRequirements(
	struct vulkan_reader *reader,
	VkMemoryRequirements *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->size = (VkDeviceSize)vulkan_read_u64(reader);
	record->alignment = (VkDeviceSize)vulkan_read_u64(reader);
	record->memoryTypeBits = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSparseMemoryBind independently of native structure padding.
 */
void
vulkan_encode_VkSparseMemoryBind(
	struct vulkan_writer *writer,
	const VkSparseMemoryBind *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u64(writer, record->resourceOffset);
	vulkan_write_u64(writer, record->size);
	vulkan_encode_handle(writer, (uint64_t)record->memory);
	vulkan_write_u64(writer, record->memoryOffset);
	vulkan_write_u32(writer, record->flags);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSparseBufferMemoryBindInfo independently of native structure padding.
 */
void
vulkan_encode_VkSparseBufferMemoryBindInfo(
	struct vulkan_writer *writer,
	const VkSparseBufferMemoryBindInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_handle(writer, (uint64_t)record->buffer);
	vulkan_write_u32(writer, record->bindCount);

	/* Keeps absent pBinds distinct from its count-selected payload. */
	count = 0;
	if (record->pBinds != NULL)
		count = record->bindCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkSparseMemoryBind(writer, &record->pBinds[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSparseImageOpaqueMemoryBindInfo independently of native structure padding.
 */
void
vulkan_encode_VkSparseImageOpaqueMemoryBindInfo(
	struct vulkan_writer *writer,
	const VkSparseImageOpaqueMemoryBindInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_handle(writer, (uint64_t)record->image);
	vulkan_write_u32(writer, record->bindCount);

	/* Keeps absent pBinds distinct from its count-selected payload. */
	count = 0;
	if (record->pBinds != NULL)
		count = record->bindCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkSparseMemoryBind(writer, &record->pBinds[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkImageSubresource independently of native structure padding.
 */
void
vulkan_encode_VkImageSubresource(
	struct vulkan_writer *writer,
	const VkImageSubresource *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->aspectMask);
	vulkan_write_u32(writer, record->mipLevel);
	vulkan_write_u32(writer, record->arrayLayer);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkImageSubresource independently of native structure padding.
 */
void
vulkan_decode_VkImageSubresource(
	struct vulkan_reader *reader,
	VkImageSubresource *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->aspectMask = (VkImageAspectFlags)vulkan_read_u32(reader);
	record->mipLevel = (uint32_t)vulkan_read_u32(reader);
	record->arrayLayer = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSparseImageMemoryBind independently of native structure padding.
 */
void
vulkan_encode_VkSparseImageMemoryBind(
	struct vulkan_writer *writer,
	const VkSparseImageMemoryBind *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_VkImageSubresource(writer, &record->subresource);
	vulkan_encode_VkOffset3D(writer, &record->offset);
	vulkan_encode_VkExtent3D(writer, &record->extent);
	vulkan_encode_handle(writer, (uint64_t)record->memory);
	vulkan_write_u64(writer, record->memoryOffset);
	vulkan_write_u32(writer, record->flags);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSparseImageMemoryBindInfo independently of native structure padding.
 */
void
vulkan_encode_VkSparseImageMemoryBindInfo(
	struct vulkan_writer *writer,
	const VkSparseImageMemoryBindInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_handle(writer, (uint64_t)record->image);
	vulkan_write_u32(writer, record->bindCount);

	/* Keeps absent pBinds distinct from its count-selected payload. */
	count = 0;
	if (record->pBinds != NULL)
		count = record->bindCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkSparseImageMemoryBind(writer, &record->pBinds[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkBindSparseInfo independently of native structure padding.
 */
void
vulkan_encode_VkBindSparseInfo(
	struct vulkan_writer *writer,
	const VkBindSparseInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->waitSemaphoreCount);

	/* Keeps absent pWaitSemaphores distinct from its count-selected payload. */
	count = 0;
	if (record->pWaitSemaphores != NULL)
		count = record->waitSemaphoreCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_handle(writer, (uint64_t)record->pWaitSemaphores[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->bufferBindCount);

	/* Keeps absent pBufferBinds distinct from its count-selected payload. */
	count = 0;
	if (record->pBufferBinds != NULL)
		count = record->bufferBindCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkSparseBufferMemoryBindInfo(writer, &record->pBufferBinds[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->imageOpaqueBindCount);

	/* Keeps absent pImageOpaqueBinds distinct from its count-selected payload. */
	count = 0;
	if (record->pImageOpaqueBinds != NULL)
		count = record->imageOpaqueBindCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkSparseImageOpaqueMemoryBindInfo(writer, &record->pImageOpaqueBinds[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->imageBindCount);

	/* Keeps absent pImageBinds distinct from its count-selected payload. */
	count = 0;
	if (record->pImageBinds != NULL)
		count = record->imageBindCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkSparseImageMemoryBindInfo(writer, &record->pImageBinds[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->signalSemaphoreCount);

	/* Keeps absent pSignalSemaphores distinct from its count-selected payload. */
	count = 0;
	if (record->pSignalSemaphores != NULL)
		count = record->signalSemaphoreCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_handle(writer, (uint64_t)record->pSignalSemaphores[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSparseImageFormatProperties independently of native structure padding.
 */
void
vulkan_encode_VkSparseImageFormatProperties(
	struct vulkan_writer *writer,
	const VkSparseImageFormatProperties *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->aspectMask);
	vulkan_encode_VkExtent3D(writer, &record->imageGranularity);
	vulkan_write_u32(writer, record->flags);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkSparseImageFormatProperties independently of native structure padding.
 */
void
vulkan_decode_VkSparseImageFormatProperties(
	struct vulkan_reader *reader,
	VkSparseImageFormatProperties *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->aspectMask = (VkImageAspectFlags)vulkan_read_u32(reader);
	vulkan_decode_VkExtent3D(reader, &record->imageGranularity);
	record->flags = (VkSparseImageFormatFlags)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSparseImageMemoryRequirements independently of native structure padding.
 */
void
vulkan_encode_VkSparseImageMemoryRequirements(
	struct vulkan_writer *writer,
	const VkSparseImageMemoryRequirements *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_VkSparseImageFormatProperties(writer, &record->formatProperties);
	vulkan_write_u32(writer, record->imageMipTailFirstLod);
	vulkan_write_u64(writer, record->imageMipTailSize);
	vulkan_write_u64(writer, record->imageMipTailOffset);
	vulkan_write_u64(writer, record->imageMipTailStride);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkSparseImageMemoryRequirements independently of native structure padding.
 */
void
vulkan_decode_VkSparseImageMemoryRequirements(
	struct vulkan_reader *reader,
	VkSparseImageMemoryRequirements *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_decode_VkSparseImageFormatProperties(reader, &record->formatProperties);
	record->imageMipTailFirstLod = (uint32_t)vulkan_read_u32(reader);
	record->imageMipTailSize = (VkDeviceSize)vulkan_read_u64(reader);
	record->imageMipTailOffset = (VkDeviceSize)vulkan_read_u64(reader);
	record->imageMipTailStride = (VkDeviceSize)vulkan_read_u64(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkFenceCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkFenceCreateInfo(
	struct vulkan_writer *writer,
	const VkFenceCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSemaphoreCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkSemaphoreCreateInfo(
	struct vulkan_writer *writer,
	const VkSemaphoreCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkEventCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkEventCreateInfo(
	struct vulkan_writer *writer,
	const VkEventCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkQueryPoolCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkQueryPoolCreateInfo(
	struct vulkan_writer *writer,
	const VkQueryPoolCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->queryType);
	vulkan_write_u32(writer, record->queryCount);
	vulkan_write_u32(writer, record->pipelineStatistics);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkBufferCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkBufferCreateInfo(
	struct vulkan_writer *writer,
	const VkBufferCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u64(writer, record->size);
	vulkan_write_u32(writer, record->usage);
	vulkan_write_u32(writer, record->sharingMode);
	vulkan_write_u32(writer, record->queueFamilyIndexCount);

	/* Does not inspect queue-family storage ignored by exclusive ownership. */
	count = 0;
	if (record->sharingMode == VK_SHARING_MODE_CONCURRENT && record->pQueueFamilyIndices != NULL)
		count = record->queueFamilyIndexCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_u32(writer, record->pQueueFamilyIndices[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkBufferViewCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkBufferViewCreateInfo(
	struct vulkan_writer *writer,
	const VkBufferViewCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_encode_handle(writer, (uint64_t)record->buffer);
	vulkan_write_u32(writer, record->format);
	vulkan_write_u64(writer, record->offset);
	vulkan_write_u64(writer, record->range);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkImageCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkImageCreateInfo(
	struct vulkan_writer *writer,
	const VkImageCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->imageType);
	vulkan_write_u32(writer, record->format);
	vulkan_encode_VkExtent3D(writer, &record->extent);
	vulkan_write_u32(writer, record->mipLevels);
	vulkan_write_u32(writer, record->arrayLayers);
	vulkan_write_u32(writer, record->samples);
	vulkan_write_u32(writer, record->tiling);
	vulkan_write_u32(writer, record->usage);
	vulkan_write_u32(writer, record->sharingMode);
	vulkan_write_u32(writer, record->queueFamilyIndexCount);

	/* Does not inspect queue-family storage ignored by exclusive ownership. */
	count = 0;
	if (record->sharingMode == VK_SHARING_MODE_CONCURRENT && record->pQueueFamilyIndices != NULL)
		count = record->queueFamilyIndexCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_u32(writer, record->pQueueFamilyIndices[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, vulkan_wire_image_layout(record->initialLayout));

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSubresourceLayout independently of native structure padding.
 */
void
vulkan_encode_VkSubresourceLayout(
	struct vulkan_writer *writer,
	const VkSubresourceLayout *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u64(writer, record->offset);
	vulkan_write_u64(writer, record->size);
	vulkan_write_u64(writer, record->rowPitch);
	vulkan_write_u64(writer, record->arrayPitch);
	vulkan_write_u64(writer, record->depthPitch);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkSubresourceLayout independently of native structure padding.
 */
void
vulkan_decode_VkSubresourceLayout(
	struct vulkan_reader *reader,
	VkSubresourceLayout *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->offset = (VkDeviceSize)vulkan_read_u64(reader);
	record->size = (VkDeviceSize)vulkan_read_u64(reader);
	record->rowPitch = (VkDeviceSize)vulkan_read_u64(reader);
	record->arrayPitch = (VkDeviceSize)vulkan_read_u64(reader);
	record->depthPitch = (VkDeviceSize)vulkan_read_u64(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkComponentMapping independently of native structure padding.
 */
void
vulkan_encode_VkComponentMapping(
	struct vulkan_writer *writer,
	const VkComponentMapping *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->r);
	vulkan_write_u32(writer, record->g);
	vulkan_write_u32(writer, record->b);
	vulkan_write_u32(writer, record->a);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkComponentMapping independently of native structure padding.
 */
void
vulkan_decode_VkComponentMapping(
	struct vulkan_reader *reader,
	VkComponentMapping *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->r = (VkComponentSwizzle)vulkan_read_u32(reader);
	record->g = (VkComponentSwizzle)vulkan_read_u32(reader);
	record->b = (VkComponentSwizzle)vulkan_read_u32(reader);
	record->a = (VkComponentSwizzle)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkImageViewCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkImageViewCreateInfo(
	struct vulkan_writer *writer,
	const VkImageViewCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_encode_handle(writer, (uint64_t)record->image);
	vulkan_write_u32(writer, record->viewType);
	vulkan_write_u32(writer, record->format);
	vulkan_encode_VkComponentMapping(writer, &record->components);
	vulkan_encode_VkImageSubresourceRange(writer, &record->subresourceRange);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkShaderModuleCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkShaderModuleCreateInfo(
	struct vulkan_writer *writer,
	const VkShaderModuleCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u64(writer, record->codeSize);

	/* Keeps absent pCode distinct from its count-selected payload. */
	count = 0;
	if (record->pCode != NULL)
		count = record->codeSize / 4;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_u32(writer, record->pCode[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineCacheCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineCacheCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineCacheCreateInfo *record)
{
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u64(writer, record->initialDataSize);

	/* Keeps absent pInitialData distinct from its count-selected payload. */
	count = 0;
	if (record->pInitialData != NULL)
		count = record->initialDataSize;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);
	vulkan_write_bytes(writer, record->pInitialData, (size_t)count);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSpecializationMapEntry independently of native structure padding.
 */
void
vulkan_encode_VkSpecializationMapEntry(
	struct vulkan_writer *writer,
	const VkSpecializationMapEntry *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->constantID);
	vulkan_write_u32(writer, record->offset);
	vulkan_write_u64(writer, record->size);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkSpecializationMapEntry independently of native structure padding.
 */
void
vulkan_decode_VkSpecializationMapEntry(
	struct vulkan_reader *reader,
	VkSpecializationMapEntry *record)
{
	uint64_t wide;

	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->constantID = (uint32_t)vulkan_read_u32(reader);
	record->offset = (uint32_t)vulkan_read_u32(reader);

	/* Rejects a renderer extent outside this application address width. */
	wide = vulkan_read_u64(reader);
	if (wide > SIZE_MAX) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Publishes the extent after proving that this ABI can represent it. */
	record->size = (size_t)wide;

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSpecializationInfo independently of native structure padding.
 */
void
vulkan_encode_VkSpecializationInfo(
	struct vulkan_writer *writer,
	const VkSpecializationInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->mapEntryCount);

	/* Keeps absent pMapEntries distinct from its count-selected payload. */
	count = 0;
	if (record->pMapEntries != NULL)
		count = record->mapEntryCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkSpecializationMapEntry(writer, &record->pMapEntries[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u64(writer, record->dataSize);

	/* Keeps absent pData distinct from its count-selected payload. */
	count = 0;
	if (record->pData != NULL)
		count = record->dataSize;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);
	vulkan_write_bytes(writer, record->pData, (size_t)count);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineShaderStageCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineShaderStageCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineShaderStageCreateInfo *record)
{
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->stage);
	vulkan_encode_handle(writer, (uint64_t)record->module);

	/* Serializes pName with its terminator and protocol byte padding. */
	vulkan_write_string(writer, record->pName);

	/* Keeps absent pSpecializationInfo distinct from its count-selected payload. */
	count = 0;
	if (record->pSpecializationInfo != NULL)
		count = 1;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Traverses the optional record only when its presence marker was emitted. */
	if (count != 0)
		vulkan_encode_VkSpecializationInfo(writer, record->pSpecializationInfo);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkComputePipelineCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkComputePipelineCreateInfo(
	struct vulkan_writer *writer,
	const VkComputePipelineCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_encode_VkPipelineShaderStageCreateInfo(writer, &record->stage);
	vulkan_encode_handle(writer, (uint64_t)record->layout);
	vulkan_encode_handle(writer, (uint64_t)record->basePipelineHandle);
	vulkan_write_u32(writer, record->basePipelineIndex);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkVertexInputBindingDescription independently of native structure padding.
 */
void
vulkan_encode_VkVertexInputBindingDescription(
	struct vulkan_writer *writer,
	const VkVertexInputBindingDescription *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->binding);
	vulkan_write_u32(writer, record->stride);
	vulkan_write_u32(writer, record->inputRate);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkVertexInputBindingDescription independently of native structure padding.
 */
void
vulkan_decode_VkVertexInputBindingDescription(
	struct vulkan_reader *reader,
	VkVertexInputBindingDescription *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->binding = (uint32_t)vulkan_read_u32(reader);
	record->stride = (uint32_t)vulkan_read_u32(reader);
	record->inputRate = (VkVertexInputRate)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkVertexInputAttributeDescription independently of native structure padding.
 */
void
vulkan_encode_VkVertexInputAttributeDescription(
	struct vulkan_writer *writer,
	const VkVertexInputAttributeDescription *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->location);
	vulkan_write_u32(writer, record->binding);
	vulkan_write_u32(writer, record->format);
	vulkan_write_u32(writer, record->offset);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkVertexInputAttributeDescription independently of native structure padding.
 */
void
vulkan_decode_VkVertexInputAttributeDescription(
	struct vulkan_reader *reader,
	VkVertexInputAttributeDescription *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->location = (uint32_t)vulkan_read_u32(reader);
	record->binding = (uint32_t)vulkan_read_u32(reader);
	record->format = (VkFormat)vulkan_read_u32(reader);
	record->offset = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineVertexInputStateCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineVertexInputStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineVertexInputStateCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->vertexBindingDescriptionCount);

	/* Keeps absent pVertexBindingDescriptions distinct from its count-selected payload. */
	count = 0;
	if (record->pVertexBindingDescriptions != NULL)
		count = record->vertexBindingDescriptionCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkVertexInputBindingDescription(writer, &record->pVertexBindingDescriptions[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->vertexAttributeDescriptionCount);

	/* Keeps absent pVertexAttributeDescriptions distinct from its count-selected payload. */
	count = 0;
	if (record->pVertexAttributeDescriptions != NULL)
		count = record->vertexAttributeDescriptionCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkVertexInputAttributeDescription(writer, &record->pVertexAttributeDescriptions[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineInputAssemblyStateCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineInputAssemblyStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineInputAssemblyStateCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->topology);
	vulkan_write_u32(writer, record->primitiveRestartEnable);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineTessellationStateCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineTessellationStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineTessellationStateCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->patchControlPoints);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkViewport independently of native structure padding.
 */
void
vulkan_encode_VkViewport(
	struct vulkan_writer *writer,
	const VkViewport *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_float(writer, record->x);
	vulkan_write_float(writer, record->y);
	vulkan_write_float(writer, record->width);
	vulkan_write_float(writer, record->height);
	vulkan_write_float(writer, record->minDepth);
	vulkan_write_float(writer, record->maxDepth);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkViewport independently of native structure padding.
 */
void
vulkan_decode_VkViewport(
	struct vulkan_reader *reader,
	VkViewport *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->x = (float)vulkan_read_float(reader);
	record->y = (float)vulkan_read_float(reader);
	record->width = (float)vulkan_read_float(reader);
	record->height = (float)vulkan_read_float(reader);
	record->minDepth = (float)vulkan_read_float(reader);
	record->maxDepth = (float)vulkan_read_float(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineViewportStateCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineViewportStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineViewportStateCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->viewportCount);

	/* Keeps absent pViewports distinct from its count-selected payload. */
	count = 0;
	if (record->pViewports != NULL)
		count = record->viewportCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkViewport(writer, &record->pViewports[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->scissorCount);

	/* Keeps absent pScissors distinct from its count-selected payload. */
	count = 0;
	if (record->pScissors != NULL)
		count = record->scissorCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkRect2D(writer, &record->pScissors[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineRasterizationStateCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineRasterizationStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineRasterizationStateCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->depthClampEnable);
	vulkan_write_u32(writer, record->rasterizerDiscardEnable);
	vulkan_write_u32(writer, record->polygonMode);
	vulkan_write_u32(writer, record->cullMode);
	vulkan_write_u32(writer, record->frontFace);
	vulkan_write_u32(writer, record->depthBiasEnable);
	vulkan_write_float(writer, record->depthBiasConstantFactor);
	vulkan_write_float(writer, record->depthBiasClamp);
	vulkan_write_float(writer, record->depthBiasSlopeFactor);
	vulkan_write_float(writer, record->lineWidth);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineMultisampleStateCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineMultisampleStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineMultisampleStateCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->rasterizationSamples);
	vulkan_write_u32(writer, record->sampleShadingEnable);
	vulkan_write_float(writer, record->minSampleShading);

	/* Keeps absent pSampleMask distinct from its count-selected payload. */
	count = 0;
	if (record->pSampleMask != NULL)
		count = ((uint32_t)record->rasterizationSamples + 31) / 32;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_u32(writer, record->pSampleMask[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->alphaToCoverageEnable);
	vulkan_write_u32(writer, record->alphaToOneEnable);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkStencilOpState independently of native structure padding.
 */
void
vulkan_encode_VkStencilOpState(
	struct vulkan_writer *writer,
	const VkStencilOpState *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->failOp);
	vulkan_write_u32(writer, record->passOp);
	vulkan_write_u32(writer, record->depthFailOp);
	vulkan_write_u32(writer, record->compareOp);
	vulkan_write_u32(writer, record->compareMask);
	vulkan_write_u32(writer, record->writeMask);
	vulkan_write_u32(writer, record->reference);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkStencilOpState independently of native structure padding.
 */
void
vulkan_decode_VkStencilOpState(
	struct vulkan_reader *reader,
	VkStencilOpState *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->failOp = (VkStencilOp)vulkan_read_u32(reader);
	record->passOp = (VkStencilOp)vulkan_read_u32(reader);
	record->depthFailOp = (VkStencilOp)vulkan_read_u32(reader);
	record->compareOp = (VkCompareOp)vulkan_read_u32(reader);
	record->compareMask = (uint32_t)vulkan_read_u32(reader);
	record->writeMask = (uint32_t)vulkan_read_u32(reader);
	record->reference = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineDepthStencilStateCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineDepthStencilStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineDepthStencilStateCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->depthTestEnable);
	vulkan_write_u32(writer, record->depthWriteEnable);
	vulkan_write_u32(writer, record->depthCompareOp);
	vulkan_write_u32(writer, record->depthBoundsTestEnable);
	vulkan_write_u32(writer, record->stencilTestEnable);
	vulkan_encode_VkStencilOpState(writer, &record->front);
	vulkan_encode_VkStencilOpState(writer, &record->back);
	vulkan_write_float(writer, record->minDepthBounds);
	vulkan_write_float(writer, record->maxDepthBounds);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineColorBlendAttachmentState independently of native structure padding.
 */
void
vulkan_encode_VkPipelineColorBlendAttachmentState(
	struct vulkan_writer *writer,
	const VkPipelineColorBlendAttachmentState *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->blendEnable);
	vulkan_write_u32(writer, record->srcColorBlendFactor);
	vulkan_write_u32(writer, record->dstColorBlendFactor);
	vulkan_write_u32(writer, record->colorBlendOp);
	vulkan_write_u32(writer, record->srcAlphaBlendFactor);
	vulkan_write_u32(writer, record->dstAlphaBlendFactor);
	vulkan_write_u32(writer, record->alphaBlendOp);
	vulkan_write_u32(writer, record->colorWriteMask);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkPipelineColorBlendAttachmentState independently of native structure padding.
 */
void
vulkan_decode_VkPipelineColorBlendAttachmentState(
	struct vulkan_reader *reader,
	VkPipelineColorBlendAttachmentState *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->blendEnable = (VkBool32)vulkan_read_u32(reader);
	record->srcColorBlendFactor = (VkBlendFactor)vulkan_read_u32(reader);
	record->dstColorBlendFactor = (VkBlendFactor)vulkan_read_u32(reader);
	record->colorBlendOp = (VkBlendOp)vulkan_read_u32(reader);
	record->srcAlphaBlendFactor = (VkBlendFactor)vulkan_read_u32(reader);
	record->dstAlphaBlendFactor = (VkBlendFactor)vulkan_read_u32(reader);
	record->alphaBlendOp = (VkBlendOp)vulkan_read_u32(reader);
	record->colorWriteMask = (VkColorComponentFlags)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineColorBlendStateCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineColorBlendStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineColorBlendStateCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->logicOpEnable);
	vulkan_write_u32(writer, record->logicOp);
	vulkan_write_u32(writer, record->attachmentCount);

	/* Keeps absent pAttachments distinct from its count-selected payload. */
	count = 0;
	if (record->pAttachments != NULL)
		count = record->attachmentCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkPipelineColorBlendAttachmentState(writer, &record->pAttachments[index]);
	}

	/* Includes the required extent marker even though blendConstants has a fixed API length. */
	count = 4;
	vulkan_write_u64(writer, count);

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_write_float(writer, record->blendConstants[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineDynamicStateCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineDynamicStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineDynamicStateCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->dynamicStateCount);

	/* Keeps absent pDynamicStates distinct from its count-selected payload. */
	count = 0;
	if (record->pDynamicStates != NULL)
		count = record->dynamicStateCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_u32(writer, record->pDynamicStates[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPushConstantRange independently of native structure padding.
 */
void
vulkan_encode_VkPushConstantRange(
	struct vulkan_writer *writer,
	const VkPushConstantRange *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->stageFlags);
	vulkan_write_u32(writer, record->offset);
	vulkan_write_u32(writer, record->size);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkPushConstantRange independently of native structure padding.
 */
void
vulkan_decode_VkPushConstantRange(
	struct vulkan_reader *reader,
	VkPushConstantRange *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->stageFlags = (VkShaderStageFlags)vulkan_read_u32(reader);
	record->offset = (uint32_t)vulkan_read_u32(reader);
	record->size = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkPipelineLayoutCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkPipelineLayoutCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineLayoutCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->setLayoutCount);

	/* Keeps absent pSetLayouts distinct from its count-selected payload. */
	count = 0;
	if (record->pSetLayouts != NULL)
		count = record->setLayoutCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_handle(writer, (uint64_t)record->pSetLayouts[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->pushConstantRangeCount);

	/* Keeps absent pPushConstantRanges distinct from its count-selected payload. */
	count = 0;
	if (record->pPushConstantRanges != NULL)
		count = record->pushConstantRangeCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkPushConstantRange(writer, &record->pPushConstantRanges[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSamplerCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkSamplerCreateInfo(
	struct vulkan_writer *writer,
	const VkSamplerCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->magFilter);
	vulkan_write_u32(writer, record->minFilter);
	vulkan_write_u32(writer, record->mipmapMode);
	vulkan_write_u32(writer, record->addressModeU);
	vulkan_write_u32(writer, record->addressModeV);
	vulkan_write_u32(writer, record->addressModeW);
	vulkan_write_float(writer, record->mipLodBias);
	vulkan_write_u32(writer, record->anisotropyEnable);
	vulkan_write_float(writer, record->maxAnisotropy);
	vulkan_write_u32(writer, record->compareEnable);
	vulkan_write_u32(writer, record->compareOp);
	vulkan_write_float(writer, record->minLod);
	vulkan_write_float(writer, record->maxLod);
	vulkan_write_u32(writer, record->borderColor);
	vulkan_write_u32(writer, record->unnormalizedCoordinates);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkCopyDescriptorSet independently of native structure padding.
 */
void
vulkan_encode_VkCopyDescriptorSet(
	struct vulkan_writer *writer,
	const VkCopyDescriptorSet *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_handle(writer, (uint64_t)record->srcSet);
	vulkan_write_u32(writer, record->srcBinding);
	vulkan_write_u32(writer, record->srcArrayElement);
	vulkan_encode_handle(writer, (uint64_t)record->dstSet);
	vulkan_write_u32(writer, record->dstBinding);
	vulkan_write_u32(writer, record->dstArrayElement);
	vulkan_write_u32(writer, record->descriptorCount);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkDescriptorBufferInfo independently of native structure padding.
 */
void
vulkan_encode_VkDescriptorBufferInfo(
	struct vulkan_writer *writer,
	const VkDescriptorBufferInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_handle(writer, (uint64_t)record->buffer);
	vulkan_write_u64(writer, record->offset);
	vulkan_write_u64(writer, record->range);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkDescriptorPoolSize independently of native structure padding.
 */
void
vulkan_encode_VkDescriptorPoolSize(
	struct vulkan_writer *writer,
	const VkDescriptorPoolSize *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->type);
	vulkan_write_u32(writer, record->descriptorCount);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkDescriptorPoolSize independently of native structure padding.
 */
void
vulkan_decode_VkDescriptorPoolSize(
	struct vulkan_reader *reader,
	VkDescriptorPoolSize *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->type = (VkDescriptorType)vulkan_read_u32(reader);
	record->descriptorCount = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkDescriptorPoolCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkDescriptorPoolCreateInfo(
	struct vulkan_writer *writer,
	const VkDescriptorPoolCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->maxSets);
	vulkan_write_u32(writer, record->poolSizeCount);

	/* Keeps absent pPoolSizes distinct from its count-selected payload. */
	count = 0;
	if (record->pPoolSizes != NULL)
		count = record->poolSizeCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkDescriptorPoolSize(writer, &record->pPoolSizes[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkDescriptorSetAllocateInfo independently of native structure padding.
 */
void
vulkan_encode_VkDescriptorSetAllocateInfo(
	struct vulkan_writer *writer,
	const VkDescriptorSetAllocateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_handle(writer, (uint64_t)record->descriptorPool);
	vulkan_write_u32(writer, record->descriptorSetCount);

	/* Keeps absent pSetLayouts distinct from its count-selected payload. */
	count = 0;
	if (record->pSetLayouts != NULL)
		count = record->descriptorSetCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_handle(writer, (uint64_t)record->pSetLayouts[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkDescriptorSetLayoutBinding independently of native structure padding.
 */
void
vulkan_encode_VkDescriptorSetLayoutBinding(
	struct vulkan_writer *writer,
	const VkDescriptorSetLayoutBinding *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->binding);
	vulkan_write_u32(writer, record->descriptorType);
	vulkan_write_u32(writer, record->descriptorCount);
	vulkan_write_u32(writer, record->stageFlags);

	/* Does not inspect immutable samplers for descriptor types that ignore them. */
	count = 0;
	if ((record->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
	     record->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
	    record->pImmutableSamplers != NULL)
		count = record->descriptorCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_handle(writer, (uint64_t)record->pImmutableSamplers[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkDescriptorSetLayoutCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkDescriptorSetLayoutCreateInfo(
	struct vulkan_writer *writer,
	const VkDescriptorSetLayoutCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->bindingCount);

	/* Keeps absent pBindings distinct from its count-selected payload. */
	count = 0;
	if (record->pBindings != NULL)
		count = record->bindingCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkDescriptorSetLayoutBinding(writer, &record->pBindings[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkAttachmentDescription independently of native structure padding.
 */
void
vulkan_encode_VkAttachmentDescription(
	struct vulkan_writer *writer,
	const VkAttachmentDescription *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->format);
	vulkan_write_u32(writer, record->samples);
	vulkan_write_u32(writer, record->loadOp);
	vulkan_write_u32(writer, record->storeOp);
	vulkan_write_u32(writer, record->stencilLoadOp);
	vulkan_write_u32(writer, record->stencilStoreOp);
	vulkan_write_u32(writer, vulkan_wire_image_layout(record->initialLayout));
	vulkan_write_u32(writer, vulkan_wire_image_layout(record->finalLayout));

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkAttachmentDescription independently of native structure padding.
 */
void
vulkan_decode_VkAttachmentDescription(
	struct vulkan_reader *reader,
	VkAttachmentDescription *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->flags = (VkAttachmentDescriptionFlags)vulkan_read_u32(reader);
	record->format = (VkFormat)vulkan_read_u32(reader);
	record->samples = (VkSampleCountFlagBits)vulkan_read_u32(reader);
	record->loadOp = (VkAttachmentLoadOp)vulkan_read_u32(reader);
	record->storeOp = (VkAttachmentStoreOp)vulkan_read_u32(reader);
	record->stencilLoadOp = (VkAttachmentLoadOp)vulkan_read_u32(reader);
	record->stencilStoreOp = (VkAttachmentStoreOp)vulkan_read_u32(reader);
	record->initialLayout = (VkImageLayout)vulkan_read_u32(reader);
	record->finalLayout = (VkImageLayout)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkAttachmentReference independently of native structure padding.
 */
void
vulkan_encode_VkAttachmentReference(
	struct vulkan_writer *writer,
	const VkAttachmentReference *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->attachment);
	vulkan_write_u32(writer, vulkan_wire_image_layout(record->layout));

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkAttachmentReference independently of native structure padding.
 */
void
vulkan_decode_VkAttachmentReference(
	struct vulkan_reader *reader,
	VkAttachmentReference *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->attachment = (uint32_t)vulkan_read_u32(reader);
	record->layout = (VkImageLayout)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkFramebufferCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkFramebufferCreateInfo(
	struct vulkan_writer *writer,
	const VkFramebufferCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_encode_handle(writer, (uint64_t)record->renderPass);
	vulkan_write_u32(writer, record->attachmentCount);

	/* Keeps absent pAttachments distinct from its count-selected payload. */
	count = 0;
	if (record->pAttachments != NULL)
		count = record->attachmentCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_handle(writer, (uint64_t)record->pAttachments[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->width);
	vulkan_write_u32(writer, record->height);
	vulkan_write_u32(writer, record->layers);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSubpassDescription independently of native structure padding.
 */
void
vulkan_encode_VkSubpassDescription(
	struct vulkan_writer *writer,
	const VkSubpassDescription *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->pipelineBindPoint);
	vulkan_write_u32(writer, record->inputAttachmentCount);

	/* Keeps absent pInputAttachments distinct from its count-selected payload. */
	count = 0;
	if (record->pInputAttachments != NULL)
		count = record->inputAttachmentCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkAttachmentReference(writer, &record->pInputAttachments[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->colorAttachmentCount);

	/* Keeps absent pColorAttachments distinct from its count-selected payload. */
	count = 0;
	if (record->pColorAttachments != NULL)
		count = record->colorAttachmentCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkAttachmentReference(writer, &record->pColorAttachments[index]);
	}

	/* Keeps absent pResolveAttachments distinct from its count-selected payload. */
	count = 0;
	if (record->pResolveAttachments != NULL)
		count = record->colorAttachmentCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkAttachmentReference(writer, &record->pResolveAttachments[index]);
	}

	/* Keeps absent pDepthStencilAttachment distinct from its count-selected payload. */
	count = 0;
	if (record->pDepthStencilAttachment != NULL)
		count = 1;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Traverses the optional record only when its presence marker was emitted. */
	if (count != 0)
		vulkan_encode_VkAttachmentReference(writer, record->pDepthStencilAttachment);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->preserveAttachmentCount);

	/* Keeps absent pPreserveAttachments distinct from its count-selected payload. */
	count = 0;
	if (record->pPreserveAttachments != NULL)
		count = record->preserveAttachmentCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_write_u32(writer, record->pPreserveAttachments[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkSubpassDependency independently of native structure padding.
 */
void
vulkan_encode_VkSubpassDependency(
	struct vulkan_writer *writer,
	const VkSubpassDependency *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->srcSubpass);
	vulkan_write_u32(writer, record->dstSubpass);
	vulkan_write_u32(writer, record->srcStageMask);
	vulkan_write_u32(writer, record->dstStageMask);
	vulkan_write_u32(writer, record->srcAccessMask);
	vulkan_write_u32(writer, record->dstAccessMask);
	vulkan_write_u32(writer, record->dependencyFlags);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkSubpassDependency independently of native structure padding.
 */
void
vulkan_decode_VkSubpassDependency(
	struct vulkan_reader *reader,
	VkSubpassDependency *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->srcSubpass = (uint32_t)vulkan_read_u32(reader);
	record->dstSubpass = (uint32_t)vulkan_read_u32(reader);
	record->srcStageMask = (VkPipelineStageFlags)vulkan_read_u32(reader);
	record->dstStageMask = (VkPipelineStageFlags)vulkan_read_u32(reader);
	record->srcAccessMask = (VkAccessFlags)vulkan_read_u32(reader);
	record->dstAccessMask = (VkAccessFlags)vulkan_read_u32(reader);
	record->dependencyFlags = (VkDependencyFlags)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkRenderPassCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkRenderPassCreateInfo(
	struct vulkan_writer *writer,
	const VkRenderPassCreateInfo *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->attachmentCount);

	/* Keeps absent pAttachments distinct from its count-selected payload. */
	count = 0;
	if (record->pAttachments != NULL)
		count = record->attachmentCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkAttachmentDescription(writer, &record->pAttachments[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->subpassCount);

	/* Keeps absent pSubpasses distinct from its count-selected payload. */
	count = 0;
	if (record->pSubpasses != NULL)
		count = record->subpassCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkSubpassDescription(writer, &record->pSubpasses[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->dependencyCount);

	/* Keeps absent pDependencies distinct from its count-selected payload. */
	count = 0;
	if (record->pDependencies != NULL)
		count = record->dependencyCount;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Stops before touching later array elements after a failed output allocation. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Serializes each selected element without inheriting native array padding. */
		vulkan_encode_VkSubpassDependency(writer, &record->pDependencies[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkCommandPoolCreateInfo independently of native structure padding.
 */
void
vulkan_encode_VkCommandPoolCreateInfo(
	struct vulkan_writer *writer,
	const VkCommandPoolCreateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);
	vulkan_write_u32(writer, record->queueFamilyIndex);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkCommandBufferAllocateInfo independently of native structure padding.
 */
void
vulkan_encode_VkCommandBufferAllocateInfo(
	struct vulkan_writer *writer,
	const VkCommandBufferAllocateInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_handle(writer, (uint64_t)record->commandPool);
	vulkan_write_u32(writer, record->level);
	vulkan_write_u32(writer, record->commandBufferCount);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkCommandBufferInheritanceInfo independently of native structure padding.
 */
void
vulkan_encode_VkCommandBufferInheritanceInfo(
	struct vulkan_writer *writer,
	const VkCommandBufferInheritanceInfo *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_handle(writer, (uint64_t)record->renderPass);
	vulkan_write_u32(writer, record->subpass);
	vulkan_encode_handle(writer, (uint64_t)record->framebuffer);
	vulkan_write_u32(writer, record->occlusionQueryEnable);
	vulkan_write_u32(writer, record->queryFlags);
	vulkan_write_u32(writer, record->pipelineStatistics);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkCommandBufferBeginInfo independently of native structure padding.
 */
void
vulkan_encode_VkCommandBufferBeginInfo(
	struct vulkan_writer *writer,
	const VkCommandBufferBeginInfo *record)
{
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->sType);

	/* No advertised core 1.0 extension adds a chain to this record. */
	vulkan_write_u64(writer, 0);

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->flags);

	/* Keeps absent pInheritanceInfo distinct from its count-selected payload. */
	count = 0;
	if (record->pInheritanceInfo != NULL)
		count = 1;

	/* Publishes the exact 64-bit payload extent before any referenced contents. */
	vulkan_write_u64(writer, count);

	/* Traverses the optional record only when its presence marker was emitted. */
	if (count != 0)
		vulkan_encode_VkCommandBufferInheritanceInfo(writer, record->pInheritanceInfo);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkBufferCopy independently of native structure padding.
 */
void
vulkan_encode_VkBufferCopy(
	struct vulkan_writer *writer,
	const VkBufferCopy *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u64(writer, record->srcOffset);
	vulkan_write_u64(writer, record->dstOffset);
	vulkan_write_u64(writer, record->size);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkBufferCopy independently of native structure padding.
 */
void
vulkan_decode_VkBufferCopy(
	struct vulkan_reader *reader,
	VkBufferCopy *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->srcOffset = (VkDeviceSize)vulkan_read_u64(reader);
	record->dstOffset = (VkDeviceSize)vulkan_read_u64(reader);
	record->size = (VkDeviceSize)vulkan_read_u64(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkImageSubresourceLayers independently of native structure padding.
 */
void
vulkan_encode_VkImageSubresourceLayers(
	struct vulkan_writer *writer,
	const VkImageSubresourceLayers *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u32(writer, record->aspectMask);
	vulkan_write_u32(writer, record->mipLevel);
	vulkan_write_u32(writer, record->baseArrayLayer);
	vulkan_write_u32(writer, record->layerCount);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkImageSubresourceLayers independently of native structure padding.
 */
void
vulkan_decode_VkImageSubresourceLayers(
	struct vulkan_reader *reader,
	VkImageSubresourceLayers *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->aspectMask = (VkImageAspectFlags)vulkan_read_u32(reader);
	record->mipLevel = (uint32_t)vulkan_read_u32(reader);
	record->baseArrayLayer = (uint32_t)vulkan_read_u32(reader);
	record->layerCount = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkBufferImageCopy independently of native structure padding.
 */
void
vulkan_encode_VkBufferImageCopy(
	struct vulkan_writer *writer,
	const VkBufferImageCopy *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_u64(writer, record->bufferOffset);
	vulkan_write_u32(writer, record->bufferRowLength);
	vulkan_write_u32(writer, record->bufferImageHeight);
	vulkan_encode_VkImageSubresourceLayers(writer, &record->imageSubresource);
	vulkan_encode_VkOffset3D(writer, &record->imageOffset);
	vulkan_encode_VkExtent3D(writer, &record->imageExtent);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkBufferImageCopy independently of native structure padding.
 */
void
vulkan_decode_VkBufferImageCopy(
	struct vulkan_reader *reader,
	VkBufferImageCopy *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->bufferOffset = (VkDeviceSize)vulkan_read_u64(reader);
	record->bufferRowLength = (uint32_t)vulkan_read_u32(reader);
	record->bufferImageHeight = (uint32_t)vulkan_read_u32(reader);
	vulkan_decode_VkImageSubresourceLayers(reader, &record->imageSubresource);
	vulkan_decode_VkOffset3D(reader, &record->imageOffset);
	vulkan_decode_VkExtent3D(reader, &record->imageExtent);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkClearDepthStencilValue independently of native structure padding.
 */
void
vulkan_encode_VkClearDepthStencilValue(
	struct vulkan_writer *writer,
	const VkClearDepthStencilValue *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_write_float(writer, record->depth);
	vulkan_write_u32(writer, record->stencil);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkClearDepthStencilValue independently of native structure padding.
 */
void
vulkan_decode_VkClearDepthStencilValue(
	struct vulkan_reader *reader,
	VkClearDepthStencilValue *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	record->depth = (float)vulkan_read_float(reader);
	record->stencil = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkClearRect independently of native structure padding.
 */
void
vulkan_encode_VkClearRect(
	struct vulkan_writer *writer,
	const VkClearRect *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_VkRect2D(writer, &record->rect);
	vulkan_write_u32(writer, record->baseArrayLayer);
	vulkan_write_u32(writer, record->layerCount);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkClearRect independently of native structure padding.
 */
void
vulkan_decode_VkClearRect(
	struct vulkan_reader *reader,
	VkClearRect *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_decode_VkRect2D(reader, &record->rect);
	record->baseArrayLayer = (uint32_t)vulkan_read_u32(reader);
	record->layerCount = (uint32_t)vulkan_read_u32(reader);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkImageBlit independently of native structure padding.
 */
void
vulkan_encode_VkImageBlit(
	struct vulkan_writer *writer,
	const VkImageBlit *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_VkImageSubresourceLayers(writer, &record->srcSubresource);

	/* Includes the required extent marker even though srcOffsets has a fixed API length. */
	count = 2;
	vulkan_write_u64(writer, count);

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_encode_VkOffset3D(writer, &record->srcOffsets[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_VkImageSubresourceLayers(writer, &record->dstSubresource);

	/* Includes the required extent marker even though dstOffsets has a fixed API length. */
	count = 2;
	vulkan_write_u64(writer, count);

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && writer->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_encode_VkOffset3D(writer, &record->dstOffsets[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkImageBlit independently of native structure padding.
 */
void
vulkan_decode_VkImageBlit(
	struct vulkan_reader *reader,
	VkImageBlit *record)
{
	size_t index;
	uint64_t count;

	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_decode_VkImageSubresourceLayers(reader, &record->srcSubresource);

	/* Requires the exact declared extent of srcOffsets before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != 2) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && reader->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_decode_VkOffset3D(reader, &record->srcOffsets[index]);
	}

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_decode_VkImageSubresourceLayers(reader, &record->dstSubresource);

	/* Requires the exact declared extent of dstOffsets before touching output storage. */
	count = vulkan_read_u64(reader);
	if (count != 2) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Traverses only complete fixed-array elements while framing remains valid. */
	for (index = 0;
	     index < count && reader->error == VK_SUCCESS;
	     index++) {
		/* Uses each element's declared scalar width or nested record encoding. */
		vulkan_decode_VkOffset3D(reader, &record->dstOffsets[index]);
	}

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkImageCopy independently of native structure padding.
 */
void
vulkan_encode_VkImageCopy(
	struct vulkan_writer *writer,
	const VkImageCopy *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_VkImageSubresourceLayers(writer, &record->srcSubresource);
	vulkan_encode_VkOffset3D(writer, &record->srcOffset);
	vulkan_encode_VkImageSubresourceLayers(writer, &record->dstSubresource);
	vulkan_encode_VkOffset3D(writer, &record->dstOffset);
	vulkan_encode_VkExtent3D(writer, &record->extent);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkImageCopy independently of native structure padding.
 */
void
vulkan_decode_VkImageCopy(
	struct vulkan_reader *reader,
	VkImageCopy *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_decode_VkImageSubresourceLayers(reader, &record->srcSubresource);
	vulkan_decode_VkOffset3D(reader, &record->srcOffset);
	vulkan_decode_VkImageSubresourceLayers(reader, &record->dstSubresource);
	vulkan_decode_VkOffset3D(reader, &record->dstOffset);
	vulkan_decode_VkExtent3D(reader, &record->extent);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Encodes VkImageResolve independently of native structure padding.
 */
void
vulkan_encode_VkImageResolve(
	struct vulkan_writer *writer,
	const VkImageResolve *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_encode_VkImageSubresourceLayers(writer, &record->srcSubresource);
	vulkan_encode_VkOffset3D(writer, &record->srcOffset);
	vulkan_encode_VkImageSubresourceLayers(writer, &record->dstSubresource);
	vulkan_encode_VkOffset3D(writer, &record->dstOffset);
	vulkan_encode_VkExtent3D(writer, &record->extent);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}

/*
 * Decodes VkImageResolve independently of native structure padding.
 */
void
vulkan_decode_VkImageResolve(
	struct vulkan_reader *reader,
	VkImageResolve *record)
{
	/* Preserves the first failed field without reading later application storage. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Preserves declared field order using explicit protocol widths and nested encoders. */
	vulkan_decode_VkImageSubresourceLayers(reader, &record->srcSubresource);
	vulkan_decode_VkOffset3D(reader, &record->srcOffset);
	vulkan_decode_VkImageSubresourceLayers(reader, &record->dstSubresource);
	vulkan_decode_VkOffset3D(reader, &record->dstOffset);
	vulkan_decode_VkExtent3D(reader, &record->extent);

	/* Succeeded: every selected field has its specified wire representation. */
	return;
}
