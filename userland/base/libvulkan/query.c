/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Retrieves query availability without blocking host event producers or overwriting gaps.
 */

#include <string.h>
#include "sync-internal.h"

/* One query pool retains the result cardinality used to preserve unavailable outputs. */
struct vulkan_query_pool {
	struct vulkan_object object;
	uint32_t query_count;
	uint32_t result_count;
};

static struct vulkan_query_pool *query_pool_object(VkQueryPool pool);
static VkResult query_fetch(struct VkDevice_T *device, struct vulkan_query_pool *pool, uint32_t first, uint32_t count, size_t packed_stride, VkQueryResultFlags flags, struct vulkan_reader *reader);
static VkBool32 query_available(const struct vulkan_reader *reader, uint32_t count, size_t packed_stride, size_t width);
static void query_copy(const struct vulkan_reader *reader, uint32_t count, uint32_t results, size_t packed_stride, size_t width, VkDeviceSize stride, VkQueryResultFlags flags, void *destination);
static uint64_t query_word(const uint8_t *bytes, size_t width);

/*
 * Creates a native query pool and retains its standard per-query result shape.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateQueryPool(
	VkDevice device,
	const VkQueryPoolCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkQueryPool *pQueryPool)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	struct vulkan_query_pool *pool;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;
	VkQueryPipelineStatisticFlags statistics;
	uint64_t present;
	uint64_t returned;
	uint64_t handle;

	/* Allocate one pool with its own creation callbacks before requesting native state. */
	owner = vulkan_device(device);
	status = vulkan_object_alloc(sizeof(*pool), sizeof(uint64_t), VULKAN_OBJECT_QUERY_POOL, &owner->object, owner->object.context, pAllocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Retain the number of native result words rather than a demo-specific shape. */
	pool = (struct vulkan_query_pool *)object;
	pool->query_count = pCreateInfo->queryCount;
	pool->result_count = 1;
	if (pCreateInfo->queryType == VK_QUERY_TYPE_PIPELINE_STATISTICS) {
		/* Pipeline statistics produce one result for each selected standard flag. */
		pool->result_count = 0;
		statistics = pCreateInfo->pipelineStatistics;
		while (statistics != 0) {
			/* Count the low selected statistic before advancing toward the next one. */
			if ((statistics & 1U) != 0)
				pool->result_count++;

			/* Standard result ordering follows increasing statistic bit order. */
			statistics >>= 1;
		}
	}

	/* Reserve a never-reused query-pool identity before native creation. */
	status = vulkan_object_reserve_id(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Encode the entire core1.0 create record without host structure padding. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateQueryPool);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u32(&writer, pCreateInfo->flags);
	vulkan_write_u32(&writer, pCreateInfo->queryType);
	vulkan_write_u32(&writer, pCreateInfo->queryCount);
	vulkan_write_u32(&writer, pCreateInfo->pipelineStatistics);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, object->wire_id);
	status = vulkan_command_execute(owner->object.context, &writer, 24, &reader, VK_TRUE);
	if (status != VK_SUCCESS) {
		status = vulkan_reply_finish(owner->object.context, &reader, status);
		vulkan_writer_finish(&writer);
		vulkan_object_free(object);
		return status;
	}

	/* Verify native output identity before exposing a local public handle. */
	present = vulkan_read_u64(&reader);
	returned = vulkan_read_u64(&reader);
	status = reader.error;
	if (present != 1)
		status = VK_ERROR_DEVICE_LOST;

	/* A different wire identity cannot be substituted for the allocated pool. */
	if (returned != object->wire_id)
		status = VK_ERROR_DEVICE_LOST;

	/* Share malformed creation with every later local synchronization observer. */
	if (status == VK_ERROR_DEVICE_LOST)
		__atomic_store_n(&owner->object.context->error, status, __ATOMIC_RELEASE);

	/* Native creation already succeeded, so ordinary destruction owns rollback. */
	vulkan_reader_finish(&reader);
	vulkan_writer_finish(&writer);
	handle = vulkan_nondispatchable_handle(object);
	if (status != VK_SUCCESS) {
		vkDestroyQueryPool(device, (VkQueryPool)(uintptr_t)handle, pAllocator);
		return status;
	}

	/* Add the complete pool to its device's ordinary child ownership list. */
	status = vulkan_object_publish(object);
	if (status != VK_SUCCESS) {
		vkDestroyQueryPool(device, (VkQueryPool)(uintptr_t)handle, pAllocator);
		return status;
	}

	/* Succeeded: expose the fully created query pool without altering other outputs. */
	*pQueryPool = (VkQueryPool)(uintptr_t)handle;
	return VK_SUCCESS;
}

