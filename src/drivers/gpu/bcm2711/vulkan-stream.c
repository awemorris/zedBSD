/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native command transport owns reply backing, copied external streams and atomic decoder-completion publication. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/vulkan-private.h"

/* One submission has a bounded external-stream copy budget shared by every nested stream. */
#define VULKAN_STREAM_COPY_BYTES (64U << 20)

/* Nested transport wrappers are finite even when client blobs refer to themselves. */
#define VULKAN_STREAM_DEPTH 4U

/* The existing libvulkan decoder-completion contract requires a Vulkan 1.1 version response. */
#define VULKAN_STREAM_VERSION ((1U << 22) | (1U << 12))

/* One submitted transport frame retains CPU reply backing independently of resource IDs and native GPU VA mappings. */
struct vulkan_stream {
	struct bcm2711_vulkan_session *session;
	struct bcm2711_buffer *reply_buffer;
	struct i915_wire_writer reply;
	uint64_t copy_budget;
	uint32_t depth;
	int (*dispatch)(struct bcm2711_vulkan_session *, uint32_t, uint32_t, struct i915_wire_reader *, struct i915_wire_writer *);
};

static int execute_stream(struct vulkan_stream *stream, struct i915_wire_reader *reader);
static int execute_command(struct vulkan_stream *stream, struct i915_wire_reader *reader);
static int select_reply(struct vulkan_stream *stream, struct i915_wire_reader *reader);
static int seek_reply(struct vulkan_stream *stream, struct i915_wire_reader *reader);
static int version_reply(struct vulkan_stream *stream, struct i915_wire_reader *reader);
static int external_stream(struct vulkan_stream *stream, struct i915_wire_reader *reader);

/*
 * Executes one immutable command snapshot while retaining its selected CPU reply storage.
 */
int
bcm2711_vulkan_stream_execute(
	struct bcm2711_vulkan_session *session,
	const void *wire,
	uint32_t bytes,
	int (*dispatch)(struct bcm2711_vulkan_session *, uint32_t, uint32_t, struct i915_wire_reader *, struct i915_wire_writer *))
{
	struct vulkan_stream stream;
	struct i915_wire_reader reader;
	int error;

	/* The caller holds the controller mutex and supplies an already copied immutable submission. */
	if (session == NULL || wire == NULL || bytes == 0 || dispatch == NULL)
		return EINVAL;
	if (session->closing)
		return ECANCELED;

	/* Reply selection starts absent and external-copy bounds cover the entire submitted frame. */
	kern_memset(&stream, 0, sizeof(stream));
	stream.session = session;
	stream.copy_budget = VULKAN_STREAM_COPY_BYTES;
	stream.dispatch = dispatch;

	/* The shared read-only Zlib codec sees only the immutable caller-owned snapshot. */
	kern_memset(&reader, 0, sizeof(reader));
	reader.base = wire;
	reader.size = bytes;
	error = execute_stream(&stream, &reader);

	/* CPU reply storage has no native DMA use and retires independently of its resource descriptor's GPU VA. */
	if (stream.reply_buffer != NULL)
		bcm2711_buffer_release(stream.reply_buffer);
	if (error != 0)
		return error;

	/* Every command and its final decoder trailer completed before the frame released its reply backing. */
	return 0;
}

/* Decodes each complete command and stops at the first unsupported or malformed operation. */
static int
execute_stream(
	struct vulkan_stream *stream,
	struct i915_wire_reader *reader)
{
	int error;

	/* Generated record pointers are valid for one command and are never retained by this transport. */
	while (reader->offset < reader->size) {
		stream->session->arena.used = 0;
		error = execute_command(stream, reader);
		if (error != 0)
			return error;
		if (reader->error != 0 || stream->reply.error != 0)
			return EINVAL;
	}

	/* No unknown-length command was skipped or accepted as an empty success. */
	return 0;
}

/* Owns transport headers and delegates only complete non-transport command bodies to the typed runtime. */
static int
execute_command(
	struct vulkan_stream *stream,
	struct i915_wire_reader *reader)
{
	uint32_t opcode;
	uint32_t requested;
	int error;

