/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's command pools, command buffers and the commands they
 * record.
 *
 * A command buffer keeps the commands a submission runs: the compute
 * pipeline bound, the sets bound, and each dispatch.  A barrier is read
 * and not kept: a submission runs its dispatches in order with a fence
 * between each (vk-queue.c).  Any other recorded command fails the
 * recording, which its End reports.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/bcm2711/vk/vulkan-command-pool.c,
 * vulkan-command-buffer.c and vulkan-record-decode.c (the wire of each
 * command).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/powervr/vk/vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The command buffers one call carries, the barriers one barrier command carries, and a recording's first room. */
#define COMMAND_BATCH_MAX		32U
#define COMMAND_BARRIERS_MAX		64U
#define COMMAND_RECORDS_FIRST		16U
#define COMMAND_RECORDS_MAX		4096U

static int create_pool(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_pool(struct powervr_vk_session *session, struct i915_wire_reader *reader);
static int reset_pool(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int pool_free(struct powervr_vk_session *session, const void *pool);
static int allocate(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int free_buffers(struct powervr_vk_session *session, struct i915_wire_reader *reader);
static int begin(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int end(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int reset(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int record(struct powervr_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader);
static int record_pipeline(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct powervr_vk_record *entry);
static int record_sets(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct powervr_vk_record *entry);
static int record_dispatch(struct i915_wire_reader *reader, struct powervr_vk_record *entry);
static int record_barrier(struct powervr_vk_session *session, struct i915_wire_reader *reader);
static int record_hold(struct powervr_vk_session *session, struct powervr_vk_record *entry, enum i915_vk_object_kind kind, uint64_t identity);
static int record_append(struct powervr_vk_command_buffer *command, const struct powervr_vk_record *entry);
static void record_drop(struct powervr_vk_record *entry);
static int command_destroy(struct powervr_vk_session *session, void *payload);

/*
 * Runs a command pool or command buffer command, or records a command;
 * *handled stays clear for another handler's opcode.
 */
int
powervr_vk_command_route(
	struct powervr_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	(void)requested;
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
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
	case GPU_OP_CMD_DISPATCH:
	case GPU_OP_CMD_PIPELINE_BARRIER:
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
	case GPU_OP_CMD_NEXT_SUBPASS:
	case GPU_OP_CMD_END_RENDER_PASS:
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
	case GPU_OP_CMD_SET_VIEWPORT:
	case GPU_OP_CMD_SET_SCISSOR:
	case GPU_OP_CMD_PUSH_CONSTANTS:
	case GPU_OP_CMD_DRAW:
	case GPU_OP_CMD_DRAW_INDEXED:
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

/* Empties a command buffer: its records let their objects go, and it is back in its initial state. */
void
powervr_vk_command_reset(
	struct powervr_vk_command_buffer *command)
{
	uint32_t index;

	/* Every record. */
	for (index = 0; index < command->record_count; index++)
		record_drop(&command->records[index]);

	/* No record, no error, the initial state. */
	command->record_count = 0;
	command->state = POWERVR_VK_COMMAND_INITIAL;
	command->error = 0;
}

/* vkCreateCommandPool: [device][present][VkCommandPoolCreateInfo] and the create's tail -> the create's reply. */
static int
create_pool(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandPoolCreateInfo info;
	struct powervr_vk_command_pool *pool;
	uint64_t identity;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkCommandPoolCreateInfo(reader, &session->arena, &info);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A pool of the one family. */
	error = 0;
	if (info.queueFamilyIndex != 0)
		error = EINVAL;

	/* The pool. */
	pool = NULL;
	if (error == 0) {
		pool = kern_calloc(1, sizeof(*pool));
		if (pool != NULL)
			pool->family = info.queueFamilyIndex;
	}

	/* Publishes the pool and answers. */
	powervr_vk_create_reply(session, reply, I915_VK_OBJ_COMMAND_POOL, identity, pool, powervr_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/* vkDestroyCommandPool: [device][identity][allocator], no reply body.  The pool's command buffers go with it. */
static int
destroy_pool(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader)
{
	const struct powervr_vk_command_pool *pool;
	uint64_t identity;
	int error;

	/* The identity between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An unknown pool is already gone. */
	pool = powervr_vk_payload(session, I915_VK_OBJ_COMMAND_POOL, identity);
	if (pool == NULL)
		return 0;

	/* The pool's command buffers, then the pool. */
	error = pool_free(session, pool);
	if (error == 0)
		error = powervr_vk_object_remove(session, I915_VK_OBJ_COMMAND_POOL, identity);
	if (error != 0)
		return error;

	/* Succeeded: the pool and its command buffers are gone. */
	return 0;
}

/* vkResetCommandPool: [device][identity][flags] -> [result].  Every command buffer of the pool is emptied. */
static int
reset_pool(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const struct powervr_vk_command_pool *pool;
	struct powervr_vk_object *object;
	struct powervr_vk_command_buffer *command;
	uint64_t identity;

	/* The identity between the device and the flags. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Every command buffer of a known pool. */
	pool = powervr_vk_payload(session, I915_VK_OBJ_COMMAND_POOL, identity);
	for (object = session->objects; pool != NULL && object != NULL; object = object->next) {
		if (object->kind != I915_VK_OBJ_COMMAND_BUFFER)
			continue;
		command = object->payload;
		if (command->pool == pool)
			powervr_vk_command_reset(command);
	}

	/* Success. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/* Removes every published command buffer of a pool; the walk starts over after each removal. */
static int
pool_free(
	struct powervr_vk_session *session,
	const void *pool)
{
	struct powervr_vk_object *object;
	const struct powervr_vk_command_buffer *command;
	bool removed;
	int error;

	/* Until a walk finds no command buffer of the pool. */
	do {
		removed = false;
		for (object = session->objects; object != NULL; object = object->next) {
			if (object->kind != I915_VK_OBJ_COMMAND_BUFFER)
				continue;
			command = object->payload;
			if (command->pool != pool)
				continue;

			/* Unpublishes it. */
			error = powervr_vk_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, object->identity);
			if (error != 0)
				return error;
			removed = true;
			break;
		}
	} while (removed);

	/* Succeeded: the pool has no command buffer. */
	return 0;
}

/*
 * vkAllocateCommandBuffers: [device][present][VkCommandBufferAllocateInfo]
 * [count][identities] -> [result][count][identities].  Primary buffers of
 * a known pool, all of them or none.
 */
static int
allocate(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandBufferAllocateInfo info;
	uint64_t identities[COMMAND_BATCH_MAX];
	const struct powervr_vk_command_pool *pool;
	struct powervr_vk_command_buffer *command;
	uint64_t count;
	uint64_t index;
	uint64_t published;
	uint32_t result;
	int error;

	/* The allocate info behind the device and its presence word, then the identities. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkCommandBufferAllocateInfo(reader, &session->arena, &info);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > COMMAND_BATCH_MAX || count != info.commandBufferCount)
		return EINVAL;
	for (index = 0; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Primary buffers of a known pool. */
	error = 0;
	pool = powervr_vk_payload(session, I915_VK_OBJ_COMMAND_POOL, (uint64_t)(uintptr_t)info.commandPool);
	if (pool == NULL) {
		error = EINVAL;
	} else if (info.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
		error = ENOTSUP;
	}

	/* Publishes each, up to the first failure. */
	published = 0;
	for (index = 0; error == 0 && index < count; index++) {
		command = kern_calloc(1, sizeof(*command));
		if (command == NULL) {
			error = ENOMEM;
			break;
		}

		command->pool = pool;
		error = powervr_vk_object_publish(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index], command, command_destroy);
		if (error != 0) {
			kern_free(command);
			break;
		}

		published++;
	}

	/* A batch that failed part-way leaves none. */
	if (error != 0) {
		for (index = 0; index < published; index++)
			(void)powervr_vk_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index]);
	}

	/* The result, and on success the count and every identity. */
	result = powervr_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);
	if (error == 0) {
		drv_i915_wire_reply_u64(reply, count);
		for (index = 0; index < count; index++)
			drv_i915_wire_reply_u64(reply, identities[index]);
	}

	/* Succeeded: the reply carries the allocation's result. */
	return 0;
}

/* vkFreeCommandBuffers: [device][pool][count][count]{identity}, no reply body. */
static int
free_buffers(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader)
{
	uint64_t count;
	uint64_t index;
	uint64_t identity;
	int error;

	/* The count behind the device, the pool and the count's first form. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > COMMAND_BATCH_MAX)
		return EINVAL;

	/* Removes each; an unknown or null one is already gone. */
	for (index = 0; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;
		if (identity == 0)
			continue;
		error = powervr_vk_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
		if (error != 0 && error != ENOENT)
			return error;
	}

	/* Succeeded: the command buffers are gone. */
	return 0;
}

/* vkBeginCommandBuffer: [command buffer][present][VkCommandBufferBeginInfo] -> [result]. */
static int
begin(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandBufferBeginInfo info;
	struct powervr_vk_command_buffer *command;
	uint64_t identity;
	uint64_t present;
	uint32_t result;
	int error;

	/* The command buffer and its begin info. */
	kern_memset(&info, 0, sizeof(info));
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0)
		i915_vkc_dec_VkCommandBufferBeginInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return EINVAL;

	/* A known command buffer starts recording afresh. */
	error = EINVAL;
	command = powervr_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (command != NULL) {
		powervr_vk_command_reset(command);
		command->state = POWERVR_VK_COMMAND_RECORDING;
		error = 0;
	}

	/* The begin's result. */
	result = powervr_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/* vkEndCommandBuffer: [command buffer] -> [result]; a recording that met an error ends invalid. */
static int
end(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct powervr_vk_command_buffer *command;
	uint64_t identity;
	uint32_t result;
	int error;

	/* The command buffer. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A recording command buffer ends executable, or invalid with its first error. */
	error = EINVAL;
	command = powervr_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (command != NULL && command->state == POWERVR_VK_COMMAND_RECORDING) {
		error = command->error;
		command->state = POWERVR_VK_COMMAND_EXECUTABLE;
		if (error != 0)
			command->state = POWERVR_VK_COMMAND_INVALID;
	}

	/* The end's result. */
	result = powervr_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/* vkResetCommandBuffer: [command buffer][flags] -> [result]. */
static int
reset(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct powervr_vk_command_buffer *command;
	uint64_t identity;
	uint32_t result;
	int error;

	/* The command buffer and the flags. */
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A known command buffer is emptied. */
	error = EINVAL;
	command = powervr_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (command != NULL) {
		powervr_vk_command_reset(command);
		error = 0;
	}

	/* The reset's result. */
	result = powervr_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/*
 * Records one vkCmd* command: [command buffer] then its arguments.  A
 * command that does not decode fails the frame; one that decodes but
 * cannot be run fails the recording.
 */
static int
record(
	struct powervr_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader)
{
	struct powervr_vk_command_buffer *command;
	struct powervr_vk_record entry;
	uint64_t identity;
	int refused;
	int error;

	/* The command buffer the record is for. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Reads the arguments to their end. */
	kern_memset(&entry, 0, sizeof(entry));
	entry.opcode = opcode;
	if (opcode == GPU_OP_CMD_BIND_PIPELINE) {
		refused = record_pipeline(session, reader, &entry);
	} else if (opcode == GPU_OP_CMD_BIND_DESCRIPTOR_SETS) {
		refused = record_sets(session, reader, &entry);
	} else if (opcode == GPU_OP_CMD_DISPATCH) {
		refused = record_dispatch(reader, &entry);
	} else if (opcode == GPU_OP_CMD_BEGIN_RENDER_PASS) {
		refused = powervr_vk_record_begin_pass(session, reader, &entry);
	} else if (opcode == GPU_OP_CMD_CLEAR_ATTACHMENTS) {
		refused = powervr_vk_record_clear_attachments(session, reader, &entry);
	} else if (opcode == GPU_OP_CMD_NEXT_SUBPASS) {
		/* [contents]: a second subpass is never created, so this is a client error. */
		(void)drv_i915_wire_read_u32(reader);
		refused = ENOTSUP;
	} else if (opcode == GPU_OP_CMD_END_RENDER_PASS) {
		/* No argument. */
		refused = 0;
	} else if (opcode == GPU_OP_CMD_COPY_BUFFER || opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE || opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER || opcode == GPU_OP_CMD_COPY_IMAGE ||
		   opcode == GPU_OP_CMD_BLIT_IMAGE || opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
		refused = powervr_vk_record_transfer(session, opcode, reader, &entry);
	} else if (opcode != GPU_OP_CMD_PIPELINE_BARRIER) {
		refused = powervr_vk_record_draw(session, opcode, reader, &entry);
	} else {
		refused = record_barrier(session, reader);
	}

	if (refused == EINVAL || reader->error != 0) {
		record_drop(&entry);
		return EINVAL;
	}

	/* Only a command buffer that is recording keeps it. */
	command = powervr_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (command == NULL || command->state != POWERVR_VK_COMMAND_RECORDING) {
		record_drop(&entry);
		return 0;
	}

	/* A refused record fails the recording. */
	if (refused != 0) {
		kern_logf("powervr: vk: a recorded command %u is not run (%d)\n", (unsigned)opcode, refused);
		record_drop(&entry);
		if (command->error == 0)
			command->error = refused;
		return 0;
	}

	/* A barrier changes nothing a submission runs. */
	if (opcode == GPU_OP_CMD_PIPELINE_BARRIER)
		return 0;

	/* Appends it; one that cannot be kept fails the recording. */
	error = record_append(command, &entry);
	if (error != 0) {
		record_drop(&entry);
		if (command->error == 0)
			command->error = error;
	}

	/* Succeeded: the command is recorded, or its command buffer remembers why not. */
	return 0;
}

/* vkCmdBindPipeline: [bind point][pipeline]; a compute or graphics pipeline at its own bind point. */
static int
record_pipeline(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	uint64_t pipeline;
	uint32_t point;
	bool graphics;
	int error;

	/* The bind point and the pipeline. */
	point = drv_i915_wire_read_u32(reader);
	pipeline = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The compute and the graphics bind points. */
	if (point != VK_PIPELINE_BIND_POINT_COMPUTE && point != VK_PIPELINE_BIND_POINT_GRAPHICS)
		return ENOTSUP;

	/* The record holds a known pipeline. */
	error = record_hold(session, entry, I915_VK_OBJ_PIPELINE, pipeline);
	if (error != 0)
		return error;
	if (entry->object_count == 0)
		return ENOENT;

	/* A pipeline of the bind point's kind; a graphics bind is a record of its own. */
	graphics = ((const struct powervr_vk_pipeline *)entry->objects[0]->payload)->graphics != NULL;
	if (graphics != (point == VK_PIPELINE_BIND_POINT_GRAPHICS))
		return EINVAL;
	if (graphics)
		entry->opcode = POWERVR_VK_RECORD_BIND_GRAPHICS;

	/* Succeeded: the pipeline is recorded. */
	return 0;
}

/*
 * vkCmdBindDescriptorSets: [bind point][layout][first set][count][count]
 * {set}[dynamic count][count]{offset}; compute or graphics sets, held, without
 * dynamic offsets.
 */
static int
record_sets(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	uint64_t count;
	uint64_t index;
	uint64_t identity;
	uint64_t offsets;
	uint32_t point;
	int refused;
	int error;

	/* The bind point, the layout, the first set and how many sets follow. */
	point = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	entry->first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > COMMAND_BARRIERS_MAX)
		return EINVAL;

	/* Each set, held while the bind fits a pipeline layout. */
	refused = 0;
	if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		entry->opcode = POWERVR_VK_RECORD_SETS_GRAPHICS;
	} else if (point != VK_PIPELINE_BIND_POINT_COMPUTE) {
		refused = ENOTSUP;
	}
	if (entry->first >= POWERVR_VK_SETS_MAX || count > POWERVR_VK_SETS_MAX - entry->first)
		refused = ENOTSUP;
	for (index = 0; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (refused != 0)
			continue;

		/* Object i is set first + i; a null set holds nothing. */
		entry->object_count = (uint32_t)index;
		error = record_hold(session, entry, I915_VK_OBJ_DESCRIPTOR_SET, identity);
		if (error != 0)
			refused = error;
		entry->object_count = (uint32_t)index + 1U;
	}

	/* The dynamic offsets: none is taken. */
	(void)drv_i915_wire_read_u32(reader);
	offsets = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || offsets > COMMAND_BARRIERS_MAX)
		return EINVAL;
	for (index = 0; index < offsets; index++)
		(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;
	if (offsets != 0 && refused == 0)
		refused = ENOTSUP;

	/* A bind that cannot be run. */
	if (refused != 0)
		return refused;

	/* Succeeded: the sets are recorded. */
	return 0;
}

/* vkCmdDispatch: [x][y][z] workgroups. */
static int
record_dispatch(
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	/* The three counts. */
	entry->values[0] = drv_i915_wire_read_u32(reader);
	entry->values[1] = drv_i915_wire_read_u32(reader);
	entry->values[2] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* One row of workgroups. */
	if (entry->values[1] != 1 || entry->values[2] != 1)
		return ENOTSUP;

	/* Succeeded: the dispatch is recorded. */
	return 0;
}

/* vkCmdPipelineBarrier: the masks and the three barrier arrays, read and not kept. */
static int
record_barrier(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader)
{
	VkMemoryBarrier memory;
	VkBufferMemoryBarrier buffer;
	VkImageMemoryBarrier image;
	uint64_t count;
	uint64_t index;

	/* The stage masks and the dependency flags. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The memory barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > COMMAND_BARRIERS_MAX)
		return EINVAL;
	for (index = 0; index < count; index++)
		i915_vkc_dec_VkMemoryBarrier(reader, &session->arena, &memory);

	/* The buffer barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > COMMAND_BARRIERS_MAX)
		return EINVAL;
	for (index = 0; index < count; index++)
		i915_vkc_dec_VkBufferMemoryBarrier(reader, &session->arena, &buffer);

	/* The image barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > COMMAND_BARRIERS_MAX)
		return EINVAL;
	for (index = 0; index < count; index++)
		i915_vkc_dec_VkImageMemoryBarrier(reader, &session->arena, &image);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the barrier is read. */
	return 0;
}

/* Holds an object of a kind for a record; a null identity holds nothing, an unknown one is ENOENT. */
static int
record_hold(
	struct powervr_vk_session *session,
	struct powervr_vk_record *entry,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct powervr_vk_object *object;
	int error;

	/* A null handle names nothing. */
	if (identity == 0)
		return 0;

	/* A known object, while the record has room for it. */
	object = powervr_vk_object_find(session, kind, identity);
	if (object == NULL)
		return ENOENT;
	if (entry->object_count >= POWERVR_VK_SETS_MAX)
		return ENOTSUP;

	/* The record holds it. */
	error = powervr_vk_object_retain(object);
	if (error != 0)
		return error;
	entry->objects[entry->object_count] = object;
	entry->object_count++;

	/* Succeeded: the record holds the object. */
	return 0;
}

/* Appends a record to a command buffer, growing its array by doubling. */
static int
record_append(
	struct powervr_vk_command_buffer *command,
	const struct powervr_vk_record *entry)
{
	struct powervr_vk_record *grown;
	uint32_t capacity;

	/* Grows the array when it is full, up to the most records. */
	if (command->record_count == command->record_capacity) {
		capacity = COMMAND_RECORDS_FIRST;
		if (command->record_capacity != 0)
			capacity = command->record_capacity * 2U;
		if (capacity > COMMAND_RECORDS_MAX)
			return ENOMEM;
		grown = kern_calloc(capacity, sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		if (command->record_count != 0)
			kern_memcpy(grown, command->records, command->record_count * sizeof(*grown));
		kern_free(command->records);
		command->records = grown;
		command->record_capacity = capacity;
	}

	/* The record, whose held objects the array now owns. */
	command->records[command->record_count] = *entry;
	command->record_count++;

	/* Succeeded: the record is kept. */
	return 0;
}

/* Lets go of the objects a record holds. */
static void
record_drop(
	struct powervr_vk_record *entry)
{
	uint32_t index;

	/* Each held object; a null slot holds nothing. */
	for (index = 0; index < entry->object_count; index++) {
		(void)powervr_vk_object_release(entry->objects[index]);
		entry->objects[index] = NULL;
	}
	entry->object_count = 0;

	/* The record's own data. */
	kern_free(entry->data);
	entry->data = NULL;
}

/* Destroys a command buffer nothing holds: its records let their objects go. */
static int
command_destroy(
	struct powervr_vk_session *session,
	void *payload)
{
	struct powervr_vk_command_buffer *command;

	/* The records, the array and the command buffer. */
	(void)session;
	command = payload;
	powervr_vk_command_reset(command);
	kern_free(command->records);
	kern_free(command);

	/* Succeeded: the command buffer is gone. */
	return 0;
}
