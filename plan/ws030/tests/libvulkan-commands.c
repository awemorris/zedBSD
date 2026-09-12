/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Tests actual command wrappers against an independent stateful protocol peer. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"

#define PEER_BUFFERS 128
#define EXPECTED_BYTES 4096

/* Models native identity and recording state without invoking implementation encoders. */
struct peer_buffer {
	uint64_t identity;
	uint32_t level;
	unsigned state;
};

/* Tracks application callback allocations and controlled allocation refusal. */
struct allocation_state {
	unsigned allocations;
	unsigned frees;
	int fail;
};

/* Keeps each expected record independent of production writers and typed codecs. */
static uint8_t expected[EXPECTED_BYTES];
/* Counts the exact expected parameter byte range of one recording command. */
static size_t expected_bytes;
/* Identifies the pinned numeric recording opcode selected by the test case. */
static uint32_t expected_opcode;
/* Records whether the next peer operation must consume an explicit recording fixture. */
static unsigned expected_pending;
/* Accounts for every one of the forty-four standard recording entry points. */
static unsigned record_seen[44];
/* Holds a bounded native namespace larger than the former thirty-two-object limit. */
static struct peer_buffer peer_buffers[PEER_BUFFERS];
/* Associates native command buffers with the one live fixture pool. */
static uint64_t peer_pool;
/* Counts native command submissions independently of local recording calls. */
static unsigned peer_calls;
/* Supplies attachment semantics through the separately tested resource metadata boundary. */
static VkAttachmentDescription attachments[3];

static uint32_t peer_word(const uint8_t *bytes, size_t size, size_t *cursor);
static uint64_t peer_long(const uint8_t *bytes, size_t size, size_t *cursor);
static void put_word(uint8_t *bytes, size_t *cursor, uint32_t value);
static void put_long(uint8_t *bytes, size_t *cursor, uint64_t value);
static struct peer_buffer *peer_find(uint64_t identity);
static void expect_begin(uint32_t opcode);
static void expect_word(uint32_t value);
static void expect_long(uint64_t value);
static void expect_zero_words(unsigned count);
static void expect_layers(const VkImageSubresourceLayers *layers);
static void expect_offset(const VkOffset3D *offset);
static void expect_extent(const VkExtent3D *extent);
static void expect_range(const VkImageSubresourceRange *range);
static void expect_color(void);
static struct vulkan_object *test_object(enum vulkan_object_kind kind, uint64_t identity);
static void *VKAPI_PTR test_allocate(void *argument, size_t size, size_t alignment, VkSystemAllocationScope scope);
static void *VKAPI_PTR test_reallocate(void *argument, void *original, size_t size, size_t alignment, VkSystemAllocationScope scope);
static void VKAPI_PTR test_free(void *argument, void *allocation);
static void test_recording(VkCommandBuffer buffer);
static void test_lifecycle(void);

