/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exercises real marshaling with a stateful native sync peer and concurrent producers. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sync-internal.h"

#define TEST_NATIVE_OBJECTS 256U

/* The mock peer records native state independently of library software payloads. */
struct native_object {
	uint32_t kind;
	uint32_t signaled;
	uint32_t pending;
};

/* The mock transport mutex protects all peer state during concurrent event tests. */
struct mock_state {
	struct native_object objects[TEST_NATIVE_OBJECTS];
	uint32_t calls[64];
	uint32_t waits;
	uint64_t wait_ids[16];
	uint32_t wait_stages[16];
	uint32_t sparse_ranges;
	uint32_t event_blocked;
	uint32_t query_complete;
	uint32_t malformed_query;
	uint32_t fail_opcode;
	VkResult fail_result;
};

/* Test cases own this bounded peer for the complete focused process lifetime. */
static struct mock_state peer;

/* Every transport call serializes the fixture's independent native state. */
static pthread_mutex_t peer_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Callback accounting is accessed only by serial object-lifecycle cases. */
static uint32_t allocations;
static uint32_t deallocations;

/* The event producer borrows its device/event only until pthread_join completes. */
struct event_producer {
	VkDevice device;
	VkEvent event;
};

static void expect(uint64_t actual, uint64_t expected, const char *meaning);
static void expect_status(VkResult actual, VkResult expected, const char *meaning);
static void expect_wire(struct vulkan_reader *request, size_t bytes);
static uint64_t native_create(struct vulkan_reader *request, uint32_t opcode);
static void native_submit(struct vulkan_reader *request, uint32_t opcode, VkResult status);
static void native_query(struct vulkan_reader *request, struct vulkan_writer *response, VkResult *status);
static void native_sparse_ranges(struct vulkan_reader *request);
static void *allocate_object(void *private_data, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void release_object(void *private_data, void *allocation);
static void *signal_event(void *private_data);
static void test_fences(VkDevice device, VkQueue queue);
static void test_semaphores(VkDevice device, VkQueue queue);
static void test_events_and_waits(VkDevice device, VkQueue queue);
static void test_queries(VkDevice device);
static void test_sparse(VkDevice device, VkQueue queue);
static void test_context_loss(void);

/*
 * Runs independent native-peer state transitions through the production API family.
 */
int
main(
	void)
{
	struct vulkan_context context;
	struct VkDevice_T device;
	struct VkQueue_T queue;
	struct VkQueue_T *queues[1];
	VkResult status;
	uint32_t index;

	/* Construct only the surrounding device/queue objects outside the owned API family. */
	memset(&context, 0, sizeof(context));
	memset(&device, 0, sizeof(device));
	memset(&queue, 0, sizeof(queue));
	device.object.context = &context;
	device.object.wire_id = 1000;
	queue.object.context = &context;
	queue.object.wire_id = 1001;
	queue.device = &device;
	queues[0] = &queue;
	device.queues = queues;
	device.queue_count = 1;
	pthread_mutex_init(&device.mutex, NULL);
	pthread_mutex_init(&queue.mutex, NULL);

	/* Cover native state, acquired payloads, failed enqueue rollback and sparse records. */
	test_fences((VkDevice)&device, (VkQueue)&queue);
	test_semaphores((VkDevice)&device, (VkQueue)&queue);
	test_sparse((VkDevice)&device, (VkQueue)&queue);
	test_events_and_waits((VkDevice)&device, (VkQueue)&queue);
	test_queries((VkDevice)&device);

	/* Public device-idle must use ordinary fence completion, never the fatal host opcode. */
	status = vkDeviceWaitIdle((VkDevice)&device);
	expect_status(status, VK_SUCCESS, "device idle covers ordinary queues");
	expect(peer.calls[19], 0, "no native QueueWaitIdle opcode");
	expect(peer.calls[20], 0, "no native DeviceWaitIdle opcode");
	expect(peer.calls[39], 0, "no blocking native WaitForFences opcode");

	/* Loss in one family must invalidate other families and completed software payloads. */
	test_context_loss();

	/* Every created native object and every callback allocation must be released. */
	for (index = 0; index < TEST_NATIVE_OBJECTS; index++) {
		expect(peer.objects[index].kind, 0, "native object destroyed");
	}

	/* Object callbacks remain paired even after native creation failure. */
	expect(allocations, deallocations, "callback ownership balanced");
	pthread_mutex_destroy(&queue.mutex);
	pthread_mutex_destroy(&device.mutex);
	puts("libvulkan sync: PASS (payload transitions, rollback, unlocked waits, query preservation)");

	/* Succeeded: real production API encoders preserved native and local semantics. */
	return 0;
}

/*
 * Supplies a stateful protocol peer while retaining the production wire encoder/decoder.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reply)
{
	struct vulkan_reader request;
	struct vulkan_writer response;
	VkResult status;
	uint32_t opcode;
	uint32_t count;
	uint32_t index;
	uint64_t identifier;
	uint64_t identity;
	VkBool32 has_result;

	/* Transport loss remains distinct from a valid negative native Vulkan result. */
	status = __atomic_load_n(&context->error, __ATOMIC_ACQUIRE);
	if (status != VK_SUCCESS)
		return status;

	/* Protect the independent peer while the event-producing host thread runs. */
	pthread_mutex_lock(&peer_mutex);

	vulkan_reader_init(&request, writer->data, writer->bytes);
	opcode = vulkan_read_u32(&request);
	identity = vulkan_read_u32(&request);
	expect(identity, 1, "ordinary response-request flag");
	if (opcode >= 64)
		exit(1);

	/* Record native calls so tests can detect forbidden or redundant operations. */
	peer.calls[opcode]++;
	status = VK_SUCCESS;
	if (peer.fail_opcode == opcode) {
		status = peer.fail_result;
		peer.fail_opcode = 0;
	}

	/* Build one independently owned reply with the command's exact output shape. */
	vulkan_writer_init(&response);
	vulkan_write_u32(&response, opcode);
	has_result = VK_TRUE;

	/* Model native object state without consulting any production software-signaled bit. */
	switch (opcode) {
	case 35:
	case 40:
	case 42:
	case 47:
		/* Native creation receives a reserved output identity, not a local pointer. */
		identifier = native_create(&request, opcode);
		vulkan_write_u32(&response, (uint32_t)status);
		vulkan_write_u64(&response, 1);
		vulkan_write_u64(&response, identifier);
		if (status != VK_SUCCESS)
			memset(&peer.objects[identifier], 0, sizeof(peer.objects[identifier]));
		break;
	case 36:
	case 41:
	case 43:
	case 48:
		/* Destroy consumes one native object and never carries a Vulkan result. */
		identity = vulkan_read_u64(&request);
		expect(identity, 1000, "destroy device identity");
		identifier = vulkan_read_u64(&request);
		identity = vulkan_read_u64(&request);
		expect(identity, 0, "destroy remote allocator is null");
		memset(&peer.objects[identifier], 0, sizeof(peer.objects[identifier]));
		has_result = VK_FALSE;
		break;
	case 37:
		/* Native reset changes all supplied fence objects. */
		identity = vulkan_read_u64(&request);
		expect(identity, 1000, "reset device identity");
		count = vulkan_read_u32(&request);
		identity = vulkan_read_u64(&request);
		expect(identity, count, "reset fence cardinality");
		for (index = 0; index < count; index++) {
			identifier = vulkan_read_u64(&request);
			peer.objects[identifier].signaled = 0;
		}
		break;
	case 38:
		/* A host event can satisfy queued work while the API caller polls unlocked. */
		identity = vulkan_read_u64(&request);
		expect(identity, 1000, "fence status device");
		identifier = vulkan_read_u64(&request);
		if (peer.objects[identifier].pending) {
			/* Completion remains pending until the independent producer sets its event. */
			if (!peer.event_blocked) {
				peer.objects[identifier].pending = 0;
				peer.objects[identifier].signaled = 1;
			}
		}

		/* Native readiness is independent of the library's acquisition payload. */
		if (!peer.objects[identifier].signaled)
			status = VK_NOT_READY;
		break;
	case 44:
	case 45:
	case 46:
		/* Events are shared native state rather than local success stubs. */
		identity = vulkan_read_u64(&request);
		expect(identity, 1000, "event device identity");
		identifier = vulkan_read_u64(&request);
		if (opcode == 45) {
			peer.objects[identifier].signaled = 1;
			peer.event_blocked = 0;
			peer.query_complete = 1;
		} else if (opcode == 46) {
			peer.objects[identifier].signaled = 0;
		} else {
			status = VK_EVENT_RESET;
			if (peer.objects[identifier].signaled)
				status = VK_EVENT_SET;
		}
		break;
	case 18:
	case 34:
		/* The native peer checks wait stage correspondence and applies accepted signals. */
		native_submit(&request, opcode, status);
		break;
	case 49:
		/* Availability and result words are supplied as a padded response byte array. */
		native_query(&request, &response, &status);
		break;
	default:
		/* Idle or blocking-wait opcodes would hide the producer-progress contract. */
		fprintf(stderr, "unexpected native opcode %u\n", opcode);
		exit(1);
	}

	/* Commands without extra output carry only their exact Vulkan result. */
	if (has_result) {
		/* Create and query handlers already supplied their result-bearing output record. */
		if (response.bytes == 4)
			vulkan_write_u32(&response, (uint32_t)status);
	}

	/* Exact request consumption detects missing counts, padding or fields. */
	expect_wire(&request, writer->bytes);
	if (response.bytes > reply_capacity) {
		fprintf(stderr, "reply capacity too small\n");
		exit(1);
	}

	/* The caller owns response bytes after the native transaction unlocks. */
	vulkan_reader_init(reply, response.data, response.bytes);

	pthread_mutex_unlock(&peer_mutex);

	/* Succeeded: native Vulkan status is carried in the reply, not transport status. */
	return VK_SUCCESS;
}

