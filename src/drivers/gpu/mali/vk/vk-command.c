/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's command pools and command buffers, and the recording of
 * every vkCmd* they take.
 *
 * Each recorded command names its command buffer first.  A record keeps
 * the objects it names held, so they live until the record goes, and
 * its arguments as they came; a submission builds the GPU's command
 * stream from the records (vk-queue.c).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/mali/vk/vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The most items of one array a record takes, and the most records of one command buffer. */
#define RECORD_ITEMS_MAX	256U
#define RECORDS_MAX		65536U

static int create_pool(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_pool(struct mali_vk_session *session, struct i915_wire_reader *reader);
static int reset_pool(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void pool_buffers(struct mali_vk_session *session, const void *pool, bool remove);
static int allocate(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int buffer_destroy(struct mali_vk_session *session, void *payload);
static int free_buffers(struct mali_vk_session *session, struct i915_wire_reader *reader);
static int begin(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int end(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int reset(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int record(struct mali_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader);
static int decode(struct mali_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct mali_vk_record *entry);
static int hold(struct mali_vk_session *session, struct mali_vk_record *entry, enum i915_vk_object_kind kind, uint64_t identity);
static int take_data(struct mali_vk_session *session, struct mali_vk_record *entry, const void *bytes, size_t length);
static int decode_regions(struct mali_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct mali_vk_record *entry);
static int decode_barrier(struct mali_vk_session *session, struct i915_wire_reader *reader);
static void read_clear(struct i915_wire_reader *reader, uint32_t *words);
static void record_drop(struct mali_vk_record *entry);

/* Runs the command pool and command buffer commands, and records every vkCmd* taken. */
int
mali_vk_command_route(
	struct mali_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_COMMAND_POOL:
		error = create_pool(session, reader, reply);
		break;
	case GPU_OP_DESTROY_COMMAND_POOL:
		error = destroy_pool(session, reader);
		break;
	case GPU_OP_RESET_COMMAND_POOL:
		error = reset_pool(session, reader, reply);
		break;
	case GPU_OP_ALLOCATE_COMMAND_BUFFERS:
		error = allocate(session, reader, reply);
		break;
	case GPU_OP_FREE_COMMAND_BUFFERS:
		error = free_buffers(session, reader);
		break;
	case GPU_OP_BEGIN_COMMAND_BUFFER:
		error = begin(session, reader, reply);
		break;
	case GPU_OP_END_COMMAND_BUFFER:
		error = end(session, reader, reply);
		break;
	case GPU_OP_RESET_COMMAND_BUFFER:
		error = reset(session, reader, reply);
		break;
	case GPU_OP_CMD_BIND_PIPELINE:
	case GPU_OP_CMD_SET_VIEWPORT:
	case GPU_OP_CMD_SET_SCISSOR:
	case GPU_OP_CMD_SET_DEPTH_BIAS:
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
	case GPU_OP_CMD_DRAW:
	case GPU_OP_CMD_DRAW_INDEXED:
	case GPU_OP_CMD_DISPATCH:
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_FILL_BUFFER:
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
	case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
	case GPU_OP_CMD_PIPELINE_BARRIER:
	case GPU_OP_CMD_PUSH_CONSTANTS:
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
	case GPU_OP_CMD_NEXT_SUBPASS:
	case GPU_OP_CMD_END_RENDER_PASS:
		error = record(session, opcode, reader);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran or was recorded. */
	return 0;
}

/* Empties a command buffer of its records, letting go of what they hold. */
void
mali_vk_command_reset(
	struct mali_vk_command_buffer *command)
{
	uint32_t index;

	/* Each record's holds and data, then the array. */
	for (index = 0; index < command->record_count; index++)
		record_drop(&command->records[index]);
	kern_free(command->records);
	command->records = NULL;
	command->record_count = 0;
	command->record_capacity = 0;
	command->error = 0;
	command->state = MALI_VK_COMMAND_INITIAL;
}

/* vkCreateCommandPool: the create form with VkCommandPoolCreateInfo. */
static int
create_pool(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandPoolCreateInfo info;
	struct mali_vk_command_pool *pool;
	uint64_t present;
	uint64_t created;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkCommandPoolCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The payload. */
	pool = kern_calloc(1, sizeof(*pool));
	if (pool != NULL)
		pool->family = info.queueFamilyIndex;

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_COMMAND_POOL, created, pool, mali_vk_free_payload, 0);
	return 0;
}

/* vkDestroyCommandPool: the destroy form; its command buffers go first. */
static int
destroy_pool(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader)
{
	uint64_t gone;
	void *pool;

	/* The pool. */
	(void)drv_i915_wire_read_u64(reader);
	gone = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Its buffers, then itself. */
	pool = mali_vk_payload(session, I915_VK_OBJ_COMMAND_POOL, gone);
	if (pool != NULL)
		pool_buffers(session, pool, true);
	(void)mali_vk_object_remove(session, I915_VK_OBJ_COMMAND_POOL, gone);
	return 0;
}

/* vkResetCommandPool: [device][pool][flags] -> [result]: every buffer of the pool emptied. */
static int
reset_pool(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t named;
	void *pool;

	/* The pool and the flags. */
	(void)drv_i915_wire_read_u64(reader);
	named = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Its buffers. */
	pool = mali_vk_payload(session, I915_VK_OBJ_COMMAND_POOL, named);
	if (pool != NULL)
		pool_buffers(session, pool, false);

	/* Succeeded: the result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	return 0;
}

/* Empties or removes every command buffer of a pool. */
static void
pool_buffers(
	struct mali_vk_session *session,
	const void *pool,
	bool remove)
{
	struct mali_vk_object *object;
	struct mali_vk_command_buffer *command;
	uint64_t gone;
	bool found;

	/* Emptied in place. */
	if (!remove) {
		for (object = session->objects; object != NULL; object = object->next) {
			if (object->kind != I915_VK_OBJ_COMMAND_BUFFER)
				continue;
			command = object->payload;
			if (command->pool == pool)
				mali_vk_command_reset(command);
		}
		return;
	}

	/* Removed one at a time, as each removal changes the list. */
	do {
		found = false;
		for (object = session->objects; object != NULL; object = object->next) {
			if (object->kind != I915_VK_OBJ_COMMAND_BUFFER)
				continue;
			command = object->payload;
			if (command->pool != pool)
				continue;
			gone = object->identity;
			found = true;
			break;
		}

		if (found)
			(void)mali_vk_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, gone);
	} while (found);
}

/* vkAllocateCommandBuffers: [device][present][info][count][buffers] -> [result][count][buffers]. */
static int
allocate(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandBufferAllocateInfo info;
	struct mali_vk_command_buffer *command;
	uint64_t *identities;
	uint64_t present;
	uint64_t count;
	uint64_t index;
	void *pool;
	int error;

	/* The device, the info and the identities. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkCommandBufferAllocateInfo(reader, &session->arena, &info);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > RECORD_ITEMS_MAX || count != info.commandBufferCount)
		return EINVAL;
	identities = mali_vk_arena_take(session, (size_t)count * sizeof(uint64_t) + 8U);
	if (identities == NULL)
		return EINVAL;
	for (index = 0; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Each buffer of the pool. */
	pool = mali_vk_payload(session, I915_VK_OBJ_COMMAND_POOL, mali_vk_handle(info.commandPool));
	error = 0;
	if (pool == NULL || info.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
		error = ENOTSUP;
	for (index = 0; error == 0 && index < count; index++) {
		command = kern_calloc(1, sizeof(*command));
		if (command == NULL) {
			error = ENOMEM;
			break;
		}

		command->pool = pool;
		error = mali_vk_object_publish(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index], command, buffer_destroy);
		if (error != 0)
			kern_free(command);
	}

	/* A failure takes back the buffers made before it. */
	if (error != 0) {
		while (index > 0U) {
			index--;
			(void)mali_vk_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index]);
		}
		drv_i915_wire_reply_u32(reply, mali_vk_result(error));
		drv_i915_wire_reply_u64(reply, 0);
		return 0;
	}

	/* Succeeded: the identities echoed. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, count);
	for (index = 0; index < count; index++)
		drv_i915_wire_reply_u64(reply, identities[index]);
	return 0;
}

/* Destroys a command buffer: its records, then itself. */
static int
buffer_destroy(
	struct mali_vk_session *session,
	void *payload)
{
	(void)session;
	mali_vk_command_reset(payload);
	kern_free(payload);
	return 0;
}

/* vkFreeCommandBuffers: [device][pool][count][count][buffers]. */
static int
free_buffers(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader)
{
	uint64_t count;
	uint64_t index;
	uint64_t gone;

	/* The device, the pool and the counts. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > RECORD_ITEMS_MAX)
		return EINVAL;

	/* Each buffer. */
	for (index = 0; index < count; index++) {
		gone = drv_i915_wire_read_u64(reader);
		(void)mali_vk_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, gone);
	}

	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the buffers are gone. */
	return 0;
}

/* vkBeginCommandBuffer: [buffer][present][info] -> [result]: a recording afresh. */
static int
begin(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandBufferBeginInfo info;
	struct mali_vk_command_buffer *command;
	uint64_t named;
	uint64_t present;

	/* The buffer and the info. */
	kern_memset(&info, 0, sizeof(info));
	named = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkCommandBufferBeginInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return EINVAL;

	/* A known buffer starts recording with no record. */
	command = mali_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, named);
	if (command == NULL) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
		return 0;
	}

	mali_vk_command_reset(command);
	command->state = MALI_VK_COMMAND_RECORDING;

	/* Succeeded: the result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	return 0;
}

/* vkEndCommandBuffer: [buffer] -> [result]; a recording that met an error ends invalid. */
static int
end(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct mali_vk_command_buffer *command;
	uint64_t named;

	/* The buffer. */
	named = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A recording one ends. */
	command = mali_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, named);
	if (command == NULL || command->state != MALI_VK_COMMAND_RECORDING) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
		return 0;
	}

	if (command->error != 0) {
		command->state = MALI_VK_COMMAND_INVALID;
		drv_i915_wire_reply_u32(reply, mali_vk_result(command->error));
		return 0;
	}

	command->state = MALI_VK_COMMAND_EXECUTABLE;

	/* Succeeded: the result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	return 0;
}

/* vkResetCommandBuffer: [buffer][flags] -> [result]. */
static int
reset(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct mali_vk_command_buffer *command;
	uint64_t named;

	/* The buffer and the flags. */
	named = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Emptied. */
	command = mali_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, named);
	if (command != NULL)
		mali_vk_command_reset(command);

	/* Succeeded: the result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	return 0;
}

/*
 * Records one vkCmd*: [command buffer] then its arguments.  A record the
 * executor does not run fails the recording (vkEndCommandBuffer reports
 * it); one that does not decode fails the frame.
 */
static int
record(
	struct mali_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader)
{
	struct mali_vk_command_buffer *command;
	struct mali_vk_record entry;
	struct mali_vk_record *grown;
	uint32_t capacity;
	uint64_t named;
	int refused;

	/* The command buffer the record is for. */
	named = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The arguments, read to their end. */
	kern_memset(&entry, 0, sizeof(entry));
	entry.opcode = opcode;
	refused = decode(session, opcode, reader, &entry);
	if (refused == EINVAL || reader->error != 0) {
		record_drop(&entry);
		return EINVAL;
	}

	/* Only a recording buffer keeps it; a barrier keeps nothing. */
	command = mali_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, named);
	if (command == NULL || command->state != MALI_VK_COMMAND_RECORDING || opcode == GPU_OP_CMD_PIPELINE_BARRIER) {
		record_drop(&entry);
		return 0;
	}

	/* A refused record fails the recording. */
	if (refused != 0) {
		kern_logf("mali: vk: recorded command %u is not run (%d)\n", (unsigned)opcode, refused);
		record_drop(&entry);
		if (command->error == 0)
			command->error = refused;
		return 0;
	}

	/* Room for one more record. */
	if (command->record_count == command->record_capacity) {
		capacity = command->record_capacity * 2U;
		if (capacity == 0U)
			capacity = 32U;
		if (capacity > RECORDS_MAX) {
			record_drop(&entry);
			command->error = ENOMEM;
			return 0;
		}

		grown = kern_calloc(capacity, sizeof(*grown));
		if (grown == NULL) {
			record_drop(&entry);
			command->error = ENOMEM;
			return 0;
		}

		if (command->records != NULL)
			kern_memcpy(grown, command->records, command->record_count * sizeof(*grown));
		kern_free(command->records);
		command->records = grown;
		command->record_capacity = capacity;
	}

	/* Succeeded: the record is the buffer's. */
	command->records[command->record_count] = entry;
	command->record_count++;
	return 0;
}