	/* All commands have exactly one opcode word and one Boolean reply-request word. */
	opcode = drv_i915_wire_read_u32(reader);
	requested = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || requested > 1)
		return EINVAL;

	/* Selector, seek and external-stream wrappers never produce a Vulkan result reply themselves. */
	if (opcode == GPU_OP_SET_REPLY_STREAM || opcode == GPU_OP_SEEK_REPLY_STREAM || opcode == GPU_OP_EXECUTE_STREAMS) {
		if (requested != 0)
			return EINVAL;
	}

	/* A requested command response starts with its echoed opcode in the currently owned reply region. */
	if (requested != 0) {
		if (stream->reply_buffer == NULL)
			return EINVAL;
		drv_i915_wire_reply_u32(&stream->reply, opcode);
		if (stream->reply.error != 0)
			return EINVAL;
	}

	/* The version trailer needs a reply request because its final atomic word is the decoder completion proof. */
	if (opcode == GPU_OP_ENUMERATE_INSTANCE_VERSION && requested == 0)
		return EINVAL;

	/* Typed runtime commands receive no transport-specific resource address or mutable input blob. */
	switch (opcode) {
	case GPU_OP_SET_REPLY_STREAM:
		error = select_reply(stream, reader);
		break;
	case GPU_OP_SEEK_REPLY_STREAM:
		error = seek_reply(stream, reader);
		break;
	case GPU_OP_ENUMERATE_INSTANCE_VERSION:
		error = version_reply(stream, reader);
		break;
	case GPU_OP_EXECUTE_STREAMS:
		error = external_stream(stream, reader);
		break;
	default:
		error = stream->dispatch(stream->session, opcode, requested, reader, &stream->reply);
		break;
	}

	/* Any failure stops decoding before a later trailer could falsely publish success. */
	if (error != 0)
		return error;

	/* This command's complete body and requested output have been processed. */
	return 0;
}

/* Selects and retains the actual requested session-local blob extent, including its explicit capacity. */
static int
select_reply(
	struct vulkan_stream *stream,
	struct i915_wire_reader *reader)
{
	struct bcm2711_render_resource *resource;
	struct bcm2711_buffer *buffer;
	uint64_t present;
	uint64_t offset;
	uint64_t capacity;
	uint32_t identifier;

	/* Only the concrete nonnull reply selector used by the existing libvulkan transport is accepted. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	identifier = drv_i915_wire_read_u32(reader);
	offset = drv_i915_wire_read_u64(reader);
	capacity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The controller mutex keeps the borrowed resource stable until its actual CPU backing is retained. */
	resource = bcm2711_render_find(stream->session->render, identifier);
	if (resource == NULL || !resource->blob || resource->view == NULL)
		return EINVAL;
	buffer = resource->view->buffer;
	if (buffer == NULL || buffer->address == NULL)
		return EINVAL;

	/* The whole reply extent fits its allocation and every completion word has natural four-byte alignment. */
	if ((offset & 3) != 0 || offset > buffer->bytes || capacity > buffer->bytes - offset || capacity < 20)
		return EINVAL;

	/* Retaining the replacement first permits selecting the same backing without a transient last-owner release. */
	bcm2711_buffer_retain(buffer);
	if (stream->reply_buffer != NULL)
		bcm2711_buffer_release(stream->reply_buffer);
	stream->reply_buffer = buffer;
	stream->reply.base = (uint8_t *)buffer->address + (size_t)offset;
	stream->reply.size = (size_t)capacity;
	stream->reply.offset = 0;
	stream->reply.error = 0;

	/* Following responses are bounded by the selected capacity rather than the unused remainder of the allocation. */
	return 0;
}

/* Moves the reply cursor only within the currently retained reply extent. */
static int
seek_reply(
	struct vulkan_stream *stream,
	struct i915_wire_reader *reader)
{
	uint64_t offset;

