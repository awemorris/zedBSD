/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The entry of the Vulkan executor (see render.h).
 *
 * The executor attaches to an i915 device, keeps the state of each open of
 * the GPU node, and receives the command streams libvulkan submits through
 * the node.  The capset it reports lets libvulkan open the node unchanged.
 * Decoding is the router's (dispatch.c) and the transport's (transport.c);
 * this file is the boundary the node's operations call.
 */

#include "render.h"
#include "internal.h"
#include "gfx.h"
#include "object.h"
#include "transport.h"
#include <kern/kcrt.h>

#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/gpu.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* VK_MAKE_VERSION(1, 3, 269): the Vulkan headers version the wire was generated from. */
#define I915_CAPSET_VK_XML_VERSION	0x0040310DU

/* The vendor suffix of the capset: its tag ("SDBZ" read little endian) and its flags. */
#define I915_CAPSET_VENDOR_TAG		0x5a424453U
#define I915_CAPSET_VENDOR_FLAGS	7U

static void i915_render_capset_fill(struct i915_render_device *vk);

/*
 * Attaches the executor to an i915 device and prepares its capset.
 */
int
drv_i915_render_attach(
	struct i915_device *device,
	struct i915_render_device **out)
{
	struct i915_render_device *vk;
	int error;

	/* The caller receives nothing on failure. */
	*out = NULL;

	/* Allocates the executor. */
	vk = kern_calloc(1U, sizeof(*vk));
	if (vk == NULL)
		return ENOMEM;

	/* Keeps a back reference to the hardware device the executor serves. */
	vk->i915 = device;

	/* Creates the object table that indexes every object libvulkan creates. */
	error = drv_i915_object_table_create(&vk->objects);
	if (error != 0) {
		kern_free(vk);
		return error;
	}

	/* Fills the capset that lets libvulkan accept the node as a Vulkan backend. */
	i915_render_capset_fill(vk);

	/* Succeeded: the device can accept executor sessions and commands. */
	*out = vk;
	return 0;
}

/*
 * Releases the executor and its object table.
 */
void
drv_i915_render_detach(
	struct i915_render_device *vk)
{
	/* A device that never attached is nothing to release. */
	if (vk == NULL)
		return;

	/* Releases the table's index, then the executor. */
	drv_i915_object_table_destroy(vk->objects);
	kern_free(vk);
}

/*
 * Opens an executor session over a GPU node session and its address space.
 */
int
drv_i915_render_open(
	struct i915_render_device *vk,
	struct i915_session *gpu_session,
	struct i915_render_session **out)
{
	struct i915_render_session *session;

	/* The caller receives nothing on failure. */
	*out = NULL;

	/* Allocates the session. */
	session = kern_calloc(1U, sizeof(*session));
	if (session == NULL)
		return ENOMEM;

	/* Links the session to the executor and to the address space it draws into. */
	session->vk = vk;
	session->gpu = gpu_session;

	/* Allocates the scratch the session's commands decode their records into. */
	session->arena.base = kern_calloc(1U, I915_WIRE_ARENA_BYTES);
	if (session->arena.base == NULL) {
		kern_free(session);
		return ENOMEM;
	}

	session->arena.size = I915_WIRE_ARENA_BYTES;

	/* Succeeded: the session accepts commands. */
	*out = session;
	return 0;
}

/*
 * Closes an executor session; it cannot fail.
 *
 * The node session it wraps is closed by the caller afterwards.
 */
void
drv_i915_render_close(
	struct i915_render_session *session)
{
	/* A session that never opened is nothing to release. */
	if (session == NULL)
		return;

	/*
	 * Releases what the session's draws kept and the allocations it left,
	 * forgets the identities it recorded, then releases the scratch and the
	 * session.
	 */
	drv_i915_gfx_session_close(session);
	drv_i915_gfx_memory_forget(session);
	drv_i915_object_forget(session);
	kern_free(session->arena.base);
	kern_free(session);
}

/*
 * Decodes and executes one submitted command stream, writing its replies.
 *
 * reply is the region the stream selected (see transport.h), or NULL for a
 * stream that replies nowhere; reply_bytes gives its capacity and receives
 * how many reply bytes the commands wrote.  A refused command fails the
 * stream, and a reply that overflowed the region fails it with EMSGSIZE;
 * the reply length is only published on success.
 */