/* Decodes one vkCmd*'s arguments into a record: EINVAL when they do not decode, ENOTSUP when not run. */
static int
decode(
	struct mali_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct mali_vk_record *entry)
{
	VkViewport viewport;
	VkRect2D rect;
	const uint8_t *bytes;
	uint64_t count;
	uint64_t index;
	uint64_t identity;
	uint32_t dynamic;
	uint32_t first;
	int error;

	/* One case a command. */
	error = 0;
	switch (opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
		/* [bind point][pipeline] */
		entry->values[0] = drv_i915_wire_read_u32(reader);
		error = hold(session, entry, I915_VK_OBJ_PIPELINE, drv_i915_wire_read_u64(reader));
		break;
	case GPU_OP_CMD_SET_VIEWPORT:
	case GPU_OP_CMD_SET_SCISSOR:
		/* [first][count][count][records]; only the first viewport or scissor is kept. */
		first = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || count > RECORD_ITEMS_MAX)
			return EINVAL;
		for (index = 0; index < count; index++) {
			if (opcode == GPU_OP_CMD_SET_VIEWPORT) {
				i915_vkc_dec_VkViewport(reader, &session->arena, &viewport);
				if (index == 0U && first == 0U)
					error = take_data(session, entry, &viewport, sizeof(viewport));
			} else {
				i915_vkc_dec_VkRect2D(reader, &session->arena, &rect);
				if (index == 0U && first == 0U)
					error = take_data(session, entry, &rect, sizeof(rect));
			}
		}
		break;
	case GPU_OP_CMD_SET_DEPTH_BIAS:
		/* [constant factor][clamp][slope factor], as float bits */
		for (index = 0; index < 3U; index++)
			entry->values[index] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
		/* [face mask][value] */
		entry->values[0] = drv_i915_wire_read_u32(reader);
		entry->values[1] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		/* [bind point][layout][first][count][count][sets][dynamic][dynamic][offsets] */
		entry->values[0] = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u64(reader);
		entry->values[1] = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || count > MALI_VK_SETS_MAX)
			return EINVAL;
		for (index = 0; index < count; index++) {
			identity = drv_i915_wire_read_u64(reader);
			if (error == 0)
				error = hold(session, entry, I915_VK_OBJ_DESCRIPTOR_SET, identity);
		}
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || count > RECORD_ITEMS_MAX)
			return EINVAL;
		for (index = 0; index < count; index++) {
			dynamic = drv_i915_wire_read_u32(reader);
			(void)dynamic;
		}
		if (count != 0U && error == 0)
			error = ENOTSUP;
		break;
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
		/* [buffer][offset][index type] */
		error = hold(session, entry, I915_VK_OBJ_BUFFER, drv_i915_wire_read_u64(reader));
		entry->wide[0] = drv_i915_wire_read_u64(reader);
		entry->values[0] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
		/* [first][count][count][buffers][count][offsets] */
		entry->values[0] = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || count > MALI_VK_RECORD_OBJECTS)
			return EINVAL;
		for (index = 0; index < count; index++) {
			identity = drv_i915_wire_read_u64(reader);
			if (error == 0)
				error = hold(session, entry, I915_VK_OBJ_BUFFER, identity);
		}
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || count > MALI_VK_RECORD_OBJECTS)
			return EINVAL;
		for (index = 0; index < count; index++)
			entry->wide[index] = drv_i915_wire_read_u64(reader);
		break;
	case GPU_OP_CMD_DRAW:
		/* [vertices][instances][first vertex][first instance] */
		for (index = 0; index < 4U; index++)
			entry->values[index] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_DRAW_INDEXED:
		/* [indices][instances][first index][vertex offset][first instance] */
		for (index = 0; index < 5U; index++)
			entry->values[index] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_DISPATCH:
		/* [x][y][z] */
		for (index = 0; index < 3U; index++)
			entry->values[index] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_FILL_BUFFER:
		/* [buffer][offset][size][data] */
		error = hold(session, entry, I915_VK_OBJ_BUFFER, drv_i915_wire_read_u64(reader));
		entry->wide[0] = drv_i915_wire_read_u64(reader);
		entry->wide[1] = drv_i915_wire_read_u64(reader);
		entry->values[0] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
	case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
		error = decode_regions(session, opcode, reader, entry);
		break;
	case GPU_OP_CMD_PIPELINE_BARRIER:
		error = decode_barrier(session, reader);
		break;
	case GPU_OP_CMD_PUSH_CONSTANTS:
		/* [layout][stages][offset][size][size][bytes] */
		(void)drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		entry->values[0] = drv_i915_wire_read_u32(reader);
		entry->values[1] = drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || count != entry->values[1] || entry->values[0] + count > MALI_VK_PUSH_BYTES)
			return EINVAL;
		bytes = drv_i915_wire_read_array(reader, (size_t)count, 1);
		if (bytes != NULL)
			error = take_data(session, entry, bytes, (size_t)count);
		break;
	case GPU_OP_CMD_NEXT_SUBPASS:
		/* [contents]: a pass has one subpass. */
		(void)drv_i915_wire_read_u32(reader);
		error = ENOTSUP;
		break;
	case GPU_OP_CMD_END_RENDER_PASS:
		/* No argument. */
		break;
	default:
		return EINVAL;
	}

	/* A command that did not decode. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded unless the command is not run. */
	if (error != 0)
		return error;
	return 0;
}

