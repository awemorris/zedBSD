/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of libvulkan's result status query pools (ws083-p008).
 *
 * query.c runs against a stand-in transport that keeps every request and
 * answers vkGetQueryPoolResults with records built here by hand: the
 * creation of a VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR pool, the status of
 * each query with VK_QUERY_RESULT_WITH_STATUS_BIT_KHR in 32 and 64 bits
 * (ERROR, COMPLETE, and 0 for an unavailable query with VK_NOT_READY), the
 * caller's stride padding kept, and an occlusion pool unchanged.
 */

#include "sync-internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The session the device belongs to. */
static struct vulkan_context context;

/* The logical device the pools are created on. */
static struct VkDevice_T device;

/* The last request the transport saw, copied out of the library's writer. */
static uint8_t request[4096];

/* The size of the last request, and the read position in it. */
static size_t request_bytes;
static size_t request_cursor;

/* The reply being built for the current transaction, and its size. */
static uint8_t reply[4096];
static size_t reply_bytes;

/*
 * What the next vkGetQueryPoolResults answers: the native result, and for
 * each query its result word and its availability.
 */
static uint32_t results_status;
static uint64_t results_values[4];
static uint64_t results_available[4];

static void put32(uint32_t value);
static void put64(uint64_t value);
static uint32_t take32(void);
static uint64_t take64(void);
static VkQueryPool pool_create(VkQueryType type, uint32_t count);
static void test_create(void);
static void test_status_32(void);
static void test_status_64(void);
static void test_occlusion(void);

/*
 * The stand-in transport: keeps the request and answers with a hand-built reply.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *ctx,
	const struct vulkan_writer *writer,
	size_t capacity,
	struct vulkan_reader *reader)
{
	uint64_t identity;
	uint64_t bytes;
	uint64_t stride;
	uint32_t opcode;
	uint32_t count;
	uint32_t flags;
	uint32_t index;
	size_t width;

	/* Every transaction belongs to the one session. */
	assert(ctx == &context);
	assert(writer->bytes <= sizeof(request));
	memcpy(request, writer->data, writer->bytes);
	request_bytes = writer->bytes;
	request_cursor = 0;

	/* Every reply starts with the opcode it answers. */
	opcode = take32();
	(void)take32();
	reply_bytes = 0;
	put32(opcode);

	/* Builds the answer of each command by hand. */
	switch (opcode) {
	case GPU_OP_CREATE_QUERY_POOL:
		/* The created pool's identity, the last double word of the request. */
		memcpy(&identity, writer->data + writer->bytes - 8, 8);
		put32(VK_SUCCESS);
		put64(1);
		put64(identity);
		break;
	case GPU_OP_GET_QUERY_POOL_RESULTS:
		/* [device][pool][first][count][dataSize][bytes][stride][flags]: the packed records asked for. */
		(void)take64();
		(void)take64();
		(void)take32();
		count = take32();
		(void)take64();
		bytes = take64();
		stride = take64();
		flags = take32();
		width = 4;
		if ((flags & VK_QUERY_RESULT_64_BIT) != 0)
			width = 8;
		assert((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0);
		assert(stride == 2 * width);
		assert(bytes == count * stride);
		put32(results_status);
		put64(bytes);
		for (index = 0; index < count; index++) {
			/* The result word, then the availability, in the asked width. */
			if (width == 8) {
				put64(results_values[index]);
				put64(results_available[index]);
			} else {
				put32((uint32_t)results_values[index]);
				put32((uint32_t)results_available[index]);
			}
		}
		break;
	default:
		/* A destroy is answered with its opcode alone. */
		break;
	}

	/* Hands libvulkan its copy. */
	assert(reply_bytes <= capacity);
	vulkan_reader_init(reader, NULL, 0);
	reader->data = malloc(reply_bytes);
	assert(reader->data != NULL);
	memcpy(reader->data, reply, reply_bytes);
	reader->bytes = reply_bytes;
	return VK_SUCCESS;
}

/* The synchronization layer's device error, which no scenario here reaches. */
void
vulkan_sync_device_error(
	struct VkDevice_T *owner,
	VkResult status)
{
	/* Not reached. */
	(void)owner;
	(void)status;
	assert(0);
}

/* The synchronization layer's pause between waits, which no scenario here reaches. */
VkResult
vulkan_sync_pause(
	uint64_t nanoseconds)
{
	/* Not reached. */
	(void)nanoseconds;
	assert(0);
	return VK_SUCCESS;
}

/* Runs every scenario and reports the result on one line. */
int
main(void)
{
	/* A session and the device the pools belong to. */
	memset(&context, 0, sizeof(context));
	context.fd = -1;
	context.video_h264 = VK_TRUE;
	memset(&device, 0, sizeof(device));
	device.object.kind = VULKAN_OBJECT_DEVICE;
	device.object.context = &context;
	device.object.wire_id = 0x2222;

	/* The scenarios. */
	test_create();
	test_status_32();
	test_status_64();
	test_occlusion();

	/* Every scenario passed. */
	printf("ws083 libvulkan result status host test PASS\n");
	return 0;
}

/* Appends one little-endian word to the reply. */
static void
put32(
	uint32_t value)
{
	/* The protocol is little-endian. */
	reply[reply_bytes++] = (uint8_t)value;
	reply[reply_bytes++] = (uint8_t)(value >> 8);
	reply[reply_bytes++] = (uint8_t)(value >> 16);
	reply[reply_bytes++] = (uint8_t)(value >> 24);
}