/*
 * Implements only the protocol peer's ownership and recording transitions.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t capacity,
	struct vulkan_reader *reader)
{
	const uint8_t *bytes;
	uint8_t *reply;
	struct peer_buffer *buffer;
	uint64_t identity;
	uint64_t present;
	uint64_t returned[PEER_BUFFERS];
	uint32_t opcode;
	uint32_t level;
	uint32_t count;
	uint32_t index;
	uint32_t flags;
	size_t cursor;
	size_t out;
	VkResult status;

	/* Matches the real context boundary's sticky namespace-loss rejection. */
	vulkan_reader_init(reader, NULL, 0);
	status = __atomic_load_n(&context->error, __ATOMIC_ACQUIRE);
	if (status != VK_SUCCESS)
		return status;

	/* Decodes the actual submitted bytes with independent fixed-width peer primitives. */
	bytes = writer->data;
	cursor = 0;
	opcode = peer_word(bytes, writer->bytes, &cursor);
	assert(peer_word(bytes, writer->bytes, &cursor) == 1);
	peer_calls++;
	reply = calloc(1, capacity);
	assert(reply != NULL);
	out = 0;
	put_word(reply, &out, opcode);

	/* Recording requests all begin with one native command-buffer identity. */
	if (opcode >= 93 && opcode <= 136) {
		identity = peer_long(bytes, writer->bytes, &cursor);
		buffer = peer_find(identity);
		assert(buffer != NULL && buffer->state == 1);
		assert(expected_pending && expected_opcode == opcode);
		assert(writer->bytes - cursor == expected_bytes);
		assert(memcmp(bytes + cursor, expected, expected_bytes) == 0);
		expected_pending = 0;
		record_seen[opcode - 93]++;
		vulkan_reader_init(reader, reply, out);
		return VK_SUCCESS;
	}

	/* Implements the pinned numeric lifecycle operations independently of production opcodes. */
	switch (opcode) {
	case 85:
		assert(peer_long(bytes, writer->bytes, &cursor) == 1000);
		assert(peer_long(bytes, writer->bytes, &cursor) == 1);
		assert(peer_word(bytes, writer->bytes, &cursor) == 39);
		assert(peer_long(bytes, writer->bytes, &cursor) == 0);
		assert(peer_word(bytes, writer->bytes, &cursor) == 3);
		assert(peer_word(bytes, writer->bytes, &cursor) == 2);
		assert(peer_long(bytes, writer->bytes, &cursor) == 0);
		assert(peer_long(bytes, writer->bytes, &cursor) == 1);
		peer_pool = peer_long(bytes, writer->bytes, &cursor);
		put_word(reply, &out, 0);
		put_long(reply, &out, 1);
		put_long(reply, &out, peer_pool);
		break;
	case 88:
		assert(peer_long(bytes, writer->bytes, &cursor) == 1000);
		assert(peer_long(bytes, writer->bytes, &cursor) == 1);
		assert(peer_word(bytes, writer->bytes, &cursor) == 40);
		assert(peer_long(bytes, writer->bytes, &cursor) == 0);
		assert(peer_long(bytes, writer->bytes, &cursor) == peer_pool);
		level = peer_word(bytes, writer->bytes, &cursor);
		count = peer_word(bytes, writer->bytes, &cursor);
		assert(count <= PEER_BUFFERS);
		assert(peer_long(bytes, writer->bytes, &cursor) == count);
		for (index = 0; index < count; index++) {
			identity = peer_long(bytes, writer->bytes, &cursor);
			assert(identity != 0 && peer_find(identity) == NULL);
			buffer = peer_find(0);
			assert(buffer != NULL);
			buffer->identity = identity;
			buffer->level = level;
			buffer->state = 0;
			returned[index] = identity;
		}
		put_word(reply, &out, 0);
		put_long(reply, &out, count);
		for (index = 0; index < count; index++) {
			put_long(reply, &out, returned[index]);
		}
		break;
	case 90:
		identity = peer_long(bytes, writer->bytes, &cursor);
		buffer = peer_find(identity);
		assert(buffer != NULL && buffer->state != 1);
		assert(peer_long(bytes, writer->bytes, &cursor) == 1);
		assert(peer_word(bytes, writer->bytes, &cursor) == 42);
		assert(peer_long(bytes, writer->bytes, &cursor) == 0);
		flags = peer_word(bytes, writer->bytes, &cursor);
		present = peer_long(bytes, writer->bytes, &cursor);
		if (buffer->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
			assert(present == 0);
		} else {
			assert(present == 1);
			assert(peer_word(bytes, writer->bytes, &cursor) == 41);
			assert(peer_long(bytes, writer->bytes, &cursor) == 0);
			assert(peer_long(bytes, writer->bytes, &cursor) == 0);
			assert(peer_word(bytes, writer->bytes, &cursor) == 0);
			assert(peer_long(bytes, writer->bytes, &cursor) == 0);
			assert(peer_word(bytes, writer->bytes, &cursor) == 0);
			assert(peer_word(bytes, writer->bytes, &cursor) == 0);
			assert(peer_word(bytes, writer->bytes, &cursor) == 0);
			assert(!(flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT));
		}
		buffer->state = 1;
		put_word(reply, &out, 0);
		break;
	case 91:
		identity = peer_long(bytes, writer->bytes, &cursor);
		buffer = peer_find(identity);
		assert(buffer != NULL && buffer->state == 1);
		buffer->state = 2;
		put_word(reply, &out, 0);
		break;
	case 92:
		identity = peer_long(bytes, writer->bytes, &cursor);
		buffer = peer_find(identity);
		assert(buffer != NULL);
		assert(peer_word(bytes, writer->bytes, &cursor) <= 1);
		buffer->state = 0;
		put_word(reply, &out, 0);
		break;
	case 87:
		assert(peer_long(bytes, writer->bytes, &cursor) == 1000);
		assert(peer_long(bytes, writer->bytes, &cursor) == peer_pool);
		assert(peer_word(bytes, writer->bytes, &cursor) == 1);
		for (index = 0; index < PEER_BUFFERS; index++) {
			peer_buffers[index].state = 0;
		}
		put_word(reply, &out, 0);
		break;
	case 89:
		assert(peer_long(bytes, writer->bytes, &cursor) == 1000);
		assert(peer_long(bytes, writer->bytes, &cursor) == peer_pool);
		count = peer_word(bytes, writer->bytes, &cursor);
		assert(peer_long(bytes, writer->bytes, &cursor) == count);
		for (index = 0; index < count; index++) {
			identity = peer_long(bytes, writer->bytes, &cursor);
			if (identity != 0) {
				buffer = peer_find(identity);
				assert(buffer != NULL);
				memset(buffer, 0, sizeof(*buffer));
			}
		}
		break;
	case 86:
		assert(peer_long(bytes, writer->bytes, &cursor) == 1000);
		assert(peer_long(bytes, writer->bytes, &cursor) == peer_pool);
		assert(peer_long(bytes, writer->bytes, &cursor) == 0);
		memset(peer_buffers, 0, sizeof(peer_buffers));
		peer_pool = 0;
		break;
	default:
		assert(0);
		break;
	}

	/* Every input byte belongs to a declared field and every output byte fits the promised capacity. */
	assert(cursor == writer->bytes);
	assert(out <= capacity);
	vulkan_reader_init(reader, reply, out);

	/* Succeeded: the response reflects real peer lifetime transitions rather than copied request data. */
	return VK_SUCCESS;
}

/*
 * Supplies immutable render-pass metadata independently of command clear encoding.
 */
const VkAttachmentDescription *
vulkan_render_pass_attachment(
	VkRenderPass render_pass,
	uint32_t index)
{
	/* The resource family's separate fixture verifies metadata creation and ownership. */
	assert(render_pass != VK_NULL_HANDLE);
	if (index >= 3)
		return NULL;

	/* Succeeded: the command test receives a stable independent attachment definition. */
	return &attachments[index];
}

/* Reads a little-endian word without production decoder helpers. */
static uint32_t
peer_word(
	const uint8_t *bytes,
	size_t size,
	size_t *cursor)
{
	uint32_t value;

	/* Checks the complete independent peer field before reading any bytes. */
	assert(*cursor <= size && size - *cursor >= 4);
	value = bytes[*cursor];
	value |= (uint32_t)bytes[*cursor + 1] << 8;
	value |= (uint32_t)bytes[*cursor + 2] << 16;
	value |= (uint32_t)bytes[*cursor + 3] << 24;
	*cursor += 4;

	/* Succeeded: returns one exact pinned-protocol word. */
	return value;
}

/* Reads a little-endian scalar with independent bounds and byte order. */
static uint64_t
peer_long(
	const uint8_t *bytes,
	size_t size,
	size_t *cursor)
{
	uint64_t low;
	uint64_t high;

	/* Composes two individually checked words in the protocol's declared order. */
	low = peer_word(bytes, size, cursor);
	high = peer_word(bytes, size, cursor);

	/* Succeeded: no native structure layout influenced the decoded scalar. */
	return low | (high << 32);
}

