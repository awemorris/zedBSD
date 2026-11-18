/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transport of the Vulkan executor: one submitted frame of commands,
 * run in order.
 *
 * The transport's own commands are run here: the reply stream (a range
 * of one of the node session's blobs) is selected and moved, a stream
 * that lies in a blob is copied and run one level deeper, and the version
 * probe is the trailer libvulkan polls, stored last so that a client that
 * sees it also sees every reply before it.  Every other command goes to
 * the dispatcher.  A refused or malformed command ends the frame: nothing
 * after it runs, and no trailer says the frame is complete.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "vk.h"

/*
 * The most bytes of blob streams one frame copies, and how deeply
 * streams may name further streams.
 */
#define STREAM_COPY_BYTES		(64ULL << 20)
#define STREAM_DEPTH_MAX		4U

/*
 * The least a reply range holds (the probe's 20 bytes), the probe's
 * answer after the echoed opcode, and the version it answers: the wire
 * says 1.1, which libvulkan requires and shows applications as 1.0.
 */
#define STREAM_REPLY_LEAST		20U
#define STREAM_PROBE_BYTES		16U
#define STREAM_VERSION			((1U << 22) | (1U << 12))

/*
 * One frame being run: its session, the reply range's writer (base NULL
 * until a reply stream is selected), what may still be copied, and how
 * deep the streams are nested now.  It lives on the stack of the run.
 */
struct stream_frame {
	struct ghci_vk_session *session;
	struct i915_wire_writer reply;
	uint64_t copy_left;
	uint32_t depth;
};

static int stream_run(struct stream_frame *frame, struct i915_wire_reader *reader);
static int stream_command(struct stream_frame *frame, struct i915_wire_reader *reader);
static int stream_select(struct stream_frame *frame, struct i915_wire_reader *reader);
static int stream_seek(struct stream_frame *frame, struct i915_wire_reader *reader);
static int stream_probe(struct stream_frame *frame, struct i915_wire_reader *reader);
static int stream_nested(struct stream_frame *frame, struct i915_wire_reader *reader);
static int stream_blob(struct ghci_vk_session *session, uint32_t resource_id, uint8_t **cpu, uint64_t *bytes);

/*
 * Runs one submitted frame (a copy the caller owns), and then lets the
 * submissions the session's devices hold back go as far as they may on
 * this thread, which may wait for the ring's room: a piece the service
 * thread cannot place at once (one longer than half the ring between two
 * cuts) then goes at the client's next frame.  Returns 0 once every
 * command ran, EIO for a session the GPU core stopped or isolated,
 * EINVAL for a malformed command, or the error of the command that
 * failed.
 */
int
ghci_vk_stream_execute(
	struct ghci_vk_session *session,
	const void *wire,
	uint32_t bytes)
{
	struct stream_frame frame;
	struct i915_wire_reader reader;
	struct ghci_vk_device *device;
	unsigned long irq;
	int stopped;
	int error;

	/* A frame of no bytes is no frame. */
	if (wire == NULL || bytes == 0U)
		return EINVAL;

	/* A session the core stopped or isolated runs nothing more. */
	irq = spin_lock_irqsave(&session->device->irq_lock);

	stopped = session->stopping || session->isolated;

	spin_unlock_irqrestore(&session->device->irq_lock, irq);
	if (stopped)
		return EIO;

	/* No reply stream yet; the copy budget covers the whole frame. */
	kern_memset(&frame, 0, sizeof(frame));
	frame.session = session;
	frame.copy_left = STREAM_COPY_BYTES;

	/* Reads the frame from its start; the session's objects are the frame's until it ends. */
	kern_memset(&reader, 0, sizeof(reader));
	reader.base = wire;
	reader.size = bytes;
	mutex_lock(&session->hold_lock);

	error = stream_run(&frame, &reader);

	/* The held submissions, as far as they may go now (the frame's error is the frame's alone). */
	for (device = session->devices; device != NULL; device = device->next) {
		if (device->holds != NULL)
			(void)ghci_vk_holds_advance(session, device, 1);
	}

	mutex_unlock(&session->hold_lock);
	if (error != 0)
		return error;

	/* Succeeded: every command of the frame ran. */
	return 0;
}