/* Holds a published object of a kind in the record; a missing one is refused. */
static int
hold(
	struct mali_vk_session *session,
	struct mali_vk_record *entry,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct mali_vk_object *object;
	int error;

	/* The null handle holds nothing. */
	if (identity == 0U) {
		if (entry->object_count < MALI_VK_RECORD_OBJECTS)
			entry->object_count++;
		return 0;
	}

	/* A known object, in the next place. */
	object = mali_vk_object_find(session, kind, identity);
	if (object == NULL || entry->object_count >= MALI_VK_RECORD_OBJECTS)
		return ENOTSUP;
	error = mali_vk_object_retain(object);
	if (error != 0)
		return ENOTSUP;
	entry->objects[entry->object_count] = object;
	entry->object_count++;

	/* Succeeded: the record holds it. */
	return 0;
}

/* Keeps a copy of bytes as the record's data. */
static int
take_data(
	struct mali_vk_session *session,
	struct mali_vk_record *entry,
	const void *bytes,
	size_t length)
{
	/* One block of data a record. */
	(void)session;
	if (entry->data != NULL || length == 0U)
		return 0;
	entry->data = kern_malloc(length);
	if (entry->data == NULL)
		return ENOMEM;
	kern_memcpy(entry->data, bytes, length);
	entry->data_bytes = (uint32_t)length;

	/* Succeeded: the record owns the copy. */
	return 0;
}

