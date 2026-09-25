/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transport of the Vulkan executor (see transport.h).
 *
 * The reply target is resolved before the stream is decoded, from the
 * fixed-size selector the stream opens with; the selector itself then only
 * restarts the reply cursor.  An external stream is copied out of its blob
 * before it is decoded, because the sender keeps its mapping and could
 * change the bytes underneath the decoder.
 */

#include "transport.h"
#include "codec.h"
#include "dispatch.h"
#include <kern/kcrt.h>

#include "../memory.h"
#include "../session.h"

#include <kern/kmem.h>
#include <kern/pmem.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* The transport opcodes. */
#define I915_OPCODE_ENUMERATE_INSTANCE_VERSION	137U
#define I915_OPCODE_SET_REPLY_STREAM		178U
#define I915_OPCODE_SEEK_REPLY_STREAM		179U
#define I915_OPCODE_EXECUTE_STREAMS		180U

/* The reply selector a stream opens with: [178][flag][present][resource id][offset][capacity]. */
#define I915_REPLY_SELECTOR_BYTES		36U
#define I915_REPLY_SELECTOR_RESOURCE		16U
#define I915_REPLY_SELECTOR_OFFSET		20U

/* The largest external stream decoded in one command. */
#define I915_EXTERNAL_STREAM_MAX_BYTES		(64U << 20)

/* The API version the version probe reports: Vulkan 1.1.0. */
#define I915_TRANSPORT_API_VERSION		((1U << 22) | (1U << 12))