/* Writes one independent fixed-width expected or reply word. */
static void
put_word(
	uint8_t *bytes,
	size_t *cursor,
	uint32_t value)
{
	/* Stores explicit wire byte order without reusing production encoding code. */
	bytes[(*cursor)++] = (uint8_t)value;
	bytes[(*cursor)++] = (uint8_t)(value >> 8);
	bytes[(*cursor)++] = (uint8_t)(value >> 16);
	bytes[(*cursor)++] = (uint8_t)(value >> 24);

	/* Succeeded: four expected bytes were appended. */
	return;
}

/* Writes one independent 64-bit expected or reply scalar. */
static void
put_long(
	uint8_t *bytes,
	size_t *cursor,
	uint64_t value)
{
	/* Appends the low word before the high word regardless of host pointer width. */
	put_word(bytes, cursor, (uint32_t)value);
	put_word(bytes, cursor, (uint32_t)(value >> 32));

	/* Succeeded: eight expected bytes were appended. */
	return;
}

/* Finds an existing native identity or a free zero-identity fixture slot. */
static struct peer_buffer *
peer_find(
	uint64_t identity)
{
	uint32_t index;

	/* Searches a fixture bound deliberately greater than previous implementation limits. */
	for (index = 0; index < PEER_BUFFERS; index++) {
		if (peer_buffers[index].identity == identity)
			return &peer_buffers[index];
	}

	/* Reports absence so callers can assert expected ownership independently. */
	return NULL;
}

/* Starts one complete hand-authored recording argument fixture. */
static void
expect_begin(
	uint32_t opcode)
{
	/* No prior expected record may be silently skipped by a wrapper. */
	assert(!expected_pending);
	expected_pending = 1;
	expected_opcode = opcode;
	expected_bytes = 0;

	/* Succeeded: independent expected arguments can now be appended. */
	return;
}

/* Appends one bounded expected protocol word. */
static void
expect_word(
	uint32_t value)
{
	/* Keeps malformed fixture data from overflowing its own expected-byte storage. */
	assert(expected_bytes + 4 <= sizeof(expected));
	put_word(expected, &expected_bytes, value);

	/* Succeeded: the expected word belongs to this one pending record. */
	return;
}

/* Appends one bounded expected protocol scalar. */
static void
expect_long(
	uint64_t value)
{
	/* Checks both expected words before appending either one. */
	assert(expected_bytes + 8 <= sizeof(expected));
	put_long(expected, &expected_bytes, value);

	/* Succeeded: the expected scalar has an independent exact width. */
	return;
}

/* Appends canonical ignored fields to an expected protocol record. */
static void
expect_zero_words(
	unsigned count)
{
	unsigned index;

	/* Explicit zeros detect accidental reads of caller poison values in ignored fields. */
	for (index = 0; index < count; index++) {
		expect_word(0);
	}

	/* Succeeded: the ignored record portion has deterministic expected bytes. */
	return;
}

/* Encodes independent subresource-layer field order for copy fixtures. */
static void
expect_layers(
	const VkImageSubresourceLayers *layers)
{
	/* Vulkan field order determines the independent expected primitive sequence. */
	expect_word(layers->aspectMask);
	expect_word(layers->mipLevel);
	expect_word(layers->baseArrayLayer);
	expect_word(layers->layerCount);

	/* Succeeded: the expected layer record contains no native padding. */
	return;
}

/* Encodes signed offset representations in an independent copy-region fixture. */
static void
expect_offset(
	const VkOffset3D *offset)
{
	/* Signed values retain their two's-complement word representation on the wire. */
	expect_word((uint32_t)offset->x);
	expect_word((uint32_t)offset->y);
	expect_word((uint32_t)offset->z);

	/* Succeeded: all offset coordinates have exact expected representations. */
	return;
}

/* Encodes one independent three-dimensional extent fixture. */
static void
expect_extent(
	const VkExtent3D *extent)
{
	/* The extent is three declared words, independent of any nested parent structure. */
	expect_word(extent->width);
	expect_word(extent->height);
	expect_word(extent->depth);

	/* Succeeded: the expected extent has no additional array or pointer framing. */
	return;
}

/* Encodes one independent image-range fixture. */
static void
expect_range(
	const VkImageSubresourceRange *range)
{
	/* Includes each aspect and subresource selector in official field order. */
	expect_word(range->aspectMask);
	expect_word(range->baseMipLevel);
	expect_word(range->levelCount);
	expect_word(range->baseArrayLayer);
	expect_word(range->layerCount);

	/* Succeeded: the fixture expects the complete selected image range. */
	return;
}

/* Encodes a known floating-point color's union tag, array length, and exact IEEE bits. */
static void
expect_color(
	void)
{
	/* The unsigned wire branch preserves the application's full float-color representation. */
	expect_word(2);
	expect_long(4);
	expect_word(0x3f800000U);
	expect_word(0x3e800000U);
	expect_word(0x3f000000U);
	expect_word(0x3f800000U);

	/* Succeeded: a numeric float-to-integer conversion would fail this independent fixture. */
	return;
}

