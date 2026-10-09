/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Primary command state transitions never erase records still owned by a pending native submission. */
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-record.h"

static int begin_buffer(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int finish_buffer(struct bcm2711_vulkan_session *session, bool reset, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

/*
 * Routes primary recording begin, end and individually permitted reset operations.
 */
int
bcm2711_vulkan_command_buffer_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Generated reply-free recording calls remain the separate graphics recorder's responsibility. */
	*handled = 1;
	if (opcode != GPU_OP_BEGIN_COMMAND_BUFFER &&
	    opcode != GPU_OP_END_COMMAND_BUFFER &&
	    opcode != GPU_OP_RESET_COMMAND_BUFFER) {
		*handled = 0;
		return 0;
	}

	/* Every lifecycle operation has an explicit Vulkan result, including the deferred stream's final End. */
	if (requested != 1)
		return EINVAL;
	if (opcode == GPU_OP_BEGIN_COMMAND_BUFFER)
		error = begin_buffer(session, reader, reply);
	else if (opcode == GPU_OP_RESET_COMMAND_BUFFER)
		error = finish_buffer(session, true, reader, reply);
	else
		error = finish_buffer(session, false, reader, reply);
	if (error != 0)
		return error;

	/* Succeeded: the exact primary command lifecycle result was acknowledged. */
	return 0;
}

/*
 * Clears owned recording nodes only when no native submission can still read them.
 */
int
bcm2711_vulkan_command_clear(
	struct bcm2711_vulkan_command_buffer *command)
{
	struct bcm2711_vulkan_command_node *node;
	struct bcm2711_vulkan_command_node *next;
	int error;
	int retired;

	/* Pending counts protect immutable recording storage through actual native completion and disposal. */
	if (command->pending != 0)
		return EBUSY;
	node = command->first;
	command->first = NULL;
	command->last = NULL;
	command->state = BCM2711_VULKAN_COMMAND_INVALID;
	error = 0;

	/* Every node releases its own graph; a cleanup error must not strand later independently owned nodes. */
	while (node != NULL) {
		next = node->next;
		retired = node->release(node);
		if (retired != 0 && error == 0)
			error = retired;
		node = next;
	}

	/* Native retirement failure leaves an invalid recording and remains visible to its lifecycle caller. */
	command->flags = 0;
	command->recorded_bytes = 0;
	command->recording_error = error;
	command->render_open = false;
	if (error != 0)
		return error;
	command->state = BCM2711_VULKAN_COMMAND_INITIAL;

	/* Succeeded: no node or typed input edge belongs to the reset primary recording. */
	return 0;
}

/*
 * Releases one command buffer's recording, borrowed pool membership and independently acquired parent edges.
 */
int
bcm2711_vulkan_command_release(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_command_buffer *command;
	int error;
	int retired;

	/* Every pending submission retains its command object, making final destruction during execution impossible. */
	(void)session;
	command = payload;
	if (command->pending != 0)
		__builtin_trap();
	error = bcm2711_vulkan_command_clear(command);

	/* Partial construction has no list membership; complete children unlink before their last pool edge can destroy the pool. */
	if (command->previous != NULL) {
		*command->previous = command->next;
		if (command->next != NULL)
			command->next->previous = command->previous;
	}

	/* Both independent edges retire even if a recorded resource needs native quarantine recovery. */
	retired = bcm2711_vulkan_object_release(command->owner.parent);
	if (retired != 0 && error == 0)
		error = retired;
	retired = bcm2711_vulkan_object_release(command->owner.device);
	if (retired != 0 && error == 0)
		error = retired;
	kern_free(command);
	if (error != 0)
		return error;

	/* Succeeded: this command buffer owns no record storage, child membership or logical parent. */
	return 0;
}

/* Begins primary recording after complete framing and pool reset authority are verified. */
static int
begin_buffer(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_command_pool *pool;
	uint64_t identity;
	uint64_t present;
	uint64_t chain;
	uint64_t inheritance;
	uint32_t type;
	uint32_t flags;
	int error;

	/* Primary begin has no inherited render/query state and supports only finite ordinary usage flags. */
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	inheritance = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    present != 1 ||
	    type != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO ||
	    chain != 0 ||
	    inheritance != 0 ||
	    (flags & ~(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)) != 0)
		return ENOTSUP;

	/* Primary one-time and simultaneous usage are mutually exclusive. */
	if ((flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != 0 && (flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) != 0)
		return EINVAL;

	/* Resolve the exact primary recording identity before inspecting its lifecycle. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (object == NULL)
		return EINVAL;
	command = object->payload;
	pool = command->owner.parent->payload;

	/* Re-recording is an implicit individual reset, allowed only outside pending/recording state and by the pool flag. */
	if (command->pending != 0)
		return EBUSY;

	/* An active recording cannot be started a second time. */
	if (command->state == BCM2711_VULKAN_COMMAND_RECORDING)
		return EINVAL;

	/* Implicit re-recording needs the pool's individual-reset permission. */
	if (command->state != BCM2711_VULKAN_COMMAND_INITIAL && (pool->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == 0)
		return EINVAL;

	/* Release all old node ownership before starting this new empty recording. */
	error = bcm2711_vulkan_command_clear(command);
	if (error != 0)
		return error;

	/* RECORDING permits reply-free commands; only complete successful End makes this exact graph executable. */
	command->flags = flags;
	command->state = BCM2711_VULKAN_COMMAND_RECORDING;
	drv_i915_wire_reply_u32(reply, VK_SUCCESS);

	/* Succeeded: subsequent native records append to an empty primary recording. */
	return 0;
}

/* Completes a valid recording or resets one command while preserving exact pending ownership and first recording failure. */
static int
finish_buffer(
	struct bcm2711_vulkan_session *session,
	bool reset,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_command_pool *pool;
	uint64_t identity;
	uint32_t flags;
	int error;

	/* End has only its command identity; reset additionally carries the standard release-resources flag word. */
	identity = drv_i915_wire_read_u64(reader);
	if (reset) {
		flags = drv_i915_wire_read_u32(reader);
		if ((flags & ~VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT) != 0)
			return ENOTSUP;
	}

	/* No malformed or foreign command can change a different recording. */
	if (reader->error != 0)
		return EINVAL;

	/* Resolve the exact primary recording identity before inspecting its lifecycle. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (object == NULL)
		return EINVAL;
	command = object->payload;
	pool = command->owner.parent->payload;
	if (command->pending != 0)
		return EBUSY;

	/* Explicit individual reset needs pool permission, including for an initial or invalid command buffer. */
	if (reset) {
		if ((pool->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == 0)
			return EINVAL;
		error = bcm2711_vulkan_command_clear(command);
		if (error != 0)
			return error;
		drv_i915_wire_reply_u32(reply, VK_SUCCESS);
		return 0;
	}

	/* An unfinished render pass or an earlier recording refusal cannot create an executable partial command graph. */
	if (command->state != BCM2711_VULKAN_COMMAND_RECORDING)
		return EINVAL;

	/* A later ordinary descriptor update invalidates an earlier recorded bind before it can become executable. */
	error = bcm2711_vulkan_record_current(command);
	if (error != 0 && command->recording_error == 0)
		command->recording_error = error;

	/* Preserve the first recording refusal instead of promoting a partial or invalidated graph. */
	if (command->recording_error != 0) {
		command->state = BCM2711_VULKAN_COMMAND_INVALID;
		if (command->recording_error == ENOMEM)
			drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		else
			drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT);
		return 0;
	}

	/* An otherwise valid recording still needs its final inline pass closed. */
	if (command->render_open)
		return EINVAL;

	/* EXECUTABLE records are immutable for every future pending owner until a permitted reset. */
	command->state = BCM2711_VULKAN_COMMAND_EXECUTABLE;
	drv_i915_wire_reply_u32(reply, VK_SUCCESS);

	/* Succeeded: this complete primary command buffer can be validated for native submission. */
	return 0;
}