/*
 * Decodes the copies, the blit, the clears and the pass's start: the
 * objects, then the regions or the clear values as the record's data.
 *
 * The regions are kept as the codec decodes them (VkBufferCopy,
 * VkImageCopy, VkImageBlit, VkBufferImageCopy, VkImageSubresourceRange);
 * a clear of attachments keeps, per attachment, [aspect][index][tag]
 * [four words], then the rectangles; a pass's start keeps its clear
 * values as [tag][four words] each.  A clear of a depth and stencil
 * image keeps its depth's float bits and its stencil in the first two
 * values (zero when the value is absent).
 */
static int
decode_regions(
	struct mali_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct mali_vk_record *entry)
{
	uint32_t *words;
	uint8_t *regions;
	uint64_t count;
	uint64_t rects;
	uint64_t present;
	uint64_t index;
	size_t size;
	int error;

	/* The objects and the scalars before the array. */
	error = 0;
	size = 0;
	if (opcode == GPU_OP_CMD_COPY_BUFFER) {
		error = hold(session, entry, I915_VK_OBJ_BUFFER, drv_i915_wire_read_u64(reader));
		if (error == 0)
			error = hold(session, entry, I915_VK_OBJ_BUFFER, drv_i915_wire_read_u64(reader));
		size = sizeof(VkBufferCopy);
	} else if (opcode == GPU_OP_CMD_COPY_IMAGE || opcode == GPU_OP_CMD_BLIT_IMAGE) {
		error = hold(session, entry, I915_VK_OBJ_IMAGE, drv_i915_wire_read_u64(reader));
		(void)drv_i915_wire_read_u32(reader);
		if (error == 0)
			error = hold(session, entry, I915_VK_OBJ_IMAGE, drv_i915_wire_read_u64(reader));
		(void)drv_i915_wire_read_u32(reader);
		size = sizeof(VkImageCopy);
		if (opcode == GPU_OP_CMD_BLIT_IMAGE)
			size = sizeof(VkImageBlit);
	} else if (opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE) {
		error = hold(session, entry, I915_VK_OBJ_BUFFER, drv_i915_wire_read_u64(reader));
		if (error == 0)
			error = hold(session, entry, I915_VK_OBJ_IMAGE, drv_i915_wire_read_u64(reader));
		(void)drv_i915_wire_read_u32(reader);
		size = sizeof(VkBufferImageCopy);
	} else if (opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER) {
		error = hold(session, entry, I915_VK_OBJ_IMAGE, drv_i915_wire_read_u64(reader));
		(void)drv_i915_wire_read_u32(reader);
		if (error == 0)
			error = hold(session, entry, I915_VK_OBJ_BUFFER, drv_i915_wire_read_u64(reader));
		size = sizeof(VkBufferImageCopy);
	} else if (opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
		/* [image][layout][present][tag][count][four words] */
		error = hold(session, entry, I915_VK_OBJ_IMAGE, drv_i915_wire_read_u64(reader));
		(void)drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		if (drv_i915_wire_read_u64(reader) != 4U)
			return EINVAL;
		for (index = 0; index < 4U; index++)
			entry->values[index] = drv_i915_wire_read_u32(reader);
		size = sizeof(VkImageSubresourceRange);
	} else if (opcode == GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE) {
		/* [image][layout][present][depth][stencil]: the value only when present is one. */
		error = hold(session, entry, I915_VK_OBJ_IMAGE, drv_i915_wire_read_u64(reader));
		(void)drv_i915_wire_read_u32(reader);
		present = drv_i915_wire_read_u64(reader);
		if (present > 1U)
			return EINVAL;
		if (present == 1U) {
			entry->values[0] = drv_i915_wire_read_u32(reader);
			entry->values[1] = drv_i915_wire_read_u32(reader);
		}
		size = sizeof(VkImageSubresourceRange);
	} else if (opcode == GPU_OP_CMD_BEGIN_RENDER_PASS) {
		/* [present][sType][chain][pass][framebuffer][area] */
		(void)drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u64(reader);
		error = hold(session, entry, I915_VK_OBJ_RENDER_PASS, drv_i915_wire_read_u64(reader));
		if (error == 0)
			error = hold(session, entry, I915_VK_OBJ_FRAMEBUFFER, drv_i915_wire_read_u64(reader));
		entry->values[0] = drv_i915_wire_read_u32(reader);
		entry->values[1] = drv_i915_wire_read_u32(reader);
		entry->values[2] = drv_i915_wire_read_u32(reader);
		entry->values[3] = drv_i915_wire_read_u32(reader);
	}

