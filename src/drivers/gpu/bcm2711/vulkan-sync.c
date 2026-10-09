/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Fence observations use native retirement state; decoder progress never signals a fence or binary semaphore. */
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/vulkan-sync.h"

/* The bounded object namespace also bounds one all-or-nothing host reset vector. */
#define VULKAN_SYNC_RESET_COUNT 4096U

static int create_sync(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_sync(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int fence_status(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int reset_fences(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int release_sync(struct bcm2711_vulkan_session *session, void *payload);
static void reply_identity(struct i915_wire_writer *reply, VkResult status, uint64_t identity);

/*
 * Executes the implemented core fence and binary semaphore commands with exact native typed ownership.
 */
int
bcm2711_vulkan_sync_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Other commands remain available to the independent queue and resource dispatchers. */
	*handled = 0;
	if (opcode != GPU_OP_CREATE_FENCE &&
	    opcode != GPU_OP_CREATE_SEMAPHORE &&
	    opcode != GPU_OP_DESTROY_FENCE &&
	    opcode != GPU_OP_DESTROY_SEMAPHORE &&
	    opcode != GPU_OP_GET_FENCE_STATUS &&
	    opcode != GPU_OP_RESET_FENCES)
		return 0;
	*handled = 1;

	/* Void destruction may echo its opcode; status and creation require their actual result response. */
	if (opcode == GPU_OP_DESTROY_FENCE || opcode == GPU_OP_DESTROY_SEMAPHORE) {
		if (requested > 1)
			return EINVAL;
	} else {
		if (requested != 1)
			return EINVAL;
	}

	/* Each selected route consumes its complete explicit client record. */
	switch (opcode) {
	case GPU_OP_CREATE_FENCE:
		error = create_sync(session, I915_VK_OBJ_FENCE, reader, reply);
		break;
	case GPU_OP_CREATE_SEMAPHORE:
		error = create_sync(session, I915_VK_OBJ_SEMAPHORE, reader, reply);
		break;
	case GPU_OP_DESTROY_FENCE:
		error = destroy_sync(session, I915_VK_OBJ_FENCE, reader);
		break;
	case GPU_OP_DESTROY_SEMAPHORE:
		error = destroy_sync(session, I915_VK_OBJ_SEMAPHORE, reader);
		break;
	case GPU_OP_GET_FENCE_STATUS:
		error = fence_status(session, reader, reply);
		break;
	default:
		error = reset_fences(session, reader, reply);
		break;
	}

	/* Framing or ownership refusal cannot masquerade as a completed native synchronization operation. */
	if (error != 0)
		return error;

	/* Succeeded: exactly one complete implemented synchronization command was processed. */
	return 0;
}

/* Creates one actual flags-only sync payload before publishing its reserved native identity. */
static int
create_sync(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_sync *sync;
	uint64_t device_id;
	uint64_t present;
	uint64_t extension;
	uint64_t allocator;
	uint64_t output;
	uint64_t identity;
	uint32_t structure;
	uint32_t expected;
	uint32_t flags;
	int error;
	int released;

	/* The real client strips local external-fence bookkeeping and sends this complete flags-only create structure. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	structure = drv_i915_wire_read_u32(reader);
	extension = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	allocator = drv_i915_wire_read_u64(reader);
	output = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    present != 1 || allocator != 0 ||
	    output != 1 || identity == 0)
		return EINVAL;

	/* Only the real same-session logical device can become this sync payload's independent parent. */
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (device == NULL)
		return EINVAL;
	expected = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (kind == I915_VK_OBJ_SEMAPHORE)
		expected = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	if (structure != expected)
		return EINVAL;

	/* Native timeline and imported sync payloads are not advertised by this binary-only profile. */
	if (extension != 0)
		return ENOTSUP;
	if (kind == I915_VK_OBJ_FENCE) {
		if ((flags & ~VK_FENCE_CREATE_SIGNALED_BIT) != 0)
			return ENOTSUP;
	} else {
		if (flags != 0)
			return ENOTSUP;
	}

	/* Allocation refusal produces an ordinary Vulkan error without acquiring a parent or publishing an output. */
	sync = kern_calloc(1, sizeof(*sync));
	if (sync == NULL) {
		reply_identity(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
		return 0;
	}

	/* The retained parent and initial native state are complete before publication. */
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		kern_free(sync);
		return error;
	}

	/* Complete initial payload state becomes visible only with its retained parent. */
	sync->device = device;
	sync->status = VK_SUCCESS;
	if (kind == I915_VK_OBJ_FENCE && (flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0)
		sync->signaled = true;

	/* Registry publication owns the complete payload, or exact construction unwind retires its parent. */
	error = bcm2711_vulkan_object_publish(session, kind, identity, sync, release_sync, &object);
	if (error != 0) {
		released = release_sync(session, sync);
		if (released != 0)
			return released;
		if (error == ENOMEM) {
			reply_identity(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
			return 0;
		}

		/* Duplicate or malformed native identity refusal remains a transport ownership failure. */
		return error;
	}

	/* The exact acknowledged identity becomes observable only after its complete payload is live. */
	reply_identity(reply, VK_SUCCESS, identity);

	/* Succeeded: one ordinary native fence or binary semaphore owns an independent device reference. */
	return 0;
}

/* Withdraws only the matching same-device identity while independently retained queue references preserve pending payloads. */
static int
destroy_sync(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_sync *sync;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocator;
	int error;

	/* Complete destruction framing precedes namespace withdrawal. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	allocator = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0)
		return EINVAL;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (device == NULL)
		return EINVAL;
	if (identity == 0)
		return 0;

	/* An ID of another kind or logical device cannot select this payload. */
	object = bcm2711_vulkan_object_find(session, kind, identity);
	if (object == NULL || object->payload == NULL)
		return EINVAL;
	sync = object->payload;
	if (sync->device != device)
		return EINVAL;
	error = bcm2711_vulkan_object_remove(session, kind, identity);
	if (error != 0)
		return error;

	/* Succeeded: the registry edge retired while any independent pending queue owner remains valid. */
	return 0;
}

/* Reports the real fence payload without equating command decoder completion with native work retirement. */
static int
fence_status(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_sync *sync;
	struct bcm2711_v3d_hardware *hardware;
	uint64_t identity;
	unsigned long enabled;
	VkResult status;

	/* Resolve both exact typed owners before observing any state. */
	identity = drv_i915_wire_read_u64(reader);
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, identity);
	identity = drv_i915_wire_read_u64(reader);
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_FENCE, identity);
	if (reader->error != 0 ||
	    device == NULL || object == NULL ||
	    object->payload == NULL)
		return EINVAL;
	sync = object->payload;
	if (sync->device != device)
		return EINVAL;

	/* IRQ fault publication takes precedence even over an initially signaled or previously completed payload. */
	hardware = &session->render->device->space.native->hardware;
	enabled = spin_lock_irqsave(&hardware->guard);

	status = sync->status;
	if (session->render->stopping ||
	    session->render->device->worker.uncertain ||
	    !hardware->ready || hardware->faulted) {
		status = VK_ERROR_DEVICE_LOST;
	} else if (status == VK_SUCCESS) {
		/* An unsignaled or pending fence remains unavailable regardless of protocol decoder progress. */
		if (!sync->signaled || sync->pending)
			status = VK_NOT_READY;
	}

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Preserve Vulkan's ordinary NOT_READY as well as native completion and sticky payload failure. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);

	/* Succeeded: the observation reflects native payload and guarded controller state. */
	return 0;
}

/* Resets the complete same-device non-pending fence vector only after every trailing identity was validated. */
static int
reset_fences(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object **objects;
	struct bcm2711_vulkan_sync *sync;
	uint64_t identity;
	uint64_t array;
	uint32_t count;
	uint32_t index;

	/* Complete bounded vector framing precedes every signaled-state change. */
	identity = drv_i915_wire_read_u64(reader);
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, identity);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    device == NULL || count == 0 ||
	    count > VULKAN_SYNC_RESET_COUNT || array != count)
		return EINVAL;
	objects = i915_vkc_array(reader, &session->arena, count, sizeof(*objects));
	if (objects == NULL)
		return EINVAL;

	/* Decode all IDs first so a trailing invalid element cannot reset an earlier fence. */
	for (index = 0; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		objects[index] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_FENCE, identity);
	}

	/* Truncation cannot publish any prefix reset. */
	if (reader->error != 0)
		return EINVAL;
	for (index = 0; index < count; index++) {
		if (objects[index] == NULL || objects[index]->payload == NULL)
			return EINVAL;
		sync = objects[index]->payload;
		if (sync->device != device)
			return EINVAL;
		if (sync->pending)
			return EBUSY;
		if (sync->status != VK_SUCCESS) {
			drv_i915_wire_reply_u32(reply, (uint32_t)sync->status);
			return 0;
		}
	}

	/* The controller mutex makes this complete reset atomic with queue admission and worker completion. */
	for (index = 0; index < count; index++) {
		sync = objects[index]->payload;
		sync->signaled = false;
	}

	/* The reply follows the complete all-or-nothing reset publication. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);

	/* Succeeded: every selected fence is unsignaled and no pending native completion was discarded. */
	return 0;
}

/* Retires one CPU synchronization payload and its device edge after its final independent owner releases it. */
static int
release_sync(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_sync *sync;
	int error;

	/* CPU synchronization metadata has no DMA mapping or hidden common completion ownership. */
	(void)session;
	sync = payload;
	error = bcm2711_vulkan_object_release(sync->device);
	kern_free(sync);
	if (error != 0)
		return error;

	/* Succeeded: all synchronization metadata and its exact parent edge retired. */
	return 0;
}

/* Echoes the exact reserved identity only after successful publication and preserves the required output-pointer framing on failure. */
static void
reply_identity(
	struct i915_wire_writer *reply,
	VkResult status,
	uint64_t identity)
{
	/* A real client distinguishes a zero failed output from an acknowledged live object. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the status and output identity follow the exact client create response. */
	return;
}
