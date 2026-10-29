/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's command pools and command buffers: their allocation, and the
 * begin, end and reset of a recording.  The recorded commands themselves are
 * vk-record.c's.
 *
 * A command buffer keeps its records until it is begun again, reset, or
 * freed; each record holds the objects it names, so they outlive their
 * destruction by the client while the command buffer may still run them.
 * A submission runs the records at once and waits for the GPU (vk-queue.c),
 * so no command buffer is ever pending when the client changes it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The most command buffers one allocation or free names. */
#define COMMAND_BATCH_MAX		64U

static int command_create_pool(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int command_destroy_pool(struct nvrtx_vk_session *session, struct i915_wire_reader *reader);
static int command_reset_pool(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int command_pool_free(struct nvrtx_vk_session *session, const void *pool);
static int command_allocate(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int command_free(struct nvrtx_vk_session *session, struct i915_wire_reader *reader);
static int command_begin(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int command_end(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int command_reset(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int command_destroy(struct nvrtx_vk_session *session, void *payload);

/*
 * Runs a command pool or command buffer command; *handled stays clear for
 * an opcode of another handler's.
 */
int
nvrtx_vk_command_route(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	UNUSED_PARAMETER(requested);

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_COMMAND_POOL:
		error = command_create_pool(session, reader, reply);
		break;
	case GPU_OP_DESTROY_COMMAND_POOL:
		error = command_destroy_pool(session, reader);
		break;
	case GPU_OP_RESET_COMMAND_POOL:
		error = command_reset_pool(session, reader, reply);
		break;
	case GPU_OP_ALLOCATE_COMMAND_BUFFERS:
		error = command_allocate(session, reader, reply);
		break;
	case GPU_OP_FREE_COMMAND_BUFFERS:
		error = command_free(session, reader);
		break;
	case GPU_OP_BEGIN_COMMAND_BUFFER:
		error = command_begin(session, reader, reply);
		break;
	case GPU_OP_END_COMMAND_BUFFER:
		error = command_end(session, reader, reply);
		break;
	case GPU_OP_RESET_COMMAND_BUFFER:
		error = command_reset(session, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Reports a command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/*
 * Empties a command buffer's recording: every record lets its objects
 * go and its own runs are freed.  The command buffer is back in its
 * initial state.
 */
void
nvrtx_vk_command_reset(
	struct nvrtx_vk_command_buffer *command)
{
	struct nvrtx_vk_record *record;
	uint32_t index;
	uint32_t object;
	bool video;

	/* Each record's objects and runs (a video or query command's structure with what it holds). */
	for (index = 0U; index < command->record_count; index++) {
		record = &command->records[index];
		for (object = 0U; object < record->object_count; object++)
			(void)nvrtx_vk_object_release(record->objects[object]);

		/* A video or query command's structure goes with what it holds. */
		video = nvrtx_vk_video_recorded(record->opcode);
		if (video)
			nvrtx_vk_video_drop(record);

		/* The regions, when the record kept them. */
		if (record->regions != NULL)
			kern_free(record->regions);
	}

	/* The arrays stay for the next recording; the state and the error start over. */
	command->record_count = 0U;
	command->push_used = 0U;
	command->state = NVRTX_VK_COMMAND_INITIAL;
	command->error = 0;
}

/*
 * vkCreateCommandPool: [device][present][VkCommandPoolCreateInfo] and the
 * create's tail -> the create's reply.
 */
static int
command_create_pool(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandPoolCreateInfo info;
	struct nvrtx_vk_command_pool *pool;
	uint64_t identity;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkCommandPoolCreateInfo(reader, &session->arena, &info);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A pool of the one queue family. */
	error = 0;
	if (info.queueFamilyIndex != 0U)
		error = EINVAL;

	/* The pool. */
	pool = NULL;
	if (error == 0) {
		pool = kern_calloc(1U, sizeof(*pool));
		if (pool != NULL)
			pool->family = info.queueFamilyIndex;
	}

	/* Publishes the pool and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_COMMAND_POOL, identity, pool, nvrtx_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkDestroyCommandPool: [device][identity][allocator], no reply body.  The
 * pool's command buffers go with it.
 */
static int
command_destroy_pool(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader)
{
	const struct nvrtx_vk_command_pool *pool;
	uint64_t identity;
	int error;

	/* The identity between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An unknown pool is already gone. */
	pool = nvrtx_vk_payload(session, I915_VK_OBJ_COMMAND_POOL, identity);
	if (pool == NULL)
		return 0;

	/* The pool's command buffers, then the pool. */
	error = command_pool_free(session, pool);
	if (error == 0)
		error = nvrtx_vk_object_remove(session, I915_VK_OBJ_COMMAND_POOL, identity);
	if (error != 0)
		return error;

	/* Succeeded: the pool and its command buffers are gone. */
	return 0;
}

/*
 * vkResetCommandPool: [device][identity][flags] -> [result].  Every
 * command buffer of the pool is back in its initial state.
 */
static int
command_reset_pool(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const struct nvrtx_vk_command_pool *pool;
	struct nvrtx_vk_object *object;
	struct nvrtx_vk_command_buffer *command;
	uint64_t identity;

	/* The identity between the device and the flags. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Every command buffer of a known pool is emptied. */
	pool = nvrtx_vk_payload(session, I915_VK_OBJ_COMMAND_POOL, identity);
	for (object = session->objects; pool != NULL && object != NULL; object = object->next) {
		if (object->kind != I915_VK_OBJ_COMMAND_BUFFER)
			continue;
		command = object->payload;
		if (command->pool == pool)
			nvrtx_vk_command_reset(command);
	}

	/* Success. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/*
 * Removes every published command buffer of a pool.  The list changes
 * with each removal, so the walk starts over after one.
 */
static int
command_pool_free(
	struct nvrtx_vk_session *session,
	const void *pool)
{
	struct nvrtx_vk_object *object;
	const struct nvrtx_vk_command_buffer *command;
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
			error = nvrtx_vk_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, object->identity);
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
command_allocate(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandBufferAllocateInfo info;
	uint64_t identities[COMMAND_BATCH_MAX];
	const struct nvrtx_vk_command_pool *pool;
	struct nvrtx_vk_command_buffer *command;
	struct nvrtx_vk_object *object;
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
	if (reader->error != 0)
		return EINVAL;
	if (count > COMMAND_BATCH_MAX || count != info.commandBufferCount)
		return EINVAL;
	for (index = 0U; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Primary buffers of a known pool. */
	error = 0;
	pool = nvrtx_vk_payload(session, I915_VK_OBJ_COMMAND_POOL, (uint64_t)(uintptr_t)info.commandPool);
	if (pool == NULL) {
		error = EINVAL;
	} else if (info.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
		error = ENOTSUP;
	}

	/* Publishes each, up to the first failure. */
	published = 0U;
	for (index = 0U; error == 0 && index < count; index++) {
		command = kern_calloc(1U, sizeof(*command));
		if (command == NULL) {
			error = ENOMEM;
			break;
		}

		command->pool = pool;
		error = nvrtx_vk_object_publish(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index], command, command_destroy, &object);
		if (error != 0) {
			kern_free(command);
			break;
		}

		published++;
	}

	/* A batch that failed part-way leaves none. */
	if (error != 0) {
		for (index = 0U; index < published; index++)
			(void)nvrtx_vk_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index]);
	}

	/* The result, and on success the count and every identity. */
	result = nvrtx_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);
	if (error == 0) {
		drv_i915_wire_reply_u64(reply, count);
		for (index = 0U; index < count; index++)
			drv_i915_wire_reply_u64(reply, identities[index]);
	}

	/* Succeeded: the reply carries the allocation's result. */
	return 0;
}

/*
 * vkFreeCommandBuffers: [device][pool][count][count]{identity}, no reply
 * body.  An unknown identity (or a null one) is already gone.
 */
static int
command_free(
	struct nvrtx_vk_session *session,
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
	if (reader->error != 0)
		return EINVAL;
	if (count > COMMAND_BATCH_MAX)
		return EINVAL;

	/* Removes each. */
	for (index = 0U; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;
		if (identity == 0U)
			continue;

		error = nvrtx_vk_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
		if (error != 0 && error != ENOENT)
			return error;
	}

	/* Succeeded: the command buffers are gone. */
	return 0;
}

/*
 * vkBeginCommandBuffer: [command buffer][present][VkCommandBufferBeginInfo]
 * -> [result].  The earlier recording is emptied and a new one starts.
 */
static int
command_begin(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandBufferBeginInfo info;
	struct nvrtx_vk_command_buffer *command;
	uint64_t identity;
	uint64_t present;
	uint32_t result;
	int error;

	/* The command buffer and its begin info. */
	kern_memset(&info, 0, sizeof(info));
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkCommandBufferBeginInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return EINVAL;

	/* A known command buffer starts recording afresh. */
	error = EINVAL;
	command = nvrtx_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (command != NULL) {
		nvrtx_vk_command_reset(command);
		command->state = NVRTX_VK_COMMAND_RECORDING;
		error = 0;
	}

	/* The begin's result. */
	result = nvrtx_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/*
 * vkEndCommandBuffer: [command buffer] -> [result].  A recording that met
 * an error ends invalid and reports it; otherwise it is executable.
 */
static int
command_end(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct nvrtx_vk_command_buffer *command;
	uint64_t identity;
	uint32_t result;
	int error;

	/* The command buffer. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A recording command buffer ends executable, or invalid with its first error. */
	error = EINVAL;
	command = nvrtx_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (command != NULL && command->state == NVRTX_VK_COMMAND_RECORDING) {
		error = command->error;
		command->state = NVRTX_VK_COMMAND_EXECUTABLE;
		if (error != 0)
			command->state = NVRTX_VK_COMMAND_INVALID;
	}

	/* The end's result. */
	result = nvrtx_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/* vkResetCommandBuffer: [command buffer][flags] -> [result]. */
static int
command_reset(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct nvrtx_vk_command_buffer *command;
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
	command = nvrtx_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (command != NULL) {
		nvrtx_vk_command_reset(command);
		error = 0;
	}

	/* The reset's result. */
	result = nvrtx_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/* Destroys a command buffer nothing holds: its records let their objects go. */
static int
command_destroy(
	struct nvrtx_vk_session *session,
	void *payload)
{
	struct nvrtx_vk_command_buffer *command;

	UNUSED_PARAMETER(session);

	command = payload;

	/* The records, the arrays and the command buffer. */
	nvrtx_vk_command_reset(command);
	if (command->records != NULL)
		kern_free(command->records);
	if (command->push_bytes != NULL)
		kern_free(command->push_bytes);
	kern_free(command);

	/* Succeeded: the command buffer is gone. */
	return 0;
}
