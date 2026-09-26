/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The replies every object command of the graphics path shares (see
 * reply.h).
 */

#include "reply.h"
#include "codec.h"
#include "internal.h"
#include "object.h"

#include <kern/kmem.h>

#include <libc/vulkan/vulkan_core.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Translates an errno into the VkResult a reply carries.
 *
 * Out of memory and an unsupported request keep their meaning; every other
 * failure is reported as an initialization failure.
 */
uint32_t
drv_i915_gfx_result(
	int error)
{
	/* Success is VK_SUCCESS. */
	if (error == 0)
		return 0U;

	/* An allocation that failed is out of device memory. */
	if (error == ENOMEM)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* A request the executor refuses by name is a missing feature. */
	if (error == ENOTSUP)
		return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;

	/* Anything else failed the object's initialization. */
	return (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
}

/*
 * Reads the tail every generic create shares and returns the identity.
 *
 * The tail is [pAllocator][present][identity]; the allocator is always
 * absent and the presence marker is not checked.  A failed read is latched
 * in the reader, and the caller checks it.
 */
uint64_t
drv_i915_gfx_create_tail(
	struct i915_wire_reader *reader)
{
	uint64_t identity;

	/* Skips the allocator and the identity's presence marker. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);

	/* Reads the identity libvulkan chose for the new object. */
	identity = drv_i915_wire_read_u64(reader);

	/* Reports the identity; a failed read is latched in the reader. */
	return identity;
}

/*
 * Publishes an object under its identity and writes the create reply.
 *
 * `error` is what the caller already decided; a NULL object with no error is
 * an allocation that failed.  On any failure the object is freed and the
 * reply is the result alone; on success it is the result, a presence marker
 * and the identity.
 */
void
drv_i915_gfx_create_reply(
	struct i915_render_session *session,
	struct i915_wire_writer *reply,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	void *object,
	int error)
{
	uint32_t result;

	/* An object the caller could not allocate is out of memory. */
	if (error == 0 && object == NULL)
		error = ENOMEM;

	/*
	 * Publishes the object, or frees it: an object that could not be
	 * published, or that was made for a refused command, is not kept.
	 */
	if (error == 0) {
		error = drv_i915_object_insert(session, kind, identity, object);
		if (error != 0)
			kern_free(object);
	} else if (object != NULL) {
		kern_free(object);
	}

	/* Writes the result of the create. */
	result = drv_i915_gfx_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* A published object is answered with its identity behind a presence marker. */
	if (error == 0) {
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, identity);
	}
}
