/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises actual pipeline encoders against an independent stateful native peer.
 */

#include "internal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One decoder cursor owns only the current immutable command interval. */
struct peer_cursor {
	const uint8_t *bytes;
	size_t length;
	size_t offset;
};

/* A peer record keeps native identities and opaque cache bytes until destruction. */
struct peer_object {
	uint64_t identifier;
	uint32_t kind;
	uint8_t cache[96];
	size_t cache_bytes;
};

/* All peer objects belong to this fixture and are protected by the context mutex. */
static struct peer_object peer_objects[256];

/* The logical device owns the actual production object registry for this fixture. */
static struct VkDevice_T test_device;

/* The context serializes peer transactions just as the native transport does. */
static struct vulkan_context test_context;

/* Synthetic prerequisite objects have stable identities without invoking other API families. */
static struct vulkan_object shader_object, set_layout_object, render_pass_object;

/* Failure injection and accounting are accessed only by the single-threaded setup paths. */
static unsigned allocation_position, allocation_failure, allocations, releases, destroy_callbacks;

/* Peer error switches affect exactly the next submitted batch. */
static unsigned partial_failure, malformed_reply, transport_failure;

/* Submission accounting distinguishes preflight failure from native side effects. */
static unsigned commands, live_objects, compute_batches, graphics_batches;

/* Allocator user-data tokens prove compatible destruction uses the current callback. */
static int creation_token, destruction_token;

