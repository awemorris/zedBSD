/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's transport: one frame of commands, the reply stream the
 * frame selects in a blob of the session's, the decoder's completion
 * trailer, and a command stream carried in a blob.
 *
 * Every command has an opcode word and a word that says whether it wants
 * a reply; a reply starts with the echoed opcode.  The trailer is the
 * reply to the instance version query, written last with a release store:
 * libvulkan trusts the replies only once it sees it.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/bcm2711/vk/vulkan-stream.c
 * (the framing of a frame and its transport commands).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/vk/vk.h"

/* What all command streams of one frame copy at most, and how deep streams nest. */
#define STREAM_COPY_BYTES		(16U << 20)
#define STREAM_DEPTH			4U

/* The smallest reply region: the trailer's opcode, result, presence and version. */
#define STREAM_REPLY_MIN		20U

/* The version the trailer reports (libvulkan's decoder completion needs Vulkan 1.1). */
#define STREAM_VERSION			((1U << 22) | (1U << 12))

/* One frame being decoded: its reply region, the bytes it may still copy, how deep it is. */
struct stream_frame {
	struct powervr_vk_session *session;
	struct i915_wire_writer reply;
	bool reply_selected;
	uint64_t copy_budget;
	uint32_t depth;
};

static int run_stream(struct stream_frame *frame, struct i915_wire_reader *reader);
static int run_command(struct stream_frame *frame, struct i915_wire_reader *reader);
static int dispatch(struct powervr_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int select_reply(struct stream_frame *frame, struct i915_wire_reader *reader);
static int seek_reply(struct stream_frame *frame, struct i915_wire_reader *reader);
static int version_reply(struct stream_frame *frame, struct i915_wire_reader *reader);
static int nested_stream(struct stream_frame *frame, struct i915_wire_reader *reader);
static int blob_bytes(struct powervr_vk_session *session, uint32_t identifier, uint8_t **bytes, uint64_t *length);

/* The handlers, tried in order; each takes only its own opcodes. */
static const powervr_vk_route routes[] = {
	powervr_vk_device_route,
	powervr_vk_memory_route,
	powervr_vk_image_route,
	powervr_vk_layout_route,
	powervr_vk_command_route,
	powervr_vk_pass_route,
	powervr_vk_queue_route,
	powervr_vk_query_route
};

/*
 * Runs one frame of commands: every command in order, stopping at the
 * first one refused.  The caller holds the session's admission.
 */
int
powervr_vk_execute(
	struct powervr_vk_session *session,
	const void *wire,
	uint32_t bytes)
{
	struct stream_frame frame;
	struct i915_wire_reader reader;
	int error;

	/* A frame of whole words for a session that is open. */
	if (session == NULL || wire == NULL || bytes == 0 || (bytes & 3U) != 0)
		return EINVAL;
	if (session->closing)
		return ECANCELED;

	/* No reply region yet; the copy budget covers the whole frame. */
	kern_memset(&frame, 0, sizeof(frame));
	frame.session = session;
	frame.copy_budget = STREAM_COPY_BYTES;

	/* Decodes the frame. */
	kern_memset(&reader, 0, sizeof(reader));
	reader.base = wire;
	reader.size = bytes;
	error = run_stream(&frame, &reader);
	if (error != 0)
		return error;

	/* Succeeded: every command and its replies are done. */
	return 0;
}

/* Runs each command of a stream; the scratch is emptied before each. */
static int
run_stream(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	int error;

	/* One command at a time to the stream's end. */
	while (reader->offset < reader->size) {
		frame->session->arena.used = 0;
		error = run_command(frame, reader);
		if (error != 0)
			return error;
		if (reader->error != 0 || frame->reply.error != 0)
			return EINVAL;
	}

	/* Succeeded: the stream is decoded. */
	return 0;
}

/* Reads one command's header and runs its body. */
static int
run_command(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	uint32_t opcode;
	uint32_t requested;
	int error;

	/* The opcode and whether a reply is wanted. */
	opcode = drv_i915_wire_read_u32(reader);
	requested = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || requested > 1)
		return EINVAL;
	frame->session->last_opcode = opcode;

	/* The transport's own commands never reply. */
	if ((opcode == GPU_OP_SET_REPLY_STREAM || opcode == GPU_OP_SEEK_REPLY_STREAM || opcode == GPU_OP_EXECUTE_STREAMS) && requested != 0)
		return EINVAL;

	/* A reply starts with the echoed opcode in the selected region. */
	if (requested != 0) {
		if (!frame->reply_selected)
			return EINVAL;
		drv_i915_wire_reply_u32(&frame->reply, opcode);
		if (frame->reply.error != 0)
			return EINVAL;
	}

	/* The trailer needs a reply: its last word is the decoder's completion. */
	if (opcode == GPU_OP_ENUMERATE_INSTANCE_VERSION && requested == 0)
		return EINVAL;

	/* The transport's commands here, every other one to the handlers. */
	if (opcode == GPU_OP_SET_REPLY_STREAM) {
		error = select_reply(frame, reader);
	} else if (opcode == GPU_OP_SEEK_REPLY_STREAM) {
		error = seek_reply(frame, reader);
	} else if (opcode == GPU_OP_ENUMERATE_INSTANCE_VERSION) {
		error = version_reply(frame, reader);
	} else if (opcode == GPU_OP_EXECUTE_STREAMS) {
		error = nested_stream(frame, reader);
	} else {
		error = dispatch(frame->session, opcode, requested, reader, &frame->reply);
	}

	/* A refused command ends the frame before a later trailer could claim success. */
	if (error != 0) {
		frame->session->failed_opcode = opcode;
		return error;
	}

	/* Succeeded: the command and its reply are done. */
	return 0;
}