	/* The count of the array. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > RECORD_ITEMS_MAX)
		return EINVAL;
	entry->values[7] = (uint32_t)count;

	/* A pass's clear values, five words each, and its contents word. */
	if (opcode == GPU_OP_CMD_BEGIN_RENDER_PASS) {
		words = mali_vk_arena_take(session, (size_t)count * 20U + 8U);
		if (words == NULL)
			return EINVAL;
		for (index = 0; index < count; index++)
			read_clear(reader, &words[index * 5U]);
		(void)drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		if (error == 0 && count != 0U)
			error = take_data(session, entry, words, (size_t)count * 20U);
		return error;
	}

	/* A clear of attachments: seven words an attachment, then the rectangles. */
	if (opcode == GPU_OP_CMD_CLEAR_ATTACHMENTS) {
		words = mali_vk_arena_take(session, (size_t)count * 28U + RECORD_ITEMS_MAX * sizeof(VkClearRect) + 8U);
		if (words == NULL)
			return EINVAL;
		for (index = 0; index < count; index++) {
			words[index * 7U] = drv_i915_wire_read_u32(reader);
			words[index * 7U + 1U] = drv_i915_wire_read_u32(reader);
			read_clear(reader, &words[index * 7U + 2U]);
		}
		(void)drv_i915_wire_read_u32(reader);
		rects = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || rects > RECORD_ITEMS_MAX)
			return EINVAL;
		regions = (uint8_t *)&words[count * 7U];
		for (index = 0; index < rects; index++)
			i915_vkc_dec_VkClearRect(reader, &session->arena, (VkClearRect *)(void *)(regions + index * sizeof(VkClearRect)));
		if (reader->error != 0)
			return EINVAL;
		entry->values[6] = (uint32_t)rects;
		if (count != 0U)
			error = take_data(session, entry, words, (size_t)count * 28U + (size_t)rects * sizeof(VkClearRect));
		return error;
	}

	/* The regions, decoded into the scratch, then kept. */
	regions = mali_vk_arena_take(session, (size_t)count * size + 8U);
	if (regions == NULL)
		return EINVAL;
	for (index = 0; index < count; index++) {
		if (opcode == GPU_OP_CMD_COPY_BUFFER) {
			i915_vkc_dec_VkBufferCopy(reader, &session->arena, (VkBufferCopy *)(void *)(regions + index * size));
		} else if (opcode == GPU_OP_CMD_COPY_IMAGE) {
			i915_vkc_dec_VkImageCopy(reader, &session->arena, (VkImageCopy *)(void *)(regions + index * size));
		} else if (opcode == GPU_OP_CMD_BLIT_IMAGE) {
			i915_vkc_dec_VkImageBlit(reader, &session->arena, (VkImageBlit *)(void *)(regions + index * size));
		} else if (opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE || opcode == GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE) {
			i915_vkc_dec_VkImageSubresourceRange(reader, &session->arena, (VkImageSubresourceRange *)(void *)(regions + index * size));
		} else {
			i915_vkc_dec_VkBufferImageCopy(reader, &session->arena, (VkBufferImageCopy *)(void *)(regions + index * size));
		}
	}

	/* A blit's filter. */
	if (opcode == GPU_OP_CMD_BLIT_IMAGE)
		entry->values[5] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the regions are the record's. */
	if (error == 0 && count != 0U)
		error = take_data(session, entry, regions, (size_t)count * size);
	return error;
}

