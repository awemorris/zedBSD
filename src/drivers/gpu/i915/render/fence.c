/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Vulkan fences.
 *
 * A fence is a latch: vkQueueSubmit runs every batch of a submission to its
 * end before it replies, then signals the submission's fence.  The client
 * waits for a fence by polling vkGetFenceStatus, so no wait reaches the
 * executor.  XXX: a fence is never armed on an engine seqno; that belongs to
 * an asynchronous submission, which does not exist yet.
 */

#include "fence.h"
#include "codec.h"
#include "internal.h"
#include "object.h"

#include "../i915.h"

#include <kern/klog.h>
#include <kern/kmem.h>

#include <uapi/errno.h>
#include <stddef.h>

/* VK_STRUCTURE_TYPE_FENCE_CREATE_INFO. */
#define I915_FENCE_CREATE_INFO		8U

/* VK_FENCE_CREATE_SIGNALED_BIT. */
#define I915_FENCE_CREATE_SIGNALED	1U

/* VK_SUCCESS, VK_NOT_READY and VK_ERROR_INITIALIZATION_FAILED on the wire. */
#define I915_FENCE_VK_SUCCESS		0U
#define I915_FENCE_VK_NOT_READY		1U
#define I915_FENCE_VK_FAILED		((uint32_t)-3)

/* The most fences one vkResetFences may name. */
#define I915_FENCE_RESET_MAX		64U

/*
 * One Vulkan fence.
 *
 * The object table owns it from vkCreateFence to vkDestroyFence; the
 * submission that names it signals it.
 */
struct i915_vk_fence {
	/* The session the fence was created in. */
	struct i915_render_session *session;

	/* Nonzero once the fence is signaled; vkResetFences clears it. */
	unsigned signaled;
};