/*
 * Completes the absent presentation subsystem without submitting hidden GPU work.
 */
VkResult
vulkan_wsi_queue_idle(
	struct VkQueue_T *queue)
{
	/* This focused peer owns no native display or asynchronous presentation worker. */
	(void)queue;

	/* Succeeded: no WSI work remains outside the tested native queue. */
	return VK_SUCCESS;
}

/*
 * Completes the absent device presentation subsystem.
 */
VkResult
vulkan_wsi_device_idle(
	struct VkDevice_T *device)
{
	/* Device-idle still exercises the real per-queue private fence implementation. */
	(void)device;

	/* Succeeded: this fixture has no separate native display state. */
	return VK_SUCCESS;
}

/* Fail a semantic assertion without folding the compared values into a Boolean expression. */
static void
expect(
	uint64_t actual,
	uint64_t expected,
	const char *meaning)
{
	/* Expose the observed and required values at one debugger-friendly failure point. */
	if (actual != expected) {
		fprintf(stderr, "%s: observed=%llu required=%llu\n", meaning, (unsigned long long)actual, (unsigned long long)expected);
		exit(1);
	}

	/* Succeeded: this independently stated semantic expectation holds. */
	return;
}

/* Keep signed Vulkan result diagnostics distinct from native scalar comparisons. */
static void
expect_status(
	VkResult actual,
	VkResult expected,
	const char *meaning)
{
	/* A different status changes the public API's observable outcome. */
	if (actual != expected) {
		fprintf(stderr, "%s: observed=%d required=%d\n", meaning, actual, expected);
		exit(1);
	}

	/* Succeeded: the ordinary Vulkan result matches the semantic expectation. */
	return;
}