int
drv_i915_render_execute(
	struct i915_render_session *session,
	const void *wire,
	size_t bytes,
	void *reply,
	size_t *reply_bytes)
{
	struct i915_wire_reader reader;
	struct i915_wire_writer writer;
	int error;

	/* Bounds every decode against the submitted length. */
	reader.base = wire;
	reader.size = bytes;
	reader.offset = 0U;
	reader.error = 0;

	/* Bounds the replies against the selected region; a stream without one has no room. */
	writer.base = reply;
	writer.size = 0U;
	if (reply != NULL)
		writer.size = *reply_bytes;
	writer.offset = 0U;
	writer.error = 0;

	/* Executes the commands the stream carries back to back. */
	error = drv_i915_render_transport_execute(session, &reader, &writer);
	if (error != 0)
		return error;

	/* A reply that overflowed the region is a protocol error. */
	if (writer.error != 0)
		return EMSGSIZE;

	/*
	 * Publishes the replies, written into the shared blob the stream
	 * selected, before the completion libvulkan polls.
	 */
	kern_io_write_barrier();

	/* Tells the caller how many reply bytes the commands produced. */
	if (reply_bytes != NULL)
		*reply_bytes = writer.offset;

	/* Succeeded: every command in the stream was decoded and executed. */
	return 0;
}

/*
 * Copies the executor's capset into a capset request.
 *
 * Returns EINVAL when the capset does not fit the requested capacity.
 */
int
drv_i915_render_get_capset(
	const struct i915_render_device *vk,
	struct gpu_capset *capset)
{
	/* The capset must fit the requested capacity. */
	if (vk->capset_bytes > capset->capacity)
		return EINVAL;

	/* Copies the one capset the client reads before it opens. */
	capset->bytes = vk->capset_bytes;
	kern_memcpy(capset->data, vk->capset, vk->capset_bytes);

	/* Succeeded: the client can open the node as a Vulkan backend. */
	return 0;
}

/*
 * Maps a Vulkan result code carried on the wire to an errno.
 *
 * VK_SUCCESS maps to zero, the two out-of-memory results to ENOMEM, and
 * every other result to EINVAL.
 */
int
drv_i915_render_errno(
	int vk_result)
{
	/* VK_SUCCESS is the only result forwarded as success. */
	if (vk_result == 0)
		return 0;

	/* VK_ERROR_OUT_OF_HOST_MEMORY and VK_ERROR_OUT_OF_DEVICE_MEMORY. */
	if (vk_result == -1 || vk_result == -2)
		return ENOMEM;

	/* Everything else is a generic error. */
	return EINVAL;
}

/* Fills the capset libvulkan reads to accept the node as a Vulkan backend. */
static void
i915_render_capset_fill(
	struct i915_render_device *vk)
{
	/*
	 * Starts from the base record libvulkan requires (at least 156 bytes):
	 * a Venus capability record with one supported revision and every
	 * other field clear.
	 */
	kern_memset(vk->capset, 0, sizeof(vk->capset));
	vk->capset[0] = 1U;
	vk->capset_bytes = 156U;

	/*
	 * Extends it to the record libvulkan accepts (context.c): wire version
	 * 1, the Vulkan headers version it was generated from, a non-zero
	 * timeline count (RCS0), and the 168-byte vendor suffix.
	 *
	 * XXX: flags 7 = OPAQUE | STRICT_QUEUE | QUIESCE is declared for the
	 * connectivity check only; the request worker honours it on the happy
	 * path and has no retire or recovery behind it.
	 */
	vk->capset[1] = I915_CAPSET_VK_XML_VERSION;
	vk->capset[152U / 4U] = 1U;
	vk->capset[160U / 4U] = I915_CAPSET_VENDOR_TAG;
	vk->capset[164U / 4U] = I915_CAPSET_VENDOR_FLAGS;
	vk->capset_bytes = 168U;
	kern_logf("i915: vk: XXX capset declares vendor flags 7 for the connectivity check (contracts not implemented beyond the happy path)\n");
}