/* Runs the commands of one stream until its end or the first that fails. */
static int
stream_run(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	int error;

	/* Each command, with the scratch emptied for its records. */
	while (reader->offset < reader->size) {
		frame->session->arena.used = 0U;
		error = stream_command(frame, reader);
		if (error != 0)
			return error;

		/* A read past the stream's end or a write past the reply range is malformed. */
		if (reader->error != 0)
			return EINVAL;
		if (frame->reply.error != 0)
			return EINVAL;
	}

	/* Succeeded: the stream ended on a command's end. */
	return 0;
}

/* Runs one command: its header, the echoed opcode when a reply is asked for, then its body. */
static int
stream_command(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	uint32_t opcode;
	uint32_t requested;
	int transport;
	int error;

	/* The header: the opcode and whether a reply is asked for (0 or 1). */
	opcode = drv_i915_wire_read_u32(reader);
	requested = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || requested > GPU_OP_FLAG_REPLY)
		return EINVAL;
	frame->session->last_opcode = opcode;

	/* The transport's commands ask for no reply; the probe always asks. */
	transport = 0;
	if (opcode == GPU_OP_SET_REPLY_STREAM || opcode == GPU_OP_SEEK_REPLY_STREAM)
		transport = 1;
	else if (opcode == GPU_OP_EXECUTE_STREAMS)
		transport = 1;
	if (transport && requested != 0U)
		return EINVAL;
	if (opcode == GPU_OP_ENUMERATE_INSTANCE_VERSION && requested == 0U)
		return EINVAL;

	/* A reply starts with the echoed opcode, in a selected reply stream. */
	if (requested != 0U) {
		if (frame->reply.base == NULL)
			return EINVAL;
		drv_i915_wire_reply_u32(&frame->reply, opcode);
		if (frame->reply.error != 0)
			return EINVAL;
	}

	/* The transport's own commands, or the dispatcher. */
	switch (opcode) {
	case GPU_OP_SET_REPLY_STREAM:
		error = stream_select(frame, reader);
		break;
	case GPU_OP_SEEK_REPLY_STREAM:
		error = stream_seek(frame, reader);
		break;
	case GPU_OP_ENUMERATE_INSTANCE_VERSION:
		error = stream_probe(frame, reader);
		break;
	case GPU_OP_EXECUTE_STREAMS:
		error = stream_nested(frame, reader);
		break;
	default:
		error = ghci_vk_dispatch(frame->session, opcode, requested, reader, &frame->reply);
		break;
	}

	/* A refused command is remembered for the log and ends the frame. */
	if (error != 0) {
		frame->session->failed_opcode = opcode;
		return error;
	}

	/* Succeeded: the command's body was taken. */
	return 0;
}

/* Selects the reply stream: a range of one of the session's blobs, its offset and capacity. */
static int
stream_select(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	uint64_t present;
	uint32_t resource_id;
	uint64_t offset;
	uint64_t capacity;
	uint8_t *cpu;
	uint64_t bytes;
	int error;

	/* The stream is named (a present record) by its blob, offset and capacity. */
	present = drv_i915_wire_read_u64(reader);
	resource_id = drv_i915_wire_read_u32(reader);
	offset = drv_i915_wire_read_u64(reader);
	capacity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;

	/* A resource of the session's. */
	error = stream_blob(frame->session, resource_id, &cpu, &bytes);
	if (error != 0)
		return EINVAL;

	/* The range lies in the blob, word-aligned, and holds at least the probe. */
	if ((offset & 3U) != 0U)
		return EINVAL;
	if (offset > bytes || capacity > bytes - offset)
		return EINVAL;
	if (capacity < STREAM_REPLY_LEAST)
		return EINVAL;

	/* The replies go there from its start, bounded by the capacity. */
	frame->reply.base = cpu + (size_t)offset;
	frame->reply.size = (size_t)capacity;
	frame->reply.offset = 0U;
	frame->reply.error = 0;

	/* Succeeded: the reply stream is selected. */
	return 0;
}

/* Moves the reply writer to a word-aligned offset of the selected stream. */
static int
stream_seek(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	uint64_t offset;

	/* The offset. */
	offset = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Only in a selected stream, word-aligned, inside it. */
	if (frame->reply.base == NULL)
		return EINVAL;
	if (offset > frame->reply.size || (offset & 3U) != 0U)
		return EINVAL;

	/* The next reply goes there. */
	frame->reply.offset = (size_t)offset;

	/* Succeeded: the writer moved. */
	return 0;
}

/*
 * Answers the version probe, the frame's trailer: the result, the
 * present word, and the version, stored last with release order so that
 * a client that sees it also sees every reply before it.
 */
