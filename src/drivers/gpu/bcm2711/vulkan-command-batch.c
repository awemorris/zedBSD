/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Primary command-buffer allocation is atomic across the complete client-reserved output vector. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-command.h"

/* Keiland allocates short primary batches; the session namespace independently limits all surviving typed owners. */
#define VULKAN_COMMAND_BATCH 64U

static int allocate_buffers(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int free_buffers(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader);
static int prepare_buffer(struct bcm2711_vulkan_object *device, struct bcm2711_vulkan_object *pool, struct bcm2711_vulkan_command_buffer **created);
static void allocation_reply(struct i915_wire_writer *reply, VkResult status, uint32_t count, const uint64_t *identities);

/*
 * Routes complete primary allocation batches and exact same-pool buffer identity retirement.
 */
int
bcm2711_vulkan_command_batch_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Pool lifecycle and primary record transitions are independent typed routes. */
	*handled = 1;
	if (opcode != GPU_OP_ALLOCATE_COMMAND_BUFFERS && opcode != GPU_OP_FREE_COMMAND_BUFFERS) {
		*handled = 0;
		return 0;
	}

	/* Allocation needs a result vector, whereas ordinary void free may ask only for the opcode echo. */
	if (requested > 1)
		return EINVAL;
	if (opcode == GPU_OP_ALLOCATE_COMMAND_BUFFERS) {
		if (requested != 1)
			return EINVAL;
		error = allocate_buffers(session, reader, reply);
	} else {
		error = free_buffers(session, reader);
	}

	/* A failed ownership operation never receives an invented success trailer. */
	if (error != 0)
		return error;

	/* Succeeded: this exact selected primary buffer batch has a recoverable ownership outcome. */
	return 0;
}

/* Consumes and validates every fresh output before any construction, then unwinds all partial publication on failure. */
static int
allocate_buffers(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_command_buffer *prepared[VULKAN_COMMAND_BATCH];
	struct bcm2711_vulkan_object *published[VULKAN_COMMAND_BATCH];
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *pool_object;
	struct bcm2711_vulkan_object *existing;
	struct bcm2711_vulkan_command_pool *pool;
	uint64_t identities[VULKAN_COMMAND_BATCH];
	uint64_t device_id;
	uint64_t pool_id;
	uint64_t present;
	uint64_t chain;
	uint64_t array;
	uint32_t type;
	uint32_t level;
	uint32_t count;
	uint32_t index;
	uint32_t previous;
	int error;
	int retired;
	int retirement_error;

	/* Actual client allocation carries one primary pool record followed by the complete reserved identity array. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	pool_id = drv_i915_wire_read_u64(reader);
	level = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    present != 1 ||
	    type != VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO ||
	    chain != 0 ||
	    level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
	    count == 0 ||
	    count > VULKAN_COMMAND_BATCH ||
	    array != count)
		return ENOTSUP;

	/* Consume the whole exact reserved or selected identity vector before any mutation. */
	for (index = 0; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);

	/* Complete framing and same-device pool ownership precede every parent acquisition. */
	if (reader->error != 0)
		return EINVAL;

	/* Resolve both retained logical owners before constructing or consuming any buffer. */
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	pool_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_POOL, pool_id);
	if (device == NULL || pool_object == NULL)
		return EINVAL;

	/* Require an exact same-device pool, even when another device uses the same numeric identity. */
	pool = pool_object->payload;
	if (pool->owner.device != device)
		return EINVAL;

	/* Fresh typed IDs and uniqueness are properties of the entire batch, not a partially published prefix. */
	for (index = 0; index < count; index++) {
		if (identities[index] == 0)
			return EINVAL;
		existing = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index]);
		if (existing != NULL)
			return EEXIST;
		for (previous = 0; previous < index; previous++) {
			if (identities[previous] == identities[index])
				return EEXIST;
		}
	}

	/* Independent metadata/registry allocations may refuse anywhere; keep exact partial ownership for complete rollback. */
	kern_memset(prepared, 0, sizeof(prepared));
	kern_memset(published, 0, sizeof(published));
	error = 0;
	for (index = 0; index < count; index++) {
		error = prepare_buffer(device, pool_object, &prepared[index]);
		if (error != 0)
			break;
		error = bcm2711_vulkan_object_publish(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index], prepared[index], bcm2711_vulkan_command_release, &published[index]);
		if (error != 0)
			break;
		prepared[index]->object = published[index];
	}

	/* No failed allocation acknowledges a prefix; published IDs and unpublished payloads retire through their distinct owners. */
	if (error != 0) {
		retirement_error = 0;
		for (index = 0; index < count; index++) {
			retired = 0;
			if (published[index] != NULL)
				retired = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index]);
			else if (prepared[index] != NULL)
				retired = bcm2711_vulkan_command_release(session, prepared[index]);
			if (retired != 0 && retirement_error == 0)
				retirement_error = retired;
		}

		/* A failed native resource release has precedence over ordinary heap exhaustion. */
		if (retirement_error != 0)
			return retirement_error;
		if (error == ENOMEM || error == ENOSPC) {
			allocation_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0, NULL);
			return 0;
		}

		/* Other typed owner refusals preserve their native meaning. */
		return error;
	}

	/* Acknowledge only the complete vector, preserving exact client identity ordering. */
	allocation_reply(reply, VK_SUCCESS, count, identities);

	/* Succeeded: every requested primary buffer owns its pool and an empty initial recording. */
	return 0;
}

