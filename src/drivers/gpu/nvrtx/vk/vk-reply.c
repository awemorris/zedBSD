/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the executor's create and destroy commands share: the tail of a create,
 * its reply, the plain destroy, and the result an errno becomes on the wire.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"

/*
 * Reports the VkResult an executor errno becomes: success; the GPU's
 * memory (its heap or a window onto it) used up (ENOSPC) is out of device
 * memory; the kernel's own storage for an object or its work (ENOMEM,
 * ENOBUFS) is out of host memory; something the executor does not
 * implement or a size past its limits (ENOTSUP, E2BIG) is a missing
 * feature; a bad argument and anything else (EINVAL, ERANGE, ...) failed
 * the object's initialization.  A refused create never loses the device;
 * only a submission whose GPU work failed does (queue_result()).
 */
uint32_t
nvrtx_vk_result(
	int error)
{
	/* Success is VK_SUCCESS. */
	if (error == 0)
		return (uint32_t)VK_SUCCESS;

	/* The GPU's memory used up. */
	if (error == ENOSPC)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* The kernel's storage used up. */
	if (error == ENOMEM || error == ENOBUFS)
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Something the executor does not implement, or past its limits. */
	if (error == ENOTSUP || error == E2BIG)
		return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;

	/* Anything else failed the object's initialization. */
	return (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
}

/*
 * Reads the tail every create shares, [allocator][present][identity], and
 * reports the identity libvulkan chose; a failed read is latched in the
 * reader for the caller to check.
 */
uint64_t
nvrtx_vk_create_tail(
	struct i915_wire_reader *reader)
{
	uint64_t identity;

	/* Skips the allocator (always absent) and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);

	/* The new object's identity. */
	identity = drv_i915_wire_read_u64(reader);

	/* Succeeded: the identity, or 0 with the reader's error latched. */
	return identity;
}

/*
 * Publishes a created payload under its identity and writes the create's
 * reply: [result], and on success [present][identity].
 *
 * error is what the caller decided; a NULL payload without one is storage
 * that could not be had.  A payload that is not published is destroyed
 * here with its destructor.
 */
void
nvrtx_vk_create_reply(
	struct nvrtx_vk_session *session,
	struct i915_wire_writer *reply,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	void *payload,
	int (*destroy)(struct nvrtx_vk_session *, void *),
	int error)
{
	struct nvrtx_vk_object *object;
	uint32_t result;

	/* A payload the caller could not allocate is out of memory. */
	if (error == 0 && payload == NULL)
		error = ENOMEM;

	/* Publishes an accepted payload under its identity. */
	if (error == 0)
		error = nvrtx_vk_object_publish(session, kind, identity, payload, destroy, &object);

	/* A payload that was made but not published goes now. */
	if (error != 0 && payload != NULL)
		(void)destroy(session, payload);

	/* The result. */
	result = nvrtx_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* A published object is answered with its identity behind a presence word. */
	if (error == 0) {
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, identity);
	}
}

/*
 * Runs a destroy whose wire is [device][identity][allocator] and whose
 * reply has no body: the identity is removed, and its payload goes once
 * nothing holds it.  An unknown identity is not an error.
 */
int
nvrtx_vk_destroy_plain(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	enum i915_vk_object_kind kind)
{
	uint64_t identity;
	int error;

	/* The identity between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Unpublishes the identity; one never published is already gone. */
	error = nvrtx_vk_object_remove(session, kind, identity);
	if (error == ENOENT)
		error = 0;

	/* Reports a destructor that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the identity is gone. */
	return 0;
}

/*
 * Reports the payload a published identity of a kind stands for, or NULL
 * when there is none.
 */
void *
nvrtx_vk_payload(
	struct nvrtx_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct nvrtx_vk_object *object;

	/* The published object of the kind. */
	object = nvrtx_vk_object_find(session, kind, identity);
	if (object == NULL)
		return NULL;

	/* Succeeded: what the identity stands for. */
	return object->payload;
}

/* Destroys a payload that holds nothing but its own storage. */
int
nvrtx_vk_free_payload(
	struct nvrtx_vk_session *session,
	void *payload)
{
	UNUSED_PARAMETER(session);

	/* Its storage. */
	kern_free(payload);

	/* Succeeded: the payload is gone. */
	return 0;
}
