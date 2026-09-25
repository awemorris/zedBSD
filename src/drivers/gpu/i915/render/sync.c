/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's synchronization objects (see sync.h).
 *
 * The command is decoded exactly as libvulkan encodes it (sync_create in
 * sync.c of libvulkan).
 */

#include "sync.h"
#include "codec.h"
#include "internal.h"
#include "object.h"
#include "reply.h"

#include <kern/kmem.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Creates a VkSemaphore: vkCreateSemaphore.
 *
 * The command is [device][present][sType][pNext][flags][pAllocator]
 * [present][identity] and the reply that of a generic create.  XXX: one
 * queue runs every submission to its end before the next is decoded, so a
 * semaphore has nothing to order: it is an identity and nothing else.
 */
int
drv_i915_gfx_create_semaphore(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t identity;
	uint32_t *semaphore;

	/* Skips the device, the create info's presence marker, sType, pNext and flags, and reads the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Allocates a placeholder that stands for the semaphore in the object table. */
	semaphore = kern_calloc(1U, sizeof(*semaphore));

	/* Publishes the semaphore and answers; a failed allocation is reported there. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_SEMAPHORE, identity, semaphore, 0);

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}