/* Validates all same-pool non-null selections and pending counts before withdrawing any command identity. */
static int
free_buffers(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *pool_object;
	struct bcm2711_vulkan_object *objects[VULKAN_COMMAND_BATCH];
	struct bcm2711_vulkan_command_pool *pool;
	struct bcm2711_vulkan_command_buffer *command;
	uint64_t identities[VULKAN_COMMAND_BATCH];
	uint64_t device_id;
	uint64_t pool_id;
	uint64_t array;
	uint32_t count;
	uint32_t index;
	uint32_t previous;
	int error;
	int retired;

	/* The standard void free array permits null entries, but no non-null object may be consumed twice. */
	device_id = drv_i915_wire_read_u64(reader);
	pool_id = drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    count > VULKAN_COMMAND_BATCH ||
	    array != count)
		return ENOTSUP;

	/* Consume the whole exact reserved or selected identity vector before any mutation. */
	for (index = 0; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Resolve both retained logical owners before constructing or consuming any buffer. */
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	pool_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_POOL, pool_id);
	if (device == NULL || pool_object == NULL)
		return EINVAL;

	/* Require an exact same-device pool, even when another device uses the same numeric identity. */
	pool = pool_object->payload;
	if (pool->owner.device != device)
		return EINVAL;

	/* The complete selected vector is checked before a prefix can retire its records or dependency edges. */
	for (index = 0; index < count; index++) {
		objects[index] = NULL;
		if (identities[index] == 0)
			continue;
		objects[index] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index]);
		if (objects[index] == NULL)
			return EINVAL;
		command = objects[index]->payload;
		if (command->owner.device != device || command->owner.parent != pool_object)
			return EINVAL;
		if (command->pending != 0)
			return EBUSY;
		for (previous = 0; previous < index; previous++) {
			if (identities[previous] == identities[index])
				return EINVAL;
		}
	}

	/* All selected identities retire even when a recorded resource's native storage requires recovery. */
	error = 0;
	for (index = 0; index < count; index++) {
		if (objects[index] == NULL)
			continue;
		retired = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index]);
		if (retired != 0 && error == 0)
			error = retired;
	}

	/* Independent retained command objects survive registry removal with their complete recording. */
	if (error != 0)
		return error;

	/* Succeeded: exact same-pool public selections have been withdrawn once. */
	return 0;
}

/* Acquires successful parent edges before publishing borrowed pool membership or returning an owned child payload. */
static int
prepare_buffer(
	struct bcm2711_vulkan_object *device,
	struct bcm2711_vulkan_object *pool_object,
	struct bcm2711_vulkan_command_buffer **created)
{
	struct bcm2711_vulkan_command_pool *pool;
	struct bcm2711_vulkan_command_buffer *command;
	int error;
	int retired;

	/* The caller gains no payload when an ordinary allocation or parent retention fails. */
	*created = NULL;

	/* Allocate independent initial recording metadata before acquiring either parent. */
	command = kern_calloc(1, sizeof(*command));
	if (command == NULL)
		return ENOMEM;

	/* Acquire the device independently of the optional later pool edge. */
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		kern_free(command);
		return error;
	}

	/* Publish only successful ownership edges, permitting a later parent failure to unwind exact acquired references. */
	command->owner.device = device;
	error = bcm2711_vulkan_object_retain(pool_object);
	if (error != 0) {
		retired = bcm2711_vulkan_command_release(device->session, command);
		if (retired != 0)
			return retired;
		return error;
	}

	/* The buffer's parent edge owns the pool; its list contains borrowed children and therefore creates no retain cycle. */
	command->owner.parent = pool_object;
	command->state = BCM2711_VULKAN_COMMAND_INITIAL;
	pool = pool_object->payload;
	command->next = pool->children;
	command->previous = &pool->children;
	if (command->next != NULL)
		command->next->previous = &command->next;
	pool->children = command;
	*created = command;

	/* Succeeded: the caller owns a complete initial primary payload with independent parent edges. */
	return 0;
}

/* Acknowledges the complete all-or-nothing output vector without inventing partial buffer success. */
static void
allocation_reply(
	struct i915_wire_writer *reply,
	VkResult status,
	uint32_t count,
	const uint64_t *identities)
{
	uint32_t index;

	/* Standard allocate returns its status followed by the exact count-selected native output identities. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, count);
	for (index = 0; index < count; index++)
		drv_i915_wire_reply_u64(reply, identities[index]);

	/* Succeeded: complete native allocation framing belongs to this selected batch. */
	return;
}