/*
 * Reads a pipeline barrier to its end: every submission runs its work in
 * order and waits for all of it, so a barrier has nothing to do.
 */
static int
decode_barrier(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader)
{
	VkMemoryBarrier memory;
	VkBufferMemoryBarrier buffer;
	VkImageMemoryBarrier image;
	uint64_t count;
	uint64_t index;

	/* The stages and the dependency flags. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The memory barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > RECORD_ITEMS_MAX)
		return EINVAL;
	for (index = 0; index < count; index++)
		i915_vkc_dec_VkMemoryBarrier(reader, &session->arena, &memory);

	/* The buffer barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > RECORD_ITEMS_MAX)
		return EINVAL;
	for (index = 0; index < count; index++)
		i915_vkc_dec_VkBufferMemoryBarrier(reader, &session->arena, &buffer);

	/* The image barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > RECORD_ITEMS_MAX)
		return EINVAL;
	for (index = 0; index < count; index++)
		i915_vkc_dec_VkImageMemoryBarrier(reader, &session->arena, &image);

	/* Succeeded: read to its end. */
	if (reader->error != 0)
		return EINVAL;
	return 0;
}

/*
 * Reads one tagged clear value into five words: the tag (0 colour, 1
 * depth and stencil), then four colour words or the depth and stencil.
 */
static void
read_clear(
	struct i915_wire_reader *reader,
	uint32_t *words)
{
	uint32_t index;

	/* The tag. */
	kern_memset(words, 0, 5U * sizeof(uint32_t));
	words[0] = drv_i915_wire_read_u32(reader);
	if (words[0] != 0U) {
		words[1] = drv_i915_wire_read_u32(reader);
		words[2] = drv_i915_wire_read_u32(reader);
		return;
	}

	/* A colour: its member's tag, the count four, and the words. */
	(void)drv_i915_wire_read_u32(reader);
	if (drv_i915_wire_read_u64(reader) != 4U)
		reader->error = 1;
	for (index = 0; index < 4U; index++)
		words[1U + index] = drv_i915_wire_read_u32(reader);
}

/* Lets go of what a record holds and owns. */
static void
record_drop(
	struct mali_vk_record *entry)
{
	uint32_t index;

	/* The objects, then the data. */
	for (index = 0; index < entry->object_count; index++)
		mali_vk_object_release(entry->objects[index]);
	kern_free(entry->data);
	kern_memset(entry, 0, sizeof(*entry));
}
