/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Pool reset preserves command identities, while destruction withdraws only registry ownership of each child. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-command.h"

static int create_pool(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int retire_pool(struct bcm2711_vulkan_session *session, bool reset, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int release_pool(struct bcm2711_vulkan_session *session, void *payload);
static void create_reply(struct i915_wire_writer *reply, VkResult status, uint64_t identity);

/*
 * Routes same-device primary command pool creation, reset and independent identity retirement.
 */
int
bcm2711_vulkan_command_pool_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Other routers own command allocation, recording and actual queue submission. */
	*handled = 1;
	if (opcode != GPU_OP_CREATE_COMMAND_POOL &&
	    opcode != GPU_OP_RESET_COMMAND_POOL &&
	    opcode != GPU_OP_DESTROY_COMMAND_POOL) {
		*handled = 0;
		return 0;
	}

	/* Creation and reset return Vulkan results; ordinary void destruction may request an opcode echo. */
	if (requested > 1)
		return EINVAL;

	/* Result-bearing lifecycle calls require an explicit reply. */
	if (opcode != GPU_OP_DESTROY_COMMAND_POOL && requested != 1)
		return EINVAL;
	if (opcode == GPU_OP_CREATE_COMMAND_POOL)
		error = create_pool(session, reader, reply);
	else if (opcode == GPU_OP_RESET_COMMAND_POOL)
		error = retire_pool(session, true, reader, reply);
	else
		error = retire_pool(session, false, reader, reply);
	if (error != 0)
		return error;

	/* Succeeded: this pool operation has an exact ownership outcome in the selected session. */
	return 0;
}

/* Creates one finite-family pool only after consuming the complete standard record and creation tail. */
static int
create_pool(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_pool *pool;
	uint64_t device_id;
	uint64_t present;
	uint64_t output;
	uint64_t chain;
	uint64_t allocator;
	uint64_t identity;
	uint32_t type;
	uint32_t flags;
	uint32_t family;
	int error;
	int retired;

	/* Only the native graphics/transfer queue family and ordinary transient/reset flags have implemented meaning. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	family = drv_i915_wire_read_u32(reader);
	allocator = drv_i915_wire_read_u64(reader);
	output = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    present != 1 ||
	    type != VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO ||
	    chain != 0 ||
	    family != 0 ||
	    (flags & ~(VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT)) != 0)
		return ENOTSUP;

	/* Complete output framing precedes every device lookup and pool allocation. */
	if (reader->error != 0 ||
	    allocator != 0 ||
	    output != 1 ||
	    identity == 0)
		return EINVAL;

	/* Resolve the exact retained device before creating or retiring its pool. */
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (device == NULL)
		return EINVAL;

	/* Select only this session's exact typed pool identity. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_POOL, identity);
	if (object != NULL)
		return EEXIST;

	/* Allocate ordinary pool metadata before acquiring any parent edge. */
	pool = kern_calloc(1, sizeof(*pool));
	if (pool == NULL) {
		create_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
		return 0;
	}

	/* A complete pool acquires its device before entering the public registry. */
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		kern_free(pool);
		return error;
	}

	/* The empty borrowed list gains children only after their independent pool ownership exists. */
	pool->owner.device = device;
	pool->flags = flags;
	error = bcm2711_vulkan_object_publish(session, I915_VK_OBJ_COMMAND_POOL, identity, pool, release_pool, &object);
	if (error != 0) {
		retired = release_pool(session, pool);
		if (retired != 0)
			return retired;
		if (error == ENOMEM || error == ENOSPC) {
			create_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
			return 0;
		}

		/* Namespace refusals never acknowledge a replacement pool identity. */
		return error;
	}

	/* Exact publication precedes ordinary client handle acknowledgement. */
	create_reply(reply, VK_SUCCESS, identity);

	/* Succeeded: one empty primary pool independently retains its logical device. */
	return 0;
}

/* Refuses mutation of any pending child before changing an entire pool, including withdrawn native owners. */
static int
retire_pool(
	struct bcm2711_vulkan_session *session,
	bool reset,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_pool *pool;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_command_buffer *next;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocator;
	uint32_t flags;
	int error;
	int retired;

	/* Reset flags and ordinary null allocator framing are distinct native records. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reset) {
		flags = drv_i915_wire_read_u32(reader);
		if ((flags & ~VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) != 0)
			return ENOTSUP;
	} else {
		allocator = drv_i915_wire_read_u64(reader);
		if (allocator != 0)
			return EINVAL;
	}

	/* Null destruction consumes its complete record without touching any pool or child. */
	if (reader->error != 0)
		return EINVAL;
	if (!reset && identity == 0)
		return 0;

	/* Resolve the exact retained device before creating or retiring its pool. */
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);

	/* Select only this session's exact typed pool identity. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_POOL, identity);
	if (device == NULL || object == NULL)
		return EINVAL;
	pool = object->payload;
	if (pool->owner.device != device)
		return EINVAL;

	/* Borrowed children remain alive through their own independent pool edges; validate all before any reset/removal. */
	for (command = pool->children; command != NULL; command = command->next) {
		if (command->pending != 0)
			return EBUSY;
	}

	/* Reset preserves every live identity; destruction withdraws only published children and leaves retained old records intact. */
	error = 0;
	command = pool->children;
	while (command != NULL) {
		next = command->next;
		retired = 0;
		if (reset)
			retired = bcm2711_vulkan_command_clear(command);
		else if (command->object->published)
			retired = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, command->object->identity);
		if (retired != 0 && error == 0)
			error = retired;
		command = next;
	}

	/* Remove the pool registry edge after every child was considered, preserving the first native retirement failure. */
	if (!reset) {
		retired = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_COMMAND_POOL, identity);
		if (retired != 0 && error == 0)
			error = retired;
	}

	/* Native cleanup failures cannot become successful pool reset results. */
	if (error != 0)
		return error;

	/* Reset alone has a Vulkan result parameter after complete child retirement. */
	if (reset)
		drv_i915_wire_reply_u32(reply, VK_SUCCESS);

	/* Succeeded: the selected pool lifecycle operation preserved independent native owners. */
	return 0;
}

/* Releases a pool only after the last current or withdrawn child's independent parent edge has retired. */
static int
release_pool(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_command_pool *pool;
	int error;

	/* A child parent edge makes destruction with a nonempty borrowed list impossible. */
	(void)session;
	pool = payload;
	if (pool->children != NULL)
		__builtin_trap();
	error = bcm2711_vulkan_object_release(pool->owner.device);
	kern_free(pool);
	if (error != 0)
		return error;

	/* Succeeded: no command buffer retains this pool or its logical device edge. */
	return 0;
}

/* Returns a standard pool creation result and the exact nullable reserved output identity. */
static void
create_reply(
	struct i915_wire_writer *reply,
	VkResult status,
	uint64_t identity)
{
	/* A present creation output remains null on an ordinary allocation failure. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: client creation framing is complete. */
	return;
}