/* Check full protocol consumption after the independent native peer decoded a command. */
static void
expect_wire(
	struct vulkan_reader *request,
	size_t bytes)
{
	/* A correct primitive decode must also consume every command field exactly once. */
	expect_status(request->error, VK_SUCCESS, "wire decoding");
	expect(request->cursor, bytes, "complete wire command consumed");

	/* Succeeded: no command field was missing or silently ignored. */
	return;
}

/* Decode flags-only sync creation and the additional query-pool fields independently. */
static uint64_t
native_create(
	struct vulkan_reader *request,
	uint32_t opcode)
{
	uint64_t identity;
	uint64_t identifier;
	uint32_t structure;
	uint32_t flags;
	uint32_t extra;

	/* Device, required create-info pointer and pNext use distinct wire widths. */
	identity = vulkan_read_u64(request);
	expect(identity, 1000, "create device identity");
	identity = vulkan_read_u64(request);
	expect(identity, 1, "required create-info pointer");
	structure = vulkan_read_u32(request);
	identity = vulkan_read_u64(request);
	expect(identity, 0, "selected core creation has no pNext");
	flags = vulkan_read_u32(request);

	/* Query-pool creation additionally preserves type, count and statistics flags. */
	if (opcode == 47) {
		expect(structure, VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, "query structure");
		extra = vulkan_read_u32(request);
		expect(extra, VK_QUERY_TYPE_OCCLUSION, "query type");
		extra = vulkan_read_u32(request);
		expect(extra, 2, "query count");
		extra = vulkan_read_u32(request);
		expect(extra, 0, "query statistics");
	}

	/* Application allocator callbacks never cross into the native renderer. */
	identity = vulkan_read_u64(request);
	expect(identity, 0, "remote allocator pointer");
	identity = vulkan_read_u64(request);
	expect(identity, 1, "required output pointer");
	identifier = vulkan_read_u64(request);
	if (identifier >= TEST_NATIVE_OBJECTS)
		exit(1);

	/* Independently track each created native object's initial payload. */
	peer.objects[identifier].kind = opcode;
	if (opcode == 35) {
		expect(structure, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, "fence structure");
		if ((flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0)
			peer.objects[identifier].signaled = 1;
	}

	/* Succeeded: the output identity names newly created native peer state. */
	return identifier;
}

/* Decode queue work and model only side effects accepted by the native enqueue. */
static void
native_submit(
	struct vulkan_reader *request,
	uint32_t opcode,
	VkResult status)
{
	uint64_t identity;
	uint64_t identifier;
	uint32_t count;
	uint32_t request_index;
	uint32_t index;
	uint32_t waits;
	uint32_t commands;
	uint32_t signals;
	uint32_t structure;

	/* Every queue request names the ordinary native queue before its record array. */
	identity = vulkan_read_u64(request);
	expect(identity, 1001, "queue native identity");
	count = vulkan_read_u32(request);
	identity = vulkan_read_u64(request);
	expect(identity, count, "submit record cardinality");
	peer.waits = 0;

	/* Inspect each record in order, including a repeated wait after a native signal. */
	for (request_index = 0; request_index < count; request_index++) {
		structure = vulkan_read_u32(request);
		identity = vulkan_read_u64(request);
		expect(identity, 0, "core queue record has no pNext");
		waits = vulkan_read_u32(request);
		identity = vulkan_read_u64(request);
		expect(identity, waits, "native wait array length");

		/* Native waits must never include a software-only acquisition signal. */
		for (index = 0; index < waits; index++) {
			identifier = vulkan_read_u64(request);
			peer.wait_ids[peer.waits + index] = identifier;
			if (status == VK_SUCCESS)
				peer.objects[identifier].signaled = 0;
		}

		/* Only command submission carries one destination stage per remaining wait. */
		if (opcode == 18) {
			expect(structure, VK_STRUCTURE_TYPE_SUBMIT_INFO, "submit structure");
			identity = vulkan_read_u64(request);
			expect(identity, waits, "native stage-mask cardinality");
			for (index = 0; index < waits; index++) {
				peer.wait_stages[peer.waits + index] = vulkan_read_u32(request);
			}

			/* Actual command buffers would already exist in the independent renderer. */
			commands = vulkan_read_u32(request);
			identity = vulkan_read_u64(request);
			expect(identity, commands, "command-buffer cardinality");
			for (index = 0; index < commands; index++) {
				identity = vulkan_read_u64(request);
				expect(identity, 1002, "resolved command-buffer identity");
			}
		} else {
			expect(structure, VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, "sparse structure");
			native_sparse_ranges(request);
		}

		/* The observed native wait count excludes only already completed software payloads. */
		peer.waits += waits;
		signals = vulkan_read_u32(request);
		identity = vulkan_read_u64(request);
		expect(identity, signals, "signal cardinality");
		for (index = 0; index < signals; index++) {
			identifier = vulkan_read_u64(request);
			if (status == VK_SUCCESS)
				peer.objects[identifier].signaled = 1;
		}
	}

	/* A native enqueue failure must not signal the fence or consume local payloads. */
	identifier = vulkan_read_u64(request);
	if (identifier != 0) {
		/* Completed mock work signals immediately unless an event deliberately blocks it. */
		if (status == VK_SUCCESS) {
			peer.objects[identifier].pending = peer.event_blocked;
			peer.objects[identifier].signaled = 1;
			if (peer.event_blocked)
				peer.objects[identifier].signaled = 0;
		}
	}

	/* Succeeded: the peer has applied only accepted native queue side effects. */
	return;
}

/* Decode sparse fields using independently chosen nonzero offsets and a null-memory unbind. */
static void
native_sparse_ranges(
	struct vulkan_reader *request)
{
	uint32_t family;
	uint32_t count;
	uint32_t flags;
	uint64_t scalar;

	/* This fixture supplies one buffer interval and no opaque or tiled image intervals. */
	for (family = 0; family < 3; family++) {
		count = vulkan_read_u32(request);
		scalar = vulkan_read_u64(request);
		expect(scalar, count, "sparse family array length");
		if (family != 0) {
			expect(count, 0, "absent image binding family");
			continue;
		}

		/* Nonempty sparse tests retain all offsets and null-memory identity. */
		if (count != 0) {
			expect(count, 1, "sparse buffer count");
			scalar = vulkan_read_u64(request);
			expect(scalar, 2000, "sparse buffer wire identity");
			flags = vulkan_read_u32(request);
			expect(flags, 1, "sparse range count");
			scalar = vulkan_read_u64(request);
			expect(scalar, 1, "sparse range cardinality");
			scalar = vulkan_read_u64(request);
			expect(scalar, 4096, "sparse resource offset");
			scalar = vulkan_read_u64(request);
			expect(scalar, 8192, "sparse byte extent");
			scalar = vulkan_read_u64(request);
			expect(scalar, 0, "null memory unbind preserved");
			scalar = vulkan_read_u64(request);
			expect(scalar, 0, "unbind memory offset");
			flags = vulkan_read_u32(request);
			expect(flags, 0, "sparse flags preserved");
			peer.sparse_ranges++;
		}
	}

	/* Succeeded: the independent fixture saw the requested sparse binding geometry. */
	return;
}

/* Return available and unavailable native records, with controlled progress for WAIT tests. */
static void
native_query(
	struct vulkan_reader *request,
	struct vulkan_writer *response,
	VkResult *status)
{
	uint64_t scalar;
	uint64_t bytes;
	uint64_t stride;
	uint32_t first;
	uint32_t count;
	uint32_t flags;
	uint32_t index;
	uint32_t width;
	uint32_t available;

	/* Every request adds availability but must never block the renderer thread. */
	scalar = vulkan_read_u64(request);
	expect(scalar, 1000, "query device identity");
	scalar = vulkan_read_u64(request);
	first = vulkan_read_u32(request);
	count = vulkan_read_u32(request);
	bytes = vulkan_read_u64(request);
	scalar = vulkan_read_u64(request);
	expect(scalar, bytes, "query output cardinality");
	stride = vulkan_read_u64(request);
	flags = vulkan_read_u32(request);
	expect(flags & VK_QUERY_RESULT_WAIT_BIT, 0, "native query never blocks producers");
	expect(flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, "native query returns availability");
	width = 4;
	if ((flags & VK_QUERY_RESULT_64_BIT) != 0)
		width = 8;

	/* Packing must remove caller stride gaps while retaining one result and availability. */
	expect(stride, width * 2, "native query packed stride");
	expect(bytes, count * stride, "native query packed bytes");
	*status = VK_SUCCESS;
	if (!peer.query_complete) {
		/* The second query deliberately remains unavailable until a producer progresses. */
		if (first + count > 1)
			*status = VK_NOT_READY;
	}

	/* PARTIAL may return success before availability; WAIT must still inspect availability. */
	if ((flags & VK_QUERY_RESULT_PARTIAL_BIT) != 0)
		*status = VK_SUCCESS;

	/* Supply an exact response array independently of the application buffer's contents. */
	vulkan_write_u32(response, (uint32_t)*status);
	if (peer.malformed_query == 2U) {
		/* A truncated count must invalidate the namespace before any output is copied. */
		vulkan_write_u32(response, 0U);
		return;
	}

	/* A wrong count differs from a structurally truncated marker. */
	if (peer.malformed_query) {
		vulkan_write_u64(response, bytes + 4);
	} else {
		vulkan_write_u64(response, bytes);
	}

	/* An unavailable record contains garbage results to catch blind response memcpy. */
	for (index = 0; index < count; index++) {
		available = 1;
		if (first + index != 0) {
			/* Completion is controlled by an independent event-producing host thread. */
			if (!peer.query_complete)
				available = 0;
		}

		/* Actual values differ from both sentinels and stride padding in the tests. */
		scalar = 111 + first + index;
		if (!available)
			scalar = 777;

		/* Preserve the requested scalar width in each packed native query record. */
		if (width == 8) {
			vulkan_write_u64(response, scalar);
			vulkan_write_u64(response, available);
		} else {
			vulkan_write_u32(response, (uint32_t)scalar);
			vulkan_write_u32(response, available);
		}
	}

	/* Succeeded: the response distinguishes available results from untouched caller bytes. */
	return;
}

/* Count callback allocations while honoring the object's requested host alignment. */
static void *
allocate_object(
	void *private_data,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	void *allocation;
	int error;

	/* Object creation supplies its original callback userdata and scope. */
	(void)private_data;
	if (scope != VK_SYSTEM_ALLOCATION_SCOPE_OBJECT) {
		/* Temporary encode/reply storage belongs to the same effective callback policy. */
		expect(scope, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND, "callback command allocation scope");
	}
	allocation = NULL;
	error = posix_memalign(&allocation, alignment, bytes);
	if (error != 0)
		return NULL;

	/* Count only storage actually handed to the implementation. */
	allocations++;

	/* Succeeded: the callback retains the matching free obligation. */
	return allocation;
}

/* Return a callback-owned object exactly once. */
static void
release_object(
	void *private_data,
	void *allocation)
{
	/* The fixture's accounting is serial and independent of native command state. */
	(void)private_data;
	if (allocation == NULL)
		return;

	/* Count storage actually released, including compatible destruction userdata. */
	deallocations++;
	if (private_data != NULL)
		(*(uint32_t *)private_data)++;

	/* Return the same callback-owned allocation to its original host allocator. */
	free(allocation);

	/* Succeeded: this application callback has reclaimed its own allocation. */
	return;
}

/* Release a native dependency after the waiting thread has had time to poll it. */
static void *
signal_event(
	void *private_data)
{
	struct event_producer *producer;
	struct timespec delay;
	VkResult status;
	int error;

	/* A separate thread proves that waits do not retain the device/context locks. */
	producer = private_data;
	delay.tv_sec = 0;
	delay.tv_nsec = 15000000;
	error = nanosleep(&delay, NULL);
	if (error != 0)
		exit(1);

	/* SetEvent must progress while another host thread waits for GPU results. */
	status = vkSetEvent(producer->device, producer->event);
	expect_status(status, VK_SUCCESS, "concurrent event producer");

	/* Succeeded: the waiting API can now observe native completion. */
	return NULL;
}

/* Verify initial state, any/all waiting, acquisition completion, resets and callbacks. */
static void
test_fences(
	VkDevice device,
	VkQueue queue)
{
	VkFenceCreateInfo create;
	VkAllocationCallbacks callbacks;
	VkFence fences[2];
	VkFence rejected;
	VkResult status;
	uint32_t observations;
	uint32_t before_allocations;
	uint32_t alternate_frees;

	/* A signaled create flag belongs to the real native fence, not a software shortcut. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	create.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.pfnAllocation = allocate_object;
	callbacks.pfnFree = release_object;
	status = vkCreateFence(device, &create, &callbacks, &fences[0]);
	expect_status(status, VK_SUCCESS, "signaled fence creation");
	status = vkGetFenceStatus(device, fences[0]);
	expect_status(status, VK_SUCCESS, "initial native fence signal");

	/* An ordinary unflagged fence is genuinely unsignaled. */
	create.flags = 0;
	status = vkCreateFence(device, &create, NULL, &fences[1]);
	expect_status(status, VK_SUCCESS, "unsignaled fence creation");
	status = vkGetFenceStatus(device, fences[1]);
	expect_status(status, VK_NOT_READY, "initial native unsignaled fence");
	status = vkWaitForFences(device, 2, fences, VK_FALSE, 0);
	expect_status(status, VK_SUCCESS, "any fence completes without polling all");
	status = vkWaitForFences(device, 2, fences, VK_TRUE, 0);
	expect_status(status, VK_TIMEOUT, "all fences preserves pending payload");

	/* Zero timeout performs one native observation, while finite time expires normally. */
	observations = peer.calls[38];
	status = vkWaitForFences(device, 1, &fences[1], VK_TRUE, 0);
	expect_status(status, VK_TIMEOUT, "zero timeout");
	expect(peer.calls[38] - observations, 1, "one zero-timeout observation");
	status = vkWaitForFences(device, 1, &fences[1], VK_TRUE, UINT64_C(2000000));
	expect_status(status, VK_TIMEOUT, "finite timeout");

	/* Completed image acquisition signals the ordinary fence without native queue work. */
	observations = peer.calls[18];
	status = vulkan_wsi_acquire_signal((struct VkDevice_T *)device, VK_NULL_HANDLE, fences[1]);
	expect_status(status, VK_SUCCESS, "acquisition fence signal");
	status = vkWaitForFences(device, 2, fences, VK_TRUE, 0);
	expect_status(status, VK_SUCCESS, "mixed native and acquired all wait");
	expect(peer.calls[18], observations, "acquisition never queues behind blocked work");
	status = vkResetFences(device, 2, fences);
	expect_status(status, VK_SUCCESS, "native and acquired fence reset");
	status = vkGetFenceStatus(device, fences[1]);
	expect_status(status, VK_NOT_READY, "acquired fence reset reveals native unsignaled state");
	status = vkQueueSubmit(queue, 0, NULL, fences[1]);
	expect_status(status, VK_SUCCESS, "reuse reset acquisition fence natively");
	status = vkGetFenceStatus(device, fences[1]);
	expect_status(status, VK_SUCCESS, "native completion after software payload");

	/* Native allocation failure leaves the caller's output unchanged and frees callbacks. */
	rejected = fences[0];
	before_allocations = allocations - deallocations;
	peer.fail_opcode = 35;
	peer.fail_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	status = vkCreateFence(device, &create, &callbacks, &rejected);
	expect_status(status, VK_ERROR_OUT_OF_DEVICE_MEMORY, "native create failure preserved");
	expect((uint64_t)(uintptr_t)rejected, (uint64_t)(uintptr_t)fences[0], "failed create preserves output");
	expect(allocations - deallocations, before_allocations, "failed create releases every callback allocation");

	/* Compatible destruction callbacks may intentionally select different userdata. */
	alternate_frees = 0;
	callbacks.pUserData = &alternate_frees;
	vkDestroyFence(device, fences[0], &callbacks);
	if (alternate_frees == 0) {
		fprintf(stderr, "compatible destruction userdata was ignored\n");
		exit(1);
	}

	/* Release the remaining ordinary fence after its native completion. */
	vkDestroyFence(device, fences[1], NULL);

	/* Succeeded: native and software fence lifetimes remain distinct and reusable. */
	return;
}

/* Verify native/software wait mixing, failed enqueue rollback and alternating payloads. */
static void
test_semaphores(
	VkDevice device,
	VkQueue queue)
{
	VkSemaphoreCreateInfo create;
	VkSemaphore semaphores[2];
	VkPipelineStageFlags stages[2];
	VkSubmitInfo submit[2];
	struct vulkan_sync *native;
	VkResult status;
	uint64_t native_id;

	/* Each semaphore begins with an independent native unsignaled payload. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	status = vkCreateSemaphore(device, &create, NULL, &semaphores[0]);
	expect_status(status, VK_SUCCESS, "software candidate semaphore");
	status = vkCreateSemaphore(device, &create, NULL, &semaphores[1]);
	expect_status(status, VK_SUCCESS, "native candidate semaphore");
	native = vulkan_sync_object((uint64_t)(uintptr_t)semaphores[1]);
	native_id = native->object.wire_id;

	/* Signal one native semaphore using the production queue path. */
	memset(submit, 0, sizeof(submit));
	submit[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit[0].signalSemaphoreCount = 1;
	submit[0].pSignalSemaphores = &semaphores[1];
	status = vkQueueSubmit(queue, 1, submit, VK_NULL_HANDLE);
	expect_status(status, VK_SUCCESS, "native semaphore signal");
	status = vulkan_wsi_acquire_signal((struct VkDevice_T *)device, semaphores[0], VK_NULL_HANDLE);
	expect_status(status, VK_SUCCESS, "software acquisition semaphore signal");

	/* One submit contains both native and completed waits with different stage masks. */
	stages[0] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	stages[1] = VK_PIPELINE_STAGE_TRANSFER_BIT;
	submit[0].signalSemaphoreCount = 0;
	submit[0].waitSemaphoreCount = 2;
	submit[0].pWaitSemaphores = semaphores;
	submit[0].pWaitDstStageMask = stages;
	peer.fail_opcode = 18;
	peer.fail_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	status = vkQueueSubmit(queue, 1, submit, VK_NULL_HANDLE);
	expect_status(status, VK_ERROR_OUT_OF_DEVICE_MEMORY, "enqueue failure preserves exact result");
	status = vkQueueSubmit(queue, 1, submit, VK_NULL_HANDLE);
	expect_status(status, VK_SUCCESS, "software wait survives failed enqueue");
	expect(peer.waits, 1, "only native wait remains after rollback");
	expect(peer.wait_ids[0], native_id, "remaining native semaphore identity");
	expect(peer.wait_stages[0], VK_PIPELINE_STAGE_TRANSFER_BIT, "stage mask follows remaining native wait");

	/* A previously acquired semaphore can then signal and wait through its native payload. */
	submit[0].waitSemaphoreCount = 0;
	submit[0].signalSemaphoreCount = 1;
	submit[0].pSignalSemaphores = &semaphores[0];
	status = vkQueueSubmit(queue, 1, submit, VK_NULL_HANDLE);
	expect_status(status, VK_SUCCESS, "native signal after acquired wait");
	submit[0].signalSemaphoreCount = 0;
	submit[0].waitSemaphoreCount = 1;
	status = vkQueueSubmit(queue, 1, submit, VK_NULL_HANDLE);
	expect_status(status, VK_SUCCESS, "native wait after acquired payload consumed");
	expect(peer.waits, 1, "native alternation is not accidentally elided");

	/* A multi-record submit consumes acquisition once, then observes its intervening native signal. */
	status = vulkan_wsi_acquire_signal((struct VkDevice_T *)device, semaphores[0], VK_NULL_HANDLE);
	expect_status(status, VK_SUCCESS, "second acquisition payload");
	submit[0].signalSemaphoreCount = 1;
	submit[1] = submit[0];
	submit[1].signalSemaphoreCount = 0;
	status = vkQueueSubmit(queue, 2, submit, VK_NULL_HANDLE);
	expect_status(status, VK_SUCCESS, "multi-submit payload alternation");
	expect(peer.waits, 1, "acquisition consumed only once across records");
	vkDestroySemaphore(device, semaphores[0], NULL);
	vkDestroySemaphore(device, semaphores[1], NULL);

	/* Succeeded: accepted queue transactions consume exactly one completed payload. */
	return;
}

/* Prove a blocked GPU fence does not prevent a concurrent host event producer. */
static void
test_events_and_waits(
	VkDevice device,
	VkQueue queue)
{
	VkEventCreateInfo event_create;
	VkFenceCreateInfo fence_create;
	VkEvent event;
	VkFence fence;
	struct event_producer producer;
	pthread_t thread;
	VkResult status;
	int error;

	/* Event observation must preserve RESET, SET and RESET transitions. */
	memset(&event_create, 0, sizeof(event_create));
	event_create.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
	status = vkCreateEvent(device, &event_create, NULL, &event);
	expect_status(status, VK_SUCCESS, "event creation");
	status = vkGetEventStatus(device, event);
	expect_status(status, VK_EVENT_RESET, "event initially reset");
	status = vkSetEvent(device, event);
	expect_status(status, VK_SUCCESS, "host event set");
	status = vkGetEventStatus(device, event);
	expect_status(status, VK_EVENT_SET, "host event set observed");
	status = vkResetEvent(device, event);
	expect_status(status, VK_SUCCESS, "host event reset");
	status = vkGetEventStatus(device, event);
	expect_status(status, VK_EVENT_RESET, "host reset observed");

	/* Submit a native fence that cannot complete before another host thread sets its event. */
	memset(&fence_create, 0, sizeof(fence_create));
	fence_create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	status = vkCreateFence(device, &fence_create, NULL, &fence);
	expect_status(status, VK_SUCCESS, "event-dependent fence creation");
	peer.event_blocked = 1;
	status = vkQueueSubmit(queue, 0, NULL, fence);
	expect_status(status, VK_SUCCESS, "event-dependent queue work accepted");
	producer.device = device;
	producer.event = event;
	error = pthread_create(&thread, NULL, signal_event, &producer);
	expect(error, 0, "event producer thread created");
	status = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
	expect_status(status, VK_SUCCESS, "infinite wait permits producer progress");
	error = pthread_join(thread, NULL);
	expect(error, 0, "event producer joined");
	vkDestroyFence(device, fence, NULL);
	vkDestroyEvent(device, event, NULL);

	/* Succeeded: no GPU wait retained a mutex required by host event signaling. */
	return;
}

/* Check unavailable outputs, PARTIAL, availability width, padding and nonblocking WAIT. */
static void
test_queries(
	VkDevice device)
{
	VkQueryPoolCreateInfo create;
	VkEventCreateInfo event_create;
	VkQueryPool pool;
	VkEvent event;
	VkResult status;
	uint32_t words[8];
	uint64_t wide[8];
	struct event_producer producer;
	pthread_t thread;
	uint32_t index;
	int error;

	/* An occlusion pool produces one result word per query. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	create.queryType = VK_QUERY_TYPE_OCCLUSION;
	create.queryCount = 2;
	status = vkCreateQueryPool(device, &create, NULL, &pool);
	expect_status(status, VK_SUCCESS, "query pool creation");
	peer.query_complete = 0;

	/* Unavailable results and every stride-padding word must retain their sentinel. */
	for (index = 0; index < 8; index++)
		words[index] = 0xabcdef01U;

	/* Without PARTIAL or availability, only the completed result may change. */
	status = vkGetQueryPoolResults(device, pool, 0, 2, sizeof(words), words, 16, 0);
	expect_status(status, VK_NOT_READY, "query pending result preserved");
	expect(words[0], 111, "available query result copied");
	for (index = 1; index < 8; index++) {
		expect(words[index], 0xabcdef01U, "unavailable result or padding retained");
	}

	/* PARTIAL permits incomplete result words but never changes caller stride padding. */
	status = vkGetQueryPoolResults(device, pool, 0, 2, sizeof(words), words, 16, VK_QUERY_RESULT_PARTIAL_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	expect_status(status, VK_SUCCESS, "partial query result accepted");
	expect(words[0], 111, "completed partial-query word");
	expect(words[1], 1, "available status appended");
	expect(words[4], 777, "explicitly allowed partial result copied");
	expect(words[5], 0, "unavailable status appended");
	expect(words[6], 0xabcdef01U, "partial query stride padding retained");

	/* The 64-bit path preserves the same availability and padding semantics. */
	for (index = 0; index < 8; index++)
		wide[index] = UINT64_C(0xabcdef0123456789);

	/* Native packing must not inherit the wider caller's 32-byte record stride. */
	status = vkGetQueryPoolResults(device, pool, 0, 2, sizeof(wide), wide, 32, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	expect_status(status, VK_NOT_READY, "64-bit pending query result");
	expect(wide[0], 111, "64-bit result copied");
	expect(wide[1], 1, "64-bit availability copied");
	expect(wide[4], UINT64_C(0xabcdef0123456789), "64-bit unavailable result retained");
	expect(wide[5], 0, "64-bit unavailable status copied");

	/* A concurrent event producer must progress even with WAIT and PARTIAL together. */
	memset(&event_create, 0, sizeof(event_create));
	event_create.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
	status = vkCreateEvent(device, &event_create, NULL, &event);
	expect_status(status, VK_SUCCESS, "query producer event creation");
	producer.device = device;
	producer.event = event;
	error = pthread_create(&thread, NULL, signal_event, &producer);
	expect(error, 0, "query producer thread created");
	status = vkGetQueryPoolResults(device, pool, 0, 2, sizeof(words), words, 16, VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_PARTIAL_BIT);
	expect_status(status, VK_SUCCESS, "query WAIT permits producer progress");
	expect(words[4], 112, "WAIT ignores premature PARTIAL success until available");
	error = pthread_join(thread, NULL);
	expect(error, 0, "query producer joined");
	vkDestroyEvent(device, event, NULL);
	vkDestroyQueryPool(device, pool, NULL);

	/* Succeeded: available results are copied without corrupting unrelated caller bytes. */
	return;
}

/* Exercise sparse queue dispatch and software wait rollback with a real binding interval. */
static void
test_sparse(
	VkDevice device,
	VkQueue queue)
{
	VkSemaphoreCreateInfo create;
	VkSemaphore semaphore;
	VkSparseMemoryBind memory;
	VkSparseBufferMemoryBindInfo buffer;
	VkBindSparseInfo bind;
	struct vulkan_object buffer_object;
	uint64_t handle;
	VkResult status;

	/* The unbind uses a nullable memory handle but retains its nonzero byte interval. */
	memset(&buffer_object, 0, sizeof(buffer_object));
	buffer_object.wire_id = 2000;
	handle = vulkan_nondispatchable_handle(&buffer_object);
	memset(&memory, 0, sizeof(memory));
	memory.resourceOffset = 4096;
	memory.size = 8192;
	memset(&buffer, 0, sizeof(buffer));
	buffer.buffer = (VkBuffer)(uintptr_t)handle;
	buffer.bindCount = 1;
	buffer.pBinds = &memory;

	/* Acquisition completion belongs to the same binary semaphore used by sparse waits. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	status = vkCreateSemaphore(device, &create, NULL, &semaphore);
	expect_status(status, VK_SUCCESS, "sparse semaphore creation");
	status = vulkan_wsi_acquire_signal((struct VkDevice_T *)device, semaphore, VK_NULL_HANDLE);
	expect_status(status, VK_SUCCESS, "sparse acquisition signal");
	memset(&bind, 0, sizeof(bind));
	bind.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
	bind.waitSemaphoreCount = 1;
	bind.pWaitSemaphores = &semaphore;
	bind.bufferBindCount = 1;
	bind.pBufferBinds = &buffer;
	peer.fail_opcode = 34;
	peer.fail_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	status = vkQueueBindSparse(queue, 1, &bind, VK_NULL_HANDLE);
	expect_status(status, VK_ERROR_OUT_OF_DEVICE_MEMORY, "sparse enqueue failure preserved");
	status = vkQueueBindSparse(queue, 1, &bind, VK_NULL_HANDLE);
	expect_status(status, VK_SUCCESS, "sparse acquisition survives failed enqueue");
	expect(peer.waits, 0, "software sparse wait omitted after successful retry");
	expect(peer.sparse_ranges, 2, "both sparse wire requests preserved the range");
	vkDestroySemaphore(device, semaphore, NULL);

	/* Succeeded: sparse bind uses the same transactional payload semantics as submit. */
	return;
}

/* Makes native and malformed-reply loss observable across independent API families. */
static void
test_context_loss(void)
{
	struct vulkan_context context;
	struct VkDevice_T device;
	struct vulkan_object *object;
	VkFenceCreateInfo fence_info;
	VkQueryPoolCreateInfo query_info;
	VkFence fences[2];
	VkQueryPool pool;
	VkResult status;
	uint64_t identities[3];
	uint32_t output[2];
	uint32_t mode;
	uint32_t index;
	uint32_t calls;
	int error;

	/* Each independent namespace exercises one terminal failure with ordinary owned objects. */
	for (mode = 0; mode < 2; mode++) {
		memset(&context, 0, sizeof(context));
		memset(&device, 0, sizeof(device));
		device.object.context = &context;
		device.object.wire_id = 1000;
		error = pthread_mutex_init(&device.mutex, NULL);
		expect(error, 0, "loss fixture device mutex");

		/* One native fence and one acquired fence expose both completion representations. */
		memset(&fence_info, 0, sizeof(fence_info));
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		status = vkCreateFence((VkDevice)&device, &fence_info, NULL, &fences[0]);
		expect_status(status, VK_SUCCESS, "loss fixture native fence");
		fence_info.flags = 0;
		status = vkCreateFence((VkDevice)&device, &fence_info, NULL, &fences[1]);
		expect_status(status, VK_SUCCESS, "loss fixture software fence");
		status = vulkan_wsi_acquire_signal(&device, VK_NULL_HANDLE, fences[1]);
		expect_status(status, VK_SUCCESS, "loss fixture acquired payload");

		/* Query results provide a separate family which consults the shared context. */
		memset(&query_info, 0, sizeof(query_info));
		query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		query_info.queryType = VK_QUERY_TYPE_OCCLUSION;
		query_info.queryCount = 2;
		status = vkCreateQueryPool((VkDevice)&device, &query_info, NULL, &pool);
		expect_status(status, VK_SUCCESS, "loss fixture query pool");
		output[0] = 0x12345678U;
		output[1] = 0x87654321U;

		/* Save only native IDs before local destruction makes public handles invalid. */
		for (index = 0; index < 2; index++) {
			object = vulkan_nondispatchable_object((uint64_t)(uintptr_t)fences[index]);
			identities[index] = object->wire_id;
		}

		/* The fake renderer's later namespace close will retire uncertain native objects. */
		object = vulkan_nondispatchable_object((uint64_t)(uintptr_t)pool);
		identities[2] = object->wire_id;

		/* Native device loss and a truncated query marker must have the same terminal effect. */
		if (mode == 0) {
			peer.fail_opcode = 38;
			peer.fail_result = VK_ERROR_DEVICE_LOST;
			status = vkGetFenceStatus((VkDevice)&device, fences[0]);
			expect_status(status, VK_ERROR_DEVICE_LOST, "native fence loss");
		} else {
			peer.malformed_query = 2;
			status = vkGetQueryPoolResults((VkDevice)&device, pool, 0, 2, sizeof(output), output, sizeof(output[0]), 0);
			expect_status(status, VK_ERROR_DEVICE_LOST, "truncated query marker");
			peer.malformed_query = 0;
		}

		/* The atomic shared namespace must reject independent API families without another command. */
		status = __atomic_load_n(&context.error, __ATOMIC_ACQUIRE);
		expect_status(status, VK_ERROR_DEVICE_LOST, "context loss is sticky");
		calls = peer.calls[49];
		status = vkGetQueryPoolResults((VkDevice)&device, pool, 0, 2, sizeof(output), output, sizeof(output[0]), 0);
		expect_status(status, VK_ERROR_DEVICE_LOST, "query observes other family loss");
		expect(peer.calls[49], calls, "lost query cannot enqueue");
		expect(output[0], 0x12345678U, "lost query preserves first caller word");
		expect(output[1], 0x87654321U, "lost query preserves second caller word");
		status = vkGetFenceStatus((VkDevice)&device, fences[1]);
		expect_status(status, VK_ERROR_DEVICE_LOST, "software payload cannot mask context loss");
		status = vulkan_wsi_acquire_signal(&device, VK_NULL_HANDLE, fences[1]);
		expect_status(status, VK_ERROR_DEVICE_LOST, "new acquisition cannot mask context loss");

		/* Standard destruction consumes every local object even when transport use is terminal. */
		vkDestroyQueryPool((VkDevice)&device, pool, NULL);
		vkDestroyFence((VkDevice)&device, fences[0], NULL);
		vkDestroyFence((VkDevice)&device, fences[1], NULL);
		expect((uint64_t)(uintptr_t)device.object.first_child, 0, "lost namespace frees local children");
		error = pthread_mutex_destroy(&device.mutex);
		expect(error, 0, "loss fixture mutex retirement");

		/* Closing the mock renderer namespace discards its remaining objects as real context close does. */
		for (index = 0; index < 3; index++)
			memset(&peer.objects[identities[index]], 0, sizeof(peer.objects[identities[index]]));
	}

	/* Succeeded: both terminal failures propagate across families before any local success shortcut. */
	return;
}