/*
 * Destroys an externally synchronized query pool and its saved allocation.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyQueryPool(
	VkDevice device,
	VkQueryPool queryPool,
	const VkAllocationCallbacks *pAllocator)
{
	struct VkDevice_T *owner;
	struct vulkan_query_pool *pool;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;

	/* The destruction callback may carry compatible but different application userdata. */
	owner = vulkan_device(device);
	pool = query_pool_object(queryPool);
	if (pool == NULL)
		return;

	/* Withdraw the native pool without sending application allocator pointers. */
	vulkan_writer_init_for_object(&writer, &pool->object);
	if (pAllocator != NULL) {
		writer.allocator.callbacks = *pAllocator;
		writer.allocator.has_callbacks = VK_TRUE;
	}

	/* Encode native destruction after selecting this call's command allocator. */
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkDestroyQueryPool);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, pool->object.wire_id);
	vulkan_write_u64(&writer, 0);
	status = vulkan_command_execute(owner->object.context, &writer, 4, &reader, VK_FALSE);
	if (status == VK_ERROR_DEVICE_LOST) {
		/* Preserve transport loss for later software completion checks. */
		pthread_mutex_lock(&owner->mutex);

		vulkan_sync_device_error(owner, status);

		pthread_mutex_unlock(&owner->mutex);
	}

	/* Void destruction consumes local ownership even after device loss. */
	vulkan_reader_finish(&reader);
	vulkan_writer_finish(&writer);
	vulkan_object_free_with_allocator(&pool->object, pAllocator);

	/* Succeeded: this query pool no longer retains local ownership. */
	return;
}

/*
 * Copies available query results while preserving unavailable values and stride padding.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetQueryPoolResults(
	VkDevice device,
	VkQueryPool queryPool,
	uint32_t firstQuery,
	uint32_t queryCount,
	size_t dataSize,
	void *pData,
	VkDeviceSize stride,
	VkQueryResultFlags flags)
{
	struct VkDevice_T *owner;
	struct vulkan_query_pool *pool;
	struct vulkan_reader reader;
	VkQueryResultFlags native_flags;
	VkResult status;
	VkBool32 available;
	size_t width;
	size_t output_stride;
	size_t packed_stride;
	size_t required;

	/* Always obtain availability internally without changing the caller's output shape. */
	owner = vulkan_device(device);
	pool = query_pool_object(queryPool);
	width = 4;
	if ((flags & VK_QUERY_RESULT_64_BIT) != 0)
		width = 8;

	/* Vulkan1.0 statistics are bounded by their 32-bit flag set. */
	packed_stride = ((size_t)pool->result_count + 1) * width;
	output_stride = (size_t)pool->result_count * width;
	if ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0)
		output_stride += width;

	/* Reject arithmetic that cannot describe the caller's standard output buffer. */
	if (stride > SIZE_MAX)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* A nonempty output must leave room for the final query's actual result words. */
	required = output_stride;
	if (queryCount > 1) {
		/* Avoid multiplying an untrusted stride before checking the buffer bound. */
		if ((size_t)stride > (SIZE_MAX - required) / (queryCount - 1))
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Padding belongs to the application and is never included in copied words. */
		required += (size_t)stride * (queryCount - 1);
	}

	/* Valid callers provide enough bytes for the requested query records. */
	if (queryCount != 0) {
		/* Do not write beyond the explicit host buffer on an inconsistent request. */
		if (required > dataSize)
			return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* A renderer-thread wait could block the SetEvent or Submit that satisfies it. */
	native_flags = flags & ~VK_QUERY_RESULT_WAIT_BIT;
	native_flags |= VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
	for (;;) {
		/* Each transaction is nonblocking with respect to actual GPU query completion. */
		status = query_fetch(owner, pool, firstQuery, queryCount, packed_stride, native_flags, &reader);
		if (status < 0) {
			vulkan_reader_finish(&reader);
			return status;
		}

		/* PARTIAL may report success before availability, so inspect every availability word. */
		available = query_available(&reader, queryCount, packed_stride, width);
		if ((flags & VK_QUERY_RESULT_WAIT_BIT) == 0)
			break;

		/* A requested wait ends only when all queries have their final values. */
		if (available)
			break;

		/* Release response storage and all locks before another producer can run. */
		vulkan_reader_finish(&reader);
		status = vulkan_sync_pause(UINT64_C(1000000));
		if (status != VK_SUCCESS)
			return status;
	}

	/* Preserve unavailable results unless the caller explicitly accepts partial values. */
	query_copy(&reader, queryCount, pool->result_count, packed_stride, width, stride, flags, pData);
	vulkan_reader_finish(&reader);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the requested query values or explicitly accepted partial values are copied. */
	return VK_SUCCESS;
}

/* Resolve one standard query-pool handle through the common object representation. */
static struct vulkan_query_pool *
query_pool_object(
	VkQueryPool pool)
{
	struct vulkan_object *object;

	/* Keep local pointer bits out of renderer request encoding. */
	object = vulkan_nondispatchable_object((uint64_t)(uintptr_t)pool);

	/* Succeeded: every pool allocation begins with its common ownership object. */
	return (struct vulkan_query_pool *)object;
}

/* Read packed native records with availability and no renderer-thread GPU wait. */
static VkResult
query_fetch(
	struct VkDevice_T *device,
	struct vulkan_query_pool *pool,
	uint32_t first,
	uint32_t count,
	size_t packed_stride,
	VkQueryResultFlags flags,
	struct vulkan_reader *reader)
{
	struct vulkan_writer writer;
	VkResult status;
	uint64_t returned;
	size_t bytes;