static int
stream_probe(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	uint32_t *version;
	uint64_t present;

	/* The probe names its output. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;

	/* The rest of the answer fits after the echoed opcode, word-aligned. */
	if ((frame->reply.offset & 3U) != 0U)
		return EINVAL;
	if (frame->reply.offset > frame->reply.size)
		return EINVAL;
	if (frame->reply.size - frame->reply.offset < STREAM_PROBE_BYTES)
		return EINVAL;

	/* The result and the present word. */
	drv_i915_wire_reply_u32(&frame->reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(&frame->reply, 1U);
	if (frame->reply.error != 0)
		return EINVAL;

	/* The version last, released after everything written before it. */
	version = (uint32_t *)(void *)(frame->reply.base + frame->reply.offset);
	__atomic_store_n(version, STREAM_VERSION, __ATOMIC_RELEASE);
	frame->reply.offset += 4U;

	/* Succeeded: the frame's replies up to here are complete. */
	return 0;
}

/*
 * Runs a stream that lies in one of the session's blobs: one stream, no
 * reply offsets, no dependencies, no flags (the form libvulkan sends).
 * The stream is copied first, so that the client cannot change it under
 * the decoder.
 */
static int
stream_nested(
	struct stream_frame *frame,
	struct i915_wire_reader *reader)
{
	struct i915_wire_reader nested;
	uint32_t count;
	uint64_t present;
	uint32_t resource_id;
	uint64_t offset;
	uint64_t length;
	uint64_t reply_offsets;
	uint32_t dependencies;
	uint64_t dependency_list;
	uint32_t flags;
	uint8_t *cpu;
	uint64_t bytes;
	uint8_t *copy;
	int error;

	/* The record: the count, the stream's blob, offset and length, and the empty extras. */
	count = drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	resource_id = drv_i915_wire_read_u32(reader);
	offset = drv_i915_wire_read_u64(reader);
	length = drv_i915_wire_read_u64(reader);
	reply_offsets = drv_i915_wire_read_u64(reader);
	dependencies = drv_i915_wire_read_u32(reader);
	dependency_list = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Only the one form is run. */
	if (count != 1U || present != 1U || flags != 0U)
		return ENOTSUP;
	if (reply_offsets != 0U || dependencies != 0U || dependency_list != 0U)
		return ENOTSUP;

	/* Within the frame's copy budget and nesting. */
	if (length == 0U || length > frame->copy_left)
		return E2BIG;
	if (frame->depth == STREAM_DEPTH_MAX)
		return E2BIG;

	/* The blob holds the whole stream. */
	error = stream_blob(frame->session, resource_id, &cpu, &bytes);
	if (error != 0)
		return EINVAL;
	if (offset > bytes || length > bytes - offset)
		return EINVAL;

	/* Copies the stream, so the decoder reads what was submitted. */
	copy = kern_malloc((size_t)length);
	if (copy == NULL)
		return ENOMEM;
	kern_memcpy(copy, cpu + (size_t)offset, (size_t)length);
	frame->copy_left -= length;

	/* Runs the copy one level deeper. */
	kern_memset(&nested, 0, sizeof(nested));
	nested.base = copy;
	nested.size = (size_t)length;
	frame->depth++;
	error = stream_run(frame, &nested);
	frame->depth--;
	kern_free(copy);

	/* Reports a command of the stream that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the stream ran. */
	return 0;
}

/*
 * Finds a resource of the node session by its id and gives its processor
 * view and size.  Only host memory carries a stream: the driver reads it
 * directly.  Returns 0, or ENOENT (also for a resource of local memory).
 */
static int
stream_blob(
	struct ghci_vk_session *session,
	uint32_t resource_id,
	uint8_t **cpu,
	uint64_t *bytes)
{
	struct ghci_resource *resource;
	struct ghci_device *device;

	/* The session's resources, under the device's lock that guards the list. */
	device = session->device;
	mutex_lock(&device->lock);

	for (resource = session->owner->resources; resource != NULL; resource = resource->next) {
		if (resource->resource_id == resource_id)
			break;
	}
	if (resource != NULL && resource->allocation->kind != GHCI_MEMORY_HOST)
		resource = NULL;
	if (resource != NULL) {
		*cpu = resource->allocation->view;
		*bytes = resource->allocation->bytes;
	}

	mutex_unlock(&device->lock);

	/* No resource of that id. */
	if (resource == NULL)
		return ENOENT;

	/* Succeeded: the view lives as long as the resource, which only this session's commands destroy. */
	return 0;
}
