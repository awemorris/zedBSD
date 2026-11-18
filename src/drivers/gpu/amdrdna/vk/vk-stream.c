/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's transport: one submitted frame of the zedBSD GPU command
 * protocol, decoded command by command.
 *
 * The transport's own commands are taken here: selecting the reply stream
 * (a range of one of the session's blobs the replies are written to),
 * moving in it, the version probe whose last word, written last, tells
 * libvulkan every reply before it is complete, and running a stream that
 * lies in a blob (copied first, so the client cannot change it under the
 * decoder).  Every other command goes to the dispatcher with the reader
 * after its header and the reply writer after its echoed opcode.  The
 * first refused or malformed command ends the frame: nothing after it is
 * run, and no later probe can say the frame is complete.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/amdrdna/vk/amdrdna-vk.h"

/* The most bytes of blob streams one frame copies, and how deeply streams may name further streams. */
#define STREAM_COPY_BYTES		(64ULL << 20)
#define STREAM_DEPTH_MAX		4U

/* The least a reply range holds (the probe's 20 bytes), and the version the probe answers (Vulkan 1.1). */
#define STREAM_REPLY_LEAST		20U
#define STREAM_PROBE_BYTES		16U
#define STREAM_VERSION			((1U << 22) | (1U << 12))

/*
 * One frame being run: its session, the dispatcher, the reply range's
 * writer (base NULL until a reply stream is selected), what may still be
 * copied, and how deep the streams are nested now.
 */
struct stream_frame {
	struct amdrdna_vk_session *session;
	amdrdna_vk_dispatcher dispatch;
	struct i915_wire_writer reply;
	uint64_t copy_left;
	uint32_t depth;
};

static int stream_run(struct stream_frame *frame, struct i915_wire_reader *reader);
static int stream_command(struct stream_frame *frame, struct i915_wire_reader *reader);
static int stream_select(struct stream_frame *frame, struct i915_wire_reader *reader);
static int stream_seek(struct stream_frame *frame, struct i915_wire_reader *reader);
static int stream_probe(struct stream_frame *frame, struct i915_wire_reader *reader);
static int stream_blob(struct stream_frame *frame, struct i915_wire_reader *reader);

/*
 * Runs one submitted frame (a copy the caller owns) through the dispatcher.
 *
 * Returns 0 once every command ran, ECANCELED when the session closes,
 * EINVAL for a malformed command, or the error of the command that failed.
 */
int
amdrdna_vk_stream_execute(
	struct amdrdna_vk_session *session,
	const void *wire,
	uint32_t bytes,
	amdrdna_vk_dispatcher dispatch)
{
	struct stream_frame frame;
	struct i915_wire_reader reader;
	int error;

	/* A frame of no bytes, or for a closing session, runs nothing. */
	if (wire == NULL || bytes == 0U)
		return EINVAL;
	if (session->closing)
		return ECANCELED;

	/* No reply stream yet; the copy budget covers the whole frame. */
	kern_memset(&frame, 0, sizeof(frame));
	frame.session = session;
	frame.dispatch = dispatch;
	frame.copy_left = STREAM_COPY_BYTES;

	/* Reads the frame from its start. */
	kern_memset(&reader, 0, sizeof(reader));
	reader.base = wire;
	reader.size = bytes;
	error = stream_run(&frame, &reader);
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
	if (reader->error != 0 || requested > 1U)
		return EINVAL;

	frame->session->last_opcode = opcode;

	/* The transport's commands ask for no reply; the probe always asks. */
	transport = 0;
	if (opcode == GPU_OP_SET_REPLY_STREAM)
		transport = 1;
	else if (opcode == GPU_OP_SEEK_REPLY_STREAM)
		transport = 1;
	else if (opcode == GPU_OP_EXECUTE_STREAMS)
		transport = 1;
	if (transport != 0 && requested != 0U)
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
		error = stream_blob(frame, reader);
		break;
	default:
		error = frame->dispatch(frame->session, opcode, requested, reader, &frame->reply);
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

	/* The stream is named (a present record), by its blob, offset and capacity. */
	present = drv_i915_wire_read_u64(reader);
	resource_id = drv_i915_wire_read_u32(reader);
	offset = drv_i915_wire_read_u64(reader);
	capacity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;

	/* The blob of the session's. */
	error = amdrdna_vk_blob(frame->session->owner, resource_id, &cpu, &bytes);
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
 * Answers the version probe: the result, the present version, and the
 * version itself, stored last with release order so that a client that
 * sees it also sees every reply before it.
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

	/* The result and the present version. */
	drv_i915_wire_reply_u32(&frame->reply, 0U);
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
 * The stream is copied first.
 */
static int
stream_blob(
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
	error = amdrdna_vk_blob(frame->session->owner, resource_id, &cpu, &bytes);
	if (error != 0)
		return EINVAL;
	if (offset > bytes || length > bytes - offset)
		return EINVAL;

	/* Copies the stream, so the decoder reads what was submitted. */
	copy = kern_malloc((size_t)length);
	if (copy == NULL)
		return ENOMEM;

	amdrdna_vk_blob_read(frame->session->owner, copy, cpu + (size_t)offset, (size_t)length);
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