	/* Reply seeks cannot invent a region or place an atomic trailer at an unaligned address. */
	offset = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || stream->reply_buffer == NULL || offset > stream->reply.size || (offset & 3) != 0)
		return EINVAL;
	stream->reply.offset = (size_t)offset;

	/* The next actual reply begins at this checked byte offset. */
	return 0;
}

/* Writes the decoder-completion version only after all earlier response and trailer words are published. */
static int
version_reply(
	struct vulkan_stream *stream,
	struct i915_wire_reader *reader)
{
	uint32_t *completion;
	uint64_t present;

	/* The existing library requests one concrete version output pointer. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	if ((stream->reply.offset & 3) != 0 || stream->reply.offset > stream->reply.size ||
	    stream->reply.size - stream->reply.offset < 16)
		return EINVAL;

	/* Result and pointer-presence precede the final version, with the opcode already written by header dispatch. */
	drv_i915_wire_reply_u32(&stream->reply, 0);
	drv_i915_wire_reply_u64(&stream->reply, 1);
	if (stream->reply.error != 0)
		return EINVAL;

	/* BCM2711 is little endian; a naturally aligned release word matches libvulkan's acquire trailer observation. */
	completion = (uint32_t *)(stream->reply.base + stream->reply.offset);
	__atomic_store_n(completion, VULKAN_STREAM_VERSION, __ATOMIC_RELEASE);
	stream->reply.offset += 4;

	/* Decoder completion proves all earlier CPU replies, independently of the separate native job completion domains. */
	return 0;
}

/* Copies one supported external wrapper into immutable host storage before decoding any nested command. */
static int
external_stream(
	struct vulkan_stream *stream,
	struct i915_wire_reader *reader)
{
	struct bcm2711_render_resource *resource;
	struct bcm2711_buffer *buffer;
	struct i915_wire_reader nested;
	uint32_t count;
	uint32_t identifier;
	uint32_t dependencies;
	uint32_t flags;
	uint64_t present;
	uint64_t offset;
	uint64_t bytes;
	uint64_t reply_offsets;
	uint64_t dependency_pointer;
	uint8_t *copy;
	int error;

	/* Only the one-stream, no-dependency wrapper produced by the current library has implemented semantics. */
	count = drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	identifier = drv_i915_wire_read_u32(reader);
	offset = drv_i915_wire_read_u64(reader);
	bytes = drv_i915_wire_read_u64(reader);
	reply_offsets = drv_i915_wire_read_u64(reader);
	dependencies = drv_i915_wire_read_u32(reader);
	dependency_pointer = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count != 1 || present != 1 || reply_offsets != 0 || dependencies != 0 || dependency_pointer != 0 || flags != 0)
		return ENOTSUP;
	if (bytes == 0 || bytes > stream->copy_budget || stream->depth == VULKAN_STREAM_DEPTH)
		return E2BIG;

	/* Actual session-local blob storage must contain the complete requested immutable copy. */
	resource = bcm2711_render_find(stream->session->render, identifier);
	if (resource == NULL || !resource->blob || resource->view == NULL)
		return EINVAL;
	buffer = resource->view->buffer;
	if (buffer == NULL || buffer->address == NULL || offset > buffer->bytes || bytes > buffer->bytes - offset)
		return EINVAL;

	/* The global frame budget bounds total retained recursive copy storage, rather than each wrapper separately. */
	copy = kern_malloc((size_t)bytes);
	if (copy == NULL)
		return ENOMEM;
	kern_memcpy(copy, (const uint8_t *)buffer->address + (size_t)offset, (size_t)bytes);
	stream->copy_budget -= bytes;
	stream->depth++;

	/* The generated codec observes the private snapshot even if later CPU replies alias the client's source blob. */
	kern_memset(&nested, 0, sizeof(nested));
	nested.base = copy;
	nested.size = (size_t)bytes;
	error = execute_stream(stream, &nested);
	stream->depth--;
	kern_free(copy);
	if (error != 0)
		return error;

	/* No mutable client stream pointer survives this external command. */
	return 0;
}