/* Creates a local prerequisite object whose native ID is independent of its user pointer. */
static struct vulkan_object *
test_object(
	enum vulkan_object_kind kind,
	uint64_t identity)
{
	struct vulkan_object *object;
	VkResult status;

	/* Only ordinary handle lookup is under test; resource creation has its own actual-source fixture. */
	status = vulkan_object_alloc(sizeof(*object), 16, kind, NULL, NULL, NULL, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	assert(status == VK_SUCCESS);
	object->wire_id = identity;

	/* Succeeded: expected wire IDs cannot accidentally match arbitrary object addresses. */
	return object;
}

/* Supplies counted aligned application callback storage with controlled failure. */
static void *VKAPI_PTR
test_allocate(
	void *argument,
	size_t size,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	struct allocation_state *state;
	void *allocation;
	int status;

	/* Refusal happens before allocating or changing callback ownership accounting. */
	state = argument;
	(void)scope;
	if (state->fail)
		return NULL;

	/* POSIX requires at least pointer alignment for the independent callback allocation. */
	if (alignment < sizeof(void *))
		alignment = sizeof(void *);
	status = posix_memalign(&allocation, alignment, size);
	assert(status == 0);
	state->allocations++;

	/* Succeeded: the returned storage satisfies the requested application alignment. */
	return allocation;
}

/* Supplies a valid reallocation callback that may legally report host memory exhaustion. */
static void *VKAPI_PTR
test_reallocate(
	void *argument,
	void *original,
	size_t size,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	/* These wrappers use allocate-copy-free growth; accidental reallocation is deliberately visible. */
	(void)argument;
	(void)original;
	(void)size;
	(void)alignment;
	(void)scope;
	assert(0);

	/* A refused reallocation leaves original ownership with the caller. */
	return NULL;
}

/* Reclaims callback-owned storage without treating permitted free(NULL) calls as allocations. */
static void VKAPI_PTR
test_free(
	void *argument,
	void *allocation)
{
	struct allocation_state *state;

	/* Null callback frees are valid and consume no ownership. */
	if (allocation == NULL)
		return;

	/* Counts only actual callback allocations returned by this fixture. */
	state = argument;
	state->frees++;
	free(allocation);

	/* Succeeded: the application allocation is no longer live. */
	return;
}

/* Verifies every recording entry point against independent exact protocol parameter bytes. */
static void
test_recording(
	VkCommandBuffer buffer)
{
	struct vulkan_object *objects[10];
	VkBuffer gpu_buffer;
	VkImage image;
	VkPipeline pipeline;
	VkPipelineLayout layout;
	VkDescriptorSet descriptor;
	VkEvent event;
	VkQueryPool query;
	VkRenderPass render_pass;
	VkFramebuffer framebuffer;
	VkCommandBuffer secondary;
	VkDeviceSize offsets[1];
	uint32_t dynamic_offset;
	uint32_t payload[2];
	float blend[4];
	VkViewport viewport;
	VkRect2D scissor;
	VkBufferCopy buffer_copy;
	VkImageCopy image_copy;
	VkImageBlit blit;
	VkBufferImageCopy buffer_image;
	VkImageResolve resolve;
	VkImageSubresourceRange range;
	VkClearColorValue clear_color;
	VkClearDepthStencilValue clear_depth;
	VkClearAttachment clear_attachment;
	VkClearRect clear_rect;
	VkMemoryBarrier memory_barrier;
	VkBufferMemoryBarrier buffer_barrier;
	VkImageMemoryBarrier image_barrier;
	VkRenderPassBeginInfo render_begin;
	VkClearValue clear_values[4];
	uint32_t index;

	/* Uses unrelated user pointers and native identities for every handle-bearing family. */
	objects[0] = test_object(VULKAN_OBJECT_BUFFER, 501);
	objects[1] = test_object(VULKAN_OBJECT_IMAGE, 502);
	objects[2] = test_object(VULKAN_OBJECT_PIPELINE, 503);
	objects[3] = test_object(VULKAN_OBJECT_PIPELINE_LAYOUT, 504);
	objects[4] = test_object(VULKAN_OBJECT_DESCRIPTOR_SET, 505);
	objects[5] = test_object(VULKAN_OBJECT_EVENT, 506);
	objects[6] = test_object(VULKAN_OBJECT_QUERY_POOL, 507);
	objects[7] = test_object(VULKAN_OBJECT_RENDER_PASS, 508);
	objects[8] = test_object(VULKAN_OBJECT_FRAMEBUFFER, 509);
	objects[9] = test_object(VULKAN_OBJECT_COMMAND_BUFFER, 510);
	gpu_buffer = (VkBuffer)(uintptr_t)objects[0];
	image = (VkImage)(uintptr_t)objects[1];
	pipeline = (VkPipeline)(uintptr_t)objects[2];
	layout = (VkPipelineLayout)(uintptr_t)objects[3];
	descriptor = (VkDescriptorSet)(uintptr_t)objects[4];
	event = (VkEvent)(uintptr_t)objects[5];
	query = (VkQueryPool)(uintptr_t)objects[6];
	render_pass = (VkRenderPass)(uintptr_t)objects[7];
	framebuffer = (VkFramebuffer)(uintptr_t)objects[8];
	secondary = (VkCommandBuffer)objects[9];

	/* Pipeline and dynamic viewport commands must preserve native handles and floating-point bits. */
	expect_begin(93);
	expect_word(0);
	expect_long(503);
	vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = 320.0f;
	viewport.height = 240.0f;
	viewport.maxDepth = 1.0f;
	expect_begin(94);
	expect_word(2);
	expect_word(1);
	expect_long(1);
	expect_zero_words(2);
	expect_word(0x43a00000U);
	expect_word(0x43700000U);
	expect_word(0);
	expect_word(0x3f800000U);
	vkCmdSetViewport(buffer, 2, 1, &viewport);

	/* A signed scissor offset remains signed wire bits inside its nested structure. */
	scissor.offset.x = -2;
	scissor.offset.y = 3;
	scissor.extent.width = 320;
	scissor.extent.height = 240;
	expect_begin(95);
	expect_word(4);
	expect_word(1);
	expect_long(1);
	expect_word(0xfffffffeU);
	expect_word(3);
	expect_word(320);
	expect_word(240);
	vkCmdSetScissor(buffer, 4, 1, &scissor);

	/* Scalar float commands must encode IEEE representations rather than integer conversions. */
	expect_begin(96);
	expect_word(0x3f800000U);
	vkCmdSetLineWidth(buffer, 1.0f);
	expect_begin(97);
	expect_word(0x3f800000U);
	expect_word(0);
	expect_word(0x40000000U);
	vkCmdSetDepthBias(buffer, 1.0f, 0.0f, 2.0f);
	blend[0] = 1.0f;
	blend[1] = 0.25f;
	blend[2] = 0.5f;
	blend[3] = 1.0f;
	expect_begin(98);
	expect_long(4);
	expect_word(0x3f800000U);
	expect_word(0x3e800000U);
	expect_word(0x3f000000U);
	expect_word(0x3f800000U);
	vkCmdSetBlendConstants(buffer, blend);
	expect_begin(99);
	expect_word(0);
	expect_word(0x3f800000U);
	vkCmdSetDepthBounds(buffer, 0.0f, 1.0f);

	/* Separate stencil operations retain their own opcodes, masks, and values. */
	expect_begin(100);
	expect_word(3);
	expect_word(0xff);
	vkCmdSetStencilCompareMask(buffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
	expect_begin(101);
	expect_word(1);
	expect_word(0x7f);
	vkCmdSetStencilWriteMask(buffer, VK_STENCIL_FACE_FRONT_BIT, 0x7f);
	expect_begin(102);
	expect_word(2);
	expect_word(5);
	vkCmdSetStencilReference(buffer, VK_STENCIL_FACE_BACK_BIT, 5);

	/* Descriptor and vertex binding include independent count markers for every pointed-to array. */
	dynamic_offset = 256;
	expect_begin(103);
	expect_word(0);
	expect_long(504);
	expect_word(2);
	expect_word(1);
	expect_long(1);
	expect_long(505);
	expect_word(1);
	expect_long(1);
	expect_word(256);
	vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &descriptor, 1, &dynamic_offset);
	expect_begin(104);
	expect_long(501);
	expect_long(UINT64_C(0x100000080));
	expect_word(1);
	vkCmdBindIndexBuffer(buffer, gpu_buffer, UINT64_C(0x100000080), VK_INDEX_TYPE_UINT32);
	offsets[0] = UINT64_C(0x200000040);
	expect_begin(105);
	expect_word(3);
	expect_word(1);
	expect_long(1);
	expect_long(501);
	expect_long(1);
	expect_long(offsets[0]);
	vkCmdBindVertexBuffers(buffer, 3, 1, &gpu_buffer, offsets);

	/* Direct and indirect drawing distinguish signed vertex offsets and full-width byte offsets. */
	expect_begin(106);
	expect_word(36);
	expect_word(2);
	expect_word(4);
	expect_word(5);
	vkCmdDraw(buffer, 36, 2, 4, 5);
	expect_begin(107);
	expect_word(36);
	expect_word(2);
	expect_word(4);
	expect_word(0xfffffffdU);
	expect_word(5);
	vkCmdDrawIndexed(buffer, 36, 2, 4, -3, 5);
	expect_begin(108);
	expect_long(501);
	expect_long(256);
	expect_word(7);
	expect_word(16);
	vkCmdDrawIndirect(buffer, gpu_buffer, 256, 7, 16);
	expect_begin(109);
	expect_long(501);
	expect_long(512);
	expect_word(9);
	expect_word(20);
	vkCmdDrawIndexedIndirect(buffer, gpu_buffer, 512, 9, 20);

	/* Compute dispatch carries real caller work-group dimensions and indirect storage offsets. */
	expect_begin(110);
	expect_word(3);
	expect_word(5);
	expect_word(7);
	vkCmdDispatch(buffer, 3, 5, 7);
	expect_begin(111);
	expect_long(501);
	expect_long(1024);
	vkCmdDispatchIndirect(buffer, gpu_buffer, 1024);

	/* Buffer copies preserve every 64-bit region member and its surrounding array marker. */
	buffer_copy.srcOffset = 64;
	buffer_copy.dstOffset = 128;
	buffer_copy.size = 256;
	expect_begin(112);
	expect_long(501);
	expect_long(501);
	expect_word(1);
	expect_long(1);
	expect_long(64);
	expect_long(128);
	expect_long(256);
	vkCmdCopyBuffer(buffer, gpu_buffer, gpu_buffer, 1, &buffer_copy);

	/* Image copy structure fields remain correctly nested and PRESENT maps to local GENERAL. */
	memset(&image_copy, 0, sizeof(image_copy));
	image_copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	image_copy.srcSubresource.mipLevel = 2;
	image_copy.srcSubresource.baseArrayLayer = 3;
	image_copy.srcSubresource.layerCount = 1;
	image_copy.dstSubresource = image_copy.srcSubresource;
	image_copy.srcOffset.x = -1;
	image_copy.srcOffset.y = 2;
	image_copy.srcOffset.z = 3;
	image_copy.dstOffset.x = 4;
	image_copy.dstOffset.y = 5;
	image_copy.dstOffset.z = 6;
	image_copy.extent.width = 7;
	image_copy.extent.height = 8;
	image_copy.extent.depth = 9;
	expect_begin(113);
	expect_long(502);
	expect_word(1);
	expect_long(502);
	expect_word(7);
	expect_word(1);
	expect_long(1);
	expect_layers(&image_copy.srcSubresource);
	expect_offset(&image_copy.srcOffset);
	expect_layers(&image_copy.dstSubresource);
	expect_offset(&image_copy.dstOffset);
	expect_extent(&image_copy.extent);
	vkCmdCopyImage(buffer, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_copy);

	/* Blit fixed two-element offset arrays require their own two-element framing. */
	blit.srcSubresource = image_copy.srcSubresource;
	blit.dstSubresource = image_copy.dstSubresource;
	blit.srcOffsets[0] = image_copy.srcOffset;
	blit.srcOffsets[1] = image_copy.dstOffset;
	blit.dstOffsets[0] = image_copy.dstOffset;
	blit.dstOffsets[1] = image_copy.srcOffset;
	expect_begin(114);
	expect_long(502);
	expect_word(6);
	expect_long(502);
	expect_word(7);
	expect_word(1);
	expect_long(1);
	expect_layers(&blit.srcSubresource);
	expect_long(2);
	expect_offset(&blit.srcOffsets[0]);
	expect_offset(&blit.srcOffsets[1]);
	expect_layers(&blit.dstSubresource);
	expect_long(2);
	expect_offset(&blit.dstOffsets[0]);
	expect_offset(&blit.dstOffsets[1]);
	expect_word(0);
	vkCmdBlitImage(buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

	/* Both buffer/image copy directions use the same actual region layout and distinct argument order. */
	buffer_image.bufferOffset = 512;
	buffer_image.bufferRowLength = 32;
	buffer_image.bufferImageHeight = 16;
	buffer_image.imageSubresource = image_copy.srcSubresource;
	buffer_image.imageOffset = image_copy.srcOffset;
	buffer_image.imageExtent = image_copy.extent;
	expect_begin(115);
	expect_long(501);
	expect_long(502);
	expect_word(7);
	expect_word(1);
	expect_long(1);
	expect_long(512);
	expect_word(32);
	expect_word(16);
	expect_layers(&buffer_image.imageSubresource);
	expect_offset(&buffer_image.imageOffset);
	expect_extent(&buffer_image.imageExtent);
	vkCmdCopyBufferToImage(buffer, gpu_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &buffer_image);
	expect_begin(116);
	expect_long(502);
	expect_word(6);
	expect_long(501);
	expect_word(1);
	expect_long(1);
	expect_long(512);
	expect_word(32);
	expect_word(16);
	expect_layers(&buffer_image.imageSubresource);
	expect_offset(&buffer_image.imageOffset);
	expect_extent(&buffer_image.imageExtent);
	vkCmdCopyImageToBuffer(buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gpu_buffer, 1, &buffer_image);

	/* Opaque update and fill payloads keep full-width storage offsets and exact caller bytes. */
	payload[0] = 0x12345678;
	payload[1] = 0x9abcdef0;
	expect_begin(117);
	expect_long(501);
	expect_long(64);
	expect_long(8);
	expect_long(8);
	expect_word(payload[0]);
	expect_word(payload[1]);
	vkCmdUpdateBuffer(buffer, gpu_buffer, 64, sizeof(payload), payload);
	expect_begin(118);
	expect_long(501);
	expect_long(128);
	expect_long(256);
	expect_word(0xaabbccdd);
	vkCmdFillBuffer(buffer, gpu_buffer, 128, 256, 0xaabbccdd);

	/* Tagged color clears preserve float bits, range counts, and all range selectors. */
	memcpy(clear_color.float32, blend, sizeof(blend));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 2;
	range.levelCount = 3;
	range.baseArrayLayer = 4;
	range.layerCount = 5;
	expect_begin(119);
	expect_long(502);
	expect_word(1);
	expect_long(1);
	expect_color();
	expect_word(1);
	expect_long(1);
	expect_range(&range);
	vkCmdClearColorImage(buffer, image, VK_IMAGE_LAYOUT_GENERAL, &clear_color, 1, &range);

	/* A stencil-only clear must canonicalize the ignored depth member instead of encoding its poison. */
	range.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
	clear_depth.depth = -1234.0f;
	clear_depth.stencil = 73;
	expect_begin(120);
	expect_long(502);
	expect_word(1);
	expect_long(1);
	expect_word(0);
	expect_word(73);
	expect_word(1);
	expect_long(1);
	expect_range(&range);
	vkCmdClearDepthStencilImage(buffer, image, VK_IMAGE_LAYOUT_GENERAL, &clear_depth, 1, &range);

	/* Depth-only attachment clears ignore both colorAttachment and the unused stencil field. */
	memset(&clear_attachment, 0xa5, sizeof(clear_attachment));
	clear_attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	clear_attachment.clearValue.depthStencil.depth = 0.5f;
	clear_rect.rect = scissor;
	clear_rect.baseArrayLayer = 2;
	clear_rect.layerCount = 3;
	expect_begin(121);
	expect_word(1);
	expect_long(1);
	expect_word(2);
	expect_word(0);
	expect_word(1);
	expect_word(0x3f000000U);
	expect_word(0);
	expect_word(1);
	expect_long(1);
	expect_word(0xfffffffeU);
	expect_word(3);
	expect_word(320);
	expect_word(240);
	expect_word(2);
	expect_word(3);
	vkCmdClearAttachments(buffer, 1, &clear_attachment, 1, &clear_rect);

	/* Resolve regions share value fields with image copies but retain their own native opcode. */
	resolve.srcSubresource = image_copy.srcSubresource;
	resolve.srcOffset = image_copy.srcOffset;
	resolve.dstSubresource = image_copy.dstSubresource;
	resolve.dstOffset = image_copy.dstOffset;
	resolve.extent = image_copy.extent;
	expect_begin(122);
	expect_long(502);
	expect_word(6);
	expect_long(502);
	expect_word(7);
	expect_word(1);
	expect_long(1);
	expect_layers(&resolve.srcSubresource);
	expect_offset(&resolve.srcOffset);
	expect_layers(&resolve.dstSubresource);
	expect_offset(&resolve.dstOffset);
	expect_extent(&resolve.extent);
	vkCmdResolveImage(buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);

	/* Recorded event operations retain event identities and stage masks. */
	expect_begin(123);
	expect_long(506);
	expect_word(0x1000);
	vkCmdSetEvent(buffer, event, VK_PIPELINE_STAGE_TRANSFER_BIT);
	expect_begin(124);
	expect_long(506);
	expect_word(0x1000);
	vkCmdResetEvent(buffer, event, VK_PIPELINE_STAGE_TRANSFER_BIT);

	/* Barrier arrays contain full standard sType/pNext and native handle/layout fields. */
	memset(&memory_barrier, 0, sizeof(memory_barrier));
	memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	memory_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	memset(&buffer_barrier, 0, sizeof(buffer_barrier));
	buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	buffer_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.buffer = gpu_buffer;
	buffer_barrier.offset = 32;
	buffer_barrier.size = 64;
	memset(&image_barrier, 0, sizeof(image_barrier));
	image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	image_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	image_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image_barrier.image = image;
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	image_barrier.subresourceRange = range;
	for (index = 0; index < 2; index++) {
		expect_begin(125 + index);
		if (index == 0) {
			expect_word(1);
			expect_long(1);
			expect_long(506);
		}
		expect_word(0x1000);
		expect_word(0x4000);
		if (index != 0)
			expect_word(1);
		expect_word(1);
		expect_long(1);
		expect_word(46);
		expect_long(0);
		expect_word(0x1000);
		expect_word(0x2000);
		expect_word(1);
		expect_long(1);
		expect_word(44);
		expect_long(0);
		expect_word(0x1000);
		expect_word(0x2000);
		expect_word(UINT32_MAX);
		expect_word(UINT32_MAX);
		expect_long(501);
		expect_long(32);
		expect_long(64);
		expect_word(1);
		expect_long(1);
		expect_word(45);
		expect_long(0);
		expect_word(0x1000);
		expect_word(0x20);
		expect_word(7);
		expect_word(1);
		expect_word(UINT32_MAX);
		expect_word(UINT32_MAX);
		expect_long(502);
		expect_range(&range);
		if (index == 0) {
			vkCmdWaitEvents(buffer, 1, &event, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 1, &memory_barrier, 1, &buffer_barrier, 1, &image_barrier);
		} else {
			vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_DEPENDENCY_BY_REGION_BIT, 1, &memory_barrier, 1, &buffer_barrier, 1, &image_barrier);
		}
	}

	/* Query recording commands retain independent indices, flags, and buffer destinations. */
	expect_begin(127);
	expect_long(507);
	expect_word(3);
	expect_word(1);
	vkCmdBeginQuery(buffer, query, 3, VK_QUERY_CONTROL_PRECISE_BIT);
	expect_begin(128);
	expect_long(507);
	expect_word(3);
	vkCmdEndQuery(buffer, query, 3);
	expect_begin(129);
	expect_long(507);
	expect_word(2);
	expect_word(4);
	vkCmdResetQueryPool(buffer, query, 2, 4);
	expect_begin(130);
	expect_word(0x2000);
	expect_long(507);
	expect_word(5);
	vkCmdWriteTimestamp(buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query, 5);
	expect_begin(131);
	expect_long(507);
	expect_word(2);
	expect_word(4);
	expect_long(501);
	expect_long(128);
	expect_long(16);
	expect_word(3);
	vkCmdCopyQueryPoolResults(buffer, query, 2, 4, gpu_buffer, 128, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

	/* Push constants are arbitrary caller bytes rather than fixed rendering matrices or timestamps. */
	expect_begin(132);
	expect_long(504);
	expect_word(0x11);
	expect_word(16);
	expect_word(8);
	expect_long(8);
	expect_word(payload[0]);
	expect_word(payload[1]);
	vkCmdPushConstants(buffer, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 16, sizeof(payload), payload);

	/* Render-pass clear interpretation is independently selected by each attachment's format/loadOps. */
	memset(attachments, 0, sizeof(attachments));
	attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[2].format = VK_FORMAT_R8G8B8A8_UNORM;
	attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	memset(clear_values, 0xa5, sizeof(clear_values));
	clear_values[0].color = clear_color;
	clear_values[1].depthStencil.depth = 0.5f;
	memset(&render_begin, 0, sizeof(render_begin));
	render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	render_begin.renderPass = render_pass;
	render_begin.framebuffer = framebuffer;
	render_begin.renderArea = scissor;
	render_begin.clearValueCount = 4;
	render_begin.pClearValues = clear_values;
	expect_begin(133);
	expect_long(1);
	expect_word(43);
	expect_long(0);
	expect_long(508);
	expect_long(509);
	expect_word(0xfffffffeU);
	expect_word(3);
	expect_word(320);
	expect_word(240);
	expect_word(4);
	expect_long(4);
	expect_word(0);
	expect_color();
	expect_word(1);
	expect_word(0x3f000000U);
	expect_word(0);
	for (index = 0; index < 2; index++) {
		expect_word(0);
		expect_word(2);
		expect_long(4);
		expect_zero_words(4);
	}
	expect_word(0);
	vkCmdBeginRenderPass(buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
	expect_begin(134);
	expect_word(1);
	vkCmdNextSubpass(buffer, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
	expect_begin(135);
	vkCmdEndRenderPass(buffer);

	/* Secondary execution resolves every dispatchable command-buffer handle to its native ID. */
	expect_begin(136);
	expect_word(1);
	expect_long(1);
	expect_long(510);
	vkCmdExecuteCommands(buffer, 1, &secondary);

	/* Requires every declared recording entry point to have consumed its expected protocol fixture. */
	assert(!expected_pending);
	for (index = 0; index < 44; index++) {
		assert(record_seen[index] != 0);
	}

	/* Prerequisite handles remain independently owned after command bytes have been copied. */
	for (index = 0; index < 10; index++) {
		vulkan_object_free(objects[index]);
	}

	/* Succeeded: all forty-four recording entry points matched independent exact wire expectations. */
	return;
}

/* Verifies dynamic batches, implicit pool frees, ignored inheritance, and error/reset recovery. */
static void
test_lifecycle(
	void)
{
	struct VkDevice_T device;
	struct vulkan_context context;
	struct allocation_state allocation;
	VkAllocationCallbacks callbacks;
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo allocate_info;
	VkCommandBufferBeginInfo begin;
	VkCommandBufferInheritanceInfo inheritance;
	VkCommandPool pool;
	VkCommandBuffer buffers[96];
	VkCommandBuffer secondary;
	VkCommandBuffer freed[2];
	struct vulkan_object *pool_object;
	struct vulkan_object *cursor;
	VkResult status;
	unsigned calls;
	unsigned count;
	unsigned index;

	/* Sets up one real local device context and a counted independent application allocator. */
	memset(&device, 0, sizeof(device));
	memset(&context, 0, sizeof(context));
	memset(&allocation, 0, sizeof(allocation));
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.pUserData = &allocation;
	callbacks.pfnAllocation = test_allocate;
	callbacks.pfnReallocation = test_reallocate;
	callbacks.pfnFree = test_free;
	device.object.kind = VULKAN_OBJECT_DEVICE;
	device.object.wire_id = 1000;
	device.object.context = &context;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = 2;
	status = vkCreateCommandPool((VkDevice)&device, &pool_info, &callbacks, &pool);
	assert(status == VK_SUCCESS);
	pool_object = vulkan_nondispatchable_object((uint64_t)pool);

	/* Allocates ninety-six primary buffers through actual code, exceeding the former fixed object ceiling. */
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate_info.commandPool = pool;
	allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate_info.commandBufferCount = 96;
	status = vkAllocateCommandBuffers((VkDevice)&device, &allocate_info, buffers);
	assert(status == VK_SUCCESS);
	count = 0;
	cursor = pool_object->first_child;
	while (cursor != NULL) {
		count++;
		cursor = cursor->next_sibling;
	}
	assert(count == 96);

	/* Primary Begin must not dereference the explicitly ignored poisoned inheritance pointer. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	begin.pInheritanceInfo = (const VkCommandBufferInheritanceInfo *)(uintptr_t)1;
	status = vkBeginCommandBuffer(buffers[0], &begin);
	assert(status == VK_SUCCESS);
	test_recording(buffers[0]);
	status = vkEndCommandBuffer(buffers[0]);
	assert(status == VK_SUCCESS);

	/* Allocation failure in a void record is retained, later records are skipped, and native End still runs. */
	status = vkBeginCommandBuffer(buffers[1], &begin);
	assert(status == VK_SUCCESS);
	calls = peer_calls;
	allocation.fail = 1;
	vkCmdDraw(buffers[1], 3, 1, 0, 0);
	assert(peer_calls == calls);
	allocation.fail = 0;
	vkCmdDraw(buffers[1], 3, 1, 0, 0);
	assert(peer_calls == calls);
	status = vkEndCommandBuffer(buffers[1]);
	assert(status == VK_ERROR_OUT_OF_HOST_MEMORY);
	assert(peer_calls == calls + 1);
	assert(context.error == VK_SUCCESS);

	/* Successful buffer reset clears the local failure and permits a new complete recording. */
	status = vkResetCommandBuffer(buffers[1], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	assert(status == VK_SUCCESS);
	status = vkBeginCommandBuffer(buffers[1], &begin);
	assert(status == VK_SUCCESS);
	expect_begin(106);
	expect_word(3);
	expect_word(1);
	expect_word(0);
	expect_word(0);
	vkCmdDraw(buffers[1], 3, 1, 0, 0);
	status = vkEndCommandBuffer(buffers[1]);
	assert(status == VK_SUCCESS);

	/* A secondary buffer keeps query inheritance while ignored render-pass pointers remain untouched. */
	allocate_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
	allocate_info.commandBufferCount = 1;
	status = vkAllocateCommandBuffers((VkDevice)&device, &allocate_info, &secondary);
	assert(status == VK_SUCCESS);
	memset(&inheritance, 0, sizeof(inheritance));
	inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
	inheritance.renderPass = (VkRenderPass)(uintptr_t)1;
	inheritance.framebuffer = (VkFramebuffer)(uintptr_t)1;
	inheritance.subpass = UINT32_MAX;
	inheritance.queryFlags = UINT32_MAX;
	begin.pInheritanceInfo = &inheritance;
	status = vkBeginCommandBuffer(secondary, &begin);
	assert(status == VK_SUCCESS);
	status = vkEndCommandBuffer(secondary);
	assert(status == VK_SUCCESS);

	/* Pool reset updates every live local and native command buffer without replacing its public identity. */
	status = vkResetCommandPool((VkDevice)&device, pool, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
	assert(status == VK_SUCCESS);
	for (index = 0; index < 96; index++) {
		assert(((struct VkCommandBuffer_T *)buffers[index])->error == VK_SUCCESS);
		assert(((struct VkCommandBuffer_T *)buffers[index])->state == 0);
	}

	/* Explicit free accepts a null element and leaves other buffers owned by the pool. */
	freed[0] = buffers[0];
	freed[1] = VK_NULL_HANDLE;
	vkFreeCommandBuffers((VkDevice)&device, pool, 2, freed);
	count = 0;
	cursor = pool_object->first_child;
	while (cursor != NULL) {
		count++;
		cursor = cursor->next_sibling;
	}
	assert(count == 96);

	/* Pool destruction implicitly frees all remaining local buffers and their native objects. */
	vkDestroyCommandPool((VkDevice)&device, pool, &callbacks);
	assert(peer_pool == 0);
	assert(device.object.first_child == NULL);
	assert(allocation.allocations == allocation.frees);

	/* Succeeded: native and callback ownership remain balanced across every tested lifecycle transition. */
	return;
}

/*
 * Runs actual Vulkan command implementation against bounded independent peer expectations.
 */
int
main(
	void)
{
	/* Executes all recording wrappers and the full pool/buffer lifecycle without a GPU dependency. */
	test_lifecycle();
	puts("libvulkan commands: 44 exact wire records, 97 buffers, ignored fields, recording failure/reset and implicit pool frees PASS");

	/* Succeeded: the focused command family gate passed without claiming native GPU conformance. */
	return 0;
}