static struct i915_gem_object *i915_transport_resource(struct i915_session *session, uint32_t resource_id);
static int i915_transport_set_reply(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_transport_seek_reply(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_transport_version(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_transport_execute_streams(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

/*
 * Resolves the reply target a Vulkan command stream selects up front.
 *
 * The stream opens with vkSetReplyCommandStreamMESA naming a session blob
 * by resource id; the blob's kernel alias, from the selected offset on, is
 * where the executor writes the replies.  A stream without the selector,
 * or naming no blob of the session, replies nowhere: NULL, with a capacity
 * of zero.
 */
void *
drv_i915_render_transport_reply(
	struct i915_session *session,
	const void *wire,
	uint32_t bytes,
	size_t *capacity)
{
	const uint8_t *stream;
	struct i915_gem_object *object;
	uint8_t *alias;
	uint32_t opcode;
	uint32_t resource_id;
	uint64_t offset;

	/* A stream that selects nothing has no reply region. */
	*capacity = 0U;
	stream = wire;

	/* The selector is a fixed record at the start of the stream. */
	if (bytes < I915_REPLY_SELECTOR_BYTES)
		return NULL;

	/* Reads the opcode; any other first command selects nothing. */
	kern_memcpy(&opcode, stream, 4U);
	if (opcode != I915_OPCODE_SET_REPLY_STREAM)
		return NULL;

	/* Reads the resource id and the offset the replies start at. */
	kern_memcpy(&resource_id, stream + I915_REPLY_SELECTOR_RESOURCE, 4U);
	kern_memcpy(&offset, stream + I915_REPLY_SELECTOR_OFFSET, 8U);

	/* Finds the named blob among the session's resources. */
	object = i915_transport_resource(session, resource_id);
	if (object == NULL)
		return NULL;

	/* An offset at or past the end leaves no room for a reply. */
	if (offset >= object->bytes)
		return NULL;

	/* Reports the room from the offset to the end of the blob. */
	*capacity = (size_t)(object->bytes - offset);
	alias = kern_pmem_to_kernel(object->run.paddr);

	/* Succeeded: the replies land in the blob libvulkan reads back. */
	return alias + offset;
}

/*
 * Decodes and executes the commands of one stream back to back.
 *
 * Each command starts with an empty arena.  The first refused command stops
 * the stream, and its error is reported.
 */
int
drv_i915_render_transport_execute(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	int error;

	/* Hands each command to the router until the stream ends or a command is refused. */
	error = 0;
	while (error == 0 && reader->offset < reader->size) {
		session->arena.used = 0U;
		error = drv_i915_render_dispatch(session, reader, reply);
	}

	/* Reports the command that was refused. */
	if (error != 0)
		return error;

	/* Succeeded: every command of the stream was decoded and executed. */
	return 0;
}

/*
 * Executes a transport command.
 *
 * handled is cleared for an opcode the transport does not own.
 */
int
drv_i915_render_transport_dispatch(
	struct i915_render_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the transport command, or reports the opcode as someone else's. */
	*handled = 1;
	switch (opcode) {
	case I915_OPCODE_SET_REPLY_STREAM:
		error = i915_transport_set_reply(reader, reply);
		break;
	case I915_OPCODE_SEEK_REPLY_STREAM:
		error = i915_transport_seek_reply(reader, reply);
		break;
	case I915_OPCODE_ENUMERATE_INSTANCE_VERSION:
		error = i915_transport_version(reader, reply);
		break;
	case I915_OPCODE_EXECUTE_STREAMS:
		error = i915_transport_execute_streams(session, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Reports why the transport command was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the transport command was executed. */
	return 0;
}

/* Finds the session resource numbered with a resource id, or NULL. */
static struct i915_gem_object *
i915_transport_resource(
	struct i915_session *session,
	uint32_t resource_id)
{
	struct i915_gem_object *object;

	/* Scans the session's resources; the first one with the slot is the one named. */
	object = session->objects;
	while (object != NULL && object->slot != resource_id)
		object = object->session_next;

	/* Reports the resource, or NULL for an id the session does not own. */
	return object;
}

/*
 * vkSetReplyCommandStreamMESA: [present][resource][offset][capacity].
 *
 * The reply region was resolved before the stream was decoded; the
 * selector only restarts the reply cursor.
 */
static int
i915_transport_set_reply(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	/* Skips the selector's fields: present, resource id, offset and capacity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Restarts the replies at the start of the selected region. */
	reply->offset = 0U;

	/* Succeeded: the following replies land from the start of the region. */
	return 0;
}

/*
 * vkSeekReplyCommandStreamMESA: [offset].
 *
 * Moves the reply cursor, so the version probe that follows writes the
 * completion trailer at its fixed position.
 */
static int
i915_transport_seek_reply(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t offset;

	/* Reads the position the next reply is written at. */
	offset = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A position past the end of the region is refused. */
	if (offset > reply->size)
		return EINVAL;

	/* Moves the cursor. */
	reply->offset = offset;

	/* Succeeded: the next reply is written at the position. */
	return 0;
}

/*
 * vkEnumerateInstanceVersion: [pApiVersion present].
 *
 * It reports the version and doubles as the completion probe: its reply is
 * the 20-byte trailer [opcode][result][present][apiVersion], the version
 * word written last.
 */
static int
i915_transport_version(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	/* Skips the pApiVersion present word. */
	(void)drv_i915_wire_read_u64(reader);

	/* Writes VK_SUCCESS, the present word and the version. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, I915_TRANSPORT_API_VERSION);

	/* Succeeded: the trailer is written. */
	return 0;
}

/*
 * vkExecuteCommandStreamsMESA as context.c sends it: [stream count = 1]
 * [present][resource][offset][bytes][pReplyOffsets = 0][dependent count = 0]
 * [pDependents = 0][flags = 0].
 *
 * The stream is a session blob; its commands are decoded in place of this
 * one and reply to the same writer.
 *
 * XXX: one stream is read, the reply offsets, dependencies and flags are
 * not checked, and a stream that names another external stream is decoded
 * recursively without a depth limit.
 */
static int
i915_transport_execute_streams(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_gem_object *object;
	struct i915_wire_reader nested;
	const uint8_t *source;
	uint64_t offset;
	uint64_t bytes;
	uint32_t resource;
	uint8_t *copy;
	int error;

	/* Reads the one stream's resource, offset and length, and skips the rest. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	resource = drv_i915_wire_read_u32(reader);
	offset = drv_i915_wire_read_u64(reader);
	bytes = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Finds the blob the stream lives in. */
	object = i915_transport_resource(session->gpu, resource);
	if (object == NULL)
		return EINVAL;

	/* The stream must lie inside the blob. */
	if (offset > object->bytes)
		return EINVAL;
	if (bytes > object->bytes - offset)
		return EINVAL;

	/* An empty stream and one larger than the decode bound are refused. */
	if (bytes == 0U)
		return EINVAL;
	if (bytes > I915_EXTERNAL_STREAM_MAX_BYTES)
		return EINVAL;

	/* Allocates the private copy the stream is decoded from. */
	copy = kern_malloc((size_t)bytes);
	if (copy == NULL)
		return ENOMEM;

	/* Copies the stream out of the blob, so its bytes cannot change underneath the decoder. */
	source = kern_pmem_to_kernel(object->run.paddr);
	kern_memcpy(copy, source + offset, (size_t)bytes);

	/* Decodes the copy's commands in place of this one, into the same reply. */
	nested.base = copy;
	nested.size = (size_t)bytes;
	nested.offset = 0U;
	nested.error = 0;
	error = drv_i915_render_transport_execute(session, &nested, reply);

	kern_free(copy);

	/* Reports the nested command that was refused. */
	if (error != 0)
		return error;

	/* Succeeded: every command of the external stream was executed. */
	return 0;
}