/* Appends one little-endian double word to the reply. */
static void
put64(
	uint64_t value)
{
	/* The low word first. */
	put32((uint32_t)value);
	put32((uint32_t)(value >> 32));
}

/* Reads the next word of the last request. */
static uint32_t
take32(void)
{
	uint32_t value;

	/* The request must hold the word. */
	assert(request_cursor + 4 <= request_bytes);
	value = (uint32_t)request[request_cursor] |
	        ((uint32_t)request[request_cursor + 1] << 8) |
	        ((uint32_t)request[request_cursor + 2] << 16) |
	        ((uint32_t)request[request_cursor + 3] << 24);
	request_cursor += 4;

	/* Reports the word. */
	return value;
}

/* Reads the next double word of the last request. */
static uint64_t
take64(void)
{
	uint64_t low;
	uint64_t high;

	/* The low word first. */
	low = take32();
	high = take32();

	/* Reports the double word. */
	return low | (high << 32);
}

/* Creates a pool of a type and a count, and checks the request's type and count. */
static VkQueryPool
pool_create(
	VkQueryType type,
	uint32_t count)
{
	VkQueryPoolCreateInfo info;
	VkQueryPool pool;
	VkResult status;

	/* The create info, with no profile chained (the executor does not read it). */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	info.queryType = type;
	info.queryCount = count;
	status = vkCreateQueryPool(&device, &info, NULL, &pool);
	assert(status == VK_SUCCESS);

	/* [opcode][reply][device][present][sType][pNext][flags][queryType][queryCount]... */
	request_cursor = 8;
	assert(take64() == device.object.wire_id);
	assert(take64() == 1);
	assert(take32() == VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
	assert(take64() == 0);
	assert(take32() == 0);
	assert(take32() == (uint32_t)type);
	assert(take32() == count);

	/* The pool. */
	return pool;
}

/* A result status pool is created with its type on the wire. */
static void
test_create(void)
{
	VkQueryPool pool;

	/* The type number travels as it is. */
	pool = pool_create(VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR, 2);
	assert((uint32_t)VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR == 1000023000U);
	vkDestroyQueryPool(&device, pool, NULL);
}

/* 32-bit statuses: ERROR, COMPLETE, and NOT_READY (0) for an unavailable query, with the stride's padding kept. */
static void
test_status_32(void)
{
	VkQueryPool pool;
	VkResult status;
	uint32_t data[9];

	/* Three queries: a skipped decode, a decode that ran, one not yet ended. */
	pool = pool_create(VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR, 3);
	results_status = VK_NOT_READY;
	results_values[0] = (uint32_t)VK_QUERY_RESULT_STATUS_ERROR_KHR;
	results_available[0] = 1;
	results_values[1] = VK_QUERY_RESULT_STATUS_COMPLETE_KHR;
	results_available[1] = 1;
	results_values[2] = 0x77;
	results_available[2] = 0;
	memset(data, 0xaa, sizeof(data));
	status = vkGetQueryPoolResults(&device, pool, 0, 3, sizeof(data), data, 12, VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
	assert(status == VK_NOT_READY);

	/* Each status at its stride; the padding between untouched. */
	assert((int32_t)data[0] == VK_QUERY_RESULT_STATUS_ERROR_KHR);
	assert(data[1] == 0xaaaaaaaaU && data[2] == 0xaaaaaaaaU);
	assert((int32_t)data[3] == VK_QUERY_RESULT_STATUS_COMPLETE_KHR);
	assert(data[4] == 0xaaaaaaaaU && data[5] == 0xaaaaaaaaU);
	assert((int32_t)data[6] == VK_QUERY_RESULT_STATUS_NOT_READY_KHR);
	assert(data[7] == 0xaaaaaaaaU && data[8] == 0xaaaaaaaaU);
	vkDestroyQueryPool(&device, pool, NULL);
}

/* 64-bit statuses, all available: SUCCESS and the sign of ERROR kept. */
static void
test_status_64(void)
{
	VkQueryPool pool;
	VkResult status;
	int64_t data[2];

	/* Two queries that ended: one decode skipped, one that ran. */
	pool = pool_create(VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR, 2);
	results_status = VK_SUCCESS;
	results_values[0] = UINT64_C(0xffffffffffffffff);
	results_available[0] = 1;
	results_values[1] = 1;
	results_available[1] = 1;
	memset(data, 0, sizeof(data));
	status = vkGetQueryPoolResults(&device, pool, 0, 2, sizeof(data), data, 8, VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_64_BIT);
	assert(status == VK_SUCCESS);
	assert(data[0] == VK_QUERY_RESULT_STATUS_ERROR_KHR);
	assert(data[1] == VK_QUERY_RESULT_STATUS_COMPLETE_KHR);
	vkDestroyQueryPool(&device, pool, NULL);
}

/* An occlusion pool is unchanged: an unavailable query's result is not written, the availability is. */
static void
test_occlusion(void)
{
	VkQueryPool pool;
	VkResult status;
	uint32_t data[4];

	/* One query with 5 samples, one unavailable. */
	pool = pool_create(VK_QUERY_TYPE_OCCLUSION, 2);
	results_status = VK_NOT_READY;
	results_values[0] = 5;
	results_available[0] = 1;
	results_values[1] = 9;
	results_available[1] = 0;
	memset(data, 0xaa, sizeof(data));
	status = vkGetQueryPoolResults(&device, pool, 0, 2, sizeof(data), data, 8, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	assert(status == VK_NOT_READY);
	assert(data[0] == 5 && data[1] == 1);
	assert(data[2] == 0xaaaaaaaaU && data[3] == 0);
	vkDestroyQueryPool(&device, pool, NULL);
}
