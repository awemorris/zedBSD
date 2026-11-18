/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Command pools, command buffers and the recorded commands of the Vulkan
 * executor.
 *
 * A recorded command (vkCmd*, numbers 93 to 136) is decoded with the
 * record codec libvulkan's encoder is mirrored by, so that its end is
 * known and a command of another shape is refused, and is kept in its
 * command buffer as it came on the wire.  A submission executes the kept
 * commands; one that cannot be kept (a recording past its bound) marks
 * the buffer failed, which the end of the recording reports.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The bytes of a command's header ([u32 number][u32 flags]) before its arguments. */
#define HEADER_BYTES			8U

/* The first room a command buffer's recording takes. */
#define RECORD_FIRST_BYTES		1024U

/* The words of a clear colour on the wire ([u32 2][u64 4][4 x u32]), and the tags of a clear value. */
#define CLEAR_COLOR_TAG			2U
#define CLEAR_COLOR_WORDS		4U
#define CLEAR_VALUE_COLOR		0U
#define CLEAR_VALUE_DEPTH		1U

static int create_pool(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_pool(struct ghci_vk_session *session, struct i915_wire_reader *reader);
static int reset_pool(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void pool_buffers(struct ghci_vk_session *session, uint64_t pool, int destroy);
static int allocate_buffers(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int free_buffers(struct ghci_vk_session *session, struct i915_wire_reader *reader);
static int begin_buffer(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int end_buffer(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int reset_buffer(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void buffer_reset(struct ghci_vk_command_buffer *commands);
static int record(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader);
static int record_arguments(struct ghci_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader);
static void record_keep(struct ghci_vk_session *session, struct ghci_vk_object *object, const uint8_t *bytes, size_t length);
static void decode_barriers(struct ghci_vk_session *session, struct i915_wire_reader *reader);
static void decode_transfer(struct ghci_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader);
static void decode_clear(struct ghci_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader);
static void decode_render_begin(struct i915_wire_reader *reader);
static void decode_color(struct i915_wire_reader *reader);
static void decode_clear_value(struct i915_wire_reader *reader);

/*
 * Runs a command pool, command buffer or recorded command; *handled
 * stays clear for another group's number.
 */
int
ghci_vk_command_route(
	struct ghci_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* The recorded commands of Vulkan 1.0. */
	*handled = 1;
	if (opcode >= GPU_OP_CMD_BIND_PIPELINE && opcode <= GPU_OP_CMD_EXECUTE_COMMANDS) {
		error = record(session, opcode, requested, reader);
		if (error != 0)
			return error;
		return 0;
	}

	/* The pools and the buffers, or another group's command. */
	switch (opcode) {
	case GPU_OP_CREATE_COMMAND_POOL:
		error = create_pool(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_COMMAND_POOL:
		error = destroy_pool(session, reader);
		break;
	case GPU_OP_RESET_COMMAND_POOL:
		error = reset_pool(session, requested, reader, reply);
		break;
	case GPU_OP_ALLOCATE_COMMAND_BUFFERS:
		error = allocate_buffers(session, requested, reader, reply);
		break;
	case GPU_OP_FREE_COMMAND_BUFFERS:
		error = free_buffers(session, reader);
		break;
	case GPU_OP_BEGIN_COMMAND_BUFFER:
		error = begin_buffer(session, requested, reader, reply);
		break;
	case GPU_OP_END_COMMAND_BUFFER:
		error = end_buffer(session, requested, reader, reply);
		break;
	case GPU_OP_RESET_COMMAND_BUFFER:
		error = reset_buffer(session, requested, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded or run. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/* Lets a command buffer's recording go. */
void
ghci_vk_commands_release(
	struct ghci_vk_object *object)
{
	/* The recorded bytes. */
	kern_free(object->as.commands.bytes);
	object->as.commands.bytes = NULL;
	object->as.commands.capacity = 0U;
	object->as.commands.used = 0U;
}

/* vkCreateCommandPool: the create form with VkCommandPoolCreateInfo, for the one queue family. */
static int
create_pool(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandPoolCreateInfo info;
	struct ghci_vk_object *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t pool;
	uint32_t result;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkCommandPoolCreateInfo(reader, &session->arena, &info);
	pool = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = ghci_vk_object_find(session, GHCI_VK_DEVICE, handle);
	if (device == NULL)
		return EINVAL;

	/* The one family. */
	result = (uint32_t)VK_SUCCESS;
	if (info.queueFamilyIndex != 0U)
		result = (uint32_t)VK_ERROR_INITIALIZATION_FAILED;

	/* The pool's object. */
	if (result == (uint32_t)VK_SUCCESS) {
		error = ghci_vk_object_add(session, GHCI_VK_COMMAND_POOL, pool, device->device, &object);
		if (error != 0)
			return error;
		object->as.pool.flags = info.flags;
		object->as.pool.family = info.queueFamilyIndex;
	}

	/* The result, and the identity when it succeeded. */
	ghci_vk_reply_created(reply, requested, result, pool);

	/* Succeeded: the pool exists, or the reply says why not. */
	return 0;
}

/* vkDestroyCommandPool: the destroy form; the pool's command buffers go with it. */
static int
destroy_pool(
	struct ghci_vk_session *session,
	struct i915_wire_reader *reader)
{
	struct ghci_vk_object *object;
	uint64_t identity;
	uint64_t device;

	/* The destroy form. */
	identity = ghci_vk_destroy_form(reader, &device);
	if (reader->error != 0)
		return EINVAL;

	/* The pool's buffers, then the pool. */
	object = ghci_vk_object_find(session, GHCI_VK_COMMAND_POOL, identity);
	if (object == NULL)
		return 0;
	pool_buffers(session, identity, 1);
	ghci_vk_object_destroy(session, object);

	/* Succeeded: the pool is gone. */
	return 0;
}

/* vkResetCommandPool: [device][pool][u32 flags] -> [result]; every buffer of the pool is new again. */
static int
reset_pool(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *object;
	uint64_t identity;

	/* The device, the pool and the flags. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A pool of the session. */
	object = ghci_vk_object_find(session, GHCI_VK_COMMAND_POOL, identity);
	if (object == NULL)
		return EINVAL;

	/* Its buffers. */
	pool_buffers(session, identity, 0);
	ghci_vk_reply_result(reply, requested, (uint32_t)VK_SUCCESS);

	/* Succeeded: the pool's buffers are reset. */
	return 0;
}

/* Resets (destroy 0) or destroys (destroy 1) every command buffer of a pool. */
static void
pool_buffers(
	struct ghci_vk_session *session,
	uint64_t pool,
	int destroy)
{
	struct ghci_vk_object **position;
	struct ghci_vk_object *object;
	uint32_t bucket;

	/* Each bucket, walked again from where a buffer left it. */
	for (bucket = 0U; bucket < GHCI_VK_BUCKETS; bucket++) {
		position = &session->buckets[bucket];
		while (*position != NULL) {
			/* A command buffer of the pool. */
			object = *position;
			if (object->kind != GHCI_VK_COMMAND_BUFFER || object->as.commands.pool != pool) {
				position = &object->next;
				continue;
			}

			/* Destroyed (it leaves the bucket), or reset. */
			if (destroy) {
				ghci_vk_object_destroy(session, object);
				continue;
			}
			buffer_reset(&object->as.commands);
			position = &object->next;
		}
	}
}

/*
 * vkAllocateCommandBuffers: [device][u64 1][VkCommandBufferAllocateInfo]
 * [u64 count][count x identity] -> [result][u64 count][count x identity].
 * Either every buffer is made or none: one that cannot be takes the ones
 * before it back.
 */
static int
allocate_buffers(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandBufferAllocateInfo info;
	struct ghci_vk_object *device;
	struct ghci_vk_object *pool;
	struct ghci_vk_object *object;
	const uint8_t *identities;
	uint64_t handle;
	uint64_t present;
	uint64_t count;
	uint64_t index;
	uint64_t made;
	uint64_t identity;
	int error;

	/* The device, the allocate info and the identities libvulkan reserved. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkCommandBufferAllocateInfo(reader, &session->arena, &info);
	count = ghci_vk_read_count(reader);
	identities = drv_i915_wire_read_array(reader, (size_t)count, 8U);
	if (reader->error != 0 || present != 1U || count != info.commandBufferCount)
		return EINVAL;

	/* A device of the session, and a pool of that device. */
	device = ghci_vk_object_find(session, GHCI_VK_DEVICE, handle);
	if (device == NULL)
		return EINVAL;
	pool = ghci_vk_object_find(session, GHCI_VK_COMMAND_POOL, (uint64_t)(uintptr_t)info.commandPool);
	if (pool == NULL || pool->device != device->device)
		return EINVAL;

	/* Each buffer's object, until one cannot be made. */
	error = 0;
	for (made = 0U; made < count; made++) {
		kern_memcpy(&identity, identities + made * 8U, 8U);
		error = ghci_vk_object_add(session, GHCI_VK_COMMAND_BUFFER, identity, pool->device, &object);
		if (error != 0)
			break;
		object->as.commands.pool = pool->identity;
		object->as.commands.level = info.level;
		object->as.commands.state = GHCI_VK_RECORD_INITIAL;
	}

	/* A buffer that could not be made takes the ones made before it back. */
	if (error != 0) {
		for (index = 0U; index < made; index++) {
			kern_memcpy(&identity, identities + index * 8U, 8U);
			object = ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, identity);
			if (object != NULL)
				ghci_vk_object_destroy(session, object);
		}
		return error;
	}

	/* The result, the count and the identities. */
	if (requested != 0U) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
		drv_i915_wire_reply_u64(reply, count);
		for (index = 0U; index < count; index++) {
			kern_memcpy(&identity, identities + index * 8U, 8U);
			drv_i915_wire_reply_u64(reply, identity);
		}
	}

	/* Succeeded: the buffers exist. */
	return 0;
}

/* vkFreeCommandBuffers: [device][pool][u32 count][u64 count][ids], no reply body. */
static int
free_buffers(
	struct ghci_vk_session *session,
	struct i915_wire_reader *reader)
{
	struct ghci_vk_object *object;
	const uint8_t *identities;
	uint64_t count;
	uint64_t index;
	uint64_t identity;

	/* The device, the pool and the identities. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = ghci_vk_read_count(reader);
	identities = drv_i915_wire_read_array(reader, (size_t)count, 8U);
	if (reader->error != 0)
		return EINVAL;

	/* Each buffer the session has. */
	for (index = 0U; index < count; index++) {
		kern_memcpy(&identity, identities + index * 8U, 8U);
		object = ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, identity);
		if (object != NULL)
			ghci_vk_object_destroy(session, object);
	}

	/* Succeeded: the buffers are gone. */
	return 0;
}

/* vkBeginCommandBuffer: [buffer][u64 1][VkCommandBufferBeginInfo] -> [result]; a new recording. */
static int
begin_buffer(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandBufferBeginInfo info;
	struct ghci_vk_object *object;
	uint64_t identity;
	uint64_t present;

	/* The buffer and the begin info. */
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkCommandBufferBeginInfo(reader, &session->arena, &info);
	if (reader->error != 0 || present != 1U)
		return EINVAL;

	/* A buffer of the session; what it held is let go. */
	object = ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, identity);
	if (object == NULL)
		return EINVAL;
	buffer_reset(&object->as.commands);
	object->as.commands.state = GHCI_VK_RECORD_RECORDING;
	ghci_vk_reply_result(reply, requested, (uint32_t)VK_SUCCESS);

	/* Succeeded: the buffer records. */
	return 0;
}

/*
 * vkEndCommandBuffer: [buffer] -> [result]: VK_SUCCESS, or
 * VK_ERROR_OUT_OF_HOST_MEMORY for a recording that could not be kept.
 */
static int
end_buffer(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *object;
	uint64_t identity;
	uint32_t result;

	/* The buffer. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A recording buffer of the session. */
	object = ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, identity);
	if (object == NULL || object->as.commands.state != GHCI_VK_RECORD_RECORDING)
		return EINVAL;

	/* The recording ends; a command that was not kept is the result. */
	object->as.commands.state = GHCI_VK_RECORD_EXECUTABLE;
	result = (uint32_t)VK_SUCCESS;
	if (object->as.commands.failed)
		result = (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
	ghci_vk_reply_result(reply, requested, result);

	/* Succeeded: the buffer may be submitted. */
	return 0;
}

/* vkResetCommandBuffer: [buffer][u32 flags] -> [result]; the buffer is new again. */
static int
reset_buffer(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *object;
	uint64_t identity;

	/* The buffer and the flags. */
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A buffer of the session. */
	object = ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, identity);
	if (object == NULL)
		return EINVAL;
	buffer_reset(&object->as.commands);
	ghci_vk_reply_result(reply, requested, (uint32_t)VK_SUCCESS);

	/* Succeeded: the buffer is reset. */
	return 0;
}

/* Empties a command buffer's recording (its room stays for the next one). */
static void
buffer_reset(
	struct ghci_vk_command_buffer *commands)
{
	/* No command, no failure, the initial state. */
	commands->used = 0U;
	commands->commands = 0U;
	commands->failed = 0;
	commands->state = GHCI_VK_RECORD_INITIAL;
}

/*
 * A recorded command: [buffer] and the Vulkan command's arguments, no
 * reply.  It is decoded to its end and kept as it came, header included,
 * in a buffer that records.
 */
static int
record(
	struct ghci_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader)
{
	struct ghci_vk_object *object;
	uint64_t identity;
	size_t start;
	int error;

	/* A recorded command has no reply; its header lies just before the reader. */
	if (requested != 0U || reader->offset < HEADER_BYTES)
		return EINVAL;
	start = reader->offset - HEADER_BYTES;

	/* The buffer, then the arguments. */
	identity = drv_i915_wire_read_u64(reader);
	error = record_arguments(session, opcode, reader);
	if (error != 0)
		return error;
	if (reader->error != 0)
		return EINVAL;

	/* A recording buffer of the session keeps the command. */
	object = ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, identity);
	if (object == NULL || object->as.commands.state != GHCI_VK_RECORD_RECORDING)
		return EINVAL;
	record_keep(session, object, reader->base + start, reader->offset - start);

	/* Succeeded: the command is recorded. */
	return 0;
}

/*
 * Decodes a recorded command's arguments as libvulkan encodes them
 * (uapi/gpu-op.h); the reader says whether they were whole.  Returns 0,
 * or ENOTSUP for a number of the range Vulkan 1.0 does not have.
 */
static int
record_arguments(
	struct ghci_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader)
{
	VkViewport viewport;
	VkRect2D rect;
	uint64_t count;
	uint64_t index;

	/* The arguments of each command. */
	switch (opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
		/* [bind point][pipeline]. */
		ghci_vk_skip(reader, 4U + 8U);
		break;
	case GPU_OP_CMD_SET_VIEWPORT:
		/* [first][count][u64 count][viewports]. */
		ghci_vk_skip(reader, 8U);
		count = ghci_vk_read_count(reader);
		for (index = 0U; index < count && reader->error == 0; index++)
			i915_vkc_dec_VkViewport(reader, &session->arena, &viewport);
		break;
	case GPU_OP_CMD_SET_SCISSOR:
		/* [first][count][u64 count][rectangles]. */
		ghci_vk_skip(reader, 8U);
		count = ghci_vk_read_count(reader);
		for (index = 0U; index < count && reader->error == 0; index++)
			i915_vkc_dec_VkRect2D(reader, &session->arena, &rect);
		break;
	case GPU_OP_CMD_SET_LINE_WIDTH:
		/* [float width]. */
		ghci_vk_skip(reader, 4U);
		break;
	case GPU_OP_CMD_SET_DEPTH_BIAS:
		/* [constant][clamp][slope]. */
		ghci_vk_skip(reader, 12U);
		break;
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
		/* [u64 4][4 x float]. */
		count = ghci_vk_read_count(reader);
		if (count != 4U)
			reader->error = 1;
		ghci_vk_skip(reader, count * 4U);
		break;
	case GPU_OP_CMD_SET_DEPTH_BOUNDS:
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
		/* Two words: the bounds, or the faces and their value. */
		ghci_vk_skip(reader, 8U);
		break;
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		/* [bind point][layout][first][count][u64 count][sets][dynamic count][u64 count][offsets]. */
		ghci_vk_skip(reader, 4U + 8U + 8U);
		count = ghci_vk_read_count(reader);
		ghci_vk_skip(reader, count * 8U);
		ghci_vk_skip(reader, 4U);
		count = ghci_vk_read_count(reader);
		ghci_vk_skip(reader, count * 4U);
		break;
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
		/* [buffer][u64 offset][index type]. */
		ghci_vk_skip(reader, 16U + 4U);
		break;
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
		/* [first][count][u64 count][buffers][u64 count][offsets]. */
		ghci_vk_skip(reader, 8U);
		count = ghci_vk_read_count(reader);
		ghci_vk_skip(reader, count * 8U);
		count = ghci_vk_read_count(reader);
		ghci_vk_skip(reader, count * 8U);
		break;
	case GPU_OP_CMD_DRAW:
		/* [vertices][instances][first vertex][first instance]. */
		ghci_vk_skip(reader, 16U);
		break;
	case GPU_OP_CMD_DRAW_INDEXED:
		/* [indices][instances][first index][vertex offset][first instance]. */
		ghci_vk_skip(reader, 20U);
		break;
	case GPU_OP_CMD_DRAW_INDIRECT:
	case GPU_OP_CMD_DRAW_INDEXED_INDIRECT:
		/* [buffer][u64 offset][draw count][stride]. */
		ghci_vk_skip(reader, 16U + 8U);
		break;
	case GPU_OP_CMD_DISPATCH:
		/* [x][y][z]. */
		ghci_vk_skip(reader, 12U);
		break;
	case GPU_OP_CMD_DISPATCH_INDIRECT:
		/* [buffer][u64 offset]. */
		ghci_vk_skip(reader, 16U);
		break;
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_RESOLVE_IMAGE:
		decode_transfer(session, opcode, reader);
		break;
	case GPU_OP_CMD_UPDATE_BUFFER:
		/* [buffer][u64 offset][u64 size][u64 count][the bytes, padded to words]. */
		ghci_vk_skip(reader, 24U);
		count = ghci_vk_read_count(reader);
		ghci_vk_skip(reader, (count + 3U) & ~(uint64_t)3U);
		break;
	case GPU_OP_CMD_FILL_BUFFER:
		/* [buffer][u64 offset][u64 size][data]. */
		ghci_vk_skip(reader, 24U + 4U);
		break;
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
	case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
		decode_clear(session, opcode, reader);
		break;
	case GPU_OP_CMD_SET_EVENT:
	case GPU_OP_CMD_RESET_EVENT:
		/* [event][stage mask]. */
		ghci_vk_skip(reader, 8U + 4U);
		break;
	case GPU_OP_CMD_WAIT_EVENTS:
		/* [count][u64 count][events][source stages][destination stages], then the barriers. */
		ghci_vk_skip(reader, 4U);
		count = ghci_vk_read_count(reader);
		ghci_vk_skip(reader, count * 8U);
		ghci_vk_skip(reader, 8U);
		decode_barriers(session, reader);
		break;
	case GPU_OP_CMD_PIPELINE_BARRIER:
		/* [source stages][destination stages][dependency flags], then the barriers. */
		ghci_vk_skip(reader, 12U);
		decode_barriers(session, reader);
		break;
	case GPU_OP_CMD_BEGIN_QUERY:
		/* [pool][query][flags]. */
		ghci_vk_skip(reader, 8U + 8U);
		break;
	case GPU_OP_CMD_END_QUERY:
		/* [pool][query]. */
		ghci_vk_skip(reader, 8U + 4U);
		break;
	case GPU_OP_CMD_RESET_QUERY_POOL:
		/* [pool][first][count]. */
		ghci_vk_skip(reader, 8U + 8U);
		break;
	case GPU_OP_CMD_WRITE_TIMESTAMP:
		/* [stage][pool][query]. */
		ghci_vk_skip(reader, 4U + 8U + 4U);
		break;
	case GPU_OP_CMD_COPY_QUERY_POOL_RESULTS:
		/* [pool][first][count][buffer][u64 offset][u64 stride][flags]. */
		ghci_vk_skip(reader, 8U + 8U + 24U + 4U);
		break;
	case GPU_OP_CMD_PUSH_CONSTANTS:
		/* [layout][stages][offset][size][u64 count][the bytes, padded to words]. */
		ghci_vk_skip(reader, 8U + 12U);
		count = ghci_vk_read_count(reader);
		ghci_vk_skip(reader, (count + 3U) & ~(uint64_t)3U);
		break;
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
		/* [u64 present][the begin info][contents]. */
		count = drv_i915_wire_read_u64(reader);
		if (count != 0U)
			decode_render_begin(reader);
		ghci_vk_skip(reader, 4U);
		break;
	case GPU_OP_CMD_NEXT_SUBPASS:
		/* [contents]. */
		ghci_vk_skip(reader, 4U);
		break;
	case GPU_OP_CMD_END_RENDER_PASS:
		/* Nothing. */
		break;
	case GPU_OP_CMD_EXECUTE_COMMANDS:
		/* [count][u64 count][command buffers]. */
		ghci_vk_skip(reader, 4U);
		count = ghci_vk_read_count(reader);
		ghci_vk_skip(reader, count * 8U);
		break;
	default:
		/* A number of the range Vulkan 1.0 does not have. */
		return ENOTSUP;
	}

	/* Succeeded: the arguments are decoded (the reader says whether they were whole). */
	return 0;
}

/*
 * Appends one command to a command buffer's recording, doubling its room
 * as needed up to GHCI_VK_RECORD_MAX and within the session's budget (the
 * room is the buffer's object's charge); one that does not fit marks the
 * recording failed.
 */
static void
record_keep(
	struct ghci_vk_session *session,
	struct ghci_vk_object *object,
	const uint8_t *bytes,
	size_t length)
{
	struct ghci_vk_command_buffer *commands;
	uint8_t *grown;
	uint64_t capacity;
	int error;

	/* A recording that already failed keeps nothing more. */
	commands = &object->as.commands;
	if (commands->failed)
		return;

	/* The room the command needs, within the bound. */
	capacity = commands->capacity;
	if (capacity == 0U)
		capacity = RECORD_FIRST_BYTES;
	while (capacity < (uint64_t)commands->used + length)
		capacity <<= 1;
	if (capacity > GHCI_VK_RECORD_MAX) {
		commands->failed = 1;
		return;
	}

	/* A larger room, with what was recorded, its growth taken of the budget. */
	if (capacity != commands->capacity) {
		error = ghci_vk_budget_take(session, capacity - commands->capacity);
		if (error != 0) {
			commands->failed = 1;
			return;
		}
		grown = kern_malloc((size_t)capacity);
		if (grown == NULL) {
			ghci_vk_budget_give(session, capacity - commands->capacity);
			commands->failed = 1;
			return;
		}
		if (commands->used != 0U)
			kern_memcpy(grown, commands->bytes, commands->used);
		kern_free(commands->bytes);
		object->charged += capacity - commands->capacity;
		commands->bytes = grown;
		commands->capacity = (uint32_t)capacity;
	}

	/* The command after the others. */
	kern_memcpy(commands->bytes + commands->used, bytes, length);
	commands->used += (uint32_t)length;
	commands->commands++;
}

/*
 * Decodes the barrier arrays of a pipeline barrier or an event wait:
 * memory, buffer and image barriers, each [u32 count][u64 count][barriers].
 */
static void
decode_barriers(
	struct ghci_vk_session *session,
	struct i915_wire_reader *reader)
{
	VkMemoryBarrier memory;
	VkBufferMemoryBarrier buffer;
	VkImageMemoryBarrier image;
	uint64_t count;
	uint64_t index;

	/* The memory barriers. */
	ghci_vk_skip(reader, 4U);
	count = ghci_vk_read_count(reader);
	for (index = 0U; index < count && reader->error == 0; index++)
		i915_vkc_dec_VkMemoryBarrier(reader, &session->arena, &memory);

	/* The buffer barriers. */
	ghci_vk_skip(reader, 4U);
	count = ghci_vk_read_count(reader);
	for (index = 0U; index < count && reader->error == 0; index++)
		i915_vkc_dec_VkBufferMemoryBarrier(reader, &session->arena, &buffer);

	/* The image barriers. */
	ghci_vk_skip(reader, 4U);
	count = ghci_vk_read_count(reader);
	for (index = 0U; index < count && reader->error == 0; index++)
		i915_vkc_dec_VkImageMemoryBarrier(reader, &session->arena, &image);
}

/*
 * Decodes a copy, a blit or a resolve: the source and the destination
 * (with their layouts where they are images), [count][u64 count] and the
 * regions, and a blit's filter.
 */
static void
decode_transfer(
	struct ghci_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader)
{
	VkBufferCopy buffer_copy;
	VkImageCopy image_copy;
	VkImageBlit blit;
	VkBufferImageCopy buffer_image;
	VkImageResolve resolve;
	uint64_t count;
	uint64_t index;

	/* The source and the destination: a buffer is its identity, an image its identity and layout. */
	switch (opcode) {
	case GPU_OP_CMD_COPY_BUFFER:
		ghci_vk_skip(reader, 16U);
		break;
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
		ghci_vk_skip(reader, 16U + 4U);
		break;
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
		ghci_vk_skip(reader, 8U + 4U + 8U);
		break;
	default:
		ghci_vk_skip(reader, 2U * (8U + 4U));
		break;
	}

	/* The regions of the command's kind. */
	ghci_vk_skip(reader, 4U);
	count = ghci_vk_read_count(reader);
	for (index = 0U; index < count && reader->error == 0; index++) {
		/* One region. */
		switch (opcode) {
		case GPU_OP_CMD_COPY_BUFFER:
			i915_vkc_dec_VkBufferCopy(reader, &session->arena, &buffer_copy);
			break;
		case GPU_OP_CMD_COPY_IMAGE:
			i915_vkc_dec_VkImageCopy(reader, &session->arena, &image_copy);
			break;
		case GPU_OP_CMD_BLIT_IMAGE:
			i915_vkc_dec_VkImageBlit(reader, &session->arena, &blit);
			break;
		case GPU_OP_CMD_RESOLVE_IMAGE:
			i915_vkc_dec_VkImageResolve(reader, &session->arena, &resolve);
			break;
		default:
			i915_vkc_dec_VkBufferImageCopy(reader, &session->arena, &buffer_image);
			break;
		}
	}

	/* A blit's filter. */
	if (opcode == GPU_OP_CMD_BLIT_IMAGE)
		ghci_vk_skip(reader, 4U);
}

/*
 * Decodes the clears: an image's colour ([image][layout][u64 present]
 * [colour][count][u64 count][ranges]) or depth and stencil ([image]
 * [layout][u64 present][float][u32][count][u64 count][ranges]), and the
 * attachments' ([count][u64 count][attachments][count][u64 count][rects]).
 */
static void
decode_clear(
	struct ghci_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader)
{
	VkImageSubresourceRange range;
	VkClearRect rect;
	uint64_t present;
	uint64_t count;
	uint64_t index;

	/* The attachments: each [aspect][attachment][the tagged value], then the rectangles. */
	if (opcode == GPU_OP_CMD_CLEAR_ATTACHMENTS) {
		ghci_vk_skip(reader, 4U);
		count = ghci_vk_read_count(reader);
		for (index = 0U; index < count && reader->error == 0; index++) {
			ghci_vk_skip(reader, 8U);
			decode_clear_value(reader);
		}

		/* The rectangles. */
		ghci_vk_skip(reader, 4U);
		count = ghci_vk_read_count(reader);
		for (index = 0U; index < count && reader->error == 0; index++)
			i915_vkc_dec_VkClearRect(reader, &session->arena, &rect);
		return;
	}

	/* The image, its layout and the value. */
	ghci_vk_skip(reader, 8U + 4U);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U && opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
		decode_color(reader);
	} else if (present != 0U) {
		ghci_vk_skip(reader, 8U);
	}

	/* The ranges. */
	ghci_vk_skip(reader, 4U);
	count = ghci_vk_read_count(reader);
	for (index = 0U; index < count && reader->error == 0; index++)
		i915_vkc_dec_VkImageSubresourceRange(reader, &session->arena, &range);
}

/*
 * Decodes a render pass's begin info: [sType][u64 0][render pass]
 * [framebuffer][rectangle][count][u64 count][the tagged clear values].
 */
static void
decode_render_begin(
	struct i915_wire_reader *reader)
{
	uint64_t chain;
	uint64_t count;
	uint64_t index;

	/* The head, the pass, the framebuffer and the area. */
	ghci_vk_skip(reader, 4U);
	chain = drv_i915_wire_read_u64(reader);
	if (chain != 0U)
		reader->error = 1;
	ghci_vk_skip(reader, 16U + 16U);

	/* The clear values. */
	ghci_vk_skip(reader, 4U);
	count = ghci_vk_read_count(reader);
	for (index = 0U; index < count && reader->error == 0; index++)
		decode_clear_value(reader);
}

/* Decodes a clear colour: [u32 2][u64 4][4 x u32]. */
static void
decode_color(
	struct i915_wire_reader *reader)
{
	uint32_t tag;
	uint64_t count;

	/* The union's tag and its four words. */
	tag = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (tag != CLEAR_COLOR_TAG || count != CLEAR_COLOR_WORDS)
		reader->error = 1;
	ghci_vk_skip(reader, CLEAR_COLOR_WORDS * 4U);
}

/* Decodes a tagged clear value: [u32 0][colour] or [u32 1][float depth][u32 stencil]. */
static void
decode_clear_value(
	struct i915_wire_reader *reader)
{
	uint32_t tag;

	/* The tag says which. */
	tag = drv_i915_wire_read_u32(reader);
	if (tag == CLEAR_VALUE_COLOR) {
		decode_color(reader);
	} else if (tag == CLEAR_VALUE_DEPTH) {
		ghci_vk_skip(reader, 8U);
	} else {
		reader->error = 1;
	}
}