static int i915_fence_create(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_fence_destroy(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_fence_reset(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_fence_status(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

/*
 * Runs one fence command.
 *
 * Returns ENOTSUP for a synchronization command other than the four fence
 * commands, after poisoning the reader: the command's length is unknown, so
 * nothing after it can be decoded.
 */
int
drv_i915_render_fence_dispatch(
	struct i915_render_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	int error;

	/* Picks the handler of the command. */
	switch (opcode) {
	case 35U:
		/* vkCreateFence */
		error = i915_fence_create(session, reader, reply);
		break;
	case 36U:
		/* vkDestroyFence */
		error = i915_fence_destroy(session, reader, reply);
		break;
	case 37U:
		/* vkResetFences */
		error = i915_fence_reset(session, reader, reply);
		break;
	case 38U:
		/* vkGetFenceStatus */
		error = i915_fence_status(session, reader, reply);
		break;
	default:
		/* XXX: semaphores, events and queries are not implemented; say so at the entry. */
		kern_logf("i915: vk: XXX unimplemented opcode %u (sync)\n", opcode);
		reader->error = 1;
		return ENOTSUP;
	}

	/* Reports a command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and its reply is written. */
	return 0;
}

/*
 * Signals a fence whose submission has run to its end.
 */
void
drv_i915_fence_signal(
	struct i915_vk_fence *fence)
{
	/* Latches the fence until vkResetFences. */
	fence->signaled = 1U;
}

/*
 * Frees a fence the object table no longer names.
 */
void
drv_i915_fence_free(
	struct i915_vk_fence *fence)
{
	/* A missing fence has nothing to free. */
	if (fence == NULL)
		return;

	kern_free(fence);
}

/*
 * vkCreateFence: [device][pCreateInfo present][sType][pNext present][flags]
 * [pAllocator present][pFence present][fence wire_id] -> [result]{[present][identity]}.
 */
static int
i915_fence_create(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vk_fence *fence;
	i915_vk_handle handle;
	uint32_t structure_type;
	uint32_t flags;
	int error;

	/* Decodes the command; only the type, the flags and the identity matter. */
	(void)drv_i915_wire_read_handle(reader);
	(void)drv_i915_wire_read_u64(reader);
	structure_type = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	handle = drv_i915_wire_read_handle(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Only a plain fence create structure is accepted. */
	if (structure_type != I915_FENCE_CREATE_INFO)
		return EINVAL;

	/* Allocates the fence, already signaled when the client asked for it. */
	error = 0;
	fence = kern_calloc(1U, sizeof(*fence));
	if (fence == NULL) {
		error = ENOMEM;
	} else {
		fence->session = session;
		if ((flags & I915_FENCE_CREATE_SIGNALED) != 0U)
			fence->signaled = 1U;

		/* Publishes the fence under the client's identity. */
		error = drv_i915_object_insert(session, I915_VK_OBJ_FENCE, handle, fence);
		if (error != 0)
			drv_i915_fence_free(fence);
	}

	/* A failed create is reported on the wire, not as a decode failure. */
	if (error != 0) {
		drv_i915_wire_reply_u32(reply, I915_FENCE_VK_FAILED);
		return 0;
	}

	/* Replies with success, the present flag and the identity. */
	drv_i915_wire_reply_u32(reply, I915_FENCE_VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, handle);

	/* Succeeded: the fence exists. */
	return 0;
}

/*
 * vkDestroyFence: [device][fence][pAllocator present].  It returns void, so the
 * reply is the echoed opcode alone.
 */
static int
i915_fence_destroy(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vk_fence *fence;
	i915_vk_handle handle;

	UNUSED_PARAMETER(reply);

	/* Decodes the fence identity. */
	(void)drv_i915_wire_read_handle(reader);
	handle = drv_i915_wire_read_handle(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Unpublishes and frees a fence the session knows; an unknown one is ignored. */
	fence = drv_i915_object_lookup(session, I915_VK_OBJ_FENCE, handle);
	if (fence != NULL) {
		drv_i915_object_remove(session, I915_VK_OBJ_FENCE, handle);
		drv_i915_fence_free(fence);
	}

	/* Succeeded: the fence is gone. */
	return 0;
}

/*
 * vkResetFences: [device][fenceCount][count][count x fence] -> [result].
 */
static int
i915_fence_reset(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vk_fence *fence;
	i915_vk_handle handle;
	uint64_t count;
	uint64_t index;

	/* Decodes the fence count. */
	(void)drv_i915_wire_read_handle(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses a list longer than the executor decodes. */
	if (count > I915_FENCE_RESET_MAX)
		return EINVAL;

	/* Clears every named fence the session knows. */
	for (index = 0U; index < count; index++) {
		handle = drv_i915_wire_read_handle(reader);
		fence = drv_i915_object_lookup(session, I915_VK_OBJ_FENCE, handle);
		if (fence != NULL)
			fence->signaled = 0U;
	}

	/* Refuses a list that ran past the command. */
	if (reader->error != 0)
		return EINVAL;

	/* Replies with success. */
	drv_i915_wire_reply_u32(reply, I915_FENCE_VK_SUCCESS);

	/* Succeeded: the fences are unsignaled. */
	return 0;
}

/*
 * vkGetFenceStatus: [device][fence] -> [VK_SUCCESS when signaled, VK_NOT_READY otherwise].
 */
static int
i915_fence_status(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vk_fence *fence;
	i915_vk_handle handle;

	/* Decodes the fence identity. */
	(void)drv_i915_wire_read_handle(reader);
	handle = drv_i915_wire_read_handle(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An unknown fence is reported on the wire. */
	fence = drv_i915_object_lookup(session, I915_VK_OBJ_FENCE, handle);
	if (fence == NULL) {
		drv_i915_wire_reply_u32(reply, I915_FENCE_VK_FAILED);
		return 0;
	}

	/* Reports the latch. */
	if (fence->signaled != 0U) {
		drv_i915_wire_reply_u32(reply, I915_FENCE_VK_SUCCESS);
	} else {
		drv_i915_wire_reply_u32(reply, I915_FENCE_VK_NOT_READY);
	}

	/* Succeeded: the status is written. */
	return 0;
}