/* Gives one command to the handler whose opcode it is; ENOTSUP when none takes it. */
static int
dispatch(
	struct powervr_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	size_t index;
	int handled;
	int error;

	/* Each handler in turn until one takes the opcode. */
	handled = 0;
	for (index = 0; index < sizeof(routes) / sizeof(routes[0]); index++) {
		error = routes[index](session, opcode, requested, reader, reply, &handled);
		if (error != 0)
			return error;
		if (handled != 0)
			break;
	}

	/* No handler: the command is not implemented. */
	if (handled == 0)
		return ENOTSUP;

	/* Succeeded: a handler ran the command. */
	return 0;
}

/* Selects the reply region: [present][resource][offset][capacity] in a blob of the session's. */
static int
select_reply(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	uint8_t *bytes;
	uint64_t length;
	uint64_t present;
	uint64_t offset;
	uint64_t capacity;
	uint32_t identifier;
	int error;

	/* The region. */
	present = drv_i915_wire_read_u64(reader);
	identifier = drv_i915_wire_read_u32(reader);
	offset = drv_i915_wire_read_u64(reader);
	capacity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;

	/* A blob of the session's that holds the whole region, word aligned. */
	error = blob_bytes(frame->session, identifier, &bytes, &length);
	if (error != 0)
		return error;
	if ((offset & 3U) != 0 || offset > length || capacity > length - offset || capacity < STREAM_REPLY_MIN)
		return EINVAL;

	/* Replies go there from now on. */
	frame->reply.base = bytes + (size_t)offset;
	frame->reply.size = (size_t)capacity;
	frame->reply.offset = 0;
	frame->reply.error = 0;
	frame->reply_selected = true;

	/* Succeeded: the region is selected. */
	return 0;
}

/* Moves the reply cursor within the region: [offset]. */
static int
seek_reply(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	uint64_t offset;

	/* A word-aligned place inside the selected region. */
	offset = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || !frame->reply_selected || offset > frame->reply.size || (offset & 3U) != 0)
		return EINVAL;
	frame->reply.offset = (size_t)offset;

	/* Succeeded: the next reply starts there. */
	return 0;
}

