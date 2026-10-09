/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The actual libvulkan writer sends concrete transport records to the actual native server and shared Zlib codec. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/vulkan-private.h"
#include "userland/desktop/libvulkan/internal.h"

/* One fixture-only typed body exposes transport forwarding, reply cursor order and immutable external copies. */
#define TEST_BODY_OPCODE 0x10017U

/* Two fixture resources represent ordinary session-local blobs; physical allocation and native DMA are not simulated. */
static struct bcm2711_buffer buffers[2];
static struct bcm2711_v3d_view views[2];
static struct bcm2711_render_resource resources[2];

/* Word arrays provide naturally aligned reply and external-stream backing for actual atomic trailer stores. */
static uint32_t reply_words[64];
static uint32_t external_words[64];

/* Callback observations track forwarding and deliberately mutate only the original external client blob. */
static uint32_t dispatched;
static uint32_t mutate_external;

/* Only actual external-stream snapshot allocations enter this host accounting. */
static uint32_t allocations;

static void encode_selector(struct vulkan_writer *writer, uint64_t capacity);
static void encode_trailer(struct vulkan_writer *writer, uint64_t offset);
static void encode_external(struct vulkan_writer *writer, uint64_t bytes, uint32_t flags);
static void encode_body(struct vulkan_writer *writer, uint32_t bits);
static int dispatch_body(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

/*
 * Supplies actual server snapshot allocation through the ordinary host heap.
 */
void *
kern_malloc(
	size_t bytes)
{
	void *pointer;

	/* Every copied external stream remains separately visible to host ownership accounting. */
	pointer = malloc(bytes);
	if (pointer != NULL)
		allocations++;

	/* The returned storage is immutable only after the production server copies the client bytes. */
	return pointer;
}

/*
 * Releases actual copied external stream storage through the ordinary host heap.
 */
void
kern_free(
	void *pointer)
{
	/* The server must retire every successfully allocated snapshot exactly once. */
	assert(pointer != NULL && allocations != 0);
	allocations--;
	free(pointer);
}

/*
 * Resolves one ordinary fixture resource inside its renderer session namespace.
 */
struct bcm2711_render_resource *
bcm2711_render_find(
	struct bcm2711_render_session *session,
	uint32_t identifier)
{
	struct bcm2711_render_resource *resource;

	/* The actual resource manager is independently tested; this fixture supplies only its borrowed descriptor contract. */
	for (resource = session->resources; resource != NULL; resource = resource->next) {
		if (resource->identifier == identifier)
			return resource;
	}

	/* Unknown identifiers cannot select a reply or external-stream backing. */
	return NULL;
}

/*
 * Observes independently retained CPU reply backing without a physical allocator or GPU mapping.
 */
void
bcm2711_buffer_retain(
	struct bcm2711_buffer *buffer)
{
	/* A retained actual reply extent must already have a live resource owner. */
	assert(buffer != NULL && buffer->references != 0);
	buffer->references++;
}

/*
 * Observes exact CPU reply-backing retirement after the native server returns.
 */
void
bcm2711_buffer_release(
	struct bcm2711_buffer *buffer)
{
	/* The production buffer API requires a nonnull live owner even during selector and failure cleanup. */
	assert(buffer != NULL && buffer->references != 0);
	buffer->references--;
}

/*
 * Checks concrete client transport bytes, external snapshots, atomic completion placement and refusal cleanup.
 */
int
main(
	void)
{
	struct bcm2711_render_session render;
	struct bcm2711_vulkan_session session;
	struct vulkan_writer client;
	struct vulkan_writer external;
	uint32_t index;
	uint32_t before;
	int error;

	/* Fixture renderer resources provide immutable CPU alias extents to the actual server. */
	memset(&render, 0, sizeof(render));
	memset(&session, 0, sizeof(session));
	session.render = &render;
	for (index = 0; index < 2; index++) {
		buffers[index].references = 1;
		buffers[index].bytes = sizeof(reply_words);
		buffers[index].address = reply_words;
		if (index == 1)
			buffers[index].address = external_words;
		views[index].buffer = &buffers[index];
		resources[index].owner = &render;
		resources[index].identifier = index + 1;
		resources[index].view = &views[index];
		resources[index].blob = true;
	}

	/* Both resources are reachable only through this renderer session. */
	resources[0].next = &resources[1];
	render.resources = &resources[0];
	vulkan_writer_init(&client);
	vulkan_writer_init(&external);

	/* An ordinary client selector, result body and fixed-position trailer use their actual library primitives. */
	memset(reply_words, 0xa5, sizeof(reply_words));
	encode_selector(&client, 64);
	encode_body(&client, 0x12345678U);
	encode_trailer(&client, 44);
	assert(client.error == VK_SUCCESS);
	error = bcm2711_vulkan_stream_execute(&session, client.data, (uint32_t)client.bytes, dispatch_body);
	assert(error == 0 && dispatched == 1 && buffers[0].references == 1);
	assert(reply_words[4] == TEST_BODY_OPCODE && reply_words[5] == 0 && reply_words[6] == 0x12345678U);
	assert(reply_words[15] == GPU_OP_ENUMERATE_INSTANCE_VERSION && reply_words[16] == 0);
	assert(reply_words[17] == 1 && reply_words[18] == 0 && reply_words[19] == VK_MAKE_VERSION(1, 1, 0));
	assert(reply_words[3] == 0xa5a5a5a5U && reply_words[20] == 0xa5a5a5a5U);

	/* Repeated selection of identical backing retains the replacement before dropping its old independent owner. */
	client.bytes = 0;
	encode_selector(&client, 64);
	encode_selector(&client, 64);
	encode_trailer(&client, 44);
	error = bcm2711_vulkan_stream_execute(&session, client.data, (uint32_t)client.bytes, dispatch_body);
	assert(error == 0 && buffers[0].references == 1);

	/* Two external bodies are copied before the first dispatch mutates the original second command. */
	encode_body(&external, 0x11111111U);
	encode_body(&external, 0x22222222U);
	assert(external.error == VK_SUCCESS && external.bytes <= sizeof(external_words));
	memcpy(external_words, external.data, external.bytes);
	client.bytes = 0;
	encode_selector(&client, 64);
	encode_external(&client, external.bytes, 0);
	encode_trailer(&client, 44);
	mutate_external = 1;
	error = bcm2711_vulkan_stream_execute(&session, client.data, (uint32_t)client.bytes, dispatch_body);
	assert(error == 0 && dispatched == 3 && allocations == 0 && buffers[0].references == 1);
	assert(external_words[5] == 0xbadbadbaU && reply_words[6] == 0x11111111U && reply_words[9] == 0x22222222U);
	mutate_external = 0;

	/* Unsupported external flags stop decoding before any body or successful trailer can be published. */
	before = dispatched;
	memset(reply_words, 0, sizeof(reply_words));
	client.bytes = 0;
	encode_selector(&client, 64);
	encode_external(&client, external.bytes, 1);
	encode_trailer(&client, 44);
	error = bcm2711_vulkan_stream_execute(&session, client.data, (uint32_t)client.bytes, dispatch_body);
	assert(error == ENOTSUP && dispatched == before && reply_words[19] == 0 && allocations == 0);
	assert(buffers[0].references == 1);

	/* A self-referential external stream reaches the shared recursion bound and unwinds every private copy. */
	external.bytes = 0;
	encode_external(&external, 64, 0);
	assert(external.bytes == 64);
	memcpy(external_words, external.data, external.bytes);
	client.bytes = 0;
	encode_selector(&client, 64);
	encode_external(&client, external.bytes, 0);
	error = bcm2711_vulkan_stream_execute(&session, client.data, (uint32_t)client.bytes, dispatch_body);
	assert(error == E2BIG && allocations == 0 && buffers[0].references == 1 && dispatched == before);

	/* Actual selector capacity, rather than the allocation remainder, bounds all following replies. */
	client.bytes = 0;
	encode_selector(&client, 20);
	encode_body(&client, 0);
	encode_trailer(&client, 0);
	error = bcm2711_vulkan_stream_execute(&session, client.data, (uint32_t)client.bytes, dispatch_body);
	assert(error == 0 && buffers[0].references == 1);
	client.bytes = 0;
	encode_selector(&client, 20);
	encode_body(&client, 0);
	encode_body(&client, 1);
	error = bcm2711_vulkan_stream_execute(&session, client.data, (uint32_t)client.bytes, dispatch_body);
	assert(error == EINVAL && buffers[0].references == 1);
	client.bytes = 0;
	encode_selector(&client, 256);
	error = bcm2711_vulkan_stream_execute(&session, client.data, (uint32_t)client.bytes, dispatch_body);
	assert(error == EINVAL && buffers[0].references == 1);

	/* Missing selection, truncated headers and unaligned trailer positions cannot dereference absent backing. */
	client.bytes = 0;
	encode_trailer(&client, 44);
	error = bcm2711_vulkan_stream_execute(&session, client.data, (uint32_t)client.bytes, dispatch_body);
	assert(error == EINVAL && buffers[0].references == 1);
	client.bytes = 0;
	encode_selector(&client, 64);
	encode_trailer(&client, 43);
	error = bcm2711_vulkan_stream_execute(&session, client.data, (uint32_t)client.bytes, dispatch_body);
	assert(error == EINVAL && buffers[0].references == 1);
	error = bcm2711_vulkan_stream_execute(&session, client.data, 3, dispatch_body);
	assert(error == EINVAL && buffers[0].references == 1 && allocations == 0);

	/* The actual client writers retire their private host byte arrays through their normal ownership operations. */
	vulkan_writer_finish(&external);
	vulkan_writer_finish(&client);
	puts("WS141 Vulkan stream host test: PASS (actual client writer, native replies, external copies and refusal cleanup)");
	return 0;
}

/* Encodes the concrete client selector with a nonzero offset to expose incorrect capacity/remainder handling. */
static void
encode_selector(
	struct vulkan_writer *writer,
	uint64_t capacity)
{
	/* The existing library's actual primitives define every field width and little-endian byte ordering. */
	vulkan_write_u32(writer, GPU_OP_SET_REPLY_STREAM);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, 1);
	vulkan_write_u64(writer, 16);
	vulkan_write_u64(writer, capacity);
}

