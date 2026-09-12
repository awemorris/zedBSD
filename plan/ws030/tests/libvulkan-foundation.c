/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Tests the real object and wire primitives against independent wire fixtures. */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "internal.h"

#define THREADS 4
#define CHILDREN 1024

/* Holds children owned by one concurrent producer until the test joins it. */
struct producer {
	struct vulkan_object *parent;
	struct vulkan_object *children[CHILDREN];
};

/* Counts application-owned host allocations and can refuse the next request. */
struct allocation_state {
	unsigned allocated;
	unsigned freed;
	int refuse;
	VkSystemAllocationScope scope;
};

/* Models transport replies at the byte boundary, without emulating API behavior. */
static uint8_t response_bytes[12];

static void *produce(void *argument);
static void *VKAPI_PTR allocate_callback(void *argument, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void VKAPI_PTR free_callback(void *argument, void *allocation);
static int compare_ids(const void *left, const void *right);
static void test_objects(void);
static void test_wire(void);

/*
 * Supplies explicit protocol response bytes to the command-framing primitive.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reader)
{
	uint8_t *copy;

	/* The fixture controls the response independently of submitted parameter bytes. */
	(void)context;
	(void)writer;
	(void)reply_capacity;
	copy = malloc(sizeof(response_bytes));
	assert(copy != NULL);
	memcpy(copy, response_bytes, sizeof(response_bytes));
	vulkan_reader_init(reader, copy, sizeof(response_bytes));

	/* Succeeded: transfers a separately owned reply to the production decoder. */
	return VK_SUCCESS;
}

/*
 * Runs allocation, concurrent identity, and independent protocol-byte checks.
 */
int
main(
	void)
{
	/* Exercises real library primitives without GPU or kernel prerequisites. */
	test_objects();
	test_wire();

	/* Identifies the focused gate without claiming a complete Vulkan runtime. */
	puts("libvulkan foundation: allocator, 4096 concurrent objects, wire bounds and framing PASS");

	/* Succeeded: every independent foundation fixture passed. */
	return 0;
}

/* Publishes independent objects through the production shared registry. */
static void *
produce(
	void *argument)
{
	struct producer *producer;
	VkResult error;
	unsigned index;

	/* Uses one common parent to force concurrent list mutations. */
	producer = argument;
	for (index = 0; index < CHILDREN; index++) {
		/* Creates and identifies one ordinary child without a fixed-size registry. */
		error = vulkan_object_alloc(sizeof(struct vulkan_object), 16, VULKAN_OBJECT_BUFFER, producer->parent, NULL, NULL, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &producer->children[index]);
		assert(error == VK_SUCCESS);
		error = vulkan_object_reserve_id(producer->children[index]);
		assert(error == VK_SUCCESS);
		error = vulkan_object_publish(producer->children[index]);
		assert(error == VK_SUCCESS);
	}

	/* Succeeded: the joining thread owns all recorded child handles. */
	return NULL;
}

/* Records allocator scope and supplies independently aligned host storage. */
static void *VKAPI_PTR
allocate_callback(
	void *argument,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	struct allocation_state *state;
	void *allocation;
	int error;

	/* Refuses the controlled allocation before changing accounting. */
	state = argument;
	if (state->refuse)
		return NULL;

	/* Uses the host's independent aligned allocator as the callback oracle. */
	error = posix_memalign(&allocation, alignment, bytes);
	assert(error == 0);
	__atomic_add_fetch(&state->allocated, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&state->scope, scope, __ATOMIC_RELAXED);

	/* Succeeded: returns callback-owned storage with the requested alignment. */
	return allocation;
}

/* Verifies each callback allocation is reclaimed through the same owner. */
static void VKAPI_PTR
free_callback(
	void *argument,
	void *allocation)
{
	struct allocation_state *state;

	/* Accounts for the callback's storage before returning it to the host. */
	state = argument;
	/* Accepts the null storage permitted by Vulkan allocation callbacks. */
	if (allocation == NULL)
		return;

	__atomic_add_fetch(&state->freed, 1, __ATOMIC_RELAXED);
	free(allocation);

	/* Succeeded: callback ownership has been released exactly once. */
	return;
}

