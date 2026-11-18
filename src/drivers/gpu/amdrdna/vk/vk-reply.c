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

#include "drivers/gpu/amdrdna/vk/amdrdna-vk-objects.h"

/*
 * Reports the VkResult an executor errno becomes, the rule libvulkan
 * applies to a create the kernel refuses so that both agree: a GPU memory
 * pool or heap that ran out (ENOSPC) is out of device memory, a kernel
 * allocation (ENOMEM, ENOBUFS) out of host memory, something the executor
 * does not implement or more than it holds (ENOTSUP, E2BIG) a missing
 * feature, and anything else (EINVAL, ERANGE among them) a failed
 * initialization.  A refused create never loses the device.
 */
uint32_t
amdrdna_vk_result(
	int error)
{
	/* Success is VK_SUCCESS. */
	if (error == 0)
		return (uint32_t)VK_SUCCESS;

	/* The GPU's memory, then the kernel's. */
	if (error == ENOSPC)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if (error == ENOMEM || error == ENOBUFS)
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Something the executor does not implement, or more than it holds. */
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
amdrdna_vk_create_tail(
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
amdrdna_vk_create_reply(
	struct amdrdna_vk_session *session,
	struct i915_wire_writer *reply,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	void *payload,
	int (*destroy)(struct amdrdna_vk_session *, void *),
	int error)
{
	struct amdrdna_vk_object *object;
	uint32_t result;

	/* A payload the caller could not allocate is out of host memory. */
	if (error == 0 && payload == NULL)
		error = ENOMEM;

	/* Publishes an accepted payload under its identity. */
	if (error == 0)
		error = amdrdna_vk_object_publish(session, kind, identity, payload, destroy, &object);

	/* A payload that was made but not published goes now. */
	if (error != 0 && payload != NULL)
		(void)destroy(session, payload);

	/* The result. */
	result = amdrdna_vk_result(error);
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
amdrdna_vk_destroy_plain(
	struct amdrdna_vk_session *session,
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
	error = amdrdna_vk_object_remove(session, kind, identity);
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
amdrdna_vk_payload(
	struct amdrdna_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct amdrdna_vk_object *object;

	/* The published object of the kind. */
	object = amdrdna_vk_object_find(session, kind, identity);
	if (object == NULL)
		return NULL;

	/* Succeeded: what the identity stands for. */
	return object->payload;
}

/* Destroys a payload that holds nothing but its own storage. */
int
amdrdna_vk_free_payload(
	struct amdrdna_vk_session *session,
	void *payload)
{
	UNUSED_PARAMETER(session);

	/* Its storage. */
	kern_free(payload);

	/* Succeeded: the payload is gone. */
	return 0;
}