/* Encodes the fixed-position version probe used by the real client completion contract. */
static void
encode_trailer(
	struct vulkan_writer *writer,
	uint64_t offset)
{
	/* Seeking and probing are separate commands with their actual reply-request flags. */
	vulkan_write_u32(writer, GPU_OP_SEEK_REPLY_STREAM);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, offset);
	vulkan_write_u32(writer, GPU_OP_ENUMERATE_INSTANCE_VERSION);
	vulkan_write_u32(writer, 1);
	vulkan_write_u64(writer, 1);
}

/* Encodes the single-stream wrapper with concrete absence markers for unsupported arrays. */
static void
encode_external(
	struct vulkan_writer *writer,
	uint64_t bytes,
	uint32_t flags)
{
	/* The real library wrapper carries one resource, byte interval and explicit empty optional fields. */
	vulkan_write_u32(writer, GPU_OP_EXECUTE_STREAMS);
	vulkan_write_u32(writer, 0);
	vulkan_write_u32(writer, 1);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, 2);
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, bytes);
	vulkan_write_u64(writer, 0);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, 0);
	vulkan_write_u32(writer, flags);
}

/* Encodes one fixture body whose values reveal forwarding and external snapshot lifetime. */
static void
encode_body(
	struct vulkan_writer *writer,
	uint32_t bits)
{
	/* This test-only body is forwarded to a callback; it is not a supported application Vulkan operation. */
	vulkan_write_u32(writer, TEST_BODY_OPCODE);
	vulkan_write_u32(writer, 1);
	vulkan_write_u32(writer, bits);
}

/* Observes actual transport forwarding while deliberately modifying only the client's original external bytes. */
static int
dispatch_body(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t bits;

	/* A typed body sees its session, actual selected backing and a reader already advanced past the header. */
	assert(session != NULL && opcode == TEST_BODY_OPCODE && requested == 1 && buffers[0].references == 2);
	bits = drv_i915_wire_read_u32(reader);
	assert(reader->error == 0);
	drv_i915_wire_reply_u32(reply, 0);
	drv_i915_wire_reply_u32(reply, bits);
	dispatched++;

	/* Mutating the original second argument must not affect a previously captured private server snapshot. */
	if (mutate_external != 0)
		external_words[5] = 0xbadbadbaU;

	/* This fixture body has no native GPU effects and only proves correct transport delivery. */
	return 0;
}