/* Orders renderer identities without truncating 64-bit differences. */
static int
compare_ids(
	const void *left,
	const void *right)
{
	uint64_t first;
	uint64_t second;

	/* Compares exact identities without a subtraction that could overflow. */
	first = *(const uint64_t *)left;
	second = *(const uint64_t *)right;
	if (first < second)
		return -1;

	/* Distinguishes a larger identity from equality. */
	if (first > second)
		return 1;

	/* Succeeded: the identities are equal. */
	return 0;
}

/* Verifies dynamic ownership, allocation rollback, and concurrent unique IDs. */
static void
test_objects(
	void)
{
	struct producer producers[THREADS];
	pthread_t threads[THREADS];
	struct vulkan_object *parent;
	struct vulkan_object *object;
	struct vulkan_object *cursor;
	struct allocation_state state;
	struct allocation_state alternate;
	struct vulkan_writer temporary;
	VkAllocationCallbacks callbacks;
	uint64_t ids[THREADS * CHILDREN];
	unsigned thread;
	unsigned index;
	unsigned count;
	VkResult error;
	int status;

	/* Captures allocator callbacks without requiring a renderer context. */
	memset(&state, 0, sizeof(state));
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.pUserData = &state;
	callbacks.pfnAllocation = allocate_callback;
	callbacks.pfnFree = free_callback;
	error = vulkan_object_alloc(sizeof(*parent), 256, VULKAN_OBJECT_DEVICE, NULL, NULL, &callbacks, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &parent);
	assert(error == VK_SUCCESS);
	assert(((uintptr_t)parent & 255) == 0);
	assert(state.scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

	/* Leaves caller output unchanged when its own allocator refuses creation. */
	state.refuse = 1;
	object = parent;
	error = vulkan_object_alloc(sizeof(*object), 16, VULKAN_OBJECT_BUFFER, parent, NULL, &callbacks, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	assert(error == VK_ERROR_OUT_OF_HOST_MEMORY);
	assert(object == parent);
	assert(state.allocated == 1);

	/* Uses the parent policy when a child has no object-specific callbacks. */
	state.refuse = 0;
	error = vulkan_object_alloc(sizeof(*object), 16, VULKAN_OBJECT_BUFFER, parent, NULL, NULL, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	assert(error == VK_SUCCESS);
	assert(state.allocated == 2);
	assert(state.scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

	/* Routes temporary command growth through the same inherited policy. */
	vulkan_writer_init_for_object(&temporary, object);
	vulkan_write_u32(&temporary, 7);
	vulkan_writer_reserve(&temporary, 4096);
	assert(temporary.error == VK_SUCCESS);
	assert(state.scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	vulkan_writer_finish(&temporary);

	/* Uses the compatible destruction callback data supplied by this call. */
	memset(&alternate, 0, sizeof(alternate));
	callbacks.pUserData = &alternate;
	vulkan_object_free_with_allocator(object, &callbacks);
	assert(alternate.freed == 1);
	callbacks.pUserData = &state;

	/* Publishes thousands of children concurrently through the same parent. */
	memset(producers, 0, sizeof(producers));
	for (thread = 0; thread < THREADS; thread++) {
		producers[thread].parent = parent;
		status = pthread_create(&threads[thread], NULL, produce, &producers[thread]);
		assert(status == 0);
	}

	/* Joins every producer before reading the now-stable parent list. */
	for (thread = 0; thread < THREADS; thread++) {
		status = pthread_join(threads[thread], NULL);
		assert(status == 0);
	}

	/* Verifies that no concurrent publication lost a child. */
	count = 0;
	cursor = parent->first_child;
	while (cursor != NULL) {
		count++;
		cursor = cursor->next_sibling;
	}

	/* Confirms all reserved IDs survive list removal and object reclamation. */
	assert(count == THREADS * CHILDREN);
	count = 0;
	for (thread = 0; thread < THREADS; thread++) {
		/* Collects one producer's identities before destroying its children. */
		for (index = 0; index < CHILDREN; index++) {
			object = producers[thread].children[index];
			ids[count++] = object->wire_id;
			vulkan_object_free(object);
		}
	}

	/* Sorts identities independently and checks uniqueness rather than allocation order. */
	assert(parent->first_child == NULL);
	qsort(ids, count, sizeof(ids[0]), compare_ids);
	assert(ids[0] != 0);
	for (index = 1; index < count; index++) {
		assert(ids[index - 1] < ids[index]);
	}

	/* Reclaims the callback-owned parent only after every child is gone. */
	vulkan_object_free(parent);
	assert(state.freed + alternate.freed == state.allocated);

	/* Succeeded: registry and callback lifetime invariants survived the concurrent case. */
	return;
}

/* Verifies fixed protocol bytes, padding, overflow, truncation, and result handling. */
static void
test_wire(
	void)
{
	static const uint8_t expected[] = {
		0x78, 0x56, 0x34, 0x12, 0x08, 0x07, 0x06, 0x05,
		0x04, 0x03, 0x02, 0x01, 0x02, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00
	};
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint8_t *copy;
	uint32_t word;
	uint64_t number;
	VkResult error;
	size_t retained;
	int comparison;

	/* Compares encoding with a hand-authored byte fixture including string padding. */
	vulkan_writer_init(&writer);
	vulkan_write_u32(&writer, 0x12345678);
	vulkan_write_u64(&writer, UINT64_C(0x0102030405060708));
	vulkan_write_string(&writer, "A");
	assert(writer.error == VK_SUCCESS);
	assert(writer.bytes == sizeof(expected));
	comparison = memcmp(writer.data, expected, sizeof(expected));
	assert(comparison == 0);

	/* Proves valid buffers can grow beyond the former 16-KiB stream ceiling. */
	vulkan_writer_reserve(&writer, 1024 * 1024);
	assert(writer.error == VK_SUCCESS);
	assert(writer.capacity >= 1024 * 1024);
	comparison = memcmp(writer.data, expected, sizeof(expected));
	assert(comparison == 0);

	/* Refuses wrapped sizes without losing already encoded data. */
	retained = writer.bytes;
	vulkan_writer_reserve(&writer, SIZE_MAX);
	assert(writer.error == VK_ERROR_OUT_OF_HOST_MEMORY);
	assert(writer.bytes == retained);
	vulkan_write_u32(&writer, 0);
	assert(writer.bytes == retained);
	vulkan_writer_finish(&writer);

	/* Reads independent fixture bytes rather than feeding the encoder back to itself. */
	copy = malloc(sizeof(expected));
	assert(copy != NULL);
	memcpy(copy, expected, sizeof(expected));
	vulkan_reader_init(&reader, copy, sizeof(expected));
	word = vulkan_read_u32(&reader);
	assert(word == 0x12345678);
	number = vulkan_read_u64(&reader);
	assert(number == UINT64_C(0x0102030405060708));
	reader.cursor = reader.bytes - 3;
	retained = reader.cursor;
	word = vulkan_read_u32(&reader);
	assert(word == 0);
	assert(reader.error == VK_ERROR_DEVICE_LOST);
	assert(reader.cursor == retained);
	vulkan_reader_finish(&reader);

	/* Preserves VK_INCOMPLETE while positioning the reader at output parameters. */
	memset(response_bytes, 0, sizeof(response_bytes));
	response_bytes[0] = 42;
	response_bytes[4] = 5;
	response_bytes[8] = 7;
	vulkan_writer_init(&writer);
	vulkan_command_begin(&writer, 42);
	error = vulkan_command_execute(NULL, &writer, 12, &reader, VK_TRUE);
	assert(error == VK_INCOMPLETE);
	word = vulkan_read_u32(&reader);
	assert(word == 7);
	vulkan_reader_finish(&reader);

	/* Rejects a response from another command before consuming its payload. */
	response_bytes[0] = 43;
	error = vulkan_command_execute(NULL, &writer, 12, &reader, VK_TRUE);
	assert(error == VK_ERROR_DEVICE_LOST);
	vulkan_reader_finish(&reader);
	vulkan_writer_finish(&writer);

	/* Succeeded: every byte and failure fixture exercised production primitives. */
	return;
}