	/* Leave the caller an empty releasable reply even on pre-encoding failure. */
	vulkan_reader_init(reader, NULL, 0);
	if (count > (SIZE_MAX - 16) / packed_stride)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The host output is tightly packed rather than inheriting application stride holes. */
	bytes = count * packed_stride;
	vulkan_writer_init_for_object(&writer, &pool->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkGetQueryPoolResults);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u64(&writer, pool->object.wire_id);
	vulkan_write_u32(&writer, first);
	vulkan_write_u32(&writer, count);
	vulkan_write_u64(&writer, bytes);
	vulkan_write_u64(&writer, bytes);
	vulkan_write_u64(&writer, packed_stride);
	vulkan_write_u32(&writer, flags);
	status = vulkan_command_execute(device->object.context, &writer, 16 + bytes, reader, VK_TRUE);
	if (status < 0) {
		status = vulkan_reply_finish(device->object.context, reader, status);
		vulkan_writer_finish(&writer);
		return status;
	}

	/* Even NOT_READY carries one complete availability-bearing response array. */
	vulkan_writer_finish(&writer);
	returned = vulkan_read_u64(reader);
	if (reader->error != VK_SUCCESS) {
		status = vulkan_reply_finish(device->object.context, reader, reader->error);
		return status;
	}

	/* A truncated or mismatched array is transport loss, not unavailable GPU work. */
	if (returned != bytes) {
		__atomic_store_n(&device->object.context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
		return VK_ERROR_DEVICE_LOST;
	}

	/* Checked scalar reads leave cursor within bytes; verify the complete raw payload. */
	if (bytes > reader->bytes - reader->cursor) {
		__atomic_store_n(&device->object.context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
		return VK_ERROR_DEVICE_LOST;
	}

	/* Succeeded: preserve native SUCCESS or NOT_READY with independently owned records. */
	return status;
}

/* Test every final availability word without exposing unfinished output to the caller. */
static VkBool32
query_available(
	const struct vulkan_reader *reader,
	uint32_t count,
	size_t packed_stride,
	size_t width)
{
	uint32_t index;
	uint64_t available;
	const uint8_t *record;

	/* Availability occupies the final scalar of every tightly packed host record. */
	for (index = 0; index < count; index++) {
		record = reader->data + reader->cursor + (size_t)index * packed_stride;
		available = query_word(record + packed_stride - width, width);
		if (available == 0)
			return VK_FALSE;
	}

	/* Succeeded: every query has a final GPU-produced result. */
	return VK_TRUE;
}

/* Copy only the words the standard API permits this call to modify. */
static void
query_copy(
	const struct vulkan_reader *reader,
	uint32_t count,
	uint32_t results,
	size_t packed_stride,
	size_t width,
	VkDeviceSize stride,
	VkQueryResultFlags flags,
	void *destination)
{
	uint32_t index;
	uint32_t field;
	uint32_t word32;
	uint64_t word64;
	uint64_t available;
	const uint8_t *record;
	uint8_t *output;
	VkBool32 copy_results;

	/* Preserve the caller's stride and every byte between actual result fields. */
	for (index = 0; index < count; index++) {
		record = reader->data + reader->cursor + (size_t)index * packed_stride;
		output = (uint8_t *)destination + (size_t)index * (size_t)stride;
		available = query_word(record + packed_stride - width, width);
		copy_results = VK_FALSE;
		if (available != 0)
			copy_results = VK_TRUE;

		/* Partial results are written only when explicitly requested by the caller. */
		if ((flags & VK_QUERY_RESULT_PARTIAL_BIT) != 0)
			copy_results = VK_TRUE;

		/* Unavailable final-result words retain the application's original contents. */
		if (copy_results) {
			/* Statistics retain increasing-bit order and the selected 32/64-bit width. */
			for (field = 0; field < results; field++) {
				word64 = query_word(record + (size_t)field * width, width);
				if (width == 8) {
					memcpy(output + (size_t)field * width, &word64, width);
				} else {
					word32 = (uint32_t)word64;
					memcpy(output + (size_t)field * width, &word32, width);
				}
			}
		}

		/* Availability is appended only when it belongs to the public output contract. */
		if ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0) {
			/* Preserve the caller's selected scalar width without alignment assumptions. */
			if (width == 8) {
				memcpy(output + (size_t)results * width, &available, width);
			} else {
				word32 = (uint32_t)available;
				memcpy(output + (size_t)results * width, &word32, width);
			}
		}
	}

	/* Succeeded: unavailable values and padding have not been overwritten. */
	return;
}

/* Decode an opaque native result scalar from the pinned little-endian renderer ABI. */
static uint64_t
query_word(
	const uint8_t *bytes,
	size_t width)
{
	uint64_t word;
	size_t index;

	/* The selected renderer and guest ABI exchange 32-bit or 64-bit result words. */
	word = 0;
	for (index = 0; index < width; index++) {
		word |= (uint64_t)bytes[index] << (index * 8);
	}

	/* Succeeded: preserve the exact unsigned query bits in native scalar form. */
	return word;
}