/*
 * Writes the trailer: [result][present] then the version, stored last
 * with release order so every earlier reply is visible first.
 */
static int
version_reply(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	uint32_t *completion;
	uint64_t present;

	/* The version's output pointer, and room for the trailer. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	if ((frame->reply.offset & 3U) != 0 || frame->reply.offset > frame->reply.size || frame->reply.size - frame->reply.offset < 16)
		return EINVAL;

	/* The result and the presence word. */
	drv_i915_wire_reply_u32(&frame->reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(&frame->reply, 1);
	if (frame->reply.error != 0)
		return EINVAL;

	/* The version, released after everything before it (the view is uncached; the core is little endian). */
	completion = (uint32_t *)(frame->reply.base + frame->reply.offset);
	__atomic_store_n(completion, STREAM_VERSION, __ATOMIC_RELEASE);
	frame->reply.offset += 4;

	/* Succeeded: the decoder's completion is published. */
	return 0;
}

/*
 * Runs a command stream carried in a blob: [count][present][resource]
 * [offset][bytes][reply offsets][dependencies][pointer][flags], one
 * stream without dependencies, copied first so the client cannot change
 * it while it is decoded.
 */
static int
nested_stream(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	struct i915_wire_reader nested;
	uint8_t *bytes;
	uint8_t *copy;
	uint64_t length;
	uint64_t present;
	uint64_t offset;
	uint64_t size;
	uint64_t reply_offsets;
	uint64_t dependency_pointer;
	uint32_t count;
	uint32_t identifier;
	uint32_t dependencies;
	uint32_t flags;
	int error;

	/* The wrapper. */
	count = drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	identifier = drv_i915_wire_read_u32(reader);
	offset = drv_i915_wire_read_u64(reader);
	size = drv_i915_wire_read_u64(reader);
	reply_offsets = drv_i915_wire_read_u64(reader);
	dependencies = drv_i915_wire_read_u32(reader);
	dependency_pointer = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* One stream, no reply offsets, no dependencies, within the budget and the depth. */
	if (count != 1 || present != 1 || reply_offsets != 0 || dependencies != 0 || dependency_pointer != 0 || flags != 0)
		return ENOTSUP;
	if (size == 0 || size > frame->copy_budget || (size & 3U) != 0 || frame->depth == STREAM_DEPTH)
		return E2BIG;

	/* A blob of the session's that holds the stream. */
	error = blob_bytes(frame->session, identifier, &bytes, &length);
	if (error != 0)
		return error;
	if (offset > length || size > length - offset)
		return EINVAL;

	/* The copy. */
	copy = kern_malloc((size_t)size);
	if (copy == NULL)
		return ENOMEM;
	kern_memcpy(copy, bytes + (size_t)offset, (size_t)size);
	frame->copy_budget -= size;
	frame->depth++;

	/* Decodes the copy. */
	kern_memset(&nested, 0, sizeof(nested));
	nested.base = copy;
	nested.size = (size_t)size;
	error = run_stream(frame, &nested);
	frame->depth--;
	kern_free(copy);
	if (error != 0)
		return error;

	/* Succeeded: the stream is decoded. */
	return 0;
}

/* Finds a blob of the session's by its resource id: its bytes and their length. */
static int
blob_bytes(
	struct powervr_vk_session *session,
	uint32_t identifier,
	uint8_t **bytes,
	uint64_t *length)
{
	struct powervr_resource *resource;

	/* A mappable blob of the session's. */
	resource = powervr_session_blob(session->owner, identifier);
	if (resource == NULL || !resource->mappable)
		return EINVAL;

	/* Its uncached view and its size. */
	*bytes = (uint8_t *)resource->buffer->view;
	*length = resource->buffer->bytes;

	/* Succeeded: the blob's bytes. */
	return 0;
}