static uint32_t peer_u32(struct peer_cursor *cursor);
static uint64_t peer_u64(struct peer_cursor *cursor);
static void peer_skip(struct peer_cursor *cursor, size_t bytes);
static void peer_structure(struct peer_cursor *cursor, uint32_t type);
static void peer_write32(uint8_t *bytes, uint32_t number);
static void peer_write64(uint8_t *bytes, uint64_t number);
static struct peer_object *peer_find(uint64_t identifier);
static struct peer_object *peer_create(uint64_t identifier, uint32_t kind);
static void peer_stage(struct peer_cursor *cursor, uint32_t expected_stage);
static void peer_compute(struct peer_cursor *cursor, uint32_t index);
static void peer_graphics(struct peer_cursor *cursor);
static void peer_single(struct peer_cursor *cursor, uint32_t opcode, struct vulkan_reader *reply);
static void peer_cache_data(struct peer_cursor *cursor, struct vulkan_reader *reply);
static void peer_batch(struct peer_cursor *cursor, uint32_t opcode, struct vulkan_reader *reply);
static void *test_allocate(void *user, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void *test_reallocate(void *user, void *original, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void test_free(void *user, void *pointer);
static void check_caches(const VkAllocationCallbacks *allocator);
static VkPipelineLayout create_layout(const VkAllocationCallbacks *allocator);
static void check_compute(VkPipelineLayout layout, const VkAllocationCallbacks *allocator, const VkAllocationCallbacks *destroy_allocator);
static void check_graphics(VkPipelineLayout layout);
static void *query_thread(void *argument);

/*
 * Executes the ordinary production wire contract using a separately written bounded decoder.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t capacity,
	struct vulkan_reader *reply)
{
	struct peer_cursor cursor;
	struct peer_object *object;
	struct peer_object *source;
	uint32_t opcode;
	uint32_t count;
	uint32_t index;
	uint64_t identifier;
	int error;

	/* Simulates native transaction admission rather than serializing callers in the API fixture. */
	assert(context == &test_context);
	error = pthread_mutex_lock(&context->mutex);
	assert(error == 0);
	commands++;

	/* A refused transport has no native allocation or response ownership. */
	if (transport_failure != 0U) {
		transport_failure = 0U;
		error = pthread_mutex_unlock(&context->mutex);
		assert(error == 0);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Response storage follows the provoking object's effective callback policy. */
	memset(reply, 0, sizeof(*reply));
	reply->allocator = writer->allocator;
	if (reply->allocator.has_callbacks != VK_FALSE)
		reply->data = vulkan_allocate(&reply->allocator, capacity, 8U, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	else
		reply->data = malloc(capacity);
	if (reply->data == NULL) {
		error = pthread_mutex_unlock(&context->mutex);
		assert(error == 0);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	memset(reply->data, 0, capacity);

	/* Every request carries the actual production header and device namespace identity. */
	cursor.bytes = writer->data;
	cursor.length = writer->bytes;
	cursor.offset = 0U;
	opcode = peer_u32(&cursor);
	assert(peer_u32(&cursor) == 1U);
	assert(peer_u64(&cursor) == test_device.object.wire_id);
	peer_write32(reply->data, opcode);

	/* Dispatches stateful cache, layout and pipeline commands through independent parsing. */
	switch (opcode) {
	case VULKAN_OPCODE_vkCreatePipelineCache:
	case VULKAN_OPCODE_vkCreatePipelineLayout:
		peer_single(&cursor, opcode, reply);
		break;
	case VULKAN_OPCODE_vkDestroyPipelineCache:
	case VULKAN_OPCODE_vkDestroyPipelineLayout:
	case VULKAN_OPCODE_vkDestroyPipeline:
		identifier = peer_u64(&cursor);
		object = peer_find(identifier);
		assert(peer_u64(&cursor) == 0U);
		memset(object, 0, sizeof(*object));
		live_objects--;
		reply->bytes = 4U;
		break;
	case VULKAN_OPCODE_vkGetPipelineCacheData:
		peer_cache_data(&cursor, reply);
		break;
	case VULKAN_OPCODE_vkMergePipelineCaches:
		identifier = peer_u64(&cursor);
		object = peer_find(identifier);
		count = peer_u32(&cursor);
		assert(peer_u64(&cursor) == count);
		for (index = 0U; index < count; index++) {
			identifier = peer_u64(&cursor);
			source = peer_find(identifier);
			assert(object != source);
			object->cache[32] ^= source->cache[32];
		}
		reply->bytes = 8U;
		break;
	case VULKAN_OPCODE_vkCreateGraphicsPipelines:
	case VULKAN_OPCODE_vkCreateComputePipelines:
		peer_batch(&cursor, opcode, reply);
		break;
	default:
		assert(0);
	}

	/* Full consumption detects hidden extra fields and incorrect protocol padding. */
	assert(cursor.offset == cursor.length);
	assert(reply->bytes <= capacity);
	error = pthread_mutex_unlock(&context->mutex);
	assert(error == 0);

	/* Succeeded: the ordinary wire reader now owns a complete native response. */
	return VK_SUCCESS;
}

/*
 * Supplies attachment metadata independently from the pipeline encoder's decisions.
 */
VkResult
vulkan_render_pass_subpass(
	VkRenderPass render_pass,
	uint32_t index,
	VkBool32 *color,
	VkBool32 *depth)
{
	/* These tests deliberately use a subpass with neither color nor depth attachments. */
	assert((uint64_t)render_pass == vulkan_nondispatchable_handle(&render_pass_object));
	assert(index == 0U);
	*color = VK_FALSE;
	*depth = VK_FALSE;

	/* Succeeded: both related state pointers are ignored by the graphics specification. */
	return VK_SUCCESS;
}

/*
 * Runs cache, derivative, partial creation, callback and ignored-state ownership checks.
 */
int
main(
	void)
{
	VkAllocationCallbacks allocator;
	VkAllocationCallbacks destroy_allocator;
	VkPipelineLayout layout;
	int error;

	/* Establishes a real dynamic local object registry and independently synchronized peer. */
	memset(&test_context, 0, sizeof(test_context));
	memset(&test_device, 0, sizeof(test_device));
	error = pthread_mutex_init(&test_context.mutex, NULL);
	assert(error == 0);
	test_device.object.kind = VULKAN_OBJECT_DEVICE;
	test_device.object.wire_id = 9000U;
	test_device.object.context = &test_context;

	/* The prerequisite handles use deliberately different local addresses and wire IDs. */
	shader_object.kind = VULKAN_OBJECT_SHADER_MODULE;
	shader_object.wire_id = 9001U;
	set_layout_object.kind = VULKAN_OBJECT_DESCRIPTOR_SET_LAYOUT;
	set_layout_object.wire_id = 9002U;
	render_pass_object.kind = VULKAN_OBJECT_RENDER_PASS;
	render_pass_object.wire_id = 9003U;

	/* Compatible callbacks retain their allocation family while changing destruction user data. */
	memset(&allocator, 0, sizeof(allocator));
	allocator.pUserData = &creation_token;
	allocator.pfnAllocation = test_allocate;
	allocator.pfnReallocation = test_reallocate;
	allocator.pfnFree = test_free;
	destroy_allocator = allocator;
	destroy_allocator.pUserData = &destruction_token;

	/* Separate scenarios exercise public APIs with actual objects, writers, readers and codec. */
	check_caches(&allocator);
	layout = create_layout(&allocator);
	check_compute(layout, &allocator, &destroy_allocator);
	check_graphics(layout);
	vkDestroyPipelineLayout(&test_device, layout, &destroy_allocator);

	/* Every successful native result and every callback allocation must retire exactly once. */
	assert(live_objects == 0U);
	assert(test_device.object.first_child == NULL);
	assert(allocations == releases);
	assert(destroy_callbacks != 0U);
	assert(compute_batches >= 4U);
	assert(graphics_batches == 2U);
	error = pthread_mutex_destroy(&test_context.mutex);
	assert(error == 0);
	puts("libvulkan pipeline: actual codec, cache bytes/merge/two-call, 37 pipelines, derivatives, partial failure, ignored pointers, callbacks and concurrent queries PASS");

	/* Succeeded: no native identity or local allocation survives the finite fixture. */
	return 0;
}

/* Reads a scalar only after proving its complete interval belongs to the command. */
static uint32_t
peer_u32(
	struct peer_cursor *cursor)
{
	uint32_t number;
	unsigned index;

	/* Decoder bounds are independent from the production writer implementation. */
	assert(cursor->offset <= cursor->length);
	assert(cursor->length - cursor->offset >= 4U);
	number = 0U;
	for (index = 0U; index < 4U; index++)
		number |= (uint32_t)cursor->bytes[cursor->offset + index] << (8U * index);
	cursor->offset += 4U;

	/* Succeeded: one little-endian protocol word has been consumed. */
	return number;
}

/* Reads a wide scalar without depending on the production reader's implementation. */
static uint64_t
peer_u64(
	struct peer_cursor *cursor)
{
	uint64_t low;
	uint64_t high;

	/* Two sequential reads preserve both protocol order and unaligned access safety. */
	low = peer_u32(cursor);
	high = peer_u32(cursor);

	/* Succeeded: the result has the protocol's complete unsigned width. */
	return low | (high << 32);
}

/* Skips only a verified encoded interval whose contents this scenario does not interpret. */
static void
peer_skip(
	struct peer_cursor *cursor,
	size_t bytes)
{
	/* Bounds checks catch both truncation and decoder-count disagreement. */
	assert(cursor->offset <= cursor->length);
	assert(bytes <= cursor->length - cursor->offset);
	cursor->offset += bytes;

	/* Succeeded: the next field starts within the same immutable command. */
	return;
}

/* Verifies each typed input's fixed prefix before parsing its selected members. */
static void
peer_structure(
	struct peer_cursor *cursor,
	uint32_t type)
{
	/* Core 1.0 structures carry an explicit structure tag and an empty extension chain. */
	assert(peer_u32(cursor) == type);
	assert(peer_u64(cursor) == 0U);

	/* Succeeded: the next field belongs to this standard structure type. */
	return;
}

/* Emits one peer response word independently of the production encoder. */
static void
peer_write32(
	uint8_t *bytes,
	uint32_t number)
{
	unsigned index;

	/* The byte loop makes the synthetic peer independent of native alignment and endianness. */
	for (index = 0U; index < 4U; index++)
		bytes[index] = (uint8_t)(number >> (index * 8U));

	/* Succeeded: a complete word is visible to the production reader. */
	return;
}

/* Emits a peer response identity with an explicit 64-bit wire width. */
static void
peer_write64(
	uint8_t *bytes,
	uint64_t number)
{
	/* Low and high words use the protocol's little-endian ordering. */
	peer_write32(bytes, (uint32_t)number);
	peer_write32(bytes + 4U, (uint32_t)(number >> 32));

	/* Succeeded: the response retains identities larger than a native 32-bit pointer. */
	return;
}

/* Resolves only native identities actually created by this peer. */
static struct peer_object *
peer_find(
	uint64_t identifier)
{
	unsigned index;

	/* No local application handle is accepted as a native object identity. */
	for (index = 0U; index < 256U; index++) {
		if (peer_objects[index].identifier == identifier)
			return &peer_objects[index];
	}
	assert(0);

	/* This unreachable fallback keeps the assertion-disabled build type correct. */
	return NULL;
}

/* Allocates one independently owned native object for a reserved client identity. */
static struct peer_object *
peer_create(
	uint64_t identifier,
	uint32_t kind)
{
	unsigned index;

	/* The fixture quota exceeds its largest 37-element batch and is not a production limit. */
	assert(identifier != 0U);
	for (index = 0U; index < 256U; index++) {
		if (peer_objects[index].identifier == 0U)
			break;
	}
	assert(index < 256U);
	peer_objects[index].identifier = identifier;
	peer_objects[index].kind = kind;
	live_objects++;

	/* Succeeded: later peer operations can resolve this new native identity. */
	return &peer_objects[index];
}

/* Checks shader entry-point string padding and optional specialization framing. */
static void
peer_stage(
	struct peer_cursor *cursor,
	uint32_t expected_stage)
{
	uint64_t specialized;

	/* Shader handles must become native IDs while the five-byte main name keeps padding. */
	peer_structure(cursor, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
	assert(peer_u32(cursor) == 0U);
	assert(peer_u32(cursor) == expected_stage);
	assert(peer_u64(cursor) == shader_object.wire_id);
	assert(peer_u64(cursor) == 5U);
	assert(memcmp(cursor->bytes + cursor->offset, "main", 5U) == 0);
	peer_skip(cursor, 8U);
	specialized = peer_u64(cursor);

	/* A single specialization entry validates native size_t and opaque data framing. */
	if (specialized != 0U) {
		assert(specialized == 1U);
		assert(peer_u32(cursor) == 1U);
		assert(peer_u64(cursor) == 1U);
		assert(peer_u32(cursor) == 7U);
		assert(peer_u32(cursor) == 0U);
		assert(peer_u64(cursor) == 4U);
		assert(peer_u64(cursor) == 4U);
		assert(peer_u64(cursor) == 4U);
		assert(peer_u32(cursor) == 0x12345678U);
	}

	/* Succeeded: the native shader stage has its exact standard parameters. */
	return;
}

/* Verifies derivative fields select either an earlier batch index or a native base handle. */
static void
peer_compute(
	struct peer_cursor *cursor,
	uint32_t index)
{
	uint32_t flags;
	uint64_t base;
	int32_t base_index;

	/* Compute's embedded stage and layout use the same common protocol contract. */
	peer_structure(cursor, VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
	flags = peer_u32(cursor);
	peer_stage(cursor, VK_SHADER_STAGE_COMPUTE_BIT);
	assert(peer_u64(cursor) != 0U);
	base = peer_u64(cursor);
	base_index = (int32_t)peer_u32(cursor);

	/* Ignored public handles must already be null before the peer ever sees them. */
	if ((flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) == 0U) {
		assert(base == 0U);
		assert(base_index == -1);
	} else if (base_index >= 0) {
		assert(base == 0U);
		assert((uint32_t)base_index < index);
	} else {
		assert(base_index == -1);
		peer_find(base);
	}

	/* Succeeded: no local handle bits or stale ignored base value entered the native namespace. */
	return;
}

/* Checks ignored graphics pointers and dynamic arrays using an attachment-free subpass. */
static void
peer_graphics(
	struct peer_cursor *cursor)
{
	uint64_t viewport;
	uint32_t discarded;

	/* The independent decoder checks each pointer marker rather than searching for byte patterns. */
	peer_structure(cursor, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
	assert(peer_u32(cursor) == 0U);
	assert(peer_u32(cursor) == 1U);
	assert(peer_u64(cursor) == 1U);
	peer_stage(cursor, VK_SHADER_STAGE_VERTEX_BIT);
	assert(peer_u64(cursor) == 1U);
	peer_structure(cursor, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
	assert(peer_u32(cursor) == 0U);
	assert(peer_u32(cursor) == 0U);
	assert(peer_u64(cursor) == 0U);
	assert(peer_u32(cursor) == 0U);
	assert(peer_u64(cursor) == 0U);
	assert(peer_u64(cursor) == 1U);
	peer_structure(cursor, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
	assert(peer_u32(cursor) == 0U);
	assert(peer_u32(cursor) == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	assert(peer_u32(cursor) == VK_FALSE);

	/* No tessellation stage means the deliberately invalid application pointer is absent. */
	assert(peer_u64(cursor) == 0U);
	viewport = peer_u64(cursor);
	if (viewport != 0U) {
		assert(viewport == 1U);
		peer_structure(cursor, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
		assert(peer_u32(cursor) == 0U);
		assert(peer_u32(cursor) == 1U);
		assert(peer_u64(cursor) == 0U);
		assert(peer_u32(cursor) == 1U);
		assert(peer_u64(cursor) == 0U);
	}

	/* Rasterizer discard removes downstream state; dynamic viewport arrays remain absent. */
	assert(peer_u64(cursor) == 1U);
	peer_structure(cursor, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
	assert(peer_u32(cursor) == 0U);
	assert(peer_u32(cursor) == VK_FALSE);
	discarded = peer_u32(cursor);
	peer_skip(cursor, 32U);
	if (discarded != VK_FALSE) {
		assert(viewport == 0U);
		assert(peer_u64(cursor) == 0U);
	} else {
		assert(viewport == 1U);
		assert(peer_u64(cursor) == 1U);
		peer_structure(cursor, VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
		assert(peer_u32(cursor) == 0U);
		assert(peer_u32(cursor) == VK_SAMPLE_COUNT_1_BIT);
		assert(peer_u32(cursor) == VK_FALSE);
		assert(peer_u32(cursor) == 0U);
		assert(peer_u64(cursor) == 0U);
		assert(peer_u32(cursor) == VK_FALSE);
		assert(peer_u32(cursor) == VK_FALSE);
	}

	/* No attachment use means both invalid application state pointers stay completely unread. */
	assert(peer_u64(cursor) == 0U);
	assert(peer_u64(cursor) == 0U);
	assert(peer_u64(cursor) == 1U);
	peer_structure(cursor, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
	assert(peer_u32(cursor) == 0U);
	assert(peer_u32(cursor) == 2U);
	assert(peer_u64(cursor) == 2U);
	assert(peer_u32(cursor) == VK_DYNAMIC_STATE_VIEWPORT);
	assert(peer_u32(cursor) == VK_DYNAMIC_STATE_SCISSOR);
	assert(peer_u64(cursor) != 0U);
	assert(peer_u64(cursor) == render_pass_object.wire_id);
	assert(peer_u32(cursor) == 0U);
	assert(peer_u64(cursor) == 0U);
	assert((int32_t)peer_u32(cursor) == -1);

	/* Succeeded: both static selection and dynamic-array omission match the native grammar. */
	return;
}

/* Creates either a cache or layout while checking its independent standard payload. */
static void
peer_single(
	struct peer_cursor *cursor,
	uint32_t opcode,
	struct vulkan_reader *reply)
{
	uint8_t initial[96];
	uint64_t count;
	uint64_t identifier;
	struct peer_object *object;

	/* Each single create supplies one typed input followed by allocator and output markers. */
	assert(peer_u64(cursor) == 1U);
	count = 0U;
	if (opcode == VULKAN_OPCODE_vkCreatePipelineCache) {
		peer_structure(cursor, VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO);
		assert(peer_u32(cursor) == 0U);
		count = peer_u64(cursor);
		assert(peer_u64(cursor) == count);
		assert(count <= sizeof(initial));
		memcpy(initial, cursor->bytes + cursor->offset, (size_t)count);
		peer_skip(cursor, ((size_t)count + 3U) & ~(size_t)3U);
	} else {
		peer_structure(cursor, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
		assert(peer_u32(cursor) == 0U);
		assert(peer_u32(cursor) == 1U);
		assert(peer_u64(cursor) == 1U);
		assert(peer_u64(cursor) == set_layout_object.wire_id);
		assert(peer_u32(cursor) == 1U);
		assert(peer_u64(cursor) == 1U);
		assert(peer_u32(cursor) == VK_SHADER_STAGE_VERTEX_BIT);
		assert(peer_u32(cursor) == 0U);
		assert(peer_u32(cursor) == 16U);
	}
	assert(peer_u64(cursor) == 0U);
	assert(peer_u64(cursor) == 1U);
	identifier = peer_u64(cursor);
	object = peer_create(identifier, opcode);

	/* A cache begins with a stable version-one header and meaningful opaque native bytes. */
	if (opcode == VULKAN_OPCODE_vkCreatePipelineCache) {
		object->cache_bytes = 64U;
		memset(object->cache, 0xa5, object->cache_bytes);
		peer_write32(object->cache, 32U);
		peer_write32(object->cache + 4U, VK_PIPELINE_CACHE_HEADER_VERSION_ONE);
		if (count != 0U) {
			object->cache_bytes = (size_t)count;
			memcpy(object->cache, initial, (size_t)count);
		}
	}
	peer_write64(reply->data + 8U, 1U);
	peer_write64(reply->data + 16U, identifier);
	reply->bytes = 24U;

	/* Succeeded: the reply echoes exactly the client-reserved identity. */
	return;
}

/* Models specification-defined header truncation without borrowing production cache logic. */
static void
peer_cache_data(
	struct peer_cursor *cursor,
	struct vulkan_reader *reply)
{
	struct peer_object *object;
	uint64_t identifier;
	size_t capacity;
	size_t copied;
	VkResult result;

	/* Native size and output-array declarations are separate fields in the protocol. */
	identifier = peer_u64(cursor);
	object = peer_find(identifier);
	assert(peer_u64(cursor) == 1U);
	capacity = (size_t)peer_u64(cursor);
	assert(peer_u64(cursor) == capacity);
	result = VK_SUCCESS;
	copied = object->cache_bytes;
	if (capacity != 0U && capacity < copied) {
		result = VK_INCOMPLETE;
		copied = capacity;
		if (capacity < 32U)
			copied = 0U;
	}
	peer_write32(reply->data + 4U, (uint32_t)result);
	peer_write64(reply->data + 8U, 1U);
	peer_write64(reply->data + 16U, copied);
	reply->bytes = 32U;

	/* A null native buffer reports required size without serializing any cache contents. */
	if (capacity != 0U) {
		peer_write64(reply->data + 24U, copied);
		memcpy(reply->data + 32U, object->cache, copied);
		reply->bytes += (copied + 3U) & ~(size_t)3U;
	}

	/* Succeeded: the production API must distinguish sizing from a nonnull zero-sized buffer. */
	return;
}

/* Returns independent partial successes without forcing a successful whole-batch result. */
static void
peer_batch(
	struct peer_cursor *cursor,
	uint32_t opcode,
	struct vulkan_reader *reply)
{
	uint32_t count;
	uint32_t index;
	uint64_t identifier;

	/* The cache may be absent, while both input and output arrays retain explicit counts. */
	identifier = peer_u64(cursor);
	if (identifier != 0U)
		peer_find(identifier);
	count = peer_u32(cursor);
	assert(peer_u64(cursor) == count);
	for (index = 0U; index < count; index++) {
		if (opcode == VULKAN_OPCODE_vkCreateComputePipelines)
			peer_compute(cursor, index);
		else
			peer_graphics(cursor);
	}
	assert(peer_u64(cursor) == 0U);
	assert(peer_u64(cursor) == count);
	peer_write64(reply->data + 8U, count);

	/* A malformed vector never creates peer objects, modeling an already-lost namespace. */
	for (index = 0U; index < count; index++) {
		identifier = peer_u64(cursor);
		if (malformed_reply != 0U) {
			peer_write64(reply->data + 16U + 8U * index, identifier + 100000U);
		} else if (partial_failure != 0U && index == 1U) {
			peer_write64(reply->data + 16U + 8U * index, 0U);
		} else {
			peer_create(identifier, opcode);
			peer_write64(reply->data + 16U + 8U * index, identifier);
		}
	}
	if (partial_failure != 0U)
		peer_write32(reply->data + 4U, (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY);
	reply->bytes = 16U + 8U * count;
	partial_failure = 0U;
	malformed_reply = 0U;

	/* Family counters prove the relevant native commands actually reached this peer. */
	if (opcode == VULKAN_OPCODE_vkCreateComputePipelines)
		compute_batches++;
	else
		graphics_batches++;

	/* Succeeded: the production implementation must preserve each successful returned identity. */
	return;
}

/* Allocates callback-owned storage while permitting a single deterministic failure position. */
static void *
test_allocate(
	void *user,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	void *pointer;
	int error;

	/* Object and command scopes cover all allocations owned by this API family. */
	assert(user == &creation_token || user == &destruction_token);
	assert(scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT || scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	allocation_position++;
	if (allocation_position == allocation_failure)
		return NULL;
	if (alignment < sizeof(void *))
		alignment = sizeof(void *);
	pointer = NULL;
	error = posix_memalign(&pointer, alignment, bytes);
	assert(error == 0);
	allocations++;

	/* Succeeded: every nonnull callback result must later reach a compatible free callback. */
	return pointer;
}

/* Refuses reallocation because production command growth uses allocate-copy-free callbacks. */
static void *
test_reallocate(
	void *user,
	void *original,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	/* This callback exists for a valid standard allocator but is not needed by this implementation. */
	(void)user;
	(void)original;
	(void)bytes;
	(void)alignment;
	(void)scope;
	assert(0);

	/* The unreachable fallback satisfies the callback signature. */
	return NULL;
}

/* Records the callback policy that actually retires each allocation. */
static void
test_free(
	void *user,
	void *pointer)
{
	/* Null free is allowed by the allocator contract and creates no accounting event. */
	assert(user == &creation_token || user == &destruction_token);
	if (pointer == NULL)
		return;
	if (user == &destruction_token)
		destroy_callbacks++;
	releases++;
	free(pointer);

	/* Succeeded: this callback-owned block has retired exactly once. */
	return;
}

/* Queries a shared cache concurrently without modifying or externally synchronizing it. */
static void *
query_thread(
	void *argument)
{
	VkPipelineCache cache;
	uint8_t bytes[96];
	size_t count;
	unsigned index;
	VkResult error;

	/* Vulkan permits simultaneous read-only cache queries; the transport serializes only submission. */
	cache = *(VkPipelineCache *)argument;
	for (index = 0U; index < 50U; index++) {
		count = sizeof(bytes);
		error = vkGetPipelineCacheData(&test_device, cache, &count, bytes);
		assert(error == VK_SUCCESS);
		assert(count == 64U);
		assert(bytes[0] == 32U);
	}

	/* Succeeded: concurrent public calls retained independent writer and reply ownership. */
	return NULL;
}

/* Checks cache data roundtrip, native merge, short buffers and concurrent public queries. */
static void
check_caches(
	const VkAllocationCallbacks *allocator)
{
	VkPipelineCacheCreateInfo info;
	VkPipelineCache cache;
	VkPipelineCache source;
	uint8_t bytes[96];
	uint8_t roundtrip[96];
	size_t count;
	pthread_t threads[4];
	unsigned index;
	int thread_error;
	VkResult error;

	/* An ordinary native cache supplies its own opaque versioned initial data. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	error = vkCreatePipelineCache(&test_device, &info, NULL, &cache);
	assert(error == VK_SUCCESS);
	count = SIZE_MAX;
	error = vkGetPipelineCacheData(&test_device, cache, &count, NULL);
	assert(error == VK_SUCCESS && count == 64U);
	count = sizeof(bytes);
	error = vkGetPipelineCacheData(&test_device, cache, &count, bytes);
	assert(error == VK_SUCCESS && count == 64U);

	/* Zero and shorter-than-header buffers cannot become accidental full size queries. */
	count = 0U;
	error = vkGetPipelineCacheData(&test_device, cache, &count, roundtrip);
	assert(error == VK_INCOMPLETE && count == 0U);
	count = 16U;
	error = vkGetPipelineCacheData(&test_device, cache, &count, roundtrip);
	assert(error == VK_INCOMPLETE && count == 0U);
	count = 33U;
	error = vkGetPipelineCacheData(&test_device, cache, &count, roundtrip);
	assert(error == VK_INCOMPLETE && count == 33U);
	assert(memcmp(bytes, roundtrip, count) == 0);

	/* Native cache initialization preserves all returned opaque bytes exactly. */
	info.initialDataSize = 64U;
	info.pInitialData = bytes;
	error = vkCreatePipelineCache(&test_device, &info, allocator, &source);
	assert(error == VK_SUCCESS);
	count = sizeof(roundtrip);
	error = vkGetPipelineCacheData(&test_device, source, &count, roundtrip);
	assert(error == VK_SUCCESS && count == 64U);
	assert(memcmp(bytes, roundtrip, count) == 0);
	error = vkMergePipelineCaches(&test_device, cache, 1U, &source);
	assert(error == VK_SUCCESS);
	count = sizeof(roundtrip);
	error = vkGetPipelineCacheData(&test_device, cache, &count, roundtrip);
	assert(error == VK_SUCCESS && roundtrip[32] == 0U);

	/* Distinct threads use one cache and context without sharing command or response storage. */
	for (index = 0U; index < 4U; index++) {
		thread_error = pthread_create(&threads[index], NULL, query_thread, &cache);
		assert(thread_error == 0);
	}
	for (index = 0U; index < 4U; index++) {
		thread_error = pthread_join(threads[index], NULL);
		assert(thread_error == 0);
	}
	vkDestroyPipelineCache(&test_device, source, allocator);
	vkDestroyPipelineCache(&test_device, cache, NULL);

	/* Succeeded: both caches retire independently after all read-only users finish. */
	return;
}

/* Creates a real layout while checking descriptor handles and push constants on the wire. */
static VkPipelineLayout
create_layout(
	const VkAllocationCallbacks *allocator)
{
	VkPipelineLayoutCreateInfo info;
	VkPushConstantRange range;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkResult error;

	/* The layout references ordinary local prerequisite objects without retaining their storage. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	set_layout = (VkDescriptorSetLayout)vulkan_nondispatchable_handle(&set_layout_object);
	info.setLayoutCount = 1U;
	info.pSetLayouts = &set_layout;
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	range.offset = 0U;
	range.size = 16U;
	info.pushConstantRangeCount = 1U;
	info.pPushConstantRanges = &range;
	error = vkCreatePipelineLayout(&test_device, &info, allocator, &layout);
	assert(error == VK_SUCCESS);

	/* Succeeded: graphics and compute can share the independently owned standard layout. */
	return layout;
}

/* Exercises dynamic batch ownership, ignored handles and standard partial creation results. */
static void
check_compute(
	VkPipelineLayout layout,
	const VkAllocationCallbacks *allocator,
	const VkAllocationCallbacks *destroy_allocator)
{
	VkComputePipelineCreateInfo infos[37];
	VkPipeline pipelines[37];
	VkPipeline base;
	VkSpecializationMapEntry map;
	VkSpecializationInfo specialization;
	uint32_t constant;
	unsigned index;
	unsigned position;
	unsigned before;
	VkResult error;

	/* Specialization data deliberately exercises 64-bit size_t and a padded opaque array. */
	map.constantID = 7U;
	map.offset = 0U;
	map.size = 4U;
	constant = 0x12345678U;
	memset(&specialization, 0, sizeof(specialization));
	specialization.mapEntryCount = 1U;
	specialization.pMapEntries = &map;
	specialization.dataSize = sizeof(constant);
	specialization.pData = &constant;

	/* Every non-derivative contains a deliberately invalid ignored public base handle. */
	memset(infos, 0, sizeof(infos));
	for (index = 0U; index < 37U; index++) {
		infos[index].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		infos[index].stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		infos[index].stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		infos[index].stage.module = (VkShaderModule)vulkan_nondispatchable_handle(&shader_object);
		infos[index].stage.pName = "main";
		infos[index].stage.pSpecializationInfo = &specialization;
		infos[index].layout = layout;
		infos[index].basePipelineHandle = (VkPipeline)1U;
		infos[index].basePipelineIndex = 123;
	}
	infos[0].flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
	infos[1].flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
	infos[1].basePipelineIndex = 0;
	error = vkCreateComputePipelines(&test_device, VK_NULL_HANDLE, 37U, infos, allocator, pipelines);
	assert(error == VK_SUCCESS);

	/* A derivative can reference an independently owned external base after the original batch. */
	base = pipelines[0];
	for (index = 1U; index < 37U; index++)
		vkDestroyPipeline(&test_device, pipelines[index], destroy_allocator);
	infos[0].flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
	infos[0].basePipelineHandle = base;
	infos[0].basePipelineIndex = -1;
	error = vkCreateComputePipelines(&test_device, VK_NULL_HANDLE, 1U, infos, allocator, pipelines);
	assert(error == VK_SUCCESS);
	vkDestroyPipeline(&test_device, pipelines[0], destroy_allocator);
	vkDestroyPipeline(&test_device, base, destroy_allocator);

	/* Failed native batch creation can still yield two ordinary independently destroyable handles. */
	infos[0].flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
	infos[0].basePipelineHandle = (VkPipeline)1U;
	partial_failure = 1U;
	error = vkCreateComputePipelines(&test_device, VK_NULL_HANDLE, 3U, infos, allocator, pipelines);
	assert(error == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	assert(pipelines[0] != VK_NULL_HANDLE);
	assert(pipelines[1] == VK_NULL_HANDLE);
	assert(pipelines[2] != VK_NULL_HANDLE);
	for (index = 0U; index < 3U; index++)
		vkDestroyPipeline(&test_device, pipelines[index], destroy_allocator);

	/* Every allocation failure before native execution must leave all requested outputs null. */
	before = commands;
	for (position = 1U; position <= 5U; position++) {
		allocation_position = 0U;
		allocation_failure = position;
		error = vkCreateComputePipelines(&test_device, VK_NULL_HANDLE, 3U, infos, allocator, pipelines);
		assert(error == VK_ERROR_OUT_OF_HOST_MEMORY);
		assert(pipelines[0] == VK_NULL_HANDLE);
		assert(pipelines[1] == VK_NULL_HANDLE);
		assert(pipelines[2] == VK_NULL_HANDLE);
	}
	allocation_failure = 0U;
	assert(commands == before);

	/* A transport refusal occurs before native object ownership and still cleans preallocated handles. */
	transport_failure = 1U;
	error = vkCreateComputePipelines(&test_device, VK_NULL_HANDLE, 3U, infos, allocator, pipelines);
	assert(error == VK_ERROR_OUT_OF_HOST_MEMORY);
	assert(pipelines[0] == VK_NULL_HANDLE && pipelines[2] == VK_NULL_HANDLE);

	/* Malformed native identities mark the namespace lost and never publish foreign handles. */
	malformed_reply = 1U;
	error = vkCreateComputePipelines(&test_device, VK_NULL_HANDLE, 3U, infos, allocator, pipelines);
	assert(error == VK_ERROR_DEVICE_LOST);
	assert(pipelines[0] == VK_NULL_HANDLE && pipelines[2] == VK_NULL_HANDLE);
	assert(test_context.error == VK_ERROR_DEVICE_LOST);

	/* This isolated synthetic peer resets terminal state only to run the remaining independent family. */
	test_device.error = VK_SUCCESS;
	test_context.error = VK_SUCCESS;

	/* Succeeded: every real native partial success has a matching explicit destruction. */
	return;
}

/* Uses invalid ignored state pointers to prove graphics selection precedes dereference. */
static void
check_graphics(
	VkPipelineLayout layout)
{
	VkGraphicsPipelineCreateInfo info;
	VkPipelineShaderStageCreateInfo stage;
	VkPipelineVertexInputStateCreateInfo vertex;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDynamicStateCreateInfo dynamic;
	VkDynamicState states[2];
	VkPipeline pipeline;
	VkResult error;

	/* Ordinary vertex input and shader state remain relevant even when rasterization discards. */
	memset(&stage, 0, sizeof(stage));
	stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
	stage.module = (VkShaderModule)vulkan_nondispatchable_handle(&shader_object);
	stage.pName = "main";
	memset(&vertex, 0, sizeof(vertex));
	vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	memset(&rasterization, 0, sizeof(rasterization));
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.rasterizerDiscardEnable = VK_TRUE;
	rasterization.lineWidth = 1.0f;

	/* Dynamic viewport and scissor arrays may contain deliberately unusable pointers. */
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	states[0] = VK_DYNAMIC_STATE_VIEWPORT;
	states[1] = VK_DYNAMIC_STATE_SCISSOR;
	dynamic.dynamicStateCount = 2U;
	dynamic.pDynamicStates = states;
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1U;
	viewport.pViewports = (const VkViewport *)1;
	viewport.scissorCount = 1U;
	viewport.pScissors = (const VkRect2D *)1;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	/* All unused downstream state pointers use the same unmapped sentinel. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = 1U;
	info.pStages = &stage;
	info.pVertexInputState = &vertex;
	info.pInputAssemblyState = &assembly;
	info.pRasterizationState = &rasterization;
	info.pDynamicState = &dynamic;
	info.pTessellationState = (const VkPipelineTessellationStateCreateInfo *)1;
	info.pViewportState = (const VkPipelineViewportStateCreateInfo *)1;
	info.pMultisampleState = (const VkPipelineMultisampleStateCreateInfo *)1;
	info.pDepthStencilState = (const VkPipelineDepthStencilStateCreateInfo *)1;
	info.pColorBlendState = (const VkPipelineColorBlendStateCreateInfo *)1;
	info.layout = layout;
	info.renderPass = (VkRenderPass)vulkan_nondispatchable_handle(&render_pass_object);
	info.basePipelineHandle = (VkPipeline)1U;
	info.basePipelineIndex = 999;
	error = vkCreateGraphicsPipelines(&test_device, VK_NULL_HANDLE, 1U, &info, NULL, &pipeline);
	assert(error == VK_SUCCESS);
	vkDestroyPipeline(&test_device, pipeline, NULL);

	/* Rasterization restores only the selected states; absent attachments keep their pointers ignored. */
	rasterization.rasterizerDiscardEnable = VK_FALSE;
	info.pViewportState = &viewport;
	info.pMultisampleState = &multisample;
	error = vkCreateGraphicsPipelines(&test_device, VK_NULL_HANDLE, 1U, &info, NULL, &pipeline);
	assert(error == VK_SUCCESS);
	vkDestroyPipeline(&test_device, pipeline, NULL);

	/* Succeeded: both discarded and dynamically supplied graphics state reached the independent peer. */
	return;
}
